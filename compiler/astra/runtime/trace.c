#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* trace module                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t id;
    aval name;
    aval attrs;    /* dict */
    aval events;   /* list */
    int64_t start_ms;
    int64_t end_ms;
} ASpan;

static ASpan *g_spans;
static size_t g_nspans, g_capspans;
static uint64_t g_next_span = 1;
static uint64_t g_current_span = 0;
static uint64_t g_trace_id_hi = UINT64_C(0x4bf92f3577b34da6);
static uint64_t g_trace_id_lo = 0;

static ASpan *span_find(uint64_t id) {
    for (size_t i = g_nspans; i-- > 0;)
        if (g_spans[i].id == id) return &g_spans[i];
    return NULL;
}

static int64_t now_ms(void) {
#if defined(_WIN32)
    return a_win_mono_ms();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

aval bi_trace_start(uint64_t n, aval *argv) {
    want_args("trace.start", n, 1, 2);
    if (g_nspans == g_capspans) {
        g_capspans = g_capspans ? g_capspans * 2 : 16;
        g_spans = (ASpan *)xrealloc(g_spans, g_capspans * sizeof(ASpan));
    }
    ASpan *s = &g_spans[g_nspans++];
    s->id = g_next_span++;
    s->name = argv[0];
    s->attrs = dict_new_empty();
    s->events = list_from_vec(NULL, 0, 0);
    s->start_ms = now_ms();
    s->end_ms = -1;
    if (!g_trace_id_lo)
        g_trace_id_lo = ((uint64_t)time(NULL) << 32) ^ (s->id * UINT64_C(0x9e3779b97f4a7c15));
    g_current_span = s->id;
    return mkint_full((int64_t)s->id);
}

aval bi_trace_end(uint64_t n, aval *argv) {
    want_args("trace.end", n, 1, 1);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.end: unknown span");
    s->end_ms = now_ms();
    if (g_current_span == s->id) g_current_span = 0;
    return A_NONE;
}

aval bi_trace_current(uint64_t n, aval *argv) {
    want_args("trace.current", n, 0, 0);
    return mkint_full((int64_t)g_current_span);
}

aval bi_trace_set_attribute(uint64_t n, aval *argv) {
    want_args("trace.set_attribute", n, 3, 3);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.set_attribute: unknown span");
    dict_put((ADict *)obj_of(s->attrs), argv[1], argv[2]);
    return A_NONE;
}

aval bi_trace_add_event(uint64_t n, aval *argv) {
    want_args("trace.add_event", n, 2, 3);
    ASpan *s = span_find((uint64_t)int_val_full(argv[0]));
    if (!s) panicf("trace.add_event: unknown span");
    /* events: list of {name, attributes?} dicts */
    aval ev = dict_new_empty();
    ADict *ed = (ADict *)obj_of(ev);
    dict_put(ed, a_str_new("name", 4), argv[1]);
    if (n > 2) dict_put(ed, a_str_new("attributes", 10), argv[2]);
    AArr *ea = (AArr *)obj_of(s->events);
    arr_push(ea, ev);
    return A_NONE;
}

static int trace_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

aval bi_trace_inject(uint64_t n, aval *argv) {
    want_args("trace.inject", n, 0, 0);
    if (!g_current_span) return a_str_new("", 0);
    char buf[64];
    snprintf(buf, sizeof buf, "00-%016llx%016llx-%016llx-01",
             (unsigned long long)g_trace_id_hi,
             (unsigned long long)g_trace_id_lo,
             (unsigned long long)g_current_span);
    return a_str_new(buf, strlen(buf));
}

aval bi_trace_extract(uint64_t n, aval *argv) {
    want_args("trace.extract", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    /* W3C traceparent: version(2)-traceid(32)-spanid(16)-flags(2). */
    if (len != 55 || s[2] != '-' || s[35] != '-' || s[52] != '-')
        return mkint_full(0);
    uint64_t span = 0;
    for (int i = 36; i < 52; ++i) {
        int v = trace_hexval(s[i]);
        if (v < 0) return mkint_full(0);
        span = (span << 4) | (uint64_t)v;
    }
    return mkint_full((int64_t)span);
}

aval bi_trace_spans(uint64_t n, aval *argv) {
    want_args("trace.spans", n, 0, 0);
    aval *items = (aval *)xmalloc(sizeof(aval) * (g_nspans ? g_nspans : 1));
    for (size_t i = 0; i < g_nspans; ++i) {
        ASpan *s = &g_spans[i];
        aval d = dict_new_empty();
        ADict *dd = (ADict *)obj_of(d);
        dict_put(dd, a_str_new("id", 2), mkint_full((int64_t)s->id));
        dict_put(dd, a_str_new("name", 4), s->name);
        dict_put(dd, a_str_new("attributes", 10), s->attrs);
        dict_put(dd, a_str_new("events", 6), s->events);
        dict_put(dd, a_str_new("start_ms", 8), mkint_full(s->start_ms));
        dict_put(dd, a_str_new("end_ms", 6), s->end_ms >= 0 ? mkint_full(s->end_ms) : A_NONE);
        items[i] = d;
    }
    aval out = list_from_vec(items, (uint64_t)g_nspans, 0);
    free(items);
    return out;
}

aval bi_trace_reset(uint64_t n, aval *argv) {
    want_args("trace.reset", n, 0, 0);
    g_nspans = 0;
    g_current_span = 0;
    g_next_span = 1;
    return A_NONE;
}

