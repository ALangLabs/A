#include "methods_internal.h"

/* Local membership scan: identical to set_contains (linear
   values_equal), with the scalar fast path inlined per element. */
static int set_has_fast(AArr *s, aval v) {
    for (uint64_t i = 0; i < s->h.len; ++i)
        if (aval_eq_fast(s->items[i], v)) return 1;
    return 0;
}

/* First-character dispatch buckets (see methods_seq.c). */
aval method_set(aval obj, const char *name, uint64_t n, aval *argv) {
        AArr *s = (AArr *)obj_of(obj);
        switch (name[0]) {
        case 'a':
            if (!strcmp(name, "add")) { want_args(name, n, 1, 1); if (!set_has_fast(s, argv[0])) arr_push(s, argv[0]); return A_NONE; }
            break;
        case 'd':
            if (!strcmp(name, "discard")) {
                want_args(name, n, 1, 1);
                for (uint64_t i = 0; i < s->h.len; ++i) {
                    if (aval_eq_fast(s->items[i], argv[0])) {
                        memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                        s->h.len--;
                        arr_shrink(s);
                        return A_NONE;
                    }
                }
                return A_NONE;
            }
            if (!strcmp(name, "difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 2); }
            break;
        case 'r':
            if (!strcmp(name, "remove")) {
                want_args(name, n, 1, 1);
                for (uint64_t i = 0; i < s->h.len; ++i) {
                    if (aval_eq_fast(s->items[i], argv[0])) {
                        memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                        s->h.len--;
                        arr_shrink(s);
                        return A_NONE;
                    }
                }
                panicf("value not in set");
                return A_NONE;
            }
            break;
        case 'c':
            if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); s->h.len = 0; arr_shrink(s); return A_NONE; }
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
            if (!strcmp(name, "contains")) {
                want_args(name, n, 1, 1);
                return a_bool(set_has_fast(s, argv[0]));
            }
            break;
        case 'u':
            if (!strcmp(name, "union")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 0); }
            break;
        case 'i':
            if (!strcmp(name, "intersection")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 1); }
            if (!strcmp(name, "issubset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 0); }
            if (!strcmp(name, "issuperset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 1); }
            break;
        case 's':
            if (!strcmp(name, "symmetric_difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 3); }
            break;
        case 'p':
            if (!strcmp(name, "pop")) {
                want_args(name, n, 0, 0);
                if (!s->h.len) panicf("pop from an empty set");
                aval r = s->items[0];
                memmove(s->items, s->items + 1, (size_t)(s->h.len - 1) * sizeof(aval));
                s->h.len--;
                arr_shrink(s);
                return r;
            }
            break;
        }
        return method_no_method(obj, name);
}
