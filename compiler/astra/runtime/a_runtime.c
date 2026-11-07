/* Astra A runtime: NaN-boxed values, objects, operators, methods, builtins.
 *
 * Plain C99 + libc only.  Generated code sees only aval words and the public
 * API in a_runtime.h; all object layouts below are private to this file.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "a_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *type_name_cstr(aval v);

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

/* ------------------------------------------------------------------ */
/* object model                                                        */
/* ------------------------------------------------------------------ */

typedef struct AObj {
    uint32_t type;
    uint32_t len;
    uint64_t cap;
} AObj;

enum {
    T_STR = 1, T_LIST, T_TUPLE, T_DICT, T_SET, T_RANGE, T_ITER,
    T_FNON, T_ENV, T_RESULT, T_INT64, T_CHAR, T_VIEW, T_ARENA
};

typedef struct { AObj h; char data[]; } AStr;
typedef struct { AObj h; aval *items; } AArr;
typedef struct { aval k, v; } APair;
typedef struct { AObj h; APair *pairs; } ADict;
typedef struct { AObj h; int64_t lo, hi, step; } ARange;
typedef struct {
    AObj h;
    uint64_t entry;
    aval env;
    uint64_t nparams;
    uint32_t flags;
    char *name;
} AFnon;
typedef struct { AObj h; uint64_t n; aval slots[]; } AEnv;
typedef struct { AObj h; uint32_t ok; uint32_t is_option; aval value; } AResult;
typedef struct { AObj h; int64_t v; } AInt64;
typedef struct { AObj h; aval parent; int64_t off; int64_t len; int is_tuple; } AView;
typedef struct {
    AObj h;
    aval src;
    uint64_t idx;
    int64_t cur, end, step;
} AIter;
typedef struct { uint64_t off, size; uint32_t free; } AAlloc;
typedef struct {
    AObj h;
    uint64_t cap, bump, live;
    uint64_t nalloc, capalloc;
    AAlloc *allocs;
} AArena;

#define FNON_BUILTIN 1u
#define NPARAMS_UNKNOWN UINT64_MAX

enum {
    K_NONE, K_BOOL, K_INT, K_FLOAT, K_STR, K_LIST, K_TUPLE, K_DICT,
    K_SET, K_RANGE, K_ITER, K_FNON, K_ENV, K_RESULT, K_OTHER, K_CHAR, K_VIEW
};

static AArr *view_root(aval v, int64_t *off);
static aval view_new(aval parent, int64_t off, int64_t len, int is_tuple);
static aval list_from_vec(const aval *p, uint64_t n, int tuple);
static void sort_arr(aval *a, uint64_t n);

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

static void panicf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) panicf("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) panicf("out of memory");
    return q;
}

static char *xstrndup(const char *s, size_t n) {
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *xstrdup(const char *s) {
    return xstrndup(s ? s : "", strlen(s ? s : ""));
}

/* ASTRA emits a __fixxfdi libcall for (int64_t)long double conversions.
 * libgcc is not linked into A programs, so provide it here.
 * x86-64 long double is 80-bit extended in a 16-byte slot:
 * bytes 0-7 mantissa (explicit integer bit), bytes 8-9 sign/exponent. */
int64_t __fixxfdi(long double x) {
    union { long double ld; unsigned char b[16]; } v;
    uint64_t m, r = 0;
    uint16_t se;
    int sign, e, sh;
    v.ld = x;
    m = ((uint64_t *)v.b)[0];
    se = ((uint16_t *)v.b)[4];
    if ((se & 0x7FFF) == 0) return 0; /* zero or unsupported denormal */
    sign = (se >> 15) & 1;
    e = (int)(se & 0x7FFF) - 16383 - 63; /* power of two for mantissa lsb */
    sh = e >= 0 ? e : -e;
    if (sh < 64) {
        if (e >= 0) {
            if (m <= (UINT64_MAX >> e))
                r = m << e;
            else
                r = UINT64_MAX;
        } else {
            r = m >> sh;
        }
    } else if (e < 0) {
        r = 0;
    } else {
        r = UINT64_MAX;
    }
    if (r > (uint64_t)INT64_MAX + 1ULL) r = (uint64_t)INT64_MAX + 1ULL;
    if (sign) {
        if (r == (uint64_t)INT64_MAX + 1ULL) return INT64_MIN;
        return -(int64_t)r;
    }
    return (int64_t)r;
}

static aval mkobj(void *p) {
    uintptr_t u = (uintptr_t)p;
    if ((u & 0xFULL) != 0 || u >= (UINT64_C(1) << 48))
        panicf("object pointer out of NaN-boxed range");
    return A_TAG | (uint64_t)u;
}

static AObj *obj_of(aval v) {
    if (!a_is_ptr(v)) return NULL;
    return (AObj *)A_PAYLOAD(v);
}

static int is_int_val(aval v) {
    if (a_is_int(v)) return 1;
    AObj *o = obj_of(v);
    return o && o->type == T_INT64;
}

static int64_t int_val_full(aval v) {
    if (a_is_int(v)) return a_int_val(v);
    AObj *o = obj_of(v);
    if (o && o->type == T_INT64) return ((AInt64 *)o)->v;
    panicf("internal: not an integer");
    return 0;
}

static int is_num(aval v) { return is_int_val(v) || a_is_double(v); }

static double num_to_double(aval v) {
    if (is_int_val(v)) return (double)int_val_full(v);
    if (a_is_double(v)) return a_double_val(v);
    panicf("internal: not a number");
    return 0.0;
}

static int64_t expect_int64(aval v, const char *what) {
    if (!is_int_val(v))
        panicf("%s must be an integer, not '%s'", what, type_name_cstr(v));
    return int_val_full(v);
}

/* Inline ints cover [-2^46, 2^46); wider values box as T_INT64. */
static aval mkint_full(int64_t i) {
    const int64_t lim = INT64_C(1) << 46;
    if (i >= -lim && i < lim) return a_mkint(i);
    AInt64 *p = (AInt64 *)xmalloc(sizeof(AInt64));
    p->h.type = T_INT64;
    p->h.len = 0;
    p->h.cap = 0;
    p->v = i;
    return mkobj(&p->h);
}

static aval mkdouble(double d) {
    if (isnan(d)) {
        union { uint64_t u; double d; } c;
        c.u = UINT64_C(0x7FF8000000000000);
        return c.u;
    }
    return a_mkdouble(d);
}

static aval bool_aval(int b) { return b ? A_TRUE : A_FALSE; }

/* ------------------------------------------------------------------ */
/* growable buffers                                                    */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->p = (char *)xrealloc(b->p, nc);
    b->cap = nc;
}

static void buf_addn(Buf *b, const char *s, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }

static void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }

static void buf_addi64(Buf *b, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n > 0) buf_addn(b, tmp, (size_t)n);
}

static aval buf_take(Buf *b) {
    aval s = a_str_new(b->p ? b->p : "", (uint64_t)b->len);
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

static AStr *str_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_STR && o->type != T_CHAR))
        panicf("%s must be str, not '%s'", what, type_name_cstr(v));
    return (AStr *)o;
}

aval a_str_new(const char *p, uint64_t len) {
    if (len && !p) panicf("null string data");
    AStr *s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
    s->h.type = T_STR;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    if (len) memcpy(s->data, p, (size_t)len);
    s->data[len] = '\0';
    return mkobj(&s->h);
}

aval a_char_new(const char *p, uint64_t len) {
    if (len && !p) panicf("null string data");
    AStr *s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
    s->h.type = T_CHAR;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    if (len) memcpy(s->data, p, (size_t)len);
    s->data[len] = '\0';
    return mkobj(&s->h);
}

static const char *str_data(aval v, uint64_t *len) {
    AStr *s = str_of(v, "value");
    if (len) *len = s->h.len;
    return s->data;
}

/* UTF-8 helpers: strings are indexed by code point, not byte. */
static int u8_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
static uint64_t u8_count(const char *p, uint64_t n) {
    uint64_t c = 0;
    uint64_t i = 0;
    while (i < n) {
        int l = u8_len((unsigned char)p[i]);
        if (i + (uint64_t)l > n) l = 1;
        i += (uint64_t)l;
        ++c;
    }
    return c;
}
/* Map code-point index to byte offset; returns n for out-of-range (caller
   clamps the same way slice_bound does for lists). */
static uint64_t cp_to_byte(const char *p, uint64_t n, int64_t cp) {
    uint64_t c = 0;
    uint64_t i = 0;
    while (i < n && c < (uint64_t)cp) {
        int l = u8_len((unsigned char)p[i]);
        if (i + (uint64_t)l > n) l = 1;
        i += (uint64_t)l;
        ++c;
    }
    return i;
}

static int str_eq(aval a, aval b) {
    uint64_t la, lb;
    const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
    return la == lb && (la == 0 || memcmp(pa, pb, (size_t)la) == 0);
}

static const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn,
                              size_t from, size_t *pos) {
    if (nn == 0) {
        *pos = from <= hn ? from : hn;
        return h + *pos;
    }
    if (from > hn || nn > hn - from) return NULL;
    for (size_t i = from; i + nn <= hn; ++i) {
        if (memcmp(h + i, n, nn) == 0) {
            *pos = i;
            return h + i;
        }
    }
    return NULL;
}

static int64_t str_find(aval hay, aval needle, int64_t start, int reverse) {
    uint64_t hn, nn;
    const char *h = str_data(hay, &hn), *n = str_data(needle, &nn);
    uint64_t total_cp = u8_count(h, hn);
    int64_t st = start;
    if (st < 0) st += (int64_t)total_cp;
    if (st < 0) st = 0;
    if (st > (int64_t)total_cp) return -1;
    if (nn == 0) return st;
    uint64_t st_byte = cp_to_byte(h, hn, st);
    if (!reverse) {
        size_t pos = 0;
        const char *p = find_bytes(h, (size_t)hn, n, (size_t)nn, st_byte, &pos);
        if (!p) return -1;
        return (int64_t)u8_count(h, pos);
    }
    for (int64_t i = (int64_t)hn - (int64_t)nn; i >= (int64_t)st_byte; --i) {
        if (memcmp(h + i, n, (size_t)nn) == 0) return (int64_t)u8_count(h, (uint64_t)i);
    }
    return -1;
}

static aval str_repeat(aval s, int64_t count) {
    uint64_t n;
    const char *p = str_data(s, &n);
    if (count <= 0 || n == 0) return a_str_new("", 0);
    if ((uint64_t)count > UINT64_MAX / n)
        panicf("repeated string is too large");
    Buf b;
    buf_init(&b);
    buf_reserve(&b, (size_t)(n * (uint64_t)count));
    for (int64_t i = 0; i < count; ++i) buf_addn(&b, p, (size_t)n);
    return buf_take(&b);
}

/* ------------------------------------------------------------------ */
/* arrays / dicts / sets                                               */
/* ------------------------------------------------------------------ */

static AArr *arr_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE && o->type != T_SET))
        panicf("%s must be a list, tuple, or set", what);
    return (AArr *)o;
}

static void arr_reserve(AArr *a, uint64_t need) {
    if (need <= a->h.cap) return;
    uint64_t nc = a->h.cap ? a->h.cap : 4;
    while (nc < need) nc *= 2;
    a->items = (aval *)xrealloc(a->items, (size_t)nc * sizeof(aval));
    a->h.cap = nc;
}

static void arr_push(AArr *a, aval v) {
    if (a->h.len == UINT32_MAX) panicf("array too large");
    arr_reserve(a, (uint64_t)a->h.len + 1);
    a->items[a->h.len++] = v;
}

static ADict *dict_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_DICT)
        panicf("%s must be dict, not '%s'", what, type_name_cstr(v));
    return (ADict *)o;
}

static void dict_reserve(ADict *d, uint64_t need) {
    if (need <= d->h.cap) return;
    uint64_t nc = d->h.cap ? d->h.cap : 4;
    while (nc < need) nc *= 2;
    d->pairs = (APair *)xrealloc(d->pairs, (size_t)nc * sizeof(APair));
    d->h.cap = nc;
}

static int values_equal(aval a, aval b);

static int64_t dict_find(ADict *d, aval k) {
    for (uint64_t i = 0; i < d->h.len; ++i)
        if (values_equal(d->pairs[i].k, k)) return (int64_t)i;
    return -1;
}

static void dict_put(ADict *d, aval k, aval v) {
    int64_t i = dict_find(d, k);
    if (i >= 0) {
        d->pairs[i].v = v;
        return;
    }
    if (d->h.len == UINT32_MAX) panicf("dict too large");
    dict_reserve(d, (uint64_t)d->h.len + 1);
    d->pairs[d->h.len].k = k;
    d->pairs[d->h.len].v = v;
    d->h.len++;
}

static int set_contains(AArr *s, aval v) {
    for (uint64_t i = 0; i < s->h.len; ++i)
        if (values_equal(s->items[i], v)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* type names / formatting                                             */
/* ------------------------------------------------------------------ */

static int kind_of(aval v) {
    if (a_is_none(v)) return K_NONE;
    if (v == A_TRUE || v == A_FALSE) return K_BOOL;
    if (is_int_val(v)) return K_INT;
    if (a_is_double(v)) return K_FLOAT;
    AObj *o = obj_of(v);
    if (!o) return K_OTHER;
    switch (o->type) {
        case T_STR: return K_STR;
        case T_LIST: return K_LIST;
        case T_TUPLE: return K_TUPLE;
        case T_DICT: return K_DICT;
        case T_SET: return K_SET;
        case T_RANGE: return K_RANGE;
        case T_ITER: return K_ITER;
        case T_FNON: return K_FNON;
        case T_ENV: return K_ENV;
        case T_RESULT: return K_RESULT;
        case T_CHAR: return K_CHAR;
        case T_VIEW: return K_VIEW;
        default: return K_OTHER;
    }
}

static const char *type_name_cstr(aval v) {
    switch (kind_of(v)) {
        case K_NONE: return "NoneType";
        case K_BOOL: return "bool";
        case K_INT: return "int";
        case K_FLOAT: return "float";
        case K_STR: return "str";
        case K_LIST: return "list";
        case K_TUPLE: return "tuple";
        case K_DICT: return "dict";
        case K_SET: return "set";
        case K_RANGE: return "range";
        case K_ITER: return "iterator";
        case K_FNON: return "function";
        case K_ENV: return "environment";
        case K_RESULT: return "result";
        case K_CHAR: return "char";
        case K_VIEW: return "view";
        default: return "object";
    }
}

aval a_type_name(aval v) {
    const char *s = type_name_cstr(v);
    return a_str_new(s, strlen(s));
}

int a_obj_is_instance(aval v, const char *type_name) {
    if (!type_name) return 0;
    return strcmp(type_name_cstr(v), type_name) == 0;
}

static void format_into(Buf *b, aval v, int quote);

static void format_double_buf(Buf *b, double d) {
    if (isnan(d)) { buf_adds(b, "nan"); return; }
    if (isinf(d)) { buf_adds(b, d < 0 ? "-inf" : "inf"); return; }
    if (d == 0.0) { buf_adds(b, signbit(d) ? "-0.0" : "0.0"); return; }
    char tmp[64];
    for (int prec = 1; prec <= 17; ++prec) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, d);
        if (strtod(tmp, NULL) == d) break;
    }
    /* Avoid scientific notation for ordinary magnitudes (Python-style);
       expand to plain decimal that still round-trips. */
    if (strchr(tmp, 'e') || strchr(tmp, 'E')) {
        double ad = d < 0.0 ? -d : d;
        if (ad >= 1e-4 && ad < 1e16) {
            for (int prec = 1; prec <= 25; ++prec) {
                snprintf(tmp, sizeof(tmp), "%.*f", prec, d);
                if (strtod(tmp, NULL) == d) break;
            }
        }
    }
    buf_adds(b, tmp);
    if (strchr(tmp, '.') == NULL && strchr(tmp, 'e') == NULL &&
        strchr(tmp, 'E') == NULL)
        buf_adds(b, ".0");
}

static void format_quoted(Buf *b, const char *p, size_t n) {
    buf_addc(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\t': buf_adds(b, "\\t"); break;
            case '\r': buf_adds(b, "\\r"); break;
            default: buf_addc(b, (char)c); break;
        }
    }
    buf_addc(b, '"');
}

static void format_into(Buf *b, aval v, int quote) {
    switch (kind_of(v)) {
        case K_NONE: buf_adds(b, "None"); return;
        case K_BOOL: buf_adds(b, v == A_TRUE ? "true" : "false"); return;
        case K_INT: buf_addi64(b, int_val_full(v)); return;
        case K_FLOAT: format_double_buf(b, a_double_val(v)); return;
        case K_STR: {
            uint64_t n;
            const char *p = str_data(v, &n);
            if (quote) format_quoted(b, p, (size_t)n);
            else buf_addn(b, p, (size_t)n);
            return;
        }
        case K_CHAR: {
            /* Chars display unquoted even inside containers (docs:
               s.chars() yields char values, distinct from strings). */
            uint64_t n;
            const char *p = str_data(v, &n);
            buf_addn(b, p, (size_t)n);
            return;
        }
        case K_VIEW: {
            AView *vw = (AView *)obj_of(v);
            AArr *a = (AArr *)obj_of(vw->parent);
            buf_addc(b, vw->is_tuple ? '(' : '[');
            for (int64_t i = 0; i < vw->len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, a->items[(size_t)(vw->off + i)], 1);
            }
            buf_addc(b, vw->is_tuple ? ')' : ']');
            return;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(v);
            buf_addc(b, '[');
            if (kind_of(v) == K_TUPLE) b->p[b->len - 1] = '(';
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, a->items[i], 1);
            }
            buf_addc(b, kind_of(v) == K_TUPLE ? ')' : ']');
            return;
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(v);
            buf_addc(b, '{');
            for (uint64_t i = 0; i < d->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, d->pairs[i].k, 1);
                buf_adds(b, ": ");
                format_into(b, d->pairs[i].v, 1);
            }
            buf_addc(b, '}');
            return;
        }
        case K_SET: {
            AArr *a = (AArr *)obj_of(v);
            if (a->h.len == 0) { buf_adds(b, "set()"); return; }
            buf_addc(b, '{');
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, a->items[i], 1);
            }
            buf_addc(b, '}');
            return;
        }
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            buf_addi64(b, r->lo);
            buf_adds(b, "..");
            buf_addi64(b, r->hi);
            if (r->step != 1) {
                buf_adds(b, "..");
                buf_addi64(b, r->step);
            }
            return;
        }
        case K_FNON: {
            AFnon *f = (AFnon *)obj_of(v);
            buf_adds(b, "<function");
            if (f->name && f->name[0]) {
                buf_addc(b, ' ');
                buf_adds(b, f->name);
            }
            buf_addc(b, '>');
            return;
        }
        case K_RESULT: {
            AResult *r = (AResult *)obj_of(v);
            buf_adds(b, r->is_option ? "Some(" : (r->ok ? "Ok(" : "Err("));
            format_into(b, r->value, 1);
            buf_addc(b, ')');
            return;
        }
        default:
            buf_adds(b, "<object>");
            return;
    }
}

static aval format_aval(aval v, int quote) {
    Buf b;
    buf_init(&b);
    format_into(&b, v, quote);
    return buf_take(&b);
}

aval a_str(aval v) { return format_aval(v, 0); }
aval a_repr(aval v) { return format_aval(v, 1); }

/* ------------------------------------------------------------------ */
/* truthiness / conversions                                            */
/* ------------------------------------------------------------------ */

static int truthy(aval v) {
    switch (kind_of(v)) {
        case K_NONE: return 0;
        case K_BOOL: return v == A_TRUE;
        case K_INT: return int_val_full(v) != 0;
        case K_FLOAT: return a_double_val(v) != 0.0;
        case K_STR: return ((AStr *)obj_of(v))->h.len != 0;
        case K_CHAR: return ((AStr *)obj_of(v))->h.len != 0;
        case K_VIEW: return ((AView *)obj_of(v))->len != 0;
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_DICT: return ((AArr *)obj_of(v))->h.len != 0;
        default: return 1;
    }
}

aval a_truthy_raw_slow(aval v) { return (aval)(truthy(v) ? 1 : 0); }
aval a_not(aval v) { return bool_aval(!truthy(v)); }
aval a_bool(aval v) { return bool_aval(truthy(v)); }

aval a_neg_slow(aval v) {
    if (is_int_val(v)) {
        int64_t i = int_val_full(v);
        if (i == INT64_MIN) return mkdouble(-(double)INT64_MIN);
        return mkint_full(-i);
    }
    if (a_is_double(v)) return mkdouble(-a_double_val(v));
    panicf("bad operand type for unary -: '%s'", type_name_cstr(v));
    return A_NONE;
}

aval a_bnot_slow(aval v) {
    if (!is_int_val(v))
        panicf("bad operand type for unary ~: '%s'", type_name_cstr(v));
    return mkint_full(~int_val_full(v));
}

static aval parse_int_str(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) { ++s; --n; }
    while (n && isspace((unsigned char)s[n - 1])) --n;
    if (n == 0) panicf("invalid literal for int()");
    char *tmp = xstrndup(s, n);
    char *end = NULL;
    errno = 0;
    long long x = strtoll(tmp, &end, 10);
    if (end == tmp) {
        double d = strtod(tmp, &end);
        if (end == tmp || *end != '\0') {
            free(tmp);
            panicf("invalid literal for int()");
        }
        if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0) {
            free(tmp);
            panicf("int() argument out of range");
        }
        free(tmp);
        return mkint_full((int64_t)d);
    }
    if (*end != '\0') {
        free(tmp);
        panicf("invalid literal for int()");
    }
    if (errno == ERANGE) {
        free(tmp);
        panicf("int() argument out of range");
    }
    free(tmp);
    return mkint_full((int64_t)x);
}

aval a_int(aval v) {
    if (is_int_val(v)) return v;
    if (v == A_TRUE) return mkint_full(1);
    if (v == A_FALSE) return mkint_full(0);
    if (a_is_double(v)) {
        double d = trunc(a_double_val(v));
        if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
            panicf("cannot convert float to int");
        return mkint_full((int64_t)d);
    }
    if (kind_of(v) == K_STR) {
        uint64_t n;
        const char *p = str_data(v, &n);
        return parse_int_str(p, (size_t)n);
    }
    panicf("cannot convert '%s' to int", type_name_cstr(v));
    return A_NONE;
}

