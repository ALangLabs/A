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
     * nibble, so every object needs 16-byte alignment. 64-bit libcs
     * guarantee that from malloc; 32-bit libcs (ARM, i386) only
     * guarantee 8, so allocate aligned explicitly. */
    void *p = NULL;
    if (posix_memalign(&p, 16, n ? n : 1) != 0 || !p)
        panicf("out of memory");
    return p;
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
aval mkint_full(int64_t i) {
    const int64_t lim = INT64_C(1) << 46;
    if (i >= -lim && i < lim) return a_mkint(i);
    AInt64 *p = (AInt64 *)xmalloc(sizeof(AInt64));
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

