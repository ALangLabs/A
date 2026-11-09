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
/* 128 KiB chunks: small enough that a hello-world program's RSS floor
   stays far below Python/Rust, large enough that refills stay amortized
   (each chunk is one GC registry block + arena range). */
static const size_t kSmallStrChunk = 128u << 10;
/* A_RT_TLS: the bump cursor is per-thread so parallel map/filter workers
   can allocate small strings without racing (rt_pool.c safety contract). */
static A_RT_TLS char *g_small_str = NULL;
static A_RT_TLS char *g_small_str_end = NULL;

/* Bump-allocate one AStr header+data from the small-string arena.
   Returns NULL when the object does not fit the <=256-byte arena class
   (caller falls back to xmalloc). `type` is T_STR or T_CHAR; both are
   pointer-free immutable objects, so they share the arena safely. */
static AStr *str_arena_alloc(uint64_t len, uint32_t type) {
    size_t need = sizeof(AStr) + (size_t)len + 1;
    if (need > 256) return NULL;
    need = (need + 15) & ~(size_t)15;
    if (g_small_str && a_gc_arena_stale(g_small_str)) g_small_str = NULL;
    size_t avail = g_small_str ? (size_t)(g_small_str_end - g_small_str) : 0;
    if (avail < need) {
        void *old = g_small_str;
        g_small_str = xarena(kSmallStrChunk);
        a_gc_arena_switch(old, g_small_str);
        g_small_str_end = g_small_str + kSmallStrChunk;
    }
    AStr *s = (AStr *)g_small_str;
    g_small_str += need;
    s->h.type = type;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    return s;
}

aval a_str_new(const char *p, uint64_t len) {
    if (len && !p) panicf("null string data");
    AStr *s = str_arena_alloc(len, T_STR);
    if (!s) {
        s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
        s->h.type = T_STR;
        s->h.len = (uint32_t)len;
        s->h.cap = len + 1;
    }
    if (len) memcpy(s->data, p, (size_t)len);
    s->data[len] = '\0';
    return mkobj(&s->h);
}

aval a_str_new_uninit(uint64_t len) {
    AStr *s = str_arena_alloc(len, T_STR);
    if (!s) {
        s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
        s->h.type = T_STR;
        s->h.len = (uint32_t)len;
        s->h.cap = len + 1;
    }
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
    /* Chars are 1-4 byte UTF-8 sequences: they always fit the small-string
       arena, so s.chars() no longer pays an xmalloc (and its GC check)
       per character. */
    AStr *s = str_arena_alloc(len, T_CHAR);
    if (!s) {
        s = (AStr *)xmalloc(sizeof(AStr) + (size_t)len + 1);
        s->h.type = T_CHAR;
        s->h.len = (uint32_t)len;
        s->h.cap = len + 1;
    }
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
        /* ASCII fast path: 8 code points per step. Only skip when the
           remaining count covers the whole word, so the result stays
           exact at the boundary. */
        if (i + 8 <= n && c + 8 <= (uint64_t)cp && ascii_word(p + i)) {
            i += 8;
            c += 8;
            continue;
        }
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

/* Rightmost byte == c in p[0..n), or NULL. Word-at-a-time scan from the
   end; portable (memrchr is a GNU/BSD extension absent on MSVC). The
   haszero bit trick can only over-report, and the verify loop re-checks
   real bytes, so no false hits are possible. */
static const char *memrchr8(const char *p, size_t n, unsigned char c) {
    const char *base = p;
    p += n;
    uint64_t pat = UINT64_C(0x0101010101010101) * c;
    while ((size_t)(p - base) >= 8) {
        uint64_t w;
        memcpy(&w, p - 8, 8);
        uint64_t x = w ^ pat;
        uint64_t m = (x - UINT64_C(0x0101010101010101)) & ~x &
                     UINT64_C(0x8080808080808080);
        if (m) {
            for (int k = 7; k >= 0; --k)
                if (p[-8 + k] == (char)c) return p - 8 + k;
        }
        p -= 8;
    }
    while (p > base) {
        --p;
        if ((unsigned char)*p == c) return p;
    }
    return NULL;
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
    /* Reverse search: jump between first-byte candidates with a
       word-at-a-time backward memchr and memcmp only on candidates.
       Keeps the old loop's early exit (a match near the end is found in
       a few words) while scanning ~8x faster on misses. */
    if (nn > hn) return -1;
    int64_t i = (int64_t)(hn - nn);
    while (i >= (int64_t)st_byte) {
        const char *hit = memrchr8(h + st_byte, (size_t)(i - (int64_t)st_byte + 1),
                                   (unsigned char)n[0]);
        if (!hit) return -1;
        size_t cand = (size_t)(hit - h);
        if (memcmp(h + cand, n, (size_t)nn) == 0)
            return (int64_t)u8_count(h, cand);
        i = (int64_t)cand - 1;
    }
    return -1;
}

aval str_repeat(aval s, int64_t count) {
    uint64_t n;
    const char *p = str_data(s, &n);
    if (count <= 0 || n == 0) return a_str_new("", 0);
    if ((uint64_t)count > UINT64_MAX / n)
        panicf("repeated string is too large");
    /* Fill the result directly: routing through a Buf meant allocating the
       full output twice (Buf growth + buf_take's copy into the AStr).
       Exponential self-copy: after the first seed copy, each memcpy doubles
       the filled region, so "x"*1000 costs ~10 memcpy calls instead of
       1000 tiny ones (call overhead dominated the old loop). */
    uint64_t total = n * (uint64_t)count;
    aval outv = a_str_new_uninit(total);
    AStr *out = (AStr *)obj_of(outv);
    char *d = out->data;
    memcpy(d, p, (size_t)n);
    uint64_t filled = n;
    while (filled < total) {
        uint64_t c = filled < total - filled ? filled : total - filled;
        memcpy(d + filled, d, (size_t)c);
        filled += c;
    }
    return outv;
}

