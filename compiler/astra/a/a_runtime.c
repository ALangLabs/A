/* Astra A runtime: NaN-boxed values, objects, operators, methods, builtins.
 *
 * Plain C99 + libc only.  Generated code sees only aval words and the public
 * API in a_runtime.h; all object layouts below are private to this file.
 */
#define _POSIX_C_SOURCE 200809L
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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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
    T_FNON, T_ENV, T_RESULT, T_INT64
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
typedef struct { AObj h; uint32_t ok; aval value; } AResult;
typedef struct { AObj h; int64_t v; } AInt64;
typedef struct {
    AObj h;
    aval src;
    uint64_t idx;
    int64_t cur, end, step;
} AIter;

#define FNON_BUILTIN 1u
#define NPARAMS_UNKNOWN UINT64_MAX

enum {
    K_NONE, K_BOOL, K_INT, K_FLOAT, K_STR, K_LIST, K_TUPLE, K_DICT,
    K_SET, K_RANGE, K_ITER, K_FNON, K_ENV, K_RESULT, K_OTHER
};

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
    exit(1);
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
    if (!o || o->type != T_STR)
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

static const char *str_data(aval v, uint64_t *len) {
    AStr *s = str_of(v, "value");
    if (len) *len = s->h.len;
    return s->data;
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
    int64_t st = start;
    if (st < 0) st += (int64_t)hn;
    if (st < 0) st = 0;
    if ((uint64_t)st > hn) return -1;
    if (nn == 0) return st <= (int64_t)hn ? st : -1;
    if ((uint64_t)st + nn > hn) return -1;
    if (!reverse) {
        size_t pos = 0;
        const char *p = find_bytes(h, (size_t)hn, n, (size_t)nn, (size_t)st, &pos);
        return p ? (int64_t)pos : -1;
    }
    /* reverse: scan candidate end positions from the back */
    for (int64_t i = (int64_t)hn - (int64_t)nn; i >= st; --i) {
        if (memcmp(h + i, n, (size_t)nn) == 0) return i;
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
            buf_adds(b, r->ok ? "Ok(" : "Err(");
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
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_DICT: return ((AArr *)obj_of(v))->h.len != 0;
        default: return 1;
    }
}

aval a_truthy_raw(aval v) { return (aval)(truthy(v) ? 1 : 0); }
aval a_not(aval v) { return bool_aval(!truthy(v)); }
aval a_bool(aval v) { return bool_aval(truthy(v)); }

aval a_neg(aval v) {
    if (is_int_val(v)) {
        int64_t i = int_val_full(v);
        if (i == INT64_MIN) return mkdouble(-(double)INT64_MIN);
        return mkint_full(-i);
    }
    if (a_is_double(v)) return mkdouble(-a_double_val(v));
    panicf("bad operand type for unary -: '%s'", type_name_cstr(v));
    return A_NONE;
}

aval a_bnot(aval v) {
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
        case K_STR: return mkint_full(((AStr *)obj_of(v))->h.len);
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
    if (ka != kb) return 0;
    switch (ka) {
        case K_BOOL: return 0;
        case K_STR: return str_eq(a, b);
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
            return x->ok == y->ok && values_equal(x->value, y->value);
        }
        default: return 0;
    }
}

aval a_eq(aval l, aval r) { return bool_aval(values_equal(l, r)); }
aval a_ne(aval l, aval r) { return bool_aval(!values_equal(l, r)); }

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

