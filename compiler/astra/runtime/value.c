#include "a_runtime_internal.h"

static aval pybuf_read(APyBuf *b, int64_t i);
static void pybuf_write(APyBuf *b, int64_t i, aval v);

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
        case K_STR: {
            AStr *s = (AStr *)obj_of(v);
            return mkint_full(u8_count(s->data, s->h.len));
        }
        case K_CHAR: {
            AStr *s = (AStr *)obj_of(v);
            return mkint_full(u8_count(s->data, s->h.len));
        }
        case K_VIEW: return mkint_full(((AView *)obj_of(v))->len);
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

/* ------------------------------------------------------------------ */
/* equality / ordering                                                 */
/* ------------------------------------------------------------------ */

int values_equal(aval a, aval b) {
    int ka = kind_of(a), kb = kind_of(b);
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b))
            return int_val_full(a) == int_val_full(b);
        return num_to_double(a) == num_to_double(b);
    }
    if (a == b) return 1;
    /* Views compare as the underlying slice (view == list if elements match). */
    if (ka == K_VIEW || kb == K_VIEW) {
        if (ka != K_VIEW && ka != K_LIST && ka != K_TUPLE) return 0;
        if (kb != K_VIEW && kb != K_LIST && kb != K_TUPLE) return 0;
        int64_t la = ka == K_VIEW ? ((AView *)obj_of(a))->len : (int64_t)((AArr *)obj_of(a))->h.len;
        int64_t lb = kb == K_VIEW ? ((AView *)obj_of(b))->len : (int64_t)((AArr *)obj_of(b))->h.len;
        if (la != lb) return 0;
        /* Views may alias packed-int storage: normalize both sides to
           aval elements once before the element-wise compare. */
        if (ka == K_VIEW)
            arr_ensure_aval((AArr *)obj_of(((AView *)obj_of(a))->parent));
        else if (ka == K_LIST)
            arr_ensure_aval((AArr *)obj_of(a));
        if (kb == K_VIEW)
            arr_ensure_aval((AArr *)obj_of(((AView *)obj_of(b))->parent));
        else if (kb == K_LIST)
            arr_ensure_aval((AArr *)obj_of(b));
        for (int64_t i = 0; i < la; ++i) {
            aval xa = ka == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(a))->parent))->items[(size_t)(((AView *)obj_of(a))->off + i)]
                                   : ((AArr *)obj_of(a))->items[(size_t)i];
            aval xb = kb == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(b))->parent))->items[(size_t)(((AView *)obj_of(b))->off + i)]
                                   : ((AArr *)obj_of(b))->items[(size_t)i];
            if (!values_equal(xa, xb)) return 0;
        }
        return 1;
    }
    if (ka != kb) return 0;
    if (ka == K_OTHER) {
        AObj *oa = obj_of(a), *ob = obj_of(b);
        if (oa && ob && oa->type == T_PYOBJ && ob->type == T_PYOBJ)
            return a_py_equals(a, b);
        if (oa && ob && oa->type == T_PYBUF && ob->type == T_PYBUF) {
            APyBuf *x = (APyBuf *)oa, *y = (APyBuf *)ob;
            if (x->count != y->count || x->kind != y->kind ||
                x->itemsize != y->itemsize)
                return 0;
            for (int64_t i = 0; i < x->count; ++i)
                if (!values_equal(pybuf_read(x, i), pybuf_read(y, i))) return 0;
            return 1;
        }
        return 0;
    }
    switch (ka) {
        case K_BOOL: return 0;
        case K_STR: return str_eq(a, b);
        case K_CHAR: return str_eq(a, b);
        case K_LIST:
        case K_TUPLE: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            arr_ensure_aval(x);
            arr_ensure_aval(y);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i)
                if (!values_equal(x->items[i], y->items[i])) return 0;
            return 1;
        }
        case K_DICT: {
            ADict *x = (ADict *)obj_of(a), *y = (ADict *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j) {
                    if (values_equal(x->pairs[i].k, y->pairs[j].k) &&
                        values_equal(x->pairs[i].v, y->pairs[j].v)) {
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            }
            return 1;
        }
        case K_SET: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j)
                    if (values_equal(x->items[i], y->items[j])) { found = 1; break; }
                if (!found) return 0;
            }
            return 1;
        }
        case K_RANGE: {
            ARange *x = (ARange *)obj_of(a), *y = (ARange *)obj_of(b);
            return x->lo == y->lo && x->hi == y->hi && x->step == y->step;
        }
        case K_RESULT: {
            AResult *x = (AResult *)obj_of(a), *y = (AResult *)obj_of(b);
            return x->ok == y->ok && x->is_option == y->is_option && values_equal(x->value, y->value);
        }
        default: return 0;
    }
}

