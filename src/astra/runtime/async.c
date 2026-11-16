/* async.c — cooperative fiber scheduler + poll event loop ("auto-async").
 *
 * Goal: user code stays plain synchronous A, but blocking runtime
 * operations (sleep, socket I/O, file I/O, system/popen, stdin) yield to
 * a central scheduler instead of blocking the OS thread whenever they
 * run inside a fiber. Multiple fibers therefore overlap on one thread —
 * the same model as asyncio/JS, but with NO language syntax: fibers are
 * created internally (main wrapper, auto-parallel loops, tests).
 *
 * Model:
 *   - A "fiber" is a ucontext with its own malloc'd stack. Fibers are
 *     cooperative: they only switch at explicit yield points inside the
 *     patched blocking builtins, never preemptively. That keeps the A
 *     heap single-threaded — xmalloc/GC registry need no locks because
 *     only ONE fiber (or the scheduler) runs at a time on the OS thread.
 *   - The scheduler (running on the main OS thread inside sched_run)
 *     multiplexes: poll(2) over fds waited on by fibers + timer deadlines
 *     + a wake pipe fed by the blocking-op worker pool. When work is
 *     ready it swapcontexts into the next READY fiber.
 *   - Non-pollable blocking work (fopen/fread, system(), popen(), large
 *     file writes) runs on a small pool of REAL pthread workers. Workers
 *     must use raw malloc only — they can never call xmalloc (the GC
 *     registry is not thread-safe). The scheduler thread wraps results
 *     into avals after the worker completes.
 *
 * GC interaction:
 *   - Every live fiber's stack range [saved_sp, stack_top) and the AFiber
 *     struct itself are reported as roots (fib_gc_roots via
 *     a_gc_add_root_cb); gc.c's a_gc_scan_region marks words in a range.
 *   - mark_c_stack() in gc.c asks a_fib_stack_bounds() for the CURRENT
 *     fiber's stack when GC runs inside a fiber (pthread bounds would
 *     point at the main stack instead).
 *
 * Availability: requires ucontext + pthreads. Disabled (A_FIBERS=0) on
 * Windows, TinyCC/astoc-built runtimes, and when A_ASYNC=0/off/no/false.
 * In that case every entry point degrades to the plain synchronous path.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* getcontext/swapcontext: Darwin hides them otherwise */
#endif
#include "a_runtime_internal.h"

#if A_FIBERS
#include <ucontext.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if A_FIBERS

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static int fib_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("A_ASYNC");
        v = !(e && (!strcmp(e, "0") || !strcmp(e, "off") ||
                    !strcmp(e, "no") || !strcmp(e, "false") || !*e));
    }
    return v;
}

