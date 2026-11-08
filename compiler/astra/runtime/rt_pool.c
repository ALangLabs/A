/* rt_pool.c — implicit-parallel worker pool for runtime kernels.
 *
 * A small persistent M:N-style pool: N pthread workers (N = online CPUs,
 * capped) pull chunk descriptors off a shared queue. a_par_for() splits a
 * range into chunks and the caller participates in draining the queue, so
 * a parallel region always uses every core.
 *
 * Availability:
 *   - Real threads only when A_PAR_REAL (system-C-compiled runtime on
 *     POSIX: the staged libaruntime.a path). astra-compiled runtimes
 *     (__TINYC__: cross targets, A_RUNTIME_CC=off) and Windows compile
 *     the same file but take the serial inline path — no pthread.h, no
 *     threads, identical results.
 *   - A_PAR=0 (or "off"/"no") forces the serial path everywhere.
 *   - A_PAR_THREADS=N overrides the worker count (0/1 => serial).
 *
 * Safety contract for kernel authors: fn(ctx,lo,hi) runs concurrently on
 * disjoint index ranges. It must only touch per-chunk storage and
 * read-only shared inputs, and must not allocate A heap objects unless the
 * allocation path is known thread-safe (xmalloc/free are; the int-box bump
 * allocator in core_util.c is mutex-guarded; generated A code is NOT —
 * a_frame_push and closure envs are unsynchronized, so arbitrary A
 * closures must never be invoked from pool workers). Nested a_par_for
 * calls from a worker degrade to serial.
 *
 * fork() safety: pthread_atfork handlers reset the pool in the child so a
 * forked child (e.g. the `a test` harness, terminal repl) can spawn its
 * own workers instead of deadlocking on mutexes held by vanished threads.
 */
#include "a_runtime_internal.h"

#if A_PAR_REAL

#define A_PAR_MAX_WORKERS 32
#define A_PAR_MAX_CHUNKS  512

typedef struct {
    void *ctx;
    AParFn fn;
    int64_t lo, hi;
} AParTask;

static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pwork = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_pdone = PTHREAD_COND_INITIALIZER;
static AParTask *g_ptasks;
static int64_t g_pnext, g_pcount, g_ppending;
static int g_pactive, g_pworkers, g_pstarted;
static int g_atfork_reg;
static __thread int g_pinworker;

static void par_run_inline(int64_t n, int64_t grain, AParFn fn, void *ctx) {
    for (int64_t lo = 0; lo < n; lo += grain) {
        int64_t hi = lo + grain;
        if (hi > n) hi = n;
        fn(ctx, lo, hi);
    }
}

static int par_disabled(void) {
    static int d = -1;
    if (d < 0) {
        const char *e = getenv("A_PAR");
        d = (e && (*e == '0' || *e == 'o' || *e == 'n' || *e == 'O' || *e == 'N')) ? 1 : 0;
    }
    return d;
}

static int par_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    const char *e = getenv("A_PAR_THREADS");
    if (e && *e) {
        long v = strtol(e, NULL, 10);
        if (v >= 0) n = v;
    }
    if (n > A_PAR_MAX_WORKERS) n = A_PAR_MAX_WORKERS;
    return (int)n;
}

static void *pool_worker(void *unused) {
    (void)unused;
    g_pinworker = 1;
    for (;;) {
        pthread_mutex_lock(&g_pmu);
        while (!(g_pactive && g_pnext < g_pcount))
            pthread_cond_wait(&g_pwork, &g_pmu);
        AParTask t = g_ptasks[g_pnext++];
        pthread_mutex_unlock(&g_pmu);
        t.fn(t.ctx, t.lo, t.hi);
        pthread_mutex_lock(&g_pmu);
        if (--g_ppending == 0) {
            g_pactive = 0;
            pthread_cond_broadcast(&g_pdone);
        }
        pthread_mutex_unlock(&g_pmu);
    }
    return NULL;
}

static void pool_atfork_prepare(void) { pthread_mutex_lock(&g_pmu); }
static void pool_atfork_parent(void)  { pthread_mutex_unlock(&g_pmu); }
static void pool_atfork_child(void) {
    /* Only the forking thread survives; worker threads and any in-flight
       batch are gone. Rebuild the sync objects and let the next a_par_for
       respawn workers. Runs with g_pmu locked by the prepare handler. */
    g_ptasks = NULL;
    g_pnext = g_pcount = g_ppending = 0;
    g_pactive = 0;
    g_pworkers = 0;
    g_pstarted = 0;
    /* g_atfork_reg intentionally survives: atfork registrations are
       inherited across fork, so the child must not register again. */
    g_pinworker = 0;
    pthread_mutex_init(&g_pmu, NULL);
    pthread_cond_init(&g_pwork, NULL);
    pthread_cond_init(&g_pdone, NULL);
}

