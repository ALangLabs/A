#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* arena module: bump/pool allocators                                  */
/* ------------------------------------------------------------------ */

static AArena *arena_of(aval v) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_ARENA) panicf("arena expected, not '%s'", type_name_cstr(v));
    return (AArena *)o;
}

/* Free-slot list: when allocs[i].free is set, allocs[i].off (the bump
   offset, which nothing reads back) stores the next free index + 1, 0 =
   end of list. The head lives in h.cap — the AObj cap word is unused for
   T_ARENA (arena_new_impl zeroes it and only `cap`/`bump`/`live`/
   `nalloc`/`capalloc`/`allocs` carry meaning) — as head_index + 1, 0 =
   empty. Reuse stays first-fit over the free list, so arena.alloc no
   longer scans every live slot on each call. */
#define ARENA_FREE_NIL 0

static aval arena_new_impl(uint64_t cap) {
    AArena *a = (AArena *)xmalloc(sizeof(AArena));
    a->h.type = T_ARENA;
    a->h.len = 0;
    a->h.cap = ARENA_FREE_NIL;   /* free-list head (index + 1, 0 = empty) */
    a->cap = cap;
    a->bump = 0;
    a->live = 0;
    a->nalloc = 0;
    a->capalloc = 0;
    a->allocs = NULL;
    return mkobj(&a->h);
}

aval bi_arena_new(uint64_t n, aval *argv) {
    want_args("arena.new", n, 0, 1);
    uint64_t cap = n ? (uint64_t)expect_int64(argv[0], "arena capacity") : 1u << 20;
    return arena_new_impl(cap);
}

aval bi_arena_pool(uint64_t n, aval *argv) {
    want_args("arena.pool", n, 1, 1);
    return arena_new_impl((uint64_t)expect_int64(argv[0], "arena capacity"));
}

aval bi_arena_region(uint64_t n, aval *argv) {
    want_args("arena.region", n, 1, 1);
    return arena_new_impl((uint64_t)expect_int64(argv[0], "arena capacity"));
}

aval bi_arena_alloc(uint64_t n, aval *argv) {
    want_args("arena.alloc", n, 2, 2);
    AArena *a = arena_of(argv[0]);
    int64_t sz = expect_int64(argv[1], "arena.alloc size");
    if (sz < 0) panicf("arena.alloc: size must be non-negative");
    uint64_t size = (uint64_t)sz;
    size = (size + 7) & ~7ULL;
    /* reuse a freed slot when possible: walk the free list and unlink the
       first slot whose recorded size still covers the request. LIFO
       ordering makes the common alloc/release/alloc pattern O(1). */
    {
        uint64_t prev = ARENA_FREE_NIL;   /* index + 1 of previous node */
        uint64_t cur = a->h.cap;          /* index + 1 of current node */
        while (cur != ARENA_FREE_NIL) {
            uint64_t idx = cur - 1;
            uint64_t nxt = a->allocs[idx].off; /* next free index + 1 */
            if (a->allocs[idx].free && a->allocs[idx].size >= size) {
                if (prev != ARENA_FREE_NIL)
                    a->allocs[prev - 1].off = nxt;
                else
                    a->h.cap = nxt;
                a->allocs[idx].free = 0;
                /* Record the requested size, not the slot's original size:
                   arena.release subtracts allocs[h-1].size from live, so
                   keeping the larger stale size would underflow `live`. */
                a->allocs[idx].size = size;
                a->live += size;
                return mkint_full((int64_t)(idx + 1));
            }
            prev = cur;
            cur = nxt;
        }
    }
    if (a->nalloc == a->capalloc) {
        a->capalloc = a->capalloc ? a->capalloc * 2 : 16;
        a->allocs = (AAlloc *)xrealloc(a->allocs, a->capalloc * sizeof(AAlloc));
    }
    if (a->bump + size > a->cap) panicf("arena exhausted");
    uint64_t idx = a->nalloc++;
    a->allocs[idx].off = a->bump;
    a->allocs[idx].size = size;
    a->allocs[idx].free = 0;
    a->bump += size;
    a->live += size;
    return mkint_full((int64_t)(idx + 1));
}

aval bi_arena_release(uint64_t n, aval *argv) {
    want_args("arena.release", n, 2, 2);
    AArena *a = arena_of(argv[0]);
    int64_t h0 = expect_int64(argv[1], "arena.release handle");
    if (h0 < 0) panicf("arena.release: invalid handle");
    uint64_t h = (uint64_t)h0;
    if (h == 0 || h > a->nalloc || a->allocs[h - 1].free)
        panicf("arena.release: invalid handle");
    a->allocs[h - 1].free = 1;
    a->live -= a->allocs[h - 1].size;
    /* push onto the free list (LIFO): off = old head, head = this slot */
    a->allocs[h - 1].off = a->h.cap;
    a->h.cap = h;
    return A_NONE;
}

aval bi_arena_reset(uint64_t n, aval *argv) {
    want_args("arena.reset", n, 1, 1);
    AArena *a = arena_of(argv[0]);
    a->bump = 0;
    a->live = 0;
    a->nalloc = 0;
    a->h.cap = ARENA_FREE_NIL;   /* drop the free list with the slots */
    return A_NONE;
}

aval bi_arena_used(uint64_t n, aval *argv) {
    want_args("arena.used", n, 1, 1);
    return mkint_full((int64_t)arena_of(argv[0])->live);
}

aval bi_arena_total(uint64_t n, aval *argv) {
    want_args("arena.total", n, 1, 1);
    return mkint_full((int64_t)arena_of(argv[0])->cap);
}