static size_t fib_stack_size(void) {
    static size_t v = 0;
    if (!v) {
        const char *e = getenv("A_FIB_STACK");
        v = e && *e ? (size_t)strtoull(e, NULL, 10) : (size_t)512 * 1024;
        if (v < 64 * 1024) v = 64 * 1024;
        if (v > 16u << 20) v = 16u << 20;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* fiber object                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    FIB_READY = 0,   /* runnable: sitting in the ready queue            */
    FIB_RUN,         /* currently executing (only meaningful for a_fib_current) */
    FIB_WAIT_FD,     /* waiting for fd readiness (poll)                 */
    FIB_WAIT_TIMER,  /* waiting for a deadline                          */
    FIB_WAIT_TASK,   /* waiting for a blocking-pool job                 */
    FIB_WAIT_FIB,    /* waiting for another fiber to finish             */
    FIB_DONE         /* finished; result stored; awaiting reap          */
} FibState;

typedef struct AFib {
    ucontext_t ctx;          /* fiber context (registers at suspend)     */
    void *stack;             /* raw malloc'd stack (NOT a GC block)      */
    size_t stack_size;
    FibState state;
    void *sp;                /* SP captured at last yield (GC scan low)  */
    int64_t wake_ms;         /* timer deadline, monotonic ms (INT64_MAX) */
    int wait_fd;
    short wait_events;
    int wait_revents;        /* filled by scheduler before wake          */
    /* blocking-pool wait */
    void *job;               /* ABlockJob* (opaque here)                 */
    void *task_result;
    /* child-fiber wait */
    struct AFib *wait_fib;
    /* entry: raw C fn aval(*)(aval*) OR an A fnval via a_call */
    int is_raw;
    uint64_t raw_entry;
    aval raw_env;
    aval fnval;
    uint64_t nargs;
    aval *args;              /* xmalloc'd: [0]=env, [1..n]=params        */
    aval small_argv[24];     /* inline argv when n+1 <= 24               */
    aval result;
    struct AFib *next_all;   /* live-fiber list                          */
    struct AFib *next_ready; /* ready-queue link                         */
    struct AFib *next_done;  /* done-list link                           */
    char name[64];
    int idx;
    int joined;
} AFib;

static AFib *g_fibs;                 /* fibers not yet DONE                  */
static AFib *g_fib_done, *g_fib_done_tail;
static AFib *g_ready_head, *g_ready_tail;
static ucontext_t g_sched_ctx;
A_RT_TLS AFib *a_fib_current;         /* running fiber, NULL = outside        */
static int g_rootcb_done;
static AFib **g_fib_tab;              /* idx -> fiber (makecontext arg channel) */
static int *g_fib_freeidx;
static int g_fib_tabn, g_fib_tabcap, g_fib_free_n;

static int fib_idx_alloc(AFib *f) {
    if (g_fib_free_n) {
        /* Recycled slot: g_fib_tab[idx] still names the freed fiber —
           refresh it or fib_trampoline() would resume a stale AFib. */
        int idx = g_fib_freeidx[--g_fib_free_n];
        g_fib_tab[idx] = f;
        return idx;
    }
    if (g_fib_tabn == g_fib_tabcap) {
        int nc = g_fib_tabcap ? g_fib_tabcap * 2 : 64;
        g_fib_tab = (AFib **)realloc(g_fib_tab, (size_t)nc * sizeof(AFib *));
        g_fib_freeidx = (int *)realloc(g_fib_freeidx, (size_t)nc * sizeof(int));
        g_fib_tabcap = nc;
    }
    g_fib_tab[g_fib_tabn] = f;
    return g_fib_tabn++;
}

static void fib_idx_free(int idx) {
    if (idx >= 0) g_fib_freeidx[g_fib_free_n++] = idx;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t fib_now_ms(void) {
    return a_rt_mono_ms();
}

static void fib_ready_push(AFib *f) {
    if (f->state == FIB_DONE || f->state == FIB_READY) return;
    f->state = FIB_READY;
    f->next_ready = NULL;
    if (g_ready_tail) g_ready_tail->next_ready = f; else g_ready_head = f;
    g_ready_tail = f;
}

static AFib *fib_ready_pop(void) {
    AFib *f = g_ready_head;
    if (!f) return NULL;
    g_ready_head = f->next_ready;
    if (!g_ready_head) g_ready_tail = NULL;
    f->next_ready = NULL;
    return f;
}

/* Unlink f from the ready queue. state==FIB_READY is the queue-membership
   invariant (push sets it, pop leaves it until the scheduler marks RUN),
   so only READY fibers can be linked. Without this, freeing a fiber that
   is still queued (reap at exit, cancel, mid-run await) leaves
   g_ready_head/g_ready_tail pointing at freed memory. */
static void fib_ready_unlink(AFib *f) {
    if (f->state != FIB_READY) { f->next_ready = NULL; return; }
    AFib **pp = &g_ready_head;
    while (*pp && *pp != f) pp = &(*pp)->next_ready;
    if (*pp) *pp = f->next_ready;
    if (g_ready_tail == f) {
        g_ready_tail = NULL;
        for (AFib *x = g_ready_head; x; x = x->next_ready)
            if (!x->next_ready) g_ready_tail = x;
    }
    f->next_ready = NULL;
}

static void fib_move_done(AFib *f) {
    /* unlink from g_fibs */
    AFib **pp = &g_fibs;
    while (*pp && *pp != f) pp = &(*pp)->next_all;
    if (*pp) *pp = f->next_all;
    f->next_all = NULL;
    /* push to done list (keeps result readable until reap) */
    f->next_done = NULL;
    if (g_fib_done_tail) g_fib_done_tail->next_done = f; else g_fib_done = f;
    g_fib_done_tail = f;
}

static void fib_unlink_done(AFib *f) {
    AFib **pp = &g_fib_done;
    while (*pp && *pp != f) pp = &(*pp)->next_done;
    if (*pp) {
        *pp = f->next_done;
        if (g_fib_done_tail == f) {
            /* tail may now be wrong; rescan (lists are tiny) */
            g_fib_done_tail = NULL;
            for (AFib *x = g_fib_done; x; x = x->next_done)
                if (!x->next_done) g_fib_done_tail = x;
        }
    }
    f->next_done = NULL;
}

static void fib_free(AFib *f) {
    if (!f) return;
    fib_ready_unlink(f);
    fib_unlink_done(f);
    AFib **pp = &g_fibs;
    while (*pp && *pp != f) pp = &(*pp)->next_all;
    if (*pp) *pp = f->next_all;
    if (f->args && f->args != f->small_argv) a_gc_free(f->args);
    if (f->stack) free(f->stack);   /* raw malloc: a_gc_free passthrough */
    fib_idx_free(f->idx);
    free(f);                        /* xmalloc'd struct: a_gc_free path  */
}

/* ------------------------------------------------------------------ */
/* GC integration                                                      */
/* ------------------------------------------------------------------ */

static void fib_gc_roots(void) {
    for (AFib *f = g_fibs; f; f = f->next_all) {
        a_gc_mark_ptr(f);            /* struct: ucontext regs + args ptr */
        if (f->args && f->args != f->small_argv) a_gc_mark_ptr(f->args);
        if (f != a_fib_current && f->sp && f->stack) {
            uintptr_t lo = (uintptr_t)f->sp;
            uintptr_t hi = (uintptr_t)f->stack + f->stack_size;
            if (hi > lo) a_gc_scan_region(lo, hi);
        }
    }
    /* DONE fibers still hold `result` avals and args until reaped. */
    for (AFib *f = g_fib_done; f; f = f->next_done) {
        a_gc_mark_ptr(f);
        if (f->args && f->args != f->small_argv) a_gc_mark_ptr(f->args);
        if (f->sp && f->stack) {
            uintptr_t lo = (uintptr_t)f->sp;
            uintptr_t hi = (uintptr_t)f->stack + f->stack_size;
            if (hi > lo) a_gc_scan_region(lo, hi);
        }
    }
}

static void fib_register_rootcb(void) {
    if (!g_rootcb_done) {
        g_rootcb_done = 1;
        a_gc_add_root_cb(fib_gc_roots);
    }
}

/* Current fiber's stack bounds — used by gc.c mark_c_stack when the GC
   runs inside a fiber (pthread bounds would name the main stack). */
int a_fib_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    AFib *f = a_fib_current;
    if (!f || !f->stack) return 0;
    *lo = (uintptr_t)f->stack;
    *hi = *lo + f->stack_size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* spawn + trampoline                                                  */
/* ------------------------------------------------------------------ */

static void fib_trampoline(int idx);

static AFib *fib_spawn_common(void) {
    AFib *f = (AFib *)xmalloc(sizeof(AFib));
    memset(f, 0, sizeof(*f));
    f->state = FIB_READY;
    f->result = A_NONE;
    f->wake_ms = INT64_MAX;
    f->stack_size = fib_stack_size();
    /* Raw malloc: the stack is scanned explicitly over [sp, top) by
       fib_gc_roots, so it must NOT be a GC block (a full-block scan per
       fiber per collection would be far too expensive, and the memory
       must be releasable without waiting for a GC cycle). free() routes
       through a_gc_free, which passes untracked pointers to real free. */
    f->stack = malloc(f->stack_size);
    if (!f->stack) panicf("out of memory (fiber stack)");
    f->next_all = g_fibs;
    g_fibs = f;
    return f;
}

static void fib_setup_ctx(AFib *f) {
    if (getcontext(&f->ctx) != 0) panicf("getcontext failed");
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = f->stack_size;
    f->ctx.uc_link = &g_sched_ctx;
    f->idx = fib_idx_alloc(f);
    makecontext(&f->ctx, (void (*)(void))fib_trampoline, 1, f->idx);
    f->next_ready = NULL;
    if (g_ready_tail) g_ready_tail->next_ready = f; else g_ready_head = f;
    g_ready_tail = f;
}

static void fib_fill_argv(AFib *f, aval env, uint64_t n, aval *args) {
    f->args = NULL;
    if (n + 1 <= 24) {
        f->args = f->small_argv;
    } else {
        f->args = (aval *)xmalloc((size_t)(n + 1) * sizeof(aval));
    }
    f->args[0] = env;
    if (n && args) memcpy(f->args + 1, args, (size_t)n * sizeof(aval));
}

static void fib_trampoline(int idx) {
    AFib *f = idx >= 0 ? g_fib_tab[idx] : NULL;
    if (!f) { swapcontext(NULL, &g_sched_ctx); return; }
    a_fib_current = f;
    f->sp = __builtin_frame_address(0);
    aval r;
    if (f->is_raw) {
        aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)f->raw_entry;
        r = code(f->args);
    } else {
        /* fnval call: a_call expects argv WITHOUT env (it pulls env from
           the AFnon). Our args[0] is env for raw calls; for fnval calls
           pass args+1. */
        r = a_call(f->fnval, f->nargs, f->args ? f->args + 1 : NULL);
    }
    f->result = r;
    f->state = FIB_DONE;
    a_fib_current = NULL;
    /* Return to the scheduler context (uc_link also covers a raw return). */
    swapcontext(&f->ctx, &g_sched_ctx);
    /* unreachable */
}

/* Promote every aval escaping into a fiber: the fiber can outlive the
   spawning frame's region, and a_region_leave bulk-frees unmarked young
   objects regardless of reachability. Scalars early-out inside a_promote. */
static void fib_promote_argv(aval fnval, aval env, uint64_t n, aval *args) {
    a_promote(fnval);
    a_promote(env);
    if (!args) return;
    for (uint64_t i = 0; i < n; ++i) a_promote(args[i]);
}

/* Spawn a fiber running an A function value with n args (a_call ABI). */
AFib *a_fib_spawn_fn(aval fnval, uint64_t n, aval *args) {
    if (!fib_enabled()) return NULL;
    AFib *f = fib_spawn_common();
    f->is_raw = 0;
    f->fnval = fnval;
    f->nargs = n;
    /* env slot unused for fnval calls but keep argv layout uniform */
    fib_fill_argv(f, A_NONE, n, args);
    fib_promote_argv(fnval, A_NONE, n, args);
    fib_setup_ctx(f);
    return f;
}

/* Spawn a fiber running a raw C entry aval(*)(aval*) with env+args. */
AFib *a_fib_spawn_raw(uint64_t entry, aval env, uint64_t n, aval *args) {
    if (!fib_enabled()) return NULL;
    AFib *f = fib_spawn_common();
    f->is_raw = 1;
    f->raw_entry = entry;
    f->raw_env = env;
    f->nargs = n;
    fib_fill_argv(f, env, n, args);
    /* raw entry is a code pointer, not a GC object: only env/args promote */
    fib_promote_argv(A_NONE, env, n, args);
    fib_setup_ctx(f);
    return f;
}

/* ------------------------------------------------------------------ */
/* blocking-op worker pool (real pthreads; raw malloc ONLY inside fn)  */
/* ------------------------------------------------------------------ */

typedef struct ABlockJob {
    void *(*fn)(void *);
    void *arg;
    void *result;
    AFib *fiber;
    struct ABlockJob *next;
} ABlockJob;

#define BLOCK_POOL_SIZE 8
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bwork = PTHREAD_COND_INITIALIZER;
static ABlockJob *g_bqueue, *g_bdone;
static int g_bstarted;
static int g_wake_pipe[2] = { -1, -1 };

static void *block_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_bmu);
        while (!g_bqueue) pthread_cond_wait(&g_bwork, &g_bmu);
        ABlockJob *j = g_bqueue;
        g_bqueue = j->next;
        pthread_mutex_unlock(&g_bmu);
        j->result = j->fn(j->arg);
        pthread_mutex_lock(&g_bmu);
        j->next = g_bdone;
        g_bdone = j;
        pthread_mutex_unlock(&g_bmu);
        if (g_wake_pipe[1] >= 0) {
            char b = 1;
            (void)!write(g_wake_pipe[1], &b, 1);
        }
    }
    return NULL;
}

