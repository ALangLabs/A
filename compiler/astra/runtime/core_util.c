#include "a_runtime_internal.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

/* A-level call frames for panic tracebacks (Python-style). Codegen emits
   a_frame_push() in every function prologue and a_frame_pop() on every
   return path; panicf() then prints the live stack, oldest frame first. */
typedef struct {
    const char *name;
    const char *file;
    int line;
} AFrame;

/* A_RT_TLS: pool workers pushing frames concurrently must not share the
   traceback stack (rt_pool.c safety contract). */
static A_RT_TLS AFrame *g_frames;
static A_RT_TLS size_t g_nframes;
static A_RT_TLS size_t g_capframes;

/* --------------------------------------------------------------- */
/* native stack-overflow guard                                     */
/*                                                                 */
/* Deep or unbounded recursion used to die with a bare SIGSEGV (exit */
/* 139) and no message. We watch the native stack from a_frame_push */
/* (emitted in every function prologue) and abort with a clean A     */
/* error before the guard page is reached.                          */
/* --------------------------------------------------------------- */
#ifndef A_STACK_GUARD_MARGIN
#define A_STACK_GUARD_MARGIN (128u * 1024u)
#endif
#ifndef A_STACK_GUARD_BUDGET
#define A_STACK_GUARD_BUDGET (512u * 1024u)
#endif

#if defined(__linux__) && !defined(__TINYC__)
static A_RT_TLS char *g_stack_lo;      /* lowest usable address */
static A_RT_TLS size_t g_stack_size;   /* usable size in bytes */
static A_RT_TLS int g_stack_have_bounds;
#endif
static A_RT_TLS int g_stack_guard_init;
static A_RT_TLS char *g_stack_probe;   /* shallowest observed SP */

/* Source line of the statement currently executing. Generated code stores
   it before every statement (`movl $N, a_stmt_line(%rip)` in the x86-64 asm
   emitter). Module frames push line 0 — a module has no declaration line —
   so panic locations fall back to this value. Not thread-local: a raced
   store can only make a diagnostic line less precise, never wrong-shaped. */
int a_stmt_line = 0;

static void print_frame_line(const AFrame *f, int line) {
    const char *file = f->file && *f->file ? f->file : "?";
    const char *name = f->name && *f->name ? f->name : "?";
    if (line > 0)
        fprintf(stderr, "  File \"%s\", line %d, in %s\n", file, line, name);
    else
        fprintf(stderr, "  File \"%s\", in %s\n", file, name);
}

/* Line to print for frame `i`: the frame's own line, or — for the innermost
   frame — the statement line the generated code last recorded. */
static int frame_line(size_t i) {
    int line = g_frames[i].line;
    if (line <= 0 && a_stmt_line > 0 && i + 1 == g_nframes) line = a_stmt_line;
    return line;
}

/* `main` pushes a "<module>" frame for the entry module and the entry
   module's __a_modinit_* function pushes another "<module>" frame for the
   same file while its top-level statements run, so a panic at top level
   prints the same synthetic frame twice in a row. Collapse consecutive
   identical <module> frames when printing (keeping the more precise line of
   the pair); real function frames are never named "<module>" and recursive
   calls keep distinct line numbers. */
static int same_module_frame(const AFrame *a, const AFrame *b) {
    if (!a->name || !b->name || strcmp(a->name, "<module>") != 0 ||
        strcmp(b->name, "<module>") != 0)
        return 0;
    if (a->file == b->file) return 1;
    if (!a->file || !b->file) return 0;
    return strcmp(a->file, b->file) == 0;
}

/* Print frames [start, g_nframes), collapsing a repeated <module> frame
   into the previous one (the later frame knows the statement line). */
static void print_frames(size_t start) {
    AFrame prev;
    int have_prev = 0;
    for (size_t i = start; i < g_nframes; ++i) {
        AFrame f = g_frames[i];
        f.line = frame_line(i);
        if (have_prev && same_module_frame(&prev, &f)) {
            if (f.line > prev.line) prev.line = f.line;
            continue;
        }
        if (have_prev) print_frame_line(&prev, prev.line);
        prev = f;
        have_prev = 1;
    }
    if (have_prev) print_frame_line(&prev, prev.line);
}

