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

/* Returns a raw double view of the first `need` elements, materializing a
 * malloc'd copy into *tmp when the list is not already verified all-double.
 * Caller frees *tmp (NULL when a direct view was returned). */
static const double *as_f64(const AArr *a, uint64_t need, double **tmp,
                            const char *what) {
    *tmp = NULL;
    if ((uint64_t)a->h.len < need) panicf("%s too short", what);
    const double *view = f64_ptr((AArr *)a);
    if (view) return view;
    *tmp = to_f64(a, need, what);
    return *tmp;
}

/* Tile width for the blocked matmul loops (Mojo/CBLAS-style cache blocking:
 * a 32-wide tile of doubles is 256 bytes, so an output row segment and the
 * 32 B-rows it touches stay hot in L1 while the k loop streams). */
#define LINALG_TILE 32

/* ------------------------------------------------------------------ */
/* implicit parallelism (rt_pool.c)                                    */
/* ------------------------------------------------------------------ */
/* The kernels below parallelize only across *independent output
   elements*: each worker owns disjoint rows/columns and runs the exact
   same per-element instruction sequence as the serial loop, so results
   are bit-identical. Reduction loops (dot, matvec's i-accumulation,
   attention's value-mix, softmax's total) stay serial to preserve FP
   ordering. Work below ~1M multiply-adds stays serial: dispatch costs
   more than it saves. */
#define LINALG_PAR_MIN ((int64_t)1 << 20)

static int lpar_ok(double work) {
    return work >= (double)LINALG_PAR_MIN && a_par_workers() > 1;
}

typedef struct {
    const double *ad, *bd;
    double *out;
    int64_t k, cols;
} AMatmulCtx;

static void matmul_rows(void *vctx, int64_t lo, int64_t hi) {
    AMatmulCtx *c = (AMatmulCtx *)vctx;
    for (int64_t i = lo; i < hi; i++) {
        double *orow = c->out + (uint64_t)i * (uint64_t)c->cols;
        const double *arow = c->ad + (uint64_t)i * (uint64_t)c->k;
        for (int64_t jj = 0; jj < c->cols; jj += LINALG_TILE) {
            int64_t jend = jj + LINALG_TILE < c->cols ? jj + LINALG_TILE : c->cols;
            for (int64_t t = 0; t < c->k; t++) {
                double a = arow[t];
                const double *brow = c->bd + (uint64_t)t * (uint64_t)c->cols;
                for (int64_t j = jj; j < jend; j++)
                    orow[j] = fma(a, brow[j], orow[j]);
            }
        }
    }
}

typedef struct {
    const double *x;
    const double *wf;      /* verified all-double view, or NULL */
    const int64_t *wi;     /* packed int view, or NULL */
    const aval *wa;        /* aval view, or NULL */
    const double *bias;    /* NULL when absent (zeros) */
    double *out;
    int64_t nin;
} AMatvecTCtx;

static void matvec_t_rows(void *vctx, int64_t lo, int64_t hi) {
    AMatvecTCtx *c = (AMatvecTCtx *)vctx;
    for (int64_t i = lo; i < hi; i++) {
        uint64_t base = (uint64_t)i * (uint64_t)c->nin;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t j = 0;
        if (c->wf) {
            const double *wr = c->wf + base;
            for (; j + 4 <= c->nin; j += 4) {
                acc0 = fma(wr[j], c->x[j], acc0);
                acc1 = fma(wr[j + 1], c->x[j + 1], acc1);
                acc2 = fma(wr[j + 2], c->x[j + 2], acc2);
                acc3 = fma(wr[j + 3], c->x[j + 3], acc3);
            }
            for (; j < c->nin; j++) acc0 = fma(wr[j], c->x[j], acc0);
        } else if (c->wi) {
            const int64_t *wr = c->wi + base;
            for (; j + 4 <= c->nin; j += 4) {
                acc0 = fma((double)wr[j], c->x[j], acc0);
                acc1 = fma((double)wr[j + 1], c->x[j + 1], acc1);
                acc2 = fma((double)wr[j + 2], c->x[j + 2], acc2);
                acc3 = fma((double)wr[j + 3], c->x[j + 3], acc3);
            }
            for (; j < c->nin; j++) acc0 = fma((double)wr[j], c->x[j], acc0);
        } else {
            const aval *wr = c->wa + base;
            for (; j + 4 <= c->nin; j += 4) {
                acc0 = fma(aval_num(wr[j]), c->x[j], acc0);
                acc1 = fma(aval_num(wr[j + 1]), c->x[j + 1], acc1);
                acc2 = fma(aval_num(wr[j + 2]), c->x[j + 2], acc2);
                acc3 = fma(aval_num(wr[j + 3]), c->x[j + 3], acc3);
            }
            for (; j < c->nin; j++) acc0 = fma(aval_num(wr[j]), c->x[j], acc0);
        }
        double bias = c->bias ? c->bias[i] : 0.0;
        c->out[i] = ((acc0 + acc1) + (acc2 + acc3)) + bias;
    }
}

