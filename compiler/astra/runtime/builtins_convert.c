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

// Borrow array storage for already-materialized list/tuple/set values;
// anything else is collected through the iterator protocol exactly like
// collect_iterable(). Callers must use arr_elem()/arr_packed() accessors
aval bi_i8(uint64_t n, aval *argv) { want_args("i8", n, 1, 1); return mkint_full((int8_t)int_val_full(a_int(argv[0]))); }
aval bi_i16(uint64_t n, aval *argv) { want_args("i16", n, 1, 1); return mkint_full((int16_t)int_val_full(a_int(argv[0]))); }
aval bi_i32(uint64_t n, aval *argv) { want_args("i32", n, 1, 1); return mkint_full((int32_t)int_val_full(a_int(argv[0]))); }
aval bi_u8(uint64_t n, aval *argv) { want_args("u8", n, 1, 1); return mkint_full((uint8_t)int_val_full(a_int(argv[0]))); }
aval bi_u16(uint64_t n, aval *argv) { want_args("u16", n, 1, 1); return mkint_full((uint16_t)int_val_full(a_int(argv[0]))); }
aval bi_u32(uint64_t n, aval *argv) { want_args("u32", n, 1, 1); return mkint_full((uint32_t)int_val_full(a_int(argv[0]))); }
aval bi_f32(uint64_t n, aval *argv) { want_args("f32", n, 1, 1); return a_mkdouble((float)num_to_double(argv[0])); }
