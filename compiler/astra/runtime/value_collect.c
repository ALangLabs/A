#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* object constructors                                                */
/* ------------------------------------------------------------------ */

aval a_str_concat(aval l, aval r) {
    uint64_t ln, rn;
    const char *lp = str_data(l, &ln), *rp = str_data(r, &rn);
    if ((uint64_t)UINT32_MAX - ln < rn) panicf("string too large");
    AStr *s = (AStr *)xmalloc(sizeof(AStr) + (size_t)ln + rn + 1);
    s->h.type = T_STR;
    s->h.len = (uint32_t)(ln + rn);
    s->h.cap = s->h.len + 1;
    memcpy(s->data, lp, (size_t)ln);
    memcpy(s->data + ln, rp, (size_t)rn);
    s->data[ln + rn] = '\0';
    return mkobj(&s->h);
}



void avec_push(AVec *v, aval x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->p = (aval *)xrealloc(v->p, v->cap * sizeof(aval));
    }
    v->p[v->len++] = x;
}

aval a_str_join(uint64_t n, aval *items) {
    return a_str_joinsep(a_str_new("", 0), n, items);
}

aval a_str_joinsep(aval sep, uint64_t n, aval *items) {
    uint64_t sn;
    const char *sp = str_data(sep, &sn);
    Buf b;
    buf_init(&b);
    for (uint64_t i = 0; i < n; ++i) {
        if (i) buf_addn(&b, sp, (size_t)sn);
        uint64_t in;
        const char *ip = str_data(items[i], &in);
        buf_addn(&b, ip, (size_t)in);
    }
    return buf_take(&b);
}

aval list_from_vec(const aval *p, uint64_t n, int tuple) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = tuple ? T_TUPLE : T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
    a->iitems = NULL;
    a->flags = 0;
    if (n) memcpy(a->items, p, (size_t)n * sizeof(aval));
    return mkobj(&a->h);
}

/* Packed int list straight from int64 storage: no aval round-trip, so a
   1 MiB byte array costs one malloc + one memcpy instead of a million tagged
   values. Used by the socket byte readers. */
aval a_list_from_i64(const int64_t *v, uint64_t n) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = NULL;
    a->iitems = (int64_t *)xmalloc((size_t)(n ? n : 1) * sizeof(int64_t));
    if (n) memcpy(a->iitems, v, (size_t)n * sizeof(int64_t));
    a->flags = ARR_F_PACKED;
    return mkobj(&a->h);
}

/* Same as a_list_from_i64 but takes ownership of `v` (no copy). The caller
   must have allocated it with xmalloc and never use it again. */
aval a_list_take_i64(int64_t *v, uint64_t n) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = NULL;
    a->iitems = v;
    a->flags = ARR_F_PACKED;
    return mkobj(&a->h);
}

aval a_list_new(uint64_t n, aval *items) {
    return list_from_vec(items, n, 0);
}

aval a_tuple_new(uint64_t n, aval *items) {
    return list_from_vec(items, n, 1);
}

aval a_list_from_iter(aval it) {
    AObj *o = obj_of(it);
    AIter *iter;
    int own = 0;
    if (o && o->type == T_ITER) {
        iter = (AIter *)o;
    } else {
        it = a_iter_new(it);
        iter = (AIter *)obj_of(it);
        own = 1;
    }
    (void)own;
    AVec v = { NULL, 0, 0 };
    aval out;
    while (a_iter_next(mkobj(iter), &out)) avec_push(&v, out);
    aval r = list_from_vec(v.p, v.len, 0);
    free(v.p);
    return r;
}

aval a_dict_new(void) {
    ADict *d = (ADict *)xmalloc(sizeof(ADict));
    d->h.type = T_DICT;
    d->h.len = 0;
    d->h.cap = 0;
    d->pairs = NULL;
    d->phash = NULL;
    d->slots = NULL;
    d->scap = 0;
    return mkobj(&d->h);
}

aval a_struct_new(aval name, aval methods) {
    AStruct *s = (AStruct *)xmalloc(sizeof(AStruct));
    s->d.h.type = T_STRUCT;
    s->d.h.len = 0;
    s->d.h.cap = 0;
    s->d.pairs = NULL;
    s->d.phash = NULL;
    s->d.slots = NULL;
    s->d.scap = 0;
    s->name = name;
    s->methods = methods;
    return mkobj(&s->d.h);
}

aval a_dict_set(aval d, aval k, aval v) {
    dict_put(dict_of(d, "dict"), k, v);
    return A_NONE;
}

aval a_dict_get(aval d, aval k, aval dflt) {
    ADict *m = dict_of(d, "dict");
    int64_t i = dict_find(m, k);
    return i >= 0 ? m->pairs[i].v : dflt;
}

aval a_dict_has(aval d, aval k) {
    ADict *m = dict_of(d, "dict");
    return a_bool(dict_find(m, k) >= 0);
}

aval a_set_new(void) {
    AArr *s = (AArr *)xmalloc(sizeof(AArr));
    s->h.type = T_SET;
    s->h.len = 0;
    s->h.cap = 0;
    s->items = NULL;
    s->iitems = NULL;
    s->flags = 0;
    return mkobj(&s->h);
}

aval a_set_add(aval s, aval v) {
    AArr *set = (AArr *)obj_of(s);
    if (!set || set->h.type != T_SET)
        panicf("set_add() argument must be set");
    if (!set_contains(set, v)) arr_push(set, v);
    return A_NONE;
}

aval a_range_new(aval lo, aval hi, aval step) {
    int64_t a = expect_int64(lo, "range() argument"),
            b = expect_int64(hi, "range() argument"),
            c = expect_int64(step, "range() step");
    if (c == 0) panicf("range() step must be positive, found 0.");
    ARange *r = (ARange *)xmalloc(sizeof(ARange));
    r->h.type = T_RANGE;
    r->h.len = 3;
    r->h.cap = 0;
    r->lo = a;
    r->hi = b;
    r->step = c;
    return mkobj(&r->h);
}

