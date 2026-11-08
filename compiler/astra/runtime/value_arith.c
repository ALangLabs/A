#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* integer overflow helpers                                            */
/* ------------------------------------------------------------------ */

static int iadd_of(int64_t a, int64_t b, int64_t *r) {
    if (b == INT64_MIN) {
        if (a < 0) return 1;
        *r = (int64_t)((uint64_t)a + (uint64_t)b);
        return 0;
    }
    if (b > 0) {
        if (a > INT64_MAX - b) return 1;
    } else if (a < INT64_MIN - b) return 1;
    *r = a + b;
    return 0;
}

static int isub_of(int64_t a, int64_t b, int64_t *r) {
    if (b == INT64_MIN) {
        if (a >= 0) return 1;
        *r = (int64_t)((uint64_t)a - (uint64_t)b);
        return 0;
    }
    if (b > 0) {
        if (a < INT64_MIN + b) return 1;
    } else if (a > INT64_MAX + b) return 1;
    *r = a - b;
    return 0;
}

static uint64_t iabs_u(int64_t x) {
    return x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
}

static int imul_of(int64_t a, int64_t b, int64_t *r) {
    if (a == 0 || b == 0) { *r = 0; return 0; }
    uint64_t ua = iabs_u(a), ub = iabs_u(b);
    int neg = (a < 0) != (b < 0);
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (ua > limit / ub) return 1;
    uint64_t m = ua * ub;
    if (neg) {
        *r = (m == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)m;
    } else {
        *r = (int64_t)m;
    }
    return 0;
}

void panic_binop(const char *op, aval l, aval r) {
    panicf("unsupported operand type(s) for %s: '%s' and '%s'", op,
           type_name_cstr(l), type_name_cstr(r));
}

