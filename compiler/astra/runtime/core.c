#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

void panicf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) panicf("out of memory");
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
    return (AObj *)A_PAYLOAD(v);
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

/* ------------------------------------------------------------------ */
/* growable buffers                                                    */
/* ------------------------------------------------------------------ */



void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->p = (char *)xrealloc(b->p, nc);
    b->cap = nc;
}

void buf_addn(Buf *b, const char *s, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }

void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }

void buf_addi64(Buf *b, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n > 0) buf_addn(b, tmp, (size_t)n);
}

aval buf_take(Buf *b) {
    aval s = a_str_new(b->p ? b->p : "", (uint64_t)b->len);
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

AStr *str_of(aval v, const char *what) {
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

const char *str_data(aval v, uint64_t *len) {
    AStr *s = str_of(v, "value");
    if (len) *len = s->h.len;
    return s->data;
}

/* UTF-8 helpers: strings are indexed by code point, not byte. */
int u8_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
uint64_t u8_count(const char *p, uint64_t n) {
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
uint64_t cp_to_byte(const char *p, uint64_t n, int64_t cp) {
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

int str_eq(aval a, aval b) {
    uint64_t la, lb;
    const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
    return la == lb && (la == 0 || memcmp(pa, pb, (size_t)la) == 0);
}

const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn,
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

int64_t str_find(aval hay, aval needle, int64_t start, int reverse) {
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

aval str_repeat(aval s, int64_t count) {
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

AArr *arr_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE && o->type != T_SET))
        panicf("%s must be a list, tuple, or set", what);
    return (AArr *)o;
}

void arr_reserve(AArr *a, uint64_t need) {
    /* Safety net: any caller that reserved aval capacity on a packed
       array first converts it back to aval storage. */
    if (arr_packed(a)) arr_ensure_aval(a);
    if (need <= a->h.cap) return;
    uint64_t nc = a->h.cap ? a->h.cap : 4;
    while (nc < need) nc *= 2;
    a->items = (aval *)xrealloc(a->items, (size_t)nc * sizeof(aval));
    a->h.cap = nc;
}

void arr_reserve_i64(AArr *a, uint64_t need) {
    if (need <= a->h.cap) return;
    uint64_t nc = a->h.cap ? a->h.cap : 4;
    while (nc < need) nc *= 2;
    a->iitems = (int64_t *)xrealloc(a->iitems, (size_t)nc * sizeof(int64_t));
    a->h.cap = nc;
}

int arr_packed(const AArr *a) { return (a->flags & ARR_F_PACKED) != 0; }

/* Convert packed int64 storage back to aval elements in place. Lazy and
   reversible: nothing here sets ARR_F_NOMIX, so a later arr_push_int /
   arr_try_pack may pack the array again if it is still all-integer. */
void arr_ensure_aval(AArr *a) {
    if (!arr_packed(a)) return;
    uint64_t cap = a->h.cap ? a->h.cap : 1;
    aval *items = (aval *)xmalloc((size_t)cap * sizeof(aval));
    for (uint64_t i = 0; i < a->h.len; ++i)
        items[i] = mkint_full(a->iitems[i]);
    free(a->iitems);
    a->iitems = NULL;
    a->items = items;
    a->flags &= ~ARR_F_PACKED;
}

/* aval -> packed. Caller must have verified every element is an int
   (is_int_val); int_val_full then cannot panic. */
static void arr_to_packed(AArr *a) {
    uint64_t cap = a->h.cap ? a->h.cap : 4;
    int64_t *iitems = (int64_t *)xmalloc((size_t)cap * sizeof(int64_t));
    for (uint64_t i = 0; i < a->h.len; ++i)
        iitems[i] = int_val_full(a->items[i]);
    free(a->items);
    a->items = NULL;
    a->iitems = iitems;
    a->flags |= ARR_F_PACKED;
}

/* Pack an aval T_LIST whose elements are all integers. No-op for
   tuples/sets, already-packed arrays, and arrays marked NOMIX. */
void arr_try_pack(AArr *a) {
    if (a->h.type != T_LIST || arr_packed(a) || (a->flags & ARR_F_NOMIX))
        return;
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (!is_int_val(a->items[i])) {
            a->flags |= ARR_F_NOMIX;
            return;
        }
    arr_to_packed(a);
}

aval arr_elem(const AArr *a, uint64_t i) {
    return arr_packed(a) ? mkint_full(a->iitems[i]) : a->items[i];
}

void arr_push(AArr *a, aval v) {
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (arr_packed(a)) {
        if (is_int_val(v)) {
            arr_reserve_i64(a, (uint64_t)a->h.len + 1);
            a->iitems[a->h.len++] = int_val_full(v);
            return;
        }
        a->flags |= ARR_F_NOMIX;
        arr_ensure_aval(a);
    }
    arr_reserve(a, (uint64_t)a->h.len + 1);
    a->items[a->h.len++] = v;
}

/* Append preferring packed-int storage: on a packed array int values
   stay packed (non-int converts the whole array to aval); on an aval
   T_LIST an int value triggers a one-time all-int scan that promotes
   the array to packed storage when every existing element is an int. */
void arr_push_int(AArr *a, aval v) {
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (!is_int_val(v)) {
        arr_push(a, v);
        return;
    }
    if (arr_packed(a)) {
        arr_reserve_i64(a, (uint64_t)a->h.len + 1);
        a->iitems[a->h.len++] = int_val_full(v);
        return;
    }
    if (a->h.type == T_LIST && !(a->flags & ARR_F_NOMIX)) {
        int all_int = 1;
        for (uint64_t i = 0; i < a->h.len; ++i)
            if (!is_int_val(a->items[i])) { all_int = 0; break; }
        if (all_int) {
            arr_to_packed(a);
            arr_reserve_i64(a, (uint64_t)a->h.len + 1);
            a->iitems[a->h.len++] = int_val_full(v);
            return;
        }
        a->flags |= ARR_F_NOMIX;
    }
    arr_push(a, v);
}

ADict *dict_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_DICT)
        panicf("%s must be dict, not '%s'", what, type_name_cstr(v));
    return (ADict *)o;
}

/* ------------------------------------------------------------------ */
/* dict hash index                                                     */
/* ------------------------------------------------------------------ */
/* pairs[] preserves insertion order; slots[] is an open-addressed table
   (linear probing, power-of-two scap, 0 = empty, else pair index + 1)
   that maps each pair's cached hash to its index, giving O(1) lookup.

   Hash/equality consistency contract with values_equal:
   - numbers: values_equal compares int-int by int64 and int-float by
     double, so all numeric keys hash by the canonical double value
     (bits of num_to_double, -0.0 folded to +0.0). Equal keys always
     produce equal doubles, hence equal hashes; unequal keys may collide
     harmlessly (verified by values_equal).
   - str/char: hash the bytes; equal strings have equal bytes.
   - None/bool: hash the aval constant.
   - every other kind (list/tuple/dict/set/view/range/fnon/env/result/
     pyobj): a single constant hash. values_equal does deep equality and
     views compare equal across list/tuple/view kinds, so these share one
     hash domain; lookups among them probe one cluster and degrade to
     the old linear scan confined to that cluster (still verified by
     values_equal). Complex keys can never equal a simple key, so the
     domains never need to merge. */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

static uint64_t dict_key_hash(aval k) {
    if (is_num(k)) {
        double d = num_to_double(k);
        if (d == 0.0) d = 0.0; /* -0.0 == +0.0 under values_equal */
        union { double d; uint64_t u; } c;
        c.d = d;
        return mix64(c.u);
    }
    if (a_is_none(k) || k == A_TRUE || k == A_FALSE)
        return mix64(k ^ UINT64_C(0x517cc1b727220a95));
    AObj *o = obj_of(k);
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        /* FNV-1a over the bytes, then final mix. */
        uint64_t h = UINT64_C(0xcbf29ce484222325);
        const char *p = ((AStr *)o)->data;
        for (uint32_t i = 0; i < o->len; ++i) {
            h ^= (unsigned char)p[i];
            h *= UINT64_C(0x100000001b3);
        }
        return mix64(h ^ ((uint64_t)o->type << 56));
    }
    /* complex/unhashable-by-value keys: one shared domain (see above) */
    return UINT64_C(0x9e3779b97f4a7c15);
}