aval a_lt(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<") < 0); }
aval a_le(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<=") <= 0); }
aval a_gt(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">") > 0); }
aval a_ge(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">=") >= 0); }

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
            if (b == 0.0) panicf("Division by zero.");
            x = a / b;
            break;
        case '%': {
            if (b == 0.0) panicf("Remainder by zero.");
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
            if (b == 0) panicf("Division by zero.");
            if (a == INT64_MIN && b == -1) return mkdouble(9223372036854775808.0);
            return mkint_full(a / b);
        case '%':
            if (b == 0) panicf("Remainder by zero.");
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

aval a_add(aval l, aval r) { return binary_arith(l, r, '+', "+"); }
aval a_sub(aval l, aval r) { return binary_arith(l, r, '-', "-"); }
aval a_mul(aval l, aval r) { return binary_arith(l, r, '*', "*"); }
aval a_div(aval l, aval r) { return binary_arith(l, r, '/', "/"); }
aval a_mod(aval l, aval r) { return binary_arith(l, r, '%', "%"); }
aval a_pow(aval l, aval r) { return binary_arith(l, r, '^', "**"); }

/* ------------------------------------------------------------------ */
/* bitwise / shifts / membership                                       */
/* ------------------------------------------------------------------ */

aval a_band(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("&", l, r);
    return mkint_full(int_val_full(l) & int_val_full(r));
}
aval a_bor(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("|", l, r);
    return mkint_full(int_val_full(l) | int_val_full(r));
}
aval a_bxor(aval l, aval r) {
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

aval a_shl(aval l, aval r) {
    if (!is_int_val(l)) panic_binop("<<", l, r);
    int64_t a = int_val_full(l), s = shift_amount(r);
    return mkint_full((int64_t)((uint64_t)a << s));
}
aval a_shr(aval l, aval r) {
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
    if (c == 0) panicf("range() step must not be zero");
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
            int64_t i = normalize_index(expect_int64(idx, "string index"), s->h.len);
            return a_str_new(s->data + i, 1);
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
            if (i < 0) panicf("key not found");
            return d->pairs[i].v;
        }
        default:
            panicf("object of type '%s' is not subscriptable", type_name_cstr(obj));
            return A_NONE;
    }
}

aval a_setitem(aval obj, aval idx, aval val) {
    switch (kind_of(obj)) {
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

static int64_t slice_step(aval v) {
    int64_t s = expect_int64(v, "slice step");
    if (s == 0) panicf("slice step cannot be zero");
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
    if (k != K_STR && k != K_LIST && k != K_TUPLE)
        panicf("object of type '%s' is not sliceable", type_name_cstr(obj));
    AObj *o = obj_of(obj);
    uint64_t len = o->type == T_STR ? ((AStr *)o)->h.len : ((AArr *)o)->h.len;
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
            buf_addc(&b, s->data[i]);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        return buf_take(&b);
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
    if (so->type == T_DICT) {
        d = (ADict *)so;
        len = d->h.len;
    } else if (so->type == T_STR) {
        len = ((AStr *)so)->h.len;
    } else {
        AArr *a = (AArr *)so;
        len = a->h.len;
        items = a->items;
    }
    if (iter->idx >= len) return (aval)0;
    uint64_t i = iter->idx++;
    if (d) *out = d->pairs[i].k;
    else if (so->type == T_STR) *out = a_str_new(((AStr *)so)->data + i, 1);
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
    r->value = value;
    return r;
}

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
    if (a_is_none(v)) panicf("Cannot unwrap None with '!'");
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
    /* mode: 0 lower, 1 upper, 2 capitalize, 3 title */
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
    if (count < 0) count = INT64_MAX;
    if (on == 0) {
        int64_t limit = count;
        if (limit > (int64_t)sn + 1) limit = (int64_t)sn + 1;
        for (uint64_t i = 0; i <= sn; ++i) {
            if (limit-- > 0) buf_addn(&b, np, (size_t)nn);
            if (i < sn) buf_addc(&b, sp[i]);
        }
        return buf_take(&b);
    }
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
    uint64_t left = pad, right = 0;
    if (mode == 1) { left = 0; right = pad; }
    else if (mode == 2) { left = pad / 2; right = pad - left; }
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
    panicf("value is not in list");
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
    if (a->h.len == 0) panicf("pop from empty list");
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
        if (!strcmp(name, "strip")) { want_args(name, n, 0, 0); return str_strip_impl(obj, 1, 1); }
        if (!strcmp(name, "lstrip")) { want_args(name, n, 0, 0); return str_strip_impl(obj, 1, 0); }
        if (!strcmp(name, "rstrip")) { want_args(name, n, 0, 0); return str_strip_impl(obj, 0, 1); }
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
                if (kind_of(a->items[i]) != K_STR)
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
                        panicf("substring not found");
                    return mkint_full(r);
                }
            }
            int64_t r = str_find(obj, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
            if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                panicf("substring not found");
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
        if (!strcmp(name, "zfill") || !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust")) {
            want_args(name, n, 1, 2);
            int64_t w = expect_int64(argv[0], "width");
            aval fill = n == 2 ? argv[1] : A_NONE;
            int mode = !strcmp(name, "ljust") ? 0 : !strcmp(name, "rjust") ? 1 : !strcmp(name, "center") ? 2 : 3;
            return str_affix(obj, w, fill, mode);
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
        if (!strcmp(name, "first")) { want_args(name, n, 0, 0); if (!a->h.len) panicf("index out of range"); return a->items[0]; }
        if (!strcmp(name, "last")) { want_args(name, n, 0, 0); if (!a->h.len) panicf("index out of range"); return a->items[a->h.len - 1]; }
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
            panicf("list.remove(x): x not in list");
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

static aval bi_pow(uint64_t n, aval *argv) { want_args("pow", n, 2, 2); return a_pow(argv[0], argv[1]); }

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
    double f = floor(x);
    double diff = x - f;
    double r = f;
    if (diff > 0.5) r = f + 1.0;
    else if (diff == 0.5 && ((int64_t)f & 1)) r = f + 1.0;
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

static aval bi_any(uint64_t n, aval *argv) { (void)argv; want_args("any", n, 1, 1); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i) if(truthy(a->items[i])) return A_TRUE; return A_FALSE; }
static aval bi_all(uint64_t n, aval *argv) { (void)argv; want_args("all", n, 1, 1); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i) if(!truthy(a->items[i])) return A_FALSE; return A_TRUE; }
static aval bi_some(uint64_t n, aval *argv) { return bi_any(n, argv); }

