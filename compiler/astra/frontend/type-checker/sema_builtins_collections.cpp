#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinCollections(const std::string& name, CallExpr& call) {

  if (name == "map" || name == "filter" || name == "any" || name == "all") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
      inferExpr(*call.args[1].value);
      return typeError();
    }
    if (name == "map") {
      TypePtr mapped = freshVar(Constraint::None, call.span,
                                "the element type produced by 'map'");
      checkArgAgainst(*call.args[1].value, typeFunction({array->elem}, mapped),
                      "in the function passed to 'map'");
      return typeArray(mapped);
    }
    checkArgAgainst(*call.args[1].value, typeFunction({array->elem}, typeBool()),
                    "in the function passed to '" + name + "'");
    return name == "filter" ? typeArray(array->elem) : typeBool();
  }

  if (name == "reduce") {
    if (call.args.size() != 3) {
      builtinArityError(name, call, 3);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    TypePtr accumulator = inferExpr(*call.args[1].value);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'reduce' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
      inferExpr(*call.args[2].value);
      return typeError();
    }
    checkArgAgainst(*call.args[2].value, typeFunction({accumulator, array->elem}, accumulator),
                    "in the function passed to 'reduce'");
    return accumulator;
  }

  if (name == "sum") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'sum' expects an array, found " + typePhrase(array) + ".");
      return typeError();
    }
    return requireNumeric(array->elem, call.args[0].value->span, "The element type of 'sum'");
  }

  if (name == "join") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeString();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'join' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
    }
    TypePtr separator = inferExpr(*call.args[1].value);
    if (!unify(separator, typeString())) {
      reportMismatch(separator, typeString(), call.args[1].value->span,
                     "in the separator passed to 'join'");
    }
    return typeString();
  }

  // Element type produced by iterating a value at runtime. Returns nullptr
  // (after reporting an error) when the type is not iterable.
  auto iterableElem = [&](Expr& arg, const std::string& fn) -> TypePtr {
    TypePtr type = inferExpr(arg);
    TypePtr resolved = resolveType(type);
    switch (resolved->kind) {
      case TypeKind::Array:
      case TypeKind::Set:
      case TypeKind::Range:
        return resolved->elem;
      case TypeKind::Map:
        return resolved->elem;  // iterating a map yields its keys
      case TypeKind::String:
        return typeChar();
      case TypeKind::Tuple:
        return freshVar(Constraint::None, arg.span,
                        "the element type iterated by '" + fn + "'");
      case TypeKind::Var:
        // Numeric-constrained vars (e.g. int literals) can never be
        // iterable; unconstrained vars stay dynamic.
        if (resolved->constraint == Constraint::Integer ||
            resolved->constraint == Constraint::Floating ||
            resolved->constraint == Constraint::Numeric) {
          sink_.error(arg.span,
                      "'" + fn + "' expects an iterable, found " +
                          typePhrase(resolved) + ".");
          return nullptr;
        }
        return freshVar(Constraint::None, arg.span,
                        "the element type iterated by '" + fn + "'");
      default:
        sink_.error(arg.span,
                    "'" + fn + "' expects an iterable, found " +
                        typePhrase(resolved) + ".");
        return nullptr;
    }
  };

  if (name == "sorted" || name == "reversed") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    // sorted() compares elements; the runtime cannot order chars, so strings
    // are rejected there (reversed() only reorders and accepts them).
    if (name == "sorted") {
      TypePtr probe = resolveType(inferExpr(*call.args[0].value));
      if (probe->kind == TypeKind::String) {
        sink_.error(call.args[0].value->span,
                    "'sorted' cannot order the characters of a string.",
                    "Convert to a list of strings first, e.g. via 'map'.");
        return typeError();
      }
      // Reuse the already-inferred type: iterableElem would infer twice.
      TypePtr elem;
      switch (probe->kind) {
        case TypeKind::Array:
        case TypeKind::Set:
        case TypeKind::Range:
        case TypeKind::Map:
          elem = probe->elem;
          break;
        case TypeKind::Tuple:
          elem = freshVar(Constraint::None, call.args[0].value->span,
                          "the element type iterated by 'sorted'");
          break;
        case TypeKind::Var:
          if (probe->constraint == Constraint::Integer ||
              probe->constraint == Constraint::Floating ||
              probe->constraint == Constraint::Numeric) {
            sink_.error(call.args[0].value->span,
                        "'sorted' expects an iterable, found " +
                            typePhrase(probe) + ".");
            return typeError();
          }
          elem = freshVar(Constraint::None, call.args[0].value->span,
                          "the element type iterated by 'sorted'");
          break;
        default:
          sink_.error(call.args[0].value->span,
                      "'sorted' expects an iterable, found " + typePhrase(probe) + ".");
          return typeError();
      }
      return typeArray(elem);
    }
    TypePtr elem = iterableElem(*call.args[0].value, name);
    if (!elem) return typeError();
    // The runtime always produces a fresh list.
    return typeArray(elem);
  }

  if (name == "enumerate") {
    if (call.args.empty() || call.args.size() > 2) {
      sink_.error(call.span, "'enumerate' expects 1 to 2 argument(s), found " +
                             std::to_string(call.args.size()) + ".");
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr elem = iterableElem(*call.args[0].value, name);
    if (!elem) return typeError();
    if (call.args.size() == 2) {
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The start index passed to 'enumerate'");
    }
    return typeArray(typeTuple({typeInt(64, true), elem}));
  }

  if (name == "zip") {
    if (call.args.empty()) {
      sink_.error(call.span, "'zip' expects at least 1 argument, found 0.");
      return typeError();
    }
    std::vector<TypePtr> elems;
    elems.reserve(call.args.size());
    for (auto& arg : call.args) {
      TypePtr elem = iterableElem(*arg.value, name);
      if (!elem) return typeError();
      elems.push_back(elem);
    }
    return typeArray(typeTuple(std::move(elems)));
  }

  if (name == "range") {
    // range(stop) / range(start, stop) / range(start, stop, step); the
    // runtime builds a lazy range object.
    if (call.args.empty() || call.args.size() > 3) {
      sink_.error(call.span, "'range' expects 1 to 3 argument(s), found " +
                             std::to_string(call.args.size()) + ".");
      inferCallArgTypes(call);
      return typeError();
    }
    for (auto& arg : call.args) {
      requireInteger(inferExpr(*arg.value), arg.value->span,
                     "The argument passed to 'range'");
    }
    return typeRange(typeInt(64, true));
  }

  if (name == "tuple") {
    if (call.args.size() > 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    if (call.args.empty()) return typeTuple({});
    // The result has the iterable's length, which is not statically known;
    // validate iterability, then keep the result dynamic.
    if (!iterableElem(*call.args[0].value, name)) return typeError();
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Var;
    t->allowUnresolved = true;
    return t;
  }

  if (name == "list") {
    if (call.args.size() > 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    if (call.args.empty()) {
      return typeArray(freshVar(Constraint::None, call.span,
                                "the element type of an empty list"));
    }
    TypePtr elem = iterableElem(*call.args[0].value, name);
    if (!elem) return typeError();
    return typeArray(elem);
  }

  if (name == "dict") {
    if (call.args.size() > 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr key = freshVar(Constraint::None, call.span, "the key type of 'dict'");
    TypePtr value = freshVar(Constraint::None, call.span, "the value type of 'dict'");
    if (call.args.empty()) return typeMap(key, value);
    TypePtr arg = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(arg);
    if (resolved->kind == TypeKind::Map) return resolved;
    const bool dynamicOk = resolved->kind == TypeKind::Var &&
                           resolved->constraint != Constraint::Integer &&
                           resolved->constraint != Constraint::Floating &&
                           resolved->constraint != Constraint::Numeric;
    // Strings iterate as chars, which are never key/value pairs.
    if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Set ||
        resolved->kind == TypeKind::Range || resolved->kind == TypeKind::Tuple ||
        dynamicOk) {
      // dict(iterable-of-pairs): pair types are not statically recoverable.
      return typeMap(key, value);
    }
    sink_.error(call.args[0].value->span,
                "'dict' expects a map or an iterable of pairs, found " +
                    typePhrase(resolved) + ".");
    return typeError();
  }

  // ---------------------------------------------------------- json module

  return nullptr;
}

}  // namespace a
