/* math_ew.c — native kernels for std.math elementwise ufuncs and flat
 * reductions over [f64] list storage.
 *
 * These mirror the A-side helpers in stdlib/std/math/ufuncs.astra
 * (_un_scalar/_bin_scalar, integer opcodes assigned by _un_opi/_bin_opi)
 * and reductions.astra (_reduce_flat, opcodes from _ropi) bit-for-bit:
 * same libm calls, same guard order, same NaN/inf edges. The A versions
 * exist so broadcasting and non-list inputs still work; the fast paths
 * in ufuncs.astra/reductions.astra call these builtins whenever the
 * operand is a flat [f64] buffer.
 *
 * Elementwise kernels are embarrassingly parallel: each worker owns a
 * disjoint output range and runs the identical per-element instruction
 * sequence, so results are bit-identical to the serial loop (same scheme
 * as linalg.c). Reductions stay serial to preserve FP accumulation
 * order — at C speed a serial pass over even 10^7 elements is sub-ms.
 */
#include "a_runtime_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* list <-> double view helpers (same scheme as linalg.c)              */
/* ------------------------------------------------------------------ */

static AArr *as_arr(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_LIST) panicf("%s must be a list", what);
    return (AArr *)o;
}

static inline double arr_num(const AArr *a, uint64_t i) {
    if (arr_packed(a)) return (double)a->iitems[i];
    aval v = a->items[i];
    if (a_is_double(v)) {
        union { uint64_t u; double d; } c;
        c.u = v;
        return c.d;
    }
    if (is_int_val(v)) return (double)int_val_full(v);
    panicf("math ufuncs expect numeric arrays");
    return 0.0;
}

/* Verified all-double aval storage -> raw double view (caches ARR_F_F64). */
static const double *f64_ptr(AArr *a) {
    if (arr_packed(a) || !a->items) return NULL;
    if (!(a->flags & ARR_F_F64)) {
        for (uint64_t i = 0; i < a->h.len; i++)
            if (!a_is_double(a->items[i])) return NULL;
        arr_f64_mark(a);   /* items is raw double bits: exempt from GC scan */
    }
    return (const double *)(const void *)a->items;
}

static double *to_f64(const AArr *a, uint64_t n, const char *what) {
    if ((uint64_t)a->h.len < n) panicf("%s too short", what);
    double *out = (double *)malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!out) panicf("out of memory");
    for (uint64_t i = 0; i < n; i++) out[i] = arr_num(a, i);
    return out;
}

/* Raw double view of the first `need` elements; *tmp gets a malloc'd copy
   when the list is not already verified all-double (caller frees). */
static const double *as_f64(const AArr *a, uint64_t need, double **tmp,
                            const char *what) {
    *tmp = NULL;
    if ((uint64_t)a->h.len < need) panicf("%s too short", what);
    const double *view = f64_ptr((AArr *)a);
    if (view) return view;
    *tmp = to_f64(a, need, what);
    return *tmp;
}

/* New [f64] list (ARR_F_F64-marked) from a double buffer. */
static aval f64_list(uint64_t n, const double *src) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
    a->iitems = NULL;
    a->flags = ARR_F_F64;
    a_gc_set_noscan(a->items, 1);   /* all-double items: no pointers inside */
    for (uint64_t i = 0; i < n; i++) a->items[i] = mkdouble(src[i]);
    return mkobj(a);
}

/* ------------------------------------------------------------------ */
/* scalar kernels — exact ports of _un_scalar/_bin_scalar/_reduce_flat  */
/* ------------------------------------------------------------------ */

#define EW_NAN() (NAN)
#define EW_PINF() (HUGE_VAL)

static double ew_cs(double x, double y) {
    /* _cs: magnitude of x with the sign of y (incl. -0.0). NaN inputs are
       filtered by the caller before this runs. */
    return copysign(x, y);
}

static double ew_div(double x, double y) {
    /* _div: IEEE division; C '/' already yields +-inf/NaN exactly like the
       A-side manual path (which exists only because A '/' panics on 0). */
    if (y == 0.0) {
        if (x == 0.0 || x != x) return EW_NAN();
        return ew_cs(EW_PINF(), x) * ew_cs(1.0, y) * EW_PINF();
    }
    double r = x / y;
    if (r != r) return EW_NAN();
    return r;
}