typedef struct {
    const double *qp, *kp;
    double *scores;
    int64_t dk;
} AAttnCtx;

static void attn_scores(void *vctx, int64_t lo, int64_t hi) {
    AAttnCtx *c = (AAttnCtx *)vctx;
    for (int64_t r = lo; r < hi; r++) {
        const double *krow = c->kp + (uint64_t)r * (uint64_t)c->dk;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t t = 0;
        for (; t + 4 <= c->dk; t += 4) {
            acc0 = fma(c->qp[t], krow[t], acc0);
            acc1 = fma(c->qp[t + 1], krow[t + 1], acc1);
            acc2 = fma(c->qp[t + 2], krow[t + 2], acc2);
            acc3 = fma(c->qp[t + 3], krow[t + 3], acc3);
        }
        for (; t < c->dk; t++) acc0 = fma(c->qp[t], krow[t], acc0);
        c->scores[r] = ((acc0 + acc1) + (acc2 + acc3));
    }
}

typedef struct {
    const double *xp;
    double *out;
    double shift;
} AExpCtx;

static void exp_fill(void *vctx, int64_t lo, int64_t hi) {
    AExpCtx *c = (AExpCtx *)vctx;
    for (int64_t i = lo; i < hi; i++) c->out[i] = exp(c->xp[i] - c->shift);
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

/* dot(a, b) -> float : sum_i a[i]*b[i] over min(len). 4-way unrolled with
 * fma() so each multiply-add rounds once (single-rounding contraction,
 * like LLVM's fmuladd / Mojo SIMD dot). */
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
            acc0 = fma(ad[i], bd[i], acc0);
            acc1 = fma(ad[i + 1], bd[i + 1], acc1);
            acc2 = fma(ad[i + 2], bd[i + 2], acc2);
            acc3 = fma(ad[i + 3], bd[i + 3], acc3);
        }
        for (; i < m; i++) acc0 = fma(ad[i], bd[i], acc0);
    } else {
        for (; i + 4 <= m; i += 4) {
            acc0 = fma(arr_num(aa, i), arr_num(bb, i), acc0);
            acc1 = fma(arr_num(aa, i + 1), arr_num(bb, i + 1), acc1);
            acc2 = fma(arr_num(aa, i + 2), arr_num(bb, i + 2), acc2);
            acc3 = fma(arr_num(aa, i + 3), arr_num(bb, i + 3), acc3);
        }
        for (; i < m; i++) acc0 = fma(arr_num(aa, i), arr_num(bb, i), acc0);
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
            for (int64_t j = 0; j < nout; j++) out[j] = fma(xi, wr[j], out[j]);
        } else if (wp) {
            const int64_t *wr = wa->iitems + base;
            for (int64_t j = 0; j < nout; j++) out[j] = fma(xi, (double)wr[j], out[j]);
        } else {
            const aval *wr = wa->items + base;
            for (int64_t j = 0; j < nout; j++) out[j] = fma(xi, aval_num(wr[j]), out[j]);
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
    int par = lpar_ok((double)nout * (double)nin);
    double *biasv = NULL;
    if (par && has_bias) biasv = to_f64(ba, (uint64_t)nout, "matvec_t() bias");
    if (par) {
        /* Row-parallel over output rows: identical per-row dot order. */
        AMatvecTCtx mc;
        mc.x = x; mc.out = out; mc.nin = nin;
        mc.wf = wf;
        mc.wi = wp ? wa->iitems : NULL;
        mc.wa = (!wf && !wp) ? wa->items : NULL;
        mc.bias = biasv;
        a_par_for(nout, 1, matvec_t_rows, &mc);
    } else {
    for (int64_t i = 0; i < nout; i++) {
        uint64_t base = (uint64_t)i * (uint64_t)nin;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t j = 0;
        if (wf) {
            const double *wr = wf + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 = fma(wr[j], x[j], acc0);
                acc1 = fma(wr[j + 1], x[j + 1], acc1);
                acc2 = fma(wr[j + 2], x[j + 2], acc2);
                acc3 = fma(wr[j + 3], x[j + 3], acc3);
            }
            for (; j < nin; j++) acc0 = fma(wr[j], x[j], acc0);
        } else if (wp) {
            const int64_t *wr = wa->iitems + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 = fma((double)wr[j], x[j], acc0);
                acc1 = fma((double)wr[j + 1], x[j + 1], acc1);
                acc2 = fma((double)wr[j + 2], x[j + 2], acc2);
                acc3 = fma((double)wr[j + 3], x[j + 3], acc3);
            }
            for (; j < nin; j++) acc0 = fma((double)wr[j], x[j], acc0);
        } else {
            const aval *wr = wa->items + base;
            for (; j + 4 <= nin; j += 4) {
                acc0 = fma(aval_num(wr[j]), x[j], acc0);
                acc1 = fma(aval_num(wr[j + 1]), x[j + 1], acc1);
                acc2 = fma(aval_num(wr[j + 2]), x[j + 2], acc2);
                acc3 = fma(aval_num(wr[j + 3]), x[j + 3], acc3);
            }
            for (; j < nin; j++) acc0 = fma(aval_num(wr[j]), x[j], acc0);
        }
        double bias = has_bias ? arr_num(ba, (uint64_t)i) : 0.0;
        out[i] = ((acc0 + acc1) + (acc2 + acc3)) + bias;
    }
    }
    aval r = f64_list((uint64_t)nout, out);
    free(out);
    free(x);
    free(biasv);
    return r;
}

