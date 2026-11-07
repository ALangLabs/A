#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

double math_arg(aval v, const char *name) {
    if (!is_num(v)) panicf("%s() argument must be a number, not '%s'", name, type_name_cstr(v));
    return num_to_double(v);
}

aval bi_print(uint64_t n, aval *argv) { (void)n; (void)argv; return a_printv(n, argv); }
aval bi_eprint(uint64_t n, aval *argv) { (void)n; (void)argv; return a_eprintv(n, argv); }
aval bi_len(uint64_t n, aval *argv) { want_args("len", n, 1, 1); return a_len(argv[0]); }
aval bi_str(uint64_t n, aval *argv) { want_args("str", n, 1, 1); return a_str(argv[0]); }
aval bi_int(uint64_t n, aval *argv) { want_args("int", n, 1, 1); return a_int(argv[0]); }
aval bi_float(uint64_t n, aval *argv) { want_args("float", n, 1, 1); return a_float(argv[0]); }
aval bi_bool(uint64_t n, aval *argv) { want_args("bool", n, 1, 1); return a_bool(argv[0]); }
aval bi_repr(uint64_t n, aval *argv) { want_args("repr", n, 1, 1); return a_repr(argv[0]); }
aval bi_type(uint64_t n, aval *argv) { want_args("type", n, 1, 1); return a_type_name(argv[0]); }

aval bi_abs(uint64_t n, aval *argv) {
    want_args("abs", n, 1, 1);
    aval v = argv[0];
    if (v == A_TRUE) return mkint_full(1);
    if (v == A_FALSE) return mkint_full(0);
    if (is_int_val(v)) {
        int64_t i = int_val_full(v);
        if (i == INT64_MIN) return mkdouble(9223372036854775808.0);
        return mkint_full(i < 0 ? -i : i);
    }
    if (a_is_double(v)) return mkdouble(fabs(a_double_val(v)));
    panicf("bad operand type for abs(): '%s'", type_name_cstr(v));
    return A_NONE;
}

static aval minmax(uint64_t n, aval *argv, int is_min, const char *name) {
    if (n == 0) panicf("%s expected at least 1 argument, got 0", name);
    aval best;
    uint64_t start = 0;
    if (n == 1) {
        aval tmp = collect_iterable(argv[0]);
        AArr *a = (AArr *)obj_of(tmp);
        if (a->h.len == 0) panicf("%s() arg is an empty sequence", name);
        best = a->items[0];
        start = 1;
        for (uint64_t i = start; i < a->h.len; ++i) {
            aval c = a_lt(a->items[i], best);
            if (truthy(is_min ? c : a_lt(best, a->items[i]))) best = a->items[i];
        }
        return best;
    }
    best = argv[0];
    for (uint64_t i = 1; i < n; ++i) {
        aval c = a_lt(argv[i], best);
        if (truthy(is_min ? c : a_lt(best, argv[i]))) best = argv[i];
    }
    return best;
}

aval bi_min(uint64_t n, aval *argv) { return minmax(n, argv, 1, "min"); }
aval bi_max(uint64_t n, aval *argv) { return minmax(n, argv, 0, "max"); }

aval bi_sum(uint64_t n, aval *argv) {
    want_args("sum", n, 1, 2);
    aval acc = n == 2 ? argv[1] : mkint_full(0);
    if (!is_num(acc)) panicf("sum() start must be a number");
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        if (!is_num(a->items[i]))
            panicf("unsupported operand type(s) for +: '%s' and '%s'",
                   type_name_cstr(acc), type_name_cstr(a->items[i]));
        acc = binary_arith(acc, a->items[i], '+', "+");
    }
    return acc;
}

aval bi_pow(uint64_t n, aval *argv) {
    /* pow() is -> f64 per docs; the ** operator stays int for int base. */
    want_args("pow", n, 2, 2);
    aval r = a_pow(argv[0], argv[1]);
    if (is_int_val(r)) return mkdouble((double)int_val_full(r));
    return r;
}

