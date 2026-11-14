/* Exported fast-path entry points for non-C callers (the LLVM backend).
 *
 * a_runtime.h already provides the hot arithmetic/comparison logic as
 * `static inline` functions (a_add, a_sub, a_mul, ...), but static inlines
 * are not callable from generated IR. These `*_fast` exports run the exact
 * same checks in C and tail-call the `*_slow` variants for every case the
 * inline paths would have deferred, so semantics are identical to the
 * header fast paths (which the asm backend and C consumers already use).
 *
 * Where the header has no inline fast path (floordiv, pow) the fast entry
 * replicates the corresponding `*_slow` implementation's number cases and
 * defers everything else.
 */
#include "a_runtime_internal.h"

/* int64 multiply with overflow check (same algorithm as imul_of in
 * value_arith.c; kept local because that one is file-static). */
static uint64_t fp_iabs_u(int64_t x) {
    return x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
}

static int fp_imul_of(int64_t a, int64_t b, int64_t *r) {
    if (a == 0 || b == 0) { *r = 0; return 0; }
    uint64_t ua = fp_iabs_u(a), ub = fp_iabs_u(b);
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

/* "numeric" here means an INLINE int (a_is_int, 47-bit) or a plain double
 * (a_is_double). Boxed T_INT64 ints and every heap object fall through to
 * the slow paths, exactly like the header inline fast paths. */
static int fp_is_num(aval v) { return a_is_int(v) || a_is_double(v); }
static double fp_num(aval v) {
    return a_is_int(v) ? (double)a_int_val(v) : a_double_val(v);
}

/* ------------------------------------------------------------------ */
/* arithmetic                                                          */
/* ------------------------------------------------------------------ */

aval a_add_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r), res;
        if (!((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))) {
            res = a + b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_add_slow(l, r);
    }
    /* Both-double (header path) and mixed int/double (slow path's
       arith_int_double) reduce to the same double add. */
    if (fp_is_num(l) && fp_is_num(r))
        return mkdouble(fp_num(l) + fp_num(r));
    return a_add_slow(l, r);
}

aval a_sub_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r), res;
        if (!((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))) {
            res = a - b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_sub_slow(l, r);
    }
    if (fp_is_num(l) && fp_is_num(r))
        return mkdouble(fp_num(l) - fp_num(r));
    return a_sub_slow(l, r);
}

aval a_mul_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (a == 0 || b == 0) return a_mkint(0);
        int64_t ub = b < 0 ? -b : b;
        int64_t ua = a < 0 ? -a : a;
        if (ua <= A_INT_LIM / ub) {
            int64_t res = a * b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_mul_slow(l, r);
    }
    if (fp_is_num(l) && fp_is_num(r))
        return mkdouble(fp_num(l) * fp_num(r));
    return a_mul_slow(l, r);
}

aval a_div_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b != 0 && !(a == INT64_MIN && b == -1)) {
            int64_t res = a / b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_div_slow(l, r);
    }
    /* Double division (incl. mixed int/double): division by zero.0 must
       panic via the slow path, not produce an IEEE infinity. */
    if (fp_is_num(l) && fp_is_num(r)) {
        double b = fp_num(r);
        if (b == 0.0) return a_div_slow(l, r);
        return mkdouble(fp_num(l) / b);
    }
    return a_div_slow(l, r);
}

aval a_floordiv_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b != 0) {
            int64_t q = a / b;
            if (a % b != 0 && ((a < 0) != (b < 0))) --q;
            if (a_in_inline_range(q)) return a_mkint(q);
        }
        return a_floordiv_slow(l, r);
    }
    if (fp_is_num(l) && fp_is_num(r)) {
        double b = fp_num(r);
        if (b == 0.0) return a_floordiv_slow(l, r);
        return mkdouble(floor(fp_num(l) / b));
    }
    return a_floordiv_slow(l, r);
}

aval a_mod_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b != 0 && b != -1) {
            int64_t res = a % b;
            if (a_in_inline_range(res)) return a_mkint(res);
        } else if (b == -1) {
            return a_mkint(0);
        }
        return a_mod_slow(l, r);
    }
    /* float % : fmod with the divisor's sign (arith_int_double '%'). */
    if (fp_is_num(l) && fp_is_num(r)) {
        double a = fp_num(l), b = fp_num(r);
        if (b == 0.0) return a_mod_slow(l, r);
        double m = fmod(a, b);
        if (m != 0.0 && ((m > 0.0) != (b > 0.0))) m += b;
        return mkdouble(m);
    }
    return a_mod_slow(l, r);
}

