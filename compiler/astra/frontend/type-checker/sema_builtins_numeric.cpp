#include "type-checker/sema_internal.hpp"

#include <cmath>

namespace a {

TypePtr Sema::checkBuiltinNumeric(const std::string& name, CallExpr& call) {

  if (name == "int" || name == "float") {
    TypePtr result = name == "int" ? typeInt(64, true) : typeFloat(64);
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return result;
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Var) {
      if (!constrainVar(type, Constraint::Numeric)) {
        /* dynamic: inferred at runtime */ (void)0;
      }
    } else if (resolved->kind != TypeKind::Int && resolved->kind != TypeKind::Float &&
               resolved->kind != TypeKind::Bool && resolved->kind != TypeKind::String) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a number, bool, or numeric string, found " +
                      typePhrase(resolved) + ".");
    }
    return result;
  }

  if (name == "abs") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    return requireNumeric(type, call.args[0].value->span, "The argument of 'abs'");
  }

  if (name == "min" || name == "max") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    if (!unify(a, b)) {
      sink_.error(call.span, "'" + name + "' requires arguments of the same numeric type.");
      return typeError();
    }
    return requireNumeric(a, call.span, "The arguments of '" + name + "'");
  }

  if (name == "pow") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeFloat(64);
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    requireNumeric(a, call.args[0].value->span, "The base of 'pow'");
    requireNumeric(b, call.args[1].value->span, "The exponent of 'pow'");
    return typeFloat(64);
  }

  if (name == "divmod") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    requireNumeric(a, call.args[0].value->span, "The first argument of 'divmod'");
    requireNumeric(b, call.args[1].value->span, "The second argument of 'divmod'");
    TypePtr ra = resolveType(a);
    TypePtr rb = resolveType(b);
    bool isFloat = ra->kind == TypeKind::Float || rb->kind == TypeKind::Float;
    TypePtr elem = isFloat ? typeFloat(64) : typeInt(64, true);
    return typeTuple({elem, elem});
  }

  if (name == "atan2") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeFloat(64);
    }
    requireNumeric(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The y argument of 'atan2'");
    requireNumeric(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The x argument of 'atan2'");
    return typeFloat(64);
  }

  if (name == "random") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeFloat(64);
  }

  if (name == "random_int") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The low bound of 'random_int'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The high bound of 'random_int'");
    return typeInt(64, true);
  }

  if (isMathBuiltin(name)) {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeFloat(64);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    return typeFloat(64);
  }

  if (isConversionBuiltin(name)) {
    TypePtr target = builtinTypeByName(name);
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return target;
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    // Constant arguments to sized-int conversions must fit the target; other
    // values keep the existing runtime-truncation semantics.
    if (target && target->kind == TypeKind::Int) {
      std::int64_t value = 0;
      bool negated = false;
      if (intLiteralValue(*call.args[0].value, value, negated)) {
        checkSizedIntRange(call.args[0].value->span, value, negated, target);
      } else if (const auto* flit =
                     dynamic_cast<const FloatLitExpr*>(call.args[0].value.get())) {
        // Float-to-int conversions truncate toward zero (i64(3.9) == 3).
        if (std::isfinite(flit->value) &&
            std::abs(flit->value) <= 9.0e18) {
          const std::int64_t truncated = static_cast<std::int64_t>(flit->value);
          checkSizedIntRange(call.args[0].value->span, truncated, truncated < 0,
                             target);
        }
      }
    }
    return target;
  }

  // --------------------------------------------- LLVM-style intrinsics
  // Bit intrinsics (llvm.cttz/ctlz/ctpop/bswap/fshl-family analogs), float
  // intrinsics (llvm.fma/fabs/copysign), integer min/max, *.with.overflow
  // arithmetic, and llvm.expect-style branch hints.

  {
    auto intTypeOf = [&](std::size_t index, const std::string& context) {
      TypePtr type = inferExpr(*call.args[index].value);
      TypePtr resolved = resolveType(type);
      if (resolved->kind == TypeKind::Int) return resolved;
      if (resolved->kind == TypeKind::Var) return typeInt(64, true);
      sink_.error(call.args[index].value->span,
                  "'" + name + "' expects an integer " + context + ", found " +
                      typePhrase(resolved) + ".");
      return typeInt(64, true);
    };
    static const char* bitUnary[] = {"ctz", "clz", "popcount", "bswap"};
    static const char* bitBinary[] = {"rotl", "rotr"};
    static const char* intMinMax[] = {"smin", "smax", "umin", "umax"};
    static const char* ovf[] = {"sadd_overflow", "ssub_overflow", "smul_overflow",
                                "uadd_overflow", "usub_overflow", "umul_overflow"};
    auto isOne = [&](const char* const* names, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i)
        if (name == names[i]) return true;
      return false;
    };
    if (isOne(bitUnary, 4) || isOne(bitBinary, 2) || isOne(intMinMax, 4) ||
        isOne(ovf, 6)) {
      const std::size_t expected = isOne(bitUnary, 4) ? 1 : 2;
      if (call.args.size() != expected) {
        builtinArityError(name, call, expected);
        inferCallArgTypes(call);
        return isOne(ovf, 6) ? typeTuple({typeInt(64, true), typeBool()})
                             : typeInt(64, true);
      }
      TypePtr first = intTypeOf(0, "argument");
      if (expected == 2) (void)intTypeOf(1, "argument");
      if (isOne(ovf, 6)) return typeTuple({first, typeBool()});
      return first;
    }
    if (name == "fma" || name == "copysign") {
      const std::size_t expected = name == "fma" ? 3 : 2;
      if (call.args.size() != expected) {
        builtinArityError(name, call, expected);
        inferCallArgTypes(call);
        return typeFloat(64);
      }
      for (std::size_t i = 0; i < expected; ++i) {
        TypePtr type = inferExpr(*call.args[i].value);
        requireNumeric(type, call.args[i].value->span, "The argument of '" + name + "'");
      }
      return typeFloat(64);
    }
    if (name == "fabs") {
      if (call.args.size() != 1) {
        builtinArityError(name, call, 1);
        inferCallArgTypes(call);
        return typeFloat(64);
      }
      TypePtr type = inferExpr(*call.args[0].value);
      requireNumeric(type, call.args[0].value->span, "The argument of 'fabs'");
      return typeFloat(64);
    }
    if (name == "expect" || name == "likely" || name == "unlikely") {
      const std::size_t maxArgs = name == "expect" ? 2 : 1;
      if (call.args.empty() || call.args.size() > maxArgs) {
        sink_.error(call.span, "'" + name + "' expects 1 to " +
                               std::to_string(maxArgs) + " argument(s), found " +
                               std::to_string(call.args.size()) + ".");
        inferCallArgTypes(call);
        return typeError();
      }
      TypePtr type = inferExpr(*call.args[0].value);
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return type;
    }
  }

  // -------------------------------------------------- terminal/env/repl
  // Interactive-REPL primitives; argument shapes are validated by the
  // interpreter, so the checker only fixes the result type.

  return nullptr;
}

}  // namespace a