/* Rebuild slots from pairs[0..len) using the cached phash values.
   ncap must be a power of two with load factor < 1. */
static void dict_rehash(ADict *d, uint64_t ncap) {
    free(d->slots);
    d->slots = (uint32_t *)xmalloc((size_t)ncap * sizeof(uint32_t));
    memset(d->slots, 0, (size_t)ncap * sizeof(uint32_t));
    d->scap = ncap;
    uint64_t mask = ncap - 1;
    for (uint64_t i = 0; i < d->h.len; ++i) {
        uint64_t s = d->phash[i] & mask;
        while (d->slots[s]) s = (s + 1) & mask;
        d->slots[s] = (uint32_t)(i + 1);
    }
}

static void dict_reserve(ADict *d, uint64_t need) {
    if (need > d->h.cap) {
        uint64_t nc = d->h.cap ? d->h.cap : 4;
        while (nc < need) nc *= 2;
        d->pairs = (APair *)xrealloc(d->pairs, (size_t)nc * sizeof(APair));
        d->phash = (uint64_t *)xrealloc(d->phash, (size_t)nc * sizeof(uint64_t));
        d->h.cap = nc;
    }
    /* keep the probe table below ~70% load so lookups stay short */
    if (d->scap == 0 || need * 10 > d->scap * 7) {
        uint64_t ns = d->scap ? d->scap : 8;
        while (need * 10 > ns * 7) ns *= 2;
        dict_rehash(d, ns);
    }
}