static int pool_ensure(void) {
    if (g_pstarted) return g_pworkers;
    g_pstarted = 1;
    if (!g_atfork_reg) {
        g_atfork_reg = 1;
        pthread_atfork(pool_atfork_prepare, pool_atfork_parent, pool_atfork_child);
    }
    int want = par_ncpu();
    if (want < 2) return 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1u << 20);
    int made = 0;
    for (int i = 0; i < want; ++i) {
        pthread_t th;
        if (pthread_create(&th, &attr, pool_worker, NULL) == 0) ++made;
    }
    pthread_attr_destroy(&attr);
    g_pworkers = made;
    if (made == 0) g_pstarted = 0; /* allow retry on next call */
    return made;
}

int a_par_workers(void) {
    if (par_disabled() || g_pinworker) return 1;
    return pool_ensure() > 0 ? g_pworkers : 1;
}

void a_par_for(int64_t n, int64_t grain, AParFn fn, void *ctx) {
    if (n <= 0 || !fn) return;
    if (grain < 1) grain = 1;
    if (par_disabled() || g_pinworker) { par_run_inline(n, grain, fn, ctx); return; }
    int workers = pool_ensure();
    if (workers < 2) { par_run_inline(n, grain, fn, ctx); return; }

    int64_t nchunks = (n + grain - 1) / grain;
    int64_t maxchunks = (int64_t)workers * 16;
    if (maxchunks < 32) maxchunks = 32;
    if (maxchunks > A_PAR_MAX_CHUNKS) maxchunks = A_PAR_MAX_CHUNKS;
    if (nchunks > maxchunks) {
        grain = (n + maxchunks - 1) / maxchunks;
        nchunks = (n + grain - 1) / grain;
    }
    if (nchunks < 2) { par_run_inline(n, grain, fn, ctx); return; }

    AParTask *tasks = (AParTask *)xmalloc((size_t)nchunks * sizeof(AParTask));
    for (int64_t c = 0; c < nchunks; ++c) {
        int64_t lo = c * grain;
        int64_t hi = lo + grain;
        if (hi > n) hi = n;
        tasks[c].ctx = ctx; tasks[c].fn = fn; tasks[c].lo = lo; tasks[c].hi = hi;
    }

    pthread_mutex_lock(&g_pmu);
    if (g_pactive) {
        /* Another thread is already running a batch; degrade to serial. */
        pthread_mutex_unlock(&g_pmu);
        free(tasks);
        par_run_inline(n, grain, fn, ctx);
        return;
    }
    g_ptasks = tasks;
    g_pnext = 0;
    g_pcount = nchunks;
    g_ppending = nchunks;
    g_pactive = 1;
    pthread_cond_broadcast(&g_pwork);
    /* The caller drains the queue too: pop under the lock, run unlocked,
       then account for the finished chunk exactly like a worker does. */
    while (g_pnext < g_pcount) {
        AParTask t = g_ptasks[g_pnext++];
        pthread_mutex_unlock(&g_pmu);
        t.fn(t.ctx, t.lo, t.hi);
        pthread_mutex_lock(&g_pmu);
        if (--g_ppending == 0) {
            g_pactive = 0;
            pthread_cond_broadcast(&g_pdone);
        }
    }
    while (g_ppending > 0)
        pthread_cond_wait(&g_pdone, &g_pmu);
    g_ptasks = NULL;
    pthread_mutex_unlock(&g_pmu);
    free(tasks);
}

#else /* !A_PAR_REAL: serial fallback (Windows, astra-compiled, i8086) */

int a_par_workers(void) { return 1; }

void a_par_for(int64_t n, int64_t grain, AParFn fn, void *ctx) {
    if (n <= 0 || !fn) return;
    if (grain < 1) grain = 1;
    for (int64_t lo = 0; lo < n; lo += grain) {
        int64_t hi = lo + grain;
        if (hi > n) hi = n;
        fn(ctx, lo, hi);
    }
}

#endif /* A_PAR_REAL */
