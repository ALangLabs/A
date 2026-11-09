#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* json module: JSON text <-> aval values.                              */
/* JSON objects map to dicts, arrays to lists, strings to str, numbers  */
/* to int/float, true/false to bool, null to None.                      */
/* ------------------------------------------------------------------ */

static aval json_unwrap(aval v) {
    AObj *o = obj_of(v);
    if (o && o->type == T_RESULT && ((AResult *)o)->is_option)
        return ((AResult *)o)->value;
    return v;
}

static const char *json_kind_of(aval v) {
    v = json_unwrap(v);
    if (a_is_none(v)) return "null";
    if (v == A_TRUE || v == A_FALSE) return "bool";
    if (is_int_val(v)) return "int";
    if (a_is_double(v)) return "float";
    AObj *o = obj_of(v);
    if (!o) return "unknown";
    switch (o->type) {
        case T_STR: case T_CHAR: return "string";
        case T_LIST: case T_TUPLE: case T_SET: return "array";
        case T_DICT: case T_STRUCT: return "object";
        default: return "unknown";
    }
}

typedef struct { const char *p, *end; int err; } JParser;

static void jws(JParser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ++ps->p;
}
static int jpeek(JParser *ps) {
    jws(ps);
    return ps->p < ps->end ? (unsigned char)*ps->p : -1;
}
static int jexpect(JParser *ps, char c) {
    if (jpeek(ps) == c) { ++ps->p; return 1; }
    ps->err = 1;
    return 0;
}

