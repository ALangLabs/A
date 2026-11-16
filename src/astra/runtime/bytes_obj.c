/* First-class bytes: a growable, NUL-terminated byte buffer.
 *
 * Layout: AObj header (h.len = byte count, h.cap = allocated capacity) followed
 * by the data pointer. `data` is always NUL-terminated for C interop, but the
 * length is authoritative, so payloads with embedded NULs round-trip exactly.
 *
 * Bytes never cost 8 bytes per payload byte the way `[i64]` does: indexing,
 * len, iteration and slicing dispatch on the object kind, so a 1 MiB body is
 * one 1 MiB allocation with no expansion pass.
 */
#include "a_runtime_internal.h"

ABytes *a_bytes_obj(aval v) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_BYTES) return NULL;
    return (ABytes *)o;
}

int a_bytes_is(aval v) { return a_bytes_obj(v) != NULL; }

/* (Re)allocate to hold at least `need` bytes plus the NUL terminator. */
void a_bytes_grow(ABytes *b, uint64_t need) {
    uint64_t want = need + 1;
    if (want <= b->h.cap) return;
    uint64_t cap = b->h.cap ? b->h.cap : 16;
    while (cap < want) {
        if (cap > (uint64_t)1 << 62) { cap = want; break; }
        cap *= 2;
    }
    b->data = (char *)xrealloc(b->data, (size_t)cap);
    b->h.cap = cap;
}

aval a_bytes_new(uint64_t cap) {
    ABytes *b = (ABytes *)xmalloc(sizeof(ABytes));
    b->h.type = T_BYTES;
    b->h.len = 0;
    b->h.cap = 0;
    b->data = NULL;
    if (cap) a_bytes_grow(b, cap);
    else {
        b->data = (char *)xmalloc(1);
        b->data[0] = 0;
        b->h.cap = 1;
    }
    b->data[0] = 0;
    return mkobj(&b->h);
}

aval a_bytes_from(const char *p, uint64_t n) {
    aval v = a_bytes_new(n);
    ABytes *b = a_bytes_obj(v);
    if (n) memcpy(b->data, p, (size_t)n);
    b->h.len = (uint32_t)n;
    b->data[n] = 0;
    return v;
}

/* Adopt a heap buffer: no copy. `cap` is the allocation size (0 = unknown). */
aval a_bytes_take(char *p, uint64_t n, uint64_t cap) {
    ABytes *b = (ABytes *)xmalloc(sizeof(ABytes));
    b->h.type = T_BYTES;
    b->h.len = (uint32_t)n;
    if (cap < n + 1) cap = n + 1;
    if (p && cap != 0) p = (char *)xrealloc(p, (size_t)cap);
    else if (!p) { cap = n + 1; p = (char *)xmalloc((size_t)cap); }
    p[n] = 0;
    b->data = p;
    b->h.cap = cap;
    return mkobj(&b->h);
}

void a_bytes_append(ABytes *b, const char *p, uint64_t n) {
    if (!n) return;
    a_bytes_grow(b, (uint64_t)b->h.len + n);
    memcpy(b->data + b->h.len, p, (size_t)n);
    b->h.len = (uint32_t)((uint64_t)b->h.len + n);
    b->data[b->h.len] = 0;
}

void a_bytes_pushc(ABytes *b, int c) {
    a_bytes_grow(b, (uint64_t)b->h.len + 1);
    b->data[b->h.len++] = (char)(c & 0xff);
    b->data[b->h.len] = 0;
}

char *a_bytes_data(aval v, uint64_t *len) {
    ABytes *b = a_bytes_obj(v);
    if (!b) { if (len) *len = 0; return NULL; }
    if (len) *len = (uint64_t)b->h.len;
    return b->data;
}

/* Bytes -> String when the payload is valid UTF-8, else A_NONE. One pass, no
   intermediate buffer: the String gets the same bytes. */
