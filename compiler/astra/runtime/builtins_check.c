#include "a_runtime_internal.h"

static void panic_with_msg_at(uint64_t n, aval *argv, int idx, const char *def) {
    (void)n;
    if (idx < 0) { a_panic_msg(def); return; }
    aval s = a_str(argv[idx]);
    uint64_t len;
    const char *p = str_data(s, &len);
    (void)len;
    a_panic_msg(p);
}

aval bi_panic(uint64_t n, aval *argv) {
    want_args("panic", n, 0, 1);
    if (n == 0) a_panic_msg("panic");
    else panic_with_msg_at(n, argv, 0, "panic");
    return A_NONE;
}

aval bi_assert(uint64_t n, aval *argv) {
    want_args("assert", n, 1, 2);
    if (!truthy(argv[0])) panic_with_msg_at(n, argv, n == 2 ? 1 : -1, "Assertion failed");
    return A_NONE;
}

aval bi_assert_eq(uint64_t n, aval *argv) {
    want_args("assert_eq", n, 2, 3);
    if (!values_equal(argv[0], argv[1])) {
        Buf b;
        buf_init(&b);
        if (n == 3) {
            /* Match integration test harness format. */
            buf_adds(&b, "Assertion failed: expected ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, ", found ");
            format_into(&b, argv[1], 1);
            buf_adds(&b, " (");
            aval msg = a_str(argv[2]);
            uint64_t mlen;
            const char *mp = str_data(msg, &mlen);
            buf_addn(&b, mp, mlen);
            buf_addc(&b, ')');
        } else {
            buf_adds(&b, "assertion failed: ");
            format_into(&b, argv[0], 1);
            buf_adds(&b, " != ");
            format_into(&b, argv[1], 1);
        }
        aval s = buf_take(&b);
        uint64_t len;
        a_panic_msg(str_data(s, &len));
    }
    return A_NONE;
}

aval bi_input(uint64_t n, aval *argv) {
    want_args("input", n, 0, 1);
    if (n == 1) {
        aval s = a_str(argv[0]);
        uint64_t len;
        const char *p = str_data(s, &len);
        fwrite(p, 1, (size_t)len, stdout);
    }
    fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) panicf("Input ended before a line could be read.");
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len && buf[len - 1] == '\r') buf[--len] = '\0';
    aval r = a_str_new(buf, (uint64_t)len);
    return r;
}


aval bi_isinstance(uint64_t n, aval *argv) {
    want_args("isinstance", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("isinstance() arg 2 must be str");
    uint64_t len;
    const char *p = str_data(argv[1], &len);
    char *name = xstrndup(p, (size_t)len);
    int r = a_obj_is_instance(argv[0], name);
    free(name);
    return bool_aval(r);
}


aval bi_struct_name(uint64_t n, aval *argv) {
    want_args("struct_name", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (o && o->type == T_STRUCT) return ((AStruct *)o)->name;
    return a_str_new("", 0);
}

aval bi_struct_methods(uint64_t n, aval *argv) {
    want_args("struct_methods", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (o && o->type == T_STRUCT) {
        aval m = ((AStruct *)o)->methods;
        if (kind_of(m) == K_LIST) return m;
    }
    return a_list_new(0, NULL);
}

aval bi_fn_signature(uint64_t n, aval *argv) {
    want_args("fn_signature", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (o && o->type == T_FNON) {
        AFnon *f = (AFnon *)o;
        if (f->sig) return a_str_new(f->sig, strlen(f->sig));
        if (f->name) return a_str_new(f->name, strlen(f->name));
    }
    return a_str_new("", 0);
}

aval bi_fn_doc(uint64_t n, aval *argv) {
    want_args("fn_doc", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (o && o->type == T_FNON) {
        AFnon *f = (AFnon *)o;
        if (f->doc) return a_str_new(f->doc, strlen(f->doc));
    }
    return a_str_new("", 0);
}