static void block_pool_init(void) {
    if (g_bstarted) return;
    g_bstarted = 1;
    int p[2];
    if (pipe(p) == 0) {
        fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
        fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
        g_wake_pipe[0] = p[0];
        g_wake_pipe[1] = p[1];
    }
    int want = BLOCK_POOL_SIZE;
    const char *e = getenv("A_BLOCK_THREADS");
    if (e && *e) {
        int v = atoi(e);
        if (v > 0 && v <= 64) want = v;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u * 1024);
    for (int i = 0; i < want; ++i) {
        pthread_t th;
        if (pthread_create(&th, &attr, block_worker, NULL) != 0) break;
    }
    pthread_attr_destroy(&attr);
}

static void block_drain_done(void) {
    pthread_mutex_lock(&g_bmu);
    ABlockJob *j = g_bdone;
    g_bdone = NULL;
    pthread_mutex_unlock(&g_bmu);
    while (j) {
        ABlockJob *nx = j->next;
        if (j->fiber && j->fiber->state == FIB_WAIT_TASK) {
            j->fiber->task_result = j->result;
            j->fiber->job = NULL;
            fib_ready_push(j->fiber);
        }
        free(j);   /* raw malloc -> a_gc_free passthrough */
        j = nx;
    }
}

/* Run fn(arg) on a blocking-pool worker; inside a fiber this yields the
   scheduler until the job completes. fn MUST use raw malloc only. */
