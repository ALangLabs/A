/* gc.c — conservative mark-sweep garbage collector for the A runtime.
 *
 * Design (Boehm-GC style, "all blocks" variant):
 *
 *   - EVERY block obtained through xmalloc()/xrealloc() is recorded in a
 *     registry keyed by its start address, with its requested size. This
 *     covers heap objects (AObj headers, flagged GC_F_OBJ by mkobj) AND
 *     plain C buffers (AArr items, ADict pairs/slots, ABytes data, AVec
 *     accumulators, argv arrays in a_call, ...).
 *   - Marking is fully conservative: every 8-byte word of every marked
 *     block is tested as a possible pointer (both raw pointers and
 *     NaN-boxed aval object pointers are recognised). A word "hits" when
 *     it lands inside a registered block; the whole containing block is
 *     then marked and its own words are scanned (worklist fixpoint).
 *     Because buffer contents are scanned too, avals staged in C
 *     temporaries (call argv, map result arrays, deepcopy memo tables)
 *     keep their objects alive with no per-site bookkeeping.
 *     Raw-pointer candidates must be 16-byte aligned (every xmalloc
 *     block is), which rejects most pointer-shaped junk in string/bytes
 *     payloads; interior pointers at unaligned offsets are NOT
 *     recognised (same trade-off as Boehm GC).
 *   - Roots: (a) the C stack of the collecting thread, scanned from the
 *     current frame up to the stack top (pthread attrs / pthread stack
 *     queries / VirtualQuery / /proc/self/maps, with generous fallbacks);
 *     (b) callee-saved registers captured via setjmp; (c) root-provider
 *     callbacks registered by runtime modules that keep avals or object
 *     pointers in file-static storage (builtin fn table, trace spans,
 *     unsafe pointer table, format/compare cycle-guard stacks); (d) the
 *     emitted __a_gc_roots/__a_gc_nroots table (weak symbols; absent in
 *     older objects -> skipped).
 *   - Sweep: unmarked GC_F_OBJ blocks are freed together with their owned
 *     C buffers (items/pairs/slots/data/name/sig/doc/allocs, per type).
 *     T_PYOBJ/T_PYBUF objects call back into the Python bridge, which
 *     drops the owned Python reference or releases the PEP 3118 buffer.
 *     Unmarked non-object blocks are only DELISTED, never freed: they are
 *     either C temporaries the program will free() itself
 *     (a_gc_free then just forwards to the real free) or buffers whose
 *     owner object is collected — those are released by the owner's
 *     sweep. A marked buffer whose owner dies is kept registered so its
 *     contents stay scannable; once unmarked it is delisted and leaks
 *     (bounded, safe — same as the pre-GC status quo).
 *   - Small strings and int boxes are carved from 2 MiB bump-arena chunks.
 *     Each chunk is tracked as one conservative block; whole chunks are
 *     released after they remain unreferenced for two collections.
 *
 *   - Parallel runtime: pool workers never collect (a_par_gc_blocked),
 *     and no collection runs while any a_par_for region is active, so
 *     worker-thread stacks/registers (which are not scanned) cannot lose
 *     objects mid-kernel. Result buffers written by workers are reachable
 *     from the dispatching thread's stack (the kernel ctx), so the next
 *     post-region collection marks them.
 *
 * Knobs: A_GC=0|off|no|false disables the collector entirely (registration
 * becomes a no-op). A_GC_STATS=1 prints one stderr line per collection.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_getattr_np declaration on glibc */
#endif
#include "a_runtime_internal.h"
#include <setjmp.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h> /* malloc_trim */
#endif

#undef free
#if defined(_WIN32) && !defined(_WIN64)
/* All runtime allocations on 32-bit Windows go through _aligned_malloc
   (see a_runtime_internal.h); the registry's own tables use the same
   malloc/realloc macros, so they must be released with _aligned_free. */
#define gc_raw_free(p) _aligned_free(p)
#else
#define gc_raw_free(p) free(p)
#endif

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static int gc_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("A_GC");
        v = !(e && (!strcmp(e, "0") || !strcmp(e, "off") ||
                    !strcmp(e, "no") || !strcmp(e, "false") || !*e));
    }
    return v;
}

