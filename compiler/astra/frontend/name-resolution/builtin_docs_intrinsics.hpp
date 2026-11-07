#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addIntrinsicsDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // LLVM-style intrinsics: bit operations
      {"ctz", {"ctz", "ctz(x) -> i64", "Count of trailing zero bits (64 for 0)."}},
      {"clz", {"clz", "clz(x) -> i64", "Count of leading zero bits (64 for 0)."}},
      {"popcount", {"popcount", "popcount(x) -> i64", "Number of set bits."}},
      {"bswap", {"bswap", "bswap(x) -> i64", "Byte-swaps the 64-bit value."}},
      {"rotl", {"rotl", "rotl(x, k) -> i64", "Rotates left by k bits (k taken mod 64)."}},
      {"rotr", {"rotr", "rotr(x, k) -> i64", "Rotates right by k bits (k taken mod 64)."}},

      // LLVM-style intrinsics: float operations
      {"fma", {"fma", "fma(a, b, c) -> f64", "Fused multiply-add: a*b+c with a single rounding."}},
      {"fabs", {"fabs", "fabs(x) -> f64", "Absolute value as f64."}},
      {"copysign", {"copysign", "copysign(x, y) -> f64", "Magnitude of x with the sign of y."}},

      // LLVM-style intrinsics: integer min/max and overflow
      {"smin", {"smin", "smin(a, b) -> i64", "Signed minimum."}},
      {"smax", {"smax", "smax(a, b) -> i64", "Signed maximum."}},
      {"umin", {"umin", "umin(a, b) -> i64", "Unsigned minimum (bit-pattern compare)."}},
      {"umax", {"umax", "umax(a, b) -> i64", "Unsigned maximum (bit-pattern compare)."}},
      {"sadd_overflow", {"sadd_overflow", "sadd_overflow(a, b) -> (i64, bool)",
                          "Signed addition; returns (wrapped result, overflowed)."}},
      {"ssub_overflow", {"ssub_overflow", "ssub_overflow(a, b) -> (i64, bool)",
                          "Signed subtraction; returns (wrapped result, overflowed)."}},
      {"smul_overflow", {"smul_overflow", "smul_overflow(a, b) -> (i64, bool)",
                          "Signed multiplication; returns (wrapped result, overflowed)."}},
      {"uadd_overflow", {"uadd_overflow", "uadd_overflow(a, b) -> (i64, bool)",
                          "Unsigned addition; returns (wrapped result, overflowed)."}},
      {"usub_overflow", {"usub_overflow", "usub_overflow(a, b) -> (i64, bool)",
                          "Unsigned subtraction; returns (wrapped result, overflowed)."}},
      {"umul_overflow", {"umul_overflow", "umul_overflow(a, b) -> (i64, bool)",
                          "Unsigned multiplication; returns (wrapped result, overflowed)."}},

      // LLVM-style intrinsics: branch hints
      {"expect", {"expect", "expect(cond, hint?) -> value", "Branch hint (llvm.expect analog); returns cond."}},
      {"likely", {"likely", "likely(cond) -> value", "Marks a condition as likely; returns it unchanged."}},
      {"unlikely", {"unlikely", "unlikely(cond) -> value", "Marks a condition as unlikely; returns it unchanged."}},
  });
}

}  // namespace a
