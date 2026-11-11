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

/* Decimal digits of |v| written backwards into tmp; returns the count. */
static size_t i64_digits(int64_t v, char *tmp) {
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    return n;
}

/* str + str / str + int are the two hot shapes (building log lines, keys and
   filenames): both fit one exact-size allocation, so they skip the Buf path
   whose growth reallocs and second copy dominated the measurement. */
static aval concat_str(aval strv, aval other, int str_left) {
    uint64_t sn;
    const char *sp = str_data(strv, &sn);
    char tmp[24];
    const char *tp = NULL;
    uint64_t tn = 0;
    char rev[24];
    if (kind_of(other) == K_STR) {
        tp = str_data(other, &tn);
    } else if (is_int_val(other)) {
        int64_t v = int_val_full(other);
        size_t nd = i64_digits(v, rev);
        size_t o = 0;
        if (v < 0) tmp[o++] = '-';
        while (nd) tmp[o++] = rev[--nd];
        tp = tmp;
        tn = (uint64_t)o;
    }
    if (tp) {
        if ((uint64_t)UINT32_MAX - sn < tn) panicf("string too large");
        aval outv = a_str_new_uninit(sn + tn);
        AStr *o = (AStr *)obj_of(outv);
        char *d = o->data;
        if (str_left) {
            memcpy(d, sp, (size_t)sn);
            memcpy(d + sn, tp, (size_t)tn);
        } else {
            memcpy(d, tp, (size_t)tn);
            memcpy(d + tn, sp, (size_t)sn);
        }
        return outv;
    }
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
        arr_iitems_raw(out);
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

static aval repeat_arr(aval listv, int64_t n) {
    AArr *a = (AArr *)obj_of(listv);
    int is_tuple = kind_of(listv) == K_TUPLE || a->h.type == T_TUPLE;
    uint64_t len = (uint64_t)a->h.len;
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = is_tuple ? T_TUPLE : T_LIST;
    out->iitems = NULL;
    out->flags = 0;
    if (n <= 0 || len == 0) {
        out->h.len = 0;
        out->h.cap = 0;
        out->items = NULL;
        return mkobj(&out->h);
    }
    if ((uint64_t)n > UINT64_MAX / len) panicf("repeated sequence too large");
    uint64_t total = len * (uint64_t)n;
    out->h.len = total;
    out->h.cap = total;
    if (!is_tuple && arr_packed(a)) {
        out->items = NULL;
        out->iitems = (int64_t *)xmalloc((size_t)total * sizeof(int64_t));
        arr_iitems_raw(out);
        for (uint64_t i = 0; i < (uint64_t)n; ++i)
            memcpy(out->iitems + i * len, a->iitems, (size_t)len * sizeof(int64_t));
        out->flags |= ARR_F_PACKED;
        return mkobj(&out->h);
    }
    arr_ensure_aval(a);
    out->items = (aval *)xmalloc((size_t)total * sizeof(aval));
    for (uint64_t i = 0; i < (uint64_t)n; ++i)
        memcpy(out->items + i * len, a->items, (size_t)len * sizeof(aval));
    return mkobj(&out->h);
}

// Growable byte buffer for str_format_percent.
typedef struct { char *p; size_t len, cap; } FmtBuf;
static void fmt_putc(FmtBuf *b, char c) {
    if (b->len + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p = (char *)realloc(b->p, b->cap);
    }
    b->p[b->len++] = c;
}
static void fmt_puts(FmtBuf *b, const char *s) {
    while (*s) fmt_putc(b, *s++);
}

// "%d %s" % args — Python-style printf formatting. Supports the C subset
// %[flags][width][.precision]conv for d i u x X o c s r f F e E g G %.
static aval str_format_percent(aval fmt, aval args) {
    uint64_t flen;
    const char *f = str_data(fmt, &flen);
    // Collect positional arguments.
    aval *argv = NULL;
    uint64_t argc = 0;
    if (kind_of(args) == K_TUPLE) {
        AArr *a = (AArr *)obj_of(args);
        if (a) {
            argc = a->h.len;
            argv = (aval *)xmalloc((size_t)(argc ? argc : 1) * sizeof(aval));
            for (uint64_t i = 0; i < argc; ++i) argv[i] = arr_elem(a, i);
        }
    } else {
        argv = (aval *)xmalloc(sizeof(aval));
        argv[0] = args;
        argc = 1;
    }
    FmtBuf out = { NULL, 0, 0 };
    uint64_t ai = 0;
    for (uint64_t i = 0; i < flen; ++i) {
        char c = f[i];
        if (c != '%') { fmt_putc(&out, c); continue; }
        if (i + 1 < flen && f[i + 1] == '%') { fmt_putc(&out, '%'); ++i; continue; }
        // Parse the spec: %[flags][width][.precision][length]conv
        uint64_t j = i + 1;
        char spec[64];
        uint64_t sn = 0;
        spec[sn++] = '%';
        while (j < flen && strchr("-+ #0", f[j]) && sn < 60) spec[sn++] = f[j++];
        while (j < flen && isdigit((unsigned char)f[j]) && sn < 60) spec[sn++] = f[j++];
        if (j < flen && f[j] == '.' && sn < 60) {
            spec[sn++] = f[j++];
            while (j < flen && isdigit((unsigned char)f[j]) && sn < 60) spec[sn++] = f[j++];
        }
        while (j < flen && strchr("lhLzjt", f[j])) ++j;  // skip length modifiers
        if (j >= flen) { panicf("incomplete format specifier"); }
        char conv = f[j];
        if (sn >= 60) panicf("format specifier too long");
        if (ai >= argc) panicf("not enough arguments for format string");
        aval v = argv[ai++];
        char numbuf[512];
        switch (conv) {
            case 'd': case 'i': {
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = 'd'; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, (long long)int_val_full(v));
                break;
            }
            case 'u': {
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = 'u'; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, (unsigned long long)int_val_full(v));
                break;
            }
            case 'x': case 'X': case 'o': {
                char lc = conv == 'X' ? 'X' : conv;
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = lc; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, (unsigned long long)int_val_full(v));
                break;
            }
            case 'c': {
                spec[sn++] = 'c'; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, (int)int_val_full(v));
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                spec[sn++] = conv; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, num_to_double(v));
                break;
            }
            case 's': {
                aval sv = a_str(v);
                uint64_t vlen;
                const char *vs = str_data(sv, &vlen);
                char *tmp = (char *)xmalloc(vlen + 1);
                memcpy(tmp, vs, vlen);
                tmp[vlen] = 0;
                spec[sn++] = 's'; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, tmp);
                free(tmp);
                break;
            }
            case 'r': {
                aval rs = a_repr(v);
                uint64_t rlen;
                const char *rsdata = str_data(rs, &rlen);
                char *tmp = (char *)xmalloc(rlen + 1);
                memcpy(tmp, rsdata, rlen);
                tmp[rlen] = 0;
                spec[sn++] = 's'; spec[sn] = 0;
                snprintf(numbuf, sizeof(numbuf), spec, tmp);
                free(tmp);
                break;
            }
            default:
                panicf("unsupported format specifier");
        }
        fmt_puts(&out, numbuf);
        i = j;
    }
    if (ai < argc && argc > 1) panicf("not all arguments converted during string formatting");
    free(argv);
    aval result = a_str_new(out.p ? out.p : "", out.len);
    free(out.p);
    return result;
}

