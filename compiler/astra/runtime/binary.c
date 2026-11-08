#include "a_runtime_internal.h"

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
        arr_ensure_aval(a);
        bin_put(b, kind_of(v) == K_TUPLE ? BIN_TUPLE
                    : kind_of(v) == K_SET ? BIN_SET : BIN_ARRAY);
        bin_varint(b, a->h.len);
        for (uint32_t i = 0; i < a->h.len; ++i) bin_encode_val(b, a->items[i]);
        return;
    }
    case K_DICT: {
        ADict *d = (ADict *)obj_of(v);
        AObj *oo = obj_of(v);
        /* Structs carry their name in AStruct.name (not in the pairs);
           plain dicts are treated as struct-shaped when they hold a
           "__name" string key. Either way the name is written once as a
           leading string and skipped among the field pairs on encode;
           decode stores it back under "__name" so re-encoding is stable. */
        aval tn = A_NONE;
        const char *np = NULL;
        uint64_t nl = 0;
        int64_t skip = -1;
        if (oo->type == T_STRUCT) {
            tn = ((AStruct *)oo)->name;
            if (kind_of(tn) == K_STR) np = str_data(tn, &nl);
        } else {
            int64_t ni = dict_find(d, a_str_new("__name", 6));
            if (ni >= 0) {
                aval tv = d->pairs[ni].v;
                if (kind_of(tv) == K_STR) {
                    np = str_data(tv, &nl);
                    tn = tv;
                    skip = ni;
                }
            }
        }
        if (tn != A_NONE) {
            bin_put(b, BIN_STRUCT);
            bin_str(b, np, (size_t)nl);
            bin_varint(b, d->h.len - (skip >= 0 ? 1 : 0));
            for (uint32_t i = 0; i < d->h.len; ++i) {
                if ((int64_t)i == skip) continue;
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

typedef struct { uint8_t *p; size_t len, pos; int err; } BinReader;

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

aval dict_new_empty(void) {
    ADict *d = (ADict *)xmalloc(sizeof(ADict));
    d->h.type = T_DICT;
    d->h.len = 0;
    d->h.cap = 0;
    d->pairs = NULL;
    d->slots = NULL;
    d->scap = 0;
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
        if (r->err || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            r->err = 1;
            return A_NONE;
        }
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
        if (tag == BIN_SET) {
            aval set = a_set_new();
            for (uint64_t i = 0; i < n; ++i) a_set_add(set, items[i]);
            free(items);
            return set;
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
        /* "__name" is the key the encoder treats as the struct marker,
           so decode stores the decoded name there for round-trips. */
        dict_put(dd, a_str_new("__name", 6), tn);
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
        AResult *res = result_new(tag == BIN_OK, 0, v);
        return mkobj(&res->h);
    }
    default: r->err = 1; return A_NONE;
    }
}

aval bi_binary_encode(uint64_t n, aval *argv) {
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

aval bi_binary_decode(uint64_t n, aval *argv) {
    want_args("binary.decode", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE && o->type != T_SET))
        panicf("'binary.decode' expects an array of byte values (0-255)");
    AArr *a = (AArr *)o;
    BinReader r;
    r.p = (uint8_t *)xmalloc(a->h.len ? a->h.len : 1);
    r.len = r.pos = 0;
    r.err = 0;
    for (uint32_t i = 0; i < a->h.len; ++i) {
        int64_t byte = arr_packed(a) ? a->iitems[i] : int_val_full(a->items[i]);
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

