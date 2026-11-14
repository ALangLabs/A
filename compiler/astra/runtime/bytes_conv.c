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
        /* ASCII fast path: eight bytes per iteration while all high bits are
           clear. Text payloads are almost entirely ASCII, and the byte loop
           below runs at ~560 MB/s, which would dominate a 1 MiB body. */
        if (i + 8 <= len) {
            uint64_t w;
            memcpy(&w, s + i, 8);
            if (!(w & 0x8080808080808080ull)) { i += 8; continue; }
        }
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

aval bi_bytes_decode_utf8(uint64_t n, aval *argv) {
    want_args("bytes_decode_utf8", n, 1, 1);
    if (a_bytes_is(argv[0])) return a_bytes_to_str(argv[0]);
    AArr *a = (AArr *)obj_of(argv[0]);
    if (!a) return a_str_new("", 0);
    uint64_t len = a->h.len;
    /* Unpack straight into the result string: the old path malloc'd a raw
       buffer, filled it from the [i64] array, validated it, then memcpy'd
       into the AStr and freed — three passes and two allocations. Values
       outside 0..255 are not bytes; report that through A_NONE exactly
       like the pure-A version does instead of silently masking bits. */
    aval outv = a_str_new_uninit(len);
    uint64_t olen = 0;
    char *dst = str_mut_data(outv, &olen);
    if (arr_packed(a)) {
        for (uint64_t i = 0; i < len; ++i) {
            int64_t b = a->iitems[i];
            if (b < 0 || b > 255) return A_NONE;
            dst[i] = (char)b;
        }
    } else {
        arr_ensure_aval(a);
        for (uint64_t i = 0; i < len; ++i) {
            int64_t b = expect_int64(a->items[i], "a byte value");
            if (b < 0 || b > 255) return A_NONE;
            dst[i] = (char)b;
        }
    }
    if (!a_utf8_valid(dst, len)) return A_NONE;
    return outv;
}

aval bi_str_encode_utf8(uint64_t n, aval *argv) {
    want_args("str_encode_utf8", n, 1, 1);
    uint64_t len = 0;
    const char *p = str_data(argv[0], &len);
    if (!p) p = "";
    /* the string is already UTF-8, so its bytes are the answer; copying keeps
       the A value independent of the string object's lifetime. Build the
       packed int64 list directly: the old path tagged every byte into an
       aval array and then memcpy'd it into the list — two passes and 8
       bytes of scratch per byte on top of the result. */
    int64_t *items = (int64_t *)xmalloc((len ? len : 1) * sizeof(int64_t));
    for (uint64_t i = 0; i < len; ++i)
        items[i] = (unsigned char)p[i];
    return a_list_take_i64(items, len);
}
