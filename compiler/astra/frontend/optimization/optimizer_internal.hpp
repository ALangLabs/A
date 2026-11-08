#pragma once

#include "optimization/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace a {
namespace ir {
inline bool isConstOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
      return true;
    default:
      return false;
  }
}

inline bool isPureOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
    case Op::Param:
    case Op::Copy:
    case Op::Binary:
    case Op::Unary:
    case Op::Select:
    case Op::TupleGet:
    case Op::Field:
    case Op::FuncRef:
    case Op::Cast:
    case Op::GetElementPtr:
    case Op::ExtractValue:
    case Op::InsertValue:
    case Op::Freeze:
    case Op::Fma:
      return true;
    default:
      return false;
  }
}

inline bool isEffectfulOp(Op op) {
  switch (op) {
    case Op::Call:
    case Op::CallValue:
    case Op::StoreIndex:
    case Op::StoreField:
    case Op::Opaque:
    case Op::IterInit:
    case Op::IterHasNext:
    case Op::IterNext:
      return true;
    default:
      return false;
  }
}

struct Const {
  Op op = Op::Opaque;
  std::int64_t i = 0;
  double f = 0.0;
  bool b = false;
  char32_t c = 0;
  std::string s;
};

inline bool sameConst(const Const& lhs, const Const& rhs) {
  if (lhs.op != rhs.op) return false;
  switch (lhs.op) {
    case Op::ConstInt: return lhs.i == rhs.i;
    case Op::ConstFloat: return lhs.f == rhs.f;
    case Op::ConstBool: return lhs.b == rhs.b;
    case Op::ConstChar: return lhs.c == rhs.c;
    case Op::ConstString: return lhs.s == rhs.s;
    default: return false;
  }
}

inline Const constOf(const Inst& inst) {
  Const value;
  value.op = inst.op;
  switch (inst.op) {
    case Op::ConstInt: value.i = inst.intValue; break;
    case Op::ConstFloat: value.f = inst.floatValue; break;
    case Op::ConstBool: value.b = inst.boolValue; break;
    case Op::ConstChar: value.c = inst.charValue; break;
    case Op::ConstString: value.s = inst.stringValue; break;
    default: break;
  }
  return value;
}

// Value fingerprint for a constant instruction: equal literals produce equal
// fingerprints, different literals never collide. Used by CSE/DSE to
// canonicalise operands.
inline std::string constFingerprint(const Inst& inst) {
  switch (inst.op) {
    case Op::ConstInt: return "i:" + std::to_string(inst.intValue);
    case Op::ConstFloat: return "f:" + std::to_string(inst.floatValue);
    case Op::ConstBool: return std::string("b:") + (inst.boolValue ? "1" : "0");
    case Op::ConstChar: return "c:" + std::to_string(static_cast<long>(inst.charValue));
    case Op::ConstString: return "s:" + inst.stringValue;
    default: return "?";
  }
}

inline void applyConst(Inst& inst, const Const& value) {
  inst.op = value.op;
  inst.operands.clear();
  inst.incomingBlocks.clear();
  switch (value.op) {
    case Op::ConstInt: inst.intValue = value.i; break;
    case Op::ConstFloat: inst.floatValue = value.f; break;
    case Op::ConstBool: inst.boolValue = value.b; break;
    case Op::ConstChar: inst.charValue = value.c; break;
    case Op::ConstString: inst.stringValue = value.s; break;
    default: break;
  }
}

inline bool isInt(const Const& value) { return value.op == Op::ConstInt; }
inline bool isFloat(const Const& value) { return value.op == Op::ConstFloat; }
inline bool isBool(const Const& value) { return value.op == Op::ConstBool; }
inline bool isStr(const Const& value) { return value.op == Op::ConstString; }

inline Const makeInt(std::int64_t value) {
  Const out;
  out.op = Op::ConstInt;
  out.i = value;
  return out;
}

inline Const makeFloat(double value) {
  Const out;
  out.op = Op::ConstFloat;
  out.f = value;
  return out;
}

inline Const makeBool(bool value) {
  Const out;
  out.op = Op::ConstBool;
  out.b = value;
  return out;
}

inline Const makeStr(std::string value) {
  Const out;
  out.op = Op::ConstString;
  out.s = std::move(value);
  return out;
}