static int gc_stats(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("A_GC_STATS");
        v = (e && *e && strcmp(e, "0") && strcmp(e, "off") &&
             strcmp(e, "no") && strcmp(e, "false")) ? 1 : 0;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* block registry                                                      */
/* ------------------------------------------------------------------ */

enum { GC_F_USED = 1u, GC_F_OBJ = 2u, GC_F_MARK = 4u, GC_F_CHUNK = 8u,
       GC_F_OLD = 16u };
#define GC_NONE UINT32_MAX

typedef struct {
    uintptr_t start, end;   /* [start, end): requested allocation extent */
    uint32_t flags;
    uint32_t next;          /* freelist link for unused slots */
    uint64_t arena_id;      /* GC_F_CHUNK: index/id into g_arena_ranges */
    uint32_t region;        /* region.c: depth of the young region that
                               owns this object (0 = none/old generation) */
} GCBlock;

static GCBlock *g_blk;          /* slot array; slots are never moved */
static uint32_t g_nblk;         /* high-water slot count */
static uint32_t g_capblk;
static uint32_t g_freehead = GC_NONE;
static uint32_t g_nused;

/* exact-start hash: start address -> block slot index + 1 (0 = empty,
   UINT32_MAX = tombstone) */
static uint32_t *g_ht;
static uint32_t g_htcap, g_htcount, g_httomb;

#if defined(__linux__)
#include <sys/mman.h>
#endif

/* The mark/sweep hot arrays are tens of MB and probed randomly; asking
   the kernel for transparent huge pages cuts TLB misses on the probe
   streams. No-op where THP is unavailable. */
static void gc_hugepage_hint(void *p, size_t n) {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (p && n >= (2u << 20)) (void)madvise(p, n, MADV_HUGEPAGE);
#else
    (void)p; (void)n;
#endif
}

static uint64_t gc_mix(uintptr_t x) {
    uint64_t h = (uint64_t)x;
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}

static void ht_rehash(uint32_t ncap) {
    /* Never let the table be smaller than the number of live blocks: a
       undersized table makes the insert probe below spin forever. Count
       USED slots first and grow ncap past them if needed. */
    uint32_t nused_now = 0;
    for (uint32_t i = 0; i < g_nblk; ++i)
        if (g_blk[i].flags & GC_F_USED) ++nused_now;
    while (ncap <= nused_now) ncap *= 2;
    /* malloc+memset (not calloc): on 32-bit Windows malloc/realloc are
       macros for the aligned CRT entry points, so every registry table
       must be releasable through gc_raw_free (_aligned_free). */
    uint32_t *nt = (uint32_t *)malloc((size_t)ncap * sizeof(uint32_t));
    if (!nt) return; /* keep old table; lookups still work, just slower */
    memset(nt, 0, (size_t)ncap * sizeof(uint32_t));
    uint32_t nins = 0;
    for (uint32_t i = 0; i < g_nblk; ++i) {
        if (!(g_blk[i].flags & GC_F_USED)) continue;
        uint32_t s = (uint32_t)(gc_mix(g_blk[i].start) & (uint64_t)(ncap - 1));
        uint64_t probes = 0;
        while (nt[s]) {
            s = (s + 1) & (ncap - 1);
            if (++probes > (uint64_t)ncap + 16) {
                fprintf(stderr,
                        "[gc] ht_rehash STUCK ncap=%u nins=%u nblk=%u nused=%u "
                        "count=%u tomb=%u start=%p\n",
                        ncap, nins, g_nblk, g_nused, g_htcount, g_httomb,
                        (void *)g_blk[i].start);
                abort();
            }
        }
        nt[s] = i + 1;
        ++nins;
    }
    if (getenv("A_GC_DEBUG"))
        fprintf(stderr, "[gc] rehash -> %u (inserted %u, nused=%u)\n",
                ncap, nins, g_nused);
    gc_raw_free(g_ht);
    g_ht = nt;
    g_htcap = ncap;
    gc_hugepage_hint(g_ht, (size_t)ncap * sizeof(uint32_t));
    g_htcount = g_nused;
    g_httomb = 0;
}

static void ht_grow_if_needed(void) {
    if (!g_htcap) { ht_rehash(1024); return; }
    if ((uint64_t)(g_htcount + g_httomb) * 10 >= (uint64_t)g_htcap * 7)
        ht_rehash(g_htcap * 2);
}

/* returns block slot index or GC_NONE */
static uint32_t ht_find(uintptr_t start) {
    if (!g_htcap) return GC_NONE;
    uint32_t mask = g_htcap - 1;
    uint32_t s = (uint32_t)(gc_mix(start) & mask);
    uint64_t probes = 0, lim = (uint64_t)g_htcap * 2 + 16;
    for (;;) {
        uint32_t e = g_ht[s];
        if (!e) return GC_NONE;
        if (e != UINT32_MAX && g_blk[e - 1].start == start) return e - 1;
        s = (s + 1) & mask;
        if (++probes > lim) {
            fprintf(stderr, "[gc] ht_find STUCK start=%p cap=%u count=%u tomb=%u nblk=%u\n",
                    (void *)start, g_htcap, g_htcount, g_httomb, g_nblk);
            abort();
        }
    }
}

static void ht_put(uint32_t idx) {
    ht_grow_if_needed();
    if (!g_htcap) return;
    uint32_t mask = g_htcap - 1;
    uint32_t s = (uint32_t)(gc_mix(g_blk[idx].start) & mask);
    uint32_t tomb = GC_NONE;
    uint64_t probes = 0, lim = (uint64_t)g_htcap * 2 + 16;
    for (;;) {
        uint32_t e = g_ht[s];
        if (!e) {
            uint32_t at = tomb != GC_NONE ? tomb : s;
            /* Every successful insert adds one live entry: reusing a
               tombstone must bump the live count AND drop the tombstone
               count. (Previously only --g_httomb ran here, so g_htcount
               drifted low under alloc/free churn until the table could no
               longer grow and ht_rehash probed forever.) */
            ++g_htcount;
            if (tomb != GC_NONE) --g_httomb;
            g_ht[at] = idx + 1;
            return;
        }
        if (e == UINT32_MAX) { if (tomb == GC_NONE) tomb = s; }
        else if (g_blk[e - 1].start == g_blk[idx].start) return; /* dup */
        s = (s + 1) & mask;
        if (++probes > lim) {
            fprintf(stderr, "[gc] ht_put STUCK start=%p cap=%u count=%u tomb=%u nblk=%u\n",
                    (void *)g_blk[idx].start, g_htcap, g_htcount, g_httomb, g_nblk);
            abort();
        }
    }
}

static void ht_del(uintptr_t start) {
    if (!g_htcap) return;
    uint32_t mask = g_htcap - 1;
    uint32_t s = (uint32_t)(gc_mix(start) & mask);
    uint64_t probes = 0, lim = (uint64_t)g_htcap * 2 + 16;
    for (;;) {
        uint32_t e = g_ht[s];
        if (!e) return;
        if (e != UINT32_MAX && g_blk[e - 1].start == start) {
            g_ht[s] = UINT32_MAX;
            --g_htcount;
            ++g_httomb;
            return;
        }
        s = (s + 1) & mask;
        if (++probes > lim) {
            fprintf(stderr, "[gc] ht_del STUCK start=%p cap=%u count=%u tomb=%u nblk=%u\n",
                    (void *)start, g_htcap, g_htcount, g_httomb, g_nblk);
            abort();
        }
    }
}

static uint32_t blk_alloc_slot(void) {
    if (g_freehead != GC_NONE) {
        uint32_t i = g_freehead;
        g_freehead = g_blk[i].next;
        return i;
    }
    if (g_nblk == g_capblk) {
        uint32_t nc = g_capblk ? g_capblk * 2 : 4096;
        GCBlock *nb = (GCBlock *)realloc(g_blk, (size_t)nc * sizeof(GCBlock));
        if (!nb) return GC_NONE;
        g_blk = nb;
        g_capblk = nc;
        gc_hugepage_hint(g_blk, (size_t)nc * sizeof(GCBlock));
    }
    return g_nblk++;
}

/* Remove a block from the registry without freeing its memory. */
static void blk_delist(uint32_t idx) {
    if (idx == GC_NONE || !(g_blk[idx].flags & GC_F_USED)) return;
    ht_del(g_blk[idx].start);
    g_blk[idx].flags = 0;
    g_blk[idx].next = g_freehead;
    g_freehead = idx;
    if (g_nused) --g_nused;
}

/* Sweep-time variant: skips the per-block ht_del probe. During
   gc_collect the hash is left stale (entries still point at slots whose
   USED flag is now clear) and rebuilt once afterwards by ht_rebuild —
   O(live blocks) instead of O(collected blocks) hash probes. Only
   valid inside gc_collect: between the fast delist and the rebuild,
   ht_find may return a slot with flags==0, which every in-sweep caller
   already tolerates (they re-check GC_F_USED / the flag bits). */
static int g_hash_stale;    /* set when any blk_delist_fast ran */

static void blk_delist_fast(uint32_t idx) {
    if (idx == GC_NONE || !(g_blk[idx].flags & GC_F_USED)) return;
    g_blk[idx].flags = 0;
    g_blk[idx].next = g_freehead;
    g_freehead = idx;
    if (g_nused) --g_nused;
    g_hash_stale = 1;
}

/* Rebuild the exact-start hash from the surviving USED blocks. Run at
   the end of every collection that used blk_delist_fast. */
static void ht_rebuild(void) {
    if (!g_htcap) { g_hash_stale = 0; return; }
    memset(g_ht, 0, (size_t)g_htcap * sizeof(uint32_t));
    g_htcount = 0;
    g_httomb = 0;
    uint32_t mask = g_htcap - 1;
    for (uint32_t i = 0; i < g_nblk; ++i) {
        if (!(g_blk[i].flags & GC_F_USED)) continue;
        uint32_t s = (uint32_t)(gc_mix(g_blk[i].start) & mask);
        uint64_t probes = 0;
        while (g_ht[s]) {
            s = (s + 1) & mask;
            if (++probes > (uint64_t)g_htcap + 16) {
                fprintf(stderr, "[gc] ht_rebuild STUCK cap=%u nblk=%u nused=%u\n",
                        g_htcap, g_nblk, g_nused);
                abort();
            }
        }
        g_ht[s] = i + 1;
        ++g_htcount;
    }
    g_hash_stale = 0;
}

/* ------------------------------------------------------------------ */
/* small/medium block pool                                             */
/*                                                                     */
/* Sweep-time releases are delisted before they reach this cache, so a   */
/* pooled block is never visible to the marker. xmalloc() pops a block   */
/* and registers it again through the normal a_gc_register path. The     */
/* cache is per-thread (pool workers never share freelists) and bounded   */
/* both per class and in total bytes; larger blocks always go straight   */
/* to free() so glibc can return mmap-backed memory to the OS.           */
/* ------------------------------------------------------------------ */

#define GC_POOL_MAX_REQ    ((size_t)128 << 10)  /* >128 KiB: direct free */
#define GC_POOL_MAX_BYTES  ((size_t)1 << 20)    /* per-thread cache cap */
#define GC_POOL_MAX_PERCLS 512u

/* Size classes are chosen so pool_class_size() is also the capacity
   guarantee:
   xmalloc/xrealloc request exactly this many bytes from malloc while the
   registry keeps the caller's requested extent for scanning. */
static size_t pool_class_size(size_t n) {
    if (n <= 4096)   return (n + 15)    & ~(size_t)15;
    if (n <= 32768)  return (n + 4095)  & ~(size_t)4095;
    if (n <= 65536)  return (n + 8191)  & ~(size_t)8191;
    if (n <= 131072) return (n + 16383) & ~(size_t)16383;
    return n;
}

#define GC_POOL_NCLS 271
static A_RT_TLS void *g_pool_head[GC_POOL_NCLS];
static A_RT_TLS uint32_t g_pool_count[GC_POOL_NCLS];
static A_RT_TLS size_t g_pool_bytes;

static uint32_t pool_index(size_t class_size) {
    if (class_size <= 4096)  return (uint32_t)(class_size / 16) - 1;
    if (class_size <= 32768) return 256 + (uint32_t)(class_size / 4096) - 2;
    if (class_size <= 65536) return 263 + (uint32_t)(class_size / 8192) - 5;
    return 267 + (uint32_t)(class_size / 16384) - 5;
}

/* Allocation size xmalloc/xrealloc must request for n bytes. For pooled
   sizes this is the class size (capacity); larger requests pass through. */
size_t a_gc_pool_alloc_size(size_t n) {
    n = n ? n : 1;
    if (!gc_enabled() || n > GC_POOL_MAX_REQ) return n;
    return pool_class_size(n);
}

/* Pop a cached block for an allocation of n requested bytes, or NULL.
   The block is already delisted; the caller registers it normally. */
void *a_gc_pool_try_alloc(size_t n) {
    size_t req = n ? n : 1;
    if (!gc_enabled() || req > GC_POOL_MAX_REQ) return NULL;
    size_t cs = pool_class_size(req);
    uint32_t idx = pool_index(cs);
    void *p = g_pool_head[idx];
    if (!p) return NULL;
    g_pool_head[idx] = *(void **)p;
    if (g_pool_count[idx]) --g_pool_count[idx];
    g_pool_bytes -= cs;
    return p;
}

/* Push a delisted block onto the thread-local cache, or release it. */
static void pool_release(void *p, size_t n) {
    size_t req = n ? n : 1;
    if (getenv("A_GC_TRACE") && req >= ((size_t)256 << 10))
        fprintf(stderr, "[gc-trace] pool_release %p sz=%zu\n", p, req);
    if (req > GC_POOL_MAX_REQ || !gc_enabled()) {
        gc_raw_free(p);
        return;
    }
    size_t cs = pool_class_size(req);
    uint32_t idx = pool_index(cs);
    if (g_pool_count[idx] >= GC_POOL_MAX_PERCLS ||
        g_pool_bytes + cs > GC_POOL_MAX_BYTES) {
        gc_raw_free(p);
        return;
    }
    *(void **)p = g_pool_head[idx];
    g_pool_head[idx] = p;
    ++g_pool_count[idx];
    g_pool_bytes += cs;
}

/* ------------------------------------------------------------------ */
/* xarena (immortal bump) ranges: objects carved from these chunks are */
/* not individually freeable, so mkobj must not flag them.            */
/* ------------------------------------------------------------------ */

typedef struct {
    uintptr_t lo, hi;
    uint64_t id;
    int mapped;
    int active;
    int candidate;
} GCArenaRange;
static GCArenaRange *g_arena_ranges;
static size_t g_narena, g_caparena;
static uint64_t g_next_arena_id = 1;

static GCArenaRange *gc_arena_by_id(uint64_t id) {
    for (size_t i = 0; i < g_narena; ++i)
        if (g_arena_ranges[i].id == id) return &g_arena_ranges[i];
    return NULL;
}

static GCArenaRange *gc_arena_containing(uintptr_t p) {
    for (size_t i = 0; i < g_narena; ++i)
        if (p >= g_arena_ranges[i].lo && p < g_arena_ranges[i].hi)
            return &g_arena_ranges[i];
    return NULL;
}

static uint32_t gc_register_chunk_block(void *p, size_t n, uint64_t id) {
    uint32_t i = ht_find((uintptr_t)p);
    if (i != GC_NONE) {
        GCBlock *b = &g_blk[i];
        b->start = (uintptr_t)p;
        b->end = b->start + n;
        b->flags |= GC_F_CHUNK;
        b->arena_id = id;
        return i;
    }
    i = blk_alloc_slot();
    if (i == GC_NONE) return GC_NONE;
    GCBlock *b = &g_blk[i];
    b->start = (uintptr_t)p;
    b->end = b->start + n;
    b->flags = GC_F_USED | GC_F_CHUNK;
    b->next = 0;
    b->arena_id = id;
    b->region = 0;
    ht_put(i);
    ++g_nused;
    return i;
}

void a_gc_note_arena(void *p, size_t n, int mapped) {
    if (!p || !n) return;
    if (g_narena == g_caparena) {
        size_t nc = g_caparena ? g_caparena * 2 : 8;
        GCArenaRange *nr = (GCArenaRange *)realloc(g_arena_ranges,
                                                 nc * sizeof(GCArenaRange));
        if (!nr) return;
        g_arena_ranges = nr;
        g_caparena = nc;
    }
    uint64_t id = g_next_arena_id++;
    GCArenaRange *r = &g_arena_ranges[g_narena++];
    r->lo = (uintptr_t)p;
    r->hi = r->lo + n;
    r->id = id;
    r->mapped = mapped;
    r->active = 0;
    r->candidate = 0;
    (void)gc_register_chunk_block(p, n, id);
}

static int gc_in_arena(uintptr_t p) {
    return gc_arena_containing(p) != NULL;
}

void a_gc_arena_switch(const void *old_cursor, void *new_chunk) {
    if (old_cursor) {
        GCArenaRange *old_range = gc_arena_containing((uintptr_t)old_cursor);
        if (old_range) old_range->active = 0;
    }
    if (new_chunk) {
        GCArenaRange *new_range = gc_arena_containing((uintptr_t)new_chunk);
        if (new_range) new_range->active = 1;
    }
}

int a_gc_arena_stale(const void *cursor) {
    if (!cursor) return 0;
    return gc_arena_containing((uintptr_t)cursor) == NULL;
}

/* ------------------------------------------------------------------ */
/* root providers: runtime modules register callbacks that mark        */
/* avals/object pointers held in file-static or TLS storage.           */
/* ------------------------------------------------------------------ */

static aval *g_gc_root_stack;
static size_t g_gc_root_len, g_gc_root_cap;

void a_gc_push_root(aval v) {
    if (!gc_enabled() || !a_is_ptr(v)) return;
    if (g_gc_root_len == g_gc_root_cap) {
        size_t nc = g_gc_root_cap ? g_gc_root_cap * 2 : 16;
        aval *nr = (aval *)realloc(g_gc_root_stack, nc * sizeof(aval));
        if (!nr) return;
        g_gc_root_stack = nr;
        g_gc_root_cap = nc;
    }
    g_gc_root_stack[g_gc_root_len++] = v;
}

void a_gc_pop_root(void) {
    if (g_gc_root_len) --g_gc_root_len;
}


typedef void (*AGcRootFn)(void);
#define GC_MAX_ROOTFNS 64
static AGcRootFn g_rootfns[GC_MAX_ROOTFNS];
static int g_nrootfns;

void a_gc_add_root_cb(AGcRootFn fn) {
    if (!fn) return;
    for (int i = 0; i < g_nrootfns; ++i)
        if (g_rootfns[i] == fn) return;
    if (g_nrootfns < GC_MAX_ROOTFNS) g_rootfns[g_nrootfns++] = fn;
}

/* emitted-code roots (weak: absent in objects built before the emitter
   started emitting them; NULL/0 -> skipped). Each entry is the address
   of an aval slot. */
#if defined(__GNUC__) || defined(__clang__)
extern unsigned long *__a_gc_roots[] __attribute__((weak));
extern const unsigned long __a_gc_nroots __attribute__((weak));
#else
extern unsigned long *__a_gc_roots[];
extern const unsigned long __a_gc_nroots;
#endif

/* ------------------------------------------------------------------ */
/* marking                                                             */
/* ------------------------------------------------------------------ */

/* Sorted-by-start view of the USED blocks, used only for INTERIOR
 * pointers: exact block-start hits are answered by the registry hash
 * (ht_find) in mark_find, so this array is the fallback for words that
 * land inside a block without being its start. 16 bytes per entry keeps
 * the radix-sort passes cheap; `end` is fetched from g_blk[idx] only for
 * the single containment candidate. */
typedef struct {
    uintptr_t start;
    uint32_t idx;           /* slot index into g_blk */
    uint32_t pad;
} GCOrdEnt;
static GCOrdEnt *g_ord;
static GCOrdEnt *g_ordtmp;
static uint32_t g_ordn, g_ordcap;
static uint32_t g_find_hint;    /* last mark_find position (finger search) */
static uintptr_t g_scan_lo, g_scan_hi; /* [lo,hi): extent of all blocks */
static uint32_t *g_wl;       /* worklist of marked-but-unscanned blocks */
static size_t g_wl_len, g_wl_cap;
static int g_in_gc;

/* --- profiling counters (A_GC_STATS=1 only; zero cost when off) --- */
static int g_prof;
static uint64_t p_words, p_pref_pass, p_finds, p_find_hits, p_wl_push;
static uint64_t p_stack_words, p_blk_words, p_marked_blks;
static uint64_t p_ub_win, p_ub_gallop, p_unaligned;
static double p_t_build, p_t_roots, p_t_regs, p_t_stack, p_t_fix,
              p_t_sweep, p_t_live;

static double gc_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* LSD radix sort of g_ord[0..n) by start address: O(n) regardless of
   input order, which matters when the registry holds ~1M blocks (a
   qsort there costs ~70ms per collection). 16-bit digits, 1-4 passes
   depending on the highest address bit actually in use. */
static void ord_radix_sort(GCOrdEnt *a, GCOrdEnt *tmp, uint32_t n,
                           uintptr_t maxstart) {
    static uint32_t cnt[65536];
    GCOrdEnt *src = a, *dst = tmp;
    int topbit = 0;
    while (topbit < 48 && (maxstart >> (topbit + 16))) topbit += 16;
    /* topbit is now the shift of the highest nonzero 16-bit digit */
    for (int shift = 0; shift <= topbit; shift += 16) {
        memset(cnt, 0, sizeof cnt);
        for (uint32_t i = 0; i < n; ++i)
            ++cnt[(uint32_t)((src[i].start >> shift) & 0xFFFFu)];
        uint32_t sum = 0;
        for (int i = 0; i < 65536; ++i) {
            uint32_t c = cnt[i];
            cnt[i] = sum;
            sum += c;
        }
        for (uint32_t i = 0; i < n; ++i)
            dst[cnt[(uint32_t)((src[i].start >> shift) & 0xFFFFu)]++] = src[i];
        GCOrdEnt *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a, src, (size_t)n * sizeof(GCOrdEnt));
}

static int mark_build_order(void) {
    if (g_ordcap < g_nused) {
        uint32_t nc = g_ordcap ? g_ordcap * 2 : 1024;
        while (nc < g_nused) nc *= 2;
        GCOrdEnt *no = (GCOrdEnt *)realloc(g_ord, (size_t)nc * sizeof(GCOrdEnt));
        if (!no) return 0;
        g_ord = no;
        GCOrdEnt *nt = (GCOrdEnt *)realloc(g_ordtmp, (size_t)nc * sizeof(GCOrdEnt));
        if (!nt) return 0; /* g_ordcap unchanged: retried next collection */
        g_ordtmp = nt;
        g_ordcap = nc;
        gc_hugepage_hint(g_ord, (size_t)nc * sizeof(GCOrdEnt));
        gc_hugepage_hint(g_ordtmp, (size_t)nc * sizeof(GCOrdEnt));
    }
    uint32_t n = 0;
    g_scan_lo = UINTPTR_MAX;
    g_scan_hi = 0;
    uintptr_t maxstart = 0;
    for (uint32_t i = 0; i < g_nblk; ++i) {
        if (!(g_blk[i].flags & GC_F_USED)) continue;
        GCBlock *b = &g_blk[i];
        g_ord[n].start = b->start;
        g_ord[n].idx = i;
        g_ord[n].pad = 0;
        ++n;
        if (b->start < g_scan_lo) g_scan_lo = b->start;
        if (b->end > g_scan_hi) g_scan_hi = b->end;
        if (b->start > maxstart) maxstart = b->start;
    }
    g_ordn = n;
    g_find_hint = 0;
    if (!n) { g_scan_lo = 1; g_scan_hi = 0; return 1; }
    /* Fast path: registry slots are recycled roughly in allocation
       order, so g_ord is often already (nearly) sorted by start — one
       O(n) check beats a full radix pass when it is. */
    int sorted = 1;
    for (uint32_t i = 1; i < n; ++i)
        if (g_ord[i].start < g_ord[i - 1].start) { sorted = 0; break; }
    if (!sorted) ord_radix_sort(g_ord, g_ordtmp, n, maxstart);
    return 1;
}

/* First index in [0,g_ordn) whose start exceeds p (g_ordn if none).
   Anchored at the last hit: consecutive scanned words usually point into
   nearby blocks, so the answer is typically a few entries away. A short
   sequential window scan around the hint covers that common case with
   prefetch-friendly loads; anything farther falls back to galloping +
   binary search. */
#define ORD_WIN 48
static uint32_t ord_ub(uintptr_t p) {
    uint32_t n = g_ordn;
    if (!n) return 0;
    uint32_t h = g_find_hint < n ? g_find_hint : n - 1;
    uint32_t lo, hi; /* answer in [lo,hi) */
    if (g_ord[h].start <= p) {
        /* answer in (h, n]: scan forward a few entries first */
        uint32_t j = h;
        uint32_t stop = h + ORD_WIN < n ? h + ORD_WIN : n;
        while (j + 1 < stop && g_ord[j + 1].start <= p) ++j;
        if (j + 1 >= n) { if (g_prof) ++p_ub_win; return n; }
        if (g_ord[j + 1].start > p) { if (g_prof) ++p_ub_win; return j + 1; }
        /* window exhausted without finding the boundary: gallop from j */
        if (g_prof) ++p_ub_gallop;
        uint32_t pos = j, step = 1;
        for (;;) {
            uint64_t nxt = (uint64_t)pos + step;
            if (nxt >= n) { lo = pos + 1; hi = n; break; }
            if (g_ord[nxt].start > p) { lo = pos + 1; hi = (uint32_t)nxt + 1; break; }
            pos = (uint32_t)nxt;
            step <<= 1;
        }
    } else {
        /* answer in [0, h]: scan backward a few entries first */
        uint32_t j = h;
        uint32_t stop = h >= ORD_WIN ? h - ORD_WIN : 0;
        while (j > stop && g_ord[j - 1].start > p) --j;
        if (j == 0) { if (g_prof) ++p_ub_win; return 0; }
        if (g_ord[j - 1].start <= p) { if (g_prof) ++p_ub_win; return j; }
        /* gallop left from j */
        if (g_prof) ++p_ub_gallop;
        uint32_t pos = j, step = 1;
        for (;;) {
            if (step > pos) { lo = 0; hi = pos + 1; break; }
            if (g_ord[pos - step].start <= p) {
                lo = pos - step + 1; hi = pos + 1; break;
            }
            pos -= step;
            step <<= 1;
        }
    }
    /* Interpolation search: block starts are roughly uniformly
       distributed over the heap span, so a proportional guess lands
       within a few entries in ~2-3 probes instead of ~log2(n)
       dependent loads. Falls back to binary once the bracket is
       small or the estimate degenerates. */
    for (int it = 0; it < 6 && hi - lo > 12; ++it) {
        uintptr_t lo_start = lo ? g_ord[lo - 1].start : g_scan_lo;
        uintptr_t hi_start = hi < n ? g_ord[hi].start : g_scan_hi;
        if (hi_start <= lo_start || p <= lo_start) break;
        uint64_t num = (uint64_t)(p - lo_start);
        uint64_t den = (uint64_t)(hi_start - lo_start);
        uint64_t est = num * (uint64_t)(hi - lo) / den;
        uint32_t mid = lo + (uint32_t)(est > (uint64_t)(hi - lo) ? hi - lo : est);
        if (mid >= hi) mid = hi - 1;
        if (g_ord[mid].start <= p) lo = mid + 1; else hi = mid;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_ord[mid].start <= p) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* Find the registered block containing address p (blocks never overlap). */
static uint32_t mark_find(uintptr_t p) {
    if (g_prof) ++p_finds;
    uint32_t ub = ord_ub(p);
    if (!ub) return GC_NONE;
    uint32_t cand = g_ord[ub - 1].idx;
    GCBlock *b = &g_blk[cand];
    if (p >= b->start && p < b->end) {
        /* Only real hits move the finger: junk words that merely pass
           the extent prefilter would otherwise keep teleporting the
           search anchor to a random position. */
        g_find_hint = ub - 1;
        if (g_prof) ++p_find_hits;
        return cand;
    }
    return GC_NONE;
}

static int g_wl_overflow;   /* set when the worklist could not grow */

static void wl_push(uint32_t idx) {
    if (g_wl_len == g_wl_cap) {
        size_t nc = g_wl_cap ? g_wl_cap * 2 : 1024;
        uint32_t *nw = (uint32_t *)realloc(g_wl, nc * sizeof(uint32_t));
        if (!nw) { g_wl_overflow = 1; return; }
        g_wl = nw;
        g_wl_cap = nc;
    }
    g_wl[g_wl_len++] = idx;
    if (g_prof) ++p_wl_push;
}

static void mark_block(uint32_t idx) {
    if (idx == GC_NONE) return;
    GCBlock *b = &g_blk[idx];
    if (!(b->flags & GC_F_USED) || (b->flags & GC_F_MARK)) return;
    b->flags |= GC_F_MARK;
    wl_push(idx);
    if (g_prof) ++p_marked_blks;
}

static void mark_addr(uintptr_t p) {
    if (!p) return;
    /* Cheap extent prefilter: almost every scanned word (ints, doubles,
       small constants, code addresses) falls outside the heap block
       range, so reject it with two compares instead of a binary search. */
    if (p < g_scan_lo || p >= g_scan_hi) return;
    if (g_prof) ++p_pref_pass;
    mark_block(mark_find(p));
}

/* One machine word: try NaN-boxed aval first, then raw pointer. */
static void mark_word(uint64_t w) {
    uintptr_t p = 0;
    if ((w & 0xFFFF000000000000ULL) == A_TAG) {
        uint64_t pay = w & 0x0000FFFFFFFFFFFFULL;
        if (pay && (pay & 0xFULL) == 0) p = (uintptr_t)pay;
        /* inline ints (pay&1) and small constants have nonzero low nibble
           or are tiny; only aligned nonzero payloads can be object ptrs */
        if (!p) return;
    } else {
        p = (uintptr_t)w;
        /* Every xmalloc block is 16-byte aligned (posix_memalign /
           _aligned_malloc), so a genuine heap pointer word is always
           16-aligned. Rejecting unaligned words here eliminates most
           false positives from string/bytes/double payloads before the
           extent prefilter. Trade-off (same as Boehm GC): an interior
           pointer at a non-16-aligned offset no longer keeps its block
           alive — the emitted code and runtime only ever store block
           base pointers and NaN-boxed avals, both 16-aligned. */
        if (p & 0xFULL) return;
    }
    mark_addr(p);
}

void a_gc_mark_ptr(const void *p) {
    if (g_in_gc && p) mark_addr((uintptr_t)p);
}

void a_gc_mark_aval(aval v) {
    if (g_in_gc && a_is_ptr(v)) mark_addr((uintptr_t)A_PAYLOAD(v));
}

static int gc_trace(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("A_GC_TRACE");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}
#define GC_TRACE_MIN ((size_t)256 << 10)

static void mark_scan_block(uint32_t idx) {
    /* A chunk must be marked as a whole, not scanned: its carved objects
       are dead candidates as a unit, and scanning every small string would
       needlessly resurrect values from interior words. */
    if (g_blk[idx].flags & GC_F_CHUNK) return;
    uintptr_t a = g_blk[idx].start, e = g_blk[idx].end;
    if (gc_trace() && e - a >= GC_TRACE_MIN)
        fprintf(stderr, "[gc-trace] scan blk idx=%u %p..%p sz=%llu flags=%x\n",
                idx, (void *)a, (void *)e,
                (unsigned long long)(e - a), g_blk[idx].flags);
    uint64_t nw = (e - a) / sizeof(uintptr_t);
    if (g_prof) { p_words += nw; p_blk_words += nw; }
    for (; a + sizeof(uintptr_t) <= e; a += sizeof(uintptr_t))
        mark_word(*(const uintptr_t *)a);
}

/* ------------------------------------------------------------------ */
/* stack bounds                                                        */
/* ------------------------------------------------------------------ */

#if defined(__linux__) && !defined(__TINYC__)
/* pthread.h is already included via a_runtime_internal.h (A_PAR_REAL). */
static int gc_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    void *base = NULL;
    size_t size = 0;
    int ok = pthread_attr_getstack(&attr, &base, &size) == 0 && base && size;
    pthread_attr_destroy(&attr);
    if (!ok) return 0;
    *lo = (uintptr_t)base;
    *hi = *lo + size;
    return 1;
}
#elif defined(__APPLE__) && !defined(__TINYC__)
static int gc_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    void *top = pthread_get_stackaddr_np(pthread_self());
    size_t sz = pthread_get_stacksize_np(pthread_self());
    if (!top || !sz) return 0;
    *hi = (uintptr_t)top;
    *lo = *hi - sz;
    return 1;
}
#elif defined(_WIN32)
/* Walk committed MEM_PRIVATE regions upward from the current SP using
   VirtualQuery (exported by kernel32; declared in a_runtime_internal.h
   style — declare locally to keep the ABI struct private). */