aval a_eq_slow(aval l, aval r) { return bool_aval(values_equal(l, r)); }
aval a_ne_slow(aval l, aval r) { return bool_aval(!values_equal(l, r)); }

static int cmp_ordered(aval a, aval b, const char *op) {
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b)) {
            int64_t x = int_val_full(a), y = int_val_full(b);
            return x < y ? -1 : x > y ? 1 : 0;
        }
        double x = num_to_double(a), y = num_to_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind_of(a) == K_STR && kind_of(b) == K_STR) {
        uint64_t la, lb;
        const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
        size_t n = la < lb ? (size_t)la : (size_t)lb;
        int c = n ? memcmp(pa, pb, n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (kind_of(a) == K_BOOL && kind_of(b) == K_BOOL) {
        int x = a == A_TRUE, y = b == A_TRUE;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    panicf("unsupported operand type(s) for %s: '%s' and '%s'", op,
           type_name_cstr(a), type_name_cstr(b));
    return 0;
}

aval a_lt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<") < 0); }
aval a_le_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<=") <= 0); }
aval a_gt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">") > 0); }
aval a_ge_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">=") >= 0); }

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

static void panic_binop(const char *op, aval l, aval r) {
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
        case K_SET: {
            AArr *a = (AArr *)obj_of(cont);
            arr_ensure_aval(a);
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (values_equal(a->items[i], item)) return A_TRUE;
            return A_FALSE;
        }
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

aval a_dict_set(aval d, aval k, aval v) {
    dict_put(dict_of(d, "dict"), k, v);
    return A_NONE;
}

aval a_dict_get(aval d, aval k, aval dflt) {
    ADict *m = dict_of(d, "dict");
    int64_t i = dict_find(m, k);
    return i >= 0 ? m->pairs[i].v : dflt;
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
/* indexing / slicing / iteration                                      */
/* ------------------------------------------------------------------ */

int64_t normalize_index(int64_t idx, uint64_t len) {
    if (idx < 0) idx += (int64_t)len;
    if (idx < 0 || idx >= (int64_t)len) panicf("index out of range");
    return idx;
}

/* ------------------------------------------------------------------ */
/* T_PYBUF element access: raw reads/writes into the exported buffer.  */
/* No GIL is taken — the Py_buffer export keeps the exporter alive.    */
/* ------------------------------------------------------------------ */

static aval pybuf_read(APyBuf *b, int64_t i) {
    const char *p = b->data + (size_t)i * (size_t)b->itemsize;
    switch (b->kind) {
        case APYBUF_U8: return mkint_full((unsigned char)p[0]);
        case APYBUF_I8: return mkint_full((signed char)p[0]);
        case APYBUF_BOOL: return bool_aval(p[0] != 0);
        case APYBUF_U16: { uint16_t x; memcpy(&x, p, 2); return mkint_full(x); }
        case APYBUF_I16: { int16_t x; memcpy(&x, p, 2); return mkint_full(x); }
        case APYBUF_U32: { uint32_t x; memcpy(&x, p, 4); return mkint_full(x); }
        case APYBUF_I32: { int32_t x; memcpy(&x, p, 4); return mkint_full(x); }
        case APYBUF_U64: {
            uint64_t x; memcpy(&x, p, 8);
            return x <= (uint64_t)INT64_MAX ? mkint_full((int64_t)x)
                                            : mkdouble((double)x);
        }
        case APYBUF_I64: { int64_t x; memcpy(&x, p, 8); return mkint_full(x); }
        case APYBUF_F32: { float f; memcpy(&f, p, 4); return mkdouble((double)f); }
        case APYBUF_F64: { double d; memcpy(&d, p, 8); return mkdouble(d); }
        default: panicf("internal: bad pybuffer element kind");
    }
    return A_NONE;
}

static void pybuf_write(APyBuf *b, int64_t i, aval v) {
    if (b->readonly)
        panicf("pybuffer is read-only (the Python exporter did not grant write access)");
    char *p = b->data + (size_t)i * (size_t)b->itemsize;
    switch (b->kind) {
        case APYBUF_BOOL:
            p[0] = (char)(truthy(v) ? 1 : 0);
            return;
        case APYBUF_F32: {
            float f = (float)num_to_double(v);
            memcpy(p, &f, 4);
            return;
        }
        case APYBUF_F64: {
            double d = num_to_double(v);
            memcpy(p, &d, 8);
            return;
        }
        default: {
            int64_t x = expect_int64(v, "pybuffer element");
            switch (b->kind) {
                case APYBUF_U8: p[0] = (char)(uint8_t)x; return;
                case APYBUF_I8: p[0] = (char)(int8_t)x; return;
                case APYBUF_U16: { uint16_t t = (uint16_t)x; memcpy(p, &t, 2); return; }
                case APYBUF_I16: { int16_t t = (int16_t)x; memcpy(p, &t, 2); return; }
                case APYBUF_U32: { uint32_t t = (uint32_t)x; memcpy(p, &t, 4); return; }
                case APYBUF_I32: { int32_t t = (int32_t)x; memcpy(p, &t, 4); return; }
                case APYBUF_U64: { uint64_t t = (uint64_t)x; memcpy(p, &t, 8); return; }
                case APYBUF_I64: memcpy(p, &x, 8); return;
                default: panicf("internal: bad pybuffer element kind");
            }
        }
    }
}

aval a_getitem(aval obj, aval idx) {
    AObj *oo = obj_of(obj);
    if (oo && oo->type == T_PYOBJ) return a_py_getitem(obj, idx);
    if (oo && oo->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)oo;
        int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)b->count);
        return pybuf_read(b, i);
    }
    switch (kind_of(obj)) {
        case K_STR: {
            AStr *s = (AStr *)obj_of(obj);
            int64_t total = (int64_t)u8_count(s->data, s->h.len);
            int64_t i = normalize_index(expect_int64(idx, "string index"), (uint64_t)total);
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            return a_str_new(s->data + boff, (uint64_t)l);
        }
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            return root->items[(size_t)(base + i)];
        }
        case K_CHAR: {
            panicf("object of type 'char' is not subscriptable");
            return A_NONE;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            return arr_elem(a, (uint64_t)i);
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(obj);
            int64_t i = dict_find(d, idx);
            if (i < 0) {
                uint64_t kl = 0;
                const char *kp = kind_of(idx) == K_STR ? str_data(idx, &kl) : NULL;
                if (kp)
                    panicf("Key %s is not in the map.", (char *)memcpy((char[256]){0}, kp, kl < 255 ? kl : 255));
                panicf("key not found");
            }
            return d->pairs[i].v;
        }
        default:
            panicf("object of type '%s' is not subscriptable", type_name_cstr(obj));
            return A_NONE;
    }
}

