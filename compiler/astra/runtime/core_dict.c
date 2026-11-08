#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* dict hash index                                                     */
/* ------------------------------------------------------------------ */
/* pairs[] preserves insertion order; slots[] is an open-addressed table
   (linear probing, power-of-two scap, 0 = empty, else pair index + 1)
   that maps each pair's cached hash to its index, giving O(1) lookup.

   Hash/equality consistency contract with values_equal:
   - numbers: values_equal compares int-int by int64 and int-float by
     double, so all numeric keys hash by the canonical double value
     (bits of num_to_double, -0.0 folded to +0.0). Equal keys always
     produce equal doubles, hence equal hashes; unequal keys may collide
     harmlessly (verified by values_equal).
   - str/char: hash the bytes; equal strings have equal bytes.
   - None/bool: hash the aval constant.
   - every other kind (list/tuple/dict/set/view/range/fnon/env/result/
     pyobj): a single constant hash. values_equal does deep equality and
     views compare equal across list/tuple/view kinds, so these share one
     hash domain; lookups among them probe one cluster and degrade to
     the old linear scan confined to that cluster (still verified by
     values_equal). Complex keys can never equal a simple key, so the
     domains never need to merge. */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

/* Key equality for probe steps. Every scalar that A can compare by value is
   canonical in its aval form: an inline int has one bit pattern per value,
   and A_NONE/A_TRUE/A_FALSE are constants. So a matching pair of avals is
   an answer without touching values_equal, and two distinct inline ints are
   unequal by the same argument. This removes a call per probe, which was
   most of the per-operation cost of int-keyed maps (the common case in
   counters, memo tables and histogram code). */
static inline int dict_key_eq(aval a, aval b) {
    if (a == b) return 1;
    if (a_is_int(a) && a_is_int(b)) return 0;
    return values_equal(a, b);
}

static uint64_t dict_key_hash(aval k) {
    if (is_num(k)) {
        double d = num_to_double(k);
        if (d == 0.0) d = 0.0; /* -0.0 == +0.0 under values_equal */
        union { double d; uint64_t u; } c;
        c.d = d;
        return mix64(c.u);
    }
    if (a_is_none(k) || k == A_TRUE || k == A_FALSE)
        return mix64(k ^ UINT64_C(0x517cc1b727220a95));
    AObj *o = obj_of(k);
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        /* FNV-1a over the bytes, then final mix. */
        uint64_t h = UINT64_C(0xcbf29ce484222325);
        const char *p = ((AStr *)o)->data;
        for (uint32_t i = 0; i < o->len; ++i) {
            h ^= (unsigned char)p[i];
            h *= UINT64_C(0x100000001b3);
        }
        return mix64(h ^ ((uint64_t)o->type << 56));
    }
    /* complex/unhashable-by-value keys: one shared domain (see above) */
    return UINT64_C(0x9e3779b97f4a7c15);
}

/* Rebuild slots from pairs[0..len), re-deriving each key hash (the slot
   word carries the hash tag, so there is no separate hash array).
   ncap must be a power of two with load factor < 1. */
static inline uint32_t dict_tag(uint64_t h) { return (uint32_t)(h >> 32); }

static void dict_rehash(ADict *d, uint64_t ncap) {
    free(d->slots);
    d->slots = (uint64_t *)xmalloc((size_t)ncap * sizeof(uint64_t));
    memset(d->slots, 0, (size_t)ncap * sizeof(uint64_t));
    d->scap = ncap;
    uint64_t mask = ncap - 1;
    for (uint64_t i = 0; i < d->h.len; ++i) {
        uint64_t h = dict_key_hash(d->pairs[i].k);
        uint64_t s = h & mask;
        while (d->slots[s]) s = (s + 1) & mask;
        d->slots[s] = ((uint64_t)dict_tag(h) << 32) | (uint32_t)(i + 1);
    }
}

