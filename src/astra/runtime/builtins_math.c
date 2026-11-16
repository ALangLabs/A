#include "a_runtime_internal.h"

aval bi_pow(uint64_t n, aval *argv) {
    /* pow() is -> f64 per docs; the ** operator stays int for int base. */
    want_args("pow", n, 2, 2);
    aval r = a_pow(argv[0], argv[1]);
    if (is_int_val(r)) return mkdouble((double)int_val_full(r));
    return r;
}

aval bi_divmod(uint64_t n, aval *argv) {
    want_args("divmod", n, 2, 2);
    if (is_int_val(argv[0]) && is_int_val(argv[1])) {
        int64_t a = int_val_full(argv[0]);
        int64_t b = int_val_full(argv[1]);
        if (b == 0) panicf("divmod() division by zero");
        if (a == INT64_MIN && b == -1) {
            /* quotient overflows i64; fall back to float pair */
            double q = floor((double)a / (double)b);
            double r = (double)a - q * (double)b;
            aval pair[2] = { mkdouble(q), mkdouble(r) };
            return a_tuple_new(2, pair);
        }
        int64_t q = a / b;
        int64_t r = a % b;
        if (r != 0 && ((r < 0) != (b < 0))) { --q; r += b; }
        aval pair[2] = { mkint_full(q), mkint_full(r) };
        return a_tuple_new(2, pair);
    }
    double a = math_arg(argv[0], "divmod");
    double b = math_arg(argv[1], "divmod");
    if (b == 0.0) panicf("divmod() division by zero");
    double q = floor(a / b);
    double r = a - q * b;
    aval pair[2] = { mkdouble(q), mkdouble(r) };
    return a_tuple_new(2, pair);
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

aval bi_log2(uint64_t n, aval *argv) { want_args("log2", n, 1, 1); double x = math_arg(argv[0], "log2"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log2(x)); }
aval bi_log10(uint64_t n, aval *argv) { want_args("log10", n, 1, 1); double x = math_arg(argv[0], "log10"); if (x <= 0.0) panicf("math domain error"); return mkdouble(log10(x)); }
aval bi_atan2(uint64_t n, aval *argv) { want_args("atan2", n, 2, 2); return mkdouble(atan2(math_arg(argv[0], "atan2"), math_arg(argv[1], "atan2"))); }