static void stack_overflow_error(uintptr_t sp) {
    (void)sp;
    fputs("error: stack overflow (possible infinite recursion)\n", stderr);
    if (g_nframes) {
        /* Print only the most recent frames: the deepest recursion is the
           useful part of the trace and avoids a multi-megabyte dump. */
        size_t start = g_nframes > 24 ? g_nframes - 24 : 0;
        fputs("Traceback (most recent call last):\n", stderr);
        if (start > 0) fprintf(stderr, "  ... %zu earlier frames ...\n", start);
        print_frames(start);
    }
    fflush(stderr);
    exit(2);
}

static void stack_guard_check(void) {
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (!g_stack_guard_init) {
        g_stack_guard_init = 1;
        g_stack_probe = (char *)sp;
#if defined(__linux__) && !defined(__TINYC__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void *base = NULL;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &base, &size) == 0 && base && size) {
                g_stack_lo = (char *)base;
                g_stack_size = size;
                g_stack_have_bounds = 1;
            }
            pthread_attr_destroy(&attr);
        }
#endif
    }
#if defined(__linux__) && !defined(__TINYC__)
    if (g_stack_have_bounds) {
        uintptr_t lo = (uintptr_t)g_stack_lo;
        if (sp < lo + A_STACK_GUARD_MARGIN) stack_overflow_error(sp);
        return;
    }
#endif
    if ((uintptr_t)g_stack_probe > sp) {
        if ((uintptr_t)g_stack_probe - sp > A_STACK_GUARD_BUDGET) stack_overflow_error(sp);
    } else {
        g_stack_probe = (char *)sp;
    }
}

void a_frame_push(const char *name, const char *file, int line) {
    stack_guard_check();
    if (g_nframes == g_capframes) {
        size_t ncap = g_capframes ? g_capframes * 2 : 64;
        AFrame *nf = (AFrame *)xrealloc(g_frames, ncap * sizeof(AFrame));
        g_frames = nf;
        g_capframes = ncap;
    }
    g_frames[g_nframes].name = name;
    g_frames[g_nframes].file = file;
    g_frames[g_nframes].line = line;
    ++g_nframes;
    a_region_enter(name);   /* young-generation region for this frame */
}

void a_frame_pop(void) {
    if (g_nframes) --g_nframes;
    a_region_leave();       /* sweep unescaped young objects of the frame */
}

static void print_traceback(void) {
    if (!g_nframes) return;
    fputs("Traceback (most recent call last):\n", stderr);
    /* Cap the dump so a deep recursion does not flood the terminal. */
    size_t start = 0;
    if (g_nframes > 64) {
        start = g_nframes - 64;
        fprintf(stderr, "  ... %zu earlier frames ...\n", start);
    }
    print_frames(start);
}

void panicf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    // Point at the innermost A frame so the message carries a source location
    // even before the full traceback (and when a builder only shows one line).
    for (size_t i = g_nframes; i-- > 0;) {
        const AFrame *f = &g_frames[i];
        int line = f->line;
        if (line <= 0 && i == g_nframes - 1) line = a_stmt_line;
        if (f->file && *f->file && line > 0) {
            fprintf(stderr, "  --> %s:%d\n", f->file, line);
            break;
        }
    }
    print_traceback();
    exit(2);
}

void *xmalloc(size_t n) {
    /* A GC may run before this allocation: everything reachable is marked
       through the roots scanned inside a_gc_maybe_collect. */
    a_gc_maybe_collect();
    size_t req = n ? n : 1;
    /* Reuse a block the last sweep delisted before asking libc for more
       memory. Pooled blocks are already 16-byte aligned and are
       re-registered below exactly like a fresh posix_memalign block. */
    void *p = a_gc_pool_try_alloc(req);
    if (p) {
        a_gc_register(p, req);
        return p;
    }
    /* Heap objects are NaN-boxed pointers distinguished by a zero low
     * nibble, so every object needs 16-byte alignment. 32-bit libcs
     * (ARM, i386, 32-bit Windows) only guarantee 8, so allocate aligned
     * explicitly; on those targets a_runtime_internal.h maps malloc/
     * realloc/free onto the _aligned_* CRT entry points so the pairing
     * stays consistent. Pool-eligible sizes request the size-class
     * capacity while the registry keeps the caller's requested extent. */
    size_t asize = a_gc_pool_alloc_size(req);
#if defined(_WIN32)
    /* Win64 CRT malloc returns 16-byte-aligned blocks; on 32-bit Windows
       the malloc macro in a_runtime_internal.h already routes this call
       to _aligned_malloc. */
    p = malloc(asize);
    if (!p) panicf("out of memory");
    a_gc_register(p, req);
    return p;
#else
    p = NULL;
    if (posix_memalign(&p, 16, asize) != 0 || !p)
        panicf("out of memory");
    a_gc_register(p, req);
    return p;
#endif
}

