#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* object constructors                                                */
/* ------------------------------------------------------------------ */

aval a_str_concat(aval l, aval r) {
    uint64_t ln, rn;
    const char *lp = str_data(l, &ln), *rp = str_data(r, &rn);
    if ((uint64_t)UINT32_MAX - ln < rn) panicf("string too large");
    /* Small results come from the bump arena (AStr objects are never freed
       individually), so a concat loop allocates without touching malloc. */
    aval outv = a_str_new_uninit(ln + rn);
    AStr *s = (AStr *)obj_of(outv);
    if (ln) memcpy(s->data, lp, (size_t)ln);
    if (rn) memcpy(s->data + ln, rp, (size_t)rn);
    return outv;
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
    /* Exact-size single allocation (same hot-path rule as concat_str):
       sum the item lengths first, then fill in one pass. The Buf path
       realloc-copied the growing buffer O(log n) times for join-heavy
       code (joining 100k parts copied ~2x the output). */
    uint64_t total = n ? (n - 1) * sn : 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t in;
        (void)str_data(items[i], &in);
        if ((uint64_t)UINT32_MAX - total < in) panicf("string too large");
        total += in;
    }
    aval outv = a_str_new_uninit(total);
    AStr *o = (AStr *)obj_of(outv);
    char *d = o->data;
    for (uint64_t i = 0; i < n; ++i) {
        if (i) {
            if (sn) memcpy(d, sp, (size_t)sn);
            d += sn;
        }
        uint64_t in;
        const char *ip = str_data(items[i], &in);
        if (in) memcpy(d, ip, (size_t)in);
        d += in;
    }
    return outv;
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
    arr_iitems_raw(a);
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
    arr_iitems_raw(a);
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
    d->slots = NULL;
    d->scap = 0;
    return mkobj(&d->h);
}