aval a_setitem(aval obj, aval idx, aval val) {
    AObj *so = obj_of(obj);
    if (so && so->type == T_PYOBJ) return a_py_setitem(obj, idx, val);
    if (so && so->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)so;
        int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)b->count);
        pybuf_write(b, i, val);
        return val;
    }
    switch (kind_of(obj)) {
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            root->items[(size_t)(base + i)] = val;
            return val;
        }
        case K_LIST: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            if (arr_packed(a)) {
                if (is_int_val(val)) {
                    a->iitems[i] = int_val_full(val);
                    return val;
                }
                a->flags |= ARR_F_NOMIX;
                arr_ensure_aval(a);
            }
            a->items[i] = val;
            return val;
        }
        case K_DICT: {
            dict_put((ADict *)obj_of(obj), idx, val);
            return val;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
}

/* Bulk fill for the `for i in lo..hi[..step]: xs[i] = v` idiom emitted by
 * the codegen fill-loop fast path. Applies a_setitem's K_LIST/K_VIEW
 * element stores for the whole index range: normalize_index panics
 * "index out of range" at the first bad index, exactly like the
 * interpreted loop, and len/root are resolved once up front (a plain
 * item store cannot resize the container). The index advances with
 * unsigned wrap to mirror the int64 slot arithmetic of the counted
 * loop. Returns val (the loop discards it). */
