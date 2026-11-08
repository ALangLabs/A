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

/* Insertion sort: only used for tiny runs inside the merge sort below. */
static void isort_small(aval *a, uint64_t n) {
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

/* Stable merge sort. The previous implementation was an insertion sort, which
   is O(n^2): sorting 200k ints took 230 seconds. a_lt() is the comparison
   oracle so ints, floats, strings and tuples all keep working. */
static void msort(aval *a, aval *tmp, uint64_t n) {
    if (n <= 16) { isort_small(a, n); return; }
    uint64_t mid = n / 2;
    msort(a, tmp, mid);
    msort(a + mid, tmp, n - mid);
    uint64_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        /* take from the right only when strictly smaller: keeps it stable */
        if (truthy(a_lt(a[j], a[i]))) tmp[k++] = a[j++];
        else tmp[k++] = a[i++];
    }
    while (i < mid) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, (size_t)n * sizeof(aval));
}

void sort_arr(aval *a, uint64_t n) {
    if (n < 2) return;
    aval *tmp = (aval *)xmalloc((size_t)n * sizeof(aval));
    msort(a, tmp, n);
    free(tmp);
}

/* Packed int64 arrays are the hot case (list of ints): sort them in place
   with a three-way quicksort so duplicates cost nothing. */
static void isort_int(int64_t *a, uint64_t n) {
    for (uint64_t i = 1; i < n; ++i) {
        int64_t key = a[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; --j; }
        a[j + 1] = key;
    }
}

static void qsort_int(int64_t *a, uint64_t n) {
    while (n > 16) {
        int64_t lo = a[0], mid = a[n / 2], hi = a[n - 1];
        int64_t pivot = lo < mid ? (mid < hi ? mid : (lo < hi ? hi : lo))
                                 : (lo < hi ? lo : (mid < hi ? hi : mid));
        uint64_t lt = 0, i = 0, gt = n;
        while (i < gt) {
            if (a[i] < pivot) { int64_t t = a[lt]; a[lt++] = a[i]; a[i++] = t; }
            else if (a[i] > pivot) { int64_t t = a[gt - 1]; a[gt - 1] = a[i]; a[i] = t; gt--; }
            else i++;
        }
        /* recurse into the smaller side, iterate on the larger one */
        if (lt < n - gt) { qsort_int(a, lt); a += gt; n -= gt; }
        else { qsort_int(a + gt, n - gt); n = lt; }
    }
    isort_int(a, n);
}

void sort_ints(int64_t *a, uint64_t n) {
    if (n < 2) return;
    qsort_int(a, n);
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