aval a_pow_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        /* Same algorithm as arith_int_int '^' in value_arith.c. */
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b < 0) return mkdouble(pow((double)a, (double)b));
        if (a == 0 && b == 0) return a_mkint(1);
        if (a == 1) return a_mkint(1);
        if (a == -1) return a_mkint((b & 1) ? -1 : 1);
        {
            int64_t base = a, e = b, power = 1;
            while (e > 0) {
                if (e & 1) {
                    if (fp_imul_of(power, base, &power))
                        return mkdouble(pow((double)a, (double)b));
                }
                e >>= 1;
                if (e > 0 && fp_imul_of(base, base, &base))
                    return mkdouble(pow((double)a, (double)b));
            }
            return mkint_full(power);
        }
    }
    if (fp_is_num(l) && fp_is_num(r))
        return mkdouble(pow(fp_num(l), fp_num(r)));
    return a_pow(l, r);
}

/* ------------------------------------------------------------------ */
/* comparisons                                                         */
/* ------------------------------------------------------------------ */

aval a_eq_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) == a_int_val(r));
    if (a_is_double(l) && a_is_double(r))
        return a_bool_inline(a_double_val(l) == a_double_val(r));
    if (l == r && (l == A_TRUE || l == A_FALSE || l == A_NONE))
        return A_TRUE;
    return a_eq_slow(l, r);
}

aval a_ne_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) != a_int_val(r));
    if (a_is_double(l) && a_is_double(r))
        return a_bool_inline(a_double_val(l) != a_double_val(r));
    if (l == r && (l == A_TRUE || l == A_FALSE || l == A_NONE))
        return A_FALSE;
    return a_ne_slow(l, r);
}

/* Mixed int/double ordering comparisons reduce to a double compare
   (header a_lt/a_le/a_gt/a_ge fast path); inline ints are < 2^53 so the
   conversion is exact. */
#define FP_CMP_NUM(l, r) \
    (((a_is_int(l) || a_is_double(l)) && (a_is_int(r) || a_is_double(r))))

aval a_lt_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) < a_int_val(r));
    if (FP_CMP_NUM(l, r))
        return a_bool_inline(fp_num(l) < fp_num(r));
    return a_lt_slow(l, r);
}

aval a_le_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) <= a_int_val(r));
    if (FP_CMP_NUM(l, r))
        return a_bool_inline(fp_num(l) <= fp_num(r));
    return a_le_slow(l, r);
}

aval a_gt_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) > a_int_val(r));
    if (FP_CMP_NUM(l, r))
        return a_bool_inline(fp_num(l) > fp_num(r));
    return a_gt_slow(l, r);
}

aval a_ge_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) >= a_int_val(r));
    if (FP_CMP_NUM(l, r))
        return a_bool_inline(fp_num(l) >= fp_num(r));
    return a_ge_slow(l, r);
}

/* ------------------------------------------------------------------ */
/* unary / bitwise / truthiness                                        */
/* ------------------------------------------------------------------ */

aval a_neg_fast(aval v) {
    if (a_is_int(v)) {
        int64_t i = a_int_val(v);
        if (i != -A_INT_LIM) return a_mkint(-i);
    }
    if (a_is_double(v)) return mkdouble(-a_double_val(v));
    return a_neg_slow(v);
}

aval a_bnot_fast(aval v) {
    if (a_is_int(v)) {
        int64_t r = ~a_int_val(v);
        if (a_in_inline_range(r)) return a_mkint(r);
    }
    return a_bnot_slow(v);
}

aval a_not_fast(aval v) {
    if (a_is_int(v)) return a_bool_inline(a_int_val(v) == 0);
    if (a_is_double(v)) return a_bool_inline(a_double_val(v) == 0.0);
    if (v == A_NONE || v == A_FALSE) return A_TRUE;
    if (v == A_TRUE) return A_FALSE;
    return a_not(v);
}

aval a_truthy_raw_fast(aval v) {
    if (a_is_int(v)) return (aval)(a_int_val(v) != 0);
    if (a_is_double(v)) return (aval)(a_double_val(v) != 0.0);
    if (v == A_NONE) return (aval)0;
    if (v == A_TRUE) return (aval)1;
    if (v == A_FALSE) return (aval)0;
    return a_truthy_raw_slow(v);
}

aval a_band_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) & a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_band_slow(l, r);
}

aval a_bor_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) | a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_bor_slow(l, r);
}

aval a_bxor_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) ^ a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_bxor_slow(l, r);
}

aval a_shl_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t s = a_int_val(r);
        if (s >= 0 && s <= 63) {
            int64_t v = (int64_t)((uint64_t)a_int_val(l) << s);
            if (a_in_inline_range(v)) return a_mkint(v);
        }
    }
    return a_shl_slow(l, r);
}

aval a_shr_fast(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t s = a_int_val(r);
        if (s >= 0 && s <= 63) {
            int64_t v = a_int_val(l) >> s;
            if (a_in_inline_range(v)) return a_mkint(v);
        }
    }
    return a_shr_slow(l, r);
}