static aval bi_ok(uint64_t n, aval *argv) { want_args("ok", n, 1, 1); return mkobj(result_new(1, argv[0])); }
static aval bi_err(uint64_t n, aval *argv) { want_args("err", n, 1, 1); return mkobj(result_new(0, argv[0])); }

static void panic_with_msg(uint64_t n, aval *argv, int has, const char *def) {
    (void)n;
    if (!has) { a_panic_msg(def); return; }
    aval s = a_str(argv[0]);
    uint64_t len;
    const char *p = str_data(s, &len);
    (void)len;
    a_panic_msg(p);
}

static aval bi_panic(uint64_t n, aval *argv) {
    want_args("panic", n, 0, 1);
    if (n == 0) a_panic_msg("panic");
    else panic_with_msg(n, argv, 1, "panic");
    return A_NONE;
}

static aval bi_assert(uint64_t n, aval *argv) {
    want_args("assert", n, 1, 2);
    if (!truthy(argv[0])) panic_with_msg(n, argv, n == 2, "Assertion failed");
    return A_NONE;
}

static aval bi_assert_eq(uint64_t n, aval *argv) {
    want_args("assert_eq", n, 2, 3);
    if (!values_equal(argv[0], argv[1])) {
        if (n == 3) panic_with_msg(n, argv, 1, "Assertion failed");
        Buf b;
        buf_init(&b);
        buf_adds(&b, "assertion failed: ");
        format_into(&b, argv[0], 1);
        buf_adds(&b, " != ");
        format_into(&b, argv[1], 1);
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
    if (!fgets(buf, 4096, stdin)) { free(buf); return A_NONE; }
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
        if (kind_of(a->items[i]) != K_STR)
            panicf("sequence item %llu: expected str instance, %s found",
                   (unsigned long long)i, type_name_cstr(a->items[i]));
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
    { "enumerate", bi_enumerate }, { "zip", bi_zip }
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

aval a_printv(uint64_t n, aval *argv) { printv(stdout, n, argv); return A_NONE; }
aval a_eprintv(uint64_t n, aval *argv) { printv(stderr, n, argv); return A_NONE; }

aval a_panic_msg(const char *msg) {
    fprintf(stderr, "error: %s\n", msg ? msg : "");
    exit(1);
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
