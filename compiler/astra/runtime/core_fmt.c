#include "a_runtime_internal.h"

/* A_RT_TLS: both the cache and the bump arena are per-thread under
   A_PAR_REAL so str()/repr() are safe to call from pool workers. The
   cache entries are immutable AStr objects; per-thread copies only cost
   a little duplicated memory (32 KiB TLS each). */
static A_RT_TLS aval g_int_str_cache[4096];
/* Cached "None"/"true"/"false" renderings (see a_str_kw below); marked by
   fmt_active_gc_roots alongside the int cache. */
static A_RT_TLS aval g_kw_strs[3];
/* 2 MiB: one huge page per chunk, so refills are cheap. */
static const size_t kSmallStrChunk = 2u << 20;
static A_RT_TLS char *g_small_str = NULL;
static A_RT_TLS char *g_small_str_end = NULL;

/* ------------------------------------------------------------------ */
/* type names / formatting                                             */
/* ------------------------------------------------------------------ */

int kind_of(aval v) {
    if (a_is_none(v)) return K_NONE;
    if (v == A_TRUE || v == A_FALSE) return K_BOOL;
    if (is_int_val(v)) return K_INT;
    if (a_is_double(v)) return K_FLOAT;
    AObj *o = obj_of(v);
    if (!o) return K_OTHER;
    switch (o->type) {
        case T_STR: case T_ROPE: return K_STR;
        case T_BYTES: return K_BYTES;
        case T_LIST: return K_LIST;
        case T_TUPLE: return K_TUPLE;
        case T_DICT: case T_STRUCT: return K_DICT;
        case T_SET: return K_SET;
        case T_RANGE: return K_RANGE;
        case T_ITER: return K_ITER;
        case T_FNON: return K_FNON;
        case T_ENV: return K_ENV;
        case T_RESULT: return K_RESULT;
        case T_CHAR: return K_CHAR;
        case T_VIEW: return K_VIEW;
        default: return K_OTHER;
    }
}

const char *type_name_cstr(aval v) {
    AObj *o = obj_of(v);
    if (o && o->type == T_STRUCT) {
        uint64_t nl;
        return str_data(((AStruct *)o)->name, &nl);
    }
    switch (kind_of(v)) {
        case K_NONE: return "NoneType";
        case K_BOOL: return "bool";
        case K_INT: return "int";
        case K_FLOAT: return "float";
        case K_STR: return "str";
        case K_LIST: return "list";
        case K_TUPLE: return "tuple";
        case K_DICT: return "dict";
        case K_SET: return "set";
        case K_RANGE: return "range";
        case K_ITER: return "iterator";
        case K_FNON: return "function";
        case K_ENV: return "environment";
        case K_RESULT: return "result";
        case K_CHAR: return "char";
        case K_VIEW: return "view";
        case K_BYTES: return "Bytes";
        default: return "object";
    }
}

aval a_type_name(aval v) {
    const char *s = type_name_cstr(v);
    return a_str_new(s, strlen(s));
}

/* Type name used in error messages: the A spelling, not the Python-style
   historical `type()` names (NoneType/list/dict/str). */
const char *type_name_error_cstr(aval v) {
    switch (kind_of(v)) {
        case K_NONE: return "None";
        case K_STR: return "String";
        case K_LIST: return "array";
        case K_DICT: return "map";
        case K_SET: return "set";
        case K_RANGE: return "range";
        case K_ITER: return "iterator";
        case K_FNON: return "function";
        case K_ENV: return "closure";
        case K_RESULT: return "Result";
        case K_VIEW: return "view";
        default: return type_name_cstr(v);
    }
}

int a_obj_is_instance(aval v, const char *type_name) {
    if (!type_name) return 0;
    return strcmp(type_name_cstr(v), type_name) == 0;
}


