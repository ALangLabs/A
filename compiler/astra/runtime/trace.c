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

static ASpan *span_find(uint64_t id) {
    for (size_t i = g_nspans; i-- > 0;)
        if (g_spans[i].id == id) return &g_spans[i];
    return NULL;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
    return g_current_span ? mkint_full((int64_t)g_current_span) : A_NONE;
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
    /* append to events list: rebuild via list concat helper is overkill;
       store events as dict keyed by count instead */
    ADict *ad = (ADict *)obj_of(s->attrs);
    aval cnt = dict_find(ad, a_str_new("__nevents", 9)) >= 0
                   ? ad->pairs[dict_find(ad, a_str_new("__nevents", 9))].v
                   : a_mkint(0);
    dict_put(ad, a_str_new("__nevents", 9), a_mkint(int_val_full(cnt) + 1));
    dict_put(ad, a_str_new("__evt", 5), ev); /* keeps last; list form below */
    /* simpler: keep an AArr of events */
    AArr *ea = (AArr *)obj_of(s->events);
    uint64_t idx = ea->h.len;
    ea->items = (aval *)xrealloc(ea->items, sizeof(aval) * (idx + 1));
    ea->items[idx] = ev;
    ea->h.len = (uint32_t)(idx + 1);
    return A_NONE;
}

aval bi_trace_inject(uint64_t n, aval *argv) {
    want_args("trace.inject", n, 0, 1);
    aval d = dict_new_empty();
    ADict *dd = (ADict *)obj_of(d);
    dict_put(dd, a_str_new("trace_id", 8), a_mkint(1));
    dict_put(dd, a_str_new("span_id", 7), g_current_span ? mkint_full((int64_t)g_current_span) : A_NONE);
    return d;
}

aval bi_trace_extract(uint64_t n, aval *argv) {
    want_args("trace.extract", n, 1, 1);
    (void)argv;
    return A_NONE;
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