aval binary_arith(aval l, aval r, char op, const char *opname) {
    if (op == '%' && kind_of(l) == K_STR) return str_format_percent(l, r);
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
        if ((kind_of(l) == K_LIST || kind_of(l) == K_TUPLE) && ri)
            return repeat_arr(l, int_val_full(r));
        if ((kind_of(r) == K_LIST || kind_of(r) == K_TUPLE) && li)
            return repeat_arr(r, int_val_full(l));
    }
    if (li && ri) return arith_int_int(l, r, op);
    panic_binop(opname, l, r);
    return A_NONE;
}

aval a_add_slow(aval l, aval r) { return binary_arith(l, r, '+', "+"); }
aval a_sub_slow(aval l, aval r) { return binary_arith(l, r, '-', "-"); }
aval a_mul_slow(aval l, aval r) { return binary_arith(l, r, '*', "*"); }
aval a_div_slow(aval l, aval r) { return binary_arith(l, r, '/', "/"); }
aval a_floordiv_slow(aval l, aval r) {
    int li = is_int_val(l), ri = is_int_val(r);
    if (li && ri) {
        int64_t a = int_val_full(l), b = int_val_full(r);
        if (b == 0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
        if (a == INT64_MIN && b == -1) return mkdouble(-9223372036854775808.0);
        int64_t q = a / b;
        if (a % b != 0 && ((a < 0) != (b < 0))) --q;
        return mkint_full(q);
    }
    int ld = a_is_double(l), rd = a_is_double(r);
    if (ld || rd) {
        double a = num_to_double(l), b = num_to_double(r);
        if (b == 0.0) panicf("Division by zero.\n  = help: Check the divisor before dividing.");
        return mkdouble(floor(a / b));
    }
    panic_binop("//", l, r);
    return A_NONE;
}
aval a_mod_slow(aval l, aval r) { return binary_arith(l, r, '%', "%"); }
aval a_pow(aval l, aval r) { return binary_arith(l, r, '^', "**"); }

