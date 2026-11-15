#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* arrays / dicts / sets                                               */
/* ------------------------------------------------------------------ */

AArr *arr_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE && o->type != T_SET))
        panicf("%s must be a list, tuple, or set", what);
    return (AArr *)o;
}

/* Geometric growth at ~1.5x instead of doubling: still amortized O(1)
   appends, but the worst-case slack after a growth step drops from 50%
   to 33% of capacity — meaningful for the large retained lists that
   dominate RSS. */
static uint64_t arr_next_cap(uint64_t cap, uint64_t need) {
    uint64_t nc = cap ? cap : 4;
    while (nc < need) {
        uint64_t step = nc / 2;
        nc += step ? step : 1; /* cap can be as small as 1 (exact-fit lists) */
    }
    return nc;
}

void arr_reserve(AArr *a, uint64_t need) {
    /* Safety net: any caller that reserved aval capacity on a packed
       array first converts it back to aval storage. */
    if (arr_packed(a)) arr_ensure_aval(a);
    if (need <= a->h.cap) return;
    uint64_t nc = arr_next_cap(a->h.cap, need);
    a->items = (aval *)xrealloc(a->items, (size_t)nc * sizeof(aval));
    /* items was NULL (fresh buffer) or a realloc that kept the flag —
       re-assert NOSCAN for F64 lists whose old block did not carry it. */
    if (a->flags & ARR_F_F64) a_gc_set_noscan(a->items, 1);
    a->h.cap = nc;
}

void arr_reserve_i64(AArr *a, uint64_t need) {
    if (need <= a->h.cap) return;
    uint64_t nc = arr_next_cap(a->h.cap, need);
    a->iitems = (int64_t *)xrealloc(a->iitems, (size_t)nc * sizeof(int64_t));
    arr_iitems_raw(a);   /* packed int64 storage never holds pointers */
    a->h.cap = nc;
}

int arr_packed(const AArr *a) { return (a->flags & ARR_F_PACKED) != 0; }

/* Convert packed int64 storage back to aval elements in place. Lazy and
   reversible: nothing here sets ARR_F_NOMIX, so a later arr_push_int /
   arr_try_pack may pack the array again if it is still all-integer. */
void arr_ensure_aval(AArr *a) {
    if (!arr_packed(a)) return;
    uint64_t cap = a->h.cap ? a->h.cap : 1;
    aval *items = (aval *)xmalloc((size_t)cap * sizeof(aval));
    for (uint64_t i = 0; i < a->h.len; ++i)
        items[i] = mkint_full(a->iitems[i]);
    free(a->iitems);
    a->iitems = NULL;
    a->items = items;
    a->flags &= ~(ARR_F_PACKED | ARR_F_F64);
}

/* aval -> packed. Caller must have verified every element is an int
   (is_int_val); int_val_full then cannot panic. */
static void arr_to_packed(AArr *a) {
    uint64_t cap = a->h.cap ? a->h.cap : 4;
    int64_t *iitems = (int64_t *)xmalloc((size_t)cap * sizeof(int64_t));
    for (uint64_t i = 0; i < a->h.len; ++i)
        iitems[i] = int_val_full(a->items[i]);
    free(a->items);
    a->items = NULL;
    a->iitems = iitems;
    arr_iitems_raw(a);
    a->flags = (a->flags & ~ARR_F_F64) | ARR_F_PACKED;
}

/* Pack an aval T_LIST whose elements are all integers. No-op for
   tuples/sets, already-packed arrays, and arrays marked NOMIX. */
void arr_try_pack(AArr *a) {
    if (a->h.type != T_LIST || arr_packed(a) || (a->flags & ARR_F_NOMIX))
        return;
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (!is_int_val(a->items[i])) {
            a->flags |= ARR_F_NOMIX;
            return;
        }
    arr_to_packed(a);
}

aval arr_elem(const AArr *a, uint64_t i) {
    return arr_packed(a) ? mkint_full(a->iitems[i]) : a->items[i];
}