/* Fast path for format_double_buf: find the minimal %.*g precision that
   round-trips without running snprintf+strtod for every precision 1..17.

   How it works: %.25e yields 26 significant digits of d (correctly rounded
   by the libc). Rounding that digit string to p significant digits gives
   the same decimal value %.*g would print, except possibly when d sits
   within ~1e-26 of a rounding boundary — the '5' with all-zero tail case
   is detected and decided with the real snprintf+strtod test instead.

   A p-digit decimal V round-trips to d iff V lies strictly inside d's
   parse interval (midpoints to the neighbouring doubles; exact ties go to
   the even neighbour and are resolved by a real snprintf+strtod test).
   The interval bounds and V are compared in long double, which has enough
   extra precision that a wide safety margin makes misclassification
   impossible; anything within the margin gets the real test instead.

   Requires a >=64-bit-mantissa long double (x86 80-bit or quad) and a
   correctly-rounded libc printf/strtod: glibc, musl, and the BSD/macOS
   libcs qualify; MSVC's msvcrt does not, so _WIN32 is excluded. */
#if !defined(_WIN32) && defined(__LDBL_MANT_DIG__) && __LDBL_MANT_DIG__ >= 64
#define A_FMT_FAST_DOUBLE 1
#endif

#ifdef A_FMT_FAST_DOUBLE
/* 10^-1 .. 10^-17 as long double literals (correctly rounded). */
static const long double kFmtInv10[18] = {
    1.0L, 1e-1L, 1e-2L, 1e-3L, 1e-4L, 1e-5L, 1e-6L, 1e-7L,
    1e-8L, 1e-9L, 1e-10L, 1e-11L, 1e-12L, 1e-13L, 1e-14L, 1e-15L,
    1e-16L, 1e-17L
};

/* The exact criterion the slow loop uses, for a single precision. */
static int fmt_dbl_real_test(double d, int pr, char *tmp) {
    snprintf(tmp, 64, "%.*g", pr, d);
    return strtod(tmp, NULL) == d;
}

/* Returns the minimal precision in [1,17] whose %.*g output parses back
   to d. tmp is scratch (>= 64 bytes). */
static int fmt_dbl_min_prec(double d, char *tmp) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.25e", d);
    const char *p = buf;
    int neg = 0;
    if (*p == '-') { neg = 1; ++p; }
    /* Layout: d . 25 fraction digits e +- XX  ->  26 significant digits. */
    int dig[26];
    dig[0] = p[0] - '0';
    for (int i = 1; i < 26; ++i) dig[i] = p[1 + i] - '0';
    p += 27;  /* land on 'e' */
    ++p;
    int x = atoi(p);  /* value ~= dig[0].dig[1..] * 10^x */

    /* Parse interval: decimals strictly inside round to d under
       round-to-nearest; the endpoints are exact midpoints (ties) and are
       covered by the margin band below. */
    int e2;
    long double fr = frexpl((long double)d, &e2);  /* d = fr * 2^e2 */
    long double dld = (long double)d;
    long double ad = dld < 0 ? -dld : dld;
    const long double kDblMin = 2.2250738585072014e-308L;
    long double gap_up, gap_dn;
    if (ad < kDblMin) {
        /* subnormal: uniform spacing 2^-1074 on both sides */
        gap_up = gap_dn = ldexpl(1.0L, -1074);
    } else {
        /* binade [2^(e2-1), 2^e2): spacing 2^(e2-53) */
        gap_up = ldexpl(1.0L, e2 - 53);
        /* a power of two (fr == 0.5) has half the spacing below, except
           DBL_MIN whose lower neighbour is subnormal with the same
           2^-1074 spacing */
        gap_dn = (fr == 0.5L && ad != kDblMin)
                     ? ldexpl(1.0L, e2 - 54)
                     : gap_up;
    }
    long double lo = dld - gap_dn * 0.5L;
    long double hi = dld + gap_up * 0.5L;

    /* scale for precision pr is 10^(x-pr+1); start at 10^x (pr == 1) and
       multiply by a correctly-rounded reciprocal each step. */
    long double base = powl(10.0L, (long double)x);
    long double scale = base;

    for (int pr = 1; pr <= 17; ++pr) {
        unsigned long long m = 0;
        for (int i = 0; i < pr; ++i) m = m * 10 + (unsigned)dig[i];
        /* Round to pr digits: first discarded digit decides; a '5' with an
           all-zero tail is exactly at a decimal boundary within our 26
           digits -> cannot decide from the approximation -> bail. */
        int rd = dig[pr], up = 0, amb = 0;
        if (rd > 5) up = 1;
        else if (rd == 5) {
            int sticky = 0;
            for (int j = pr + 1; j < 26; ++j)
                if (dig[j]) { sticky = 1; break; }
            if (sticky) up = 1;
            else amb = 1;
        }
        if (amb) {
            /* The pr-digit rounding of the 26-digit approximation is exactly
               at a decimal boundary, so digits beyond position 25 could
               still flip it: decide this precision with the real test. */
            if (fmt_dbl_real_test(d, pr, tmp)) return pr;
        } else {
            if (up) ++m;  /* carry to 10^pr stays correct via the scale */
            long double val = (long double)m * scale;
            if (neg) val = -val;
            /* The accumulated long-double error is ~1e-18 relative; the
               margin is ~1e-17 relative to |d|, so decisive verdicts are
               safe and borderline cases get the exact test. */
            long double margin = (hi - lo) * 0.05L;
            if (val > lo + margin && val < hi - margin) return pr;
            if (val > lo - margin && val < hi + margin) {
                if (fmt_dbl_real_test(d, pr, tmp)) return pr;
            }
        }
        /* next precision needs scale *= 10^-pr */
        if (pr < 17) scale = base * kFmtInv10[pr];
    }
    return 17;  /* 17 significant digits always round-trip */
}
#endif  /* A_FMT_FAST_DOUBLE */

