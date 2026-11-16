#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* truthiness / conversions                                            */
/* ------------------------------------------------------------------ */

int truthy(aval v) {
    switch (kind_of(v)) {
        case K_NONE: return 0;
        case K_BOOL: return v == A_TRUE;
        case K_INT: return int_val_full(v) != 0;
        case K_FLOAT: return a_double_val(v) != 0.0;
        case K_STR: return ((AStr *)obj_of(v))->h.len != 0;
        case K_CHAR: return ((AStr *)obj_of(v))->h.len != 0;
        case K_VIEW: return ((AView *)obj_of(v))->len != 0;
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_DICT: return ((AArr *)obj_of(v))->h.len != 0;
        default: {
            AObj *o = obj_of(v);
            if (o && o->type == T_PYOBJ) return a_py_truthy(v);
            if (o && o->type == T_PYBUF) return ((APyBuf *)o)->count != 0;
            return 1;
        }
    }
}

aval a_truthy_raw_slow(aval v) { return (aval)(truthy(v) ? 1 : 0); }
aval a_not(aval v) { return bool_aval(!truthy(v)); }
aval a_bool(aval v) { return bool_aval(truthy(v)); }

aval a_neg_slow(aval v) {
    if (is_int_val(v)) {
        int64_t i = int_val_full(v);
        if (i == INT64_MIN) return mkdouble(-(double)INT64_MIN);
        return mkint_full(-i);
    }
    if (a_is_double(v)) return mkdouble(-a_double_val(v));
    panicf("bad operand type for unary -: '%s'", type_name_cstr(v));
    return A_NONE;
}

aval a_bnot_slow(aval v) {
    if (!is_int_val(v))
        panicf("bad operand type for unary ~: '%s'", type_name_cstr(v));
    return mkint_full(~int_val_full(v));
}

static aval parse_int_str(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) { ++s; --n; }
    while (n && isspace((unsigned char)s[n - 1])) --n;
    if (n == 0) panicf("invalid literal for int()");
    char *tmp = xstrndup(s, n);
    char *end = NULL;
    errno = 0;
    long long x = strtoll(tmp, &end, 10);
    if (end == tmp) {
        double d = strtod(tmp, &end);
        if (end == tmp || *end != '\0') {
            free(tmp);
            panicf("invalid literal for int()");
        }
        if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0) {
            free(tmp);
            panicf("int() argument out of range");
        }
        free(tmp);
        return mkint_full((int64_t)d);
    }
    if (*end != '\0') {
        free(tmp);
        panicf("invalid literal for int()");
    }
    if (errno == ERANGE) {
        free(tmp);
        panicf("int() argument out of range");
    }
    free(tmp);
    return mkint_full((int64_t)x);
}

aval a_int(aval v) {
    if (is_int_val(v)) return v;
    if (v == A_TRUE) return mkint_full(1);
    if (v == A_FALSE) return mkint_full(0);
    if (a_is_double(v)) {
        double d = trunc(a_double_val(v));
        if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
            panicf("cannot convert float to int");
        return mkint_full((int64_t)d);
    }
    if (kind_of(v) == K_STR) {
        uint64_t n;
        const char *p = str_data(v, &n);
        return parse_int_str(p, (size_t)n);
    }
    panicf("cannot convert '%s' to int", type_name_cstr(v));
    return A_NONE;
}

aval a_float(aval v) {
    if (is_int_val(v)) return mkdouble((double)int_val_full(v));
    if (a_is_double(v)) return v;
    if (v == A_TRUE) return mkdouble(1.0);
    if (v == A_FALSE) return mkdouble(0.0);
    if (kind_of(v) == K_STR) {
        uint64_t n;
        const char *p = str_data(v, &n);
        char *tmp = xstrndup(p, (size_t)n);
        char *q = tmp;
        while (isspace((unsigned char)*q)) ++q;
        char *end = NULL;
        double d = strtod(q, &end);
        if (end == q) {
            free(tmp);
            panicf("could not convert string to float");
        }
        while (end && isspace((unsigned char)*end)) ++end;
        if (!end || *end != '\0') {
            free(tmp);
            panicf("could not convert string to float");
        }
        free(tmp);
        return mkdouble(d);
    }
    panicf("cannot convert '%s' to float", type_name_cstr(v));
    return A_NONE;
}

aval a_len(aval v) {
    AObj *lo = obj_of(v);
    if (lo && lo->type == T_PYOBJ) return a_py_len(v);
    if (lo && lo->type == T_PYBUF) return mkint_full(((APyBuf *)lo)->count);
    switch (kind_of(v)) {
        case K_STR:
        case K_CHAR:
            /* len() is a code-point count, which needs a UTF-8 scan; the
               count is cached per string object (ropes cache per node, so
               append loops stay amortized O(1) per len() call). */
            return mkint_full(a_str_cpcount(v));
        case K_VIEW: return mkint_full(((AView *)obj_of(v))->len);
        case K_BYTES: return mkint_full(((ABytes *)obj_of(v))->h.len);
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_DICT: return mkint_full(((AArr *)obj_of(v))->h.len);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            long double n;
            if (r->step > 0) {
                if ((long double)r->hi <= (long double)r->lo) n = 0.0L;
                else n = floorl(((long double)r->hi - (long double)r->lo - 1.0L) /
                               (long double)r->step) + 1.0L;
            } else {
                if ((long double)r->hi >= (long double)r->lo) n = 0.0L;
                else n = floorl(((long double)r->lo - (long double)r->hi - 1.0L) /
                               (-(long double)r->step)) + 1.0L;
            }
            if (n <= (long double)INT64_MAX) return mkint_full((int64_t)n);
            return mkdouble((double)n);
        }
        default:
            panicf("object of type '%s' has no len()", type_name_cstr(v));
            return A_NONE;
    }
}

