#include "methods_internal.h"

aval method_dict(aval obj, const char *name, uint64_t n, aval *argv) {
        ADict *d = (ADict *)obj_of(obj);
        if (!strcmp(name, "get")) return dict_method_get(obj, n, argv);
        if (!strcmp(name, "keys")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 0); }
        if (!strcmp(name, "values")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 1); }
        if (!strcmp(name, "items")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 2); }
        if (!strcmp(name, "update")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) == K_DICT) {
                ADict *o = (ADict *)obj_of(argv[0]);
                for (uint64_t i = 0; i < o->h.len; ++i) dict_put(d, o->pairs[i].k, o->pairs[i].v);
            } else {
                aval tmp = collect_iterable(argv[0]);
                AArr *a = (AArr *)obj_of(tmp);
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    aval p = a->items[i];
                    if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                        panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                    AArr *pa = (AArr *)obj_of(p);
                    if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                    dict_put(d, pa->items[0], pa->items[1]);
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i < 0) {
                if (n == 2) return argv[1];
                panicf("key not found");
            }
            aval r = d->pairs[i].v;
            dict_remove_at(d, (uint64_t)i);
            return r;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); dict_clear(d); return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            aval nd = a_dict_new();
            ADict *m = (ADict *)obj_of(nd);
            for (uint64_t i = 0; i < d->h.len; ++i) dict_put(m, d->pairs[i].k, d->pairs[i].v);
            return nd;
        }
        if (!strcmp(name, "setdefault")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i >= 0) return d->pairs[i].v;
            aval dv = n == 2 ? argv[1] : A_NONE;
            dict_put(d, argv[0], dv);
            return dv;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(dict_find(d, argv[0]) >= 0);
        }
        return method_no_method(obj, name);
}