static void dict_reserve(ADict *d, uint64_t need) {
    if (need > d->h.cap) {
        uint64_t nc = d->h.cap ? d->h.cap : 4;
        while (nc < need) nc *= 2;
        d->pairs = (APair *)xrealloc(d->pairs, (size_t)nc * sizeof(APair));
        d->h.cap = nc;
    }
    /* keep the probe table below ~70% load so lookups stay short */
    if (d->scap == 0 || need * 10 > d->scap * 7) {
        uint64_t ns = d->scap ? d->scap : 8;
        while (need * 10 > ns * 7) ns *= 2;
        dict_rehash(d, ns);
    }
}

static int64_t dict_find_h(ADict *d, aval k, uint64_t h) {
    if (d->scap == 0) return -1;
    uint64_t mask = d->scap - 1;
    uint64_t s = h & mask;
    const uint32_t tag = dict_tag(h);
    for (;;) {
        uint64_t w = d->slots[s];
        if (!w) return -1;
        if ((uint32_t)(w >> 32) == tag) {
            uint32_t i = (uint32_t)w - 1;
            if (dict_key_eq(d->pairs[i].k, k)) return (int64_t)i;
        }
        s = (s + 1) & mask;
    }
}

/* Probe once for `k`, returning the pair index (>= 0) or, when absent, the
   slot where it must be inserted encoded as -(slot + 1). Callers that insert
   on absence therefore never re-walk the probe chain: finding a key and
   finding its insertion point are the same scan. */
static int64_t dict_probe(ADict *d, aval k, uint64_t h) {
    uint64_t mask = d->scap ? d->scap - 1 : 0;
    uint64_t s = h & mask;
    const uint32_t tag = dict_tag(h);
    while (d->scap) {
        uint64_t w = d->slots[s];
        if (!w) break;
        if ((uint32_t)(w >> 32) == tag) {
            uint32_t i = (uint32_t)w - 1;
            if (dict_key_eq(d->pairs[i].k, k)) return (int64_t)i;
        }
        s = (s + 1) & mask;
    }
    return -(int64_t)(s + 1);
}

int64_t dict_find(ADict *d, aval k) {
    return dict_find_h(d, k, dict_key_hash(k));
}

void dict_put(ADict *d, aval k, aval v) {
    uint64_t h = dict_key_hash(k);
    int64_t slot = -1;  /* insertion slot from this probe, -1 = unknown */
    if (d->scap) {
        int64_t r = dict_probe(d, k, h);
        if (r >= 0) {
            d->pairs[r].v = v;
            return;
        }
        slot = -r - 1;
    }
    if (d->h.len == UINT32_MAX) panicf("dict too large");
    uint64_t scap0 = d->scap;
    dict_reserve(d, (uint64_t)d->h.len + 1);
    uint64_t idx = d->h.len;
    d->pairs[idx].k = k;
    d->pairs[idx].v = v;
    d->h.len++;
    uint64_t mask = d->scap - 1;
    uint64_t s;
    if (d->scap == scap0 && slot >= 0) {
        s = (uint64_t)slot;  /* same table: reuse the probed empty slot */
    } else {
        s = h & mask;  /* rehash moved every slot: walk the new chain */
        while (d->slots[s]) s = (s + 1) & mask;
    }
    d->slots[s] = ((uint64_t)dict_tag(h) << 32) | (uint32_t)(idx + 1);
}

/* Remove pair i preserving insertion order of the rest; rebuilds the
   index because every pair index above i shifts down by one. */
void dict_remove_at(ADict *d, uint64_t i) {
    uint64_t tail = d->h.len - i - 1;
    memmove(d->pairs + i, d->pairs + i + 1, (size_t)tail * sizeof(APair));
    d->h.len--;
    if (d->scap) dict_rehash(d, d->scap);
}

void dict_clear(ADict *d) {
    d->h.len = 0;
    if (d->scap) memset(d->slots, 0, (size_t)d->scap * sizeof(uint64_t));
}

int set_contains(AArr *s, aval v) {
    for (uint64_t i = 0; i < s->h.len; ++i)
        if (values_equal(s->items[i], v)) return 1;
    return 0;
}

