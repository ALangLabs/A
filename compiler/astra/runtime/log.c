#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* log module: leveled logging                                         */
/* ------------------------------------------------------------------ */

static int g_log_level = 1;   /* 0=debug 1=info 2=warn 3=error */
static int g_log_json = 0;
static const char *const k_log_level_names[] = {"debug", "info", "warn", "error"};

static void log_write_line(int level, const char *msg, uint64_t mlen, aval fields) {
    FILE *f = stderr;
    if (g_log_json) {
        Buf b;
        buf_init(&b);
        buf_addc(&b, '{');
        buf_adds(&b, "\"level\":\"");
        buf_adds(&b, k_log_level_names[level]);
        buf_adds(&b, "\",\"msg\":");
        /* reuse the JSON string escaper via format_into on the raw string */
        aval ms = a_str_new(msg, mlen);
        format_into(&b, ms, 0);
        if (fields != A_NONE) {
            buf_adds(&b, ",\"fields\":");
            format_into(&b, fields, 0);
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

