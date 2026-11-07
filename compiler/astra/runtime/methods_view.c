#include "methods_internal.h"

aval method_view(aval obj, const char *name, uint64_t n, aval *argv) {
        AView *vw = (AView *)obj_of(obj);
        int64_t base;
        AArr *root = view_root(obj, &base);
        if (!strcmp(name, "append")) {
            panicf("Cannot 'append' a view.");
        }
        if (!strcmp(name, "pop")) {
            panicf("Cannot 'pop' a view.");
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            for (int64_t i = 0; i < vw->len / 2; ++i) {
                int64_t j = vw->len - 1 - i;
                aval t = root->items[(size_t)(base + i)];
                root->items[(size_t)(base + i)] = root->items[(size_t)(base + j)];
                root->items[(size_t)(base + j)] = t;
            }
            return A_NONE;
        }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            aval *ptr = root->items + (size_t)base;
            sort_arr(ptr, (uint64_t)vw->len);
            return A_NONE;
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 1);
            int64_t c = 0;
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return mkint_full(i);
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return A_TRUE;
            return A_FALSE;
        }
        if (!strcmp(name, "first")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)base]);
        }
        if (!strcmp(name, "last")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)(base + vw->len - 1)]);
        }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            return list_from_vec(root->items + (size_t)base, (uint64_t)vw->len, vw->is_tuple);
        }
        if (!strcmp(name, "clear")) {
            panicf("Cannot 'clear' a view.");
        }
        return method_no_method(obj, name);
}
