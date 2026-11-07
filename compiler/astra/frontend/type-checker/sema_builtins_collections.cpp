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

  // ---------------------------------------------------------- json module

  return nullptr;
}

}  // namespace a
