#include "a_runtime_internal.h"

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

static AFrame *g_frames;
static size_t g_nframes;
static size_t g_capframes;

void a_frame_push(const char *name, const char *file, int line) {
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
}

void a_frame_pop(void) {
    if (g_nframes) --g_nframes;
}

static void print_traceback(void) {
    if (!g_nframes) return;
    fputs("Traceback (most recent call last):\n", stderr);
    for (size_t i = 0; i < g_nframes; ++i) {
        const AFrame *f = &g_frames[i];
        const char *file = f->file && *f->file ? f->file : "?";
        const char *name = f->name && *f->name ? f->name : "?";
        if (f->line > 0)
            fprintf(stderr, "  File \"%s\", line %d, in %s\n", file, f->line, name);
        else
            fprintf(stderr, "  File \"%s\", in %s\n", file, name);
    }
}

void panicf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    print_traceback();
    exit(2);
}

void *xmalloc(size_t n) {
    /* Heap objects are NaN-boxed pointers distinguished by a zero low
     * nibble, so every object needs 16-byte alignment. 32-bit libcs
     * (ARM, i386, 32-bit Windows) only guarantee 8, so allocate aligned
     * explicitly; on those targets a_runtime_internal.h maps malloc/
     * realloc/free onto the _aligned_* CRT entry points so the pairing
     * stays consistent. */
#if defined(_WIN32)
    /* Win64 CRT malloc returns 16-byte-aligned blocks; on 32-bit Windows
       the malloc macro in a_runtime_internal.h already routes this call
       to _aligned_malloc. */
    void *p = malloc(n ? n : 1);
    if (!p) panicf("out of memory");
    return p;
#else
    void *p = NULL;
    if (posix_memalign(&p, 16, n ? n : 1) != 0 || !p)
        panicf("out of memory");
    return p;
#endif
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) panicf("out of memory");
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
#define INT_BOX_CHUNK (1u << 16)
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
    if (g_intbox_left < INT_BOX_STEP) {
        g_intbox_cursor = (char *)xmalloc(INT_BOX_CHUNK);
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

/* Minimal kernel32 prototypes, declared directly so runtime TUs do not
   need windows.h. FILETIME is passed as an opaque 8-byte buffer and
   LARGE_INTEGER as int64_t; both are layout-compatible. WINAPI is
   __stdcall on 32-bit Windows and empty on Win64. */
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
#define A_WINAPI
#else
#define A_WINAPI __stdcall
#endif
extern void A_WINAPI GetSystemTimeAsFileTime(void *lpSystemTimeAsFileTime);
extern int A_WINAPI QueryPerformanceCounter(long long *lpPerformanceCount);
extern int A_WINAPI QueryPerformanceFrequency(long long *lpPerformanceFrequency);
extern void A_WINAPI Sleep(unsigned long dwMilliseconds);

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

