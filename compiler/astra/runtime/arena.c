#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* arena module: bump/pool allocators                                  */
/* ------------------------------------------------------------------ */

static AArena *arena_of(aval v) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_ARENA) panicf("arena expected, not '%s'", type_name_cstr(v));
    return (AArena *)o;
}

static aval arena_new_impl(uint64_t cap) {
    AArena *a = (AArena *)xmalloc(sizeof(AArena));
    a->h.type = T_ARENA;
    a->h.len = 0;
    a->h.cap = 0;
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
    uint64_t cap = n ? (uint64_t)int_val_full(argv[0]) : 1u << 20;
    return arena_new_impl(cap);
}

aval bi_arena_pool(uint64_t n, aval *argv) {
    want_args("arena.pool", n, 1, 1);
    return arena_new_impl((uint64_t)int_val_full(argv[0]));
}

aval bi_arena_region(uint64_t n, aval *argv) {
    want_args("arena.region", n, 1, 1);
    return arena_new_impl((uint64_t)int_val_full(argv[0]));
}

aval bi_arena_alloc(uint64_t n, aval *argv) {
    want_args("arena.alloc", n, 2, 2);
    AArena *a = arena_of(argv[0]);
    uint64_t size = (uint64_t)int_val_full(argv[1]);
    size = (size + 7) & ~7ULL;
    /* reuse a freed slot when possible */
    for (uint64_t i = 0; i < a->nalloc; ++i) {
        if (a->allocs[i].free && a->allocs[i].size >= size) {
            a->allocs[i].free = 0;
            a->live += size;
            return mkint_full((int64_t)(i + 1));
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
    uint64_t h = (uint64_t)int_val_full(argv[1]);
    if (h == 0 || h > a->nalloc || a->allocs[h - 1].free)
        panicf("arena.release: invalid handle");
    a->allocs[h - 1].free = 1;
    a->live -= a->allocs[h - 1].size;
    return A_NONE;
}

aval bi_arena_reset(uint64_t n, aval *argv) {
    want_args("arena.reset", n, 1, 1);
    AArena *a = arena_of(argv[0]);
    a->bump = 0;
    a->live = 0;
    a->nalloc = 0;
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

