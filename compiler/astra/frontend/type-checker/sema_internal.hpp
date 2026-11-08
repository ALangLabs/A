#pragma once

#include "type-checker/sema.hpp"

#include <algorithm>
#include <atomic>
#include <string_view>
#include <thread>
#include <utility>

#include "name-resolution/native_modules.hpp"
#include "tooling/source_text.hpp"
#include "type-checker/debug/magic_identifiers.hpp"
#include "type-checker/method_signatures.hpp"

namespace a {

// Shared by bindVar/unify/constrainVar and the final type-variable check.
inline std::recursive_mutex gVarMutex;

// True when control can never continue past this statement.
inline bool terminatesFlow(const Stmt& stmt) {
  if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
  if (dynamic_cast<const BreakStmt*>(&stmt)) return true;
  if (dynamic_cast<const ContinueStmt*>(&stmt)) return true;
  if (dynamic_cast<const LoopStmt*>(&stmt)) return true;  // loop ... only exits via break
  if (const auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
    if (node->elseBody.empty()) return false;
    for (const auto& branch : node->branches) {
      if (branch.body.empty() || !terminatesFlow(*branch.body.back())) return false;
    }
    return terminatesFlow(*node->elseBody.back());
  }
  return false;
}

inline bool isConversionBuiltin(const std::string& name) {
  static const char* names[] = {"i8", "i16",  "i32", "i64", "u8",
                                "u16", "u32", "u64", "f32", "f64"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

inline bool isMathBuiltin(const std::string& name) {
  static const char* names[] = {"sqrt", "sin",  "cos",   "tan",   "exp",  "log",
                                "floor", "ceil", "round", "trunc", "log2", "log10"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

inline bool isKnownBuiltin(const std::string& name) {
  static const char* names[] = {
      "print",    "eprint",   "input",     "len",      "str",      "abs",
      "int",      "float",    "min",       "max",      "pow",      "flush",
      "read_file", "write_file",
      "append_file", "file_exists", "file_size", "dot", "matvec", "matvec_t",
      "linalg_dot", "linalg_matvec", "linalg_matvec_t", "list_dir", "remove_file", "create_dir",
      "remove_dir", "rename", "current_dir", "now", "sleep", "panic", "assert",
      "open", "close",
      "assert_eq", "map",  "filter",   "reduce",   "any",
      "all",      "sum",      "join",      "random",   "random_int", "atan2",
      "set",      "unsafe_get", "unsafe_set",
      "arena_new", "arena_pool", "arena_region", "arena_alloc", "arena_release",
      "arena_reset", "arena_used", "arena_total", "move", "copy",
      "borrow", "borrow_mut", "unsafe_address_of", "unsafe_load", "unsafe_store",
      "unsafe_offset", "py_buffer",
      "Some", "Ok", "Err", "ic", "repr",
      "__repr__", "__debug_repr__", "__ic_format__",
      // LLVM-style intrinsics
      "ctz", "clz", "popcount", "bswap", "rotl", "rotr",
      "fma", "fabs", "copysign",
      "smin", "smax", "umin", "umax",
      "sadd_overflow", "ssub_overflow", "smul_overflow",
      "uadd_overflow", "usub_overflow", "umul_overflow",
      "expect", "likely", "unlikely"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return isConversionBuiltin(name) || isMathBuiltin(name) || isStdlibBuiltin(name);
}

inline TypePtr builtinConstantType(const std::string& name) {
  if (name == "PI" || name == "E") return typeFloat(64);
  if (name == "stdout" || name == "stderr") return typeIO();
  struct IntConstant {
    const char* name;
    int bits;
    bool isSigned;
  };
  static const IntConstant ints[] = {
      {"I8_MIN", 8, true},   {"I8_MAX", 8, true},   {"I16_MIN", 16, true},
      {"I16_MAX", 16, true}, {"I32_MIN", 32, true}, {"I32_MAX", 32, true},
      {"I64_MIN", 64, true}, {"I64_MAX", 64, true}, {"U8_MAX", 8, false},
      {"U16_MAX", 16, false}, {"U32_MAX", 32, false}};
  for (const auto& entry : ints) {
    if (name == entry.name) return typeInt(entry.bits, entry.isSigned);
  }
  struct FloatConstant {
    const char* name;
    int bits;
  };
  static const FloatConstant floats[] = {
      {"F32_MIN", 32}, {"F32_MAX", 32}, {"F64_MIN", 64}, {"F64_MAX", 64}};
  for (const auto& entry : floats) {
    if (name == entry.name) return typeFloat(entry.bits);
  }
  return nullptr;
}

inline bool isBuiltinConstant(const std::string& name) {
  return builtinConstantType(name) != nullptr;
}

// Collapses every whitespace run in an `ic` argument's source slice to a
// single space and trims the ends, so multi-line expressions stay on one
// debug line.
inline std::string collapseIcWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace a
