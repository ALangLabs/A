/* linalg.c — dense numeric kernels over list storage.
 *
 * Float list elements are aval words whose payload is the IEEE-754 double
 * bits. Once an aval-mode list has been verified all-double we mark it
 * ARR_F_F64 and hot kernels can read AArr.items as double* directly.
 * Packed integer lists (AArr.iitems) and mixed numeric lists still work
 * through arr_num(), so the fast path never changes semantics.
 */
#include "a_runtime_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static AArr *as_arr(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_LIST) panicf("%s must be a list", what);
    return (AArr *)o;
}

static AArr *as_arr_opt(aval v, const char *what) {
    if (a_is_none(v)) return NULL;
    return as_arr(v, what);
}

static inline double aval_num(aval v) {
    if (a_is_double(v)) {
        union { uint64_t u; double d; } c;
        c.u = v;
        return c.d;
    }
    if (is_int_val(v)) return (double)int_val_full(v);
    panicf("linalg expects numeric arrays");
    return 0.0;
}

static inline double arr_num(const AArr *a, uint64_t i) {
    if (arr_packed(a)) return (double)a->iitems[i];
    return aval_num(a->items[i]);
}

/* Verified all-double aval storage -> raw double view. Scans once, then
   caches ARR_F_F64 on the array; mutating writes clear the flag. */
static const double *f64_ptr(AArr *a) {
    if (arr_packed(a) || !a->items) return NULL;
    if (!(a->flags & ARR_F_F64)) {
        for (uint64_t i = 0; i < a->h.len; i++)
            if (!a_is_double(a->items[i])) return NULL;
        a->flags |= ARR_F_F64;
    }
    return (const double *)(const void *)a->items;
}

static double *to_f64(const AArr *a, uint64_t n, const char *what) {
    if ((uint64_t)a->h.len < n) panicf("%s too short", what);
    double *out = (double *)malloc((size_t)n * sizeof(double));
    if (!out) panicf("out of memory");
    for (uint64_t i = 0; i < n; i++) out[i] = arr_num(a, i);
    return out;
}

static aval f64_list(uint64_t n, const double *src) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
    a->iitems = NULL;
    a->flags = ARR_F_F64;
    for (uint64_t i = 0; i < n; i++) a->items[i] = mkdouble(src[i]);
    return mkobj(a);
}

/* dot(a, b) -> float : sum_i a[i]*b[i] over min(len) */
aval bi_linalg_dot(uint64_t n, aval *argv) {
    want_args("linalg_dot", n, 2, 2);
    AArr *aa = as_arr(argv[0], "dot() first argument");
    AArr *bb = as_arr(argv[1], "dot() second argument");
    uint64_t na = aa->h.len, nb = bb->h.len;
    uint64_t m = na < nb ? na : nb;
    const double *ad = f64_ptr(aa), *bd = f64_ptr(bb);
    double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
    uint64_t i = 0;
    if (ad && bd) {
        for (; i + 4 <= m; i += 4) {
            acc0 += ad[i] * bd[i];
            acc1 += ad[i + 1] * bd[i + 1];
            acc2 += ad[i + 2] * bd[i + 2];
            acc3 += ad[i + 3] * bd[i + 3];
        }
        for (; i < m; i++) acc0 += ad[i] * bd[i];
    } else {
        for (; i + 4 <= m; i += 4) {
            acc0 += arr_num(aa, i) * arr_num(bb, i);
            acc1 += arr_num(aa, i + 1) * arr_num(bb, i + 1);
            acc2 += arr_num(aa, i + 2) * arr_num(bb, i + 2);
            acc3 += arr_num(aa, i + 3) * arr_num(bb, i + 3);
        }
        for (; i < m; i++) acc0 += arr_num(aa, i) * arr_num(bb, i);
    }
    return mkdouble((acc0 + acc1) + (acc2 + acc3));
}

/* matvec(x, w, bias, nin, nout) -> list
 * w is stored [in][out] (safetensors Conv1D layout):
 *   out[j] = bias[j] + sum_i x[i] * w[i*nout + j]
 * Sequential w reads, `out` stays cache-hot. */