aval a_float(aval v) {
    if (is_int_val(v)) return mkdouble((double)int_val_full(v));
    if (a_is_double(v)) return v;
    if (v == A_TRUE) return mkdouble(1.0);
    if (v == A_FALSE) return mkdouble(0.0);
    if (kind_of(v) == K_STR) {
        uint64_t n;
        const char *p = str_data(v, &n);
        char *tmp = xstrndup(p, (size_t)n);
        char *q = tmp;
        while (isspace((unsigned char)*q)) ++q;
        char *end = NULL;
        double d = strtod(q, &end);
        if (end == q) {
            free(tmp);
            panicf("could not convert string to float");
        }
        while (end && isspace((unsigned char)*end)) ++end;
        if (!end || *end != '\0') {
            free(tmp);
            panicf("could not convert string to float");
        }
        free(tmp);
        return mkdouble(d);
    }
    panicf("cannot convert '%s' to float", type_name_cstr(v));
    return A_NONE;
}

aval a_len(aval v) {
    switch (kind_of(v)) {
        case K_STR: {
            AStr *s = (AStr *)obj_of(v);
            return mkint_full(u8_count(s->data, s->h.len));
        }
        case K_CHAR: {
            AStr *s = (AStr *)obj_of(v);
            return mkint_full(u8_count(s->data, s->h.len));
        }
        case K_VIEW: return mkint_full(((AView *)obj_of(v))->len);
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_DICT: return mkint_full(((AArr *)obj_of(v))->h.len);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            long double n;
            if (r->step > 0) {
                if ((long double)r->hi <= (long double)r->lo) n = 0.0L;
                else n = floorl(((long double)r->hi - (long double)r->lo - 1.0L) /
                               (long double)r->step) + 1.0L;
            } else {
                if ((long double)r->hi >= (long double)r->lo) n = 0.0L;
                else n = floorl(((long double)r->lo - (long double)r->hi - 1.0L) /
                               (-(long double)r->step)) + 1.0L;
            }
            if (n <= (long double)INT64_MAX) return mkint_full((int64_t)n);
            return mkdouble((double)n);
        }
        default:
            panicf("object of type '%s' has no len()", type_name_cstr(v));
            return A_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* equality / ordering                                                 */
/* ------------------------------------------------------------------ */

static int values_equal(aval a, aval b) {
    int ka = kind_of(a), kb = kind_of(b);
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b))
            return int_val_full(a) == int_val_full(b);
        return num_to_double(a) == num_to_double(b);
    }
    if (a == b) return 1;
    /* Views compare as the underlying slice (view == list if elements match). */
    if (ka == K_VIEW || kb == K_VIEW) {
        if (ka != K_VIEW && ka != K_LIST && ka != K_TUPLE) return 0;
        if (kb != K_VIEW && kb != K_LIST && kb != K_TUPLE) return 0;
        int64_t la = ka == K_VIEW ? ((AView *)obj_of(a))->len : (int64_t)((AArr *)obj_of(a))->h.len;
        int64_t lb = kb == K_VIEW ? ((AView *)obj_of(b))->len : (int64_t)((AArr *)obj_of(b))->h.len;
        if (la != lb) return 0;
        for (int64_t i = 0; i < la; ++i) {
            aval xa = ka == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(a))->parent))->items[(size_t)(((AView *)obj_of(a))->off + i)]
                                   : ((AArr *)obj_of(a))->items[(size_t)i];
            aval xb = kb == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(b))->parent))->items[(size_t)(((AView *)obj_of(b))->off + i)]
                                   : ((AArr *)obj_of(b))->items[(size_t)i];
            if (!values_equal(xa, xb)) return 0;
        }
        return 1;
    }
    if (ka != kb) return 0;
    switch (ka) {
        case K_BOOL: return 0;
        case K_STR: return str_eq(a, b);
        case K_CHAR: return str_eq(a, b);
        case K_LIST:
        case K_TUPLE: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i)
                if (!values_equal(x->items[i], y->items[i])) return 0;
            return 1;
        }
        case K_DICT: {
            ADict *x = (ADict *)obj_of(a), *y = (ADict *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j) {
                    if (values_equal(x->pairs[i].k, y->pairs[j].k) &&
                        values_equal(x->pairs[i].v, y->pairs[j].v)) {
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            }
            return 1;
        }
        case K_SET: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j)
                    if (values_equal(x->items[i], y->items[j])) { found = 1; break; }
                if (!found) return 0;
            }
            return 1;
        }
        case K_RANGE: {
            ARange *x = (ARange *)obj_of(a), *y = (ARange *)obj_of(b);
            return x->lo == y->lo && x->hi == y->hi && x->step == y->step;
        }
        case K_RESULT: {
            AResult *x = (AResult *)obj_of(a), *y = (AResult *)obj_of(b);
            return x->ok == y->ok && x->is_option == y->is_option && values_equal(x->value, y->value);
        }
        default: return 0;
    }
}

aval a_eq_slow(aval l, aval r) { return bool_aval(values_equal(l, r)); }
aval a_ne_slow(aval l, aval r) { return bool_aval(!values_equal(l, r)); }

static int cmp_ordered(aval a, aval b, const char *op) {
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b)) {
            int64_t x = int_val_full(a), y = int_val_full(b);
            return x < y ? -1 : x > y ? 1 : 0;
        }
        double x = num_to_double(a), y = num_to_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind_of(a) == K_STR && kind_of(b) == K_STR) {
        uint64_t la, lb;
        const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
        size_t n = la < lb ? (size_t)la : (size_t)lb;
        int c = n ? memcmp(pa, pb, n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (kind_of(a) == K_BOOL && kind_of(b) == K_BOOL) {
        int x = a == A_TRUE, y = b == A_TRUE;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    panicf("unsupported operand type(s) for %s: '%s' and '%s'", op,
           type_name_cstr(a), type_name_cstr(b));
    return 0;
}

aval a_lt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<") < 0); }
aval a_le_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<=") <= 0); }
aval a_gt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">") > 0); }
aval a_ge_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">=") >= 0); }

/* ------------------------------------------------------------------ */
/* integer overflow helpers                                            */
/* ------------------------------------------------------------------ */

static int iadd_of(int64_t a, int64_t b, int64_t *r) {
    if (b == INT64_MIN) {
        if (a < 0) return 1;
        *r = (int64_t)((uint64_t)a + (uint64_t)b);
        return 0;
    }
    if (b > 0) {
        if (a > INT64_MAX - b) return 1;
    } else if (a < INT64_MIN - b) return 1;
    *r = a + b;
    return 0;
}

static int isub_of(int64_t a, int64_t b, int64_t *r) {
    if (b == INT64_MIN) {
        if (a >= 0) return 1;
        *r = (int64_t)((uint64_t)a - (uint64_t)b);
        return 0;
    }
    if (b > 0) {
        if (a < INT64_MIN + b) return 1;
    } else if (a > INT64_MAX + b) return 1;
    *r = a - b;
    return 0;
}

static uint64_t iabs_u(int64_t x) {
    return x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
}

static int imul_of(int64_t a, int64_t b, int64_t *r) {
    if (a == 0 || b == 0) { *r = 0; return 0; }
    uint64_t ua = iabs_u(a), ub = iabs_u(b);
    int neg = (a < 0) != (b < 0);
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (ua > limit / ub) return 1;
    uint64_t m = ua * ub;
    if (neg) {
        *r = (m == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)m;
    } else {
        *r = (int64_t)m;
    }
    return 0;
}

static void panic_binop(const char *op, aval l, aval r) {
    panicf("unsupported operand type(s) for %s: '%s' and '%s'", op,
           type_name_cstr(l), type_name_cstr(r));
}

static aval arith_int_double(aval l, aval r, char op) {
    double a = num_to_double(l), b = num_to_double(r), x = 0.0;
    switch (op) {
        case '+': x = a + b; break;
        case '-': x = a - b; break;
        case '*': x = a * b; break;
        case '/':
            if (b == 0.0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
            x = a / b;
            break;
        case '%': {
            if (b == 0.0) panicf("Remainder by zero.\n  = help: Check the divisor before dividing.");
            double m = fmod(a, b);
            if (m != 0.0 && ((m > 0.0) != (b > 0.0))) m += b;
            x = m;
            break;
        }
        case '^': x = pow(a, b); break;
        default: panicf("internal arithmetic op");
    }
    return mkdouble(x);
}

static aval arith_int_int(aval l, aval r, char op) {
    int64_t a = int_val_full(l), b = int_val_full(r), res;
    switch (op) {
        case '+':
            if (iadd_of(a, b, &res)) return mkdouble((double)a + (double)b);
            return mkint_full(res);
        case '-':
            if (isub_of(a, b, &res)) return mkdouble((double)a - (double)b);
            return mkint_full(res);
        case '*':
            if (imul_of(a, b, &res)) return mkdouble((double)a * (double)b);
            return mkint_full(res);
        case '/':
            if (b == 0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
            if (a == INT64_MIN && b == -1) return mkdouble(9223372036854775808.0);
            return mkint_full(a / b);
        case '%':
            if (b == 0) panicf("Remainder by zero.\n  = help: Check the divisor before dividing.");
            if (b == -1) return mkint_full(0);
            return mkint_full(a % b);
        case '^':
            if (b < 0) return mkdouble(pow((double)a, (double)b));
            if (a == 0 && b == 0) return mkint_full(1);
            if (a == 1 || a == -1) {
                if (a == 1) return mkint_full(1);
                return mkint_full((b & 1) ? -1 : 1);
            }
            {
                int64_t base = a, e = b, power = 1;
                while (e > 0) {
                    if (e & 1) {
                        if (imul_of(power, base, &power))
                            return mkdouble(pow((double)a, (double)b));
                    }
                    e >>= 1;
                    if (e > 0 && imul_of(base, base, &base))
                        return mkdouble(pow((double)a, (double)b));
                }
                return mkint_full(power);
            }
        default: panicf("internal arithmetic op");
    }
    return A_NONE;
}

static aval concat_str(aval strv, aval other, int str_left) {
    uint64_t sn;
    const char *sp = str_data(strv, &sn);
    Buf b;
    buf_init(&b);
    if (str_left) {
        buf_addn(&b, sp, (size_t)sn);
        format_into(&b, other, 0);
    } else {
        format_into(&b, other, 0);
        buf_addn(&b, sp, (size_t)sn);
    }
    return buf_take(&b);
}

static aval concat_arr(aval l, aval r) {
    AArr *a = (AArr *)obj_of(l), *b = (AArr *)obj_of(r);
    int is_tuple = a->h.type == T_TUPLE;
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = is_tuple ? T_TUPLE : T_LIST;
    out->h.len = 0;
    out->h.cap = (uint64_t)a->h.len + b->h.len;
    out->items = (aval *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(aval));
    memcpy(out->items, a->items, (size_t)a->h.len * sizeof(aval));
    memcpy(out->items + a->h.len, b->items, (size_t)b->h.len * sizeof(aval));
    out->h.len = a->h.len + b->h.len;
    return mkobj(&out->h);
}

static aval binary_arith(aval l, aval r, char op, const char *opname) {
    int li = is_int_val(l), ri = is_int_val(r);
    int ld = a_is_double(l), rd = a_is_double(r);
    if ((li && rd) || (ld && ri) || (ld && rd))
        return arith_int_double(l, r, op);
    if (op == '+' ) {
        if (kind_of(l) == K_STR) return concat_str(l, r, 1);
        if (kind_of(r) == K_STR) return concat_str(r, l, 0);
        if (kind_of(l) == K_LIST && kind_of(r) == K_LIST) return concat_arr(l, r);
        if (kind_of(l) == K_TUPLE && kind_of(r) == K_TUPLE) return concat_arr(l, r);
    }
    if (op == '*' ) {
        if (kind_of(l) == K_STR && ri) return str_repeat(l, int_val_full(r));
        if (kind_of(r) == K_STR && li) return str_repeat(r, int_val_full(l));
    }
    if (li && ri) return arith_int_int(l, r, op);
    panic_binop(opname, l, r);
    return A_NONE;
}

aval a_add_slow(aval l, aval r) { return binary_arith(l, r, '+', "+"); }
aval a_sub_slow(aval l, aval r) { return binary_arith(l, r, '-', "-"); }
aval a_mul_slow(aval l, aval r) { return binary_arith(l, r, '*', "*"); }
aval a_div_slow(aval l, aval r) { return binary_arith(l, r, '/', "/"); }
aval a_mod_slow(aval l, aval r) { return binary_arith(l, r, '%', "%"); }
aval a_pow(aval l, aval r) { return binary_arith(l, r, '^', "**"); }

/* ------------------------------------------------------------------ */
/* bitwise / shifts / membership                                       */
/* ------------------------------------------------------------------ */

aval a_band_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("&", l, r);
    return mkint_full(int_val_full(l) & int_val_full(r));
}
aval a_bor_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("|", l, r);
    return mkint_full(int_val_full(l) | int_val_full(r));
}
aval a_bxor_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("^", l, r);
    return mkint_full(int_val_full(l) ^ int_val_full(r));
}

static int64_t shift_amount(aval v) {
    int64_t s = expect_int64(v, "shift amount");
    if (s < 0 || s > 63)
        panicf("Shift amount %lld is out of range for integers.",
               (long long)s);
    return s;
}

aval a_shl_slow(aval l, aval r) {
    if (!is_int_val(l)) panic_binop("<<", l, r);
    int64_t a = int_val_full(l), s = shift_amount(r);
    return mkint_full((int64_t)((uint64_t)a << s));
}
aval a_shr_slow(aval l, aval r) {
    if (!is_int_val(l)) panic_binop(">>", l, r);
    int64_t a = int_val_full(l), s = shift_amount(r);
    return mkint_full(a >> s);
}

static int range_contains_raw(ARange *r, int64_t v) {
    if (r->step > 0) {
        if (v < r->lo || v >= r->hi) return 0;
    } else {
        if (v > r->lo || v <= r->hi) return 0;
    }
    long double diff = (long double)v - (long double)r->lo;
    return fabsl(fmodl(diff, (long double)r->step)) == 0.0L;
}

aval a_range_contains(aval r, aval v) {
    AObj *o = obj_of(r);
    if (!o || o->type != T_RANGE)
        panicf("range_contains() argument must be range");
    if (!is_int_val(v)) panicf("range membership requires an integer");
    return bool_aval(range_contains_raw((ARange *)o, int_val_full(v)));
}

aval a_in_range(aval v, aval lo, aval hi) {
    if (is_int_val(v) && is_int_val(lo) && is_int_val(hi)) {
        int64_t x = int_val_full(v), a = int_val_full(lo), b = int_val_full(hi);
        return bool_aval(a <= x && x < b);
    }
    if (is_num(v) && is_num(lo) && is_num(hi)) {
        double x = num_to_double(v), a = num_to_double(lo), b = num_to_double(hi);
        return bool_aval(a <= x && x < b);
    }
    panicf("unsupported operand type(s) for in: '%s' and 'range'",
           type_name_cstr(v));
    return A_NONE;
}

aval a_in_range_inclusive(aval v, aval lo, aval hi) {
    if (is_int_val(v) && is_int_val(lo) && is_int_val(hi)) {
        int64_t x = int_val_full(v), a = int_val_full(lo), b = int_val_full(hi);
        return bool_aval(a <= x && x <= b);
    }
    if (is_num(v) && is_num(lo) && is_num(hi)) {
        double x = num_to_double(v), a = num_to_double(lo), b = num_to_double(hi);
        return bool_aval(a <= x && x <= b);
    }
    panicf("unsupported operand type(s) for in: '%s' and 'range'",
           type_name_cstr(v));
    return A_NONE;
}

aval a_contains(aval cont, aval item) {
    switch (kind_of(cont)) {
        case K_STR: {
            if (kind_of(item) != K_STR)
                panicf("argument of type '%s' is not iterable", type_name_cstr(item));
            uint64_t hn, nn;
            const char *h = str_data(cont, &hn), *n = str_data(item, &nn);
            size_t pos;
            return bool_aval(find_bytes(h, (size_t)hn, n, (size_t)nn, 0, &pos) != NULL);
        }
        case K_LIST:
        case K_TUPLE:
        case K_SET: {
            AArr *a = (AArr *)obj_of(cont);
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (values_equal(a->items[i], item)) return A_TRUE;
            return A_FALSE;
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(cont);
            return bool_aval(dict_find(d, item) >= 0);
        }
        case K_RANGE: {
            if (!is_int_val(item))
                panicf("unsupported operand type(s) for in: '%s' and 'range'",
                       type_name_cstr(item));
            return bool_aval(range_contains_raw((ARange *)obj_of(cont),
                                                int_val_full(item)));
        }
        default:
            panicf("argument of type '%s' is not iterable", type_name_cstr(cont));
            return A_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* object constructors                                                */
/* ------------------------------------------------------------------ */

aval a_str_concat(aval l, aval r) {
    uint64_t ln, rn;
    const char *lp = str_data(l, &ln), *rp = str_data(r, &rn);
    if ((uint64_t)UINT32_MAX - ln < rn) panicf("string too large");
    AStr *s = (AStr *)xmalloc(sizeof(AStr) + (size_t)ln + rn + 1);
    s->h.type = T_STR;
    s->h.len = (uint32_t)(ln + rn);
    s->h.cap = s->h.len + 1;
    memcpy(s->data, lp, (size_t)ln);
    memcpy(s->data + ln, rp, (size_t)rn);
    s->data[ln + rn] = '\0';
    return mkobj(&s->h);
}

typedef struct { aval *p; size_t len, cap; } AVec;

static void avec_push(AVec *v, aval x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->p = (aval *)xrealloc(v->p, v->cap * sizeof(aval));
    }
    v->p[v->len++] = x;
}

aval a_str_join(uint64_t n, aval *items) {
    return a_str_joinsep(a_str_new("", 0), n, items);
}

aval a_str_joinsep(aval sep, uint64_t n, aval *items) {
    uint64_t sn;
    const char *sp = str_data(sep, &sn);
    Buf b;
    buf_init(&b);
    for (uint64_t i = 0; i < n; ++i) {
        if (i) buf_addn(&b, sp, (size_t)sn);
        uint64_t in;
        const char *ip = str_data(items[i], &in);
        buf_addn(&b, ip, (size_t)in);
    }
    return buf_take(&b);
}

static aval list_from_vec(const aval *p, uint64_t n, int tuple) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = tuple ? T_TUPLE : T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
    if (n) memcpy(a->items, p, (size_t)n * sizeof(aval));
    return mkobj(&a->h);
}

aval a_list_new(uint64_t n, aval *items) {
    return list_from_vec(items, n, 0);
}

aval a_tuple_new(uint64_t n, aval *items) {
    return list_from_vec(items, n, 1);
}

aval a_list_from_iter(aval it) {
    AObj *o = obj_of(it);
    AIter *iter;
    int own = 0;
    if (o && o->type == T_ITER) {
        iter = (AIter *)o;
    } else {
        it = a_iter_new(it);
        iter = (AIter *)obj_of(it);
        own = 1;
    }
    (void)own;
    AVec v = { NULL, 0, 0 };
    aval out;
    while (a_iter_next(mkobj(iter), &out)) avec_push(&v, out);
    aval r = list_from_vec(v.p, v.len, 0);
    free(v.p);
    return r;
}

aval a_dict_new(void) {
    ADict *d = (ADict *)xmalloc(sizeof(ADict));
    d->h.type = T_DICT;
    d->h.len = 0;
    d->h.cap = 0;
    d->pairs = NULL;
    return mkobj(&d->h);
}

aval a_dict_set(aval d, aval k, aval v) {
    dict_put(dict_of(d, "dict"), k, v);
    return A_NONE;
}

aval a_dict_get(aval d, aval k, aval dflt) {
    ADict *m = dict_of(d, "dict");
    int64_t i = dict_find(m, k);
    return i >= 0 ? m->pairs[i].v : dflt;
}

aval a_set_new(void) {
    AArr *s = (AArr *)xmalloc(sizeof(AArr));
    s->h.type = T_SET;
    s->h.len = 0;
    s->h.cap = 0;
    s->items = NULL;
    return mkobj(&s->h);
}

aval a_set_add(aval s, aval v) {
    AArr *set = (AArr *)obj_of(s);
    if (!set || set->h.type != T_SET)
        panicf("set_add() argument must be set");
    if (!set_contains(set, v)) arr_push(set, v);
    return A_NONE;
}

aval a_range_new(aval lo, aval hi, aval step) {
    int64_t a = expect_int64(lo, "range() argument"),
            b = expect_int64(hi, "range() argument"),
            c = expect_int64(step, "range() step");
    if (c == 0) panicf("range() step must be positive, found 0.");
    ARange *r = (ARange *)xmalloc(sizeof(ARange));
    r->h.type = T_RANGE;
    r->h.len = 3;
    r->h.cap = 0;
    r->lo = a;
    r->hi = b;
    r->step = c;
    return mkobj(&r->h);
}

/* ------------------------------------------------------------------ */
/* indexing / slicing / iteration                                      */
/* ------------------------------------------------------------------ */

static int64_t normalize_index(int64_t idx, uint64_t len) {
    if (idx < 0) idx += (int64_t)len;
    if (idx < 0 || idx >= (int64_t)len) panicf("index out of range");
    return idx;
}

aval a_getitem(aval obj, aval idx) {
    switch (kind_of(obj)) {
        case K_STR: {
            AStr *s = (AStr *)obj_of(obj);
            int64_t total = (int64_t)u8_count(s->data, s->h.len);
            int64_t i = normalize_index(expect_int64(idx, "string index"), (uint64_t)total);
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            return a_str_new(s->data + boff, (uint64_t)l);
        }
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            return root->items[(size_t)(base + i)];
        }
        case K_CHAR: {
            panicf("object of type 'char' is not subscriptable");
            return A_NONE;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            return a->items[i];
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(obj);
            int64_t i = dict_find(d, idx);
            if (i < 0) {
                uint64_t kl = 0;
                const char *kp = kind_of(idx) == K_STR ? str_data(idx, &kl) : NULL;
                if (kp)
                    panicf("Key %s is not in the map.", (char *)memcpy((char[256]){0}, kp, kl < 255 ? kl : 255));
                panicf("key not found");
            }
            return d->pairs[i].v;
        }
        default:
            panicf("object of type '%s' is not subscriptable", type_name_cstr(obj));
            return A_NONE;
    }
}