static double ew_rem(double x, double y) {
    /* _rem: numpy remainder — x - floor(x/y)*y, sign of divisor. */
    if (y == 0.0 || x != x || y != y || fabs(x) == EW_PINF()) return EW_NAN();
    if (fabs(y) == EW_PINF()) return x;
    double q = x / y;
    if (q != q || fabs(q) == EW_PINF()) return EW_NAN();
    double r = x - floor(q) * y;
    if (r != r) return EW_NAN();
    return r;
}

static double ew_fmod(double x, double y) {
    /* _fmod: C fmod semantics via x - trunc(x/y)*y (NOT libm fmod — the A
       version gives up to NaN on quotient overflow instead). */
    if (y == 0.0 || x != x || y != y || fabs(x) == EW_PINF()) return EW_NAN();
    if (fabs(y) == EW_PINF()) return x;
    double q = x / y;
    if (q != q || fabs(q) == EW_PINF()) return EW_NAN();
    double r = x - trunc(q) * y;
    if (r != r) return EW_NAN();
    return r;
}

static double ew_floordiv(double x, double y) {
    /* _floordiv: floor(x/y) with explicit non-finite handling. */
    if (y == 0.0 || x != x || y != y) return EW_NAN();
    int xinf = fabs(x) == EW_PINF();
    int yinf = fabs(y) == EW_PINF();
    if (xinf && yinf) return EW_NAN();
    if (xinf) return ew_cs(EW_PINF(), x) * ew_cs(1.0, y);
    if (yinf) return floor(x / y);   /* x/y = +-0 -> floor -> +-0 */
    double q = x / y;
    if (q != q || fabs(q) == EW_PINF())
        return ew_cs(EW_PINF(), x) * ew_cs(1.0, y);
    return floor(q);
}

static double ew_hypot(double x, double y) {
    /* _hypot: overflow-safe sqrt(x*x+y*y) via scaling (same formula as A). */
    if (x != x || y != y) return EW_NAN();
    double ax = fabs(x), ay = fabs(y);
    double m = ax > ay ? ax : ay;
    if (m == 0.0) return 0.0;
    if (m == EW_PINF()) return EW_PINF();
    double rx = ax / m, ry = ay / m;
    return m * sqrt(rx * rx + ry * ry);
}

static double ew_pow(double x, double y) {
    /* _pow: math.pow on f64 operands is libm pow. */
    if (x != x || y != y) return EW_NAN();
    return pow(x, y);
}

static double ew_sinh(double x) {
    if (fabs(x) == EW_PINF()) return ew_cs(EW_PINF(), x);
    return (exp(x) - exp(-x)) / 2.0;
}

static double ew_cosh(double x) {
    if (fabs(x) == EW_PINF()) return EW_PINF();
    return (exp(x) + exp(-x)) / 2.0;
}

static double ew_tanh(double x) {
    if (fabs(x) == EW_PINF()) return ew_cs(1.0, x);
    double e = exp(-2.0 * fabs(x));
    double t = (1.0 - e) / (1.0 + e);
    if (x < 0.0) return -t;
    return t;
}

static double ew_asinh(double x) {
    double ax = fabs(x);
    if (ax == EW_PINF()) return ew_cs(EW_PINF(), x);
    if (ax > 1e150)
        return ew_cs(log(ax) + 0.6931471805599453, x);
    double r = log(ax + sqrt(ax * ax + 1.0));
    if (x < 0.0) return -r;
    return r;
}

static double ew_acosh(double x) {
    if (x < 1.0) {
        if (x != x) return x;
        return EW_NAN();
    }
    if (x == 1.0) return 0.0;
    if (x == EW_PINF()) return EW_PINF();
    return log(x + sqrt(x - 1.0) * sqrt(x + 1.0));
}

