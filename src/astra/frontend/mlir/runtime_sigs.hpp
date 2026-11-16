#pragma once
// Shared table of `a_*` runtime function signatures in compact
// "(args) -> ret" form. Single source of truth for the MLIR emitters
// (mlir_emit.cpp declares them as llvm.func; ambc_modules.cpp synthesizes
// the same decls when merging per-module .ambc packages) — keep in sync
// with the runtime exports in src/astra/runtime/.
#include <string>
#include <utility>

namespace a {
namespace runtime_sigs {

inline const std::pair<const char*, const char*> kRuntimeSigs[] = {
    {"a_add_fast", "(i64, i64) -> i64"}, {"a_sub_fast", "(i64, i64) -> i64"},
    {"a_mul_fast", "(i64, i64) -> i64"}, {"a_div_fast", "(i64, i64) -> i64"},
    {"a_floordiv_fast", "(i64, i64) -> i64"},
    {"a_mod_fast", "(i64, i64) -> i64"}, {"a_pow_fast", "(i64, i64) -> i64"},
    {"a_eq_fast", "(i64, i64) -> i64"}, {"a_ne_fast", "(i64, i64) -> i64"},
    {"a_lt_fast", "(i64, i64) -> i64"}, {"a_le_fast", "(i64, i64) -> i64"},
    {"a_gt_fast", "(i64, i64) -> i64"}, {"a_ge_fast", "(i64, i64) -> i64"},
    {"a_band_fast", "(i64, i64) -> i64"}, {"a_bor_fast", "(i64, i64) -> i64"},
    {"a_bxor_fast", "(i64, i64) -> i64"}, {"a_shl_fast", "(i64, i64) -> i64"},
    {"a_shr_fast", "(i64, i64) -> i64"}, {"a_neg_fast", "(i64) -> i64"},
    {"a_bnot_fast", "(i64) -> i64"}, {"a_not_fast", "(i64) -> i64"},
    {"a_truthy_raw_fast", "(i64) -> i64"},
    {"a_unwrap", "(i64) -> i64"}, {"a_some_new", "(i64) -> i64"},
    {"a_try_result", "(i64) -> i64"}, {"a_is_option", "(i64) -> i32"},
    {"a_is_propagate", "(i64) -> i32"}, {"a_str", "(i64) -> i64"},
    {"a_repr", "(i64) -> i64"}, {"a_len", "(i64) -> i64"},
    {"a_int", "(i64) -> i64"}, {"a_float", "(i64) -> i64"},
    {"a_bool", "(i64) -> i64"}, {"a_type_name", "(i64) -> i64"},
    {"a_int_full", "(i64) -> i64"}, {"a_i64n", "(i64) -> i64"},
    {"a_str_new", "(!llvm.ptr, i64) -> i64"},
    {"a_char_new", "(!llvm.ptr, i64) -> i64"},
    {"a_str_concat", "(i64, i64) -> i64"},
    {"a_str_join", "(i64, !llvm.ptr) -> i64"},
    {"a_str_joinsep", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_list_new", "(i64, !llvm.ptr) -> i64"},
    {"a_list_append", "(i64, i64) -> i64"},
    {"a_list_append_int", "(i64, i64) -> i64"},
    {"a_list_from_iter", "(i64) -> i64"},
    {"a_tuple_new", "(i64, !llvm.ptr) -> i64"},
    {"a_dict_new", "() -> i64"}, {"a_dict_set", "(i64, i64, i64) -> i64"},
    {"a_dict_get", "(i64, i64, i64) -> i64"},
    {"a_dict_has", "(i64, i64) -> i64"},
    {"a_set_new", "() -> i64"}, {"a_set_add", "(i64, i64) -> i64"},
    {"a_range_new", "(i64, i64, i64) -> i64"},
    {"a_range_bound", "(i64, i64) -> i64"},
    {"a_getitem", "(i64, i64) -> i64"},
    {"a_setitem", "(i64, i64, i64) -> i64"},
    {"a_slice", "(i64, i64, i64, i64) -> i64"},
    {"a_iter_new", "(i64) -> i64"},
    {"a_iter_next", "(i64, !llvm.ptr) -> i64"},
    {"a_contains", "(i64, i64) -> i64"},
    {"a_fnon_new", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_closure_env_new", "(i64) -> i64"},
    {"a_env_new", "(i64) -> i64"}, {"a_env_get", "(i64, i64) -> i64"},
    {"a_env_set", "(i64, i64, i64) -> i64"},
    {"a_struct_new", "(i64, i64, i64) -> i64"},
    {"a_builtin", "(i64) -> i64"},
    {"a_bi_call", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_call", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_method", "(i64, !llvm.ptr, i64, i64, !llvm.ptr) -> i64"},
    {"a_getattr", "(i64, !llvm.ptr) -> i64"},
    {"a_setattr", "(i64, !llvm.ptr, i64) -> i64"},
    {"a_promote", "(i64) -> i64"}, {"a_panic_msg", "(i64) -> i64"},
    {"a_join_len", "(i64) -> i64"}, {"a_match_fail", "(i64) -> i64"},
    {"a_frame_push", "(!llvm.ptr, !llvm.ptr, i32) -> ()"}, {"a_frame_pop", "() -> ()"},
    {"a_pin_slot", "(!llvm.ptr) -> ()"}, {"a_runtime_init", "() -> ()"},
    {"a_fib_main", "(i64, i64, i64, !llvm.ptr) -> i64"},
    {"a_fib_spawn_handle", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_fib_join_handle", "(i64) -> i64"},
    {"a_test_main", "(i64, !llvm.ptr, !llvm.ptr) -> i32"},
    {"a_printv", "(i64, !llvm.ptr) -> ()"},
    {"a_eprintv", "(i64, !llvm.ptr) -> ()"},
    {"a_println", "(i64) -> ()"}, {"a_sleep", "(i64) -> ()"},
    {"a_now", "() -> i64"}, {"a_gc_collect", "() -> ()"},
    {"a_gc_mark_aval", "(i64) -> ()"}, {"a_gc_add_root_cb", "(!llvm.ptr) -> ()"},
};

inline const char* sigOf(const std::string& name) {
  for (const auto& kv : kRuntimeSigs)
    if (name == kv.first) return kv.second;
  return nullptr;
}

}  // namespace runtime_sigs
}  // namespace a