/* matmul(a, b, m, k, n) -> flat [f64] of length m*n.
 * a is m×k row-major, b is k×n row-major; out[i*n+j] = sum_t a[i][t]*b[t][j].
 * Blocked ikj order (Mojo/CBLAS-style tiling): the n loop runs in tiles of
 * LINALG_TILE so an output row segment stays in L1 while B rows stream
 * sequentially, and every multiply-add uses fma() (one rounding). */
aval bi_linalg_matmul(uint64_t n, aval *argv) {
    want_args("linalg_matmul", n, 5, 5);
    AArr *aa = as_arr(argv[0], "matmul() left operand");
    AArr *bb = as_arr(argv[1], "matmul() right operand");
    int64_t m = a_int_full(argv[2]);
    int64_t k = a_int_full(argv[3]);
    int64_t cols = a_int_full(argv[4]);
    if (m <= 0 || k <= 0 || cols <= 0) panicf("matmul: bad dims");
    if ((int64_t)aa->h.len < m * k) panicf("matmul: left operand too short");
    if ((int64_t)bb->h.len < k * cols) panicf("matmul: right operand too short");
    double *ta = NULL, *tb = NULL;
    const double *ad = as_f64(aa, (uint64_t)(m * k), &ta, "matmul() left operand");
    const double *bd = as_f64(bb, (uint64_t)(k * cols), &tb, "matmul() right operand");
    double *out = (double *)malloc((size_t)(m * cols) * sizeof(double));
    if (!out) panicf("out of memory");
    memset(out, 0, (size_t)(m * cols) * sizeof(double));
    if (lpar_ok((double)m * (double)k * (double)cols)) {
        /* Row-parallel: each output row is computed by exactly one worker
           with the identical ikj sequence, so results are bit-identical. */
        AMatmulCtx mc = { ad, bd, out, k, cols };
        a_par_for(m, 1, matmul_rows, &mc);
    } else {
        for (int64_t i = 0; i < m; i++) {
            double *orow = out + (uint64_t)i * (uint64_t)cols;
            const double *arow = ad + (uint64_t)i * (uint64_t)k;
            for (int64_t jj = 0; jj < cols; jj += LINALG_TILE) {
                int64_t jend = jj + LINALG_TILE < cols ? jj + LINALG_TILE : cols;
                for (int64_t t = 0; t < k; t++) {
                    double a = arow[t];
                    const double *brow = bd + (uint64_t)t * (uint64_t)cols;
                    for (int64_t j = jj; j < jend; j++)
                        orow[j] = fma(a, brow[j], orow[j]);
                }
            }
        }
    }
    aval r = f64_list((uint64_t)(m * cols), out);
    free(out);
    free(ta);
    free(tb);
    return r;
}

/* softmax(x) -> [f64]. Numerically stable: subtract the max before exp(),
 * so arbitrarily large logits never overflow. */
