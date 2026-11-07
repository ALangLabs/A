#include "methods_internal.h"

aval method_set(aval obj, const char *name, uint64_t n, aval *argv) {
        AArr *s = (AArr *)obj_of(obj);
        if (!strcmp(name, "add")) { want_args(name, n, 1, 1); if (!set_contains(s, argv[0])) arr_push(s, argv[0]); return A_NONE; }
        if (!strcmp(name, "discard")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            panicf("value not in set");
            return A_NONE;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); s->h.len = 0; return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            AArr *out = (AArr *)xmalloc(sizeof(AArr));
            out->h.type = T_SET;
            out->h.len = s->h.len;
            out->h.cap = s->h.len;
            out->items = (aval *)xmalloc((size_t)(s->h.len ? s->h.len : 1) * sizeof(aval));
            out->iitems = NULL;
            out->flags = 0;
            if (s->h.len) memcpy(out->items, s->items, (size_t)s->h.len * sizeof(aval));
            return mkobj(&out->h);
        }
        if (!strcmp(name, "union")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 0); }
        if (!strcmp(name, "intersection")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 1); }
        if (!strcmp(name, "difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 2); }
        if (!strcmp(name, "symmetric_difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 3); }
        if (!strcmp(name, "issubset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 0); }
        if (!strcmp(name, "issuperset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 1); }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) panicf("pop from an empty set");
            aval r = s->items[0];
            memmove(s->items, s->items + 1, (size_t)(s->h.len - 1) * sizeof(aval));
            s->h.len--;
            return r;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(set_contains(s, argv[0]));
        }
        return method_no_method(obj, name);
}
