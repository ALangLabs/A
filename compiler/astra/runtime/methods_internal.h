#pragma once

#include "a_runtime_internal.h"

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
