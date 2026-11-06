#pragma once

#include <cstdint>
#include <string_view>

namespace a {

// Statically resolved builtin identity. `Sema` marks each call site whose
// callee is not shadowed by a variable, so the interpreter can dispatch a
// builtin without re-scanning name strings on every call (the same idea as
// specialized call opcodes in dynamic-language interpreters).
enum class BuiltinId : std::uint8_t {
  None,
  Print,
  Eprint,
  Len,
  Str,
  Sqrt,
  F64,
  I64,
  Abs,
  Round,
  Floor,
  Ceil,
  Pow,
  Join,
  Map,
  Filter,
  Sum,
  Reduce,
  Any,
  All,
  Some,
  Ok,
  Err,
  Panic,
  Assert,
  AssertEq,
  Ic,
};

inline BuiltinId builtinIdForName(std::string_view name) {
  if (name == "print") return BuiltinId::Print;
  if (name == "eprint") return BuiltinId::Eprint;
  if (name == "len") return BuiltinId::Len;
  if (name == "str") return BuiltinId::Str;
  if (name == "sqrt") return BuiltinId::Sqrt;
  if (name == "f64") return BuiltinId::F64;
  if (name == "i64") return BuiltinId::I64;
  if (name == "abs") return BuiltinId::Abs;
  if (name == "round") return BuiltinId::Round;
  if (name == "floor") return BuiltinId::Floor;
  if (name == "ceil") return BuiltinId::Ceil;
  if (name == "pow") return BuiltinId::Pow;
  if (name == "join") return BuiltinId::Join;
  if (name == "map") return BuiltinId::Map;
  if (name == "filter") return BuiltinId::Filter;
  if (name == "sum") return BuiltinId::Sum;
  if (name == "reduce") return BuiltinId::Reduce;
  if (name == "any") return BuiltinId::Any;
  if (name == "all") return BuiltinId::All;
  if (name == "Some") return BuiltinId::Some;
  if (name == "Ok") return BuiltinId::Ok;
  if (name == "Err") return BuiltinId::Err;
  if (name == "panic") return BuiltinId::Panic;
  if (name == "assert") return BuiltinId::Assert;
  if (name == "assert_eq") return BuiltinId::AssertEq;
  if (name == "ic") return BuiltinId::Ic;
  return BuiltinId::None;
}

}  // namespace a