#define BI_MATH1(fname, expr) \
    aval fname(uint64_t n, aval *argv) { \
        want_args(#fname, n, 1, 1); \
        double x = math_arg(argv[0], #fname); \
        return mkdouble(expr); \
    }

BI_MATH1(bi_sin, sin(x))
BI_MATH1(bi_cos, cos(x))
BI_MATH1(bi_tan, tan(x))
BI_MATH1(bi_exp, exp(x))

aval bi_sqrt(uint64_t n, aval *argv) {
    want_args("sqrt", n, 1, 1);
    double x = math_arg(argv[0], "sqrt");
    if (x < 0.0) panicf("math domain error");
    return mkdouble(sqrt(x));
}
aval bi_log(uint64_t n, aval *argv) {
    want_args("log", n, 1, 1);
    double x = math_arg(argv[0], "log");
    if (x <= 0.0) panicf("math domain error");
    return mkdouble(log(x));
}
aval bi_asin(uint64_t n, aval *argv) {
    want_args("asin", n, 1, 1);
    double x = math_arg(argv[0], "asin");
    if (x < -1.0 || x > 1.0) panicf("math domain error");
    return mkdouble(asin(x));
}
aval bi_acos(uint64_t n, aval *argv) {
    want_args("acos", n, 1, 1);
    double x = math_arg(argv[0], "acos");
    if (x < -1.0 || x > 1.0) panicf("math domain error");
    return mkdouble(acos(x));
}
aval bi_atan(uint64_t n, aval *argv) {
    want_args("atan", n, 1, 1);
    return mkdouble(atan(math_arg(argv[0], "atan")));
}

aval bi_floor(uint64_t n, aval *argv) { want_args("floor", n, 1, 1); return mkdouble(floor(math_arg(argv[0], "floor"))); }
aval bi_ceil(uint64_t n, aval *argv) { want_args("ceil", n, 1, 1); return mkdouble(ceil(math_arg(argv[0], "ceil"))); }
aval bi_trunc(uint64_t n, aval *argv) { want_args("trunc", n, 1, 1); return mkdouble(trunc(math_arg(argv[0], "trunc"))); }
aval bi_round(uint64_t n, aval *argv) {
    want_args("round", n, 1, 1);
    double x = math_arg(argv[0], "round");
    /* Round half away from zero (Python round() is banker's, spec wants away). */
    double r = x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
    return mkdouble(r);
}

aval bi_random(uint64_t n, aval *argv) {
    want_args("random", n, 0, 0);
    (void)argv;
    return mkdouble((double)rand() / ((double)RAND_MAX + 1.0));
}

aval bi_random_int(uint64_t n, aval *argv) {
    want_args("random_int", n, 2, 2);
    int64_t lo = expect_int64(argv[0], "random_int() argument");
    int64_t hi = expect_int64(argv[1], "random_int() argument");
    if (hi < lo) panicf("random_int() range is empty");
    if (hi == lo) return mkint_full(lo);
    long double span = (long double)hi - (long double)lo + 1.0L;
    long double u = (long double)rand() / ((long double)RAND_MAX + 1.0L);
    long double r = (long double)lo + floorl(u * span);
    if (r < (long double)lo) r = (long double)lo;
    if (r > (long double)hi) r = (long double)hi;
    return mkint_full((int64_t)r);
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
        for (uint64_t i = 0; i < a->h.len; ++i) a_set_add(s, a->items[i]);
    }
    return s;
}

aval bi_tuple(uint64_t n, aval *argv) {
    want_args("tuple", n, 0, 1);
    if (n == 0) return a_tuple_new(0, NULL);
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
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
                aval p = a->items[i];
                if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                    panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                AArr *pa = (AArr *)obj_of(p);
                if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                dict_put((ADict *)obj_of(d), pa->items[0], pa->items[1]);
            }
        }
    }
    return d;
}

aval bi_map(uint64_t n, aval *argv) {
    want_args("map", n, 2, 2);
    /* A calls map(xs, f); accept either order. */
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp = collect_iterable(argv[1]);
    AArr *a = (AArr *)obj_of(tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval one[1] = { a->items[i] };
        avec_push(&out, a_call(argv[0], 1, one));
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_filter(uint64_t n, aval *argv) {
    want_args("filter", n, 2, 2);
    if (kind_of(argv[1]) == K_FNON) { aval tmp2 = argv[0]; argv[0] = argv[1]; argv[1] = tmp2; }
    aval tmp = collect_iterable(argv[1]);
    AArr *a = (AArr *)obj_of(tmp);
    AVec out = { NULL, 0, 0 };
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval one[1] = { a->items[i] };
        if (truthy(a_call(argv[0], 1, one))) avec_push(&out, a->items[i]);
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
}

aval bi_any(uint64_t n, aval *argv) { want_args("any", n, 1, 2); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i){ aval v=a->items[i]; if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(truthy(v)) return A_TRUE; } return A_FALSE; }
aval bi_all(uint64_t n, aval *argv) { want_args("all", n, 1, 2); aval t = collect_iterable(argv[0]); AArr *a=(AArr*)obj_of(t); for(uint64_t i=0;i<a->h.len;++i){ aval v=a->items[i]; if(n==2){ aval one[1]={v}; v=a_call(argv[1],1,one); } if(!truthy(v)) return A_FALSE; } return A_TRUE; }
aval bi_some(uint64_t n, aval *argv) { want_args("Some", n, 1, 1); return a_some_new(argv[0]); }

aval bi_reduce(uint64_t n, aval *argv) {
    want_args("reduce", n, 3, 3);
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    aval acc = argv[1];
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval two[2] = { acc, a->items[i] };
        acc = a_call(argv[2], 2, two);
    }
    return acc;
}

aval bi_ok(uint64_t n, aval *argv) { want_args("ok", n, 1, 1); return mkobj(result_new(1, 0, argv[0])); }
aval bi_err(uint64_t n, aval *argv) { want_args("err", n, 1, 1); return mkobj(result_new(0, 0, argv[0])); }

static void panic_with_msg_at(uint64_t n, aval *argv, int idx, const char *def) {
    (void)n;
    if (idx < 0) { a_panic_msg(def); return; }
    aval s = a_str(argv[idx]);
    uint64_t len;
    const char *p = str_data(s, &len);
    (void)len;
    a_panic_msg(p);
}