typedef struct {
    void *BaseAddress;
    void *AllocationBase;
    unsigned long AllocationProtect;
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    unsigned short PartitionId;
    unsigned short _pad;
    size_t RegionSize;
    unsigned long State, Protect, Type;
#else
    size_t RegionSize;
    unsigned long State, Protect, Type;
#endif
} GCMemInfo;
extern int A_WINAPI VirtualQuery(const void *lpAddress, GCMemInfo *lpBuffer,
                                 size_t dwLength);
#define GC_MEM_COMMIT  0x1000UL
#define GC_MEM_PRIVATE 0x20000UL
static int gc_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    volatile char anchor;
    uintptr_t sp = (uintptr_t)&anchor;
    GCMemInfo mi;
    uintptr_t cur = sp & ~(uintptr_t)0xFFF;
    uintptr_t top = 0;
    for (int i = 0; i < 4096; ++i) {
        memset(&mi, 0, sizeof mi);
        if (!VirtualQuery((const void *)cur, &mi, sizeof mi)) break;
        uintptr_t rb = (uintptr_t)mi.BaseAddress;
        uintptr_t re = rb + mi.RegionSize;
        if (mi.State == GC_MEM_COMMIT && mi.Type == GC_MEM_PRIVATE) {
            if (re > cur) top = re;
            cur = re;
            continue;
        }
        break;
    }
    if (!top || top <= sp) return 0;
    *lo = sp;
    *hi = top;
    return 1;
}
#elif defined(__linux__)
/* TINYC/astoc-built runtimes: no pthread stack query; find the VMA that
   contains SP in /proc/self/maps (it is the stack). */