/* Render a double into tmp (caller provides >= 72 bytes) and return the
   length. Same output as the old format_double_buf Buf version, including
   the trailing ".0" for integral-looking results. */
size_t fmt_double_render(double d, char *tmp) {
    if (isnan(d)) { memcpy(tmp, "nan", 4); return 3; }
    if (isinf(d)) {
        if (d < 0) { memcpy(tmp, "-inf", 5); return 4; }
        memcpy(tmp, "inf", 4); return 3;
    }
    if (d == 0.0) {
        if (signbit(d)) { memcpy(tmp, "-0.0", 5); return 4; }
        memcpy(tmp, "0.0", 4); return 3;
    }
    char num[64];
    int prec = -1;
#ifdef A_FMT_FAST_DOUBLE
    prec = fmt_dbl_min_prec(d, num);
#endif
    if (prec > 0) {
        snprintf(num, sizeof(num), "%.*g", prec, d);
    } else {
        for (prec = 1; prec <= 17; ++prec) {
            snprintf(num, sizeof(num), "%.*g", prec, d);
            if (strtod(num, NULL) == d) break;
        }
    }
    /* Avoid scientific notation for ordinary magnitudes (Python-style);
       expand to plain decimal that still round-trips. */
    if (strchr(num, 'e') || strchr(num, 'E')) {
        double ad = d < 0.0 ? -d : d;
        if (ad >= 1e-4 && ad < 1e16) {
            for (int prec = 1; prec <= 25; ++prec) {
                snprintf(num, sizeof(num), "%.*f", prec, d);
                if (strtod(num, NULL) == d) break;
            }
        }
    }
    size_t n = strlen(num);
    memcpy(tmp, num, n);
    if (strchr(num, '.') == NULL && strchr(num, 'e') == NULL &&
        strchr(num, 'E') == NULL) {
        tmp[n] = '.'; tmp[n + 1] = '0'; n += 2;
    }
    return n;
}

static void format_double_buf(Buf *b, double d) {
    char tmp[72];
    buf_addn(b, tmp, fmt_double_render(d, tmp));
}

static int fmt_needs_quote_escape(unsigned char c) {
    return c == '"' || c == '\\' || c == '\n' || c == '\t' || c == '\r';
}