aval bi_linalg_softmax(uint64_t n, aval *argv) {
    want_args("linalg_softmax", n, 1, 1);
    AArr *xa = as_arr(argv[0], "softmax() argument");
    uint64_t len = xa->h.len;
    if (len == 0) panicf("softmax: empty input");
    double *tmp = NULL;
    const double *xp = as_f64(xa, len, &tmp, "softmax() argument");
    double *out = (double *)malloc((size_t)len * sizeof(double));
    if (!out) panicf("out of memory");
    double top = xp[0];
    for (uint64_t i = 1; i < len; i++) if (xp[i] > top) top = xp[i];
    /* exp() dominates the cost and is element-independent: fill in
       parallel (identical values), keep the max/sum reductions serial so
       FP ordering — and the result — is unchanged. exp() is ~20x a
       multiply-add, so the work threshold is correspondingly lower. */
    if (len >= ((uint64_t)1 << 17) && a_par_workers() > 1) {
        AExpCtx ec = { xp, out, top };
        a_par_for((int64_t)len, 16384, exp_fill, &ec);
    } else {
        for (uint64_t i = 0; i < len; i++) out[i] = exp(xp[i] - top);
    }
    double total = 0.0;
    for (uint64_t i = 0; i < len; i++) total += out[i];
    for (uint64_t i = 0; i < len; i++) out[i] /= total;
    aval r = f64_list(len, out);
    free(out);
    free(tmp);
    return r;
}

/* attention(q, k, v, nk, dk, dv) -> [f64] of length dv.
 * Single-query scaled dot-product attention, fully fused in one pass with no
 * intermediate lists (Mojo-style kernel fusion):
 *   scores[r] = dot(q, k[r]) / sqrt(dk); w = softmax(scores);
 *   out[c]    = sum_r w[r] * v[r][c].
 * k is nk×dk row-major, v is nk×dv row-major. */
aval bi_linalg_attention(uint64_t n, aval *argv) {
    want_args("linalg_attention", n, 6, 6);
    AArr *qa = as_arr(argv[0], "attention() query");
    AArr *ka = as_arr(argv[1], "attention() keys");
    AArr *va = as_arr(argv[2], "attention() values");
    int64_t nk = a_int_full(argv[3]);
    int64_t dk = a_int_full(argv[4]);
    int64_t dv = a_int_full(argv[5]);
    if (nk <= 0 || dk <= 0 || dv <= 0) panicf("attention: bad dims");
    if ((int64_t)qa->h.len < dk) panicf("attention: query too short");
    if ((int64_t)ka->h.len < nk * dk) panicf("attention: keys too short");
    if ((int64_t)va->h.len < nk * dv) panicf("attention: values too short");
    double *tq = NULL, *tk = NULL, *tv = NULL;
    const double *qp = as_f64(qa, (uint64_t)dk, &tq, "attention() query");
    const double *kp = as_f64(ka, (uint64_t)(nk * dk), &tk, "attention() keys");
    const double *vp = as_f64(va, (uint64_t)(nk * dv), &tv, "attention() values");
    double *scores = (double *)malloc((size_t)nk * sizeof(double));
    double *out = (double *)malloc((size_t)dv * sizeof(double));
    if (!scores || !out) panicf("out of memory");
    const double scale = 1.0 / sqrt((double)dk);
    if (lpar_ok((double)nk * (double)dk)) {
        /* Score rows are independent dot products: parallel over r with
           the identical accumulation order per row (bit-identical). The
           softmax reduction and value-mix below stay serial. */
        AAttnCtx ac = { qp, kp, scores, dk };
        a_par_for(nk, 1, attn_scores, &ac);
        for (int64_t r = 0; r < nk; r++) scores[r] *= scale;
    } else {
    for (int64_t r = 0; r < nk; r++) {
        const double *krow = kp + (uint64_t)r * (uint64_t)dk;
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
        int64_t t = 0;
        for (; t + 4 <= dk; t += 4) {
            acc0 = fma(qp[t], krow[t], acc0);
            acc1 = fma(qp[t + 1], krow[t + 1], acc1);
            acc2 = fma(qp[t + 2], krow[t + 2], acc2);
            acc3 = fma(qp[t + 3], krow[t + 3], acc3);
        }
        for (; t < dk; t++) acc0 = fma(qp[t], krow[t], acc0);
        scores[r] = ((acc0 + acc1) + (acc2 + acc3)) * scale;
    }
    }
    double top = scores[0];
    for (int64_t r = 1; r < nk; r++) if (scores[r] > top) top = scores[r];
    double total = 0.0;
    for (int64_t r = 0; r < nk; r++) {
        scores[r] = exp(scores[r] - top);
        total += scores[r];
    }
    memset(out, 0, (size_t)dv * sizeof(double));
    for (int64_t r = 0; r < nk; r++) {
        double w = scores[r] / total;
        const double *vrow = vp + (uint64_t)r * (uint64_t)dv;
        for (int64_t c = 0; c < dv; c++)
            out[c] = fma(w, vrow[c], out[c]);
    }
    aval result = f64_list((uint64_t)dv, out);
    free(scores);
    free(out);
    free(tq);
    free(tk);
    free(tv);
    return result;
}
