#include "methods_internal.h"

aval method_seq(aval obj, const char *name, uint64_t n, aval *argv) {
        int is_list = kind_of(obj) == K_LIST;
        AArr *a = (AArr *)obj_of(obj);
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            return seq_index(obj, argv[0], st, en);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            int64_t lo = st < 0 ? st + (int64_t)a->h.len : st;
            int64_t hi = en < 0 ? en + (int64_t)a->h.len : en;
            if (lo < 0) lo = 0;
            if (hi > (int64_t)a->h.len) hi = (int64_t)a->h.len;
            int64_t c = 0;
            if (arr_packed(a)) {
                int64_t iv = 0;
                double dv = 0.0;
                int nc = needle_class(argv[0], &iv, &dv);
                if (nc == A_NEEDLE_INT) {
                    for (int64_t i = lo; i < hi; ++i)
                        if (a->iitems[i] == iv) ++c;
                } else if (nc == A_NEEDLE_DBL) {
                    for (int64_t i = lo; i < hi; ++i)
                        if ((double)a->iitems[i] == dv) ++c;
                }
                return mkint_full(c);
            }
            arr_ensure_aval(a);
            aval *items = a->items;
            for (int64_t i = lo; i < hi; ++i)
                if (aval_eq_fast(items[i], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "contains")) { want_args(name, n, 1, 1); return bool_aval(seq_contains(obj, argv[0])); }
        if (!strcmp(name, "first")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(arr_elem(a, 0)); }
        if (!strcmp(name, "last")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(arr_elem(a, (uint64_t)a->h.len - 1)); }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            if (arr_packed(a)) {
                /* packed int copy stays packed */
                AArr *out = (AArr *)xmalloc(sizeof(AArr));
                out->h.type = a->h.type;
                out->h.len = a->h.len;
                out->h.cap = a->h.len;
                out->items = NULL;
                out->iitems = (int64_t *)xmalloc((size_t)(a->h.len ? a->h.len : 1) * sizeof(int64_t));
                if (a->h.len) memcpy(out->iitems, a->iitems, (size_t)a->h.len * sizeof(int64_t));
                out->flags = ARR_F_PACKED;
                return mkobj(&out->h);
            }
            return list_from_vec(a->items, a->h.len, !is_list);
        }
        if (!is_list) return method_no_method(obj, name);
        if (!strcmp(name, "append")) {
            want_args(name, n, 1, 1);
            arr_push(a, argv[0]);
            return A_NONE;
        }
        if (!strcmp(name, "extend")) {
            want_args(name, n, 1, 1);
            /* Fast path for list/tuple/set arguments: skip the iterator
               protocol and copy elements directly. a_list_from_iter
               yields items in index order for these kinds, so the result
               is identical — except for self-extension, where the
               original materialized a copy first (a.extend(a) doubles
               the list), so the same-object case keeps the slow path. */
            AObj *eo = obj_of(argv[0]);
            if (eo && eo != (AObj *)a &&
                (eo->type == T_LIST || eo->type == T_TUPLE || eo->type == T_SET)) {
                AArr *e = (AArr *)eo;
                if (arr_packed(a) && arr_packed(e) &&
                    e->h.len <= UINT32_MAX - a->h.len) {
                    /* packed + packed stays packed: one reserve + memcpy */
                    uint64_t old = a->h.len;
                    arr_reserve_i64(a, old + e->h.len);
                    if (e->h.len)
                        memcpy(a->iitems + old, e->iitems,
                               (size_t)e->h.len * sizeof(int64_t));
                    a->h.len = (uint32_t)(old + e->h.len);
                    return A_NONE;
                }
                for (uint64_t i = 0; i < e->h.len; ++i) arr_push(a, arr_elem(e, i));
                return A_NONE;
            }
            aval tmp = collect_iterable(argv[0]);
            AArr *e = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < e->h.len; ++i) arr_push(a, e->items[i]);
            return A_NONE;
        }
        if (!strcmp(name, "insert")) {
            want_args(name, n, 2, 2);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0) i = 0;
            if (i > (int64_t)a->h.len) i = (int64_t)a->h.len;
            if (arr_packed(a) && is_int_val(argv[1])) {
                arr_reserve_i64(a, (uint64_t)a->h.len + 1);
                memmove(a->iitems + i + 1, a->iitems + i,
                        (size_t)(a->h.len - (uint64_t)i) * sizeof(int64_t));
                a->iitems[i] = int_val_full(argv[1]);
                a->h.len++;
                return A_NONE;
            }
            if (arr_packed(a)) {
                a->flags |= ARR_F_NOMIX;
                arr_ensure_aval(a);
            }
            arr_reserve(a, (uint64_t)a->h.len + 1);
            if ((a->flags & ARR_F_F64) && !a_is_double(argv[1])) a->flags &= ~ARR_F_F64;
            memmove(a->items + i + 1, a->items + i,
                    (size_t)(a->h.len - (uint64_t)i) * sizeof(aval));
            a_region_store(a, argv[1]);   /* region write barrier */
            a->items[i] = argv[1];
            a->h.len++;
            return A_NONE;
        }
        if (!strcmp(name, "remove_at")) {
            want_args(name, n, 1, 1);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0 || i >= (int64_t)a->h.len) panicf("remove_at index out of range");
            if (arr_packed(a)) {
                int64_t r = a->iitems[i];
                memmove(a->iitems + i, a->iitems + i + 1, (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(int64_t));
                a->h.len--;
                arr_shrink(a);
                return mkint_full(r);
            }
            aval r = a->items[i];
            memmove(a->items + i, a->items + i + 1, (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
            a->h.len--;
            arr_shrink(a);
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            /* Stringify straight into a temp aval vector. The old path ran
               collect_iterable(obj), which copied the whole list into a
               fresh AArr that is never reclaimed (the runtime has no
               collector) — pure retention for a join. a_str() cannot
               mutate the source list, so reading it in place is safe. */
            AVec sv = { NULL, 0, 0 };
            for (uint64_t i = 0; i < a->h.len; ++i)
                avec_push(&sv, a_str(arr_elem(a, i)));
            aval r = a_str_joinsep(argv[0], sv.len, sv.p);
            free(sv.p);
            return r;
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            if (arr_packed(a)) {
                /* Every element is an int; only a numeric needle can match.
                   needle_class classifies once: a non-numeric needle can
                   never match (mirrors values_equal's numeric rules). */
                int64_t iv = 0;
                double dv = 0.0;
                int nc = needle_class(argv[0], &iv, &dv);
                if (nc == A_NEEDLE_INT) {
                    for (uint64_t i = 0; i < a->h.len; ++i) {
                        if (a->iitems[i] == iv) {
                            memmove(a->iitems + i, a->iitems + i + 1,
                                    (size_t)(a->h.len - i - 1) * sizeof(int64_t));
                            a->h.len--;
                            arr_shrink(a);
                            return A_NONE;
                        }
                    }
                } else if (nc == A_NEEDLE_DBL) {
                    for (uint64_t i = 0; i < a->h.len; ++i) {
                        if ((double)a->iitems[i] == dv) {
                            memmove(a->iitems + i, a->iitems + i + 1,
                                    (size_t)(a->h.len - i - 1) * sizeof(int64_t));
                            a->h.len--;
                            arr_shrink(a);
                            return A_NONE;
                        }
                    }
                }
                panicf("Value is not in the list.");
                return A_NONE;
            }
            arr_ensure_aval(a);
            aval *items = a->items;
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (aval_eq_fast(items[i], argv[0])) {
                    memmove(a->items + i, a->items + i + 1,
                            (size_t)(a->h.len - i - 1) * sizeof(aval));
                    a->h.len--;
                    arr_shrink(a);
                    return A_NONE;
                }
            }
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 1);
            return list_pop(obj, n ? expect_int64(argv[0], "index") : 0, n != 0);
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); a->h.len = 0; arr_shrink(a); return A_NONE; }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            if (arr_packed(a)) {  /* list of ints: sort int64 storage in place */
                sort_ints(a->iitems, a->h.len);
                return A_NONE;
            }
            sort_arr(a->items, a->h.len);
            return A_NONE;
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            if (arr_packed(a)) {
                for (uint64_t i = 0; i < a->h.len / 2; ++i) {
                    uint64_t j = a->h.len - 1 - i;
                    int64_t t = a->iitems[i]; a->iitems[i] = a->iitems[j]; a->iitems[j] = t;
                }
                return A_NONE;
            }
            for (uint64_t i = 0; i < a->h.len / 2; ++i) {
                uint64_t j = a->h.len - 1 - i;
                aval t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
            }
            return A_NONE;
        }
        return method_no_method(obj, name);
}