aval a_setitem(aval obj, aval idx, aval val) {
    switch (kind_of(obj)) {
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            root->items[(size_t)(base + i)] = val;
            return val;
        }
        case K_LIST: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            a->items[i] = val;
            return val;
        }
        case K_DICT: {
            dict_put((ADict *)obj_of(obj), idx, val);
            return val;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
}

static aval view_new(aval parent, int64_t off, int64_t len, int is_tuple) {
    AView *vw = (AView *)xmalloc(sizeof(AView));
    vw->h.type = T_VIEW;
    vw->h.len = (uint32_t)len;
    vw->h.cap = 0;
    vw->parent = parent;
    vw->off = off;
    vw->len = len;
    vw->is_tuple = is_tuple;
    return mkobj(&vw->h);
}
/* Resolve a view (or direct list) to root AArr + absolute offset. */
static AArr *view_root(aval v, int64_t *off) {
    AObj *o = obj_of(v);
    if (o && o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base = 0;
        AArr *root = view_root(vw->parent, &base);
        *off = base + vw->off;
        return root;
    }
    *off = 0;
    return (AArr *)o;
}
static int64_t slice_step(aval v) {
    int64_t s = expect_int64(v, "slice step");
    if (s == 0) panicf("The slice step must be positive, found 0.");
    return s;
}

static int64_t slice_bound(aval v, int64_t n, int64_t lower, int64_t upper,
                           int64_t dflt) {
    if (a_is_none(v)) return dflt;
    int64_t i = expect_int64(v, "slice index");
    if (i < 0) i += n;
    if (i < lower) i = lower;
    if (i > upper) i = upper;
    return i;
}

aval a_slice(aval obj, aval lo, aval hi, aval step) {
    int k = kind_of(obj);
    if (k != K_STR && k != K_LIST && k != K_TUPLE && k != K_VIEW)
        panicf("object of type '%s' is not sliceable", type_name_cstr(obj));
    AObj *o = obj_of(obj);
    uint64_t len;
    if (o->type == T_STR)
        len = u8_count(((AStr *)o)->data, ((AStr *)o)->h.len);
    else if (o->type == T_VIEW)
        len = (uint64_t)((AView *)o)->len;
    else
        len = ((AArr *)o)->h.len;
    int64_t n = (int64_t)len;
    int64_t st = a_is_none(step) ? 1 : slice_step(step);
    int64_t start, stop;
    if (st > 0) {
        start = slice_bound(lo, n, 0, n, 0);
        stop = slice_bound(hi, n, 0, n, n);
    } else {
        start = slice_bound(lo, n, -1, n - 1, n - 1);
        stop = slice_bound(hi, n, -1, n - 1, -1);
    }
    AVec v = { NULL, 0, 0 };
    if (o->type == T_STR) {
        AStr *s = (AStr *)o;
        Buf b;
        buf_init(&b);
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            if (boff < s->h.len) buf_addn(&b, s->data + boff, (size_t)l);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        return buf_take(&b);
    }
    /* Lists/tuples/views: contiguous step (1 or -1? only 1) returns a view
       that aliases the parent; stepped copies return a fresh list. */
    if (st == 1 && o->type != T_STR) {
        int64_t cnt = stop - start;
        if (cnt < 0) cnt = 0;
        if (o->type == T_VIEW) {
            int64_t base;
            AArr *root = view_root(obj, &base);
            (void)root;
            int64_t rootBase = 0;
            AArr *flat = view_root(obj, &rootBase);
            aval flatAval = obj;
            /* flatten: parent is the root array object */
            int64_t dummy = 0;
            AObj *rootObj = (AObj *)view_root(obj, &dummy);
            /* rootObj points at root AArr; recover its aval via parent chain */
            /* Simpler: keep obj as parent but offset relative to obj */
            AView *vw = (AView *)o;
            (void)flat; (void)flatAval;
            return view_new(vw->parent, vw->off + start, cnt, vw->is_tuple);
        }
        (void)v;
        return view_new(obj, start, cnt, o->type == T_TUPLE ? 1 : 0);
    }
    if (o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base;
        AArr *root = view_root(obj, &base);
        (void)vw;
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            avec_push(&v, root->items[(size_t)(base + i)]);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        aval r = list_from_vec(v.p, v.len, 0);
        free(v.p);
        return r;
    }
    AArr *a = (AArr *)o;
    for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
        avec_push(&v, a->items[i]);
        if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
        else { if (i < INT64_MIN - st) break; i += st; }
    }
    aval r = list_from_vec(v.p, v.len, o->type == T_TUPLE);
    free(v.p);
    return r;
}

aval a_iter_new(aval v) {
    AIter *it = (AIter *)xmalloc(sizeof(AIter));
    it->h.type = T_ITER;
    it->h.len = 0;
    it->h.cap = 0;
    it->src = v;
    it->idx = 0;
    it->cur = it->end = it->step = 0;
    switch (kind_of(v)) {
        case K_VIEW:
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_STR:
        case K_DICT:
            return mkobj(&it->h);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            it->cur = r->lo;
            it->end = r->hi;
            it->step = r->step;
            return mkobj(&it->h);
        }
        default:
            free(it);
            panicf("object of type '%s' is not iterable", type_name_cstr(v));
            return A_NONE;
    }
}

aval a_iter_next(aval it, aval *out) {
    AObj *o = obj_of(it);
    if (!o || o->type != T_ITER) panicf("argument is not an iterator");
    AIter *iter = (AIter *)o;
    aval src = iter->src;
    if (kind_of(src) == K_RANGE) {
        int64_t c = iter->cur, e = iter->end, s = iter->step;
        int more = s > 0 ? c < e : c > e;
        if (!more) return (aval)0;
        *out = mkint_full(c);
        if (s > 0) {
            if (c > INT64_MAX - s) iter->cur = e;
            else iter->cur = c + s;
        } else {
            if (c < INT64_MIN - s) iter->cur = e;
            else iter->cur = c + s;
        }
        return (aval)1;
    }
    AObj *so = obj_of(src);
    uint64_t len;
    aval *items = NULL;
    ADict *d = NULL;
    AView *vw_src = NULL;
    int64_t vw_base = 0;
    if (so->type == T_DICT) {
        d = (ADict *)so;
        len = d->h.len;
    } else if (so->type == T_VIEW) {
        vw_src = (AView *)so;
        len = (uint64_t)vw_src->len;
    } else if (so->type == T_STR || so->type == T_CHAR) {
        len = u8_count(((AStr *)so)->data, ((AStr *)so)->h.len);
    } else {
        AArr *a = (AArr *)so;
        len = a->h.len;
        items = a->items;
    }
    if (iter->idx >= len) return (aval)0;
    uint64_t i = iter->idx++;
    if (d) *out = d->pairs[i].k;
    else if (vw_src) {
        AArr *root = view_root(src, &vw_base);
        *out = root->items[(size_t)(vw_base + (int64_t)i)];
    }
    else if (so->type == T_STR || so->type == T_CHAR) {
        AStr *s = (AStr *)so;
        uint64_t boff = cp_to_byte(s->data, s->h.len, (int64_t)i);
        int l = u8_len((unsigned char)s->data[boff]);
        if (boff + (uint64_t)l > s->h.len) l = 1;
        *out = a_char_new(s->data + boff, (uint64_t)l);
    }
    else *out = items[i];
    return (aval)1;
}

/* ------------------------------------------------------------------ */
/* functions / environments / results                                  */
/* ------------------------------------------------------------------ */

static AFnon *fnon_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_FNON)
        panicf("%s must be callable, not '%s'", what, type_name_cstr(v));
    return (AFnon *)o;
}

static aval fnon_new_raw(uint64_t entry, aval env, const char *name,
                         uint32_t flags, uint64_t nparams) {
    AFnon *f = (AFnon *)xmalloc(sizeof(AFnon));
    f->h.type = T_FNON;
    f->h.len = 0;
    f->h.cap = 0;
    f->entry = entry;
    f->env = env;
    f->nparams = nparams;
    f->flags = flags;
    f->name = name ? xstrdup(name) : NULL;
    return mkobj(&f->h);
}

aval a_fnon_new(uint64_t entry, aval env, const char *name) {
    return fnon_new_raw(entry, env, name, 0, NPARAMS_UNKNOWN);
}

aval a_closure_env_new(uint64_t n) {
    AEnv *e = (AEnv *)xmalloc(sizeof(AEnv) + (size_t)(n ? n : 1) * sizeof(aval));
    e->h.type = T_ENV;
    e->h.len = (uint32_t)n;
    e->h.cap = n;
    for (uint64_t i = 0; i < n; ++i) e->slots[i] = A_NONE;
    return mkobj(&e->h);
}

aval a_env_set(aval env, uint64_t i, aval v) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    e->slots[i] = v;
    return A_NONE;
}

aval a_env_get(aval env, uint64_t i) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    return e->slots[i];
}

static AResult *result_new(int ok, aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = ok ? 1 : 0;
    r->h.cap = 0;
    r->ok = ok ? 1u : 0u;
    r->is_option = 0;
    r->value = value;
    return r;
}

aval a_some_new(aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = 1;
    r->h.cap = 0;
    r->ok = 1;
    r->is_option = 1;
    r->value = value;
    return mkobj(&r->h);
}

static int result_is_option(aval v) { return ((AResult *)obj_of(v))->is_option != 0; }

static int is_result(aval v) {
    AObj *o = obj_of(v);
    return o && o->type == T_RESULT;
}

static int result_ok(aval v) { return ((AResult *)obj_of(v))->ok != 0; }
static aval result_value(aval v) { return ((AResult *)obj_of(v))->value; }

aval a_call(aval fn, uint64_t n, aval *argv) {
    AFnon *f = fnon_of(fn, "call target");
    if (f->flags & FNON_BUILTIN) {
        a_builtin_fn bf = (a_builtin_fn)(uintptr_t)f->entry;
        return bf(n, argv);
    }
    if (f->nparams != NPARAMS_UNKNOWN && n != f->nparams)
        panicf("expected %llu argument(s), got %llu",
               (unsigned long long)f->nparams, (unsigned long long)n);
    uint64_t total = f->nparams == NPARAMS_UNKNOWN ? n + 1 : f->nparams + 1;
    aval *av = (aval *)xmalloc((size_t)total * sizeof(aval));
    av[0] = f->env;
    for (uint64_t i = 0; i < n; ++i) av[i + 1] = argv[i];
    for (uint64_t i = n + 1; i < total; ++i) av[i] = A_NONE;
    aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)f->entry;
    aval r = code(av);
    free(av);
    return r;
}

aval a_unwrap(aval v) {
    if (a_is_none(v)) panicf("Cannot unwrap None with '!'.");
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        panicf("called '!' on Err(...)");
    }
    return v;
}

aval a_try_result(aval v) {
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        return v; /* Err propagates as a result value */
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* methods                                                             */
/* ------------------------------------------------------------------ */

static void want_args(const char *name, uint64_t n, uint64_t min, uint64_t max) {
    if (n < min || n > max) {
        if (min == max)
            panicf("'%s' expects %llu argument(s), got %llu", name,
                   (unsigned long long)min, (unsigned long long)n);
        panicf("'%s' expects between %llu and %llu arguments, got %llu", name,
               (unsigned long long)min, (unsigned long long)max,
               (unsigned long long)n);
    }
}

static aval method_no_method(aval obj, const char *name) {
    panicf("'%s' object has no method '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

static aval str_case_map(aval v, int mode) {
    /* mode: 0 lower, 1 upper, 2 capitalize, 3 title, 4 swapcase */
    AStr *s = (AStr *)obj_of(v);
    AStr *r = (AStr *)xmalloc(sizeof(AStr) + (size_t)s->h.len + 1);
    r->h.type = T_STR;
    r->h.len = s->h.len;
    r->h.cap = s->h.len + 1;
    int prev_alpha = 0;
    for (uint32_t i = 0; i < s->h.len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        unsigned char o = c;
        if (mode == 0) o = (unsigned char)tolower(c);
        else if (mode == 1) o = (unsigned char)toupper(c);
        else if (mode == 2) o = (unsigned char)(i == 0 ? toupper(c) : tolower(c));
        else if (mode == 4) o = (unsigned char)(isupper(c) ? tolower(c) : islower(c) ? toupper(c) : c);
        else {
            if (isalpha(c)) o = (unsigned char)(prev_alpha ? tolower(c) : toupper(c));
            else o = c;
        }
        prev_alpha = isalpha(o) != 0;
        r->data[i] = (char)o;
    }
    r->data[s->h.len] = '\0';
    return mkobj(&r->h);
}

static int is_space_byte(unsigned char c) { return isspace(c) != 0; }

static aval str_strip_impl(aval v, int left, int right) {
    AStr *s = (AStr *)obj_of(v);
    uint32_t a = 0, b = s->h.len;
    if (left) while (a < b && is_space_byte((unsigned char)s->data[a])) ++a;
    if (right) while (b > a && is_space_byte((unsigned char)s->data[b - 1])) --b;
    return a_str_new(s->data + a, (uint64_t)(b - a));
}

static AVec str_split_impl(aval v, aval sep, int64_t maxsplit, int rsplit) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t n = s->h.len;
    AVec out = { NULL, 0, 0 };
    if (a_is_none(sep)) {
        if (maxsplit == 0) {
            avec_push(&out, a_str_new(s->data, n));
            return out;
        }
        /* whitespace split; for rsplit with a limit merge leading words */
        uint64_t i = 0;
        int64_t splits = 0;
        while (i < n) {
            while (i < n && is_space_byte((unsigned char)s->data[i])) ++i;
            if (i >= n) break;
            uint64_t st = i;
            while (i < n && !is_space_byte((unsigned char)s->data[i])) ++i;
            avec_push(&out, a_str_new(s->data + st, i - st));
            if (maxsplit >= 0 && ++splits >= maxsplit && i < n) {
                /* keep remainder as one token */
                while (i < n && is_space_byte((unsigned char)s->data[i])) ++i;
                if (i < n) avec_push(&out, a_str_new(s->data + i, n - i));
                break;
            }
        }
        (void)rsplit;
        return out;
    }
    uint64_t sn;
    const char *sp = str_data(sep, &sn);
    if (sn == 0) panicf("empty separator");
    if (maxsplit < 0) maxsplit = INT64_MAX;
    if (!rsplit) {
        uint64_t pos = 0, last = 0;
        int64_t done = 0;
        while (pos + sn <= n && done < maxsplit) {
            size_t p;
            if (!find_bytes(s->data + pos, (size_t)(n - pos), sp, (size_t)sn, 0, &p)) break;
            uint64_t at = pos + p;
            avec_push(&out, a_str_new(s->data + last, at - last));
            pos = at + sn;
            last = pos;
            ++done;
        }
        avec_push(&out, a_str_new(s->data + last, n - last));
        return out;
    }
    /* rsplit: find all occurrences, keep the last maxsplit separators */
    AVec occ = { NULL, 0, 0 };
    uint64_t pos = 0;
    while (pos + sn <= n) {
        size_t p;
        if (!find_bytes(s->data + pos, (size_t)(n - pos), sp, (size_t)sn, 0, &p)) break;
        uint64_t at = pos + p;
        avec_push(&occ, (aval)at);
        pos = at + sn;
    }
    if ((int64_t)occ.len <= maxsplit) {
        /* same as split */
        free(occ.p);
        return str_split_impl(v, sep, -1, 0);
    }
    uint64_t keep = (uint64_t)maxsplit;
    uint64_t first = occ.len - keep; /* index of first kept occurrence */
    uint64_t start = 0;
    for (uint64_t k = first; k < occ.len; ++k) {
        uint64_t at = (uint64_t)occ.p[k];
        avec_push(&out, a_str_new(s->data + start, at - start));
        start = at + sn;
    }
    avec_push(&out, a_str_new(s->data + start, n - start));
    free(occ.p);
    return out;
}

static aval str_replace(aval v, aval oldv, aval newv, int64_t count) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t on, nn, sn = s->h.len;
    const char *op = str_data(oldv, &on), *np = str_data(newv, &nn);
    const char *sp = s->data;
    Buf b;
    buf_init(&b);
    if (on == 0) panicf("The 'replace' target must not be empty.");
    if (count < 0) count = INT64_MAX;
    uint64_t pos = 0, last = 0;
    int64_t done = 0;
    while (done < count && pos + on <= sn) {
        size_t p;
        if (!find_bytes(sp + pos, (size_t)(sn - pos), op, (size_t)on, 0, &p)) break;
        uint64_t at = pos + p;
        buf_addn(&b, sp + last, at - last);
        buf_addn(&b, np, (size_t)nn);
        pos = at + on;
        last = pos;
        ++done;
    }
    buf_addn(&b, sp + last, sn - last);
    return buf_take(&b);
}

static int str_pred(aval v, int pred) {
    AStr *s = (AStr *)obj_of(v);
    if (s->h.len == 0) return 0;
    int has_lower = 0, has_upper = 0, prev_alpha = 0, title_ok = 1;
    for (uint32_t i = 0; i < s->h.len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        switch (pred) {
            case 0: if (!isdigit(c)) return 0; break;                 /* isdigit */
            case 1: if (!isalpha(c)) return 0; break;                 /* isalpha */
            case 2: if (!isspace(c)) return 0; break;                 /* isspace */
            case 3: if (!isalnum(c)) return 0; break;                 /* isalnum */
            case 4: /* islower */
                if (isupper(c)) return 0;
                if (islower(c)) has_lower = 1;
                break;
            case 5: /* isupper */
                if (islower(c)) return 0;
                if (isupper(c)) has_upper = 1;
                break;
            case 6: /* istitle */
                if (isalpha(c)) {
                    if (islower(c) && !prev_alpha) title_ok = 0;
                    if (isupper(c) && prev_alpha) title_ok = 0;
                    prev_alpha = 1;
                } else prev_alpha = 0;
                break;
        }
    }
    if (pred == 4) return has_lower;
    if (pred == 5) return has_upper;
    if (pred == 6) return title_ok;
    return 1;
}

static aval str_affix(aval v, int64_t width, aval fillv, int mode) {
    /* mode 0 ljust, 1 rjust, 2 center, 3 zfill */
    AStr *s = (AStr *)obj_of(v);
    uint64_t n = s->h.len;
    char fill = ' ';
    if (!a_is_none(fillv)) {
        uint64_t fn;
        const char *fp = str_data(fillv, &fn);
        if (fn) fill = fp[0];
    }
    if (width < 0) width = 0;
    if ((uint64_t)width <= n) return a_str_new(s->data, n);
    uint64_t pad = (uint64_t)width - n;
    Buf b;
    buf_init(&b);
    uint64_t left = 0, right = pad;
    if (mode == 1) { left = pad; right = 0; }
    else if (mode == 2) { left = (pad + 1) / 2; right = pad - left; }
    else if (mode == 3) {
        /* zfill: sign first, then zeros */
        uint64_t i = 0;
        if (n && (s->data[0] == '-' || s->data[0] == '+')) {
            buf_addc(&b, s->data[0]);
            ++i;
        }
        for (uint64_t k = i; k < pad + i; ++k) buf_addc(&b, '0');
        buf_addn(&b, s->data + i, n - i);
        return buf_take(&b);
    }
    for (uint64_t i = 0; i < left; ++i) buf_addc(&b, fill);
    buf_addn(&b, s->data, n);
    for (uint64_t i = 0; i < right; ++i) buf_addc(&b, fill);
    return buf_take(&b);
}

static aval seq_index(aval v, aval item, int64_t start, int64_t stop) {
    AArr *a = (AArr *)obj_of(v);
    uint64_t n = a->h.len;
    int64_t lo = start < 0 ? start + (int64_t)n : start;
    int64_t hi = stop < 0 ? stop + (int64_t)n : stop;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)n) hi = (int64_t)n;
    for (int64_t i = lo; i < hi; ++i)
        if (values_equal(a->items[i], item)) return mkint_full(i);
    panicf("Value is not in the list.");
    return A_NONE;
}

static int seq_contains(aval v, aval item) {
    AArr *a = (AArr *)obj_of(v);
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (values_equal(a->items[i], item)) return 1;
    return 0;
}

static void sort_arr(aval *a, uint64_t n) {
    for (uint64_t i = 1; i < n; ++i) {
        aval key = a[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && truthy(a_lt(key, a[j]))) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = key;
    }
}

static aval collect_iterable(aval v) {
    return a_list_from_iter(v);
}