aval bi_linalg_matvec(uint64_t n, aval *argv) {
    want_args("linalg_matvec", n, 5, 5);
    AArr *xa = as_arr(argv[0], "matvec() vector");
    AArr *wa = as_arr(argv[1], "matvec() weights");
    AArr *ba = as_arr_opt(argv[2], "matvec() bias");
    int64_t nin = a_int_full(argv[3]);
    int64_t nout = a_int_full(argv[4]);
    if (nin <= 0 || nout <= 0) panicf("matvec: bad dims");
    if ((int64_t)xa->h.len < nin) panicf("matvec: x too short");
    if ((int64_t)wa->h.len < nin * nout) panicf("matvec: w too short");
    int has_bias = ba && (int64_t)ba->h.len >= nout;
    double *x = to_f64(xa, (uint64_t)nin, "matvec() vector");
    double *bias = has_bias ? to_f64(ba, (uint64_t)nout, "matvec() bias") : NULL;
    double *out = (double *)malloc((size_t)nout * sizeof(double));
    if (!out) panicf("out of memory");
    if (bias) memcpy(out, bias, (size_t)nout * sizeof(double));
    else memset(out, 0, (size_t)nout * sizeof(double));
    const double *wf = f64_ptr(wa);
    int wp = wf ? 0 : arr_packed(wa);
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i];
        uint64_t base = (uint64_t)i * (uint64_t)nout;
        if (wf) {
            const double *wr = wf + base;
            for (int64_t j = 0; j < nout; j++) out[j] += xi * wr[j];
        } else if (wp) {
            const int64_t *wr = wa->iitems + base;
            for (int64_t j = 0; j < nout; j++) out[j] += xi * (double)wr[j];
        } else {
            const aval *wr = wa->items + base;
            for (int64_t j = 0; j < nout; j++) out[j] += xi * aval_num(wr[j]);
        }
    }
    aval r = f64_list((uint64_t)nout, out);
    free(out);
    free(x);
    free(bias);
    return r;
}

/* matvec_t(x, w, bias, nout, nin) -> list
 * w is stored [out][in] (row-major, e.g. wte for the tied lm_head):
 *   out[i] = bias[i] + sum_j x[j] * w[i*nin + j] */
aval bi_linalg_matvec_t(uint64_t n, aval *argv) {
    want_args("linalg_matvec_t", n, 5, 5);
    AArr *xa = as_arr(argv[0], "matvec_t() vector");
    AArr *wa = as_arr(argv[1], "matvec_t() weights");
    AArr *ba = as_arr_opt(argv[2], "matvec_t() bias");
    int64_t nout = a_int_full(argv[3]);
    int64_t nin = a_int_full(argv[4]);
    if (nin <= 0 || nout <= 0) panicf("matvec_t: bad dims");
    if ((int64_t)xa->h.len < nin) panicf("matvec_t: x too short");
    if ((int64_t)wa->h.len < nout * nin) panicf("matvec_t: w too short");
    double *x = to_f64(xa, (uint64_t)nin, "matvec_t() vector");
    double *out = (double *)malloc((size_t)nout * sizeof(double));
    if (!out) panicf("out of memory");
    const double *wf = f64_ptr(wa);
    int wp = wf ? 0 : arr_packed(wa);
    int has_bias = ba && (int64_t)ba->h.len >= nout;
    for (int64_t i = 0; i < nout; i++) {
        uint64_t base = (uint64_t)i * (uint64_t)nin;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t j = 0;
        if (wf) {
            const double *wr = wf + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 += wr[j] * x[j];
                acc1 += wr[j + 1] * x[j + 1];
                acc2 += wr[j + 2] * x[j + 2];
                acc3 += wr[j + 3] * x[j + 3];
            }
            for (; j < nin; j++) acc0 += wr[j] * x[j];
        } else if (wp) {
            const int64_t *wr = wa->iitems + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 += (double)wr[j] * x[j];
                acc1 += (double)wr[j + 1] * x[j + 1];
                acc2 += (double)wr[j + 2] * x[j + 2];
                acc3 += (double)wr[j + 3] * x[j + 3];
            }
            for (; j < nin; j++) acc0 += (double)wr[j] * x[j];
        } else {
            const aval *wr = wa->items + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 += aval_num(wr[j]) * x[j];
                acc1 += aval_num(wr[j + 1]) * x[j + 1];
                acc2 += aval_num(wr[j + 2]) * x[j + 2];
                acc3 += aval_num(wr[j + 3]) * x[j + 3];
            }
            for (; j < nin; j++) acc0 += aval_num(wr[j]) * x[j];
        }
        double bias = has_bias ? arr_num(ba, (uint64_t)i) : 0.0;
        out[i] = ((acc0 + acc1) + (acc2 + acc3)) + bias;
    }
    aval r = f64_list((uint64_t)nout, out);
    free(out);
    free(x);
    return r;
}
