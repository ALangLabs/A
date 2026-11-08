#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* implicit parallelism helpers                                        */
/* ------------------------------------------------------------------ */

/* Parallel collection paths only fire for large inputs and only when the
   per-element work is provably free of shared mutable state:
   - sum(): packed int64 storage, integer addition is associative, and we
     prove no overflow (which would promote to double and reorder results).
   - map()/filter(): the callable must be a *builtin* fnon from a whitelist
     of pure unary numeric kernels. Arbitrary A closures are rejected:
     generated code mutates frame stacks/closure envs that are not
     thread-safe, so calling them from pool workers would race. */

static uint64_t iabs_u64(int64_t v) {
    return v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
}

/* chunk partial sums: packed int64 storage */
typedef struct {
    const int64_t *src;
    int64_t *parts;
    int64_t grain;
} ASumParCtx;

static void sum_par_chunk(void *vctx, int64_t lo, int64_t hi) {
    ASumParCtx *c = (ASumParCtx *)vctx;
    int64_t s = 0;
    /* Caller proved the whole-list sum cannot overflow int64, so every
       partial sum is exact and chunk order does not matter. */
    for (int64_t i = lo; i < hi; ++i) s += c->src[i];
    c->parts[lo / c->grain] = s;
}

/* chunk partial sums: aval element storage (all elements verified int) */
typedef struct {
    const aval *src;
    int64_t *parts;
    int64_t grain;
} ASumParAvCtx;

static void sum_par_chunk_av(void *vctx, int64_t lo, int64_t hi) {
    ASumParAvCtx *c = (ASumParAvCtx *)vctx;
    int64_t s = 0;
    for (int64_t i = lo; i < hi; ++i) s += int_val_full(c->src[i]);
    c->parts[lo / c->grain] = s;
}

static int par_pure_builtin(aval fn, a_builtin_fn *out) {
    AObj *o = obj_of(fn);
    if (!o || o->type != T_FNON) return 0;
    AFnon *f = (AFnon *)o;
    if (!(f->flags & FNON_BUILTIN)) return 0;
    a_builtin_fn bf = (a_builtin_fn)(uintptr_t)f->entry;
    /* Whitelist: pure unary numeric builtins — no globals, no allocation
       beyond the (mutex-guarded) int box and NaN-boxed doubles. */
    static const a_builtin_fn ok[] = {
        bi_abs, bi_sqrt, bi_sin, bi_cos, bi_tan, bi_exp, bi_log,
        bi_floor, bi_ceil, bi_trunc, bi_round, bi_fabs,
        bi_i8, bi_i16, bi_i32, bi_u8, bi_u16, bi_u32, bi_f32,
        bi_ctz, bi_clz, bi_popcount, bi_bswap
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); ++i)
        if (bf == ok[i]) { *out = bf; return 1; }
    return 0;
}

typedef struct {
    const AArr *a;
    a_builtin_fn bf;
    aval *res;      /* map: per-index results */
    unsigned char *keep; /* filter: per-index truthiness */
} AMapParCtx;

static void map_par_chunk(void *vctx, int64_t lo, int64_t hi) {
    AMapParCtx *c = (AMapParCtx *)vctx;
    for (int64_t i = lo; i < hi; ++i) {
        aval one[1] = { arr_elem(c->a, (uint64_t)i) };
        aval r = c->bf(1, one);
        if (c->res) c->res[i] = r;
        else c->keep[i] = truthy(r) ? 1 : 0;
    }
}

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
    /* Parallel fast path: integer lists (packed int64 or aval storage with
       every element an int) with an integer start, large enough to
       amortize dispatch, and provably no int64 overflow (serial semantics
       promote to double on overflow, which would make chunked partial
       sums observably different). The validation scan is a single cheap
       pass — far cheaper than the serial per-element binary_arith. */
    if (is_int_val(acc) && a->h.len >= A_PAR_MIN_ELEMS &&
        a_par_workers() > 1) {
        int64_t len = (int64_t)a->h.len;
        uint64_t mx = 0;
        int allint = 1;
        if (arr_packed(a)) {
            for (int64_t i = 0; i < len; ++i) {
                uint64_t m = iabs_u64(a->iitems[i]);
                if (m > mx) mx = m;
            }
        } else if (a->items) {
            for (int64_t i = 0; i < len; ++i) {
                aval v = a->items[i];
                if (!is_int_val(v)) { allint = 0; break; }
                uint64_t m = iabs_u64(int_val_full(v));
                if (m > mx) mx = m;
            }
        } else allint = 0;
        int64_t start = int_val_full(acc);
        uint64_t smag = iabs_u64(start);
        /* |total| <= mx*len + smag must stay <= INT64_MAX so no partial or
           running sum can overflow (and thus promote to double). */
        if (allint && (mx == 0 || (mx <= (uint64_t)INT64_MAX / (uint64_t)len &&
                        smag <= (uint64_t)INT64_MAX - mx * (uint64_t)len))) {
            int w = a_par_workers();
            int64_t target = (int64_t)w * 8;
            if (target < 1) target = 1;
            int64_t grain = (len + target - 1) / target;
            if (grain < 1) grain = 1;
            int64_t nchunks = (len + grain - 1) / grain;
            /* nchunks <= workers*8 < the pool's chunk cap, so a_par_for
               keeps this exact grain and lo/grain is a stable index. */
            int64_t *parts = (int64_t *)xmalloc((size_t)nchunks * sizeof(int64_t));
            if (arr_packed(a)) {
                ASumParCtx c = { a->iitems, parts, grain };
                a_par_for(len, grain, sum_par_chunk, &c);
            } else {
                ASumParAvCtx c = { a->items, parts, grain };
                a_par_for(len, grain, sum_par_chunk_av, &c);
            }
            int64_t total = start;
            for (int64_t i = 0; i < nchunks; ++i) total += parts[i];
            free(parts);
            return mkint_full(total);
        }
    }
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
    a_builtin_fn bf;
    if (a->h.len >= A_PAR_MIN_ELEMS && par_pure_builtin(argv[0], &bf) &&
        a_par_workers() > 1) {
        /* Parallel map over a whitelisted pure builtin: each worker writes
           disjoint result slots, so output order is identical to serial. */
        aval *res = (aval *)xmalloc((size_t)a->h.len * sizeof(aval));
        AMapParCtx c = { a, bf, res, NULL };
        int64_t len = (int64_t)a->h.len;
        int64_t grain = (len + 4095) / 4096; /* ~4096 elems/chunk */
        a_par_for(len, grain, map_par_chunk, &c);
        aval r = list_from_vec(res, a->h.len, 0);
        free(res);
        return r;
    }
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
    a_builtin_fn bf;
    if (a->h.len >= A_PAR_MIN_ELEMS && par_pure_builtin(argv[0], &bf) &&
        a_par_workers() > 1) {
        /* Parallel predicate evaluation into a bitmap, then a serial
           order-preserving compaction — identical output to serial. */
        unsigned char *keep = (unsigned char *)xmalloc((size_t)a->h.len);
        memset(keep, 0, (size_t)a->h.len);
        AMapParCtx c = { a, bf, NULL, keep };
        int64_t len = (int64_t)a->h.len;
        int64_t grain = (len + 4095) / 4096;
        a_par_for(len, grain, map_par_chunk, &c);
        AVec out = { NULL, 0, 0 };
        for (uint64_t i = 0; i < a->h.len; ++i)
            if (keep[i]) avec_push(&out, arr_elem(a, i));
        free(keep);
        aval r = list_from_vec(out.p, out.len, 0);
        free(out.p);
        return r;
    }
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