void *a_fib_blocking(void *(*fn)(void *), void *arg) {
    if (!fib_enabled() || !a_fib_current) return fn(arg);
    block_pool_init();
    ABlockJob *j = (ABlockJob *)malloc(sizeof(ABlockJob));
    if (!j) panicf("out of memory (block job)");
    j->fn = fn; j->arg = arg; j->result = NULL;
    j->fiber = a_fib_current; j->next = NULL;
    pthread_mutex_lock(&g_bmu);
    j->next = g_bqueue;
    g_bqueue = j;
    pthread_cond_signal(&g_bwork);
    pthread_mutex_unlock(&g_bmu);
    a_fib_current->job = j;
    a_fib_current->state = FIB_WAIT_TASK;
    a_fib_current->sp = (void *)__builtin_frame_address(0);
    swapcontext(&a_fib_current->ctx, &g_sched_ctx);
    void *r = a_fib_current->task_result;
    a_fib_current->task_result = NULL;
    return r;
}

/* ------------------------------------------------------------------ */
/* yield primitives (called from patched builtins, inside a fiber)     */
/* ------------------------------------------------------------------ */

int a_fib_on(void) { return a_fib_current != NULL; }

void a_fib_sleep_ms(int64_t ms) {
    if (!a_fib_current) {
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        return;
    }
    AFib *f = a_fib_current;
    f->wake_ms = fib_now_ms() + (ms > 0 ? ms : 0);
    f->state = FIB_WAIT_TIMER;
    f->sp = (void *)__builtin_frame_address(0);
    swapcontext(&f->ctx, &g_sched_ctx);
    f->wake_ms = INT64_MAX;
}