aval a_list_fill_range(aval obj, int64_t lo, int64_t hi, int64_t step,
                       aval val) {
    AArr *root;
    int64_t base = 0;
    uint64_t len;
    switch (kind_of(obj)) {
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            len = (uint64_t)vw->len;
            root = view_root(obj, &base);
            break;
        }
        case K_LIST: {
            AArr *a = (AArr *)obj_of(obj);
            len = a->h.len;
            root = a;
            break;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
    /* Packed-int storage: an all-int fill stays packed; a non-int value
       converts the root to aval storage first (mirrors a_setitem). */
    int pok = 0;
    int64_t pv = 0;
    if (arr_packed(root)) {
        if (is_int_val(val)) {
            pok = 1;
            pv = int_val_full(val);
        } else {
            root->flags |= ARR_F_NOMIX;
            arr_ensure_aval(root);
        }
    }
    int64_t k = lo;
    if (step >= 0) {
        while (k < hi) {
            int64_t i = normalize_index(k, len);
            if (pok) root->iitems[(size_t)(base + i)] = pv;
            else root->items[(size_t)(base + i)] = val;
            k = (int64_t)((uint64_t)k + (uint64_t)step);
        }
    } else {
        while (k > hi) {
            int64_t i = normalize_index(k, len);
            if (pok) root->iitems[(size_t)(base + i)] = pv;
            else root->items[(size_t)(base + i)] = val;
            k = (int64_t)((uint64_t)k + (uint64_t)step);
        }
    }
    return val;
}

static aval view_new(aval parent, int64_t off, int64_t len, int is_tuple) {
    AView *vw = (AView *)xmalloc(sizeof(AView));
    vw->h.type = T_VIEW;
    vw->h.len = (uint32_t)len;
    vw->h.cap = 0;
    vw->parent = parent;
    vw->off = off;
    vw->len = len;
    vw->is_tuple = is_tuple;
    return mkobj(&vw->h);
}
/* Resolve a view (or direct list) to root AArr + absolute offset.
   Packed-int roots are normalized to aval storage here: every caller
   reads or writes root->items, so conversion is the single choke point
   that keeps views semantics identical. */
AArr *view_root(aval v, int64_t *off) {
    AObj *o = obj_of(v);
    if (o && o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base = 0;
        AArr *root = view_root(vw->parent, &base);
        *off = base + vw->off;
        return root;
    }
    *off = 0;
    AArr *r = (AArr *)o;
    if (r) arr_ensure_aval(r);
    return r;
}
static int64_t slice_step(aval v) {
    int64_t s = expect_int64(v, "slice step");
    if (s == 0) panicf("The slice step must be positive, found 0.");
    return s;
}

int64_t slice_bound(aval v, int64_t n, int64_t lower, int64_t upper,
                           int64_t dflt) {
    if (a_is_none(v)) return dflt;
    int64_t i = expect_int64(v, "slice index");
    if (i < 0) i += n;
    if (i < lower) i = lower;
    if (i > upper) i = upper;
    return i;
}

aval a_slice(aval obj, aval lo, aval hi, aval step) {
    int k = kind_of(obj);
    if (k != K_STR && k != K_LIST && k != K_TUPLE && k != K_VIEW)
        panicf("object of type '%s' is not sliceable", type_name_cstr(obj));
    AObj *o = obj_of(obj);
    uint64_t len;
    if (o->type == T_STR)
        len = u8_count(((AStr *)o)->data, ((AStr *)o)->h.len);
    else if (o->type == T_VIEW)
        len = (uint64_t)((AView *)o)->len;
    else
        len = ((AArr *)o)->h.len;
    int64_t n = (int64_t)len;
    int64_t st = a_is_none(step) ? 1 : slice_step(step);
    int64_t start, stop;
    if (st > 0) {
        start = slice_bound(lo, n, 0, n, 0);
        stop = slice_bound(hi, n, 0, n, n);
    } else {
        start = slice_bound(lo, n, -1, n - 1, n - 1);
        stop = slice_bound(hi, n, -1, n - 1, -1);
    }
    AVec v = { NULL, 0, 0 };
    if (o->type == T_STR) {
        AStr *s = (AStr *)o;
        Buf b;
        buf_init(&b);
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            if (boff < s->h.len) buf_addn(&b, s->data + boff, (size_t)l);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        return buf_take(&b);
    }
    /* Lists/tuples/views: contiguous step (1 or -1? only 1) returns a view
       that aliases the parent; stepped copies return a fresh list. */
    if (st == 1 && o->type != T_STR) {
        int64_t cnt = stop - start;
        if (cnt < 0) cnt = 0;
        if (o->type == T_VIEW) {
            AView *vw = (AView *)o;
            return view_new(vw->parent, vw->off + start, cnt, vw->is_tuple);
        }
        (void)v;
        return view_new(obj, start, cnt, o->type == T_TUPLE ? 1 : 0);
    }
    if (o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base;
        AArr *root = view_root(obj, &base);
        (void)vw;
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            avec_push(&v, root->items[(size_t)(base + i)]);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        aval r = list_from_vec(v.p, v.len, 0);
        free(v.p);
        arr_try_pack((AArr *)obj_of(r));
        return r;
    }
    AArr *a = (AArr *)o;
    arr_ensure_aval(a);
    for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
        avec_push(&v, a->items[i]);
        if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
        else { if (i < INT64_MIN - st) break; i += st; }
    }
    aval r = list_from_vec(v.p, v.len, o->type == T_TUPLE);
    free(v.p);
    arr_try_pack((AArr *)obj_of(r));
    return r;
}

