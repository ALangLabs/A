#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* metrics module                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;
    int kind;              /* 0=counter 1=gauge 2=histogram */
    double value;          /* gauge: last value; counter/hist: sum */
    uint64_t count;
    double min, max;
} AMetric;

static AMetric *g_metrics;
static size_t g_nmetrics, g_capmetrics;

static char *metric_key(aval name, aval labels) {
    Buf b;
    buf_init(&b);
    uint64_t nl;
    const char *np = str_data(name, &nl);
    buf_addn(&b, np, (size_t)nl);
    if (labels != A_NONE) {
        buf_addc(&b, '{');
        format_into(&b, labels, 0);
        buf_addc(&b, '}');
    }
    char *k = xstrdup(b.p ? b.p : "");
    free(b.p);
    return k;
}

static AMetric *metric_find(const char *key, int create, int kind) {
    for (size_t i = 0; i < g_nmetrics; ++i)
        if (!strcmp(g_metrics[i].key, key)) return &g_metrics[i];
    if (!create) return NULL;
    if (g_nmetrics == g_capmetrics) {
        g_capmetrics = g_capmetrics ? g_capmetrics * 2 : 16;
        g_metrics = (AMetric *)xrealloc(g_metrics, g_capmetrics * sizeof(AMetric));
    }
    AMetric *m = &g_metrics[g_nmetrics++];
    m->key = xstrdup(key);
    m->kind = kind;
    m->value = 0;
    m->count = 0;
    m->min = 0;
    m->max = 0;
    return m;
}

aval bi_metrics_counter(uint64_t n, aval *argv) {
    want_args("metrics.counter", n, 1, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 0);
    free(key);
    m->value += n > 1 ? num_to_double(argv[1]) : 1.0;
    m->count++;
    return A_NONE;
}

aval bi_metrics_gauge(uint64_t n, aval *argv) {
    want_args("metrics.gauge", n, 2, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 1);
    free(key);
    m->value = num_to_double(argv[1]);
    return A_NONE;
}

aval bi_metrics_histogram(uint64_t n, aval *argv) {
    want_args("metrics.histogram", n, 2, 3);
    char *key = metric_key(argv[0], n > 2 ? argv[2] : A_NONE);
    AMetric *m = metric_find(key, 1, 2);
    free(key);
    double v = num_to_double(argv[1]);
    if (!m->count || v < m->min) m->min = v;
    if (!m->count || v > m->max) m->max = v;
    m->value += v;
    m->count++;
    return A_NONE;
}

aval bi_metrics_render(uint64_t n, aval *argv) {
    want_args("metrics.render", n, 0, 0);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < g_nmetrics; ++i) {
        AMetric *m = &g_metrics[i];
        buf_adds(&b, m->key);
        buf_adds(&b, " ");
        if (m->kind == 2 && m->count)
            buf_addi64(&b, (int64_t)(m->value / (double)m->count));
        else if (m->kind == 0)
            buf_addi64(&b, (int64_t)m->value);
        else {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.6g", m->value);
            buf_adds(&b, tmp);
        }
        buf_addc(&b, '\n');
    }
    aval out = buf_take(&b);
    return out;
}

aval bi_metrics_reset(uint64_t n, aval *argv) {
    want_args("metrics.reset", n, 0, 0);
    for (size_t i = 0; i < g_nmetrics; ++i) free(g_metrics[i].key);
    free(g_metrics);
    g_metrics = NULL;
    g_nmetrics = g_capmetrics = 0;
    return A_NONE;
}

