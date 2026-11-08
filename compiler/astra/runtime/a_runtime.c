#include "a_runtime_internal.h"


/* builtin table: order must match the enum in a_runtime.h */
static const struct { const char *name; a_builtin_fn fn; } g_builtins[A_BI_COUNT] = {
    { "print", bi_print }, { "eprint", bi_eprint }, { "len", bi_len },
    { "str", bi_str }, { "int", bi_int }, { "float", bi_float },
    { "bool", bi_bool }, { "abs", bi_abs }, { "min", bi_min },
    { "max", bi_max }, { "sum", bi_sum }, { "pow", bi_pow },
    { "sqrt", bi_sqrt }, { "sin", bi_sin }, { "cos", bi_cos },
    { "tan", bi_tan }, { "asin", bi_asin }, { "acos", bi_acos },
    { "atan", bi_atan }, { "exp", bi_exp }, { "log", bi_log },
    { "floor", bi_floor }, { "ceil", bi_ceil }, { "round", bi_round },
    { "trunc", bi_trunc }, { "random", bi_random }, { "random_int", bi_random_int },
    { "range", bi_range }, { "set", bi_set }, { "tuple", bi_tuple },
    { "list", bi_list }, { "dict", bi_dict }, { "map", bi_map },
    { "filter", bi_filter }, { "any", bi_any }, { "all", bi_all },
    { "some", bi_some }, { "ok", bi_ok }, { "err", bi_err },
    { "panic", bi_panic }, { "assert", bi_assert }, { "assert_eq", bi_assert_eq },
    { "input", bi_input }, { "read_file", bi_read_file }, { "write_file", bi_write_file },
    { "append_file", bi_append_file }, { "file_exists", bi_file_exists },
    { "list_dir", bi_list_dir }, { "remove_file", bi_remove_file },
    { "create_dir", bi_create_dir }, { "remove_dir", bi_remove_dir },
    { "rename", bi_rename }, { "current_dir", bi_current_dir },
    { "repr", bi_repr }, { "type", bi_type }, { "isinstance", bi_isinstance },
    { "join", bi_join }, { "sorted", bi_sorted }, { "reversed", bi_reversed },
    { "enumerate", bi_enumerate }, { "zip", bi_zip }, { "reduce", bi_reduce },
    { "log2", bi_log2 }, { "log10", bi_log10 }, { "atan2", bi_atan2 }, { "flush", bi_flush },
    { "i8", bi_i8 }, { "i16", bi_i16 }, { "i32", bi_i32 }, { "u8", bi_u8 },
    { "u16", bi_u16 }, { "u32", bi_u32 }, { "f32", bi_f32 },
    { "ctz", bi_ctz }, { "clz", bi_clz }, { "popcount", bi_popcount },
    { "bswap", bi_bswap }, { "rotl", bi_rotl }, { "rotr", bi_rotr },
    { "fma", bi_fma }, { "fabs", bi_fabs }, { "copysign", bi_copysign },
    { "smin", bi_smin }, { "smax", bi_smax }, { "umin", bi_umin },
    { "umax", bi_umax },
    { "sadd_overflow", bi_sadd_ovf }, { "ssub_overflow", bi_ssub_ovf },
    { "smul_overflow", bi_smul_ovf }, { "uadd_overflow", bi_uadd_ovf },
    { "usub_overflow", bi_usub_ovf }, { "umul_overflow", bi_umul_ovf },
    { "expect", bi_expect }, { "likely", bi_likely }, { "unlikely", bi_unlikely },
    { "env_get", bi_env_get },
    { "now", bi_time_now }, { "sleep", bi_time_sleep },
    { "terminal_isatty", bi_terminal_isatty },
    { "terminal_raw_mode", bi_terminal_raw_mode },
    { "terminal_read_char", bi_terminal_read_char },
    { "terminal_read_char_ms", bi_terminal_read_char_ms },
    { "terminal_write", bi_terminal_write },
    { "terminal_flush", bi_terminal_flush },
    { "terminal_size", bi_terminal_size },
    { "terminal_home", bi_terminal_home },
    { "repl_eval", bi_repl_eval },
    { "json_parse", bi_json_parse }, { "json_stringify", bi_json_stringify },
    { "json_pretty", bi_json_pretty }, { "json_is_valid", bi_json_is_valid },
    { "json_get", bi_json_get }, { "json_at", bi_json_at },
    { "json_len", bi_json_len }, { "json_has", bi_json_has },
    { "json_keys", bi_json_keys }, { "json_type", bi_json_kind },
    { "json_as_str", bi_json_as_str }, { "json_as_int", bi_json_as_int },
    { "json_as_float", bi_json_as_float }, { "json_as_bool", bi_json_as_bool },
    { "json_as_array", bi_json_as_array }, { "json_as_map", bi_json_as_map },
    { "repl_quiet", bi_repl_quiet },
    { "binary_encode", bi_binary_encode }, { "binary_decode", bi_binary_decode },
    { "log_set_level", bi_log_set_level }, { "log_set_json", bi_log_set_json },
    { "log_debug", bi_log_debug }, { "log_info", bi_log_info },
    { "log_warn", bi_log_warn }, { "log_error", bi_log_error },
    { "metrics_counter", bi_metrics_counter }, { "metrics_gauge", bi_metrics_gauge },
    { "metrics_histogram", bi_metrics_histogram }, { "metrics_render", bi_metrics_render },
    { "metrics_reset", bi_metrics_reset },
    { "trace_start", bi_trace_start }, { "trace_end", bi_trace_end },
    { "trace_current", bi_trace_current },
    { "trace_set_attribute", bi_trace_set_attribute },
    { "trace_add_event", bi_trace_add_event },
    { "trace_inject", bi_trace_inject }, { "trace_extract", bi_trace_extract },
    { "trace_spans", bi_trace_spans }, { "trace_reset", bi_trace_reset },
    { "net_resolve", bi_net_resolve }, { "net_tls_available", bi_net_tls_available },
    { "arena_new", bi_arena_new }, { "arena_pool", bi_arena_pool },
    { "arena_region", bi_arena_region }, { "arena_alloc", bi_arena_alloc },
    { "arena_release", bi_arena_release }, { "arena_reset", bi_arena_reset },
    { "arena_used", bi_arena_used }, { "arena_total", bi_arena_total },
    { "unsafe_get", bi_unsafe_get }, { "unsafe_set", bi_unsafe_set },
    { "unsafe_address_of", bi_unsafe_address_of }, { "unsafe_load", bi_unsafe_load },
    { "unsafe_store", bi_unsafe_store }, { "unsafe_offset", bi_unsafe_offset },
    { "py_call", bi_py_call }, { "py_buffer", bi_py_buffer },
    { "py_import_module", bi_py_import_module },
    { "py_try_import_module", bi_py_try_import_module },
    { "py_getattr", bi_py_getattr_builtin },
    { "py_try_getattr", bi_py_try_getattr_builtin },
    { "py_setattr", bi_py_setattr_builtin },
    { "py_hasattr", bi_py_hasattr },
    { "py_delattr", bi_py_delattr },
    { "py_getitem", bi_py_getitem_builtin },
    { "py_setitem", bi_py_setitem_builtin },
    { "py_delitem", bi_py_delitem },
    { "py_call_obj", bi_py_call_obj },
    { "py_try_call_obj", bi_py_try_call_obj },
    { "py_call_method", bi_py_call_method_builtin },
    { "py_try_call_method", bi_py_try_call_method_builtin },
    { "py_is_pyobject", bi_py_is_pyobject },
    { "py_str", bi_py_str_builtin },
    { "py_repr", bi_py_repr_builtin },
    { "py_len", bi_py_len_builtin },
    { "py_contains", bi_py_contains_builtin },
    { "py_truthy", bi_py_truthy_builtin },
    { "py_type", bi_py_type_builtin },
    { "py_buffer_info", bi_py_buffer_info },
    { "json_set", bi_json_set }, { "json_delete", bi_json_delete },
    { "struct_name", bi_struct_name }, { "struct_methods", bi_struct_methods },
    { "fn_signature", bi_fn_signature }, { "fn_doc", bi_fn_doc },
    { "file_size", bi_file_size },
    { "linalg_dot", bi_linalg_dot },
    { "linalg_matvec", bi_linalg_matvec },
    { "linalg_matvec_t", bi_linalg_matvec_t },
    { "net_tcp_connect", bi_net_tcp_connect }, { "net_send", bi_net_send },
    { "net_recv", bi_net_recv }, { "net_close", bi_net_close },
    { "net_set_timeout", bi_net_set_timeout },
    { "net_local_port", bi_net_local_port },
    { "net_tls_connect", bi_net_tls_connect },
    { "net_recv_status", bi_net_recv_status },
    { "read_bytes", bi_read_bytes }, { "write_bytes", bi_write_bytes }
};

