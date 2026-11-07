#include "a_runtime_internal.h"
#include "methods_internal.h"

/* ------------------------------------------------------------------ */
/* methods                                                             */
/* ------------------------------------------------------------------ */

void want_args(const char *name, uint64_t n, uint64_t min, uint64_t max) {
    if (n < min || n > max) {
        if (min == max)
            panicf("'%s' expects %llu argument(s), got %llu", name,
                   (unsigned long long)min, (unsigned long long)n);
        panicf("'%s' expects between %llu and %llu arguments, got %llu", name,
               (unsigned long long)min, (unsigned long long)max,
               (unsigned long long)n);
    }
}

void sort_arr(aval *a, uint64_t n) {
    for (uint64_t i = 1; i < n; ++i) {
        aval key = a[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && truthy(a_lt(key, a[j]))) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = key;
    }
}

aval collect_iterable(aval v) {
    return a_list_from_iter(v);
}

aval a_getattr(aval obj, const char *name) {
    if (kind_of(obj) == K_DICT) {
        ADict *d = (ADict *)obj_of(obj);
        aval key = a_str_new(name, strlen(name));
        int64_t i = dict_find(d, key);
        if (i >= 0) return d->pairs[i].v;
    }
    AObj *o = obj_of(obj);
    if (o && o->type == T_PYOBJ) return a_py_getattr(obj, name);
    panicf("'%s' object has no attribute '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

aval a_method(aval obj, const char *name, uint64_t n, uint64_t nkw, aval *argv) {
    /* Named args only have meaning for Python-interop receivers; every
       other kind rejects them (sema already rejects them statically for
       known receiver types, so this only fires for dynamic `var` values). */
    AObj *po0 = obj_of(obj);
    if (nkw && !(po0 && po0->type == T_PYOBJ))
        panicf("'%s' object has no support for named arguments", type_name_cstr(obj));
    switch (kind_of(obj)) {
    case K_STR: return method_str(obj, name, n, argv);
    case K_LIST:
    case K_TUPLE: return method_seq(obj, name, n, argv);
    case K_VIEW: return method_view(obj, name, n, argv);
    case K_DICT: return method_dict(obj, name, n, argv);
    case K_SET: return method_set(obj, name, n, argv);
    default: break;
    }
    AObj *po = obj_of(obj);
    if (po && po->type == T_PYOBJ)
        return a_py_call_method(obj, name, n, nkw, argv);
    return method_no_method(obj, name);
}