static void format_quoted(Buf *b, const char *p, size_t n) {
    buf_addc(b, '"');
    size_t i = 0;
    while (i < n) {
        /* Copy runs of plain characters with one memcpy instead of a
           per-byte buf_addc. */
        size_t j = i;
        while (j < n && !fmt_needs_quote_escape((unsigned char)p[j])) ++j;
        if (j > i) buf_addn(b, p + i, j - i);
        if (j >= n) break;
        switch ((unsigned char)p[j]) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\t': buf_adds(b, "\\t"); break;
            case '\r': buf_adds(b, "\\r"); break;
        }
        i = j + 1;
    }
    buf_addc(b, '"');
}

/* Cycle guard for format_into. The formatter recurses structurally over
   the value graph; a cyclic graph (a class instance whose field points
   back to itself, a list/dict/set containing itself, a view of a list
   that contains the view, ...) would otherwise recurse until the C stack
   overflows. g_fmt_active is a per-thread stack (A_RT_TLS: str()/repr()
   may run on pool workers) of the container objects currently being
   rendered; re-encountering one means the value graph has a cycle, so we
   emit "..." — the same convention Python's repr uses for
   self-referential containers. Objects are never freed, so pointer
   identity is stable; the stack is popped on the way out, so DAG-shaped
   sharing (the same object printed twice in sibling positions) still
   renders fully both times. panicf() exits the process, so a panic
   mid-format cannot leave stale entries behind. */
static A_RT_TLS const void **g_fmt_active;
static A_RT_TLS size_t g_fmt_active_len, g_fmt_active_cap;

/* GC root provider: objects mid-format are referenced only by raw
   pointers on this stack; an allocation inside format_into can trigger
   a collection, so the stack's block must be marked (its pointer words
   then keep the formatted objects alive). The bounded int-string cache
   likewise names bump-arena strings. */
static void fmt_active_gc_roots(void) {
    a_gc_mark_ptr(g_fmt_active);
    for (int i = 0; i < 4096; ++i)
        if (g_int_str_cache[i]) a_gc_mark_aval(g_int_str_cache[i]);
    for (int i = 0; i < 3; ++i)
        if (g_kw_strs[i]) a_gc_mark_aval(g_kw_strs[i]);
}
static int g_fmt_gc_hooked;
static void fmt_active_gc_hook(void) {
    if (!g_fmt_gc_hooked) {
        g_fmt_gc_hooked = 1;
        a_gc_add_root_cb(fmt_active_gc_roots);
    }
}

static int fmt_active_has(const void *p) {
    for (size_t i = 0; i < g_fmt_active_len; ++i)
        if (g_fmt_active[i] == p) return 1;
    return 0;
}

static void fmt_active_push(const void *p) {
    fmt_active_gc_hook();
    if (g_fmt_active_len == g_fmt_active_cap) {
        g_fmt_active_cap = g_fmt_active_cap ? g_fmt_active_cap * 2 : 64;
        g_fmt_active = (const void **)xrealloc(g_fmt_active,
                                               g_fmt_active_cap * sizeof(void *));
    }
    g_fmt_active[g_fmt_active_len++] = p;
}

static int fmt_is_container(aval v) {
    switch (kind_of(v)) {
        case K_LIST: case K_TUPLE: case K_DICT: case K_SET: case K_VIEW:
            return 1;
        default:
            return 0;
    }
}

