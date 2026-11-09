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
    /* String keys are the other hot case (churn/counter maps). Both sides
       being T_STR makes the byte compare authoritative — values_equal
       would reach the same str_eq after kind_of/is_num probing and cycle
       bookkeeping, so answer inline and skip the call entirely. T_CHAR
       and mixed kinds still fall through to values_equal, which owns the
       cross-kind rules (a char never equals a str). */
    AObj *oa = obj_of(a);
    if (oa && oa->type == T_STR) {
        AObj *ob = obj_of(b);
        if (ob && ob->type == T_STR) {
            if (oa->len != ob->len) return 0;
            return oa->len == 0 ||
                   memcmp(((AStr *)oa)->data, ((AStr *)ob)->data,
                          (size_t)oa->len) == 0;
        }
    }
    return values_equal(a, b);
}

static uint64_t dict_key_hash(aval k) {
    /* Fast paths for the scalar keys that dominate real maps. They must
       produce exactly the hashes the generic is_num/num_to_double path
       produced: every numeric key hashes by the canonical double value
       (contract above), so inline ints and doubles — including boxed
       T_INT64 — all funnel through the same double bit pattern. */
    if (a_is_int(k)) {
        union { double d; uint64_t u; } c;
        c.d = (double)a_int_val(k);
        return mix64(c.u);
    }
    if (a_is_double(k)) {
        double d = a_double_val(k);
        if (d == 0.0) d = 0.0; /* -0.0 == +0.0 under values_equal */
        union { double d; uint64_t u; } c;
        c.d = d;
        return mix64(c.u);
    }
    if (is_int_val(k)) { /* boxed T_INT64 outside the inline range */
        union { double d; uint64_t u; } c;
        c.d = (double)int_val_full(k);
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

/* Dicts with fewer than DICT_HASH_MIN pairs skip the slot table entirely
   and answer probes with a linear scan over pairs[]. Most dicts in real
   programs (and nearly every struct instance's field map) stay tiny, so
   this avoids a whole second allocation (slots) plus its load-factor
   slack — and a lookup on a 3-entry dict is faster without hashing the
   key at all. The table appears once the dict grows past the threshold;
   removals drop back to linear mode under the same wide hysteresis as
   the capacity shrink. */
#define DICT_HASH_MIN 8

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
        /* ~1.5x growth (pairs need no power-of-two size); see arr_next_cap. */
        uint64_t nc = d->h.cap ? d->h.cap : 4;
        while (nc < need) {
            uint64_t step = nc / 2;
            nc += step ? step : 1;
        }
        d->pairs = (APair *)xrealloc(d->pairs, (size_t)nc * sizeof(APair));
        d->h.cap = nc;
    }
    if (d->scap == 0) {
        /* Stay linear below DICT_HASH_MIN; build the probe table once the
           dict is big enough for hashing to pay off. */
        if (need < DICT_HASH_MIN) return;
        uint64_t ns = 8;
        while (need * 10 > ns * 7) ns *= 2;
        dict_rehash(d, ns);
        return;
    }
    /* keep the probe table below ~70% load so lookups stay short */
    if (need * 10 > d->scap * 7) {
        uint64_t ns = d->scap;
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
    if (d->scap == 0) {
        /* Linear mode: no hash computed, just pairwise key equality. */
        for (uint64_t i = 0; i < d->h.len; ++i)
            if (dict_key_eq(d->pairs[i].k, k)) return (int64_t)i;
        return -1;
    }
    return dict_find_h(d, k, dict_key_hash(k));
}

void dict_put(ADict *d, aval k, aval v) {
    /* Region write barrier: keys/values escaping into a container that
       outlives their birth region must be promoted (transitively). */
    a_region_store(d, k);
    a_region_store(d, v);
    uint64_t h = 0;
    int have_h = 0;
    int64_t slot = -1;  /* insertion slot from this probe, -1 = unknown */
    if (d->scap) {
        h = dict_key_hash(k);
        have_h = 1;
        int64_t r = dict_probe(d, k, h);
        if (r >= 0) {
            d->pairs[r].v = v;
            return;
        }
        slot = -r - 1;
    } else {
        for (uint64_t i = 0; i < d->h.len; ++i)
            if (dict_key_eq(d->pairs[i].k, k)) {
                d->pairs[i].v = v;
                return;
            }
    }
    if (d->h.len == UINT32_MAX) panicf("dict too large");
    uint64_t scap0 = d->scap;
    dict_reserve(d, (uint64_t)d->h.len + 1);
    uint64_t idx = d->h.len;
    d->pairs[idx].k = k;
    d->pairs[idx].v = v;
    d->h.len++;
    if (!d->scap) return;  /* still in linear-scan range: no slot to fill */
    if (!have_h) h = dict_key_hash(k);
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

/* Backward-shift re-link of the probe cluster broken by a cleared slot
   (Knuth 6.4R): walk forward from the hole; an entry at j with home h
   must move into the hole iff the hole lies on its probe path
   [h .. j], i.e. fwd(h->hole) <= fwd(h->j). Home positions are
   recomputed from keys, but only for the cluster entries actually
   visited — never the whole table. `hole` must already be cleared. */
static void dict_repair_cluster(ADict *d, uint64_t hole) {
    const uint64_t mask = d->scap - 1;
    uint64_t i = hole;   /* current hole position */
    uint64_t j = hole;
    for (;;) {
        j = (j + 1) & mask;
        uint64_t w = d->slots[j];
        if (!w) break;
        uint32_t pi = (uint32_t)w - 1;          /* pair index */
        uint64_t home = dict_key_hash(d->pairs[pi].k) & mask;
        if (((i - home) & mask) <= ((j - home) & mask)) {
            d->slots[i] = w;
            d->slots[j] = 0;
            i = j;               /* j becomes the new hole */
        }
    }
}

/* In-place slot repair after dict_remove_at's pair compaction. The probe
   table keeps its size, so every surviving pair's home slot
   (hash & mask) is unchanged — only the pair index stored in each slot
   word shifts down by one for pairs above the removed position. One pass
   over slots[] applies that shift and clears the removed entry's slot;
   dict_repair_cluster then re-links the probe cluster the cleared slot
   may have broken.

   This replaces the old dict_rehash(d, d->scap), which reallocated,
   memset and re-hashed EVERY key on every pop: O(dict size) hashing per
   removal made churn workloads (insert + periodic pop on a large map)
   quadratic. The scan here is O(scap) of plain word updates plus
   O(cluster) hashes — an order of magnitude cheaper, no allocation. */
static void dict_remove_repair(ADict *d, uint64_t removed) {
    const uint64_t mask = d->scap - 1;
    uint64_t hole = UINT64_MAX;
    for (uint64_t s = 0; s < d->scap; ++s) {
        uint64_t w = d->slots[s];
        if (!w) continue;
        uint64_t j = (uint64_t)(uint32_t)w - 1;  /* stored index was j+1 */
        if (j == removed) {
            d->slots[s] = 0;
            hole = s;
        } else if (j > removed) {
            d->slots[s] = w - 1;   /* low32 j+1 -> j: index shifted down */
        }
    }
    /* Exactly one slot referenced the removed pair; if it is somehow
       absent the table is already consistent and there is nothing to
       repair. */
    if (hole != UINT64_MAX) dict_repair_cluster(d, hole);
}

/* Remove pair i preserving insertion order of the rest; rebuilds the
   index because every pair index above i shifts down by one. */
void dict_remove_at(ADict *d, uint64_t i) {
    uint64_t tail = d->h.len - i - 1;
    memmove(d->pairs + i, d->pairs + i + 1, (size_t)tail * sizeof(APair));
    d->h.len--;
    if (d->h.len == 0) {
        dict_clear(d);
        return;
    }
    /* Wide-hysteresis shrink: only when less than 1/8 of the pair
       capacity is in use. Below DICT_HASH_MIN the probe table is dropped
       entirely (back to linear mode); otherwise slots are rebuilt at the
       smallest power-of-two that keeps load under ~70%. The rehash is
       required anyway — every pair index above i shifted down. */
    if (d->h.cap >= 16 && d->h.len * 8 < d->h.cap) {
        uint64_t nc = d->h.len + d->h.len / 2 + 4;
        d->pairs = (APair *)xrealloc(d->pairs, (size_t)nc * sizeof(APair));
        d->h.cap = nc;
        if (d->h.len < DICT_HASH_MIN) {
            free(d->slots);
            d->slots = NULL;
            d->scap = 0;
            return;
        }
        uint64_t ns = 8;
        while (d->h.len * 10 > ns * 7) ns *= 2;
        if (ns < d->scap) {
            dict_rehash(d, ns);
            return;
        }
    }
    /* Same-size table: positions are unchanged, so repair in place
       instead of rehashing every key (see dict_remove_repair). Removing
       the LAST pair shifts no indices at all — skip the table-wide scan
       and just probe for its single slot (LIFO pop pattern). */
    if (d->scap) {
        if (i == d->h.len) {
            /* pairs[i] is past h.len now but still inside the allocation
               (tail was 0, nothing overwrote it): its key/hash are intact. */
            aval k = d->pairs[i].k;
            uint64_t h = dict_key_hash(k);
            uint64_t mask = d->scap - 1;
            uint64_t s = h & mask;
            for (;;) {
                uint64_t w = d->slots[s];
                if (!w) break;                 /* cannot happen */
                if ((uint32_t)w == (uint32_t)i + 1) {
                    d->slots[s] = 0;
                    dict_repair_cluster(d, s);
                    return;
                }
                s = (s + 1) & mask;
            }
            return;
        }
        dict_remove_repair(d, i);
    }
}

void dict_clear(ADict *d) {
    /* Release both buffers: a cleared dict that stays empty (or is only
       refilled with a few entries) should not pin its peak capacity.
       dict_put/dict_find handle scap == 0 (linear mode) from scratch. */
    d->h.len = 0;
    d->h.cap = 0;
    free(d->pairs);
    d->pairs = NULL;
    free(d->slots);
    d->slots = NULL;
    d->scap = 0;
}

int set_contains(AArr *s, aval v) {
    for (uint64_t i = 0; i < s->h.len; ++i)
        if (values_equal(s->items[i], v)) return 1;
    return 0;
}