static int64_t dict_find_h(ADict *d, aval k, uint64_t h) {
    if (d->scap == 0) return -1;
    uint64_t mask = d->scap - 1;
    uint64_t s = h & mask;
    while (d->slots[s]) {
        uint32_t i = d->slots[s] - 1;
        if (d->phash[i] == h && values_equal(d->pairs[i].k, k))
            return (int64_t)i;
        s = (s + 1) & mask;
    }
    return -1;
}

int64_t dict_find(ADict *d, aval k) {
    return dict_find_h(d, k, dict_key_hash(k));
}

void dict_put(ADict *d, aval k, aval v) {
    uint64_t h = dict_key_hash(k);
    int64_t i = dict_find_h(d, k, h);
    if (i >= 0) {
        d->pairs[i].v = v;
        return;
    }
    if (d->h.len == UINT32_MAX) panicf("dict too large");
    dict_reserve(d, (uint64_t)d->h.len + 1);
    uint64_t idx = d->h.len;
    d->pairs[idx].k = k;
    d->pairs[idx].v = v;
    d->phash[idx] = h;
    d->h.len++;
    uint64_t mask = d->scap - 1;
    uint64_t s = h & mask;
    while (d->slots[s]) s = (s + 1) & mask;
    d->slots[s] = (uint32_t)(idx + 1);
}

/* Remove pair i preserving insertion order of the rest; rebuilds the
   index because every pair index above i shifts down by one. */
void dict_remove_at(ADict *d, uint64_t i) {
    uint64_t tail = d->h.len - i - 1;
    memmove(d->pairs + i, d->pairs + i + 1, (size_t)tail * sizeof(APair));
    memmove(d->phash + i, d->phash + i + 1, (size_t)tail * sizeof(uint64_t));
    d->h.len--;
    if (d->scap) dict_rehash(d, d->scap);
}

void dict_clear(ADict *d) {
    d->h.len = 0;
    if (d->scap) memset(d->slots, 0, (size_t)d->scap * sizeof(uint32_t));
}

int set_contains(AArr *s, aval v) {
    for (uint64_t i = 0; i < s->h.len; ++i)
        if (values_equal(s->items[i], v)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* type names / formatting                                             */
/* ------------------------------------------------------------------ */

int kind_of(aval v) {
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

const char *type_name_cstr(aval v) {
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

void format_into(Buf *b, aval v, int quote) {
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
            arr_ensure_aval(a);
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
            arr_ensure_aval(a);
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
        default: {
            AObj *o = obj_of(v);
            if (o && o->type == T_PYBUF) {
                APyBuf *pb = (APyBuf *)o;
                buf_adds(b, "<pybuffer ");
                buf_addi64(b, pb->count);
                buf_adds(b, pb->readonly ? " readonly>" : ">");
                return;
            }
            if (o && o->type == T_PYOBJ) {
                /* Render via Python str()/repr() so opaque objects print
                   like they would in the interpreter. */
                aval s = quote ? a_py_repr(v) : a_py_str(v);
                format_into(b, s, quote);
                return;
            }
            buf_adds(b, "<object>");
            return;
        }
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

