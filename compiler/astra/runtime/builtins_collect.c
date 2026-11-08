#include "a_runtime_internal.h"

static AArr *iterable_arr(aval v, aval *tmp) {
    AObj *o = obj_of(v);
    if (o && (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET))
        return (AArr *)o;
    *tmp = collect_iterable(v);
    return (AArr *)obj_of(*tmp);
}

aval bi_sum(uint64_t n, aval *argv) {
    want_args("sum", n, 1, 2);
    aval acc = n == 2 ? argv[1] : mkint_full(0);
    if (!is_num(acc)) panicf("sum() start must be a number");
    aval tmp;
    AArr *a = iterable_arr(argv[0], &tmp);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval item = arr_elem(a, i);
        if (!is_num(item))
            panicf("unsupported operand type(s) for +: '%s' and '%s'",
                   type_name_cstr(acc), type_name_cstr(item));
        acc = binary_arith(acc, item, '+', "+");
    }
    return acc;
}

aval bi_range(uint64_t n, aval *argv) {
    want_args("range", n, 1, 3);
    int64_t lo = 0, hi, step = 1;
    if (n == 1) hi = expect_int64(argv[0], "range() argument");
    else {
        lo = expect_int64(argv[0], "range() argument");
        hi = expect_int64(argv[1], "range() argument");
        if (n == 3) step = expect_int64(argv[2], "range() step");
    }
    return a_range_new(mkint_full(lo), mkint_full(hi), mkint_full(step));
}

aval bi_set(uint64_t n, aval *argv) {
    want_args("set", n, 0, 1);
    aval s = a_set_new();
    if (n == 1) {
        aval tmp = collect_iterable(argv[0]);
        AArr *a = (AArr *)obj_of(tmp);
        for (uint64_t i = 0; i < a->h.len; ++i) a_set_add(s, arr_elem(a, i));
    }
    return s;
}

aval bi_tuple(uint64_t n, aval *argv) {
    want_args("tuple", n, 0, 1);
    if (n == 0) return a_tuple_new(0, NULL);
    aval tmp;
    AArr *a = iterable_arr(argv[0], &tmp);
    if (arr_packed(a)) {
        /* packed int storage has no aval items array; copy elementwise */
        AVec v = { NULL, 0, 0 };
        for (uint64_t i = 0; i < a->h.len; ++i)
            avec_push(&v, mkint_full(a->iitems[i]));
        aval r = a_tuple_new(v.len, v.p);
        free(v.p);
        return r;
    }
    return a_tuple_new(a->h.len, a->items);
}

aval bi_list(uint64_t n, aval *argv) {
    want_args("list", n, 0, 1);
    if (n == 0) return a_list_new(0, NULL);
    return collect_iterable(argv[0]);
}

aval bi_dict(uint64_t n, aval *argv) {
    want_args("dict", n, 0, 1);
    aval d = a_dict_new();
    if (n == 1) {
        if (kind_of(argv[0]) == K_DICT) {
            ADict *o = (ADict *)obj_of(argv[0]);
            for (uint64_t i = 0; i < o->h.len; ++i) dict_put((ADict *)obj_of(d), o->pairs[i].k, o->pairs[i].v);
        } else {
            aval tmp = collect_iterable(argv[0]);
            AArr *a = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < a->h.len; ++i) {
                aval p = arr_elem(a, i);
                if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                    panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                AArr *pa = (AArr *)obj_of(p);
                if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                dict_put((ADict *)obj_of(d), arr_elem(pa, 0), arr_elem(pa, 1));
            }
        }
    }
    return d;
}

aval bi_map(uint64_t n, aval *argv) {
    want_args("map", n, 2, 2);
    /* A calls map(xs, f); accept either order. */
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp;
    AArr *a = iterable_arr(argv[1], &tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval one[1] = { arr_elem(a, i) };
        avec_push(&out, a_call(argv[0], 1, one));
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_filter(uint64_t n, aval *argv) {
    want_args("filter", n, 2, 2);
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp;
    AArr *a = iterable_arr(argv[1], &tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval item = arr_elem(a, i);
        aval one[1] = { item };
        if (truthy(a_call(argv[0], 1, one))) avec_push(&out, item);
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_any(uint64_t n, aval *argv) { want_args("any", n, 1, 2); aval tmp; AArr *a=iterable_arr(argv[0],&tmp); for(uint64_t i=0;i<a->h.len;++i){ aval v=arr_elem(a,i); if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(truthy(v)) return A_TRUE; } return A_FALSE; }
aval bi_all(uint64_t n, aval *argv) { want_args("all", n, 1, 2); aval tmp; AArr *a=iterable_arr(argv[0],&tmp); for(uint64_t i=0;i<a->h.len;++i){ aval v=arr_elem(a,i); if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(!truthy(v)) return A_FALSE; } return A_TRUE; }
aval bi_some(uint64_t n, aval *argv) { want_args("Some", n, 1, 1); return a_some_new(argv[0]); }

aval bi_reduce(uint64_t n, aval *argv) {
    want_args("reduce", n, 3, 3);
    aval tmp;
    AArr *a = iterable_arr(argv[0], &tmp);
    aval acc = argv[1];
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval two[2] = { acc, arr_elem(a, i) };
        acc = a_call(argv[2], 2, two);
    }
    return acc;
}

aval bi_ok(uint64_t n, aval *argv) { want_args("ok", n, 1, 1); return mkobj(result_new(1, 0, argv[0])); }
aval bi_err(uint64_t n, aval *argv) { want_args("err", n, 1, 1); return mkobj(result_new(0, 0, argv[0])); }
aval bi_sorted(uint64_t n, aval *argv) {
    want_args("sorted", n, 1, 1);
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    if (!a) return t;
    if (arr_packed(a)) sort_ints(a->iitems, a->h.len);
    else sort_arr(a->items, a->h.len);
    return t;
}

aval bi_reversed(uint64_t n, aval *argv) {
    want_args("reversed", n, 1, 1);
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = a->h.len; i > 0; --i) avec_push(&out, a->items[i - 1]);
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_enumerate(uint64_t n, aval *argv) {
    want_args("enumerate", n, 1, 2);
    int64_t start = n == 2 ? expect_int64(argv[1], "enumerate() start") : 0;
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval pair[2] = { mkint_full(start + (int64_t)i), a->items[i] };
        avec_push(&out, a_tuple_new(2, pair));
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_zip(uint64_t n, aval *argv) {
    if (n < 1) panicf("zip expected at least 1 argument, got 0");
    aval *cols = (aval *)xmalloc((size_t)n * sizeof(aval));
    uint64_t minlen = UINT64_MAX;
    for (uint64_t j = 0; j < n; ++j) {
        cols[j] = collect_iterable(argv[j]);
        uint64_t l = ((AArr *)obj_of(cols[j]))->h.len;
        if (l < minlen) minlen = l;
    }
    AVec out = { NULL, 0, 0 };
    aval *row = (aval *)xmalloc((size_t)n * sizeof(aval));
    for (uint64_t i = 0; i < minlen; ++i) {
        for (uint64_t j = 0; j < n; ++j)
            row[j] = ((AArr *)obj_of(cols[j]))->items[i];
        avec_push(&out, a_tuple_new(n, row));
    }
    free(row);
    free(cols);
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