static double ew_atanh(double x) {
    double ax = fabs(x);
    if (ax > 1.0) return EW_NAN();
    if (ax == 1.0) return ew_cs(EW_PINF(), x);
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

static double ew_expm1(double x) {
    /* _expm1: series for small |x| (same scheme as the A version). */
    double ax = fabs(x);
    if (ax < 0.001) {
        double x2 = x * x;
        return x + x2 / 2.0 + x2 * x / 6.0;
    }
    return exp(x) - 1.0;
}

static double ew_log1p(double x) {
    /* _log1p: series for small |x|. */
    double ax = fabs(x);
    if (ax < 0.001) {
        double x2 = x * x;
        return x - x2 / 2.0 + x2 * x / 3.0 - x2 * x2 / 4.0;
    }
    return log(1.0 + x);
}

static double ew_log_guarded(double x, int base2, int base10) {
    /* _log_guarded: log/log2/log10 with IEEE domain handling. */
    if (x == 0.0) return -EW_PINF();
    if (x < 0.0) return EW_NAN();
    if (base2) return log2(x);
    if (base10) return log10(x);
    return log(x);
}

static double ew_rint(double x) {
    /* _rint: round half to even via floor decomposition (same ops as A —
       libm rint() would give -0.0 where A gives +0.0 for x = -0.5 etc.). */
    double f = floor(x);
    double d = x - f;
    if (d < 0.5) return f;
    if (d > 0.5) return f + 1.0;
    double h = floor(f / 2.0);
    if (f == h * 2.0) return f;
    return f + 1.0;
}

static double ew_sinc(double x) {
    /* _sinc: sin(pi x) / (pi x), 1 at x == 0. */
    if (fabs(x) == EW_PINF()) return EW_NAN();
    double t = x * 3.141592653589793;
    if (t == 0.0) return 1.0;
    if (fabs(t) == EW_PINF()) return EW_NAN();
    return sin(t) / t;
}

/* _un_scalar(opi, v): opcodes from _un_opi in ufuncs.astra. */
static double ew_un_scalar(int64_t opi, double v) {
    int nan = (v != v);
    if (opi == 1) return nan ? 1.0 : 0.0;              /* isnan */
    if (opi == 2) {                                    /* isinf */
        if (nan) return 0.0;
        return fabs(v) == EW_PINF() ? 1.0 : 0.0;
    }
    if (opi == 3) {                                    /* isfinite */
        if (nan) return 0.0;
        return fabs(v) == EW_PINF() ? 0.0 : 1.0;
    }
    if (opi == 4) {                                    /* signbit */
        if (nan) return 0.0;
        /* v < 0 -> 1; v == +-0 -> sign of zero; else 0 — identical to
           signbit() for every non-NaN double. */
        return signbit(v) ? 1.0 : 0.0;
    }
    if (nan) return EW_NAN();
    int inf = (fabs(v) == EW_PINF());
    switch (opi) {
    case 5:  return -v;                                /* neg */
    case 6:  return v;                                 /* pos */
    case 7:  return fabs(v);                           /* abs */
    case 8:  return v < 0.0 ? EW_NAN() : sqrt(v);      /* sqrt */
    case 9:  return v * v;                             /* square */
    case 10: return inf ? EW_NAN() : sin(v);
    case 11: return inf ? EW_NAN() : cos(v);
    case 12: return inf ? EW_NAN() : tan(v);
    case 13: return (inf || fabs(v) > 1.0) ? EW_NAN() : asin(v);
    case 14: return (inf || fabs(v) > 1.0) ? EW_NAN() : acos(v);
    case 15: return atan(v);
    case 16: return ew_sinh(v);
    case 17: return ew_cosh(v);
    case 18: return ew_tanh(v);
    case 19: return ew_asinh(v);
    case 20: return ew_acosh(v);
    case 21: return ew_atanh(v);
    case 22: return exp(v);
    case 23: return pow(2.0, v);                       /* exp2 */
    case 24: return ew_expm1(v);
    case 25: return ew_log_guarded(v, 0, 0);
    case 26: return ew_log_guarded(v, 1, 0);
    case 27: return ew_log_guarded(v, 0, 1);
    case 28:
        if (v <= -1.0) return v == -1.0 ? -EW_PINF() : EW_NAN();
        return ew_log1p(v);
    case 29: return inf ? v : floor(v);
    case 30: return inf ? v : ceil(v);
    case 31: return inf ? v : trunc(v);                /* trunc/fix */
    case 32: return inf ? v : ew_rint(v);
    case 33:                                            /* sign */
        if (v > 0.0) return 1.0;
        if (v < 0.0) return -1.0;
        return v;
    case 34:                                            /* reciprocal */
        if (v == 0.0) return ew_cs(EW_PINF(), v);
        return 1.0 / v;
    case 35: return ew_sinc(v);
    case 36: return v * 0.017453292519943295;          /* deg2rad */
    case 37: return v * 57.29577951308232;              /* rad2deg */
    }
    return v;
}

/* _bin_scalar(opi, x, y): opcodes from _bin_opi in ufuncs.astra. */
static double ew_bin_scalar(int64_t opi, double x, double y) {
    int xnan = (x != x), ynan = (y != y);
    if (xnan || ynan) {
        if (opi == 12 || opi == 13) {                   /* fmax/fmin */
            if (xnan) {
                if (ynan) return EW_NAN();
                return y;
            }
            return x;
        }
        return EW_NAN();                                /* NaN propagates */
    }
    switch (opi) {
    case 1:  return x + y;                             /* add */
    case 2:  return x - y;                             /* sub */
    case 3:  return x * y;                             /* mul */
    case 4:  return ew_div(x, y);
    case 5:  return ew_pow(x, y);
    case 6:  return ew_rem(x, y);                      /* rem/mod */
    case 7:  return ew_fmod(x, y);
    case 8:  return ew_floordiv(x, y);
    case 9:  return ew_hypot(x, y);
    case 10:                                            /* maximum */
    case 12:                                            /* fmax (NaN handled) */
        return x > y ? x : y;
    case 11:                                            /* minimum */
    case 13:                                            /* fmin (NaN handled) */
        return x < y ? x : y;
    case 14: return ew_cs(x, y);                       /* copysign */
    case 15: return atan2(x, y);
    }
    return x;
}

/* ------------------------------------------------------------------ */
/* elementwise kernels (parallel over disjoint output ranges)          */
/* ------------------------------------------------------------------ */

#define EW_PAR_MIN ((int64_t)1 << 20)
#define EW_PAR_GRAIN ((int64_t)1 << 16)

static int ew_par_ok(uint64_t n) {
    return (int64_t)n >= EW_PAR_MIN && a_par_workers() > 1;
}

typedef struct {
    const double *x;
    double *out;
    int64_t opi;
} AEWUnCtx;

static void ew_un_rows(void *vctx, int64_t lo, int64_t hi) {
    AEWUnCtx *c = (AEWUnCtx *)vctx;
    for (int64_t i = lo; i < hi; i++) c->out[i] = ew_un_scalar(c->opi, c->x[i]);
}

typedef struct {
    const double *x, *y;
    double *out;
    int64_t opi;
} AEWBinCtx;

static void ew_bin_rows(void *vctx, int64_t lo, int64_t hi) {
    AEWBinCtx *c = (AEWBinCtx *)vctx;
    for (int64_t i = lo; i < hi; i++)
        c->out[i] = ew_bin_scalar(c->opi, c->x[i], c->y[i]);
}

typedef struct {
    const double *x;
    double *out;
    int64_t opi;
    double s;
    int left;      /* scalar on the left: op(s, x[i]) */
} AEWBSCtx;

static void ew_bsc_rows(void *vctx, int64_t lo, int64_t hi) {
    AEWBSCtx *c = (AEWBSCtx *)vctx;
    if (c->left) {
        for (int64_t i = lo; i < hi; i++)
            c->out[i] = ew_bin_scalar(c->opi, c->s, c->x[i]);
    } else {
        for (int64_t i = lo; i < hi; i++)
            c->out[i] = ew_bin_scalar(c->opi, c->x[i], c->s);
    }
}

/* bi_math_ew_un(opi, list) -> [f64] : elementwise unary ufunc. */
aval bi_math_ew_un(uint64_t n, aval *argv) {
    want_args("math_ew_un", n, 2, 2);
    int64_t opi = expect_int64(argv[0], "math_ew_un op");
    AArr *a = as_arr(argv[1], "math_ew_un() input");
    uint64_t len = a->h.len;
    double *tmp = NULL;
    const double *x = as_f64(a, len, &tmp, "math_ew_un() input");
    double *out = (double *)malloc((size_t)(len ? len : 1) * sizeof(double));
    if (!out) panicf("out of memory");
    AEWUnCtx c = { x, out, opi };
    if (ew_par_ok(len)) a_par_for((int64_t)len, EW_PAR_GRAIN, ew_un_rows, &c);
    else for (uint64_t i = 0; i < len; i++) out[i] = ew_un_scalar(opi, x[i]);
    free(tmp);
    aval r = f64_list(len, out);
    free(out);
    return r;
}

/* bi_math_ew_bin(opi, listA, listB) -> [f64] : elementwise binary ufunc,
   equal lengths required (broadcasting stays in A). */
aval bi_math_ew_bin(uint64_t n, aval *argv) {
    want_args("math_ew_bin", n, 3, 3);
    int64_t opi = expect_int64(argv[0], "math_ew_bin op");
    AArr *aa = as_arr(argv[1], "math_ew_bin() first argument");
    AArr *bb = as_arr(argv[2], "math_ew_bin() second argument");
    uint64_t len = aa->h.len;
    if (bb->h.len != len)
        panicf("math_ew_bin: length mismatch (%llu vs %llu)",
               (unsigned long long)len, (unsigned long long)bb->h.len);
    double *ta = NULL, *tb = NULL;
    const double *x = as_f64(aa, len, &ta, "math_ew_bin() first argument");
    const double *y = as_f64(bb, len, &tb, "math_ew_bin() second argument");
    double *out = (double *)malloc((size_t)(len ? len : 1) * sizeof(double));
    if (!out) panicf("out of memory");
    AEWBinCtx c = { x, y, out, opi };
    if (ew_par_ok(len)) a_par_for((int64_t)len, EW_PAR_GRAIN, ew_bin_rows, &c);
    else for (uint64_t i = 0; i < len; i++)
        out[i] = ew_bin_scalar(opi, x[i], y[i]);
    free(ta);
    free(tb);
    aval r = f64_list(len, out);
    free(out);
    return r;
}

/* bi_math_ew_bin_scalar(opi, list, scalar, scalar_left) -> [f64] :
   elementwise binary ufunc against one broadcast scalar. */
aval bi_math_ew_bin_scalar(uint64_t n, aval *argv) {
    want_args("math_ew_bin_scalar", n, 4, 4);
    int64_t opi = expect_int64(argv[0], "math_ew_bin_scalar op");
    AArr *a = as_arr(argv[1], "math_ew_bin_scalar() input");
    double s = num_to_double(argv[2]);
    int left = a_truthy_raw(argv[3]) != 0;
    uint64_t len = a->h.len;
    double *tmp = NULL;
    const double *x = as_f64(a, len, &tmp, "math_ew_bin_scalar() input");
    double *out = (double *)malloc((size_t)(len ? len : 1) * sizeof(double));
    if (!out) panicf("out of memory");
    AEWBSCtx c = { x, out, opi, s, left };
    if (ew_par_ok(len)) a_par_for((int64_t)len, EW_PAR_GRAIN, ew_bsc_rows, &c);
    else if (left) for (uint64_t i = 0; i < len; i++)
        out[i] = ew_bin_scalar(opi, s, x[i]);
    else for (uint64_t i = 0; i < len; i++)
        out[i] = ew_bin_scalar(opi, x[i], s);
    free(tmp);
    aval r = f64_list(len, out);
    free(out);
    return r;
}

/* ------------------------------------------------------------------ */
/* flat reduction — serial, exact port of _reduce_flat                */
/* ------------------------------------------------------------------ */

/* bi_math_ew_reduce(opi, list, skip_nan) -> f64
   opi: 1=prod, 2=min, 3=max, 4=mean, 5=var, 6=std, else=sum.
   skip_nan: NaN elements are excluded (and excluded from counts). */
aval bi_math_ew_reduce(uint64_t n, aval *argv) {
    want_args("math_ew_reduce", n, 3, 3);
    int64_t opi = expect_int64(argv[0], "math_ew_reduce op");
    AArr *a = as_arr(argv[1], "math_ew_reduce() input");
    int skip = a_truthy_raw(argv[2]) != 0;
    uint64_t len = a->h.len;
    double *tmp = NULL;
    const double *x = as_f64(a, len, &tmp, "math_ew_reduce() input");

    double rs = 0.0, acc = 0.0;
    int64_t cnt = 0;
    for (uint64_t i = 0; i < len; i++) {
        double v = x[i];
        if (skip && v != v) continue;
        cnt++;
        if (opi == 1) {
            if (cnt == 1) acc = v;
            else acc = acc * v;
        } else if (opi == 2) {
            if (cnt == 1 || v < acc) acc = v;
        } else if (opi == 3) {
            if (cnt == 1 || v > acc) acc = v;
        } else {
            rs = rs + v;
        }
    }

    double res;
    if (opi == 1) {
        res = cnt == 0 ? 1.0 : acc;
    } else if (opi == 2 || opi == 3) {
        res = cnt == 0 ? EW_NAN() : acc;
    } else if (opi == 4) {
        res = cnt == 0 ? EW_NAN() : rs / (double)cnt;
    } else if (opi == 5 || opi == 6) {
        if (cnt == 0) {
            res = EW_NAN();
        } else {
            double m = rs / (double)cnt;
            double rss = 0.0;
            for (uint64_t i = 0; i < len; i++) {
                double v = x[i];
                if (skip && v != v) continue;
                double d = v - m;
                rss = rss + d * d;
            }
            double var = rss / (double)cnt;
            res = opi == 5 ? var : sqrt(var);
        }
    } else {
        res = rs;
    }
    free(tmp);
    return mkdouble(res);
}
