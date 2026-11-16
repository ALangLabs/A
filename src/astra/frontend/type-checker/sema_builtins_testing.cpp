#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinTesting(const std::string& name, CallExpr& call) {

  if (name == "testing_seed") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The seed passed to 'testing.seed'");
    return typeVoid();
  }

  if (name == "testing_for_all_int") {
    if (call.args.size() != 4) {
      builtinArityError(name, call, 4);
      inferCallArgTypes(call);
      return typeBool();
    }
    for (std::size_t i = 0; i < 3; ++i) {
      requireInteger(inferExpr(*call.args[i].value), call.args[i].value->span,
                     "The generator bound passed to 'testing.for_all_int'");
    }
    checkArgAgainst(*call.args[3].value, typeFunction({typeInt(64, true)}, typeBool()),
                    "in the property passed to 'testing.for_all_int'");
    return typeBool();
  }

  if (name == "testing_for_all_float") {
    if (call.args.size() != 4) {
      builtinArityError(name, call, 4);
      inferCallArgTypes(call);
      return typeBool();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The run count passed to 'testing.for_all_float'");
    for (std::size_t i = 1; i < 3; ++i) {
      requireNumeric(inferExpr(*call.args[i].value), call.args[i].value->span,
                     "A bound passed to 'testing.for_all_float'");
    }
    checkArgAgainst(*call.args[3].value, typeFunction({typeFloat(64)}, typeBool()),
                    "in the property passed to 'testing.for_all_float'");
    return typeBool();
  }

  if (name == "testing_for_all_bool") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeBool();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The run count passed to 'testing.for_all_bool'");
    checkArgAgainst(*call.args[1].value, typeFunction({typeBool()}, typeBool()),
                    "in the property passed to 'testing.for_all_bool'");
    return typeBool();
  }

  if (name == "testing_for_all_string") {
    if (call.args.size() != 3) {
      builtinArityError(name, call, 3);
      inferCallArgTypes(call);
      return typeBool();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The run count passed to 'testing.for_all_string'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The maximum length passed to 'testing.for_all_string'");
    checkArgAgainst(*call.args[2].value, typeFunction({typeString()}, typeBool()),
                    "in the property passed to 'testing.for_all_string'");
    return typeBool();
  }

  if (name == "testing_for_all_elem") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeBool();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'testing.for_all_elem' expects an array, found " + typePhrase(array) + ".");
      inferExpr(*call.args[1].value);
      return typeBool();
    }
    checkArgAgainst(*call.args[1].value, typeFunction({array->elem}, typeBool()),
                    "in the property passed to 'testing.for_all_elem'");
    return typeBool();
  }

  // ----------------------------------------------------------- log module

  return nullptr;
}

}  // namespace a