static void format_into_body(Buf *b, aval v, int quote) {
    switch (kind_of(v)) {
        case K_NONE: buf_adds(b, "None"); return;
        case K_BOOL: buf_adds(b, v == A_TRUE ? "true" : "false"); return;
        case K_INT: buf_addi64(b, int_val_full(v)); return;
        case K_FLOAT: format_double_buf(b, a_double_val(v)); return;
        case K_STR: {
            uint64_t n;
            const char *p = str_data(v, &n);
            if (quote) format_quoted(b, p, (size_t)n);
            else buf_addn(b, p, (size_t)n);
            return;
        }
        case K_BYTES: {
            /* b"..." with escape sequences; payloads that are valid UTF-8 print
               as text, everything else stays escaped so it round-trips. */
            ABytes *by = (ABytes *)obj_of(v);
            const char *p = by->data ? by->data : "";
            uint64_t n = (uint64_t)by->h.len;
            int text = a_utf8_valid(p, n) && !quote;
            if (text) {
                buf_addn(b, p, (size_t)n);
                return;
            }
            buf_addc(b, 'b');
            buf_addc(b, '"');
            uint64_t i = 0;
            while (i < n) {
                /* memcpy runs of printable bytes; escape the rest. */
                uint64_t j = i;
                while (j < n) {
                    unsigned char c = (unsigned char)p[j];
                    if (c == '"' || c == '\\' || c == '\n' || c == '\t' ||
                        c == '\r' || c < 0x20 || c >= 0x7f)
                        break;
                    ++j;
                }
                if (j > i) buf_addn(b, p + i, (size_t)(j - i));
                if (j >= n) break;
                unsigned char c = (unsigned char)p[j];
                switch (c) {
                    case '"': buf_adds(b, "\\\""); break;
                    case '\\': buf_adds(b, "\\\\"); break;
                    case '\n': buf_adds(b, "\\n"); break;
                    case '\t': buf_adds(b, "\\t"); break;
                    case '\r': buf_adds(b, "\\r"); break;
                    default: {
                        static const char hexd[] = "0123456789abcdef";
                        char hex[4] = {'\\', 'x', hexd[c >> 4], hexd[c & 15]};
                        buf_addn(b, hex, 4);
                        break;
                    }
                }
                i = j + 1;
            }
            buf_addc(b, '"');
            return;
        }
        case K_CHAR: {
            /* Chars display unquoted even inside containers (docs:
               s.chars() yields char values, distinct from strings). */
            uint64_t n;
            const char *p = str_data(v, &n);
            buf_addn(b, p, (size_t)n);
            return;
        }
        case K_VIEW: {
            /* Read-only element access (arr_elem handles packed storage):
               formatting must not mutate the value — str()/repr() are on
               the parallel-builtin whitelist, so they may run on pool
               workers where arr_ensure_aval's in-place packed->aval
               conversion would race. */
            AView *vw = (AView *)obj_of(v);
            AArr *a = (AArr *)obj_of(vw->parent);
            buf_addc(b, vw->is_tuple ? '(' : '[');
            for (int64_t i = 0; i < vw->len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, arr_elem(a, (uint64_t)(vw->off + i)), 1);
            }
            buf_addc(b, vw->is_tuple ? ')' : ']');
            return;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(v);
            buf_addc(b, '[');
            if (kind_of(v) == K_TUPLE) b->p[b->len - 1] = '(';
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, arr_elem(a, i), 1);
            }
            buf_addc(b, kind_of(v) == K_TUPLE ? ')' : ']');
            return;
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(v);
            AObj *ho = obj_of(v);
            if (ho && ho->type == T_STRUCT) {
                /* Class instance: Name(field=value, ...) like Python repr. */
                uint64_t nl;
                const char *np = str_data(((AStruct *)ho)->name, &nl);
                buf_addn(b, np, (size_t)nl);
                buf_addc(b, '(');
                for (uint64_t i = 0; i < d->h.len; ++i) {
                    if (i) buf_adds(b, ", ");
                    format_into(b, d->pairs[i].k, 0);
                    buf_adds(b, "=");
                    format_into(b, d->pairs[i].v, 1);
                }
                buf_addc(b, ')');
                return;
            }
            buf_addc(b, '{');
            for (uint64_t i = 0; i < d->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, d->pairs[i].k, 1);
                buf_adds(b, ": ");
                format_into(b, d->pairs[i].v, 1);
            }
            buf_addc(b, '}');
            return;
        }
        case K_SET: {
            AArr *a = (AArr *)obj_of(v);
            if (a->h.len == 0) { buf_adds(b, "set()"); return; }
            buf_addc(b, '{');
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, arr_elem(a, i), 1);
            }
            buf_addc(b, '}');
            return;
        }
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            buf_addi64(b, r->lo);
            buf_adds(b, "..");
            buf_addi64(b, r->hi);
            if (r->step != 1) {
                buf_adds(b, "..");
                buf_addi64(b, r->step);
            }
            return;
        }
        case K_FNON: {
            AFnon *f = (AFnon *)obj_of(v);
            buf_adds(b, "<function");
            if (f->name && f->name[0]) {
                buf_addc(b, ' ');
                buf_adds(b, f->name);
            }
            buf_addc(b, '>');
            return;
        }
        case K_RESULT: {
            AResult *r = (AResult *)obj_of(v);
            buf_adds(b, r->is_option ? "Some(" : (r->ok ? "Ok(" : "Err("));
            format_into(b, r->value, 1);
            buf_addc(b, ')');
            return;
        }
        default: {
            AObj *o = obj_of(v);
            if (o && o->type == T_PYBUF) {
                APyBuf *pb = (APyBuf *)o;
                buf_adds(b, "<pybuffer ");
                buf_addi64(b, pb->count);
                buf_adds(b, pb->readonly ? " readonly>" : ">");
                return;
            }
            if (o && o->type == T_PYOBJ) {
                /* Render via Python str()/repr() so opaque objects print
                   like they would in the interpreter. */
                aval s = quote ? a_py_repr(v) : a_py_str(v);
                format_into(b, s, quote);
                return;
            }
            buf_adds(b, "<object>");
            return;
        }
    }
}