/* Arena chunks come straight from mmap with MADV_HUGEPAGE. A has no
   reclamation for values, so an allocation-heavy program faults in fresh
   memory continuously; when a minor fault costs microseconds (VMs, hardened
   hosts) that page traffic, not the bump pointer, is what an allocation
   costs. Huge pages cut the fault count by ~500x. Falls back to the heap
   where mmap/huge pages are unavailable, and untouched pages cost nothing. */
char *xarena(size_t n) {
    /* Arena-only churn must drive collections the same way xmalloc
       churn does, otherwise bump chunks never reclaim (unbounded RSS
       in small-string / int-box loops). Reclaim runs before the fresh
       chunk is mapped; stale cursors are refreshed by callers via
       a_gc_arena_stale afterwards. */
    a_gc_maybe_collect();
#if defined(__linux__) && defined(MAP_ANONYMOUS)
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
        (void)madvise(p, n, MADV_HUGEPAGE);
#endif
        a_gc_note_arena(p, n, 1); /* mmap bump chunk */
        return (char *)p;
    }
#endif
    /* Linux branch left `p` in scope above; assign rather than redeclare. */
    p = xmalloc(n);
    a_gc_note_arena(p, n, 0);  /* heap fallback bump chunk */
    return (char *)p;
}

void *xrealloc(void *p, size_t n) {
    a_gc_maybe_collect();
    size_t req = n ? n : 1;
    void *q;
    if (!p) {
        q = a_gc_pool_try_alloc(req);
        if (!q) {
            size_t asize = a_gc_pool_alloc_size(req);
#if defined(_WIN32)
            q = malloc(asize);
#else
            if (posix_memalign(&q, 16, asize) != 0) q = NULL;
#endif
        }
    } else {
        q = realloc(p, a_gc_pool_alloc_size(req));
    }
    if (!q) panicf("out of memory");
    a_gc_reregister(p, q, req);
    return q;
}