/* Wait until fd has `events` (POLLIN/POLLOUT/...) ready or timeout.
   Returns poll revents mask; 0 means timeout/none. Only inside fibers —
   callers check a_fib_on() first. */
int a_fib_wait_fd(int fd, short events, int64_t timeout_ms) {
    AFib *f = a_fib_current;
    if (!f) return 0;
    f->wait_fd = fd;
    f->wait_events = events;
    f->wait_revents = 0;
    f->wake_ms = timeout_ms >= 0 ? fib_now_ms() + timeout_ms : INT64_MAX;
    f->state = FIB_WAIT_FD;
    f->sp = (void *)__builtin_frame_address(0);
    swapcontext(&f->ctx, &g_sched_ctx);
    int rv = f->wait_revents;
    f->wait_revents = 0;
    f->wake_ms = INT64_MAX;
    return rv;
}

/* Wait for another fiber to reach DONE (structured concurrency). */
void a_fib_wait_fiber(AFib *target) {
    if (!a_fib_current || !target || target == a_fib_current || target->state == FIB_DONE)
        return;
    AFib *f = a_fib_current;
    f->wait_fib = target;
    f->state = FIB_WAIT_FIB;
    f->sp = (void *)__builtin_frame_address(0);
    swapcontext(&f->ctx, &g_sched_ctx);
    f->wait_fib = NULL;
}

/* ------------------------------------------------------------------ */
/* scheduler                                                           */
/* ------------------------------------------------------------------ */