aval a_getattr(aval obj, const char *name) {
    if (kind_of(obj) == K_DICT) {
        ADict *d = (ADict *)obj_of(obj);
        aval key = a_str_new(name, strlen(name));
        int64_t i = dict_find(d, key);
        if (i >= 0) return d->pairs[i].v;
    }
    panicf("'%s' object has no attribute '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

static aval list_pop(aval v, int64_t idx, int has_idx) {
    AArr *a = (AArr *)obj_of(v);
    if (a->h.len == 0) panicf("Cannot pop from an empty array.");
    int64_t i = has_idx ? normalize_index(idx, a->h.len) : (int64_t)a->h.len - 1;
    aval r = a->items[i];
    memmove(a->items + i, a->items + i + 1,
            (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
    a->h.len--;
    return r;
}

static aval dict_method_get(aval v, uint64_t n, aval *argv) {
    want_args("get", n, 1, 2);
    return a_dict_get(v, argv[0], n == 2 ? argv[1] : A_NONE);
}

static aval make_keys_values_items(aval v, int mode) {
    ADict *d = (ADict *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_LIST;
    out->h.len = 0;
    out->h.cap = d->h.len;
    out->items = (aval *)xmalloc((size_t)(d->h.len ? d->h.len : 1) * sizeof(aval));
    for (uint64_t i = 0; i < d->h.len; ++i) {
        aval x;
        if (mode == 0) x = d->pairs[i].k;
        else if (mode == 1) x = d->pairs[i].v;
        else {
            aval pair[2] = { d->pairs[i].k, d->pairs[i].v };
            x = a_tuple_new(2, pair);
        }
        out->items[out->h.len++] = x;
    }
    return mkobj(&out->h);
}

static aval method_set_op(aval v, aval other, int mode) {
    /* mode: 0 union, 1 intersection, 2 difference, 3 symmetric_difference */
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_SET;
    out->h.len = 0;
    out->h.cap = s->h.len + o->h.len;
    out->items = (aval *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(aval));
    for (uint64_t i = 0; i < s->h.len; ++i) {
        int in_o = 0;
        for (uint64_t j = 0; j < o->h.len; ++j)
            if (values_equal(s->items[i], o->items[j])) { in_o = 1; break; }
        int keep = mode == 0 ? 1 : mode == 1 ? in_o : mode == 2 ? !in_o : (in_o ? 0 : 1);
        /* symmetric_difference keeps elements in exactly one side; first pass adds self-only */
        if (mode == 3) keep = !in_o;
        if (keep && !set_contains(out, s->items[i])) arr_push(out, s->items[i]);
    }
    if (mode == 0 || mode == 3) {
        for (uint64_t j = 0; j < o->h.len; ++j) {
            int in_s = 0;
            for (uint64_t i = 0; i < s->h.len; ++i)
                if (values_equal(o->items[j], s->items[i])) { in_s = 1; break; }
            if ((mode == 0 || !in_s) && !set_contains(out, o->items[j]))
                arr_push(out, o->items[j]);
        }
    }
    return mkobj(&out->h);
}

static aval method_subset(aval v, aval other, int sup) {
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *base = sup ? s : o;
    AArr *test = sup ? o : s;
    for (uint64_t i = 0; i < test->h.len; ++i) {
        int found = 0;
        for (uint64_t j = 0; j < base->h.len; ++j)
            if (values_equal(test->items[i], base->items[j])) { found = 1; break; }
        if (!found) return A_FALSE;
    }
    return A_TRUE;
}

aval a_method(aval obj, const char *name, uint64_t n, aval *argv) {
    int k = kind_of(obj);
    if (k == K_STR) {
        AStr *s = (AStr *)obj_of(obj);
        if (!strcmp(name, "upper")) { want_args(name, n, 0, 0); return str_case_map(obj, 1); }
        if (!strcmp(name, "lower")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "capitalize")) { want_args(name, n, 0, 0); return str_case_map(obj, 2); }
        if (!strcmp(name, "title")) { want_args(name, n, 0, 0); return str_case_map(obj, 3); }
        if (!strcmp(name, "casefold")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "swapcase")) { want_args(name, n, 0, 0); return str_case_map(obj, 4); }
        if (!strcmp(name, "strip") || !strcmp(name, "lstrip") || !strcmp(name, "rstrip")) {
            int left = name[0] != 'r';
            int right = strcmp(name, "lstrip") != 0;
            if (n == 0) return str_strip_impl(obj, left, right);
            want_args(name, n, 1, 1);
            /* Strip chars arg: trim those bytes (ASCII set). */
            uint64_t cn;
            const char *cp = str_data(argv[0], &cn);
            uint32_t a = 0, b = s->h.len;
            int inSet = 0;
            if (left) {
                while (a < b) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[a] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    ++a;
                }
            }
            if (right) {
                while (b > a) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[b - 1] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    --b;
                }
            }
            return a_str_new(s->data + a, (uint64_t)(b - a));
        }
        if (!strcmp(name, "split") || !strcmp(name, "rsplit")) {
            want_args(name, n, 0, 2);
            aval sep = n >= 1 ? argv[0] : A_NONE;
            int64_t ms = -1;
            if (n >= 2) ms = expect_int64(argv[1], "maxsplit");
            AVec parts = str_split_impl(obj, sep, ms, name[0] == 'r');
            aval r = list_from_vec(parts.p, parts.len, 0);
            free(parts.p);
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(argv[0]);
            AArr *a = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (kind_of(a->items[i]) != K_STR && kind_of(a->items[i]) != K_CHAR)
                    panicf("sequence item %llu: expected str instance, %s found",
                           (unsigned long long)i, type_name_cstr(a->items[i]));
            aval r = a_str_joinsep(obj, a->h.len, a->items);
            return r;
        }
        if (!strcmp(name, "replace")) {
            want_args(name, n, 2, 3);
            int64_t count = n == 3 ? expect_int64(argv[2], "count") : -1;
            return str_replace(obj, argv[0], argv[1], count);
        }
        if (!strcmp(name, "startswith") || !strcmp(name, "endswith")) {
            want_args(name, n, 1, 3);
            int64_t st = 0, en = (int64_t)s->h.len;
            if (n >= 2) st = expect_int64(argv[1], "start");
            if (n >= 3) en = expect_int64(argv[2], "end");
            if (st < 0) st += (int64_t)s->h.len;
            if (en < 0) en += (int64_t)s->h.len;
            if (st < 0) st = 0;
            if (en > (int64_t)s->h.len) en = (int64_t)s->h.len;
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if ((uint64_t)(en - st) < pn) return A_FALSE;
            int c = !strncmp(s->data + st, pp, (size_t)pn);
            if (!strcmp(name, "endswith")) {
                int64_t pos = en - (int64_t)pn;
                c = pos >= st && !strncmp(s->data + pos, pp, (size_t)pn);
            }
            return bool_aval(c);
        }
        if (!strcmp(name, "find") || !strcmp(name, "index") || !strcmp(name, "rindex") || !strcmp(name, "rfind")) {
            want_args(name, n, 1, 3);
            int64_t st = 0;
            if (n >= 2) st = expect_int64(argv[1], "start");
            int64_t en = (int64_t)s->h.len;
            if (n >= 3) {
                en = expect_int64(argv[2], "end");
                /* approximate by clamping search to prefix */
                if (en < (int64_t)s->h.len) {
                    aval tmp = a_str_new(s->data, (uint64_t)en);
                    int64_t r = str_find(tmp, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
                    if ((r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex"))) ||
                        (r >= 0 && en < st)) r = -1;
                    if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                        panicf("Substring not found.");
                    return mkint_full(r);
                }
            }
            int64_t r = str_find(obj, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
            if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                panicf("Substring not found.");
            return mkint_full(r);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)s->h.len;
            aval hay = obj;
            if (en < (int64_t)s->h.len) hay = a_str_new(s->data, (uint64_t)en);
            uint64_t hn, nn;
            const char *h = str_data(hay, &hn), *nd = str_data(argv[0], &nn);
            int64_t count = 0, pos = st < 0 ? st + (int64_t)hn : st;
            if (pos < 0) pos = 0;
            if (nn == 0) return mkint_full((int64_t)hn + 1 > pos ? (int64_t)hn - pos + 1 : 0);
            while ((uint64_t)pos + nn <= hn) {
                size_t p;
                if (!find_bytes(h + pos, (size_t)(hn - (uint64_t)pos), nd, (size_t)nn, 0, &p)) break;
                ++count;
                pos += (int64_t)p + (int64_t)nn;
            }
            return mkint_full(count);
        }
        if (!strcmp(name, "isdigit")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 0)); }
        if (!strcmp(name, "isalpha")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 1)); }
        if (!strcmp(name, "isspace")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 2)); }
        if (!strcmp(name, "isalnum")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 3)); }
        if (!strcmp(name, "islower")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 4)); }
        if (!strcmp(name, "isupper")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 5)); }
        if (!strcmp(name, "istitle")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 6)); }
        if (!strcmp(name, "isascii")) {
            want_args(name, n, 0, 0);
            for (uint64_t q = 0; q < s->h.len; ++q) if ((unsigned char)s->data[q] >= 128) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isdecimal") || !strcmp(name, "isnumeric")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isdigit((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isidentifier")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            if (!isalpha((unsigned char)s->data[0]) && s->data[0] != '_') return A_FALSE;
            for (uint64_t q = 1; q < s->h.len; ++q)
                if (!isalnum((unsigned char)s->data[q]) && s->data[q] != '_') return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isprintable")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_TRUE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isprint((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "expandtabs")) {
            want_args(name, n, 0, 1);
            int64_t tab = n == 1 ? expect_int64(argv[0], "tabsize") : 8;
            if (tab <= 0) tab = 8;
            Buf b;
            buf_init(&b);
            int64_t col = 0;
            for (uint64_t q = 0; q < s->h.len; ++q) {
                char c = s->data[q];
                if (c == '\t') {
                    int64_t spaces = tab - (col % tab);
                    for (int64_t k = 0; k < spaces; ++k) buf_addc(&b, ' ');
                    col += spaces;
                } else if (c == '\n' || c == '\r') {
                    buf_addc(&b, c);
                    col = 0;
                } else {
                    buf_addc(&b, c);
                    ++col;
                }
            }
            return buf_take(&b);
        }
        if (!strcmp(name, "removeprefix") || !strcmp(name, "removesuffix")) {
            want_args(name, n, 1, 1);
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if (!strcmp(name, "removeprefix")) {
                if (s->h.len >= pn && !memcmp(s->data, pp, (size_t)pn))
                    return a_str_new(s->data + pn, s->h.len - pn);
                return obj;
            }
            if (s->h.len >= pn && !memcmp(s->data + s->h.len - pn, pp, (size_t)pn))
                return a_str_new(s->data, s->h.len - pn);
            return obj;
        }
        if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) {
            want_args(name, n, 1, 1);
            int rev = !strcmp(name, "rpartition");
            int64_t r = str_find(obj, argv[0], 0, rev);
            aval head, sep, tail;
            if (r < 0) {
                if (rev) head = obj, tail = a_str_new("", 0);
                else head = obj, tail = a_str_new("", 0);
                sep = a_str_new("", 0);
                /* Python: not found -> (original, '', '') */
                head = obj;
            } else {
                uint64_t pn;
                str_data(argv[0], &pn);
                head = a_str_new(s->data, (uint64_t)r);
                sep = a_str_new(s->data + r, pn);
                tail = a_str_new(s->data + r + pn, s->h.len - (uint64_t)r - pn);
            }
            aval items[3] = { head, sep, tail };
            return a_tuple_new(3, items);
        }
        if (!strcmp(name, "splitlines")) {
            want_args(name, n, 0, 1);
            int keep = n == 1 && truthy(argv[0]);
            aval lst = a_list_new(0, NULL);
            uint64_t start = 0;
            uint64_t i = 0;
            while (i < s->h.len) {
                char c = s->data[i];
                if (c == '\n' || c == '\r') {
                    uint64_t br_end = i + 1;
                    if (c == '\r' && br_end < s->h.len && s->data[br_end] == '\n') br_end++;
                    uint64_t end = keep ? br_end : i;
                    a_list_push(lst, a_str_new(s->data + start, end - start));
                    i = br_end;
                    start = br_end;
                } else {
                    ++i;
                }
            }
            if (start < s->h.len)
                a_list_push(lst, a_str_new(s->data + start, s->h.len - start));
            else if (start == s->h.len && s->h.len == 0) {
                /* empty string -> [] */
            }
            return lst;
        }
        if (!strcmp(name, "zfill") || !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust")) {
            want_args(name, n, 1, 2);
            int64_t w = expect_int64(argv[0], "width");
            aval fill = n == 2 ? argv[1] : A_NONE;
            int mode = !strcmp(name, "ljust") ? 0 : !strcmp(name, "rjust") ? 1 : !strcmp(name, "center") ? 2 : 3;
            return str_affix(obj, w, fill, mode);
        }
        if (!strcmp(name, "chars")) {
            want_args(name, n, 0, 0);
            aval lst = a_list_new(0, NULL);
            uint64_t i = 0;
            while (i < s->h.len) {
                uint64_t start = i;
                unsigned char c = (unsigned char)s->data[i];
                if (c < 0x80) i += 1;
                else if ((c & 0xE0) == 0xC0) i += 2;
                else if ((c & 0xF0) == 0xE0) i += 3;
                else if ((c & 0xF8) == 0xF0) i += 4;
                else i += 1;
                if (i > s->h.len) i = s->h.len;
                a_list_push(lst, a_char_new(s->data + start, i - start));
            }
            return lst;
        }
        if (!strcmp(name, "repeat")) {
            want_args(name, n, 1, 1);
            int64_t cnt = expect_int64(argv[0], "count");
            if (cnt < 0) cnt = 0;
            Buf b; buf_init(&b);
            for (int64_t i = 0; i < cnt; ++i) buf_addn(&b, s->data, (size_t)s->h.len);
            return buf_take(&b);
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) != K_STR && kind_of(argv[0]) != K_CHAR) return a_bool(0);
            AStr *hay = (AStr *)obj_of(obj);
            AStr *needle = (AStr *)obj_of(argv[0]);
            if (needle->h.len == 0) return a_bool(1);
            if (needle->h.len > hay->h.len) return a_bool(0);
            for (uint64_t i = 0; i + needle->h.len <= hay->h.len; ++i) {
                if (memcmp(hay->data + i, needle->data, (size_t)needle->h.len) == 0)
                    return a_bool(1);
            }
            return a_bool(0);
        }
        return method_no_method(obj, name);
    }

    if (k == K_LIST || k == K_TUPLE) {
        int is_list = k == K_LIST;
        AArr *a = (AArr *)obj_of(obj);
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            return seq_index(obj, argv[0], st, en);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            int64_t lo = st < 0 ? st + (int64_t)a->h.len : st;
            int64_t hi = en < 0 ? en + (int64_t)a->h.len : en;
            if (lo < 0) lo = 0;
            if (hi > (int64_t)a->h.len) hi = (int64_t)a->h.len;
            int64_t c = 0;
            for (int64_t i = lo; i < hi; ++i)
                if (values_equal(a->items[i], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "contains")) { want_args(name, n, 1, 1); return bool_aval(seq_contains(obj, argv[0])); }
        if (!strcmp(name, "first")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(a->items[0]); }
        if (!strcmp(name, "last")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(a->items[a->h.len - 1]); }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            return list_from_vec(a->items, a->h.len, !is_list);
        }
        if (!is_list) return method_no_method(obj, name);
        if (!strcmp(name, "append")) {
            want_args(name, n, 1, 1);
            arr_push(a, argv[0]);
            return A_NONE;
        }
        if (!strcmp(name, "extend")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(argv[0]);
            AArr *e = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < e->h.len; ++i) arr_push(a, e->items[i]);
            return A_NONE;
        }
        if (!strcmp(name, "insert")) {
            want_args(name, n, 2, 2);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0) i = 0;
            if (i > (int64_t)a->h.len) i = (int64_t)a->h.len;
            arr_reserve(a, (uint64_t)a->h.len + 1);
            memmove(a->items + i + 1, a->items + i,
                    (size_t)(a->h.len - (uint64_t)i) * sizeof(aval));
            a->items[i] = argv[1];
            a->h.len++;
            return A_NONE;
        }
        if (!strcmp(name, "remove_at")) {
            want_args(name, n, 1, 1);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0 || i >= (int64_t)a->h.len) panicf("remove_at index out of range");
            aval r = a->items[i];
            memmove(a->items + i, a->items + i + 1, (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
            a->h.len--;
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(obj);
            AArr *ja = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < ja->h.len; ++i) ja->items[i] = a_str(ja->items[i]);
            return a_str_joinsep(argv[0], ja->h.len, ja->items);
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (values_equal(a->items[i], argv[0])) {
                    memmove(a->items + i, a->items + i + 1,
                            (size_t)(a->h.len - i - 1) * sizeof(aval));
                    a->h.len--;
                    return A_NONE;
                }
            }
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 1);
            return list_pop(obj, n ? expect_int64(argv[0], "index") : 0, n != 0);
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); a->h.len = 0; return A_NONE; }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            sort_arr(a->items, a->h.len);
            return A_NONE;
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            for (uint64_t i = 0; i < a->h.len / 2; ++i) {
                uint64_t j = a->h.len - 1 - i;
                aval t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
            }
            return A_NONE;
        }
        return method_no_method(obj, name);
    }

    if (k == K_VIEW) {
        AView *vw = (AView *)obj_of(obj);
        int64_t base;
        AArr *root = view_root(obj, &base);
        if (!strcmp(name, "append")) {
            panicf("Cannot 'append' a view.");
        }
        if (!strcmp(name, "pop")) {
            panicf("Cannot 'pop' a view.");
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            for (int64_t i = 0; i < vw->len / 2; ++i) {
                int64_t j = vw->len - 1 - i;
                aval t = root->items[(size_t)(base + i)];
                root->items[(size_t)(base + i)] = root->items[(size_t)(base + j)];
                root->items[(size_t)(base + j)] = t;
            }
            return A_NONE;
        }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            aval *ptr = root->items + (size_t)base;
            sort_arr(ptr, (uint64_t)vw->len);
            return A_NONE;
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 1);
            int64_t c = 0;
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return mkint_full(i);
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return A_TRUE;
            return A_FALSE;
        }
        if (!strcmp(name, "first")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)base]);
        }
        if (!strcmp(name, "last")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)(base + vw->len - 1)]);
        }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            return list_from_vec(root->items + (size_t)base, (uint64_t)vw->len, vw->is_tuple);
        }
        if (!strcmp(name, "clear")) {
            panicf("Cannot 'clear' a view.");
        }
        return method_no_method(obj, name);
    }

    if (k == K_DICT) {
        ADict *d = (ADict *)obj_of(obj);
        if (!strcmp(name, "get")) return dict_method_get(obj, n, argv);
        if (!strcmp(name, "keys")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 0); }
        if (!strcmp(name, "values")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 1); }
        if (!strcmp(name, "items")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 2); }
        if (!strcmp(name, "update")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) == K_DICT) {
                ADict *o = (ADict *)obj_of(argv[0]);
                for (uint64_t i = 0; i < o->h.len; ++i) dict_put(d, o->pairs[i].k, o->pairs[i].v);
            } else {
                aval tmp = collect_iterable(argv[0]);
                AArr *a = (AArr *)obj_of(tmp);
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    aval p = a->items[i];
                    if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                        panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                    AArr *pa = (AArr *)obj_of(p);
                    if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                    dict_put(d, pa->items[0], pa->items[1]);
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i < 0) {
                if (n == 2) return argv[1];
                panicf("key not found");
            }
            aval r = d->pairs[i].v;
            memmove(d->pairs + i, d->pairs + i + 1,
                    (size_t)(d->h.len - (uint64_t)i - 1) * sizeof(APair));
            d->h.len--;
            return r;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); d->h.len = 0; return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            aval nd = a_dict_new();
            ADict *m = (ADict *)obj_of(nd);
            for (uint64_t i = 0; i < d->h.len; ++i) dict_put(m, d->pairs[i].k, d->pairs[i].v);
            return nd;
        }
        if (!strcmp(name, "setdefault")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i >= 0) return d->pairs[i].v;
            aval dv = n == 2 ? argv[1] : A_NONE;
            dict_put(d, argv[0], dv);
            return dv;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(dict_find(d, argv[0]) >= 0);
        }
        return method_no_method(obj, name);
    }

    if (k == K_SET) {
        AArr *s = (AArr *)obj_of(obj);
        if (!strcmp(name, "add")) { want_args(name, n, 1, 1); if (!set_contains(s, argv[0])) arr_push(s, argv[0]); return A_NONE; }
        if (!strcmp(name, "discard")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            panicf("value not in set");
            return A_NONE;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); s->h.len = 0; return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            AArr *out = (AArr *)xmalloc(sizeof(AArr));
            out->h.type = T_SET;
            out->h.len = s->h.len;
            out->h.cap = s->h.len;
            out->items = (aval *)xmalloc((size_t)(s->h.len ? s->h.len : 1) * sizeof(aval));
            if (s->h.len) memcpy(out->items, s->items, (size_t)s->h.len * sizeof(aval));
            return mkobj(&out->h);
        }
        if (!strcmp(name, "union")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 0); }
        if (!strcmp(name, "intersection")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 1); }
        if (!strcmp(name, "difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 2); }
        if (!strcmp(name, "symmetric_difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 3); }
        if (!strcmp(name, "issubset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 0); }
        if (!strcmp(name, "issuperset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 1); }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) panicf("pop from an empty set");
            aval r = s->items[0];
            memmove(s->items, s->items + 1, (size_t)(s->h.len - 1) * sizeof(aval));
            s->h.len--;
            return r;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(set_contains(s, argv[0]));
        }
        return method_no_method(obj, name);
    }

    return method_no_method(obj, name);
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

static double math_arg(aval v, const char *name) {
    if (!is_num(v)) panicf("%s() argument must be a number, not '%s'", name, type_name_cstr(v));
    return num_to_double(v);
}

static aval bi_print(uint64_t n, aval *argv) { (void)n; (void)argv; return a_printv(n, argv); }
static aval bi_eprint(uint64_t n, aval *argv) { (void)n; (void)argv; return a_eprintv(n, argv); }
static aval bi_len(uint64_t n, aval *argv) { want_args("len", n, 1, 1); return a_len(argv[0]); }
static aval bi_str(uint64_t n, aval *argv) { want_args("str", n, 1, 1); return a_str(argv[0]); }
static aval bi_int(uint64_t n, aval *argv) { want_args("int", n, 1, 1); return a_int(argv[0]); }
static aval bi_float(uint64_t n, aval *argv) { want_args("float", n, 1, 1); return a_float(argv[0]); }
static aval bi_bool(uint64_t n, aval *argv) { want_args("bool", n, 1, 1); return a_bool(argv[0]); }
static aval bi_repr(uint64_t n, aval *argv) { want_args("repr", n, 1, 1); return a_repr(argv[0]); }
static aval bi_type(uint64_t n, aval *argv) { want_args("type", n, 1, 1); return a_type_name(argv[0]); }

static aval bi_abs(uint64_t n, aval *argv) {
    want_args("abs", n, 1, 1);
    aval v = argv[0];
    if (v == A_TRUE) return mkint_full(1);
    if (v == A_FALSE) return mkint_full(0);
    if (is_int_val(v)) {
        int64_t i = int_val_full(v);
        if (i == INT64_MIN) return mkdouble(9223372036854775808.0);
        return mkint_full(i < 0 ? -i : i);
    }
    if (a_is_double(v)) return mkdouble(fabs(a_double_val(v)));
    panicf("bad operand type for abs(): '%s'", type_name_cstr(v));
    return A_NONE;
}

static aval minmax(uint64_t n, aval *argv, int is_min, const char *name) {
    if (n == 0) panicf("%s expected at least 1 argument, got 0", name);
    aval best;
    uint64_t start = 0;
    if (n == 1) {
        aval tmp = collect_iterable(argv[0]);
        AArr *a = (AArr *)obj_of(tmp);
        if (a->h.len == 0) panicf("%s() arg is an empty sequence", name);
        best = a->items[0];
        start = 1;
        for (uint64_t i = start; i < a->h.len; ++i) {
            aval c = a_lt(a->items[i], best);
            if (truthy(is_min ? c : a_lt(best, a->items[i]))) best = a->items[i];
        }
        return best;
    }
    best = argv[0];
    for (uint64_t i = 1; i < n; ++i) {
        aval c = a_lt(argv[i], best);
        if (truthy(is_min ? c : a_lt(best, argv[i]))) best = argv[i];
    }
    return best;
}

static aval bi_min(uint64_t n, aval *argv) { return minmax(n, argv, 1, "min"); }
static aval bi_max(uint64_t n, aval *argv) { return minmax(n, argv, 0, "max"); }

static aval bi_sum(uint64_t n, aval *argv) {
    want_args("sum", n, 1, 2);
    aval acc = n == 2 ? argv[1] : mkint_full(0);
    if (!is_num(acc)) panicf("sum() start must be a number");
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        if (!is_num(a->items[i]))
            panicf("unsupported operand type(s) for +: '%s' and '%s'",
                   type_name_cstr(acc), type_name_cstr(a->items[i]));
        acc = binary_arith(acc, a->items[i], '+', "+");
    }
    return acc;
}

static aval bi_pow(uint64_t n, aval *argv) {
    /* pow() is -> f64 per docs; the ** operator stays int for int base. */
    want_args("pow", n, 2, 2);
    aval r = a_pow(argv[0], argv[1]);
    if (is_int_val(r)) return mkdouble((double)int_val_full(r));
    return r;
}

#define BI_MATH1(fname, expr) \
    static aval fname(uint64_t n, aval *argv) { \
        want_args(#fname, n, 1, 1); \
        double x = math_arg(argv[0], #fname); \
        return mkdouble(expr); \
    }

BI_MATH1(bi_sin, sin(x))
BI_MATH1(bi_cos, cos(x))
BI_MATH1(bi_tan, tan(x))
BI_MATH1(bi_exp, exp(x))

static aval bi_sqrt(uint64_t n, aval *argv) {
    want_args("sqrt", n, 1, 1);
    double x = math_arg(argv[0], "sqrt");
    if (x < 0.0) panicf("math domain error");
    return mkdouble(sqrt(x));
}
static aval bi_log(uint64_t n, aval *argv) {
    want_args("log", n, 1, 1);
    double x = math_arg(argv[0], "log");
    if (x <= 0.0) panicf("math domain error");
    return mkdouble(log(x));
}
static aval bi_asin(uint64_t n, aval *argv) {
    want_args("asin", n, 1, 1);
    double x = math_arg(argv[0], "asin");
    if (x < -1.0 || x > 1.0) panicf("math domain error");
    return mkdouble(asin(x));
}
static aval bi_acos(uint64_t n, aval *argv) {
    want_args("acos", n, 1, 1);
    double x = math_arg(argv[0], "acos");
    if (x < -1.0 || x > 1.0) panicf("math domain error");
    return mkdouble(acos(x));
}
static aval bi_atan(uint64_t n, aval *argv) {
    want_args("atan", n, 1, 1);
    return mkdouble(atan(math_arg(argv[0], "atan")));
}

static aval bi_floor(uint64_t n, aval *argv) { want_args("floor", n, 1, 1); return mkdouble(floor(math_arg(argv[0], "floor"))); }
static aval bi_ceil(uint64_t n, aval *argv) { want_args("ceil", n, 1, 1); return mkdouble(ceil(math_arg(argv[0], "ceil"))); }
static aval bi_trunc(uint64_t n, aval *argv) { want_args("trunc", n, 1, 1); return mkdouble(trunc(math_arg(argv[0], "trunc"))); }
static aval bi_round(uint64_t n, aval *argv) {
    want_args("round", n, 1, 1);
    double x = math_arg(argv[0], "round");
    /* Round half away from zero (Python round() is banker's, spec wants away). */
    double r = x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
    return mkdouble(r);
}

static aval bi_random(uint64_t n, aval *argv) {
    want_args("random", n, 0, 0);
    (void)argv;
    return mkdouble((double)rand() / ((double)RAND_MAX + 1.0));
}

static aval bi_random_int(uint64_t n, aval *argv) {
    want_args("random_int", n, 2, 2);
    int64_t lo = expect_int64(argv[0], "random_int() argument");
    int64_t hi = expect_int64(argv[1], "random_int() argument");
    if (hi < lo) panicf("random_int() range is empty");
    if (hi == lo) return mkint_full(lo);
    long double span = (long double)hi - (long double)lo + 1.0L;
    long double u = (long double)rand() / ((long double)RAND_MAX + 1.0L);
    long double r = (long double)lo + floorl(u * span);
    if (r < (long double)lo) r = (long double)lo;
    if (r > (long double)hi) r = (long double)hi;
    return mkint_full((int64_t)r);
}

static aval bi_range(uint64_t n, aval *argv) {
    want_args("range", n, 1, 3);
    int64_t lo = 0, hi, step = 1;
    if (n == 1) hi = expect_int64(argv[0], "range() argument");
    else {
        lo = expect_int64(argv[0], "range() argument");
        hi = expect_int64(argv[1], "range() argument");
        if (n == 3) step = expect_int64(argv[2], "range() step");
    }
    return a_range_new(mkint_full(lo), mkint_full(hi), mkint_full(step));
}

static aval bi_set(uint64_t n, aval *argv) {
    want_args("set", n, 0, 1);
    aval s = a_set_new();
    if (n == 1) {
        aval tmp = collect_iterable(argv[0]);
        AArr *a = (AArr *)obj_of(tmp);
        for (uint64_t i = 0; i < a->h.len; ++i) a_set_add(s, a->items[i]);
    }
    return s;
}

static aval bi_tuple(uint64_t n, aval *argv) {
    want_args("tuple", n, 0, 1);
    if (n == 0) return a_tuple_new(0, NULL);
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    return a_tuple_new(a->h.len, a->items);
}

static aval bi_list(uint64_t n, aval *argv) {
    want_args("list", n, 0, 1);
    if (n == 0) return a_list_new(0, NULL);
    return collect_iterable(argv[0]);
}

static aval bi_dict(uint64_t n, aval *argv) {
    want_args("dict", n, 0, 1);
    aval d = a_dict_new();
    if (n == 1) {
        if (kind_of(argv[0]) == K_DICT) {
            ADict *o = (ADict *)obj_of(argv[0]);
            for (uint64_t i = 0; i < o->h.len; ++i) dict_put((ADict *)obj_of(d), o->pairs[i].k, o->pairs[i].v);
        } else {
            aval tmp = collect_iterable(argv[0]);
            AArr *a = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < a->h.len; ++i) {
                aval p = a->items[i];
                if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                    panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                AArr *pa = (AArr *)obj_of(p);
                if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                dict_put((ADict *)obj_of(d), pa->items[0], pa->items[1]);
            }
        }
    }
    return d;
}

static aval bi_map(uint64_t n, aval *argv) {
    want_args("map", n, 2, 2);
    /* A calls map(xs, f); accept either order. */
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp = collect_iterable(argv[1]);
    AArr *a = (AArr *)obj_of(tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval one[1] = { a->items[i] };
        avec_push(&out, a_call(argv[0], 1, one));
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

static aval bi_filter(uint64_t n, aval *argv) {
    want_args("filter", n, 2, 2);
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp = collect_iterable(argv[1]);
    AArr *a = (AArr *)obj_of(tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval one[1] = { a->items[i] };
        if (truthy(a_call(argv[0], 1, one))) avec_push(&out, a->items[i]);
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

static aval bi_any(uint64_t n, aval *argv) { want_args("any", n, 1, 2); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i){ aval v=a->items[i]; if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(truthy(v)) return A_TRUE; } return A_FALSE; }
static aval bi_all(uint64_t n, aval *argv) { want_args("all", n, 1, 2); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i){ aval v=a->items[i]; if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(!truthy(v)) return A_FALSE; } return A_TRUE; }
static aval bi_some(uint64_t n, aval *argv) { want_args("Some", n, 1, 1); return a_some_new(argv[0]); }

static aval bi_reduce(uint64_t n, aval *argv) {
    want_args("reduce", n, 3, 3);
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    aval acc = argv[1];
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval two[2] = { acc, a->items[i] };
        acc = a_call(argv[2], 2, two);
    }
    return acc;
}

static aval bi_ok(uint64_t n, aval *argv) { want_args("ok", n, 1, 1); return mkobj(result_new(1, argv[0])); }
static aval bi_err(uint64_t n, aval *argv) { want_args("err", n, 1, 1); return mkobj(result_new(0, argv[0])); }

static void panic_with_msg_at(uint64_t n, aval *argv, int idx, const char *def) {
    (void)n;
    if (idx < 0) { a_panic_msg(def); return; }
    aval s = a_str(argv[idx]);
    uint64_t len;
    const char *p = str_data(s, &len);
    (void)len;
    a_panic_msg(p);
}

static aval bi_panic(uint64_t n, aval *argv) {
    want_args("panic", n, 0, 1);
    if (n == 0) a_panic_msg("panic");
    else panic_with_msg_at(n, argv, 0, "panic");
    return A_NONE;
}

static aval bi_assert(uint64_t n, aval *argv) {
    want_args("assert", n, 1, 2);
    if (!truthy(argv[0])) panic_with_msg_at(n, argv, n == 2 ? 1 : -1, "Assertion failed");
    return A_NONE;
}

static aval bi_assert_eq(uint64_t n, aval *argv) {
    want_args("assert_eq", n, 2, 3);
    if (!values_equal(argv[0], argv[1])) {
        Buf b;
        buf_init(&b);
        if (n == 3) {
            /* Match integration test harness format. */
            buf_adds(&b, "Assertion failed: expected ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, ", found ");
            format_into(&b, argv[1], 1);
            buf_adds(&b, " (");
            aval msg = a_str(argv[2]);
            uint64_t mlen;
            const char *mp = str_data(msg, &mlen);
            buf_addn(&b, mp, mlen);
            buf_addc(&b, ')');
        } else {
            buf_adds(&b, "assertion failed: ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, " != ");
            format_into(&b, argv[1], 1);
        }
        aval s = buf_take(&b);
        uint64_t len;
        a_panic_msg(str_data(s, &len));
    }
    return A_NONE;
}

static aval bi_input(uint64_t n, aval *argv) {
    want_args("input", n, 0, 1);
    if (n == 1) {
        aval s = a_str(argv[0]);
        uint64_t len;
        const char *p = str_data(s, &len);
        fwrite(p, 1, (size_t)len, stdout);
    }
    fflush(stdout);
    char *buf = (char *)xmalloc(4096);
    if (!fgets(buf, 4096, stdin)) { free(buf); panicf("Input ended before a line could be read."); }
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len && buf[len - 1] == '\r') buf[--len] = '\0';
    aval r = a_str_new(buf, (uint64_t)len);
    free(buf);
    return r;
}

static char *read_all_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) panicf("cannot read file '%s'", path);
    size_t cap = 4096, len = 0;
    char *buf = (char *)xmalloc(cap + 1);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (char *)xrealloc(buf, cap + 1); }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) break;
    }
    if (ferror(f)) { fclose(f); free(buf); panicf("cannot read file '%s'", path); }
    fclose(f);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static const char *path_arg(aval v, const char *what) {
    return str_of(v, what)->data;
}

static aval bi_read_file(uint64_t n, aval *argv) {
    want_args("read_file", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
    size_t len;
    char *data = read_all_file(path, &len);
    aval r = a_str_new(data, (uint64_t)len);
    free(data);
    return r;
}

static aval write_file_impl(aval pathv, aval textv, const char *mode, const char *name) {
    const char *path = path_arg(pathv, "path");
    if (kind_of(textv) != K_STR) panicf("%s() text must be str", name);
    uint64_t len;
    const char *p = str_data(textv, &len);
    FILE *f = fopen(path, mode);
    if (!f) panicf("cannot open file '%s'", path);
    if (len && fwrite(p, 1, (size_t)len, f) != len) {
        fclose(f);
        panicf("cannot write file '%s'", path);
    }
    fclose(f);
    return A_NONE;
}

static aval bi_write_file(uint64_t n, aval *argv) { want_args("write_file", n, 2, 2); return write_file_impl(argv[0], argv[1], "wb", "write_file"); }
static aval bi_append_file(uint64_t n, aval *argv) { want_args("append_file", n, 2, 2); return write_file_impl(argv[0], argv[1], "ab", "append_file"); }

static aval bi_file_exists(uint64_t n, aval *argv) {
    want_args("file_exists", n, 1, 1);
    FILE *f = fopen(path_arg(argv[0], "path"), "rb");
    if (!f) return A_FALSE;
    fclose(f);
    return A_TRUE;
}

static int str_ptr_cmp(const void *a, const void *b) {
    aval x = *(const aval *)a, y = *(const aval *)b;
    uint64_t lx, ly;
    const char *px = str_data(x, &lx), *py = str_data(y, &ly);
    size_t m = lx < ly ? (size_t)lx : (size_t)ly;
    int c = m ? memcmp(px, py, m) : 0;
    if (c) return c;
    return lx < ly ? -1 : lx > ly ? 1 : 0;
}

static aval bi_list_dir(uint64_t n, aval *argv) {
    want_args("list_dir", n, 1, 1);
#if !defined(_WIN32)
    const char *path = path_arg(argv[0], "path");
    DIR *d = opendir(path);
    if (!d) panicf("cannot open directory '%s'", path);
    AVec out = { NULL, 0, 0 };
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        avec_push(&out, a_str_new(de->d_name, strlen(de->d_name)));
    }
    closedir(d);
    /* insertion sort using the same comparator as qsort would need context; simple shell sort */
    for (size_t gap = out.len / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < out.len; ++i) {
            aval t = out.p[i];
            size_t j = i;
            while (j >= gap && str_ptr_cmp(&out.p[j - gap], &t) > 0) {
                out.p[j] = out.p[j - gap];
                j -= gap;
            }
            out.p[j] = t;
        }
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
#else
    (void)argv;
    panicf("list_dir is not supported on this platform");
    return A_NONE;
#endif
}

static aval bi_remove_file(uint64_t n, aval *argv) {
    want_args("remove_file", n, 1, 1);
    if (remove(path_arg(argv[0], "path")) != 0) panicf("cannot remove file '%s'", path_arg(argv[0], "path"));
    return A_NONE;
}

static aval bi_create_dir(uint64_t n, aval *argv) {
    want_args("create_dir", n, 1, 1);
#if !defined(_WIN32)
    const char *path = path_arg(argv[0], "path");
    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                panicf("cannot create directory '%s'", tmp);
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) { free(tmp); return A_NONE; }
        }
        panicf("cannot create directory '%s'", path);
    }
    free(tmp);
    return A_NONE;
#else
    (void)argv;
    panicf("create_dir is not supported on this platform");
    return A_NONE;
#endif
}

static int remove_tree(const char *path) {
#if !defined(_WIN32)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        int ok = 1;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            size_t need = strlen(path) + strlen(de->d_name) + 2;
            char *child = (char *)xmalloc(need);
            snprintf(child, need, "%s/%s", path, de->d_name);
            if (remove_tree(child) != 0) ok = 0;
            free(child);
            if (!ok) break;
        }
        closedir(d);
        if (!ok) return -1;
        return rmdir(path);
    }
    return remove(path);