aval a_iter_new(aval v) {
    AIter *it = (AIter *)xmalloc(sizeof(AIter));
    it->h.type = T_ITER;
    it->h.len = 0;
    it->h.cap = 0;
    it->src = v;
    it->idx = 0;
    it->cur = it->end = it->step = 0;
    it->py_mode = 0;
    it->has_lookahead = 0;
    it->lookahead = A_NONE;
    switch (kind_of(v)) {
        case K_VIEW:
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_STR:
        case K_DICT:
            return mkobj(&it->h);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            it->cur = r->lo;
            it->end = r->hi;
            it->step = r->step;
            return mkobj(&it->h);
        }
        default: {
            AObj *vo = obj_of(v);
            if (vo && vo->type == T_PYBUF) {
                /* Index-based iteration over the raw buffer elements;
                   handled by the generic path in a_iter_next. */
                return mkobj(&it->h);
            }
            if (vo && vo->type == T_PYOBJ) {
                /* Python iterator: src becomes a T_PYOBJ wrapping the
                   PyObject* iterator; items are fetched lazily through
                   a_py_iter_next with a one-item lookahead. */
                it->src = a_py_iter_begin(v);
                it->py_mode = 1;
                return mkobj(&it->h);
            }
            free(it);
            panicf("object of type '%s' is not iterable", type_name_cstr(v));
            return A_NONE;
        }
    }
}

aval a_iter_next(aval it, aval *out) {
    AObj *o = obj_of(it);
    if (!o || o->type != T_ITER) panicf("argument is not an iterator");
    AIter *iter = (AIter *)o;
    if (iter->py_mode) {
        if (!iter->has_lookahead) {
            if (!a_py_iter_next(iter->src, &iter->lookahead)) return (aval)0;
            iter->has_lookahead = 1;
        }
        *out = iter->lookahead;
        iter->lookahead = A_NONE;
        iter->has_lookahead = 0;
        return (aval)1;
    }
    aval src = iter->src;
    if (kind_of(src) == K_RANGE) {
        int64_t c = iter->cur, e = iter->end, s = iter->step;
        int more = s > 0 ? c < e : c > e;
        if (!more) return (aval)0;
        *out = mkint_full(c);
        if (s > 0) {
            if (c > INT64_MAX - s) iter->cur = e;
            else iter->cur = c + s;
        } else {
            if (c < INT64_MIN - s) iter->cur = e;
            else iter->cur = c + s;
        }
        return (aval)1;
    }
    AObj *so = obj_of(src);
    uint64_t len;
    aval *items = NULL;
    int64_t *iitems = NULL;
    ADict *d = NULL;
    AView *vw_src = NULL;
    APyBuf *pb_src = NULL;
    int64_t vw_base = 0;
    if (so->type == T_DICT) {
        d = (ADict *)so;
        len = d->h.len;
    } else if (so->type == T_PYBUF) {
        pb_src = (APyBuf *)so;
        len = (uint64_t)pb_src->count;
    } else if (so->type == T_VIEW) {
        vw_src = (AView *)so;
        len = (uint64_t)vw_src->len;
    } else if (so->type == T_STR || so->type == T_CHAR) {
        len = u8_count(((AStr *)so)->data, ((AStr *)so)->h.len);
    } else {
        AArr *a = (AArr *)so;
        len = a->h.len;
        if (arr_packed(a)) iitems = a->iitems;
        else items = a->items;
    }
    if (iter->idx >= len) return (aval)0;
    uint64_t i = iter->idx++;
    if (d) *out = d->pairs[i].k;
    else if (pb_src) *out = pybuf_read(pb_src, (int64_t)i);
    else if (vw_src) {
        AArr *root = view_root(src, &vw_base);
        *out = root->items[(size_t)(vw_base + (int64_t)i)];
    }
    else if (so->type == T_STR || so->type == T_CHAR) {
        AStr *s = (AStr *)so;
        uint64_t boff = cp_to_byte(s->data, s->h.len, (int64_t)i);
        int l = u8_len((unsigned char)s->data[boff]);
        if (boff + (uint64_t)l > s->h.len) l = 1;
        *out = a_char_new(s->data + boff, (uint64_t)l);
    }
    else if (iitems) *out = mkint_full(iitems[i]);
    else *out = items[i];
    return (aval)1;
}