char *xstrndup(const char *s, size_t n) {
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

char *xstrdup(const char *s) {
    return xstrndup(s ? s : "", strlen(s ? s : ""));
}

/* __fixxfdi (int64_t)long double libcall is provided by libastra1.a
 * (lib/builtin.c); defining it here too would duplicate the symbol. */

aval mkobj(void *p) {
    uintptr_t u = (uintptr_t)p;
    if ((u & 0xFULL) != 0 || u >= (UINT64_C(1) << 48))
        panicf("object pointer out of NaN-boxed range");
    a_gc_note_obj(p); /* flag the backing block as a collectable object */
    a_region_note_obj(p); /* track in the innermost young region, if any */
    return A_TAG | (uint64_t)u;
}

AObj *obj_of(aval v) {
    if (!a_is_ptr(v)) return NULL;
    return (AObj *)(uintptr_t)A_PAYLOAD(v);
}

int is_int_val(aval v) {
    if (a_is_int(v)) return 1;
    AObj *o = obj_of(v);
    return o && o->type == T_INT64;
}

int64_t int_val_full(aval v) {
    if (a_is_int(v)) return a_int_val(v);
    AObj *o = obj_of(v);
    if (o && o->type == T_INT64) return ((AInt64 *)o)->v;
    panicf("internal: not an integer");
    return 0;
}

int is_num(aval v) { return is_int_val(v) || a_is_double(v); }

double num_to_double(aval v) {
    if (is_int_val(v)) return (double)int_val_full(v);
    if (a_is_double(v)) return a_double_val(v);
    panicf("internal: not a number");
    return 0.0;
}

int64_t expect_int64(aval v, const char *what) {
    if (!is_int_val(v))
        panicf("%s must be an integer, not '%s'", what, type_name_cstr(v));
    return int_val_full(v);
}

/* Inline ints cover [-2^46, 2^46); wider values box as T_INT64. */
/* Bump arena for int boxes. An aval is 47-bit inline; anything outside
   +/-2^46 needs a heap box. Those boxes are never freed (the runtime has no
   collector), so a chunked bump allocator is strictly cheaper than malloc per
   value: it removes malloc's size-class logic and bookkeeping, which was the
   entire cost of integer arithmetic that leaves the inline range (e.g. any
   product above 7e13, as in a 32-bit LCG step). */
#define INT_BOX_CHUNK (128u << 10)  /* 128 KiB chunks (see core_str.c) */
static char *g_intbox_cursor = NULL;
static size_t g_intbox_left = 0;

/* mkobj() requires 16-byte alignment (NaN-boxed pointers), so the bump step
   is sizeof(AInt64) rounded up to 16. */
#define INT_BOX_STEP (((sizeof(AInt64) + 15u) / 16u) * 16u)

#if A_PAR_REAL
/* The bump allocator is process-global; pool workers boxing out-of-range
   ints concurrently would race on the cursor. One mutex around the whole
   carve keeps it correct (contended only by >2^46 integer arithmetic). */
static pthread_mutex_t g_intbox_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

static AInt64 *intbox_alloc(void) {
#if A_PAR_REAL
    pthread_mutex_lock(&g_intbox_mu);
#endif
    if (g_intbox_cursor && a_gc_arena_stale(g_intbox_cursor))
        g_intbox_left = 0;
    if (g_intbox_left < INT_BOX_STEP) {
        void *old = g_intbox_cursor;
        g_intbox_cursor = xarena(INT_BOX_CHUNK);
        a_gc_arena_switch(old, g_intbox_cursor);
        g_intbox_left = INT_BOX_CHUNK;
    }
    AInt64 *p = (AInt64 *)g_intbox_cursor;
    g_intbox_cursor += INT_BOX_STEP;
    g_intbox_left -= INT_BOX_STEP;
#if A_PAR_REAL
    pthread_mutex_unlock(&g_intbox_mu);
#endif
    return p;
}

aval mkint_full(int64_t i) {
    const int64_t lim = INT64_C(1) << 46;
    if (i >= -lim && i < lim) return a_mkint(i);
    AInt64 *p = intbox_alloc();
    p->h.type = T_INT64;
    p->h.len = 0;
    p->h.cap = 0;
    p->v = i;
    return mkobj(&p->h);
}

aval mkdouble(double d) {
    if (isnan(d)) {
        union { uint64_t u; double d; } c;
        c.u = UINT64_C(0x7FF8000000000000);
        return c.u;
    }
    return a_mkdouble(d);
}

aval bool_aval(int b) { return b ? A_TRUE : A_FALSE; }

#if defined(_WIN32)
/* ------------------------------------------------------------------ */
/* Windows compat: snprintf shim + Win32 timing helpers               */
/* ------------------------------------------------------------------ */

/* msvcrt.dll has no snprintf export (it lives in libmingwex, which is not
   shipped); wrap the exported vsnprintf. Note the legacy vsnprintf
   returns -1 on truncation instead of the would-be length. */
int snprintf(char *s, size_t n, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(s, n, format, ap);
    va_end(ap);
    return r;
}

/* FILETIME counts 100 ns intervals since 1601-01-01 UTC; the Unix epoch
   offset is 11644473600 s. */
int64_t a_win_time_ms(void) {
    unsigned long long ft = 0;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)((ft - 116444736000000000ULL) / 10000ULL);
}

int64_t a_win_mono_ms(void) {
    static long long freq = 0;
    if (freq <= 0) {
        long long f = 0;
        if (!QueryPerformanceFrequency(&f) || f <= 0) return 0;
        freq = f;
    }
    long long c = 0;
    if (!QueryPerformanceCounter(&c) || c < 0) return 0;
    return (c / freq) * 1000 + (c % freq) * 1000 / freq;
}

void a_win_sleep_ms(int64_t ms) {
    while (ms > 0) {
        unsigned long chunk = ms > 60000 ? 60000UL : (unsigned long)ms;
        Sleep(chunk);
        ms -= (int64_t)chunk;
    }
}
#endif