#else
    (void)path;
    return -1;
#endif
}

static aval bi_remove_dir(uint64_t n, aval *argv) {
    want_args("remove_dir", n, 1, 1);
    if (remove_tree(path_arg(argv[0], "path")) != 0)
        panicf("cannot remove directory '%s'", path_arg(argv[0], "path"));
    return A_NONE;
}

static aval bi_rename(uint64_t n, aval *argv) {
    want_args("rename", n, 2, 2);
    if (rename(path_arg(argv[0], "old"), path_arg(argv[1], "new")) != 0)
        panicf("cannot rename '%s' to '%s'", path_arg(argv[0], "old"), path_arg(argv[1], "new"));
    return A_NONE;
}

static aval bi_current_dir(uint64_t n, aval *argv) {
    want_args("current_dir", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) panicf("cannot get current directory");
    return a_str_new(buf, strlen(buf));
#else
    panicf("current_dir is not supported on this platform");
    return A_NONE;
#endif
}

static aval bi_isinstance(uint64_t n, aval *argv) {
    want_args("isinstance", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("isinstance() arg 2 must be str");
    uint64_t len;
    const char *p = str_data(argv[1], &len);
    char *name = xstrndup(p, (size_t)len);
    int r = a_obj_is_instance(argv[0], name);
    free(name);
    return bool_aval(r);
}

static aval bi_join(uint64_t n, aval *argv) {
    want_args("join", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("join() separator must be str");
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    for (uint64_t i = 0; i < a->h.len; ++i)
        a->items[i] = a_str(a->items[i]);
    return a_str_joinsep(argv[1], a->h.len, a->items);
}

static aval bi_sorted(uint64_t n, aval *argv) {
    want_args("sorted", n, 1, 1);
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    sort_arr(a->items, a->h.len);
    return t;
}

static aval bi_reversed(uint64_t n, aval *argv) {
    want_args("reversed", n, 1, 1);
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = a->h.len; i > 0; --i) avec_push(&out, a->items[i - 1]);
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

static aval bi_enumerate(uint64_t n, aval *argv) {
    want_args("enumerate", n, 1, 2);
    int64_t start = n == 2 ? expect_int64(argv[1], "enumerate() start") : 0;
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval pair[2] = { mkint_full(start + (int64_t)i), a->items[i] };
        avec_push(&out, a_tuple_new(2, pair));
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

static aval bi_zip(uint64_t n, aval *argv) {
    if (n < 1) panicf("zip expected at least 1 argument, got 0");
    aval *cols = (aval *)xmalloc((size_t)n * sizeof(aval));
    uint64_t minlen = UINT64_MAX;
    for (uint64_t j = 0; j < n; ++j) {
        cols[j] = collect_iterable(argv[j]);
        uint64_t l = ((AArr *)obj_of(cols[j]))->h.len;
        if (l < minlen) minlen = l;
    }
    AVec out = { NULL, 0, 0 };
    aval *row = (aval *)xmalloc((size_t)n * sizeof(aval));
    for (uint64_t i = 0; i < minlen; ++i) {
        for (uint64_t j = 0; j < n; ++j)
            row[j] = ((AArr *)obj_of(cols[j]))->items[i];
        avec_push(&out, a_tuple_new(n, row));
    }
    free(row);
    free(cols);
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

static aval bi_log2(uint64_t n, aval *argv) { want_args("log2", n, 1, 1); double x = math_arg(argv[0], "log2"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log2(x)); }
static aval bi_log10(uint64_t n, aval *argv) { want_args("log10", n, 1, 1); double x = math_arg(argv[0], "log10"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log10(x)); }
static aval bi_atan2(uint64_t n, aval *argv) { want_args("atan2", n, 2, 2); return mkdouble(atan2(math_arg(argv[0], "atan2"), math_arg(argv[1], "atan2"))); }
static aval bi_flush(uint64_t n, aval *argv) { (void)argv; want_args("flush", n, 0, 0); fflush(stdout); return A_NONE; }
static aval bi_i8(uint64_t n, aval *argv) { want_args("i8", n, 1, 1); return mkint_full((int8_t)int_val_full(a_int(argv[0]))); }
static aval bi_i16(uint64_t n, aval *argv) { want_args("i16", n, 1, 1); return mkint_full((int16_t)int_val_full(a_int(argv[0]))); }
static aval bi_i32(uint64_t n, aval *argv) { want_args("i32", n, 1, 1); return mkint_full((int32_t)int_val_full(a_int(argv[0]))); }
static aval bi_u8(uint64_t n, aval *argv) { want_args("u8", n, 1, 1); return mkint_full((uint8_t)int_val_full(a_int(argv[0]))); }
static aval bi_u16(uint64_t n, aval *argv) { want_args("u16", n, 1, 1); return mkint_full((uint16_t)int_val_full(a_int(argv[0]))); }
static aval bi_u32(uint64_t n, aval *argv) { want_args("u32", n, 1, 1); return mkint_full((uint32_t)int_val_full(a_int(argv[0]))); }
static aval bi_f32(uint64_t n, aval *argv) { want_args("f32", n, 1, 1); return a_mkdouble((float)num_to_double(argv[0])); }

/* ---- LLVM-style intrinsics (bit ops, min/max, overflow, branch hints) -- */

/* Portable bit scans (the Astra backend has no __builtin_ctz family). */
static int a_ctz64(uint64_t x) {
    if (!x) return 64;
    int n = 0;
    if (!(x & UINT64_C(0xFFFFFFFF))) { n += 32; x >>= 32; }
    if (!(x & UINT64_C(0xFFFF))) { n += 16; x >>= 16; }
    if (!(x & UINT64_C(0xFF))) { n += 8; x >>= 8; }
    if (!(x & UINT64_C(0xF))) { n += 4; x >>= 4; }
    if (!(x & UINT64_C(0x3))) { n += 2; x >>= 2; }
    if (!(x & UINT64_C(0x1))) { n += 1; }
    return n;
}
static int a_clz64(uint64_t x) {
    if (!x) return 64;
    int n = 0;
    if (!(x & UINT64_C(0xFFFFFFFF00000000))) { n += 32; x <<= 32; }
    if (!(x & UINT64_C(0xFFFF000000000000))) { n += 16; x <<= 16; }
    if (!(x & UINT64_C(0xFF00000000000000))) { n += 8; x <<= 8; }
    if (!(x & UINT64_C(0xF000000000000000))) { n += 4; x <<= 4; }
    if (!(x & UINT64_C(0xC000000000000000))) { n += 2; x <<= 2; }
    if (!(x & UINT64_C(0x8000000000000000))) { n += 1; }
    return n;
}
static int a_popcount64(uint64_t x) {
    x -= (x >> 1) & UINT64_C(0x5555555555555555);
    x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
    x = (x + (x >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (int)((x * UINT64_C(0x0101010101010101)) >> 56);
}

static aval bi_ctz(uint64_t n, aval *argv) {
    want_args("ctz", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "ctz() argument");
    return mkint_full(a_ctz64(x));
}
static aval bi_clz(uint64_t n, aval *argv) {
    want_args("clz", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "clz() argument");
    return mkint_full(a_clz64(x));
}
static aval bi_popcount(uint64_t n, aval *argv) {
    want_args("popcount", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "popcount() argument");
    return mkint_full(a_popcount64(x));
}
static aval bi_bswap(uint64_t n, aval *argv) {
    want_args("bswap", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "bswap() argument");
    x = ((x & UINT64_C(0x00000000000000FF)) << 56) |
        ((x & UINT64_C(0x000000000000FF00)) << 40) |
        ((x & UINT64_C(0x0000000000FF0000)) << 24) |
        ((x & UINT64_C(0x00000000FF000000)) << 8) |
        ((x & UINT64_C(0x000000FF00000000)) >> 8) |
        ((x & UINT64_C(0x0000FF0000000000)) >> 24) |
        ((x & UINT64_C(0x00FF000000000000)) >> 40) |
        ((x & UINT64_C(0xFF00000000000000)) >> 56);
    return mkint_full((int64_t)x);
}
static aval bi_rotl(uint64_t n, aval *argv) {
    want_args("rotl", n, 2, 2);
    uint64_t x = (uint64_t)expect_int64(argv[0], "rotl() value");
    uint64_t k = (uint64_t)(expect_int64(argv[1], "rotl() amount") & 63);
    return mkint_full((int64_t)(k ? ((x << k) | (x >> (64 - k))) : x));
}
static aval bi_rotr(uint64_t n, aval *argv) {
    want_args("rotr", n, 2, 2);
    uint64_t x = (uint64_t)expect_int64(argv[0], "rotr() value");
    uint64_t k = (uint64_t)(expect_int64(argv[1], "rotr() amount") & 63);
    return mkint_full((int64_t)(k ? ((x >> k) | (x << (64 - k))) : x));
}

static aval bi_fma(uint64_t n, aval *argv) {
    want_args("fma", n, 3, 3);
    double a = math_arg(argv[0], "fma");
    double b = math_arg(argv[1], "fma");
    double c = math_arg(argv[2], "fma");
    return mkdouble(fma(a, b, c));
}
static aval bi_fabs(uint64_t n, aval *argv) {
    want_args("fabs", n, 1, 1);
    return mkdouble(fabs(math_arg(argv[0], "fabs")));
}
static aval bi_copysign(uint64_t n, aval *argv) {
    want_args("copysign", n, 2, 2);
    return mkdouble(copysign(math_arg(argv[0], "copysign"),
                            math_arg(argv[1], "copysign")));
}

static aval bi_smin(uint64_t n, aval *argv) {
    want_args("smin", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smin() argument");
    int64_t b = expect_int64(argv[1], "smin() argument");
    return mkint_full(a < b ? a : b);
}
static aval bi_smax(uint64_t n, aval *argv) {
    want_args("smax", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smax() argument");
    int64_t b = expect_int64(argv[1], "smax() argument");
    return mkint_full(a > b ? a : b);
}
static aval bi_umin(uint64_t n, aval *argv) {
    want_args("umin", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umin() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umin() argument");
    return mkint_full((int64_t)(a < b ? a : b));
}
static aval bi_umax(uint64_t n, aval *argv) {
    want_args("umax", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umax() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umax() argument");
    return mkint_full((int64_t)(a > b ? a : b));
}

/* Overflow builtins return (wrapped result, overflowed?) like the LLVM
   *.with.overflow intrinsics. */
static aval ovf_result(const char *name, int64_t r, int ovf) {
    aval pair[2];
    pair[0] = mkint_full(r);
    pair[1] = bool_aval(ovf);
    return a_tuple_new(2, pair);
}

static aval bi_sadd_ovf(uint64_t n, aval *argv) {
    want_args("sadd_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "sadd_overflow() argument");
    int64_t b = expect_int64(argv[1], "sadd_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a + (uint64_t)b);
    int ovf = (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
    return ovf_result("sadd_overflow", r, ovf);
}
static aval bi_ssub_ovf(uint64_t n, aval *argv) {
    want_args("ssub_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "ssub_overflow() argument");
    int64_t b = expect_int64(argv[1], "ssub_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a - (uint64_t)b);
    int ovf = (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
    return ovf_result("ssub_overflow", r, ovf);
}
static aval bi_smul_ovf(uint64_t n, aval *argv) {
    want_args("smul_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smul_overflow() argument");
    int64_t b = expect_int64(argv[1], "smul_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a * (uint64_t)b);
    int ovf;
    if (a == 0 || b == 0) ovf = 0;
    else if (a == -1) ovf = (b == INT64_MIN);
    else if (b == -1) ovf = (a == INT64_MIN);
    else {
        int64_t q = r / a;
        ovf = (q != b);
    }
    return ovf_result("smul_overflow", r, ovf);
}
static aval bi_uadd_ovf(uint64_t n, aval *argv) {
    want_args("uadd_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "uadd_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "uadd_overflow() argument");
    uint64_t r = a + b;
    return ovf_result("uadd_overflow", (int64_t)r, r < a);
}
static aval bi_usub_ovf(uint64_t n, aval *argv) {
    want_args("usub_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "usub_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "usub_overflow() argument");
    uint64_t r = a - b;
    return ovf_result("usub_overflow", (int64_t)r, r > a);
}
static aval bi_umul_ovf(uint64_t n, aval *argv) {
    want_args("umul_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umul_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umul_overflow() argument");
    uint64_t r = a * b;
    int ovf = (a != 0 && r / a != b) || (a == 0 && b != 0 && r != 0);
    return ovf_result("umul_overflow", (int64_t)r, ovf);
}

/* llvm.expect analog: a branch-hint wrapper; the runtime ignores the hint
   and returns the condition unchanged. */
static aval bi_expect(uint64_t n, aval *argv) {
    want_args("expect", n, 1, 2);
    (void)argv;
    return argv[0];
}
static aval bi_likely(uint64_t n, aval *argv) {
    want_args("likely", n, 1, 1);
    return argv[0];
}
static aval bi_unlikely(uint64_t n, aval *argv) {
    want_args("unlikely", n, 1, 1);
    return argv[0];
}
static aval bi_env_get(uint64_t n, aval *argv) {
    want_args("env_get", n, 1, 1);
    const char *name = path_arg(argv[0], "name");
    const char *v = getenv(name);
    if (!v) return A_NONE;
    return a_str_new(v, strlen(v));
}

/* ------------------------------------------------------------------ */
/* time module                                                         */
/* ------------------------------------------------------------------ */

static aval bi_time_now(uint64_t n, aval *argv) {
    (void)argv;
    want_args("now", n, 0, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return mkint_full((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static aval bi_time_sleep(uint64_t n, aval *argv) {
    want_args("sleep", n, 1, 1);
    int64_t ms = int_val_full(argv[0]);
    if (ms < 0) ms = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    return A_NONE;
}

/* ------------------------------------------------------------------ */
/* terminal module (POSIX TTY primitives used by the self-hosted REPL) */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
static struct termios g_repl_termios;
static int g_repl_termios_saved = 0;

static aval bi_terminal_isatty(uint64_t n, aval *argv) {
    (void)argv;
    want_args("isatty", n, 0, 0);
    return isatty(STDIN_FILENO) ? A_TRUE : A_FALSE;
}

static aval bi_terminal_raw_mode(uint64_t n, aval *argv) {
    want_args("raw_mode", n, 1, 1);
    int on = (argv[0] == A_TRUE);
    if (on) {
        if (tcgetattr(STDIN_FILENO, &g_repl_termios) != 0) return A_FALSE;
        g_repl_termios_saved = 1;
        struct termios raw = g_repl_termios;
        cfmakeraw(&raw);
        /* Keep output post-processing (NL -> CR-NL) enabled: the REPL and any
         * child processes it spawns (repl.eval) share this tty, and without
         * OPOST every '\n' they print staircases instead of returning to
         * column 0. Input stays raw. */
        raw.c_oflag |= OPOST;
        return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0 ? A_TRUE : A_FALSE;
    }
    if (!g_repl_termios_saved) return A_TRUE;
    int ok = tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_repl_termios) == 0;
    g_repl_termios_saved = 0;
    return ok ? A_TRUE : A_FALSE;
}

static aval bi_terminal_read_char(uint64_t n, aval *argv) {
    (void)argv;
    want_args("read_char", n, 0, 0);
    char c;
    ssize_t r;
    do { r = read(STDIN_FILENO, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

static aval bi_terminal_read_char_ms(uint64_t n, aval *argv) {
    want_args("read_char_ms", n, 1, 1);
    int64_t ms = int_val_full(argv[0]);
    if (ms < 0) ms = 0;
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int pr;
    do { pr = poll(&pfd, 1, (int)ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) return A_NONE;
    char c;
    ssize_t r;
    do { r = read(STDIN_FILENO, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

static aval bi_terminal_write(uint64_t n, aval *argv) {
    want_args("write", n, 1, 1);
    uint64_t len;
    const char *p = str_data(argv[0], &len);
    if (len && fwrite(p, 1, (size_t)len, stdout) != len)
        panicf("terminal.write: write failed");
    return A_NONE;
}

static aval bi_terminal_flush(uint64_t n, aval *argv) {
    (void)argv;
    want_args("flush", n, 0, 0);
    fflush(stdout);
    return A_NONE;
}

static aval bi_terminal_size(uint64_t n, aval *argv) {
    (void)argv;
    want_args("size", n, 0, 0);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    aval arr[2];
    arr[0] = a_mkint((int64_t)ws.ws_col);
    arr[1] = a_mkint((int64_t)ws.ws_row);
    return list_from_vec(arr, 2, 0);
}

static aval bi_terminal_home(uint64_t n, aval *argv) {
    (void)argv;
    want_args("home", n, 0, 0);
    const char *h = getenv("HOME");
    if (!h || !*h) h = ".";
    return a_str_new(h, strlen(h));
}
#else
static aval bi_terminal_unsupported(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("terminal module is not supported on this platform");
    return A_NONE;
}
#define bi_terminal_isatty bi_terminal_unsupported
#define bi_terminal_raw_mode bi_terminal_unsupported
#define bi_terminal_read_char bi_terminal_unsupported
#define bi_terminal_read_char_ms bi_terminal_unsupported
#define bi_terminal_write bi_terminal_unsupported
#define bi_terminal_flush bi_terminal_unsupported
#define bi_terminal_size bi_terminal_unsupported
#define bi_terminal_home bi_terminal_unsupported
#endif

/* ------------------------------------------------------------------ */
/* repl.eval: run A source through the `a` toolchain as a child process. */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
static aval bi_repl_eval(uint64_t n, aval *argv) {
    want_args("repl_eval", n, 1, 1);
    if (kind_of(argv[0]) != K_STR) panicf("repl.eval() source must be str");
    /* Nested evaluation is rejected: the child `a run` inherits
       A_REPL_DEPTH, so a repl.eval inside evaluated code returns 3. */
    const char *depth = getenv("A_REPL_DEPTH");
    if (depth && *depth) return a_mkint(3);
    const char *aBin = getenv("A_A_BIN");
    if (!aBin || !*aBin) aBin = "a";
    uint64_t len;
    const char *src = str_data(argv[0], &len);
    char tmpl[] = "/tmp/a_repl_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) panicf("repl.eval: cannot create temp file");
    ssize_t wr = write(fd, src, (size_t)len);
    close(fd);
    if (wr != (ssize_t)len) {
        unlink(tmpl);
        panicf("repl.eval: temp file write failed");
    }
    setenv("A_REPL_DEPTH", "1", 1);
    pid_t pid = fork();
    if (pid == 0) {
        execl(aBin, "a", "run", tmpl, (char *)NULL);
        execlp("a", "a", "run", tmpl, (char *)NULL);
        _exit(127);
    }
    unsetenv("A_REPL_DEPTH");
    if (pid < 0) {
        unlink(tmpl);
        panicf("repl.eval: fork failed");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    unlink(tmpl);
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc == 0) return a_mkint(0);
        if (rc == 1) return a_mkint(1);   /* compile error (kCErr) */
        return a_mkint(2);                /* runtime error / signal death */
    }
    return a_mkint(2);
}
#else
static aval bi_repl_eval(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("repl.eval is not supported on this platform");
    return A_NONE;
}
#endif

/* repl.quiet(on): while set, print/eprint discard their output. The A REPL
 * wraps replayed session source in quiet(true)/quiet(false) so re-executed
 * history does not repeat its side-effect output. */
static int g_repl_quiet = 0;

static aval bi_repl_quiet(uint64_t n, aval *argv) {
    want_args("repl_quiet", n, 1, 1);
    g_repl_quiet = (argv[0] == A_TRUE) ? 1 : 0;
    return A_NONE;
}

/* ------------------------------------------------------------------ */
/* json module: JSON text <-> aval values.                              */
/* JSON objects map to dicts, arrays to lists, strings to str, numbers  */
/* to int/float, true/false to bool, null to None.                      */
/* ------------------------------------------------------------------ */

static aval json_unwrap(aval v) {
    AObj *o = obj_of(v);
    if (o && o->type == T_RESULT && ((AResult *)o)->is_option)
        return ((AResult *)o)->value;
    return v;
}

static const char *json_kind_of(aval v) {
    v = json_unwrap(v);
    if (a_is_none(v)) return "null";
    if (v == A_TRUE || v == A_FALSE) return "bool";
    if (is_int_val(v) || a_is_double(v)) return "number";
    AObj *o = obj_of(v);
    if (!o) return "unknown";
    switch (o->type) {
        case T_STR: case T_CHAR: return "string";
        case T_LIST: case T_TUPLE: case T_SET: return "array";
        case T_DICT: return "object";
        default: return "unknown";
    }
}

typedef struct { const char *p, *end; int err; } JParser;

static void jws(JParser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ++ps->p;
}
static int jpeek(JParser *ps) {
    jws(ps);
    return ps->p < ps->end ? (unsigned char)*ps->p : -1;
}
static int jexpect(JParser *ps, char c) {
    if (jpeek(ps) == c) { ++ps->p; return 1; }
    ps->err = 1;
    return 0;
}

static void jutf8(Buf *b, uint32_t cp) {
    if (cp < 0x80) {
        buf_addc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_addc(b, (char)(0xC0 | (cp >> 6)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_addc(b, (char)(0xE0 | (cp >> 12)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_addc(b, (char)(0xF0 | (cp >> 18)));
        buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int jhex4(JParser *ps) {
    if (ps->end - ps->p < 4) { ps->err = 1; return 0; }
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else { ps->err = 1; return 0; }
    }
    return v;
}

static aval jstring(JParser *ps) {
    if (!jexpect(ps, '"')) return A_NONE;
    Buf b;
    buf_init(&b);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') return buf_take(&b);
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case '"': buf_addc(&b, '"'); break;
                case '\\': buf_addc(&b, '\\'); break;
                case '/': buf_addc(&b, '/'); break;
                case 'b': buf_addc(&b, '\b'); break;
                case 'f': buf_addc(&b, '\f'); break;
                case 'n': buf_addc(&b, '\n'); break;
                case 'r': buf_addc(&b, '\r'); break;
                case 't': buf_addc(&b, '\t'); break;
                case 'u': {
                    uint32_t cp = (uint32_t)jhex4(ps);
                    if (ps->err) break;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        uint32_t lo = (uint32_t)jhex4(ps);
                        if (ps->err) break;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    jutf8(&b, cp);
                    break;
                }
                default: ps->err = 1; break;
            }
        } else {
            buf_addc(&b, (char)c);
        }
        if (ps->err) break;
    }
    free(b.p);
    ps->err = 1;
    return A_NONE;
}

static aval jnumber(JParser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ++ps->p;
    int isDouble = 0;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    if (ps->p < ps->end && *ps->p == '.') {
        isDouble = 1;
        ++ps->p;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        isDouble = 1;
        ++ps->p;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ++ps->p;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    }
    if (ps->p == start || (isDouble == 0 && ps->p == start + 1 && *start == '-')) {
        ps->err = 1;
        return A_NONE;
    }
    char tmp[64];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof(tmp)) { ps->err = 1; return A_NONE; }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (isDouble) return mkdouble(strtod(tmp, NULL));
    errno = 0;
    long long v = strtoll(tmp, NULL, 10);
    if (errno == ERANGE) return mkdouble(strtod(tmp, NULL));
    return mkint_full(v);
}

static aval jvalue(JParser *ps);

static aval jobject(JParser *ps) {
    jexpect(ps, '{');
    aval d = a_dict_new();
    if (jpeek(ps) == '}') { ++ps->p; return d; }
    for (;;) {
        if (jpeek(ps) != '"') { ps->err = 1; break; }
        aval k = jstring(ps);
        if (ps->err) break;
        if (!jexpect(ps, ':')) break;
        aval v = jvalue(ps);
        if (ps->err) break;
        dict_put(dict_of(d, "json"), k, v);
        int c = jpeek(ps);
        if (c == ',') { ++ps->p; continue; }
        if (c == '}') { ++ps->p; break; }
        ps->err = 1;
        break;
    }
    if (ps->err) return A_NONE;
    return d;
}

static aval jarray(JParser *ps) {
    jexpect(ps, '[');
    aval a = a_list_new(0, NULL);
    AArr *arr = arr_of(a, "json");
    if (jpeek(ps) == ']') { ++ps->p; return a; }
    for (;;) {
        aval v = jvalue(ps);
        if (ps->err) return A_NONE;
        arr_push(arr, v);
        int c = jpeek(ps);
        if (c == ',') { ++ps->p; continue; }
        if (c == ']') { ++ps->p; break; }
        ps->err = 1;
        break;
    }
    return ps->err ? A_NONE : a;
}

static aval jvalue(JParser *ps) {
    int c = jpeek(ps);
    if (c < 0) { ps->err = 1; return A_NONE; }
    if (c == '"') return jstring(ps);
    if (c == '{') return jobject(ps);
    if (c == '[') return jarray(ps);
    if (c == 't') {
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
            ps->p += 4; jws(ps); return A_TRUE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == 'f') {
        if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
            ps->p += 5; jws(ps); return A_FALSE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == 'n') {
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
            ps->p += 4; jws(ps); return A_NONE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jnumber(ps);
    ps->err = 1;
    return A_NONE;
}

/* ---- JSON serializer ---- */

static void json_escape_str(Buf *b, aval v) {
    uint64_t n;
    const char *s = str_data(v, &n);
    buf_addc(b, '"');
    for (uint64_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\b': buf_adds(b, "\\b"); break;
            case '\f': buf_adds(b, "\\f"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\r': buf_adds(b, "\\r"); break;
            case '\t': buf_adds(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    buf_adds(b, tmp);
                } else {
                    buf_addc(b, (char)c);
                }
        }
    }
    buf_addc(b, '"');
}

static void json_write(Buf *b, aval v, int pretty, int depth);

static void json_indent(Buf *b, int pretty, int depth) {
    if (!pretty) return;
    buf_addc(b, '\n');
    for (int i = 0; i < depth; ++i) buf_adds(b, "  ");
}

static void json_write(Buf *b, aval v, int pretty, int depth) {
    v = json_unwrap(v);
    if (a_is_none(v)) { buf_adds(b, "null"); return; }
    if (v == A_TRUE) { buf_adds(b, "true"); return; }
    if (v == A_FALSE) { buf_adds(b, "false"); return; }
    if (is_int_val(v)) { buf_addi64(b, int_val_full(v)); return; }
    if (a_is_double(v)) {
        double d = a_double_val(v);
        if (!isfinite(d)) { buf_adds(b, "null"); return; }
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        buf_adds(b, tmp);
        return;
    }
    AObj *o = obj_of(v);
    if (!o) panicf("json: value is not serializable");
    switch (o->type) {
        case T_STR: case T_CHAR:
            json_escape_str(b, v);
            return;
        case T_LIST: case T_TUPLE: case T_SET: {
            AArr *a = (AArr *)o;
            buf_addc(b, '[');
            for (uint32_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, pretty ? "," : ", ");
                json_indent(b, pretty, depth + 1);
                json_write(b, a->items[i], pretty, depth + 1);
            }
            if (a->h.len) json_indent(b, pretty, depth);
            buf_addc(b, ']');
            return;
        }
        case T_DICT: {
            ADict *d = (ADict *)o;
            buf_addc(b, '{');
            for (uint32_t i = 0; i < d->h.len; ++i) {
                if (i) buf_adds(b, pretty ? "," : ", ");
                json_indent(b, pretty, depth + 1);
                aval k = d->pairs[i].k;
                if (kind_of(k) == K_STR || kind_of(k) == K_CHAR) {
                    json_escape_str(b, k);
                } else {
                    aval ks = a_str(k);
                    json_escape_str(b, ks);
                }
                buf_adds(b, pretty ? ": " : ": ");
                json_write(b, d->pairs[i].v, pretty, depth + 1);
            }
            if (d->h.len) json_indent(b, pretty, depth);
            buf_addc(b, '}');
            return;
        }
        default:
            panicf("json: value of type '%s' is not serializable", type_name_cstr(v));
    }
}

static aval bi_json_dump(uint64_t n, aval *argv, int pretty) {
    want_args(pretty ? "json.pretty" : "json.stringify", n, 1, 1);
    Buf b;
    buf_init(&b);
    json_write(&b, argv[0], pretty, 0);
    return buf_take(&b);
}
static aval bi_json_stringify(uint64_t n, aval *argv) { return bi_json_dump(n, argv, 0); }
static aval bi_json_pretty(uint64_t n, aval *argv) { return bi_json_dump(n, argv, 1); }

static aval bi_json_parse(uint64_t n, aval *argv) {
    want_args("json.parse", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    JParser ps = { s, s + len, 0 };
    aval v = jvalue(&ps);
    jws(&ps);
    if (ps.err || ps.p != ps.end)
        panicf("json.parse: invalid JSON");
    return v;
}

static aval bi_json_is_valid(uint64_t n, aval *argv) {
    want_args("json.is_valid", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    JParser ps = { s, s + len, 0 };
    (void)jvalue(&ps);
    jws(&ps);
    return bool_aval(!ps.err && ps.p == ps.end);
}

static aval bi_json_kind(uint64_t n, aval *argv) {
    want_args("json.kind", n, 1, 1);
    return a_str_new(json_kind_of(argv[0]), strlen(json_kind_of(argv[0])));
}

static aval bi_json_get(uint64_t n, aval *argv) {
    want_args("json.get", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || o->type != T_DICT) return A_NONE;
    int64_t i = dict_find((ADict *)o, argv[1]);
    return i >= 0 ? a_some_new(((ADict *)o)->pairs[i].v) : A_NONE;
}

static aval bi_json_at(uint64_t n, aval *argv) {
    want_args("json.at", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE)) return A_NONE;
    AArr *a = (AArr *)o;
    int64_t idx = expect_int64(argv[1], "json.at index");
    if (idx < 0) idx += (int64_t)a->h.len;
    if (idx < 0 || (uint64_t)idx >= a->h.len) return A_NONE;
    return a_some_new(a->items[idx]);
}

static aval bi_json_len(uint64_t n, aval *argv) {
    want_args("json.len", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET ||
              o->type == T_DICT))
        return mkint_full((int64_t)o->len);
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        uint64_t sl;
        const char *sp = str_data(v, &sl);
        return mkint_full((int64_t)u8_count(sp, sl));
    }
    return mkint_full(0);
}

static aval bi_json_has(uint64_t n, aval *argv) {
    want_args("json.has", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || o->type != T_DICT) return A_FALSE;
    return bool_aval(dict_find((ADict *)o, argv[1]) >= 0);
}

static aval bi_json_keys(uint64_t n, aval *argv) {
    want_args("json.keys", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    aval out = a_list_new(0, NULL);
    if (o && o->type == T_DICT) {
        ADict *d = (ADict *)o;
        AArr *arr = arr_of(out, "json.keys");
        for (uint32_t i = 0; i < d->h.len; ++i) {
            aval k = d->pairs[i].k;
            if (kind_of(k) != K_STR && kind_of(k) != K_CHAR) k = a_str(k);
            arr_push(arr, k);
        }
    }
    return out;
}

static aval bi_json_as_str(uint64_t n, aval *argv) {
    want_args("json.as_str", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_STR || o->type == T_CHAR)) return a_some_new(v);
    return A_NONE;
}

static aval bi_json_as_int(uint64_t n, aval *argv) {
    want_args("json.as_int", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (is_int_val(v)) return a_some_new(mkint_full(int_val_full(v)));
    if (a_is_double(v)) {
        double d = a_double_val(v);
        if (isfinite(d) && d == floor(d) && d >= -9.0e18 && d <= 9.0e18)
            return a_some_new(mkint_full((int64_t)d));
    }
    return A_NONE;
}

static aval bi_json_as_float(uint64_t n, aval *argv) {
    want_args("json.as_float", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (is_int_val(v)) return a_some_new(mkdouble((double)int_val_full(v)));
    if (a_is_double(v)) return a_some_new(v);
    return A_NONE;
}

static aval bi_json_as_bool(uint64_t n, aval *argv) {
    want_args("json.as_bool", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (v == A_TRUE || v == A_FALSE) return a_some_new(v);
    return A_NONE;
}

static aval bi_json_as_array(uint64_t n, aval *argv) {
    want_args("json.as_array", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET))
        return a_some_new(v);
    return A_NONE;
}

static aval bi_json_as_map(uint64_t n, aval *argv) {
    want_args("json.as_map", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && o->type == T_DICT) return a_some_new(v);
    return A_NONE;
}

/* ------------------------------------------------------------------ */
/* binary module: tagged-value <-> byte-array codec                    */
/* ------------------------------------------------------------------ */

#define BIN_NULL 0
#define BIN_FALSE 1
#define BIN_TRUE 2
#define BIN_INT 3
#define BIN_FLOAT 4
#define BIN_STRING 5
#define BIN_ARRAY 6
#define BIN_MAP 7
#define BIN_STRUCT 8
#define BIN_SET 9
#define BIN_NONE 10
#define BIN_SOME 11
#define BIN_TUPLE 12
#define BIN_CHAR 13
#define BIN_OK 14
#define BIN_ERR 15

typedef struct { uint8_t *p; size_t len, cap; int err; } BinBuf;

static void bin_put(BinBuf *b, uint8_t v) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = (uint8_t *)xrealloc(b->p, b->cap);
    }
    b->p[b->len++] = v;
}

static void bin_varint(BinBuf *b, uint64_t v) {
    while (v >= 0x80) { bin_put(b, (uint8_t)(v | 0x80)); v >>= 7; }
    bin_put(b, (uint8_t)v);
}

static void bin_str(BinBuf *b, const char *s, size_t n) {
    bin_varint(b, n);
    for (size_t i = 0; i < n; ++i) bin_put(b, (uint8_t)s[i]);
}

static void bin_encode_val(BinBuf *b, aval v);

static void bin_encode_bytes(BinBuf *b, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; ++i) bin_put(b, p[i]);
}

static int utf8_decode_cp(const char *p, uint64_t n, uint64_t *cp) {
    if (!n) return 0;
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int need = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3
             : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!need || (uint64_t)need > n) return 0;
    uint64_t c = s[0] & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    for (int i = 1; i < need; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return need;
}

static int utf8_encode_cp(uint64_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void bin_encode_val(BinBuf *b, aval v) {
    switch (kind_of(v)) {
    case K_NONE: bin_put(b, BIN_NULL); return;
    case K_BOOL: bin_put(b, v == A_TRUE ? BIN_TRUE : BIN_FALSE); return;
    case K_INT: {
        bin_put(b, BIN_INT);
        int64_t i = int_val_full(v);
        uint64_t z = ((uint64_t)i << 1) ^ (uint64_t)(i >> 63);
        bin_varint(b, z);
        return;
    }
    case K_FLOAT: {
        bin_put(b, BIN_FLOAT);
        double d = a_double_val(v);
        uint64_t u;
        memcpy(&u, &d, 8);
        for (int i = 0; i < 8; ++i) bin_put(b, (uint8_t)(u >> (i * 8)));
        return;
    }
    case K_STR: case K_CHAR: {
        uint64_t n;
        const char *p = str_data(v, &n);
        if (kind_of(v) == K_CHAR) {
            uint64_t cp = 0;
            if (utf8_decode_cp(p, n, &cp)) {
                bin_put(b, BIN_CHAR);
                bin_varint(b, cp);
                return;
            }
        }
        bin_put(b, BIN_STRING);
        bin_str(b, p, (size_t)n);
        return;
    }
    case K_LIST: case K_TUPLE: case K_SET: {
        AArr *a = (AArr *)obj_of(v);
        bin_put(b, kind_of(v) == K_TUPLE ? BIN_TUPLE
                    : kind_of(v) == K_SET ? BIN_SET : BIN_ARRAY);
        bin_varint(b, a->h.len);
        for (uint32_t i = 0; i < a->h.len; ++i) bin_encode_val(b, a->items[i]);
        return;
    }
    case K_DICT: {
        ADict *d = (ADict *)obj_of(v);
        /* Structs are dicts carrying a "__name" string: encode as BIN_STRUCT. */
        uint64_t nl;
        const char *np = NULL;
        int64_t ni = dict_find(d, a_str_new("__name", 6));
        aval tn = A_NONE;
        if (ni >= 0) {
            aval tv = d->pairs[ni].v;
            if (kind_of(tv) == K_STR) { np = str_data(tv, &nl); tn = tv; }
        }
        if (tn != A_NONE) {
            bin_put(b, BIN_STRUCT);
            bin_str(b, np, (size_t)nl);
            bin_varint(b, d->h.len - 1);
            for (uint32_t i = 0; i < d->h.len; ++i) {
                if ((int64_t)i == ni) continue;
                uint64_t kl;
                const char *kp = str_data(d->pairs[i].k, &kl);
                bin_str(b, kp, (size_t)kl);
                bin_encode_val(b, d->pairs[i].v);
            }
            return;
        }
        bin_put(b, BIN_MAP);
        bin_varint(b, d->h.len);
        for (uint32_t i = 0; i < d->h.len; ++i) {
            bin_encode_val(b, d->pairs[i].k);
            bin_encode_val(b, d->pairs[i].v);
        }
        return;
    }
    case K_RESULT: {
        AResult *r = (AResult *)obj_of(v);
        if (r->is_option) {
            if (r->ok) { bin_put(b, BIN_SOME); bin_encode_val(b, r->value); }
            else bin_put(b, BIN_NONE);
        } else {
            bin_put(b, r->ok ? BIN_OK : BIN_ERR);
            bin_encode_val(b, r->value);
        }
        return;
    }
    default:
        panicf("binary.encode: cannot encode '%s'", type_name_cstr(v));
    }
}

typedef struct { const uint8_t *p; size_t len, pos; int err; } BinReader;

static int bin_eof(BinReader *r) { return r->pos >= r->len; }
static uint8_t bin_byte(BinReader *r) {
    if (bin_eof(r)) { r->err = 1; return 0; }
    return r->p[r->pos++];
}
static uint64_t bin_varint_rd(BinReader *r) {
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        uint8_t c = bin_byte(r);
        if (r->err) return 0;
        v |= (uint64_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
        shift += 7;
        if (shift > 63) { r->err = 1; return 0; }
    }
}
static aval bin_string_rd(BinReader *r) {
    uint64_t n = bin_varint_rd(r);
    if (r->err || n > r->len - r->pos) { r->err = 1; return A_NONE; }
    aval s = a_str_new((const char *)r->p + r->pos, (uint64_t)n);
    r->pos += (size_t)n;
    return s;
}

static aval bin_decode_val(BinReader *r);

static aval dict_new_empty(void) {
    ADict *d = (ADict *)xmalloc(sizeof(ADict));
    d->h.type = T_DICT;
    d->h.len = 0;
    d->h.cap = 0;
    d->pairs = NULL;
    return mkobj(&d->h);
}

static aval bin_decode_val(BinReader *r) {
    uint8_t tag = bin_byte(r);
    if (r->err) return A_NONE;
    switch (tag) {
    case BIN_NULL: return A_NONE;
    case BIN_FALSE: return A_FALSE;
    case BIN_TRUE: return A_TRUE;
    case BIN_INT: {
        uint64_t z = bin_varint_rd(r);
        if (r->err) return A_NONE;
        return mkint_full((int64_t)((z >> 1) ^ (~(z & 1) + 1)));
    }
    case BIN_FLOAT: {
        if (r->len - r->pos < 8) { r->err = 1; return A_NONE; }
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= (uint64_t)bin_byte(r) << (i * 8);
        double d;
        memcpy(&d, &u, 8);
        return a_mkdouble(d);
    }
    case BIN_STRING: return bin_string_rd(r);
    case BIN_CHAR: {
        uint64_t cp = bin_varint_rd(r);
        if (r->err || cp > 0x10FFFF) { r->err = 1; return A_NONE; }
        char buf[4];
        int n = utf8_encode_cp(cp, buf);
        return a_char_new(buf, (uint64_t)n);
    }
    case BIN_ARRAY: case BIN_SET: case BIN_TUPLE: {
        uint64_t n = bin_varint_rd(r);
        if (r->err) return A_NONE;
        aval *items = (aval *)xmalloc(sizeof(aval) * (n ? n : 1));
        for (uint64_t i = 0; i < n; ++i) {
            items[i] = bin_decode_val(r);
            if (r->err) { free(items); return A_NONE; }
        }
        aval out = list_from_vec(items, (uint64_t)n, tag == BIN_TUPLE);
        free(items);
        return out;
    }
    case BIN_MAP: {
        uint64_t n = bin_varint_rd(r);
        if (r->err) return A_NONE;
        aval d = dict_new_empty();
        ADict *dd = (ADict *)obj_of(d);
        for (uint64_t i = 0; i < n; ++i) {
            aval k = bin_decode_val(r);
            aval v = bin_decode_val(r);
            if (r->err) return A_NONE;
            dict_put(dd, k, v);
        }
        return d;
    }
    case BIN_STRUCT: {
        aval tn = bin_string_rd(r);
        if (r->err) return A_NONE;
        uint64_t n = bin_varint_rd(r);
        if (r->err) return A_NONE;
        aval d = dict_new_empty();
        ADict *dd = (ADict *)obj_of(d);
        dict_put(dd, a_str_new("$type", 5), tn);
        for (uint64_t i = 0; i < n; ++i) {
            aval k = bin_string_rd(r);
            aval v = bin_decode_val(r);
            if (r->err) return A_NONE;
            dict_put(dd, k, v);
        }
        return d;
    }
    case BIN_NONE: return A_NONE;
    case BIN_SOME: {
        aval v = bin_decode_val(r);
        return r->err ? A_NONE : a_some_new(v);
    }
    case BIN_OK: case BIN_ERR: {
        aval v = bin_decode_val(r);
        if (r->err) return A_NONE;
        AResult *res = result_new(tag == BIN_OK, v);
        return mkobj(&res->h);
    }
    default: r->err = 1; return A_NONE;
    }
}

static aval bi_binary_encode(uint64_t n, aval *argv) {
    want_args("binary.encode", n, 1, 1);
    BinBuf b = {0};
    bin_encode_val(&b, argv[0]);
    aval *items = (aval *)xmalloc(sizeof(aval) * (b.len ? b.len : 1));
    for (size_t i = 0; i < b.len; ++i) items[i] = a_mkint(b.p[i]);
    aval out = list_from_vec(items, (uint64_t)b.len, 0);
    free(items);
    free(b.p);
    return out;
}

static aval bi_binary_decode(uint64_t n, aval *argv) {
    want_args("binary.decode", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE && o->type != T_SET))
        panicf("'binary.decode' expects an array of byte values (0-255)");
    AArr *a = (AArr *)o;
    BinReader r;
    r.p = (const uint8_t *)xmalloc(a->h.len ? a->h.len : 1);
    r.len = r.pos = 0;
    r.err = 0;
    for (uint32_t i = 0; i < a->h.len; ++i) {
        int64_t byte = int_val_full(a->items[i]);
        if (byte < 0 || byte > 255) {
            free((void *)r.p);
            panicf("'binary.decode' expects an array of byte values (0-255)");
        }
        r.p[r.len++] = (uint8_t)byte;
    }
    aval v = bin_decode_val(&r);
    int ok = !r.err && r.pos == r.len;
    free((void *)r.p);
    return ok ? a_some_new(v) : A_NONE;
}

/* ------------------------------------------------------------------ */
/* log module: leveled logging                                         */
/* ------------------------------------------------------------------ */

static int g_log_level = 1;   /* 0=debug 1=info 2=warn 3=error */
static int g_log_json = 0;
static const char *const k_log_level_names[] = {"debug", "info", "warn", "error"};

static void log_write_line(int level, const char *msg, uint64_t mlen, aval fields) {
    FILE *f = stderr;
    if (g_log_json) {
        Buf b;
        buf_init(&b);
        buf_addc(&b, '{');
        buf_adds(&b, "\"level\":\"");
        buf_adds(&b, k_log_level_names[level]);
        buf_adds(&b, "\",\"msg\":");
        /* reuse the JSON string escaper via format_into on the raw string */
        aval ms = a_str_new(msg, mlen);
        format_into(&b, ms, 0);
        if (fields != A_NONE) {
            buf_adds(&b, ",\"fields\":");
            format_into(&b, fields, 0);
        }
        buf_adds(&b, "}\n");
        fwrite(b.p, 1, (size_t)b.len, f);
        free(b.p);
        return;
    }
    fprintf(f, "%s %.*s", k_log_level_names[level], (int)mlen, msg);
    if (fields != A_NONE) {
        Buf b;
        buf_init(&b);
        format_into(&b, fields, 0);
        fprintf(f, " %s", b.p ? b.p : "");
        free(b.p);
    }
    fputc('\n', f);
}

static aval bi_log_set_level(uint64_t n, aval *argv) {
    want_args("log.set_level", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    for (int i = 0; i < 4; ++i) {
        if (strlen(k_log_level_names[i]) == len && !memcmp(s, k_log_level_names[i], len)) {
            g_log_level = i;
            return A_NONE;
        }
    }
    panicf("log.set_level: unknown level '%.*s'", (int)len, s);
    return A_NONE;
}

static aval bi_log_set_json(uint64_t n, aval *argv) {
    want_args("log.set_json", n, 1, 1);
    g_log_json = kind_of(argv[0]) == K_BOOL ? (argv[0] == A_TRUE) : (int_val_full(argv[0]) != 0);
    return A_NONE;
}

static aval bi_log_emit(int level, uint64_t n, aval *argv) {
    want_args(k_log_level_names[level], n, 1, 2);
    if (level < g_log_level) return A_NONE;
    uint64_t mlen;
    const char *msg = str_data(argv[0], &mlen);
    aval fields = n > 1 ? argv[1] : A_NONE;
    log_write_line(level, msg, mlen, fields);
    return A_NONE;
}
static aval bi_log_debug(uint64_t n, aval *argv) { return bi_log_emit(0, n, argv); }
static aval bi_log_info(uint64_t n, aval *argv) { return bi_log_emit(1, n, argv); }
static aval bi_log_warn(uint64_t n, aval *argv) { return bi_log_emit(2, n, argv); }
static aval bi_log_error(uint64_t n, aval *argv) { return bi_log_emit(3, n, argv); }

/* ------------------------------------------------------------------ */
/* metrics module                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;
    int kind;              /* 0=counter 1=gauge 2=histogram */
    double value;          /* gauge: last value; counter/hist: sum */
    uint64_t count;
    double min, max;
} AMetric;

static AMetric *g_metrics;
static size_t g_nmetrics, g_capmetrics;

static char *metric_key(aval name, aval labels) {
    Buf b;
    buf_init(&b);
    uint64_t nl;
    const char *np = str_data(name, &nl);
    buf_addn(&b, np, (size_t)nl);
    if (labels != A_NONE) {
        buf_addc(&b, '{');
        format_into(&b, labels, 0);
        buf_addc(&b, '}');
    }
    char *k = xstrdup(b.p ? b.p : "");
    free(b.p);
    return k;
}

static AMetric *metric_find(const char *key, int create, int kind) {
    for (size_t i = 0; i < g_nmetrics; ++i)
        if (!strcmp(g_metrics[i].key, key)) return &g_metrics[i];
    if (!create) return NULL;
    if (g_nmetrics == g_capmetrics) {
        g_capmetrics = g_capmetrics ? g_capmetrics * 2 : 16;
        g_metrics = (AMetric *)xrealloc(g_metrics, g_capmetrics * sizeof(AMetric));
    }
    AMetric *m = &g_metrics[g_nmetrics++];
    m->key = xstrdup(key);
    m->kind = kind;
    m->value = 0;
    m->count = 0;
    m->min = 0;
    m->max = 0;
    return m;
}

static aval bi_metrics_counter(uint64_t n, aval *argv) {
    want_args("metrics.counter", n, 1, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 0);
    free(key);
    m->value += n > 1 ? num_to_double(argv[1]) : 1.0;
    m->count++;
    return A_NONE;
}

static aval bi_metrics_gauge(uint64_t n, aval *argv) {
    want_args("metrics.gauge", n, 2, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 1);
    free(key);
    m->value = num_to_double(argv[1]);
    return A_NONE;
}

static aval bi_metrics_histogram(uint64_t n, aval *argv) {
    want_args("metrics.histogram", n, 2, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 2);
    free(key);
    double v = num_to_double(argv[1]);
    if (!m->count || v < m->min) m->min = v;
    if (!m->count || v > m->max) m->max = v;
    m->value += v;
    m->count++;
    return A_NONE;
}

static aval bi_metrics_render(uint64_t n, aval *argv) {
    want_args("metrics.render", n, 0, 0);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < g_nmetrics; ++i) {
        AMetric *m = &g_metrics[i];
        buf_adds(&b, m->key);
        buf_adds(&b, " ");
        if (m->kind == 2 && m->count)
            buf_addi64(&b, (int64_t)(m->value / (double)m->count));
        else if (m->kind == 0)
            buf_addi64(&b, (int64_t)m->value);
        else {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.6g", m->value);
            buf_adds(&b, tmp);
        }
        buf_addc(&b, '\n');
    }
    aval out = buf_take(&b);
    return out;
}

static aval bi_metrics_reset(uint64_t n, aval *argv) {
    want_args("metrics.reset", n, 0, 0);
    for (size_t i = 0; i < g_nmetrics; ++i) free(g_metrics[i].key);
    free(g_metrics);
    g_metrics = NULL;
    g_nmetrics = g_capmetrics = 0;
    return A_NONE;
}

/* ------------------------------------------------------------------ */
/* trace module                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t id;
    aval name;
    aval attrs;    /* dict */
    aval events;   /* list */
    int64_t start_ms;
    int64_t end_ms;
} ASpan;

static ASpan *g_spans;
static size_t g_nspans, g_capspans;
static uint64_t g_next_span = 1;
static uint64_t g_current_span = 0;

static ASpan *span_find(uint64_t id) {
    for (size_t i = g_nspans; i-- > 0;)
        if (g_spans[i].id == id) return &g_spans[i];
    return NULL;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static aval bi_trace_start(uint64_t n, aval *argv) {
    want_args("trace.start", n, 1, 2);
    if (g_nspans == g_capspans) {
        g_capspans = g_capspans ? g_capspans * 2 : 16;
        g_spans = (ASpan *)xrealloc(g_spans, g_capspans * sizeof(ASpan));
    }
    ASpan *s = &g_spans[g_nspans++];
    s->id = g_next_span++;
    s->name = argv[0];
    s->attrs = dict_new_empty();
    s->events = list_from_vec(NULL, 0, 0);
    s->start_ms = now_ms();
    s->end_ms = -1;
    g_current_span = s->id;
    return mkint_full((int64_t)s->id);
}

static aval bi_trace_end(uint64_t n, aval *argv) {
    want_args("trace.end", n, 1, 1);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.end: unknown span");
    s->end_ms = now_ms();
    if (g_current_span == s->id) g_current_span = 0;
    return A_NONE;
}

static aval bi_trace_current(uint64_t n, aval *argv) {
    want_args("trace.current", n, 0, 0);
    return g_current_span ? mkint_full((int64_t)g_current_span) : A_NONE;
}

static aval bi_trace_set_attribute(uint64_t n, aval *argv) {
    want_args("trace.set_attribute", n, 3, 3);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.set_attribute: unknown span");
    dict_put((ADict *)obj_of(s->attrs), argv[1], argv[2]);
    return A_NONE;
}

static aval bi_trace_add_event(uint64_t n, aval *argv) {
    want_args("trace.add_event", n, 2, 3);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.add_event: unknown span");
    /* events: list of {name, attributes?} dicts */
    aval ev = dict_new_empty();
    ADict *ed = (ADict *)obj_of(ev);
    dict_put(ed, a_str_new("name", 4), argv[1]);
    if (n > 2) dict_put(ed, a_str_new("attributes", 10), argv[2]);
    /* append to events list: rebuild via list concat helper is overkill;
       store events as dict keyed by count instead */
    ADict *ad = (ADict *)obj_of(s->attrs);
    aval cnt = dict_find(ad, a_str_new("__nevents", 9)) >= 0
                   ? ad->pairs[dict_find(ad, a_str_new("__nevents", 9))].v
                   : a_mkint(0);
    dict_put(ad, a_str_new("__nevents", 9), a_mkint(int_val_full(cnt) + 1));
    dict_put(ad, a_str_new("__evt", 5), ev); /* keeps last; list form below */
    /* simpler: keep an AArr of events */
    AArr *ea = (AArr *)obj_of(s->events);
    uint64_t idx = ea->h.len;
    ea->items = (aval *)xrealloc(ea->items, sizeof(aval) * (idx + 1));
    ea->items[idx] = ev;
    ea->h.len = (uint32_t)(idx + 1);
    return A_NONE;
}

static aval bi_trace_inject(uint64_t n, aval *argv) {
    want_args("trace.inject", n, 0, 1);
    aval d = dict_new_empty();
    ADict *dd = (ADict *)obj_of(d);
    dict_put(dd, a_str_new("trace_id", 8), a_mkint(1));
    dict_put(dd, a_str_new("span_id", 7), g_current_span ? mkint_full((int64_t)g_current_span) : A_NONE);
    return d;
}

static aval bi_trace_extract(uint64_t n, aval *argv) {
    want_args("trace.extract", n, 1, 1);
    (void)argv;
    return A_NONE;
}

static aval bi_trace_spans(uint64_t n, aval *argv) {
    want_args("trace.spans", n, 0, 0);
    aval *items = (aval *)xmalloc(sizeof(aval) * (g_nspans ? g_nspans : 1));
    for (size_t i = 0; i < g_nspans; ++i) {
        ASpan *s = &g_spans[i];
        aval d = dict_new_empty();
        ADict *dd = (ADict *)obj_of(d);
        dict_put(dd, a_str_new("id", 2), mkint_full((int64_t)s->id));
        dict_put(dd, a_str_new("name", 4), s->name);
        dict_put(dd, a_str_new("attributes", 10), s->attrs);
        dict_put(dd, a_str_new("events", 6), s->events);
        dict_put(dd, a_str_new("start_ms", 8), mkint_full(s->start_ms));
        dict_put(dd, a_str_new("end_ms", 6), s->end_ms >= 0 ? mkint_full(s->end_ms) : A_NONE);
        items[i] = d;
    }
    aval out = list_from_vec(items, (uint64_t)g_nspans, 0);
    free(items);
    return out;
}

static aval bi_trace_reset(uint64_t n, aval *argv) {
    want_args("trace.reset", n, 0, 0);
    g_nspans = 0;
    g_current_span = 0;
    g_next_span = 1;
    return A_NONE;
}

/* ------------------------------------------------------------------ */
/* net module: resolve + TLS probe                                     */
/* ------------------------------------------------------------------ */

static aval bi_net_resolve(uint64_t n, aval *argv) {
    want_args("net.resolve", n, 1, 1);
    uint64_t hl;
    const char *host = str_data(argv[0], &hl);
    char hbuf[256];
    if (hl >= sizeof hbuf) panicf("net.resolve: host name too long");
    memcpy(hbuf, host, hl);
    hbuf[hl] = '\0';
    aval *items = NULL;
    size_t cnt = 0, cap = 0;
#if defined(__unix__) || defined(__APPLE__)
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hbuf, NULL, &hints, &res) == 0) {
        for (ai = res; ai; ai = ai->ai_next) {
            char buf[INET6_ADDRSTRLEN];
            void *addr = ai->ai_family == AF_INET
                             ? (void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                             : (void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
            if (!inet_ntop(ai->ai_family, addr, buf, sizeof buf)) continue;
            if (cnt == cap) {
                cap = cap ? cap * 2 : 8;
                items = (aval *)xrealloc(items, cap * sizeof(aval));
            }
            items[cnt++] = a_str_new(buf, strlen(buf));
        }
        freeaddrinfo(res);
    }
#endif
    aval out = list_from_vec(items, (uint64_t)cnt, 0);
    free(items);
    return out;
}

static aval bi_net_tls_available(uint64_t n, aval *argv) {
    want_args("net.tls_available", n, 0, 0);
    return A_FALSE;
}

/* ------------------------------------------------------------------ */
/* arena module: bump/pool allocators                                  */
/* ------------------------------------------------------------------ */

static AArena *arena_of(aval v) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_ARENA) panicf("arena expected, not '%s'", type_name_cstr(v));
    return (AArena *)o;
}

static aval arena_new_impl(uint64_t cap) {
    AArena *a = (AArena *)xmalloc(sizeof(AArena));
    a->h.type = T_ARENA;
    a->h.len = 0;
    a->h.cap = 0;
    a->cap = cap;
    a->bump = 0;
    a->live = 0;
    a->nalloc = 0;
    a->capalloc = 0;
    a->allocs = NULL;
    return mkobj(&a->h);
}

static aval bi_arena_new(uint64_t n, aval *argv) {
    want_args("arena.new", n, 0, 1);
    uint64_t cap = n ? (uint64_t)int_val_full(argv[0]) : 1u << 20;
    return arena_new_impl(cap);
}

static aval bi_arena_pool(uint64_t n, aval *argv) {
    want_args("arena.pool", n, 1, 1);
    return arena_new_impl((uint64_t)int_val_full(argv[0]));
}

static aval bi_arena_region(uint64_t n, aval *argv) {
    want_args("arena.region", n, 1, 1);
    return arena_new_impl((uint64_t)int_val_full(argv[0]));
}

static aval bi_arena_alloc(uint64_t n, aval *argv) {
    want_args("arena.alloc", n, 2, 2);
    AArena *a = arena_of(argv[0]);
    uint64_t size = (uint64_t)int_val_full(argv[1]);
    size = (size + 7) & ~7ULL;
    /* reuse a freed slot when possible */
    for (uint64_t i = 0; i < a->nalloc; ++i) {
        if (a->allocs[i].free && a->allocs[i].size >= size) {
            a->allocs[i].free = 0;
            a->live += size;
            return mkint_full((int64_t)(i + 1));
        }
    }
    if (a->nalloc == a->capalloc) {
        a->capalloc = a->capalloc ? a->capalloc * 2 : 16;
        a->allocs = (AAlloc *)xrealloc(a->allocs, a->capalloc * sizeof(AAlloc));
    }
    if (a->bump + size > a->cap) panicf("arena exhausted");
    uint64_t idx = a->nalloc++;
    a->allocs[idx].off = a->bump;
    a->allocs[idx].size = size;
    a->allocs[idx].free = 0;
    a->bump += size;
    a->live += size;
    return mkint_full((int64_t)(idx + 1));
}

static aval bi_arena_release(uint64_t n, aval *argv) {
    want_args("arena.release", n, 2, 2);
    AArena *a = arena_of(argv[0]);
    uint64_t h = (uint64_t)int_val_full(argv[1]);
    if (h == 0 || h > a->nalloc || a->allocs[h - 1].free)
        panicf("arena.release: invalid handle");
    a->allocs[h - 1].free = 1;
    a->live -= a->allocs[h - 1].size;
    return A_NONE;
}

static aval bi_arena_reset(uint64_t n, aval *argv) {
    want_args("arena.reset", n, 1, 1);
    AArena *a = arena_of(argv[0]);
    a->bump = 0;
    a->live = 0;
    a->nalloc = 0;
    return A_NONE;
}

static aval bi_arena_used(uint64_t n, aval *argv) {
    want_args("arena.used", n, 1, 1);
    return mkint_full((int64_t)arena_of(argv[0])->live);
}

static aval bi_arena_total(uint64_t n, aval *argv) {
    want_args("arena.total", n, 1, 1);
    return mkint_full((int64_t)arena_of(argv[0])->cap);
}

/* ------------------------------------------------------------------ */
/* unsafe module                                                       */
/* ------------------------------------------------------------------ */

/* address_of/load/store/offset use a flat table of aval slots; an
   "address" is a 1-based slot index. */
static aval *g_ptrtab;
static size_t g_nptrtab, g_capptrtab;

static uint64_t ptr_id(aval v) {
    if (kind_of(v) != K_INT) panicf("unsafe: address must be int");
    uint64_t id = (uint64_t)int_val_full(v);
    if (id == 0 || id > g_nptrtab) panicf("unsafe: invalid address");
    return id - 1;
}

static aval bi_unsafe_get(uint64_t n, aval *argv) {
    want_args("unsafe.get", n, 2, 2);
    AObj *o = obj_of(argv[0]);
    int64_t i = int_val_full(argv[1]);
    if (o && (o->type == T_LIST || o->type == T_TUPLE)) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.get: index out of bounds");
        return a->items[i];
    }
    if (o && o->type == T_SET) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.get: index out of bounds");
        return a->items[i];
    }
    if (o && o->type == T_DICT) {
        ADict *d = (ADict *)o;
        int64_t idx = dict_find(d, argv[1]);
        if (idx < 0) return A_NONE;
        return d->pairs[idx].v;
    }
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        uint64_t len;
        const char *p = str_data(argv[0], &len);
        if (i < 0 || (uint64_t)i >= len) panicf("unsafe.get: index out of bounds");
        char c = p[i];
        return a_char_new(&c, 1);
    }
    panicf("unsafe.get: unsupported container '%s'", type_name_cstr(argv[0]));
    return A_NONE;
}

static aval bi_unsafe_set(uint64_t n, aval *argv) {
    want_args("unsafe.set", n, 3, 3);
    AObj *o = obj_of(argv[0]);
    int64_t i = int_val_full(argv[1]);
    if (o && (o->type == T_LIST || o->type == T_TUPLE)) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.set: index out of bounds");
        a->items[i] = argv[2];
        return A_NONE;
    }
    if (o && o->type == T_DICT) {
        dict_put((ADict *)o, argv[1], argv[2]);
        return A_NONE;
    }
    panicf("unsafe.set: unsupported container '%s'", type_name_cstr(argv[0]));
    return A_NONE;
}

static aval bi_unsafe_address_of(uint64_t n, aval *argv) {
    want_args("unsafe.address_of", n, 1, 1);
    if (g_nptrtab == g_capptrtab) {
        g_capptrtab = g_capptrtab ? g_capptrtab * 2 : 64;
        g_ptrtab = (aval *)xrealloc(g_ptrtab, g_capptrtab * sizeof(aval));
    }
    g_ptrtab[g_nptrtab] = argv[0];
    return mkint_full((int64_t)(g_nptrtab++ + 1));
}

static aval bi_unsafe_load(uint64_t n, aval *argv) {
    want_args("unsafe.load", n, 1, 1);
    return g_ptrtab[ptr_id(argv[0])];
}

static aval bi_unsafe_store(uint64_t n, aval *argv) {
    want_args("unsafe.store", n, 2, 2);
    g_ptrtab[ptr_id(argv[0])] = argv[1];
    return A_NONE;
}

static aval bi_unsafe_offset(uint64_t n, aval *argv) {
    want_args("unsafe.offset", n, 2, 2);
    uint64_t id = ptr_id(argv[0]);
    int64_t off = int_val_full(argv[1]);
    uint64_t nid = id + (uint64_t)off;
    if (nid >= g_nptrtab) panicf("unsafe.offset: address out of bounds");
    return mkint_full((int64_t)(nid + 1));
}

/* builtin table: order must match the enum in a_runtime.h */
static const struct { const char *name; a_builtin_fn fn; } g_builtins[A_BI_COUNT] = {
    { "print", bi_print }, { "eprint", bi_eprint }, { "len", bi_len },
    { "str", bi_str }, { "int", bi_int }, { "float", bi_float },
    { "bool", bi_bool }, { "abs", bi_abs }, { "min", bi_min },
    { "max", bi_max }, { "sum", bi_sum }, { "pow", bi_pow },
    { "sqrt", bi_sqrt }, { "sin", bi_sin }, { "cos", bi_cos },
    { "tan", bi_tan }, { "asin", bi_asin }, { "acos", bi_acos },
    { "atan", bi_atan }, { "exp", bi_exp }, { "log", bi_log },
    { "floor", bi_floor }, { "ceil", bi_ceil }, { "round", bi_round },
    { "trunc", bi_trunc }, { "random", bi_random }, { "random_int", bi_random_int },
    { "range", bi_range }, { "set", bi_set }, { "tuple", bi_tuple },
    { "list", bi_list }, { "dict", bi_dict }, { "map", bi_map },
    { "filter", bi_filter }, { "any", bi_any }, { "all", bi_all },
    { "some", bi_some }, { "ok", bi_ok }, { "err", bi_err },
    { "panic", bi_panic }, { "assert", bi_assert }, { "assert_eq", bi_assert_eq },
    { "input", bi_input }, { "read_file", bi_read_file }, { "write_file", bi_write_file },
    { "append_file", bi_append_file }, { "file_exists", bi_file_exists },
    { "list_dir", bi_list_dir }, { "remove_file", bi_remove_file },
    { "create_dir", bi_create_dir }, { "remove_dir", bi_remove_dir },
    { "rename", bi_rename }, { "current_dir", bi_current_dir },
    { "repr", bi_repr }, { "type", bi_type }, { "isinstance", bi_isinstance },
    { "join", bi_join }, { "sorted", bi_sorted }, { "reversed", bi_reversed },
    { "enumerate", bi_enumerate }, { "zip", bi_zip }, { "reduce", bi_reduce },
    { "log2", bi_log2 }, { "log10", bi_log10 }, { "atan2", bi_atan2 }, { "flush", bi_flush },
    { "i8", bi_i8 }, { "i16", bi_i16 }, { "i32", bi_i32 }, { "u8", bi_u8 },
    { "u16", bi_u16 }, { "u32", bi_u32 }, { "f32", bi_f32 },
    { "ctz", bi_ctz }, { "clz", bi_clz }, { "popcount", bi_popcount },
    { "bswap", bi_bswap }, { "rotl", bi_rotl }, { "rotr", bi_rotr },
    { "fma", bi_fma }, { "fabs", bi_fabs }, { "copysign", bi_copysign },
    { "smin", bi_smin }, { "smax", bi_smax }, { "umin", bi_umin },
    { "umax", bi_umax },
    { "sadd_overflow", bi_sadd_ovf }, { "ssub_overflow", bi_ssub_ovf },
    { "smul_overflow", bi_smul_ovf }, { "uadd_overflow", bi_uadd_ovf },
    { "usub_overflow", bi_usub_ovf }, { "umul_overflow", bi_umul_ovf },
    { "expect", bi_expect }, { "likely", bi_likely }, { "unlikely", bi_unlikely },
    { "env_get", bi_env_get },
    { "now", bi_time_now }, { "sleep", bi_time_sleep },
    { "terminal_isatty", bi_terminal_isatty },
    { "terminal_raw_mode", bi_terminal_raw_mode },
    { "terminal_read_char", bi_terminal_read_char },
    { "terminal_read_char_ms", bi_terminal_read_char_ms },
    { "terminal_write", bi_terminal_write },
    { "terminal_flush", bi_terminal_flush },
    { "terminal_size", bi_terminal_size },
    { "terminal_home", bi_terminal_home },
    { "repl_eval", bi_repl_eval },
    { "json_parse", bi_json_parse }, { "json_stringify", bi_json_stringify },
    { "json_pretty", bi_json_pretty }, { "json_is_valid", bi_json_is_valid },
    { "json_get", bi_json_get }, { "json_at", bi_json_at },
    { "json_len", bi_json_len }, { "json_has", bi_json_has },
    { "json_keys", bi_json_keys }, { "json_type", bi_json_kind },
    { "json_as_str", bi_json_as_str }, { "json_as_int", bi_json_as_int },
    { "json_as_float", bi_json_as_float }, { "json_as_bool", bi_json_as_bool },
    { "json_as_array", bi_json_as_array }, { "json_as_map", bi_json_as_map },
    { "repl_quiet", bi_repl_quiet },
    { "binary_encode", bi_binary_encode }, { "binary_decode", bi_binary_decode },
    { "log_set_level", bi_log_set_level }, { "log_set_json", bi_log_set_json },
    { "log_debug", bi_log_debug }, { "log_info", bi_log_info },
    { "log_warn", bi_log_warn }, { "log_error", bi_log_error },
    { "metrics_counter", bi_metrics_counter }, { "metrics_gauge", bi_metrics_gauge },
    { "metrics_histogram", bi_metrics_histogram }, { "metrics_render", bi_metrics_render },
    { "metrics_reset", bi_metrics_reset },
    { "trace_start", bi_trace_start }, { "trace_end", bi_trace_end },
    { "trace_current", bi_trace_current },
    { "trace_set_attribute", bi_trace_set_attribute },
    { "trace_add_event", bi_trace_add_event },
    { "trace_inject", bi_trace_inject }, { "trace_extract", bi_trace_extract },
    { "trace_spans", bi_trace_spans }, { "trace_reset", bi_trace_reset },
    { "net_resolve", bi_net_resolve }, { "net_tls_available", bi_net_tls_available },
    { "arena_new", bi_arena_new }, { "arena_pool", bi_arena_pool },
    { "arena_region", bi_arena_region }, { "arena_alloc", bi_arena_alloc },
    { "arena_release", bi_arena_release }, { "arena_reset", bi_arena_reset },
    { "arena_used", bi_arena_used }, { "arena_total", bi_arena_total },
    { "unsafe_get", bi_unsafe_get }, { "unsafe_set", bi_unsafe_set },
    { "unsafe_address_of", bi_unsafe_address_of }, { "unsafe_load", bi_unsafe_load },
    { "unsafe_store", bi_unsafe_store }, { "unsafe_offset", bi_unsafe_offset }
};

static aval g_builtin_objs[A_BI_COUNT];

aval a_builtin(uint64_t id) {
    if (id >= A_BI_COUNT) panicf("invalid builtin id %llu", (unsigned long long)id);
    if (g_builtin_objs[id] == 0)
        g_builtin_objs[id] = fnon_new_raw((uint64_t)(uintptr_t)g_builtins[id].fn,
                                          A_NONE, g_builtins[id].name,
                                          FNON_BUILTIN, NPARAMS_UNKNOWN);
    return g_builtin_objs[id];
}

aval a_bi_call(uint64_t id, uint64_t n, aval *argv) {
    if (id >= A_BI_COUNT) panicf("invalid builtin id %llu", (unsigned long long)id);
    return g_builtins[id].fn(n, argv);
}

int64_t a_int_full(aval v) { return int_val_full(v); }

uint64_t a_builtin_count(void) { return (uint64_t)A_BI_COUNT; }

const char *a_builtin_name(uint64_t id) {
    return id < A_BI_COUNT ? g_builtins[id].name : "<unknown>";
}

/* ------------------------------------------------------------------ */
/* printing / panic / init                                             */
/* ------------------------------------------------------------------ */

static void printv(FILE *f, uint64_t n, aval *argv) {
    for (uint64_t i = 0; i < n; ++i) {
        if (i) fputc(' ', f);
        Buf b;
        buf_init(&b);
        format_into(&b, argv[i], 0);
        if (b.len) fwrite(b.p, 1, b.len, f);
        free(b.p);
    }
    fputc('\n', f);
    fflush(f);
}

aval a_printv(uint64_t n, aval *argv) {
    if (g_repl_quiet) return A_NONE;
    printv(stdout, n, argv);
    return A_NONE;
}
aval a_eprintv(uint64_t n, aval *argv) {
    if (g_repl_quiet) return A_NONE;
    printv(stderr, n, argv);
    return A_NONE;
}

aval a_panic_msg(const char *msg) {
    fprintf(stderr, "error: %s\n", msg ? msg : "");
    exit(2);
    return A_NONE;
}

static int g_runtime_ready = 0;

void a_runtime_init(void) {
    if (g_runtime_ready) return;
    g_runtime_ready = 1;
    srand((unsigned)time(NULL));
    for (uint64_t i = 0; i < (uint64_t)A_BI_COUNT; ++i) (void)a_builtin(i);
}





/* ---- codegen-support helpers (appended) ---- */
aval a_i64n(long long v) { return mkint_full((int64_t)v); }
int a_is_propagate(aval v) {
    if (a_is_none(v)) return 1;
    if (is_result(v) && !result_ok(v)) return 1;
    return 0;
}
aval a_match_fail(aval subj) {
    (void)subj;
    panicf("no match arm matched");
    return A_NONE;
}
aval a_list_push(aval list, aval v) {
    AArr *a = arr_of(list, "list");
    arr_push(a, v);
    return list;
}
aval a_setattr(aval obj, const char *name, aval v) {
    /* field assignment: dicts store by key; others panic */
    AObj *o = obj_of(obj);
    if (o && o->type == T_DICT) {
        aval k = a_str_new(name, (uint64_t)strlen(name));
        a_dict_set(obj, k, v);
        return v;
    }
    panicf("'%s' object has no attribute '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

#if !defined(_WIN32)
/* `a test` harness: fork per test so assertion panics don't stop the suite.
   Mirrors the protocol the C codegen used to emit inline. */
int a_test_main(uint64_t n, const char * const *names, a_test_fn const *fns) {
    int passed = 0, failed = 0;
    for (uint64_t i = 0; i < n; ++i) {
        int pfd[2];
        if (pipe(pfd) != 0) { perror("pipe"); return 1; }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]); dup2(pfd[1], 2); close(pfd[1]);
            int dn = open("/dev/null", 1);
            if (dn >= 0) { dup2(dn, 1); close(dn); }
            aval argv[1]; argv[0] = A_NONE;
            fns[i](argv);
            fflush(stdout); fflush(stderr); _exit(0);
        }
        close(pfd[1]);
        char msg[4096];
        ssize_t rn = read(pfd[0], msg, sizeof(msg) - 1);
        if (rn < 0) rn = 0;
        msg[rn] = 0;
        close(pfd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            printf("ok %s\n", names[i]);
            ++passed;
        } else {
            char *m = (char *)msg;
            if (strncmp(m, "error: ", 7) == 0) m += 7;
            size_t ml = strlen(m);
            while (ml > 0 && (m[ml-1] == '\n' || m[ml-1] == '\r')) m[--ml] = 0;
            printf("FAILED %s: %s\n", names[i], m);
            ++failed;
        }
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
#endif