/* ------------------------------------------------------------------ */
/* functions / environments / results                                  */
/* ------------------------------------------------------------------ */

static AFnon *fnon_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_FNON)
        panicf("%s must be callable, not '%s'", what, type_name_cstr(v));
    return (AFnon *)o;
}

aval fnon_new_raw(uint64_t entry, aval env, const char *name,
                         uint32_t flags, uint64_t nparams) {
    AFnon *f = (AFnon *)xmalloc(sizeof(AFnon));
    f->h.type = T_FNON;
    f->h.len = 0;
    f->h.cap = 0;
    f->entry = entry;
    f->env = env;
    f->nparams = nparams;
    f->flags = flags;
    f->name = name ? xstrdup(name) : NULL;
    return mkobj(&f->h);
}

aval a_fnon_new(uint64_t entry, aval env, const char *name) {
    return fnon_new_raw(entry, env, name, 0, NPARAMS_UNKNOWN);
}

aval a_closure_env_new(uint64_t n) {
    AEnv *e = (AEnv *)xmalloc(sizeof(AEnv) + (size_t)(n ? n : 1) * sizeof(aval));
    e->h.type = T_ENV;
    e->h.len = (uint32_t)n;
    e->h.cap = n;
    for (uint64_t i = 0; i < n; ++i) e->slots[i] = A_NONE;
    return mkobj(&e->h);
}

aval a_env_set(aval env, uint64_t i, aval v) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    e->slots[i] = v;
    return A_NONE;
}

aval a_env_get(aval env, uint64_t i) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    return e->slots[i];
}

AResult *result_new(int ok, int is_option, aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = ok ? 1 : 0;
    r->h.cap = 0;
    r->ok = ok ? 1u : 0u;
    r->is_option = is_option ? 1u : 0u;
    r->value = value;
    return r;
}

aval a_some_new(aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = 1;
    r->h.cap = 0;
    r->ok = 1;
    r->is_option = 1;
    r->value = value;
    return mkobj(&r->h);
}

static int result_is_option(aval v) { return ((AResult *)obj_of(v))->is_option != 0; }

int is_result(aval v) {
    AObj *o = obj_of(v);
    return o && o->type == T_RESULT;
}

int result_ok(aval v) { return ((AResult *)obj_of(v))->ok != 0; }
static aval result_value(aval v) { return ((AResult *)obj_of(v))->value; }

aval a_call(aval fn, uint64_t n, aval *argv) {
    AObj *fo = obj_of(fn);
    if (fo && fo->type == T_PYOBJ) return a_py_call(fn, n, argv);
    AFnon *f = fnon_of(fn, "call target");
    if (f->flags & FNON_BUILTIN) {
        a_builtin_fn bf = (a_builtin_fn)(uintptr_t)f->entry;
        return bf(n, argv);
    }
    if (f->nparams != NPARAMS_UNKNOWN && n != f->nparams)
        panicf("expected %llu argument(s), got %llu",
               (unsigned long long)f->nparams, (unsigned long long)n);
    uint64_t total = f->nparams == NPARAMS_UNKNOWN ? n + 1 : f->nparams + 1;
    aval *av = (aval *)xmalloc((size_t)total * sizeof(aval));
    av[0] = f->env;
    for (uint64_t i = 0; i < n; ++i) av[i + 1] = argv[i];
    for (uint64_t i = n + 1; i < total; ++i) av[i] = A_NONE;
    aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)f->entry;
    aval r = code(av);
    free(av);
    return r;
}

aval a_unwrap(aval v) {
    if (a_is_none(v)) panicf("Cannot unwrap None with '!'.");
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        panicf("called '!' on Err(...)");
    }
    return v;
}

aval a_try_result(aval v) {
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        return v; /* Err propagates as a result value */
    }
    return v;
}