static void sched_run(void) {
    fib_register_rootcb();
    for (;;) {
        /* move finished fibers to the done list */
        {
            AFib **pp = &g_fibs;
            while (*pp) {
                AFib *f = *pp;
                if (f->state == FIB_DONE) {
                    *pp = f->next_all;
                    f->next_all = NULL;
                    f->next_done = NULL;
                    if (g_fib_done_tail) g_fib_done_tail->next_done = f;
                    else g_fib_done = f;
                    g_fib_done_tail = f;
                } else {
                    pp = &f->next_all;
                }
            }
        }
        /* Wake WAIT_FIB fibers whose target already finished — MUST happen
           before poll(): with no timers/fd waits pending, poll(-1) would
           otherwise sleep forever and the waiter would never be scheduled
           (fibers only reach DONE while another fiber runs, so checking
           here each iteration is sufficient). */
        for (AFib *f = g_fibs; f; f = f->next_all)
            if (f->state == FIB_WAIT_FIB && f->wait_fib &&
                f->wait_fib->state == FIB_DONE)
                fib_ready_push(f);
        /* DONE fibers live in g_fib_done and do NOT keep the scheduler
           alive: their results stay readable until fib_await/reap frees
           them. Only unfinished fibers (g_fibs) matter. */
        int live = 0;
        for (AFib *f = g_fibs; f; f = f->next_all) ++live;
        if (!live) break;
        /* ---- build poll set: wake pipe + distinct waited fds ---- */
        /* count distinct fds first (upper bound = live fibers + 1) */
        size_t cap = 1;
        for (AFib *f = g_fibs; f; f = f->next_all)
            if (f->state == FIB_WAIT_FD) ++cap;
        struct pollfd *pfds = (struct pollfd *)malloc(cap * sizeof(struct pollfd));
        AFib ***waiters = (AFib ***)malloc(cap * sizeof(AFib **));
        int np = 0;
        if (g_wake_pipe[0] >= 0) {
            pfds[np].fd = g_wake_pipe[0];
            pfds[np].events = POLLIN;
            pfds[np].revents = 0;
            waiters[np] = NULL;
            ++np;
        }
        /* group WAIT_FD fibers by fd: one pollfd per distinct fd, and a
           small list of fibers per slot (chains via next_wait are NOT
           used — fibers are matched by scanning g_fibs after poll). */
        for (AFib *f = g_fibs; f; f = f->next_all) {
            if (f->state != FIB_WAIT_FD) continue;
            int slot = -1;
            for (int k = (g_wake_pipe[0] >= 0 ? 1 : 0); k < np; ++k)
                if (pfds[k].fd == f->wait_fd) { slot = k; break; }
            if (slot < 0) {
                slot = np++;
                pfds[slot].fd = f->wait_fd;
                pfds[slot].events = 0;
                pfds[slot].revents = 0;
            }
            pfds[slot].events |= f->wait_events;
        }
        for (int k = 0; k < np; ++k)
            if (!pfds[k].events) pfds[k].events = POLLIN | POLLOUT;

        /* ---- nearest deadline across timed waits ---- */
        int64_t now = fib_now_ms();
        int64_t next_ms = INT64_MAX;
        for (AFib *f = g_fibs; f; f = f->next_all) {
            if ((f->state == FIB_WAIT_TIMER || f->state == FIB_WAIT_FD) &&
                f->wake_ms < next_ms)
                next_ms = f->wake_ms;
        }
        int timeout = -1;
        if (next_ms != INT64_MAX) {
            int64_t d = next_ms - now;
            timeout = d <= 0 ? 0 : (d > 3600000 ? 3600000 : (int)d);
        }
        /* Ready fibers must run before we sleep: poll(…, -1) with no
           registered fds would otherwise hang the scheduler forever. */
        if (g_ready_head) timeout = 0;
        if (np > 0 || timeout >= 0) {
            poll(pfds, (nfds_t)np, timeout);
        } else {
            /* Nothing can wake us except dispatch-side events (done
               targets, pool completions): idle briefly, never spin. */
            poll(NULL, (nfds_t)0, 50);
        }

        /* ---- dispatch ---- */
        int pipe_ready = 0;
        for (int i = 0; i < np; ++i)
            if (waiters[i] == NULL && (pfds[i].revents & POLLIN))
                pipe_ready = 1;
        if (pipe_ready) {
            if (g_wake_pipe[0] >= 0) {
                char buf[256];
                while (read(g_wake_pipe[0], buf, sizeof buf) > 0) {}
            }
        }
        block_drain_done();   /* cheap; also covers lost pipe writes */

        /* fd readiness + timer expiry */
        now = fib_now_ms();
        for (AFib *f = g_fibs; f; f = f->next_all) {
            if (f->state == FIB_WAIT_FD) {
                short rev = 0;
                for (int k = 0; k < np; ++k)
                    if (pfds[k].fd == f->wait_fd) { rev = pfds[k].revents; break; }
                if (rev & f->wait_events) {
                    f->wait_revents = rev & (f->wait_events | POLLERR |
                                            POLLHUP | POLLNVAL);
                    fib_ready_push(f);
                } else if (f->wake_ms <= now) {
                    fib_ready_push(f);   /* timeout: revents stays 0 */
                }
            } else if (f->state == FIB_WAIT_TIMER && f->wake_ms <= now) {
                fib_ready_push(f);
            } else if (f->state == FIB_WAIT_FIB && f->wait_fib &&
                       f->wait_fib->state == FIB_DONE) {
                fib_ready_push(f);
            }
        }

        free(pfds);
        free(waiters);

        /* ---- run next ready fiber ---- */
        AFib *f = fib_ready_pop();
        if (!f) {
            for (AFib *x = g_fibs; x; x = x->next_all)
                fprintf(stderr, " %p(st=%d)", (void*)x, (int)x->state);
            fprintf(stderr, " readyhead=%p np=%d timeout=%d\n",
                    (void*)g_ready_head, np, timeout);
            continue;
        }
        f->state = FIB_RUN;
        /* a_fib_current must name the RUNNING fiber at all times: yield builtins
           (a_fib_on, a_fib_sleep_ms, a_fib_blocking) and GC stack scans
           depend on it. The trampoline only sets it on first entry, so
           the scheduler maintains it on every resume/yield. */
        a_fib_current = f;
        swapcontext(&g_sched_ctx, &f->ctx);
        a_fib_current = NULL;
        /* fiber yielded (state WAIT_*) or finished (state DONE): loop */
    }
}

