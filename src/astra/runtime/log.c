#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* log module: leveled logging                                         */
/* ------------------------------------------------------------------ */

static int g_log_level = 1;   /* 0=debug 1=info 2=warn 3=error */
static int g_log_json = 0;
static const char *const k_log_level_names[] = {"debug", "info", "warn", "error"};

static void log_append_json_str(Buf *b, const char *s, uint64_t n) {
    buf_addc(b, '"');
    for (uint64_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\b': buf_adds(b, "\\b"); break;
            case '\f': buf_adds(b, "\\f"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\r': buf_adds(b, "\\r"); break;
            case '\t': buf_adds(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof tmp, "\\u%04x", c);
                    buf_adds(b, tmp);
                } else {
                    buf_addc(b, (char)c);
                }
        }
    }
    buf_addc(b, '"');
}

static void log_write_line(int level, const char *msg, uint64_t mlen, aval fields) {
    FILE *f = stderr;
    if (g_log_json) {
        Buf b;
        buf_init(&b);
        buf_addc(&b, '{');
        buf_adds(&b, "\"level\":\"");
        buf_adds(&b, k_log_level_names[level]);
        buf_adds(&b, "\",\"msg\":");
        log_append_json_str(&b, msg, mlen);
        if (fields != A_NONE) {
            buf_adds(&b, ",\"fields\":");
            aval one[1] = { fields };
            aval js = bi_json_stringify(1, one);
            uint64_t fl;
            const char *fp = str_data(js, &fl);
            buf_addn(&b, fp, (size_t)fl);
        }
        buf_adds(&b, "}\n");
        fwrite(b.p, 1, (size_t)b.len, f);
        free(b.p);
        return;
    }
    fprintf(f, "%s %.*s", k_log_level_names[level], (int)mlen, msg);
    if (fields != A_NONE) {
        Buf b;
        buf_init(&b);
        format_into(&b, fields, 0);
        fprintf(f, " %s", b.p ? b.p : "");
        free(b.p);
    }
    fputc('\n', f);
}

aval bi_log_set_level(uint64_t n, aval *argv) {
    want_args("log.set_level", n, 1, 1);
    uint64_t len;
    const char *s = str_data(argv[0], &len);
    for (int i = 0; i < 4; ++i) {
        if (strlen(k_log_level_names[i]) == len && !memcmp(s, k_log_level_names[i], len)) {
            g_log_level = i;
            return A_NONE;
        }
    }
    panicf("log.set_level: unknown level '%.*s'", (int)len, s);
    return A_NONE;
}

aval bi_log_set_json(uint64_t n, aval *argv) {
    want_args("log.set_json", n, 1, 1);
    g_log_json = kind_of(argv[0]) == K_BOOL ? (argv[0] == A_TRUE) : (int_val_full(argv[0]) != 0);
    return A_NONE;
}

static aval bi_log_emit(int level, uint64_t n, aval *argv) {
    want_args(k_log_level_names[level], n, 1, 2);
    if (level < g_log_level) return A_NONE;
    uint64_t mlen;
    const char *msg = str_data(argv[0], &mlen);
    aval fields = n > 1 ? argv[1] : A_NONE;
    log_write_line(level, msg, mlen, fields);
    return A_NONE;
}
aval bi_log_debug(uint64_t n, aval *argv) { return bi_log_emit(0, n, argv); }
aval bi_log_info(uint64_t n, aval *argv) { return bi_log_emit(1, n, argv); }
aval bi_log_warn(uint64_t n, aval *argv) { return bi_log_emit(2, n, argv); }
aval bi_log_error(uint64_t n, aval *argv) { return bi_log_emit(3, n, argv); }

