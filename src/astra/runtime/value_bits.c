#include "a_runtime_internal.h"

/* Packed-int fast path for `in` on sequences (methods_helpers.c). */
int seq_contains(aval v, aval item);

/* ------------------------------------------------------------------ */
/* bitwise / shifts / membership                                       */
/* ------------------------------------------------------------------ */

aval a_band_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("&", l, r);
    return mkint_full(int_val_full(l) & int_val_full(r));
}
aval a_bor_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("|", l, r);
    return mkint_full(int_val_full(l) | int_val_full(r));
}
aval a_bxor_slow(aval l, aval r) {
    if (!is_int_val(l) || !is_int_val(r)) panic_binop("^", l, r);
    return mkint_full(int_val_full(l) ^ int_val_full(r));
}

static int64_t shift_amount(aval v) {
    int64_t s = expect_int64(v, "shift amount");
    if (s < 0 || s > 63)
        panicf("Shift amount %lld is out of range for integers.",
               (long long)s);
    return s;
}

aval a_shl_slow(aval l, aval r) {
    if (!is_int_val(l)) panic_binop("<<", l, r);
    int64_t a = int_val_full(l), s = shift_amount(r);
    return mkint_full((int64_t)((uint64_t)a << s));
}
aval a_shr_slow(aval l, aval r) {
    if (!is_int_val(l)) panic_binop(">>", l, r);
    int64_t a = int_val_full(l), s = shift_amount(r);
    return mkint_full(a >> s);
}

static int range_contains_raw(ARange *r, int64_t v) {
    if (r->step > 0) {
        if (v < r->lo || v >= r->hi) return 0;
    } else {
        if (v > r->lo || v <= r->hi) return 0;
    }
    long double diff = (long double)v - (long double)r->lo;
    return fabsl(fmodl(diff, (long double)r->step)) == 0.0L;
}

aval a_range_contains(aval r, aval v) {
    AObj *o = obj_of(r);
    if (!o || o->type != T_RANGE)
        panicf("range_contains() argument must be range");
    if (!is_int_val(v)) panicf("range membership requires an integer");
    return bool_aval(range_contains_raw((ARange *)o, int_val_full(v)));
}

aval a_in_range(aval v, aval lo, aval hi) {
    if (is_int_val(v) && is_int_val(lo) && is_int_val(hi)) {
        int64_t x = int_val_full(v), a = int_val_full(lo), b = int_val_full(hi);
        return bool_aval(a <= x && x < b);
    }
    if (is_num(v) && is_num(lo) && is_num(hi)) {
        double x = num_to_double(v), a = num_to_double(lo), b = num_to_double(hi);
        return bool_aval(a <= x && x < b);
    }
    panicf("unsupported operand type(s) for in: '%s' and 'range'",
           type_name_cstr(v));
    return A_NONE;
}

aval a_in_range_inclusive(aval v, aval lo, aval hi) {
    if (is_int_val(v) && is_int_val(lo) && is_int_val(hi)) {
        int64_t x = int_val_full(v), a = int_val_full(lo), b = int_val_full(hi);
        return bool_aval(a <= x && x <= b);
    }
    if (is_num(v) && is_num(lo) && is_num(hi)) {
        double x = num_to_double(v), a = num_to_double(lo), b = num_to_double(hi);
        return bool_aval(a <= x && x <= b);
    }
    panicf("unsupported operand type(s) for in: '%s' and 'range'",
           type_name_cstr(v));
    return A_NONE;
}

aval a_contains(aval cont, aval item) {
    AObj *co = obj_of(cont);
    if (co && co->type == T_PYOBJ) return a_py_contains(cont, item);
    if (co && co->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)co;
        for (int64_t i = 0; i < b->count; ++i)
            if (values_equal(pybuf_read(b, i), item)) return A_TRUE;
        return A_FALSE;
    }
    switch (kind_of(cont)) {
        case K_STR: {
            if (kind_of(item) != K_STR)
                panicf("argument of type '%s' is not iterable", type_name_cstr(item));
            uint64_t hn, nn;
            const char *h = str_data(cont, &hn), *n = str_data(item, &nn);
            size_t pos;
            return bool_aval(find_bytes(h, (size_t)hn, n, (size_t)nn, 0, &pos) != NULL);
        }
        case K_LIST:
        case K_TUPLE:
        case K_SET:
            return bool_aval(seq_contains(cont, item));
        case K_DICT: {
            ADict *d = (ADict *)obj_of(cont);
            return bool_aval(dict_find(d, item) >= 0);
        }
        case K_RANGE: {
            if (!is_int_val(item))
                panicf("unsupported operand type(s) for in: '%s' and 'range'",
                       type_name_cstr(item));
            return bool_aval(range_contains_raw((ARange *)obj_of(cont),
                                                int_val_full(item)));
        }
        default:
            panicf("argument of type '%s' is not iterable", type_name_cstr(cont));
            return A_NONE;
    }
}