inline std::int64_t wrapAdd(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

inline std::int64_t wrapSub(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

inline std::int64_t wrapMul(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

inline bool compareInts(const std::string& op, std::int64_t a, std::int64_t b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

inline bool compareFloats(const std::string& op, double a, double b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

// Returns false when the operation cannot be folded (for example integer
// division by zero or a non-constant operand).
inline bool foldBinary(const std::string& op, const Const& lhs, const Const& rhs, Const& out) {
  if (isInt(lhs) && isInt(rhs)) {
    const std::int64_t a = lhs.i;
    const std::int64_t b = rhs.i;
    if (op == "+") { out = makeInt(wrapAdd(a, b)); return true; }
    if (op == "-") { out = makeInt(wrapSub(a, b)); return true; }
    if (op == "*") { out = makeInt(wrapMul(a, b)); return true; }
    if (op == "/" || op == "//") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      std::int64_t value = a / b;
      if (op == "//" && ((a % b) != 0) && ((a < 0) != (b < 0))) --value;
      out = makeInt(value);
      return true;
    }
    if (op == "%") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      out = makeInt(a % b);
      return true;
    }
    if (op == "&") { out = makeInt(a & b); return true; }
    if (op == "|") { out = makeInt(a | b); return true; }
    if (op == "^") { out = makeInt(a ^ b); return true; }
    if (op == "<<" || op == ">>") {
      if (b < 0 || b >= 64) return false;
      out = makeInt(op == "<<" ? static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << b)
                               : (a >> b));
      return true;
    }
    bool cmp = false;
    if (compareInts(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if ((isInt(lhs) || isFloat(lhs)) && (isInt(rhs) || isFloat(rhs))) {
    const double a = isInt(lhs) ? static_cast<double>(lhs.i) : lhs.f;
    const double b = isInt(rhs) ? static_cast<double>(rhs.i) : rhs.f;
    if (op == "+") { out = makeFloat(a + b); return true; }
    if (op == "-") { out = makeFloat(a - b); return true; }
    if (op == "*") { out = makeFloat(a * b); return true; }
    if (op == "/" || op == "//") {
      if (b == 0.0) return false;
      double value = a / b;
      if (op == "//") value = std::floor(value);
      out = makeFloat(value);
      return true;
    }
    if (op == "**") { out = makeFloat(std::pow(a, b)); return true; }
    bool cmp = false;
    if (compareFloats(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if (isBool(lhs) && isBool(rhs)) {
    if (op == "==") { out = makeBool(lhs.b == rhs.b); return true; }
    if (op == "!=") { out = makeBool(lhs.b != rhs.b); return true; }
    if (op == "and") { out = makeBool(lhs.b && rhs.b); return true; }
    if (op == "or") { out = makeBool(lhs.b || rhs.b); return true; }
    return false;
  }
  if (isStr(lhs) && isStr(rhs) && op == "+") {
    out = makeStr(lhs.s + rhs.s);
    return true;
  }
  return false;
}

inline std::uint64_t lowMask(int bits) {
  return bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
}

// Folds an LLVM-style cast of a constant. `src`/`dst` are the operand and
// result types; widths/sign determine trunc/zext/sext semantics.
inline bool foldCast(const std::string& kind, const TypePtr& srcIn, const TypePtr& dstIn,
              const Const& value, Const& out) {
  TypePtr src = resolveType(srcIn);
  TypePtr dst = resolveType(dstIn);
  if (!src || !dst) return false;
  if (value.op == Op::ConstInt && src->kind == TypeKind::Int) {
    const std::uint64_t raw = static_cast<std::uint64_t>(value.i);
    if (dst->kind == TypeKind::Int &&
        (kind == "trunc" || kind == "zext" || kind == "sext" || kind == "bitcast")) {
      std::uint64_t val = raw;
      if (kind == "trunc") {
        val = raw & lowMask(dst->bits);
      } else if (kind == "zext") {
        val = raw & lowMask(src->bits);
      } else if (kind == "sext" && src->bits < 64) {
        const std::uint64_t m = lowMask(src->bits);
        val = raw & m;
        if ((val >> (src->bits - 1)) & 1) val |= ~m;
      }
      if (dst->bits < 64) {
        const std::uint64_t m = lowMask(dst->bits);
        val &= m;
        if (dst->isSigned && ((val >> (dst->bits - 1)) & 1)) val |= ~m;
      }
      out = makeInt(static_cast<std::int64_t>(val));
      return true;
    }
    if (dst->kind == TypeKind::Float && (kind == "sitofp" || kind == "uitofp")) {
      out = makeFloat(kind == "sitofp"
                          ? static_cast<double>(value.i)
                          : static_cast<double>(static_cast<std::uint64_t>(value.i)));
      return true;
    }
    return false;
  }
  if (value.op == Op::ConstFloat && src->kind == TypeKind::Float) {
    if (dst->kind == TypeKind::Float &&
        (kind == "fpext" || kind == "fptrunc" || kind == "bitcast")) {
      out = makeFloat(kind == "fptrunc"
                          ? static_cast<double>(static_cast<float>(value.f))
                          : value.f);
      return true;
    }
    if (dst->kind == TypeKind::Int && (kind == "fptosi" || kind == "fptoui")) {
      if (kind == "fptosi") {
        out = makeInt(static_cast<std::int64_t>(value.f));
      } else {
        out = makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(value.f)));
      }
      return true;
    }
    return false;
  }
  return false;
}

inline bool foldUnary(const std::string& op, const Const& value, Const& out) {
  if (isInt(value)) {
    if (op == "-") { out = makeInt(wrapSub(0, value.i)); return true; }
    if (op == "+") { out = value; return true; }
    if (op == "~") { out = makeInt(~value.i); return true; }
    return false;
  }
  if (isFloat(value)) {
    if (op == "-") { out = makeFloat(-value.f); return true; }
    if (op == "+") { out = value; return true; }
    return false;
  }
  if (isBool(value)) {
    if (op == "!" || op == "not") { out = makeBool(!value.b); return true; }
    return false;
  }
  return false;
}

// Folds `fma(a, b, c)` == a*b + c. All-int uses wrapping arithmetic (exact
// mod 2^64, same as the other folds); float/mixed uses std::fma so the
// folded result matches single-rounding hardware semantics.
inline bool foldFma(const Const& a, const Const& b, const Const& c, Const& out) {
  auto isNum = [](const Const& v) { return isInt(v) || isFloat(v); };
  if (!isNum(a) || !isNum(b) || !isNum(c)) return false;
  if (isInt(a) && isInt(b) && isInt(c)) {
    out = makeInt(wrapAdd(wrapMul(a.i, b.i), c.i));
    return true;
  }
  const double av = isInt(a) ? static_cast<double>(a.i) : a.f;
  const double bv = isInt(b) ? static_cast<double>(b.i) : b.f;
  const double cv = isInt(c) ? static_cast<double>(c.i) : c.f;
  out = makeFloat(std::fma(av, bv, cv));
  return true;
}

}  // namespace ir
}  // namespace a