void format_into(Buf *b, aval v, int quote) {
    if (fmt_is_container(v)) {
        AObj *o = obj_of(v);
        if (o) {
            if (fmt_active_has(o)) { buf_adds(b, "..."); return; }
            fmt_active_push(o);
            format_into_body(b, v, quote);
            g_fmt_active_len--;
            return;
        }
    }
    format_into_body(b, v, quote);
}

static aval format_aval(aval v, int quote) {
    Buf b;
    buf_init(&b);
    format_into(&b, v, quote);
    return buf_take(&b);
}

static aval a_str_i64(int64_t v) {
    /* The cached small-int strings below are arena objects held only in
       g_int_str_cache; install the GC root callback that pins them before
       the first cache store, or a collection can reclaim+munmap their arena
       chunk while the cache still points into it (use-after-munmap). */
    fmt_active_gc_hook();
    int ci = (v >= 0 && v < 4096) ? (int)v : -1;
    if (ci >= 0 && g_int_str_cache[ci]) return g_int_str_cache[ci];
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    uint64_t t = u;
    int nd = 1;
    while (t >= 10) { t /= 10; ++nd; }
    if (g_small_str && a_gc_arena_stale(g_small_str)) g_small_str = NULL;
    size_t len = (size_t)nd + (v < 0 ? 1 : 0);
    size_t need = (sizeof(AStr) + len + 1 + 15) & ~(size_t)15;
    size_t avail = g_small_str ? (size_t)(g_small_str_end - g_small_str) : 0;
    if (avail < need) {
        void *old = g_small_str;
        g_small_str = xarena(kSmallStrChunk);
        a_gc_arena_switch(old, g_small_str);
        g_small_str_end = g_small_str + kSmallStrChunk;
    }
    AStr *s = (AStr *)g_small_str;
    g_small_str += need;
    s->h.type = T_STR;
    s->h.len = (uint32_t)len;
    s->h.cap = len + 1;
    astr_cp_cache(s, (uint32_t)len);  /* digits and '-' are ASCII */
    char *p = s->data + len;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    s->data[len] = '\0';
    aval r = mkobj(&s->h);
    if (ci >= 0) g_int_str_cache[ci] = r;
    return r;
}

/* Cached "None"/"true"/"false" strings for str()/repr() of the three
   singleton kinds — same lifetime rules as g_int_str_cache (arena objects
   pinned by the GC root callback below). g_kw_strs is declared at the top
   of the file so fmt_active_gc_roots can mark it. */
static const char *const kKwStrs[3] = { "None", "true", "false" };

static aval a_str_kw(int which) {
    fmt_active_gc_hook();
    if (!g_kw_strs[which])
        g_kw_strs[which] = a_str_new(kKwStrs[which], strlen(kKwStrs[which]));
    return g_kw_strs[which];
}