aval a_bytes_to_str(aval v) {
    ABytes *b = a_bytes_obj(v);
    if (!b) return A_NONE;
    const char *p = b->data ? b->data : "";
    uint64_t n = (uint64_t)b->h.len;
    if (!a_utf8_valid(p, n)) return A_NONE;
    aval s = a_str_new_uninit(n);
    uint64_t slen = 0;
    char *dst = str_mut_data(s, &slen);
    if (n) memcpy(dst, p, (size_t)n);
    return s;
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

/* Bytes()            empty buffer
   Bytes(n: i64)      n zero bytes
   Bytes(s: String)   UTF-8 encoding of s
   Bytes(xs: [i64])   one byte per element (values are masked to 0..255)
   Bytes(b: Bytes)    copy of b */
aval bi_bytes_new(uint64_t n, aval *argv) {
    if (n == 0) return a_bytes_new(64);
    if (n > 1) want_args("Bytes", n, 0, 1);
    aval src = argv[0];
    ABytes *sb = a_bytes_obj(src);
    if (sb) return a_bytes_from(sb->data ? sb->data : "", (uint64_t)sb->h.len);
    if (is_int_val(src)) {
        int64_t count = int_val_full(src);
        if (count < 0) panicf("Bytes: length must be non-negative, got %lld", (long long)count);
        aval v = a_bytes_new((uint64_t)count);
        ABytes *b = a_bytes_obj(v);
        if (count) memset(b->data, 0, (size_t)count);
        b->h.len = (uint32_t)count;
        b->data[count] = 0;
        return v;
    }
    int k = kind_of(src);
    if (k == K_STR || k == K_CHAR) {
        uint64_t len = 0;
        const char *p = str_data(src, &len);
        return a_bytes_from(p ? p : "", len);
    }
    if (k == K_LIST || k == K_TUPLE || k == K_SET) {
        aval tmp = (k == K_SET) ? a_list_from_iter(src) : src;
        AArr *a = (AArr *)obj_of(tmp);
        if (!a) return a_bytes_new(16);
        uint64_t count = a->h.len;
        aval v = a_bytes_new(count);
        ABytes *b = a_bytes_obj(v);
        if (arr_packed(a)) {
            for (uint64_t i = 0; i < count; ++i)
                b->data[i] = (char)(a->iitems[i] & 0xff);
        } else {
            arr_ensure_aval(a);
            for (uint64_t i = 0; i < count; ++i)
                b->data[i] = (char)(expect_int64(a->items[i], "a byte value") & 0xff);
        }
        b->h.len = (uint32_t)count;
        b->data[count] = 0;
        return v;
    }
    panicf("Bytes() cannot build from '%s'", type_name_cstr(src));
    return A_NONE;
}

/* bytes_append(dst: Bytes, src) -> None; dst is grown in place. src may be a
   Bytes buffer, a String, an [i64] byte array, or a single integer. */
aval bi_bytes_append(uint64_t n, aval *argv) {
    want_args("bytes_append", n, 2, 2);
    ABytes *dst = a_bytes_obj(argv[0]);
    if (!dst) panicf("bytes_append: first argument must be Bytes, got '%s'",
                     type_name_cstr(argv[0]));
    aval src = argv[1];
    ABytes *sb = a_bytes_obj(src);
    if (sb) { a_bytes_append(dst, sb->data ? sb->data : "", (uint64_t)sb->h.len); return A_NONE; }
    if (is_int_val(src)) { a_bytes_pushc(dst, (int)int_val_full(src)); return A_NONE; }
    int k = kind_of(src);
    if (k == K_STR || k == K_CHAR) {
        uint64_t len = 0;
        const char *p = str_data(src, &len);
        a_bytes_append(dst, p ? p : "", len);
        return A_NONE;
    }
    if (k == K_LIST || k == K_TUPLE) {
        AArr *a = (AArr *)obj_of(src);
        if (!a) return A_NONE;
        uint64_t count = a->h.len;
        a_bytes_grow(dst, (uint64_t)dst->h.len + count);
        char *out = dst->data + dst->h.len;
        if (arr_packed(a)) {
            for (uint64_t i = 0; i < count; ++i) out[i] = (char)(a->iitems[i] & 0xff);
        } else {
            arr_ensure_aval(a);
            for (uint64_t i = 0; i < count; ++i)
                out[i] = (char)(expect_int64(a->items[i], "a byte value") & 0xff);
        }
        dst->h.len = (uint32_t)((uint64_t)dst->h.len + count);
        dst->data[dst->h.len] = 0;
        return A_NONE;
    }
    panicf("bytes_append: cannot append '%s'", type_name_cstr(src));
    return A_NONE;
}

/* bytes_to_str(b) -> String? (None when the payload is not valid UTF-8). */
aval bi_bytes_to_str(uint64_t n, aval *argv) {
    want_args("bytes_to_str", n, 1, 1);
    if (a_bytes_is(argv[0])) return a_bytes_to_str(argv[0]);
    return bi_bytes_decode_utf8(1, argv);
}