aval bi_panic(uint64_t n, aval *argv) {
    want_args("panic", n, 0, 1);
    if (n == 0) a_panic_msg("panic");
    else panic_with_msg_at(n, argv, 0, "panic");
    return A_NONE;
}

aval bi_assert(uint64_t n, aval *argv) {
    want_args("assert", n, 1, 2);
    if (!truthy(argv[0])) panic_with_msg_at(n, argv, n == 2 ? 1 : -1, "Assertion failed");
    return A_NONE;
}

aval bi_assert_eq(uint64_t n, aval *argv) {
    want_args("assert_eq", n, 2, 3);
    if (!values_equal(argv[0], argv[1])) {
        Buf b;
        buf_init(&b);
        if (n == 3) {
            /* Match integration test harness format. */
            buf_adds(&b, "Assertion failed: expected ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, ", found ");
            format_into(&b, argv[1], 1);
            buf_adds(&b, " (");
            aval msg = a_str(argv[2]);
            uint64_t mlen;
            const char *mp = str_data(msg, &mlen);
            buf_addn(&b, mp, mlen);
            buf_addc(&b, ')');
        } else {
            buf_adds(&b, "assertion failed: ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, " != ");
            format_into(&b, argv[1], 1);
        }
        aval s = buf_take(&b);
        uint64_t len;
        a_panic_msg(str_data(s, &len));
    }
    return A_NONE;
}

aval bi_input(uint64_t n, aval *argv) {
    want_args("input", n, 0, 1);
    if (n == 1) {
        aval s = a_str(argv[0]);
        uint64_t len;
        const char *p = str_data(s, &len);
        fwrite(p, 1, (size_t)len, stdout);
    }
    fflush(stdout);
    char *buf = (char *)xmalloc(4096);
    if (!fgets(buf, 4096, stdin)) { free(buf); panicf("Input ended before a line could be read."); }
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len && buf[len - 1] == '\r') buf[--len] = '\0';
    aval r = a_str_new(buf, (uint64_t)len);
    free(buf);
    return r;
}


aval bi_isinstance(uint64_t n, aval *argv) {
    want_args("isinstance", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("isinstance() arg 2 must be str");
    uint64_t len;
    const char *p = str_data(argv[1], &len);
    char *name = xstrndup(p, (size_t)len);
    int r = a_obj_is_instance(argv[0], name);
    free(name);
    return bool_aval(r);
}

aval bi_join(uint64_t n, aval *argv) {
    want_args("join", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("join() separator must be str");
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    for (uint64_t i = 0; i < a->h.len; ++i)
        a->items[i] = a_str(a->items[i]);
    return a_str_joinsep(argv[1], a->h.len, a->items);
}

aval bi_sorted(uint64_t n, aval *argv) {
    want_args("sorted", n, 1, 1);
    aval t = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(t);
    sort_arr(a->items, a->h.len);
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

aval bi_log2(uint64_t n, aval *argv) { want_args("log2", n, 1, 1); double x = math_arg(argv[0], "log2"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log2(x)); }
aval bi_log10(uint64_t n, aval *argv) { want_args("log10", n, 1, 1); double x = math_arg(argv[0], "log10"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log10(x)); }
aval bi_atan2(uint64_t n, aval *argv) { want_args("atan2", n, 2, 2); return mkdouble(atan2(math_arg(argv[0], "atan2"), math_arg(argv[1], "atan2"))); }
aval bi_flush(uint64_t n, aval *argv) { (void)argv; want_args("flush", n, 0, 0); fflush(stdout); return A_NONE; }
aval bi_i8(uint64_t n, aval *argv) { want_args("i8", n, 1, 1); return mkint_full((int8_t)int_val_full(a_int(argv[0]))); }
aval bi_i16(uint64_t n, aval *argv) { want_args("i16", n, 1, 1); return mkint_full((int16_t)int_val_full(a_int(argv[0]))); }
aval bi_i32(uint64_t n, aval *argv) { want_args("i32", n, 1, 1); return mkint_full((int32_t)int_val_full(a_int(argv[0]))); }
aval bi_u8(uint64_t n, aval *argv) { want_args("u8", n, 1, 1); return mkint_full((uint8_t)int_val_full(a_int(argv[0]))); }
aval bi_u16(uint64_t n, aval *argv) { want_args("u16", n, 1, 1); return mkint_full((uint16_t)int_val_full(a_int(argv[0]))); }
aval bi_u32(uint64_t n, aval *argv) { want_args("u32", n, 1, 1); return mkint_full((uint32_t)int_val_full(a_int(argv[0]))); }
aval bi_f32(uint64_t n, aval *argv) { want_args("f32", n, 1, 1); return a_mkdouble((float)num_to_double(argv[0])); }


aval bi_env_get(uint64_t n, aval *argv) {
    want_args("env_get", n, 1, 1);
    const char *name = path_arg(argv[0], "name");
    const char *v = getenv(name);
    if (!v) return A_NONE;
    return a_str_new(v, strlen(v));
}