static aval arith_int_double(aval l, aval r, char op) {
    double a = num_to_double(l), b = num_to_double(r), x = 0.0;
    switch (op) {
        case '+': x = a + b; break;
        case '-': x = a - b; break;
        case '*': x = a * b; break;
        case '/':
            if (b == 0.0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
            x = a / b;
            break;
        case '%': {
            if (b == 0.0) panicf("Remainder by zero.\n  = help: Check the divisor before dividing.");
            double m = fmod(a, b);
            if (m != 0.0 && ((m > 0.0) != (b > 0.0))) m += b;
            x = m;
            break;
        }
        case '^': x = pow(a, b); break;
        default: panicf("internal arithmetic op");
    }
    return mkdouble(x);
}

static aval arith_int_int(aval l, aval r, char op) {
    int64_t a = int_val_full(l), b = int_val_full(r), res;
    switch (op) {
        case '+':
            if (iadd_of(a, b, &res)) return mkdouble((double)a + (double)b);
            return mkint_full(res);
        case '-':
            if (isub_of(a, b, &res)) return mkdouble((double)a - (double)b);
            return mkint_full(res);
        case '*':
            if (imul_of(a, b, &res)) return mkdouble((double)a * (double)b);
            return mkint_full(res);
        case '/':
            if (b == 0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
            if (a == INT64_MIN && b == -1) return mkdouble(9223372036854775808.0);
            return mkint_full(a / b);
        case '%':
            if (b == 0) panicf("Remainder by zero.\n  = help: Check the divisor before dividing.");
            if (b == -1) return mkint_full(0);
            return mkint_full(a % b);
        case '^':
            if (b < 0) return mkdouble(pow((double)a, (double)b));
            if (a == 0 && b == 0) return mkint_full(1);
            if (a == 1 || a == -1) {
                if (a == 1) return mkint_full(1);
                return mkint_full((b & 1) ? -1 : 1);
            }
            {
                int64_t base = a, e = b, power = 1;
                while (e > 0) {
                    if (e & 1) {
                        if (imul_of(power, base, &power))
                            return mkdouble(pow((double)a, (double)b));
                    }
                    e >>= 1;
                    if (e > 0 && imul_of(base, base, &base))
                        return mkdouble(pow((double)a, (double)b));
                }
                return mkint_full(power);
            }
        default: panicf("internal arithmetic op");
    }
    return A_NONE;
}

static aval concat_str(aval strv, aval other, int str_left) {
    uint64_t sn;
    const char *sp = str_data(strv, &sn);
    Buf b;
    buf_init(&b);
    if (str_left) {
        buf_addn(&b, sp, (size_t)sn);
        format_into(&b, other, 0);
    } else {
        format_into(&b, other, 0);
        buf_addn(&b, sp, (size_t)sn);
    }
    return buf_take(&b);
}

static aval concat_arr(aval l, aval r) {
    AArr *a = (AArr *)obj_of(l), *b = (AArr *)obj_of(r);
    int is_tuple = a->h.type == T_TUPLE;
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = is_tuple ? T_TUPLE : T_LIST;
    out->h.len = 0;
    out->h.cap = (uint64_t)a->h.len + b->h.len;
    out->iitems = NULL;
    out->flags = 0;
    if (!is_tuple && arr_packed(a) && arr_packed(b)) {
        /* int-list + int-list stays packed */
        out->items = NULL;
        out->iitems = (int64_t *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(int64_t));
        memcpy(out->iitems, a->iitems, (size_t)a->h.len * sizeof(int64_t));
        memcpy(out->iitems + a->h.len, b->iitems, (size_t)b->h.len * sizeof(int64_t));
        out->h.len = a->h.len + b->h.len;
        out->flags |= ARR_F_PACKED;
        return mkobj(&out->h);
    }
    arr_ensure_aval(a);
    arr_ensure_aval(b);
    out->items = (aval *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(aval));
    memcpy(out->items, a->items, (size_t)a->h.len * sizeof(aval));
    memcpy(out->items + a->h.len, b->items, (size_t)b->h.len * sizeof(aval));
    out->h.len = a->h.len + b->h.len;
    return mkobj(&out->h);
}

aval binary_arith(aval l, aval r, char op, const char *opname) {
    int li = is_int_val(l), ri = is_int_val(r);
    int ld = a_is_double(l), rd = a_is_double(r);
    if ((li && rd) || (ld && ri) || (ld && rd))
        return arith_int_double(l, r, op);
    if (op == '+' ) {
        if (kind_of(l) == K_STR) return concat_str(l, r, 1);
        if (kind_of(r) == K_STR) return concat_str(r, l, 0);
        if (kind_of(l) == K_BYTES && kind_of(r) == K_BYTES) {
            ABytes *a = (ABytes *)obj_of(l), *b2 = (ABytes *)obj_of(r);
            aval out = a_bytes_new((uint64_t)a->h.len + (uint64_t)b2->h.len);
            ABytes *o = a_bytes_obj(out);
            a_bytes_append(o, a->data ? a->data : "", (uint64_t)a->h.len);
            a_bytes_append(o, b2->data ? b2->data : "", (uint64_t)b2->h.len);
            return out;
        }
        if (kind_of(l) == K_LIST && kind_of(r) == K_LIST) return concat_arr(l, r);
        if (kind_of(l) == K_TUPLE && kind_of(r) == K_TUPLE) return concat_arr(l, r);
    }
    if (op == '*' ) {
        if (kind_of(l) == K_STR && ri) return str_repeat(l, int_val_full(r));
        if (kind_of(r) == K_STR && li) return str_repeat(r, int_val_full(l));
    }
    if (li && ri) return arith_int_int(l, r, op);
    panic_binop(opname, l, r);
    return A_NONE;
}

aval a_add_slow(aval l, aval r) { return binary_arith(l, r, '+', "+"); }
aval a_sub_slow(aval l, aval r) { return binary_arith(l, r, '-', "-"); }
aval a_mul_slow(aval l, aval r) { return binary_arith(l, r, '*', "*"); }
aval a_div_slow(aval l, aval r) { return binary_arith(l, r, '/', "/"); }
aval a_mod_slow(aval l, aval r) { return binary_arith(l, r, '%', "%"); }
aval a_pow(aval l, aval r) { return binary_arith(l, r, '^', "**"); }