/* Wait for `f` to finish. Inside a fiber: cooperative wait. Outside:
   drive the scheduler (used by a_fib_main). */
static aval fib_await(AFib *f) {
    if (!f) return A_NONE;
    if (a_fib_current) {
        a_fib_wait_fiber(f);
        aval r = f->result;
        fib_free(f);
        return r;
    }
    sched_run();
    aval r = f->result;
    fib_free(f);
    return r;
}

/* Spawn an A fnval on a fiber and wait for its result. Outside a fiber
   this drives the whole scheduler; inside one it waits cooperatively. */
/* Run a raw C entry inside a fiber + scheduler (emitMain wraps
   __a_main_body this way). Falls back to a direct call when off. */
aval a_fib_main(uint64_t entry, aval env, uint64_t n, aval *args) {
    if (!fib_enabled()) {
        aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)entry;
        aval *av = (aval *)xmalloc((size_t)(n + 1) * sizeof(aval));
        av[0] = env;
        if (n && args) memcpy(av + 1, args, (size_t)n * sizeof(aval));
        aval r = code(av);
        free(av);
        return r;
    }
    AFib *f = a_fib_spawn_raw(entry, env, n, args);
    aval rr = fib_await(f);
    return rr;
}

/* Reap every remaining fiber (program exit path). The currently running
   fiber (a_fib_current — the main body itself) is skipped: freeing its stack
   while it executes would be a use-after-free; it is freed by fib_await
   once it finishes. */
static void reap_one(AFib *f) {
    /* A fiber suspended in FIB_WAIT_TASK has blocking-pool workers that
       may still write job results into its stack — the per-call job
       structs (OsSysJob, FsReadJob, NetSslJob, ...) live in the caller's
       frame ON the fiber stack. The process is exiting anyway, so leak
       the stack rather than free it under the workers' feet. */
    if (f->state == FIB_WAIT_TASK) f->stack = NULL;
    fib_free(f);
}

void a_fib_reap_all(void) {
    AFib **pp = &g_fibs;
    while (*pp) {
        AFib *f = *pp;
        if (f == a_fib_current) { pp = &f->next_all; continue; }
        *pp = f->next_all;
        f->next_all = NULL;
        reap_one(f);
    }
    pp = &g_fib_done;
    while (*pp) {
        AFib *f = *pp;
        if (f == a_fib_current) { pp = &f->next_done; continue; }
        *pp = f->next_done;
        f->next_done = NULL;
        reap_one(f);
    }
    /* drain any pending blocking jobs (their fibers are gone; results
       are raw allocations owned by nobody — free the job shells). */
    pthread_mutex_lock(&g_bmu);
    ABlockJob *j = g_bdone;
    g_bdone = NULL;
    pthread_mutex_unlock(&g_bmu);
    while (j) {
        ABlockJob *nx = j->next;
        free(j);
        j = nx;
    }
}

/* ------------------------------------------------------------------ */
/* public predicates                                                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* builtins: async.spawn / async.join                                  */
/* ------------------------------------------------------------------ */

/* spawn(fn, *args) -> task handle (integer). The fn runs on a fiber as
   soon as the scheduler gets to it; join(handle) waits for the result.
   With fibers disabled spawn runs fn inline and returns -1 (join on -1
   panics — callers should check or use join only on real handles). */
/* Spawn wrapper for compiler-emitted auto-parfor: returns the task
   handle aval directly so codegen can collect handles without going
   through the builtin dispatch layer. */
aval a_fib_spawn_handle(aval fnval, uint64_t n, aval *args) {
    if (!fib_enabled()) {
        a_call(fnval, n, args);
        return mkint_full(-1);
    }
    AFib *f = a_fib_spawn_fn(fnval, n, args);
    if (!f) {
        a_call(fnval, n, args);
        return mkint_full(-1);
    }
    return mkint_full((int64_t)(intptr_t)f);
}

/* Join wrapper for compiler-emitted auto-parfor: a -1 handle means the
   body already ran inline (fibers disabled), so there is nothing to wait
   for — return None instead of panicking like bi_fib_join does. */
aval a_fib_join_handle(aval h) {
    int64_t v = expect_int64(h, "join handle");
    if (v == -1) return A_NONE;
    aval tmp[1];
    tmp[0] = h;
    return bi_fib_join(1, tmp);
}

