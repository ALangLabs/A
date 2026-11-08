#include "a_runtime_internal.h"

static aval g_int_str_cache[4096];
/* 2 MiB (one huge page) per chunk, from the mmap arena. */
static const size_t kSmallStrChunk = 2u << 20;
static char *g_small_str = NULL;
static char *g_small_str_end = NULL;

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
        case T_STR: return K_STR;
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

int a_obj_is_instance(aval v, const char *type_name) {
    if (!type_name) return 0;
    return strcmp(type_name_cstr(v), type_name) == 0;
}


static void format_double_buf(Buf *b, double d) {
    if (isnan(d)) { buf_adds(b, "nan"); return; }
    if (isinf(d)) { buf_adds(b, d < 0 ? "-inf" : "inf"); return; }
    if (d == 0.0) { buf_adds(b, signbit(d) ? "-0.0" : "0.0"); return; }
    char tmp[64];
    for (int prec = 1; prec <= 17; ++prec) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, d);
        if (strtod(tmp, NULL) == d) break;
    }
    /* Avoid scientific notation for ordinary magnitudes (Python-style);
       expand to plain decimal that still round-trips. */
    if (strchr(tmp, 'e') || strchr(tmp, 'E')) {
        double ad = d < 0.0 ? -d : d;
        if (ad >= 1e-4 && ad < 1e16) {
            for (int prec = 1; prec <= 25; ++prec) {
                snprintf(tmp, sizeof(tmp), "%.*f", prec, d);
                if (strtod(tmp, NULL) == d) break;
            }
        }
    }
    buf_adds(b, tmp);
    if (strchr(tmp, '.') == NULL && strchr(tmp, 'e') == NULL &&
        strchr(tmp, 'E') == NULL)
        buf_adds(b, ".0");
}

static void format_quoted(Buf *b, const char *p, size_t n) {
    buf_addc(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\t': buf_adds(b, "\\t"); break;
            case '\r': buf_adds(b, "\\r"); break;
            default: buf_addc(b, (char)c); break;
        }
    }
    buf_addc(b, '"');
}

void format_into(Buf *b, aval v, int quote) {
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
                for (uint64_t i = 0; i < n; ++i) buf_addc(b, p[i]);
                return;
            }
            buf_addc(b, 'b');
            buf_addc(b, '"');
            for (uint64_t i = 0; i < n; ++i) {
                unsigned char c = (unsigned char)p[i];
                switch (c) {
                    case '"': buf_adds(b, "\\\""); break;
                    case '\\': buf_adds(b, "\\\\"); break;
                    case '\n': buf_adds(b, "\\n"); break;
                    case '\t': buf_adds(b, "\\t"); break;
                    case '\r': buf_adds(b, "\\r"); break;
                    default:
                        if (c >= 0x20 && c < 0x7f) buf_addc(b, (char)c);
                        else {
                            char hex[8];
                            snprintf(hex, sizeof(hex), "\\x%02x", c);
                            buf_adds(b, hex);
                        }
                        break;
                }
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
            AView *vw = (AView *)obj_of(v);
            AArr *a = (AArr *)obj_of(vw->parent);
            arr_ensure_aval(a);
            buf_addc(b, vw->is_tuple ? '(' : '[');
            for (int64_t i = 0; i < vw->len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, a->items[(size_t)(vw->off + i)], 1);
            }
            buf_addc(b, vw->is_tuple ? ')' : ']');
            return;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(v);
            arr_ensure_aval(a);
            buf_addc(b, '[');
            if (kind_of(v) == K_TUPLE) b->p[b->len - 1] = '(';
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, ", ");
                format_into(b, a->items[i], 1);
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
                format_into(b, a->items[i], 1);
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

static aval format_aval(aval v, int quote) {
    Buf b;
    buf_init(&b);
    format_into(&b, v, quote);
    return buf_take(&b);
}

static aval a_str_i64(int64_t v) {
    int ci = (v >= 0 && v < 4096) ? (int)v : -1;
    if (ci >= 0 && g_int_str_cache[ci]) return g_int_str_cache[ci];
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    uint64_t t = u;
    int nd = 1;
    while (t >= 10) { t /= 10; ++nd; }
    size_t len = (size_t)nd + (v < 0 ? 1 : 0);
    size_t need = (sizeof(AStr) + len + 1 + 15) & ~(size_t)15;
    size_t avail = g_small_str ? (size_t)(g_small_str_end - g_small_str) : 0;
    if (avail < need) {
        g_small_str = xarena(kSmallStrChunk);
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

aval a_str(aval v) {
    if (is_int_val(v)) return a_str_i64(int_val_full(v));
    return format_aval(v, 0);
}
aval a_repr(aval v) { return format_aval(v, 1); }

aval a_list_push_int_str(aval list, int64_t v) {
    AArr *a = arr_of(list, "list");
    aval s = (v >= 0 && v < 4096 && g_int_str_cache[v])
        ? g_int_str_cache[v] : a_str_i64(v);
    if (a->h.len == UINT32_MAX) panicf("array too large");
    if (arr_packed(a)) {
        arr_push(a, s);
        return list;
    }
    if (a->h.len + 1 > a->h.cap) arr_reserve(a, (uint64_t)a->h.len + 1);
    a->items[a->h.len++] = s;
    return list;
}

aval a_list_fill_str_range(aval list, int64_t lo, int64_t hi, int64_t step) {
    if (step == 0) panicf("range() step must be positive, found 0.");
    AArr *a = arr_of(list, "list");
    if (arr_packed(a)) arr_ensure_aval(a);
    a->flags &= ~ARR_F_F64;
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