static aval g_builtin_objs[A_BI_COUNT];

aval a_builtin(uint64_t id) {
    if (id >= A_BI_COUNT) panicf("invalid builtin id %llu", (unsigned long long)id);
    if (g_builtin_objs[id] == 0)
        g_builtin_objs[id] = fnon_new_raw((uint64_t)(uintptr_t)g_builtins[id].fn,
                                          A_NONE, g_builtins[id].name,
                                          FNON_BUILTIN, NPARAMS_UNKNOWN);
    return g_builtin_objs[id];
}

aval a_bi_call(uint64_t id, uint64_t n, aval *argv) {
    if (id >= A_BI_COUNT) panicf("invalid builtin id %llu", (unsigned long long)id);
    return g_builtins[id].fn(n, argv);
}

int64_t a_int_full(aval v) { return int_val_full(v); }

uint64_t a_builtin_count(void) { return (uint64_t)A_BI_COUNT; }

const char *a_builtin_name(uint64_t id) {
    return id < A_BI_COUNT ? g_builtins[id].name : "<unknown>";
}

/* ------------------------------------------------------------------ */
/* printing / panic / init                                             */
/* ------------------------------------------------------------------ */

static void printv(FILE *f, uint64_t n, aval *argv) {
    for (uint64_t i = 0; i < n; ++i) {
        if (i) fputc(' ', f);
        Buf b;
        buf_init(&b);
        format_into(&b, argv[i], 0);
        if (b.len) fwrite(b.p, 1, b.len, f);
        free(b.p);
    }
    fputc('\n', f);
    fflush(f);
}