/* Scalar fast path shared by a_str/a_repr: kinds whose rendering is a
   short fixed string or a direct byte copy skip the Buf growth +
   buf_take double copy. Containers and other kinds still go through
   format_aval. `quote` only matters for K_STR (repr quotes it). */
static aval a_str_scalar(aval v, int quote) {
    int k = kind_of(v);
    if (k == K_STR) {
        if (!quote) return v;   /* str(s) is s; immutable -> alias safe */
        return A_NONE;          /* sentinel: caller falls through */
    }
    if (k == K_CHAR) {
        /* Chars print unquoted even under repr(): same bytes as str(). */
        uint64_t n;
        const char *p = str_data(v, &n);
        return a_str_new(p ? p : "", n);
    }
    if (k == K_BOOL) return a_str_kw(v == A_TRUE ? 1 : 2);
    if (k == K_NONE) return a_str_kw(0);
    if (k == K_FLOAT) {
        char tmp[72];
        return a_str_new(tmp, fmt_double_render(a_double_val(v), tmp));
    }
    return A_NONE;
}

aval a_str(aval v) {
    if (is_int_val(v)) return a_str_i64(int_val_full(v));
    aval s = a_str_scalar(v, 0);
    if (s != A_NONE) return s;
    return format_aval(v, 0);
}
aval a_repr(aval v) {
    aval s = a_str_scalar(v, 1);
    if (s != A_NONE) return s;
    return format_aval(v, 1);
}

aval a_list_push_int_str(aval list, int64_t v) {
    AArr *a = arr_of(list, "list");
    aval s = (v >= 0 && v < 4096 && g_int_str_cache[v])
        ? g_int_str_cache[v] : a_str_i64(v);
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (arr_packed(a)) {
        arr_push(a, s);
        return list;
    }
    arr_f64_unmark(a);   /* string avals may be pointers: restore scanning */
    if (a->h.len + 1 > a->h.cap) arr_reserve(a, (uint64_t)a->h.len + 1);
    a->items[a->h.len++] = s;
    return list;
}

aval a_list_fill_str_range(aval list, int64_t lo, int64_t hi, int64_t step) {
    if (step == 0) panicf("range() step must be positive, found 0.");
    AArr *a = arr_of(list, "list");
    if (arr_packed(a)) arr_ensure_aval(a);
    arr_f64_unmark(a);
    uint64_t count = 0;
    if (step > 0) {
        if (lo < hi) {
            uint64_t diff = (uint64_t)hi - (uint64_t)lo;
            uint64_t st = (uint64_t)step;
            count = diff / st + (diff % st != 0);
        }
    } else if (lo > hi) {
        uint64_t diff = (uint64_t)lo - (uint64_t)hi;
        uint64_t st = (uint64_t)(-(step + 1)) + 1;
        count = diff / st + (diff % st != 0);
    }
    if (count && count <= (1u << 20)) arr_reserve(a, count);
    aval *items = a->items;
    uint64_t len = a->h.len;
    for (int64_t cur = lo; step > 0 ? cur < hi : cur > hi;) {
        aval s = ((uint64_t)cur < 4096 && g_int_str_cache[cur])
            ? g_int_str_cache[cur] : a_str_i64(cur);
        if (len == UINT32_MAX) panicf("array too large");
        if (len + 1 > a->h.cap) {
            arr_reserve(a, len + 1);
            items = a->items;
        }
        items[len++] = s;
        int64_t lim = step > 0
            ? (int64_t)((uint64_t)INT64_MAX - (uint64_t)step)
            : (int64_t)((uint64_t)INT64_MIN - (uint64_t)step);
        if (step > 0 ? cur > lim : cur < lim) cur = hi;
        else cur += step;
    }
    a->items = items;
    a->h.len = (uint32_t)len;
    return list;
}

aval a_set_add_int_str(aval s, int64_t v) {
    return a_set_add(s, a_str_i64(v));
}

