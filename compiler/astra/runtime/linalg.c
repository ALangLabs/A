/* linalg.c — dense numeric kernels over packed list storage.
 *
 * For a T_LIST whose elements are all doubles, AArr.items is an aval[]
 * whose 64-bit words ARE the IEEE-754 double bits (NaN-boxing stores
 * doubles inline). These builtins reinterpret items as double* and run
 * the whole reduction in C — no per-element aval dispatch, bounds checks,
 * or boxing. This is the fast path for matmul/attention-style code.
 */
#include "a_runtime_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* aval bits for a non-NaN double equal the IEEE-754 representation, so a
 * plain pointer cast is exact on x86-64/aarch64 (little-endian IEEE). */
static const double *as_f64(aval v, AArr **out) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_LIST) return NULL;
    AArr *a = (AArr *)o;
    *out = a;
    return (const double *)(const void *)a->items;
}

static aval *f64_list(uint64_t n, const double *src) {
    aval *items = (aval *)malloc((size_t)n * sizeof(aval));
    if (!items) panicf("out of memory");
    for (uint64_t i = 0; i < n; i++) items[i] = mkdouble(src[i]);
    aval r = a_list_new(n, items);
    free(items);
    return r;
}

/* dot(a, b) -> float : sum_i a[i]*b[i] over min(len) */
aval bi_linalg_dot(uint64_t n, aval *argv) {
    (void)n;
    AArr *aa = NULL, *bb = NULL;
    const double *a = as_f64(argv[0], &aa);
    const double *b = as_f64(argv[1], &bb);
    if (!a || !b) panicf("linalg.dot expects two lists of numbers");
    uint64_t na = aa->h.len, nb = bb->h.len;
    uint64_t m = na < nb ? na : nb;
    const double *ar = a, *br = b;
    double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
    uint64_t i = 0;
    for (; i + 4 <= m; i += 4) {
        acc0 += ar[i] * br[i];
        acc1 += ar[i + 1] * br[i + 1];
        acc2 += ar[i + 2] * br[i + 2];
        acc3 += ar[i + 3] * br[i + 3];
    }
    for (; i < m; i++) acc0 += ar[i] * br[i];
    return mkdouble((acc0 + acc1) + (acc2 + acc3));
}

/* matvec(x, w, bias, nin, nout) -> list
 * w is stored [in][out] (safetensors Conv1D layout):
 *   out[j] = bias[j] + sum_i x[i] * w[i*nout + j]
 * Sequential w reads, `out` stays cache-hot. */
aval bi_linalg_matvec(uint64_t n, aval *argv) {
    (void)n;
    AArr *xa = NULL, *wa = NULL, *ba = NULL;
    const double *x = as_f64(argv[0], &xa);
    const double *w = as_f64(argv[1], &wa);
    const double *bias = as_f64(argv[2], &ba);
    if (!x || !w) panicf("linalg.matvec expects list args");
    int64_t nin = a_int_full(argv[3]);
    int64_t nout = a_int_full(argv[4]);
    if (nin <= 0 || nout <= 0) panicf("linalg.matvec: bad dims");
    if ((int64_t)xa->h.len < nin) panicf("linalg.matvec: x too short");
    if ((int64_t)wa->h.len < nin * nout) panicf("linalg.matvec: w too short");
    double *out = (double *)malloc((size_t)nout * sizeof(double));
    if (!out) panicf("out of memory");
    if (bias && (int64_t)ba->h.len >= nout) {
        memcpy(out, bias, (size_t)nout * sizeof(double));
    } else {
        memset(out, 0, (size_t)nout * sizeof(double));
    }
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i];
        const double *wr = w + (size_t)i * (size_t)nout;
        int64_t j = 0;
        for (; j + 4 <= nout; j += 4) {
            out[j] += xi * wr[j];
            out[j + 1] += xi * wr[j + 1];
            out[j + 2] += xi * wr[j + 2];
            out[j + 3] += xi * wr[j + 3];
        }
        for (; j < nout; j++) out[j] += xi * wr[j];
    }
    aval r = f64_list((uint64_t)nout, out);
    free(out);
    return r;
}

/* matvec_t(x, w, bias, nout, nin) -> list
 * w is stored [out][in] (row-major, e.g. wte for the tied lm_head):
 *   out[i] = bias[i] + sum_j x[j] * w[i*nin + j] */
aval bi_linalg_matvec_t(uint64_t n, aval *argv) {
    (void)n;
    AArr *xa = NULL, *wa = NULL, *ba = NULL;
    const double *x = as_f64(argv[0], &xa);
    const double *w = as_f64(argv[1], &wa);
    const double *bias = as_f64(argv[2], &ba);
    if (!x || !w) panicf("linalg.matvec_t expects list args");
    int64_t nout = a_int_full(argv[3]);
    int64_t nin = a_int_full(argv[4]);
    if (nin <= 0 || nout <= 0) panicf("linalg.matvec_t: bad dims");
    if ((int64_t)xa->h.len < nin) panicf("linalg.matvec_t: x too short");
    if ((int64_t)wa->h.len < nout * nin) panicf("linalg.matvec_t: w too short");
    double *out = (double *)malloc((size_t)nout * sizeof(double));
    if (!out) panicf("out of memory");
    const double *xr = x;
    for (int64_t i = 0; i < nout; i++) {
        const double *wr = w + (size_t)i * (size_t)nin;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t j = 0;
        for (; j + 4 <= nin; j += 4) {
            acc0 += wr[j] * xr[j];
            acc1 += wr[j + 1] * xr[j + 1];
            acc2 += wr[j + 2] * xr[j + 2];
            acc3 += wr[j + 3] * xr[j + 3];
        }
        for (; j < nin; j++) acc0 += wr[j] * xr[j];
        out[i] = ((acc0 + acc1) + (acc2 + acc3)) +
                 (bias && (int64_t)ba->h.len > i ? bias[i] : 0.0);
    }
    aval r = f64_list((uint64_t)nout, out);
    free(out);
    return r;
}