/* Release spare element capacity after removals (pop/remove_at/remove/
   discard/clear). Wide hysteresis: only when less than 1/8 of the
   allocation is in use and the buffer is at least 32 slots, and never
   for a list a view can still alias (ARR_F_HAS_VIEW) — view offsets are
   absolute into items[], so shrinking the buffer under a live view
   would leave its accesses out of bounds. Sets are never view parents,
   tuples never shrink, so the type check is just T_LIST/T_SET. */
void arr_shrink(AArr *a) {
    if (a->h.type != T_LIST && a->h.type != T_SET) return;
    if (a->flags & ARR_F_HAS_VIEW) return;
    if (a->h.cap < 32 || a->h.len * 8 >= a->h.cap) return;
    if (a->h.len == 0) {
        free(a->items);
        free(a->iitems);
        a->items = NULL;
        a->iitems = NULL;
        a->h.cap = 0;
        return;
    }
    /* ~1.5x headroom over len, matching the growth policy, so a refill
       does not immediately reallocate. */
    uint64_t nc = a->h.len + a->h.len / 2 + 4;
    if (arr_packed(a))
        a->iitems = (int64_t *)xrealloc(a->iitems, (size_t)nc * sizeof(int64_t));
    else
        a->items = (aval *)xrealloc(a->items, (size_t)nc * sizeof(aval));
    if (arr_packed(a)) arr_iitems_raw(a);
    else if (a->flags & ARR_F_F64) a_gc_set_noscan(a->items, 1);
    a->h.cap = nc;
}

/* Mark the root list backing a new view so arr_shrink leaves its buffer
   alone. Views are one level deep (a_slice stores the root list as
   parent), but walk defensively anyway. */
void arr_note_view(aval parent) {
    for (;;) {
        AObj *o = obj_of(parent);
        if (!o) return;
        if (o->type == T_LIST) {
            ((AArr *)o)->flags |= ARR_F_HAS_VIEW;
            return;
        }
        if (o->type != T_VIEW) return;
        parent = ((AView *)o)->parent;
    }
}

void arr_push(AArr *a, aval v) {
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (arr_packed(a)) {
        if (is_int_val(v)) {
            arr_reserve_i64(a, (uint64_t)a->h.len + 1);
            a->iitems[a->h.len++] = int_val_full(v);
            return;
        }
        a->flags |= ARR_F_NOMIX;
        arr_ensure_aval(a);
    }
    arr_reserve(a, (uint64_t)a->h.len + 1);
    if ((a->flags & ARR_F_F64) && !a_is_double(v)) arr_f64_unmark(a);
    a_region_store(a, v);   /* region write barrier */
    a->items[a->h.len++] = v;
}

/* Append preferring packed-int storage: on a packed array int values
   stay packed (non-int converts the whole array to aval); on an aval
   T_LIST an int value triggers a one-time all-int scan that promotes
   the array to packed storage when every existing element is an int. */
void arr_push_int(AArr *a, aval v) {
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (!is_int_val(v)) {
        arr_push(a, v);
        return;
    }
    if (arr_packed(a)) {
        arr_reserve_i64(a, (uint64_t)a->h.len + 1);
        a->iitems[a->h.len++] = int_val_full(v);
        return;
    }
    if (a->h.type == T_LIST && !(a->flags & ARR_F_NOMIX)) {
        int all_int = 1;
        for (uint64_t i = 0; i < a->h.len; ++i)
            if (!is_int_val(a->items[i])) { all_int = 0; break; }
        if (all_int) {
            arr_to_packed(a);
            arr_reserve_i64(a, (uint64_t)a->h.len + 1);
            a->iitems[a->h.len++] = int_val_full(v);
            return;
        }
        a->flags |= ARR_F_NOMIX;
    }
    arr_push(a, v);
}

ADict *dict_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || !is_dict_t(o->type))
        panicf("%s must be dict, not '%s'", what, type_name_cstr(v));
    return (ADict *)o;
}