aval a_struct_new(aval name, aval methods, aval methodFns) {
    AStruct *s = (AStruct *)xmalloc(sizeof(AStruct));
    s->d.h.type = T_STRUCT;
    s->d.h.len = 0;
    s->d.h.cap = 0;
    s->d.pairs = NULL;
    s->d.slots = NULL;
    s->d.scap = 0;
    s->name = name;
    s->methods = methods;
    s->methodFns = methodFns;
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

/* ------------------------------------------------------------------ */
/* deep copy (copy() builtin)                                          */
/* ------------------------------------------------------------------ */

/* Cycle/sharing-preserving memo for deepcopy. Containers are allocated
   and registered in the map BEFORE their children are copied, so a
   structure that refers back to itself (a list containing itself, a dict
   whose value is the dict, ...) copies finitely: the back-reference
   resolves to the already-created copy. DAG-shaped sharing is likewise
   preserved — the same source object maps to the same copy everywhere it
   appears. Objects are never freed, so source pointer identity is stable
   for the whole process and a plain pointer-keyed table suffices. */
typedef struct { const void *old; aval nw; } DCEnt;
typedef struct { DCEnt *t; size_t cap, len; } DCMap;

static uint64_t dcent_hash(const void *p) {
    uintptr_t x = (uintptr_t)p >> 4;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return (uint64_t)x;
}

static void dcmap_grow(DCMap *m) {
    size_t ncap = m->cap ? m->cap * 2 : 64;
    DCEnt *nt = (DCEnt *)xmalloc(ncap * sizeof(DCEnt));
    memset(nt, 0, ncap * sizeof(DCEnt));
    for (size_t i = 0; i < m->cap; ++i) {
        if (!m->t[i].old) continue;
        size_t s = (size_t)(dcent_hash(m->t[i].old) & (ncap - 1));
        while (nt[s].old) s = (s + 1) & (ncap - 1);
        nt[s] = m->t[i];
    }
    free(m->t);
    m->t = nt;
    m->cap = ncap;
}

static void dcmap_put(DCMap *m, const void *old, aval nw) {
    if (!m->cap || (m->len + 1) * 10 >= m->cap * 7) dcmap_grow(m);
    size_t s = (size_t)(dcent_hash(old) & (m->cap - 1));
    while (m->t[s].old) s = (s + 1) & (m->cap - 1);
    m->t[s].old = old;
    m->t[s].nw = nw;
    m->len++;
}

static int dcmap_get(DCMap *m, const void *old, aval *out) {
    if (!m->cap) return 0;
    size_t s = (size_t)(dcent_hash(old) & (m->cap - 1));
    while (m->t[s].old) {
        if (m->t[s].old == old) { *out = m->t[s].nw; return 1; }
        s = (s + 1) & (m->cap - 1);
    }
    return 0;
}

static aval deepcopy(aval v, int depth, DCMap *m) {
    /* Depth cap stays as a guard against pathological acyclic nesting
       overflowing the C stack; true cycles no longer recurse because the
       memo resolves back-references before descending. */
    if (depth > 500)
        panicf("copy() value is too deeply nested");
    AObj *o = obj_of(v);
    if (!o) return v;
    aval prev;
    if (dcmap_get(m, o, &prev)) return prev;
    switch (o->type) {
        case T_LIST:
        case T_TUPLE: {
            AArr *a = (AArr *)o;
            uint64_t n = a->h.len;
            AArr *na = (AArr *)xmalloc(sizeof(AArr));
            na->h.type = o->type == T_TUPLE ? T_TUPLE : T_LIST;
            na->h.len = (uint32_t)n;
            na->h.cap = n;
            na->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
            na->iitems = NULL;
            na->flags = 0;
            aval nv = mkobj(&na->h);
            dcmap_put(m, o, nv);
            /* arr_elem reads packed int64 storage too, so the copy is
               always aval-mode regardless of the source's packing. */
            for (uint64_t i = 0; i < n; ++i)
                na->items[i] = deepcopy(arr_elem(a, i), depth + 1, m);
            return nv;
        }
        case T_SET: {
            AArr *a = (AArr *)o;
            aval r = a_set_new();
            dcmap_put(m, o, r);
            for (uint64_t i = 0; i < a->h.len; ++i)
                a_set_add(r, deepcopy(arr_elem(a, i), depth + 1, m));
            return r;
        }
        case T_DICT:
        case T_STRUCT: {
            ADict *d = (ADict *)o;
            aval r = o->type == T_STRUCT
                         ? a_struct_new(((AStruct *)o)->name,
                                        ((AStruct *)o)->methods,
                                        ((AStruct *)o)->methodFns)
                         : a_dict_new();
            dcmap_put(m, o, r);
            ADict *rd = dict_of(r, "dict");
            for (uint64_t i = 0; i < d->h.len; ++i) {
                aval k = deepcopy(d->pairs[i].k, depth + 1, m);
                aval val = deepcopy(d->pairs[i].v, depth + 1, m);
                dict_put(rd, k, val);
            }
            return r;
        }
        case T_BYTES: {
            ABytes *b = (ABytes *)o;
            return a_bytes_from(b->data, b->h.len);
        }
        case T_RESULT: {
            AResult *res = (AResult *)o;
            AResult *nr = result_new(res->ok, res->is_option, A_NONE);
            aval r = mkobj(&nr->h);
            dcmap_put(m, o, r);
            nr->value = deepcopy(res->value, depth + 1, m);
            return r;
        }
        case T_VIEW: {
            AView *vw = (AView *)o;
            AView *nv = (AView *)xmalloc(sizeof(AView));
            nv->h.type = T_VIEW;
            nv->h.len = vw->h.len;
            nv->h.cap = 0;
            nv->parent = A_NONE;
            nv->off = vw->off;
            nv->len = vw->len;
            nv->is_tuple = vw->is_tuple;
            aval r = mkobj(&nv->h);
            dcmap_put(m, o, r);
            nv->parent = deepcopy(vw->parent, depth + 1, m);
            /* The copied view aliases the copied parent's buffer: pin its
               capacity against arr_shrink exactly like view_new does. */
            arr_note_view(nv->parent);
            return r;
        }
        default:
            /* immediates, strings, ranges, functions, environments,
               iterators, python objects: shared, not copied. */
            return v;
    }
}

aval a_deepcopy(aval v) {
    DCMap m = { NULL, 0, 0 };
    aval r = deepcopy(v, 0, &m);
    free(m.t);
    return r;
}

