#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

AStr *str_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_STR && o->type != T_CHAR))
        panicf("%s must be str, not '%s'", what, type_name_cstr(v));
    return (AStr *)o;
}

/* Small immutable strings are bump-allocated from 1 MiB chunks: str(int)
   and other short conversions dominate allocation-heavy benchmarks, and
   AStr objects are never individually freed. Large strings still use
   xmalloc so a few big allocations cannot waste arena space. */
/* 2 MiB: one huge page per chunk, so refills are cheap. */
static const size_t kSmallStrChunk = 2u << 20;
/* A_RT_TLS: the bump cursor is per-thread so parallel map/filter workers
   can allocate small strings without racing (rt_pool.c safety contract). */
static A_RT_TLS char *g_small_str = NULL;
static A_RT_TLS char *g_small_str_end = NULL;

aval a_str_new(const char *p, uint64_t len) {
    if (len && !p) panicf("null string data");
    size_t need = sizeof(AStr) + (size_t)len + 1;
    if (need <= 256) {
        need = (need + 15) & ~(size_t)15;
        size_t avail = g_small_str ? (size_t)(g_small_str_end - g_small_str) : 0;
        if (avail < need) {
            g_small_str = xarena(kSmallStrChunk);
            g_small_str_end = g_small_str + kSmallStrChunk;
        }
        AStr *s = (AStr *)g_small_str;
        g_small_str += need;
        s->h.type = T_STR;
        s->h.len = (uint32_t)len;
        s->h.cap = len + 1;
        if (len) memcpy(s->data, p, (size_t)len);
        s->data[len] = '\0';
        return mkobj(&s->h);
    }
    AStr *s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
    s->h.type = T_STR;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    if (len) memcpy(s->data, p, (size_t)len);
    s->data[len] = '\0';
    return mkobj(&s->h);
}

aval a_str_new_uninit(uint64_t len) {
    size_t need = sizeof(AStr) + (size_t)len + 1;
    AStr *s;
    if (need <= 256) {
        need = (need + 15) & ~(size_t)15;
        size_t avail = g_small_str ? (size_t)(g_small_str_end - g_small_str) : 0;
        if (avail < need) {
            g_small_str = xarena(kSmallStrChunk);
            g_small_str_end = g_small_str + kSmallStrChunk;
        }
        s = (AStr *)g_small_str;
        g_small_str += need;
    } else {
        s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
    }
    s->h.type = T_STR;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    s->data[len] = '\0';
    return mkobj(&s->h);
}

/* Writable view of a string's storage, for native code that fills the buffer
   it just allocated (net.recv_body reads a body straight into the string). */
char *str_mut_data(aval v, uint64_t *len) {
    AStr *s = str_of(v, "string");
    if (len) *len = s->h.len;
    astr_cp_uncache(s);  /* the caller is about to overwrite bytes */
    return s->data;
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
/* Word-at-a-time helper: 1 when all eight bytes are ASCII (no high bits). */
static int ascii_word(const char *p) {
    uint64_t w;
    memcpy(&w, p, sizeof w);
    return !(w & 0x8080808080808080ull);
}

uint64_t u8_count(const char *p, uint64_t n) {
    uint64_t c = 0;
    uint64_t i = 0;
    while (i < n) {
        /* ASCII fast path: text is almost always ASCII, and this loop runs for
           every find/contains/slice index conversion. */
        if (i + 8 <= n && ascii_word(p + i)) {
            i += 8;
            c += 8;
            continue;
        }
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
    /* memchr for the first byte (SIMD in libc) instead of memcmp at every
       offset: a 200 KB contains() miss cost 2 ms with the naive loop. */
    size_t i = from;
    while (i + nn <= hn) {
        const void *hit = memchr(h + i, n[0], hn - nn - i + 1);
        if (!hit) return NULL;
        size_t cand = (size_t)((const char *)hit - h);
        if (memcmp(h + cand, n, nn) == 0) {
            *pos = cand;
            return h + cand;
        }
        i = cand + 1;
    }
    return NULL;
}

int64_t str_find(aval hay, aval needle, int64_t start, int reverse) {
    uint64_t hn, nn;
    const char *h = str_data(hay, &hn), *n = str_data(needle, &nn);
    /* The codepoint count is only needed for negative starts, empty needles
       and the "start past the end" check: counting the whole haystack first
       made every contains()/find() two full passes. */
    int64_t st = start;
    if (st < 0) {
        st += (int64_t)u8_count(h, hn);
        if (st < 0) st = 0;
    }
    if (nn == 0) {
        if (st > (int64_t)u8_count(h, hn)) return -1;
        return st;
    }
    uint64_t st_byte = st == 0 ? 0 : cp_to_byte(h, hn, st);
    if (st_byte >= hn) return -1;
    if (!reverse) {
        size_t pos = 0;
        const char *p = find_bytes(h, (size_t)hn, n, (size_t)nn, st_byte, &pos);
        if (!p) return -1;
        return (int64_t)u8_count(h, pos);
    }
    for (int64_t i = (int64_t)hn - (int64_t)nn; i >= (int64_t)st_byte; --i) {
        if (h[i] == n[0] && memcmp(h + i, n, (size_t)nn) == 0)
            return (int64_t)u8_count(h, (uint64_t)i);
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