aval a_printv(uint64_t n, aval *argv) {
    if (a_rt_repl_quiet()) return A_NONE;
    printv(stdout, n, argv);
    return A_NONE;
}
aval a_eprintv(uint64_t n, aval *argv) {
    if (a_rt_repl_quiet()) return A_NONE;
    printv(stderr, n, argv);
    return A_NONE;
}

aval a_panic_msg(const char *msg) {
    panicf("%s", msg ? msg : "");
    return A_NONE;
}

static int g_runtime_ready = 0;

void a_runtime_init(void) {
    if (g_runtime_ready) return;
    g_runtime_ready = 1;
    srand((unsigned)time(NULL));
}





/* ---- codegen-support helpers (appended) ---- */
aval a_i64n(long long v) { return mkint_full((int64_t)v); }
int a_is_propagate(aval v) {
    if (a_is_none(v)) return 1;
    if (is_result(v) && !result_ok(v)) return 1;
    return 0;
}
aval a_match_fail(aval subj) {
    (void)subj;
    panicf("no match arm matched");
    return A_NONE;
}
aval a_list_push(aval list, aval v) {
    AArr *a = arr_of(list, "list");
    arr_push(a, v);
    return list;
}
aval a_list_append_int(aval list, aval v) {
    AObj *o = obj_of(list);
    if (o && o->type == T_LIST) {
        arr_push_int((AArr *)o, v);
        return A_NONE;
    }
    return a_list_append(list, v);
}
aval a_list_pack(aval v) {
    AObj *o = obj_of(v);
    if (o && o->type == T_LIST) arr_try_pack((AArr *)o);
    return v;
}
aval a_list_append(aval list, aval v) {
    AObj *o = obj_of(list);
    if (o && o->type == T_LIST) {
        arr_push((AArr *)o, v);
        return A_NONE;
    }
    if (o && o->type == T_VIEW)
        panicf("Cannot 'append' a view.");
    aval args[1] = { v };
    return a_method(list, "append", 1, 0, args);
}
aval a_setattr(aval obj, const char *name, aval v) {
    /* field assignment: dicts store by key; opaque Python objects go
       through PyObject_SetAttrString; others panic */
    AObj *o = obj_of(obj);
    if (o && o->type == T_PYOBJ) return a_py_setattr(obj, name, v);
    if (o && is_dict_t(o->type)) {
        aval k = a_str_new(name, (uint64_t)strlen(name));
        a_dict_set(obj, k, v);
        return v;
    }
    panicf("'%s' object has no attribute '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

#if !defined(_WIN32)
/* `a test` harness: fork per test so assertion panics don't stop the suite.
   Mirrors the protocol the C codegen used to emit inline. */
int a_test_main(uint64_t n, const char * const *names, a_test_fn const *fns) {
    int passed = 0, failed = 0;
    for (uint64_t i = 0; i < n; ++i) {
        int pfd[2];
        if (pipe(pfd) != 0) { perror("pipe"); return 1; }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]); dup2(pfd[1], 2); close(pfd[1]);
            int dn = open("/dev/null", 1);
            if (dn >= 0) { dup2(dn, 1); close(dn); }
            aval argv[1]; argv[0] = A_NONE;
            fns[i](argv);
            fflush(stdout); fflush(stderr); _exit(0);
        }
        close(pfd[1]);
        char msg[4096];
        /* panicf emits the error line and traceback in several small
           writes; a single read() can return after just "error: ". Drain
           the pipe until EOF so the FAILED summary sees the full message. */
        ssize_t rn = 0;
        while (rn < (ssize_t)sizeof(msg) - 1) {
            ssize_t r = read(pfd[0], msg + rn, sizeof(msg) - 1 - (size_t)rn);
            if (r <= 0) break;
            rn += r;
        }
        if (rn < 0) rn = 0;
        msg[rn] = 0;
        close(pfd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            printf("ok %s\n", names[i]);
            ++passed;
        } else {
            char *m = (char *)msg;
            if (strncmp(m, "error: ", 7) == 0) m += 7;
            /* Keep the FAILED summary to the panic message itself; the
               traceback section is noise in the one-line test report. */
            char *tb = strstr(m, "\nTraceback");
            if (tb) *tb = 0;
            size_t ml = strlen(m);
            while (ml > 0 && (m[ml-1] == '\n' || m[ml-1] == '\r')) m[--ml] = 0;
            printf("FAILED %s: %s\n", names[i], m);
            ++failed;
        }
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
#endif

