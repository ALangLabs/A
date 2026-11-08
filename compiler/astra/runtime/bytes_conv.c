/* bytes <-> string conversion builtins.

   std:bytes implements utf8_decode/s2b with per-byte loops that allocate one
   one-char string per byte (and, before the join() rewrite, concatenated
   quadratically). That is fine for a 20-byte token and unusable for an HTTP
   body: ~1s per MiB. These builtins do the same validation in a single pass
   over the bytes so decoding a multi-megabyte response costs microseconds.

   - bytes_decode_utf8(bytes) -> String?   (None when the bytes are not UTF-8)
   - str_encode_utf8(s)       -> [i64]     (UTF-8 bytes of a string)          */

#include "a_runtime_internal.h"

/* Copy an [i64] byte array into a fresh char buffer. Values outside 0..255
   are not bytes; report that through *bad so the caller can fail like the
   pure-A version does instead of silently masking bits off. */
/* Is `len` bytes of `p` well-formed UTF-8? Single pass, no allocation.
   Exported so net.recv_body can hand back a String instead of an [i64] byte
   array (8 bytes per byte) whenever the payload is text. */
int a_utf8_valid(const char *p, uint64_t len) {
    const unsigned char *s = (const unsigned char *)p;
    uint64_t i = 0;
    while (i < len) {
        unsigned char b = s[i];
        if (b < 0x80) { i += 1; continue; }
        uint64_t need = (b & 0xE0) == 0xC0 ? 2
                       : (b & 0xF0) == 0xE0 ? 3
                       : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (!need || i + need > len) return 0;
        uint64_t cp = need == 2 ? (b & 0x1F) : need == 3 ? (b & 0x0F) : (b & 0x07);
        for (uint64_t k = 1; k < need; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
            (need == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += need;
    }
    return 1;
}

static char *conv_bytes_in(aval v, uint64_t *out_len, int *bad) {
    AArr *a = (AArr *)obj_of(v);
    *bad = 0;
    *out_len = 0;
    if (!a) return NULL;
    uint64_t n = a->h.len;
    char *buf = (char *)xmalloc(n ? n : 1);
    if (arr_packed(a)) {
        /* Socket reads hand back packed int64 storage; unpacking it into
           1M tagged values first would double the cost of every decode. */
        for (uint64_t i = 0; i < n; ++i) {
            int64_t b = a->iitems[i];
            if (b < 0 || b > 255) {
                free(buf);
                *bad = 1;
                return NULL;
            }
            buf[i] = (char)b;
        }
        *out_len = n;
        return buf;
    }
    arr_ensure_aval(a);
    for (uint64_t i = 0; i < n; ++i) {
        int64_t b = expect_int64(a->items[i], "a byte value");
        if (b < 0 || b > 255) {
            free(buf);
            *bad = 1;
            return NULL;
        }
        buf[i] = (char)b;
    }
    *out_len = n;
    return buf;
}

aval bi_bytes_decode_utf8(uint64_t n, aval *argv) {
    want_args("bytes_decode_utf8", n, 1, 1);
    uint64_t len = 0;
    int bad = 0;
    char *raw = conv_bytes_in(argv[0], &len, &bad);
    if (bad) return A_NONE;
    if (!raw) return a_str_new("", 0);

    int ok = a_utf8_valid(raw, len);
    aval out = ok ? a_str_new(raw, len) : A_NONE;
    free(raw);
    return out;
}

aval bi_str_encode_utf8(uint64_t n, aval *argv) {
    want_args("str_encode_utf8", n, 1, 1);
    uint64_t len = 0;
    const char *p = str_data(argv[0], &len);
    if (!p) p = "";
    /* the string is already UTF-8, so its bytes are the answer; copying keeps
       the A value independent of the string object's lifetime */
    aval *items = (aval *)xmalloc((len ? len : 1) * sizeof(aval));
    for (uint64_t i = 0; i < len; ++i)
        items[i] = mkint_full((unsigned char)p[i]);
    aval out = list_from_vec(items, len, 0);
    free(items);
    return out;
}
