#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* unsafe module                                                       */
/* ------------------------------------------------------------------ */

/* address_of/load/store/offset use a flat table of aval slots; an
   "address" is a 1-based slot index. */
static aval *g_ptrtab;
static size_t g_nptrtab, g_capptrtab;

static uint64_t ptr_id(aval v) {
    if (kind_of(v) != K_INT) panicf("unsafe: address must be int");
    uint64_t id = (uint64_t)int_val_full(v);
    if (id == 0 || id > g_nptrtab) panicf("unsafe: invalid address");
    return id - 1;
}

aval bi_unsafe_get(uint64_t n, aval *argv) {
    want_args("unsafe.get", n, 2, 2);
    AObj *o = obj_of(argv[0]);
    int64_t i = int_val_full(argv[1]);
    if (o && (o->type == T_LIST || o->type == T_TUPLE)) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.get: index out of bounds");
        return arr_elem(a, (uint64_t)i);
    }
    if (o && o->type == T_SET) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.get: index out of bounds");
        return a->items[i];
    }
    if (o && is_dict_t(o->type)) {
        ADict *d = (ADict *)o;
        int64_t idx = dict_find(d, argv[1]);
        if (idx < 0) return A_NONE;
        return d->pairs[idx].v;
    }
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        uint64_t len;
        const char *p = str_data(argv[0], &len);
        if (i < 0 || (uint64_t)i >= len) panicf("unsafe.get: index out of bounds");
        char c = p[i];
        return a_char_new(&c, 1);
    }
    panicf("unsafe.get: unsupported container '%s'", type_name_cstr(argv[0]));
    return A_NONE;
}

aval bi_unsafe_set(uint64_t n, aval *argv) {
    want_args("unsafe.set", n, 3, 3);
    AObj *o = obj_of(argv[0]);
    int64_t i = int_val_full(argv[1]);
    if (o && (o->type == T_LIST || o->type == T_TUPLE)) {
        AArr *a = (AArr *)o;
        if (i < 0 || (uint64_t)i >= a->h.len) panicf("unsafe.set: index out of bounds");
        if (arr_packed(a)) {
            if (is_int_val(argv[2])) {
                a->iitems[i] = int_val_full(argv[2]);
                return A_NONE;
            }
            a->flags |= ARR_F_NOMIX;
            arr_ensure_aval(a);
        }
        a->items[i] = argv[2];
        return A_NONE;
    }
    if (o && is_dict_t(o->type)) {
        dict_put((ADict *)o, argv[1], argv[2]);
        return A_NONE;
    }
    panicf("unsafe.set: unsupported container '%s'", type_name_cstr(argv[0]));
    return A_NONE;
}

aval bi_unsafe_address_of(uint64_t n, aval *argv) {
    want_args("unsafe.address_of", n, 1, 1);
    if (g_nptrtab == g_capptrtab) {
        g_capptrtab = g_capptrtab ? g_capptrtab * 2 : 64;
        g_ptrtab = (aval *)xrealloc(g_ptrtab, g_capptrtab * sizeof(aval));
    }
    g_ptrtab[g_nptrtab] = argv[0];
    return mkint_full((int64_t)(g_nptrtab++ + 1));
}

aval bi_unsafe_load(uint64_t n, aval *argv) {
    want_args("unsafe.load", n, 1, 1);
    return g_ptrtab[ptr_id(argv[0])];
}

aval bi_unsafe_store(uint64_t n, aval *argv) {
    want_args("unsafe.store", n, 2, 2);
    g_ptrtab[ptr_id(argv[0])] = argv[1];
    return A_NONE;
}

aval bi_unsafe_offset(uint64_t n, aval *argv) {
    want_args("unsafe.offset", n, 2, 2);
    uint64_t id = ptr_id(argv[0]);
    int64_t off = int_val_full(argv[1]);
    uint64_t nid = id + (uint64_t)off;
    if (nid >= g_nptrtab) panicf("unsafe.offset: address out of bounds");
    return mkint_full((int64_t)(nid + 1));
}

