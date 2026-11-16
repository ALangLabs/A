#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

AStr *str_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_STR && o->type != T_CHAR && o->type != T_ROPE))
        panicf("%s must be str, not '%s'", what, type_name_cstr(v));
    return astr_flat(v);
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
    AObj *o = obj_of(v);
    AStr *s = str_of(v, "string");
    if (len) *len = s->h.len;
    astr_cp_uncache(s);  /* the caller is about to overwrite bytes */
    if (o->type == T_ROPE) astr_cp_uncache((AStr *)o); /* rope cp cache too */
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
    /* Single-byte needle (very common: count("o"), split(" "), replace):
       one memchr is the whole search — skip the memcmp-per-hit loop. */
    if (nn == 1) {
        const void *hit = memchr(h + from, n[0], hn - from);
        if (!hit) return NULL;
        *pos = (size_t)((const char *)hit - h);
        return (const char *)hit;
    }
    /* memchr for the first byte (SIMD in libc) instead of memcmp at every
       offset: a 200 KB contains() miss cost 2 ms with the naive loop. */
    size_t i = from;
    while (i + nn <= hn) {
        const void *hit = memchr(h + i, n[0], hn - nn - i + 1);
        if (!hit) return NULL;
        size_t cand = (size_t)((const char *)hit - h);
        /* Cheap last-byte check before the memcmp call: when the first
           byte is common but the tail differs (e.g. "ab" in "aaaa..."),
           most candidates die here without entering memcmp. */
        if (h[cand + nn - 1] == n[nn - 1] &&
            memcmp(h + cand, n, nn) == 0) {
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

/* ------------------------------------------------------------------ */
/* ropes: O(1) string concatenation for append loops                    */
/* ------------------------------------------------------------------ */

static int astr_is_strkind(AObj *o) {
    return o && (o->type == T_STR || o->type == T_CHAR || o->type == T_ROPE);
}

aval a_str_cat(aval l, aval r) {
    AObj *lo = obj_of(l), *ro = obj_of(r);
    if (!astr_is_strkind(lo) || !astr_is_strkind(ro))
        panicf("string concat operands must be str");
    uint64_t ln = lo->len, rn = ro->len;
    if (ln + rn > (uint64_t)UINT32_MAX) panicf("string too large");
    if (lo->type != T_ROPE && ro->type != T_ROPE &&
        ln + rn <= A_STR_ROPE_MIN) {
        /* Small result: same exact-size copy the old concat_str did. */
        aval outv = a_str_new_uninit(ln + rn);
        AStr *o = (AStr *)obj_of(outv);
        if (ln) memcpy(o->data, astr_flat(l)->data, (size_t)ln);
        if (rn) memcpy(o->data + ln, astr_flat(r)->data, (size_t)rn);
        return outv;
    }
    ARope *p = (ARope *)xmalloc(sizeof(ARope));
    p->h.type = T_ROPE;
    p->h.len = (uint32_t)(ln + rn);
    p->h.cap = 0;
    p->left = l;
    p->right = r;
    p->flat = A_NONE;
    return mkobj(&p->h);
}

/* Flatten a rope into its canonical flat AStr (cached in r->flat).
   Iterative in-order traversal: append loops build ropes tens of
   thousands of nodes deep, so recursion would overflow the C stack.
   A rope child that already has a flat is a leaf (one memcpy); sharing
   (DAG shapes like `s = s + s`) is handled naturally — a shared subtree
   with a cached flat is copied once per occurrence, which is exactly
   what the output requires. */
static void rope_flatten(ARope *r) {
    if (r->flat != A_NONE) return;
    aval fv = a_str_new_uninit(r->h.len);
    AStr *f = (AStr *)obj_of(fv);
    char *d = f->data;
    size_t n = 0, cap = 256;
    AObj **st = (AObj **)xmalloc(cap * sizeof(AObj *));
    st[n++] = &r->h;
    while (n) {
        AObj *o = st[--n];
        if (o->type == T_ROPE) {
            ARope *q = (ARope *)o;
            if (q->flat != A_NONE) {
                AStr *fs = (AStr *)obj_of(q->flat);
                memcpy(d, fs->data, (size_t)q->h.len);
                d += q->h.len;
                continue;
            }
            if (n + 2 > cap) {
                cap *= 2;
                st = (AObj **)xrealloc(st, cap * sizeof(AObj *));
            }
            /* Push right first so left is visited first (in-order). */
            st[n++] = obj_of(q->right);
            st[n++] = obj_of(q->left);
            continue;
        }
        AStr *s = (AStr *)o;
        if (s->h.len) memcpy(d, s->data, (size_t)s->h.len);
        d += s->h.len;
    }
    free(st);
    r->flat = fv;
}

AStr *astr_flat(aval v) {
    AObj *o = obj_of(v);
    if (o && o->type == T_ROPE) {
        ARope *r = (ARope *)o;
        rope_flatten(r);
        return (AStr *)obj_of(r->flat);
    }
    return (AStr *)o;
}

/* Code-point count of a flat string-kind object: cache hit, else a u8_count
   scan cached in the cap word (same scheme a_len always used). Returns
   UINT32_MAX for an unflattened rope — callers that need the count go
   through a_str_cpcount/rope_cp instead. */
static uint32_t astr_cp_flat(AObj *o) {
    AStr *s = (AStr *)o;
    uint32_t c = astr_cp_cached(s);
    if (c) return c - 1u;
    uint32_t cp = (uint32_t)u8_count(s->data, s->h.len);
    astr_cp_cache(s, cp);
    return cp;
}

/* cp of any string-kind object; UINT32_MAX when `o` is a rope whose count
   has not been computed yet. */
static uint32_t astr_cp_known(AObj *o) {
    if (o->type == T_ROPE) {
        uint32_t c = astr_cp_cached((AStr *)o);
        return c ? c - 1u : UINT32_MAX;
    }
    return astr_cp_flat(o);
}

/* Code-point count of a rope, cached per node in h.cap's high half
   (cp+1, 0 = unknown — same layout as AStr). Computed by an iterative
   post-order pass so deep append chains cannot overflow the C stack and
   shared subtrees (DAGs) are counted once. Amortized O(1): each rope
   node's count is computed at most once, from children's cached counts
   plus one u8_count per flat leaf. */
static uint32_t rope_cp(ARope *root) {
    uint32_t c = astr_cp_cached((AStr *)root);
    if (c) return c - 1u;
    typedef struct { AObj *o; uint32_t expanded; } CpFr;
    size_t n = 0, cap = 256;
    CpFr *st = (CpFr *)xmalloc(cap * sizeof(CpFr));
    st[n++] = (CpFr){ &root->h, 0 };
    while (n) {
        CpFr f = st[--n];
        AObj *o = f.o;
        if (o->type != T_ROPE) continue;
        ARope *q = (ARope *)o;
        uint32_t lc = astr_cp_known(obj_of(q->left));
        uint32_t rc = astr_cp_known(obj_of(q->right));
        if (lc != UINT32_MAX && rc != UINT32_MAX) {
            astr_cp_cache((AStr *)q, lc + rc);
            continue;
        }
        if (!f.expanded) {
            if (n + 1 > cap) {
                cap *= 2;
                st = (CpFr *)xrealloc(st, cap * sizeof(CpFr));
            }
            st[n++] = (CpFr){ o, 1 };
            /* Push rope children whose count is still unknown; flat
               children and already-counted ropes resolve via astr_cp_known
               when the expanded frame is revisited. */
            AObj *kids[2] = { obj_of(q->left), obj_of(q->right) };
            for (int i = 0; i < 2; ++i) {
                AObj *k = kids[i];
                if (k->type == T_ROPE && astr_cp_cached((AStr *)k) == 0) {
                    if (n + 1 > cap) {
                        cap *= 2;
                        st = (CpFr *)xrealloc(st, cap * sizeof(CpFr));
                    }
                    st[n++] = (CpFr){ k, 0 };
                }
            }
        } else {
            /* Children were processed above this frame: their counts are
               cached now, so this resolves. If a child is somehow still
               unknown (cannot happen — every unknown rope child was
               pushed), leave the node uncached rather than storing a
               wrong count. */
            lc = astr_cp_known(obj_of(q->left));
            rc = astr_cp_known(obj_of(q->right));
            if (lc != UINT32_MAX && rc != UINT32_MAX)
                astr_cp_cache((AStr *)q, lc + rc);
        }
    }
    free(st);
    c = astr_cp_cached((AStr *)root);
    return c ? c - 1u : 0;
}

uint32_t a_str_cpcount(aval v) {
    AObj *o = obj_of(v);
    if (o->type == T_ROPE) return rope_cp((ARope *)o);
    return astr_cp_flat(o);
}

