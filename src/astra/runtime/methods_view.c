#include "methods_internal.h"

/* First-character dispatch buckets (see methods_seq.c). */
aval method_view(aval obj, const char *name, uint64_t n, aval *argv) {
        AView *vw = (AView *)obj_of(obj);
        int64_t base;
        AArr *root = view_root(obj, &base);
        switch (name[0]) {
        case 'a':
            if (!strcmp(name, "append")) {
                panicf("Cannot 'append' a view.");
            }
            break;
        case 'p':
            if (!strcmp(name, "pop")) {
                panicf("Cannot 'pop' a view.");
            }
            break;
        case 'r':
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
            break;
        case 's':
            if (!strcmp(name, "sort")) {
                want_args(name, n, 0, 0);
                /* root is NULL only for an empty (parentless) view; sorting
                   zero elements is a no-op either way. */
                if (vw->len) {
                    aval *ptr = root->items + (size_t)base;
                    sort_arr(ptr, (uint64_t)vw->len);
                }
                return A_NONE;
            }
            break;
        case 'c':
            if (!strcmp(name, "count")) {
                want_args(name, n, 1, 1);
                /* view_root already normalized the parent to aval storage;
                   hoist the element pointer and use the scalar fast path. */
                aval *items = root ? root->items + (size_t)base : NULL;
                int64_t c = 0;
                for (int64_t i = 0; i < vw->len; ++i)
                    if (aval_eq_fast(items[i], argv[0])) ++c;
                return mkint_full(c);
            }
            if (!strcmp(name, "contains")) {
                want_args(name, n, 1, 1);
                aval *items = root ? root->items + (size_t)base : NULL;
                for (int64_t i = 0; i < vw->len; ++i)
                    if (aval_eq_fast(items[i], argv[0]))
                        return A_TRUE;
                return A_FALSE;
            }
            if (!strcmp(name, "copy")) {
                want_args(name, n, 0, 0);
                /* root is NULL only for an empty (parentless) view; copying
                   zero elements yields an empty list/tuple either way. */
                return list_from_vec(root ? root->items + (size_t)base : NULL,
                                     (uint64_t)vw->len, vw->is_tuple);
            }
            if (!strcmp(name, "clear")) {
                panicf("Cannot 'clear' a view.");
            }
            break;
        case 'i':
            if (!strcmp(name, "index")) {
                want_args(name, n, 1, 1);
                aval *items = root ? root->items + (size_t)base : NULL;
                for (int64_t i = 0; i < vw->len; ++i)
                    if (aval_eq_fast(items[i], argv[0]))
                        return mkint_full(i);
                panicf("Value is not in the list.");
                return A_NONE;
            }
            break;
        case 'f':
            if (!strcmp(name, "first")) {
                want_args(name, n, 0, 0);
                if (!vw->len) return A_NONE;
                return a_some_new(root->items[(size_t)base]);
            }
            break;
        case 'l':
            if (!strcmp(name, "last")) {
                want_args(name, n, 0, 0);
                if (!vw->len) return A_NONE;
                return a_some_new(root->items[(size_t)(base + vw->len - 1)]);
            }
            break;
        }
        return method_no_method(obj, name);
}