static int gc_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    volatile char anchor;
    uintptr_t sp = (uintptr_t)&anchor;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a = 0, b = 0;
        char perm[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perm) != 3) continue;
        if (sp >= a && sp < b && perm[0] == 'r') {
            *lo = (uintptr_t)sp;
            *hi = (uintptr_t)b;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}
#else
static int gc_stack_bounds(uintptr_t *lo, uintptr_t *hi) { (void)lo; (void)hi; return 0; }
#endif

static void mark_c_stack(void) {
    volatile char anchor;
    uintptr_t sp = (uintptr_t)&anchor;
    uintptr_t lo = 0, hi = 0;
    if (!gc_stack_bounds(&lo, &hi) || hi <= sp) {
        /* Last-resort window: scan what we can above SP. Under-scanning is
           the dangerous direction, so bias generous but stay in bounds we
           can prove — without stack bounds we can only safely scan the
           current frame region, so fall back to a modest window. */
        hi = sp + (4u << 20);
        lo = sp;
    }
    if (hi - lo > (1u << 30)) hi = lo + (1u << 30); /* sanity cap */
    uintptr_t w = (sp + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    if (w < lo) w = lo;
    if (w + sizeof(uintptr_t) > hi) { if (g_prof) p_stack_words += 0; return; }
    uint64_t nw = (hi - w) / sizeof(uintptr_t);
    for (uintptr_t e = hi - sizeof(uintptr_t); w <= e; w += sizeof(uintptr_t))
        mark_word(*(const uintptr_t *)w);
    if (g_prof) { p_words += nw; p_stack_words += nw; }
}

/* Callee-saved registers at GC entry: catches avals/ pointers that the
   compiler keeps in registers across the allocation call. */
static void mark_registers(void) {
    jmp_buf jb;
    if (setjmp(jb) != 0) return;
    unsigned long long tmp[sizeof(jmp_buf) / sizeof(unsigned long long) + 2];
    memcpy(tmp, jb, sizeof(jb) < sizeof(tmp) ? sizeof(jb) : sizeof(tmp));
    size_t n = sizeof(jb) / sizeof(unsigned long long);
    if (n > sizeof(tmp) / sizeof(tmp[0])) n = sizeof(tmp) / sizeof(tmp[0]);
    for (size_t i = 0; i < n; ++i) mark_word(tmp[i]);
}

static void mark_emitted_roots(void) {
#if defined(__GNUC__) || defined(__clang__)
    if (__a_gc_roots == NULL || &__a_gc_nroots == NULL) return;
#else
    if (__a_gc_roots == NULL || __a_gc_nroots == 0) return;
#endif
    unsigned long nr = __a_gc_nroots;
    if (nr > (1u << 22)) nr = 1u << 22; /* corrupt-table sanity cap */
    for (unsigned long i = 0; i < nr; ++i) {
        /* memcpy: emitted slots are 8-aligned (balign), but never depend
           on it — a misaligned slot would be UB through *slot. */
        void *slot = (void *)(uintptr_t)__a_gc_roots[i];
        if (slot) { aval v; memcpy(&v, slot, sizeof(v)); a_gc_mark_aval(v); }
    }
}

/* ------------------------------------------------------------------ */
/* sweep                                                               */
/* ------------------------------------------------------------------ */

static void gc_release_child(void *p) {
    if (!p) return;
    uint32_t i = ht_find((uintptr_t)p);
    if (i == GC_NONE) {
        if (getenv("A_GC_TRACE"))
            fprintf(stderr, "[gc-trace] release_child UNTRACKED %p in_gc=%d\n",
                    p, g_in_gc);
        gc_raw_free(p);
        return; /* untracked (raw malloc) */
    }
    GCBlock *b = &g_blk[i];
    if (b->flags & GC_F_MARK) return;  /* still referenced: keep it */
    if (b->flags & GC_F_OBJ) return;   /* another object: never free here */
    size_t sz = b->end - b->start;
    /* Region release runs outside gc_collect: use the hash-updating
       delist there. The fast variant is only safe inside a collection,
       where ht_rebuild runs before the collector exits. */
    if (g_in_gc) blk_delist_fast(i); else blk_delist(i);
    pool_release(p, sz);
}

static void gc_free_obj_interior(void *obj) {
    AObj *o = (AObj *)obj;
    switch (o->type) {
        case T_LIST: case T_TUPLE: case T_SET: {
            AArr *a = (AArr *)o;
            gc_release_child(a->items);
            gc_release_child(a->iitems);
            break;
        }
        case T_DICT: case T_STRUCT: {
            /* AStruct embeds ADict first, so the cast is valid for both. */
            ADict *d = (ADict *)o;
            gc_release_child(d->pairs);
            gc_release_child(d->slots);
            break;
        }
        case T_BYTES:
            gc_release_child(((ABytes *)o)->data);
            break;
        case T_FNON: {
            AFnon *f = (AFnon *)o;
            gc_release_child(f->name);
            gc_release_child(f->sig);
            gc_release_child(f->doc);
            break;
        }
        case T_ARENA:
            gc_release_child(((AArena *)o)->allocs);
            break;
        case T_PYOBJ:
        case T_PYBUF:
            /* Drop the owned Python reference / release the exported
               buffer. The bridge owns GIL choreography; its bookkeeping
               (PyBufNode / PyBufferView copies) is raw malloc and is
               invisible to this collector until a_py_release_obj frees
               it while unlinking the live-view list. */
            a_py_release_obj(obj);
            break;
        default:
            break; /* T_STR/T_CHAR/T_RANGE/T_ITER/T_ENV/T_RESULT/T_INT64/T_VIEW */
    }
}

static int gc_obj_pinned(const void *obj) {
    /* Python-owned objects are releasable through the bridge and therefore
       are no longer pinned by the collector.  Kept for region tracking,
       where a synchronous interpreter release is not attempted. */
    uint32_t t = ((const AObj *)obj)->type;
    return t == T_PYOBJ || t == T_PYBUF;
}

/* ------------------------------------------------------------------ */
/* collection                                                          */
/* ------------------------------------------------------------------ */

static uint64_t g_alloc_bytes;   /* bytes registered since last GC */
static uint64_t g_next_gc = 1u << 20; /* 1 MiB initial threshold */
static uint64_t g_live_bytes;
static uint64_t g_ncollections, g_nfreed_objs, g_nfreed_bytes, g_ndelisted;

static int gc_debug(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("A_GC_DEBUG");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}

static void gc_collect(void) {
    if (g_in_gc) return;
    g_in_gc = 1;
    g_wl_overflow = 0;
    int dbg = gc_debug();
    g_prof = gc_stats();
    double t0 = g_prof ? gc_now_s() : 0.0;
    if (dbg) fprintf(stderr, "[gc] collect enter: nblk=%u nused=%u ord?\n", g_nblk, g_nused);

    if (!mark_build_order()) { g_in_gc = 0; return; } /* cannot mark safely */
    if (g_prof) p_t_build += gc_now_s() - t0;
    if (dbg) fprintf(stderr, "[gc] order built: %u used blocks\n", g_ordn);
    t0 = g_prof ? gc_now_s() : 0.0;

    for (size_t i = 0; i < g_gc_root_len; ++i) a_gc_mark_aval(g_gc_root_stack[i]);
    for (int i = 0; i < g_nrootfns; ++i) g_rootfns[i]();
    mark_emitted_roots();
    if (g_prof) p_t_roots += gc_now_s() - t0;
    t0 = g_prof ? gc_now_s() : 0.0;
    mark_registers();
    if (g_prof) p_t_regs += gc_now_s() - t0;
    t0 = g_prof ? gc_now_s() : 0.0;
    mark_c_stack();
    if (g_prof) p_t_stack += gc_now_s() - t0;
    if (dbg) fprintf(stderr, "[gc] stack done, wl=%zu\n", g_wl_len);
    t0 = g_prof ? gc_now_s() : 0.0;

    while (g_wl_len) {
        uint32_t idx = g_wl[--g_wl_len];
        if (dbg) fprintf(stderr, "[gc] scan idx=%u start=%p end=%p flags=%x\n",
                         idx, (void *)g_blk[idx].start, (void *)g_blk[idx].end,
                         g_blk[idx].flags);
        mark_scan_block(idx);
    }
    if (g_prof) p_t_fix += gc_now_s() - t0;
    if (dbg) fprintf(stderr, "[gc] mark fixpoint done\n");

    if (g_wl_overflow) {
        /* A marked block could not be queued for scanning, so the mark
           set is incomplete. Aborting is the only safe choice: sweep
           nothing, just reset the marks we did set. */
        for (uint32_t i = 0; i < g_nblk; ++i)
            if (g_blk[i].flags & GC_F_USED) g_blk[i].flags &= ~GC_F_MARK;
        g_wl_len = 0;
        g_in_gc = 0;
        return;
    }

    /* Sweep: collect dead objects first, then release them. */
    t0 = g_prof ? gc_now_s() : 0.0;
    uint32_t *dead = NULL;
    size_t ndead = 0, capdead = 0;
    for (uint32_t i = 0; i < g_nblk; ++i) {
        GCBlock *b = &g_blk[i];
        if (!(b->flags & GC_F_USED)) continue;
        if (b->flags & GC_F_CHUNK) {
            GCArenaRange *range = gc_arena_by_id(b->arena_id);
            if (!range) continue;
            /* Arena chunks are immortal bump storage (see xarena): carved
               strings/int boxes are never individually freed, and a chunk
               can hold live objects referenced only from C cells that the
               conservative marker does not scan (e.g. refs that survive
               only in unscanned heap blocks). Reclaiming such a chunk
               munmaps live data and segfaults on next use. With 2 MiB
               chunks this was rare; 128 KiB chunks spread the working
               set over many inactive chunks and made the race likely.
               Retain chunks: drop the candidate bookkeeping, keep the
               ranges (and mappings) for the process lifetime. */
            (void)range;
            continue;
        }
        if (b->flags & GC_F_MARK) continue;
        /* Python wrappers (T_PYOBJ/T_PYBUF) are collectible here via
           a_py_release_obj in gc_free_obj_interior; gc_obj_pinned
           remains for region hooks, which do not synchronously enter
           the interpreter. */
        if (b->flags & GC_F_OBJ) {
            if (ndead == capdead) {
                size_t nc = capdead ? capdead * 2 : 256;
                uint32_t *nd = (uint32_t *)realloc(dead, nc * sizeof(uint32_t));
                if (nd) { dead = nd; capdead = nc; }
            }
            if (ndead < capdead) dead[ndead++] = i;
        } else {
            /* Unmarked object or non-object block: delist only. This is a
               deliberate conservative fallback because root scanning can
               miss live pointers in unscanned C cells. */
            blk_delist_fast(i);
            ++g_ndelisted;
        }
    }

    uint64_t sweep_freed_bytes = 0;
    for (size_t i = 0; i < ndead; ++i) {
        uint32_t idx = dead[i];
        if (!(g_blk[idx].flags & GC_F_USED)) continue; /* released already */
        void *obj = (void *)g_blk[idx].start;
        size_t sz = g_blk[idx].end - g_blk[idx].start;
        gc_free_obj_interior(obj);
        g_nfreed_objs++;
        g_nfreed_bytes += sz;
        sweep_freed_bytes += sz;
        blk_delist_fast(idx);
        pool_release(obj, sz);
    }
    gc_raw_free(dead);
#if defined(__GLIBC__)
    /* The pool above is bounded; everything else just went back to libc.
       Churn otherwise leaves glibc's retained free chunks as the RSS
       floor. Trim only when this sweep released a meaningful amount. */
    if (sweep_freed_bytes >= (1u << 20)) malloc_trim(0);
#endif
    if (g_prof) p_t_sweep += gc_now_s() - t0;
    t0 = g_prof ? gc_now_s() : 0.0;
    if (dbg) fprintf(stderr, "[gc] sweep done: ndead=%zu\n", ndead);

    /* Reset mark bits and recompute the heap-growth threshold.  Bump-arena
       chunk extents are not counted here: each 2 MiB chunk would make the
       collection cadence jump far beyond the workload that should
       trigger the next sweep. */
    g_live_bytes = 0;
    for (uint32_t i = 0; i < g_nblk; ++i) {
        GCBlock *b = &g_blk[i];
        if (!(b->flags & GC_F_USED)) continue;
        b->flags &= ~GC_F_MARK;
        if (!(b->flags & GC_F_CHUNK)) g_live_bytes += b->end - b->start;
    }
    g_alloc_bytes = 0;
    if (g_prof) p_t_live += gc_now_s() - t0;
    /* The sweep delisted blocks without touching the hash; rebuild it
       once now that only the survivors remain. */
    if (g_hash_stale) ht_rebuild();
    /* Collect again once we've allocated ~8x the live heap (min 8 MiB) —
       a tighter threshold re-collects far too often on allocation-heavy
       programs (each collection is O(heap)), which is what made graph.a
       pathologically slow; a looser one lets churn.a retain too much
       floating garbage between sweeps. */
    uint64_t next = g_live_bytes * 8;
    if (next < (2u << 20)) next = 2u << 20;
    g_next_gc = next;
    ++g_ncollections;

    if (gc_stats())
        fprintf(stderr,
                "gc: collection #%llu: freed %llu objs (%llu B), "
                "delisted %llu, live %llu B in %u blocks\n",
                (unsigned long long)g_ncollections,
                (unsigned long long)g_nfreed_objs,
                (unsigned long long)g_nfreed_bytes,
                (unsigned long long)g_ndelisted,
                (unsigned long long)g_live_bytes, g_nused);
        if (g_prof) {
            fprintf(stderr,
                    "gc-prof: words=%llu (stack=%llu blk=%llu) "
                    "prefpass=%llu finds=%llu hits=%llu wlpush=%llu marked=%llu "
                    "ubwin=%llu ubgallop=%llu unaligned=%llu\n"
                    "gc-prof: t build=%.1fms roots=%.1fms regs=%.1fms "
                    "stack=%.1fms fix=%.1fms sweep=%.1fms live=%.1fms\n",
                    (unsigned long long)p_words,
                    (unsigned long long)p_stack_words,
                    (unsigned long long)p_blk_words,
                    (unsigned long long)p_pref_pass,
                    (unsigned long long)p_finds,
                    (unsigned long long)p_find_hits,
                    (unsigned long long)p_wl_push,
                    (unsigned long long)p_marked_blks,
                    (unsigned long long)p_ub_win,
                    (unsigned long long)p_ub_gallop,
                    (unsigned long long)p_unaligned,
                    p_t_build * 1e3, p_t_roots * 1e3, p_t_regs * 1e3,
                    p_t_stack * 1e3, p_t_fix * 1e3, p_t_sweep * 1e3,
                    p_t_live * 1e3);
            p_words = p_pref_pass = p_finds = p_find_hits = 0;
            p_wl_push = p_marked_blks = p_stack_words = p_blk_words = 0;
            p_ub_win = p_ub_gallop = p_unaligned = 0;
            p_t_build = p_t_roots = p_t_regs = p_t_stack = 0;
            p_t_fix = p_t_sweep = p_t_live = 0;
        }

    g_in_gc = 0;
}

/* ------------------------------------------------------------------ */
/* public hooks (called from core_util.c / mkobj / rt_pool.c / roots)  */
/* ------------------------------------------------------------------ */

static void gc_ensure_init(void) {
    /* Everything is lazily allocated on first use; no explicit init. */
}

void a_gc_maybe_collect(void) {
    if (!gc_enabled() || g_in_gc) return;
    if (g_alloc_bytes < g_next_gc) return;
    if (a_par_gc_blocked()) return;
    gc_collect();
}

void a_gc_register(void *p, size_t n) {
    if (!gc_enabled() || !p) return;
    gc_ensure_init();
    /* If a stale entry still claims this start address (a block that was
       released without delisting, then the address was reused), drop it
       first — otherwise two USED slots would share a start: the hash maps
       to only one of them and g_nused inflates without bound. */
    {
        uint32_t old = ht_find((uintptr_t)p);
        if (old != GC_NONE) blk_delist(old);
    }
    uint32_t i = blk_alloc_slot();
    if (i == GC_NONE) return;
    GCBlock *b = &g_blk[i];
    b->start = (uintptr_t)p;
    b->end = b->start + (n ? n : 1);
    b->flags = GC_F_USED;
    b->next = 0;
    b->arena_id = 0;
    b->region = 0;
    ht_put(i);
    ++g_nused;
    g_alloc_bytes += n ? n : 1;
    if (getenv("A_GC_TRACE") && n >= ((size_t)256 << 10)) {
        fprintf(stderr, "[gc-trace] register %p sz=%zu idx=%u ra=%p ra1=%p\n",
                p, n, i, __builtin_return_address(0),
                __builtin_return_address(1));
        if (n >= ((size_t)1 << 20)) {
            void *bt[32];
            int nb = backtrace(bt, 32);
            backtrace_symbols_fd(bt, nb, 2);
        }
    }
}

void a_gc_reregister(void *oldp, void *newp, size_t n) {
    if (!gc_enabled()) return;
    if (oldp) {
        uint32_t i = ht_find((uintptr_t)oldp);
        if (getenv("A_GC_TRACE") && n >= ((size_t)256 << 10))
            fprintf(stderr, "[gc-trace] reregister old=%p new=%p n=%zu hit=%u same=%d\n",
                    oldp, newp, n, i, oldp == newp);
        if (i != GC_NONE) blk_delist(i);
        else if (getenv("A_GC_TRACE") && n >= ((size_t)256 << 10))
            fprintf(stderr, "[gc-trace] reregister MISS old=%p\n", oldp);
    }
    if (newp) a_gc_register(newp, n);
}

/* mkobj() calls a_gc_note_obj() immediately followed by
   a_region_note_obj() -> a_gc_track_region() for the same pointer. Cache
   that one registry lookup so object creation does not hash twice. The
   cache is per-thread and consumed on first use; any other caller falls
   back to ht_find(). */
static A_RT_TLS uintptr_t g_note_addr;
static A_RT_TLS uint32_t g_note_slot;
static A_RT_TLS int g_note_hit;

void a_gc_note_obj(void *p) {
    g_note_hit = 0;
    if (!gc_enabled() || !p || g_in_gc) return;
    /* Only flag when p is exactly a registered block start: arena objects
       (xarena chunks, including the mmap fast path which is never
       registered) simply miss the hash and stay untracked/immortal. */
    uint32_t i = ht_find((uintptr_t)p);
    g_note_addr = (uintptr_t)p;
    g_note_slot = i;
    g_note_hit = 1;
    if (i == GC_NONE) return;
    if (gc_in_arena((uintptr_t)p)) return;
    g_blk[i].flags |= GC_F_OBJ;
}

void a_gc_free(void *p) {
    if (!p) return;
    if (gc_enabled()) {
        uint32_t i = ht_find((uintptr_t)p);
        if (i != GC_NONE && (g_blk[i].flags & GC_F_USED)) {
            size_t sz = g_blk[i].end - g_blk[i].start;
            if (getenv("A_GC_TRACE") && sz >= ((size_t)256 << 10))
                fprintf(stderr, "[gc-trace] a_gc_free tracked %p sz=%zu idx=%u flags=%x\n",
                        p, sz, i, g_blk[i].flags);
            blk_delist(i);
            pool_release(p, sz);
            return;
        }
        if (getenv("A_GC_TRACE"))
            fprintf(stderr, "[gc-trace] a_gc_free UNTRACKED %p (ht=%u)\n",
                    p, i);
    }
    gc_raw_free(p);
}

/* ------------------------------------------------------------------ */
/* region.c hooks: deterministic young-generation reclamation.         */
/* GC_F_OLD marks objects that escaped their birth region (stored into  */
/* a container that outlives the frame, captured by a closure, stored  */
/* to a global/static, or returned). Region sweep frees young objects   */
/* only; OLD objects remain GC-managed exactly as before.              */
/* ------------------------------------------------------------------ */

/* 1 when p is a registered GC object that regions may reclaim
   (excludes arena/xarena objects, which are not individually freeable,
   and pinned T_PYOBJ/T_PYBUF objects). */
int a_gc_regionable(void *p) {
    if (!gc_enabled() || !p || g_in_gc) return 0;
    uint32_t i = ht_find((uintptr_t)p);
    if (i == GC_NONE) return 0;
    GCBlock *b = &g_blk[i];
    if (!(b->flags & GC_F_USED) || !(b->flags & GC_F_OBJ)) return 0;
    if (gc_in_arena((uintptr_t)p)) return 0;
    if (gc_obj_pinned(p)) return 0;
    return 1;
}

/* Single-lookup variant of regionable+set_region for the mkobj hot path:
   stamps the block's region depth and returns 1 iff the object is
   region-trackable (registered GC object, not arena, not pinned). */
int a_gc_track_region(void *p, uint32_t depth) {
    if (!gc_enabled() || !p || g_in_gc) return 0;
    int cached = g_note_hit && g_note_addr == (uintptr_t)p;
    uint32_t i = cached ? g_note_slot : ht_find((uintptr_t)p);
    g_note_hit = 0;
    if (i == GC_NONE) return 0;
    GCBlock *b = &g_blk[i];
    if (!(b->flags & GC_F_USED) || !(b->flags & GC_F_OBJ)) return 0;
    /* A cached hit means a_gc_note_obj() just ran for this pointer: GC_F_OBJ
       is only set after its arena check passed, so the range scan is not
       needed again here. */
    if (!cached && gc_in_arena((uintptr_t)p)) return 0;
    if (gc_obj_pinned(p)) return 0;
    b->region = depth;
    return 1;
}

void a_gc_set_region(void *p, uint32_t depth) {
    if (!gc_enabled() || !p) return;
    uint32_t i = ht_find((uintptr_t)p);
    if (i == GC_NONE) return;
    g_blk[i].region = depth;
}

uint32_t a_gc_obj_region(void *p) {
    if (!gc_enabled() || !p) return 0;
    uint32_t i = ht_find((uintptr_t)p);
    return i == GC_NONE ? 0 : g_blk[i].region;
}

int a_gc_obj_old(void *p) {
    if (!gc_enabled() || !p) return 0;
    uint32_t i = ht_find((uintptr_t)p);
    return i != GC_NONE && (g_blk[i].flags & GC_F_OLD) != 0;
}

void a_gc_promote_block(void *p) {
    if (!gc_enabled() || !p) return;
    uint32_t i = ht_find((uintptr_t)p);
    if (i != GC_NONE) g_blk[i].flags |= GC_F_OLD;
}

int a_gc_registered_obj(void *p) {
    if (!p) return 0;
    uint32_t i = ht_find((uintptr_t)p);
    if (i == GC_NONE) return 0;
    GCBlock *b = &g_blk[i];
    return ((b->flags & (GC_F_USED | GC_F_OBJ)) == (GC_F_USED | GC_F_OBJ));
}

/* Free a young region object (interior buffers + delist + release) when
   it was not promoted. Returns 1 when the object was freed. */
int a_gc_release_obj(void *p) {
    if (!gc_enabled() || !p || g_in_gc) return 0;
    uint32_t i = ht_find((uintptr_t)p);
    if (i == GC_NONE) {
        if (gc_debug()) fprintf(stderr, "[gc] release_obj %p: not registered\n", p);
        return 0;
    }
    GCBlock *b = &g_blk[i];
    if (!(b->flags & GC_F_USED) || !(b->flags & GC_F_OBJ)) {
        if (gc_debug()) fprintf(stderr, "[gc] release_obj %p: flags=%x not obj\n", p, b->flags);
        return 0;
    }
    if (b->flags & (GC_F_OLD | GC_F_MARK)) {
        if (gc_debug()) fprintf(stderr, "[gc] release_obj %p: flags=%x skip\n", p, b->flags);
        return 0;
    }
    if (gc_obj_pinned(p)) return 0;
    if (gc_debug()) fprintf(stderr, "[gc] release_obj %p: FREEING\n", p);
    size_t sz = b->end - b->start;
    gc_free_obj_interior(p);
    blk_delist(i);
    pool_release(p, sz);
    return 1;
}