aval bi_fib_spawn(uint64_t n, aval *argv) {
    want_args("spawn", n, 1, 64);
    if (!fib_enabled()) {
        /* No scheduler: run inline, signal "already done" with -1. */
        a_call(argv[0], n - 1, argv + 1);
        return mkint_full(-1);
    }
    AFib *f = a_fib_spawn_fn(argv[0], n - 1, argv + 1);
    if (!f) {
        a_call(argv[0], n - 1, argv + 1);
        return mkint_full(-1);
    }
    return mkint_full((int64_t)(intptr_t)f);
}

static AFib *fib_from_handle(aval hv) {
    int64_t h = expect_int64(hv, "join() task handle");
    if (h == -1) panicf("join() on a task that already ran inline");
    AFib *f = (AFib *)(intptr_t)h;
    if (!f) panicf("join() on an invalid task handle");
    return f;
}

/* join(handle) -> result. Inside a fiber this is a cooperative wait;
   outside it drives the scheduler to completion for that task. */
aval bi_fib_join(uint64_t n, aval *argv) {
    want_args("join", n, 1, 1);
    AFib *f = fib_from_handle(argv[0]);
    if (f->joined) panicf("join() on an already-joined task");
    if (a_fib_on()) {
        if (f->state != FIB_DONE) a_fib_wait_fiber(f);
    } else {
        sched_run();
    }
    /* Keep the (now DONE) fiber allocated until a_fib_reap_all at exit:
       the handle stays valid for done()/inspection and double-join is
       caught by the joined flag instead of a use-after-free. */
    f->joined = 1;
    return f->result;
}

/* done(handle) -> bool: non-blocking completion check. */
aval bi_fib_done(uint64_t n, aval *argv) {
    want_args("done", n, 1, 1);
    AFib *f = fib_from_handle(argv[0]);
    return bool_aval(f->state == FIB_DONE);
}

/* cancel is not supported (fibers are cooperative); kept for API
   symmetry — it is a no-op that returns the handle unchanged. */
aval bi_fib_cancel(uint64_t n, aval *argv) {
    want_args("cancel", n, 1, 1);
    fib_from_handle(argv[0]);
    return argv[0];
}

#else /* !A_FIBERS — synchronous stubs */

int a_fib_on(void) { return 0; }
int a_fib_stack_bounds(uintptr_t *lo, uintptr_t *hi) { (void)lo; (void)hi; return 0; }
void a_fib_sleep_ms(int64_t ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
void *a_fib_blocking(void *(*fn)(void *), void *arg) { return fn(arg); }
int a_fib_wait_fd(int fd, short events, int64_t timeout_ms) {
    (void)fd; (void)events; (void)timeout_ms; return 0;
}
struct AFib_opaque;
AFib *a_fib_spawn_fn(aval fnval, uint64_t n, aval *args) { (void)fnval; (void)n; (void)args; return NULL; }
aval a_fib_spawn_handle(aval fnval, uint64_t n, aval *args) {
    a_call(fnval, n, args);
    return mkint_full(-1);
}
aval a_fib_join_handle(aval h) { (void)h; return A_NONE; }
AFib *a_fib_spawn_raw(uint64_t entry, aval env, uint64_t n, aval *args) { (void)entry; (void)env; (void)n; (void)args; return NULL; }
void a_fib_wait_fiber(AFib *target) { (void)target; }
aval a_fib_main(uint64_t entry, aval env, uint64_t n, aval *args) {
    aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)entry;
    aval *av = (aval *)xmalloc((size_t)(n + 1) * sizeof(aval));
    av[0] = env;
    if (n && args) memcpy(av + 1, args, (size_t)n * sizeof(aval));
    aval r = code(av);
    free(av);
    return r;
}
void a_fib_reap_all(void) {}

aval bi_fib_spawn(uint64_t n, aval *argv) {
    want_args("spawn", n, 1, 64);
    a_call(argv[0], n - 1, argv + 1);
    return mkint_full(-1);
}
aval bi_fib_join(uint64_t n, aval *argv) {
    want_args("join", n, 1, 1);
    int64_t h = expect_int64(argv[0], "join() task handle");
    if (h == -1) panicf("join() on a task that already ran inline");
    panicf("join() requires async support on this platform");
    return A_NONE;
}
aval bi_fib_done(uint64_t n, aval *argv) {
    want_args("done", n, 1, 1);
    return A_TRUE;
}
aval bi_fib_cancel(uint64_t n, aval *argv) {
    want_args("cancel", n, 1, 1);
    return argv[0];
}

#endif /* A_FIBERS */
