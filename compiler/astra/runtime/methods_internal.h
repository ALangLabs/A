#pragma once

#include "a_runtime_internal.h"

/* Element-compare fast path for read-only scans (index/contains/count/
   remove/set ops): inline ints, plain doubles, and mixed int/double
   pairs decide without a values_equal call — identical to
   values_equal's is_num branch — and identical tagged non-pointer avals
   (the None/bool constants) are trivially equal, matching the body's
   `a == b` check. Everything else falls back to values_equal, so cycle
   tracking and every container/string rule are preserved exactly. */
static inline int aval_eq_fast(aval a, aval b) {
    if (a_is_int(a) && a_is_int(b)) return a_int_val(a) == a_int_val(b);
    if (a_is_double(a) && a_is_double(b))
        return a_double_val(a) == a_double_val(b);
    if (a_is_int(a) && a_is_double(b))
        return (double)a_int_val(a) == a_double_val(b);
    if (a_is_double(a) && a_is_int(b))
        return a_double_val(a) == (double)a_int_val(b);
    if (a == b && A_IS_TAGGED(a) && (A_PAYLOAD(a) & 0xFULL) != 0)
        return 1;
    return values_equal(a, b);
}

/* Packed-int arrays hold only int64 elements; values_equal against a
   non-numeric needle is always false, a numeric needle compares as
   e == i (int) or (double)e == d (float). Classify the needle once so
   scans run as a bare int64 loop instead of a per-element
   arr_i64_eq_aval/values_equal call. */
enum { A_NEEDLE_NONE = 0, A_NEEDLE_INT = 1, A_NEEDLE_DBL = 2 };
static inline int needle_class(aval v, int64_t *iv, double *dv) {
    if (is_int_val(v)) { *iv = int_val_full(v); return A_NEEDLE_INT; }
    if (a_is_double(v)) { *dv = a_double_val(v); return A_NEEDLE_DBL; }
    return A_NEEDLE_NONE;
}

aval method_no_method(aval obj, const char *name);
aval str_case_map(aval v, int mode);
int is_space_byte(unsigned char c);
aval str_strip_impl(aval v, int left, int right);
AVec str_split_impl(aval v, aval sep, int64_t maxsplit, int rsplit);
aval str_replace(aval v, aval oldv, aval newv, int64_t count);
int str_pred(aval v, int pred);
aval str_affix(aval v, int64_t width, aval fillv, int mode);
aval seq_index(aval v, aval item, int64_t start, int64_t stop);
int seq_contains(aval v, aval item);
aval list_pop(aval v, int64_t idx, int has_idx);
aval dict_method_get(aval v, uint64_t n, aval *argv);
aval make_keys_values_items(aval v, int mode);
aval method_set_op(aval v, aval other, int mode);
aval method_subset(aval v, aval other, int sup);
aval method_str(aval obj, const char *name, uint64_t n, aval *argv);
aval method_seq(aval obj, const char *name, uint64_t n, aval *argv);
aval method_view(aval obj, const char *name, uint64_t n, aval *argv);
aval method_dict(aval obj, const char *name, uint64_t n, aval *argv);
aval method_set(aval obj, const char *name, uint64_t n, aval *argv);