static void jutf8(Buf *b, uint32_t cp) {
    if (cp < 0x80) {
        buf_addc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_addc(b, (char)(0xC0 | (cp >> 6)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_addc(b, (char)(0xE0 | (cp >> 12)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_addc(b, (char)(0xF0 | (cp >> 18)));
        buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int jhex4(JParser *ps) {
    if (ps->end - ps->p < 4) { ps->err = 1; return 0; }
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else { ps->err = 1; return 0; }
    }
    return v;
}

static aval jstring(JParser *ps) {
    if (!jexpect(ps, '"')) return A_NONE;
    /* No-escape fast path: the whole string is a slice of the input, so it
       can be copied straight into a String instead of grown byte by byte
       through a Buf. Keys and most values have no escapes. */
    {
        const char *start = ps->p;
        const char *q = start;
        int escaped = 0;
        while (q < ps->end) {
            unsigned char ch = (unsigned char)*q;
            if (ch == '"') break;
            if (ch == '\\') { escaped = 1; break; }
            if (ch < 0x20) { ps->err = 1; return A_NONE; }
            ++q;
        }
        if (q >= ps->end) { ps->err = 1; return A_NONE; }
        if (!escaped) {
            aval out = a_str_new(start, (uint64_t)(q - start));
            ps->p = q + 1;
            return out;
        }
        ps->p = start;  /* rewind: the escape path below re-reads from here */
    }
    Buf b;
    buf_init(&b);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') return buf_take(&b);
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case '"': buf_addc(&b, '"'); break;
                case '\\': buf_addc(&b, '\\'); break;
                case '/': buf_addc(&b, '/'); break;
                case 'b': buf_addc(&b, '\b'); break;
                case 'f': buf_addc(&b, '\f'); break;
                case 'n': buf_addc(&b, '\n'); break;
                case 'r': buf_addc(&b, '\r'); break;
                case 't': buf_addc(&b, '\t'); break;
                case 'u': {
                    uint32_t cp = (uint32_t)jhex4(ps);
                    if (ps->err) break;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        uint32_t lo = (uint32_t)jhex4(ps);
                        if (ps->err) break;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    jutf8(&b, cp);
                    break;
                }
                default: ps->err = 1; break;
            }
        } else {
            if (c < 0x20) { ps->err = 1; break; }
            buf_addc(&b, (char)c);
        }
        if (ps->err) break;
    }
    free(b.p);
    ps->err = 1;
    return A_NONE;
}

static aval jnumber(JParser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ++ps->p;
    const char *int_start = ps->p;
    int digits = 0;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') { ++ps->p; ++digits; }
    if (!digits ||
        (int_start[0] == '0' && ps->p - int_start > 1)) {
        ps->err = 1;
        return A_NONE;
    }
    int isDouble = 0;
    if (ps->p < ps->end && *ps->p == '.') {
        isDouble = 1;
        ++ps->p;
        int frac = 0;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') { ++ps->p; ++frac; }
        if (!frac) { ps->err = 1; return A_NONE; }
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        isDouble = 1;
        ++ps->p;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ++ps->p;
        int exp = 0;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') { ++ps->p; ++exp; }
        if (!exp) { ps->err = 1; return A_NONE; }
    }
    size_t n = (size_t)(ps->p - start);
    char tmp[64];
    char *heap = NULL;
    char *num = tmp;
    if (n >= sizeof(tmp)) {
        heap = (char *)xmalloc(n + 1);
        num = heap;
    }
    memcpy(num, start, n);
    num[n] = '\0';
    if (isDouble) {
        double d = strtod(num, NULL);
        free(heap);
        return mkdouble(d);
    }
    errno = 0;
    long long v = strtoll(num, NULL, 10);
    if (errno == ERANGE) {
        double d = strtod(num, NULL);
        free(heap);
        return mkdouble(d);
    }
    free(heap);
    return mkint_full(v);
}

static aval jvalue(JParser *ps);

static aval jobject(JParser *ps) {
    jexpect(ps, '{');
    aval d = a_dict_new();
    if (jpeek(ps) == '}') { ++ps->p; return d; }
    for (;;) {
        if (jpeek(ps) != '"') { ps->err = 1; break; }
        aval k = jstring(ps);
        if (ps->err) break;
        if (!jexpect(ps, ':')) break;
        aval v = jvalue(ps);
        if (ps->err) break;
        dict_put(dict_of(d, "json"), k, v);
        int c = jpeek(ps);
        if (c == ',') { ++ps->p; continue; }
        if (c == '}') { ++ps->p; break; }
        ps->err = 1;
        break;
    }
    if (ps->err) return A_NONE;
    return d;
}

static aval jarray(JParser *ps) {
    jexpect(ps, '[');
    aval a = a_list_new(0, NULL);
    AArr *arr = arr_of(a, "json");
    if (jpeek(ps) == ']') { ++ps->p; return a; }
    for (;;) {
        aval v = jvalue(ps);
        if (ps->err) return A_NONE;
        arr_push(arr, v);
        int c = jpeek(ps);
        if (c == ',') { ++ps->p; continue; }
        if (c == ']') { ++ps->p; break; }
        ps->err = 1;
        break;
    }
    return ps->err ? A_NONE : a;
}

static aval jvalue(JParser *ps) {
    int c = jpeek(ps);
    if (c < 0) { ps->err = 1; return A_NONE; }
    if (c == '"') return jstring(ps);
    if (c == '{') return jobject(ps);
    if (c == '[') return jarray(ps);
    if (c == 't') {
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
            ps->p += 4; jws(ps); return A_TRUE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == 'f') {
        if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
            ps->p += 5; jws(ps); return A_FALSE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == 'n') {
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
            ps->p += 4; jws(ps); return A_NONE;
        }
        ps->err = 1; return A_NONE;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jnumber(ps);
    ps->err = 1;
    return A_NONE;
}

/* ---- JSON serializer ---- */

static int json_needs_escape(unsigned char c) {
    return c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' ||
           c == '\r' || c == '\t' || c < 0x20;
}

static void json_escape_str(Buf *b, aval v) {
    uint64_t n;
    const char *s = str_data(v, &n);
    static const char hexd[] = "0123456789abcdef";
    buf_addc(b, '"');
    uint64_t i = 0;
    while (i < n) {
        /* memcpy runs of unescaped bytes instead of per-byte buf_addc. */
        uint64_t j = i;
        while (j < n && !json_needs_escape((unsigned char)s[j])) ++j;
        if (j > i) buf_addn(b, s + i, (size_t)(j - i));
        if (j >= n) break;
        unsigned char c = (unsigned char)s[j];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\b': buf_adds(b, "\\b"); break;
            case '\f': buf_adds(b, "\\f"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\r': buf_adds(b, "\\r"); break;
            case '\t': buf_adds(b, "\\t"); break;
            default: {
                /* c < 0x20 -> \u00XX (lowercase hex, same as %04x) */
                char tmp[6] = {'\\', 'u', '0', '0',
                               hexd[c >> 4], hexd[c & 15]};
                buf_addn(b, tmp, 6);
                break;
            }
        }
        i = j + 1;
    }
    buf_addc(b, '"');
}

static void json_write(Buf *b, aval v, int pretty, int depth);

static void json_indent(Buf *b, int pretty, int depth) {
    if (!pretty) return;
    buf_addc(b, '\n');
    for (int i = 0; i < depth; ++i) buf_adds(b, "  ");
}

/* Depth guard against cyclic input: json_write recurses structurally and
   a self-referential dict/list would otherwise recurse until the C stack
   overflows (same failure format_into's g_fmt_active guard prevents).
   1000 matches the practical nesting depth json.parse itself tolerates. */
#define JSON_WRITE_MAX_DEPTH 1000

static void json_write(Buf *b, aval v, int pretty, int depth) {
    if (depth > JSON_WRITE_MAX_DEPTH)
        panicf("json: value nesting too deep (possible cycle)");
    v = json_unwrap(v);
    if (a_is_none(v)) { buf_adds(b, "null"); return; }
    if (v == A_TRUE) { buf_adds(b, "true"); return; }
    if (v == A_FALSE) { buf_adds(b, "false"); return; }
    if (is_int_val(v)) { buf_addi64(b, int_val_full(v)); return; }
    if (a_is_double(v)) {
        double d = a_double_val(v);
        if (!isfinite(d)) { buf_adds(b, "null"); return; }
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        buf_adds(b, tmp);
        return;
    }
    AObj *o = obj_of(v);
    if (!o) panicf("json: value is not serializable");
    switch (o->type) {
        case T_STR: case T_CHAR:
            json_escape_str(b, v);
            return;
        case T_LIST: case T_TUPLE: case T_SET: {
            AArr *a = (AArr *)o;
            arr_ensure_aval(a);
            buf_addc(b, '[');
            for (uint32_t i = 0; i < a->h.len; ++i) {
                if (i) buf_adds(b, pretty ? "," : ", ");
                json_indent(b, pretty, depth + 1);
                json_write(b, a->items[i], pretty, depth + 1);
            }
            if (a->h.len) json_indent(b, pretty, depth);
            buf_addc(b, ']');
            return;
        }
        case T_DICT: case T_STRUCT: {
            ADict *d = (ADict *)o;
            buf_addc(b, '{');
            for (uint32_t i = 0; i < d->h.len; ++i) {
                if (i) buf_adds(b, pretty ? "," : ", ");
                json_indent(b, pretty, depth + 1);
                aval k = d->pairs[i].k;
                if (kind_of(k) == K_STR || kind_of(k) == K_CHAR) {
                    json_escape_str(b, k);
                } else {
                    aval ks = a_str(k);
                    json_escape_str(b, ks);
                }
                buf_adds(b, ": ");
                json_write(b, d->pairs[i].v, pretty, depth + 1);
            }
            if (d->h.len) json_indent(b, pretty, depth);
            buf_addc(b, '}');
            return;
        }
        default:
            panicf("json: value of type '%s' is not serializable", type_name_cstr(v));
    }
}

static aval bi_json_dump(uint64_t n, aval *argv, int pretty) {
    want_args(pretty ? "json.pretty" : "json.stringify", n, 1, 1);
    Buf b;
    buf_init(&b);
    json_write(&b, argv[0], pretty, 0);
    return buf_take(&b);
}
aval bi_json_stringify(uint64_t n, aval *argv) { return bi_json_dump(n, argv, 0); }
aval bi_json_pretty(uint64_t n, aval *argv) { return bi_json_dump(n, argv, 1); }

aval bi_json_parse(uint64_t n, aval *argv) {
    want_args("json.parse", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    JParser ps = { s, s + len, 0 };
    aval v = jvalue(&ps);
    jws(&ps);
    if (ps.err || ps.p != ps.end)
        panicf("json.parse: invalid JSON");
    return v;
}

aval bi_json_is_valid(uint64_t n, aval *argv) {
    want_args("json.is_valid", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    JParser ps = { s, s + len, 0 };
    (void)jvalue(&ps);
    jws(&ps);
    return bool_aval(!ps.err && ps.p == ps.end);
}

aval bi_json_kind(uint64_t n, aval *argv) {
    want_args("json.kind", n, 1, 1);
    return a_str_new(json_kind_of(argv[0]), strlen(json_kind_of(argv[0])));
}

aval bi_json_get(uint64_t n, aval *argv) {
    want_args("json.get", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || !is_dict_t(o->type)) return A_NONE;
    int64_t i = dict_find((ADict *)o, argv[1]);
    return i >= 0 ? a_some_new(((ADict *)o)->pairs[i].v) : A_NONE;
}

aval bi_json_at(uint64_t n, aval *argv) {
    want_args("json.at", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || (o->type != T_LIST && o->type != T_TUPLE)) return A_NONE;
    AArr *a = (AArr *)o;
    int64_t idx = expect_int64(argv[1], "json.at index");
    if (idx < 0) idx += (int64_t)a->h.len;
    if (idx < 0 || (uint64_t)idx >= a->h.len) return A_NONE;
    return a_some_new(arr_elem(a, (uint64_t)idx));
}

aval bi_json_len(uint64_t n, aval *argv) {
    want_args("json.len", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET ||
              is_dict_t(o->type)))
        return mkint_full((int64_t)o->len);
    if (o && (o->type == T_STR || o->type == T_CHAR)) {
        uint64_t sl;
        const char *sp = str_data(v, &sl);
        return mkint_full((int64_t)u8_count(sp, sl));
    }
    return mkint_full(0);
}

aval bi_json_has(uint64_t n, aval *argv) {
    want_args("json.has", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || !is_dict_t(o->type)) return A_FALSE;
    return bool_aval(dict_find((ADict *)o, argv[1]) >= 0);
}

aval bi_json_keys(uint64_t n, aval *argv) {
    want_args("json.keys", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    aval out = a_list_new(0, NULL);
    if (o && is_dict_t(o->type)) {
        ADict *d = (ADict *)o;
        AArr *arr = arr_of(out, "json.keys");
        for (uint32_t i = 0; i < d->h.len; ++i) {
            aval k = d->pairs[i].k;
            if (kind_of(k) != K_STR && kind_of(k) != K_CHAR) k = a_str(k);
            arr_push(arr, k);
        }
    }
    return out;
}

aval bi_json_as_str(uint64_t n, aval *argv) {
    want_args("json.as_str", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_STR || o->type == T_CHAR)) return a_some_new(v);
    return A_NONE;
}

aval bi_json_as_int(uint64_t n, aval *argv) {
    want_args("json.as_int", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (is_int_val(v)) return a_some_new(mkint_full(int_val_full(v)));
    if (a_is_double(v)) {
        double d = a_double_val(v);
        if (isfinite(d) && d == floor(d) && d >= -9.0e18 && d <= 9.0e18)
            return a_some_new(mkint_full((int64_t)d));
    }
    return A_NONE;
}

aval bi_json_as_float(uint64_t n, aval *argv) {
    want_args("json.as_float", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (is_int_val(v)) return a_some_new(mkdouble((double)int_val_full(v)));
    if (a_is_double(v)) return a_some_new(v);
    return A_NONE;
}

aval bi_json_as_bool(uint64_t n, aval *argv) {
    want_args("json.as_bool", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    if (v == A_TRUE || v == A_FALSE) return a_some_new(v);
    return A_NONE;
}

aval bi_json_as_array(uint64_t n, aval *argv) {
    want_args("json.as_array", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET))
        return a_some_new(v);
    return A_NONE;
}

aval bi_json_as_map(uint64_t n, aval *argv) {
    want_args("json.as_map", n, 1, 1);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (o && is_dict_t(o->type)) return a_some_new(v);
    return A_NONE;
}


aval bi_json_set(uint64_t n, aval *argv) {
    want_args("json.set", n, 3, 3);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || !is_dict_t(o->type))
        panicf("json.set: value must be an object, not '%s'", type_name_cstr(v));
    dict_put((ADict *)o, argv[1], argv[2]);
    return argv[2];
}

aval bi_json_delete(uint64_t n, aval *argv) {
    want_args("json.delete", n, 2, 2);
    aval v = json_unwrap(argv[0]);
    AObj *o = obj_of(v);
    if (!o || !is_dict_t(o->type))
        panicf("json.delete: value must be an object, not '%s'", type_name_cstr(v));
    ADict *d = (ADict *)o;
    int64_t i = dict_find(d, argv[1]);
    if (i < 0) return A_FALSE;
    dict_remove_at(d, (uint64_t)i);
    return A_TRUE;
}
