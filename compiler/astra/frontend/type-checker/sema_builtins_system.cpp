#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinSystem(const std::string& name, CallExpr& call) {

  if (name == "now") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeInt(64, true);
  }

  if (name == "sleep") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The duration passed to 'sleep'");
    return typeVoid();
  }

  if (name == "panic") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the message passed to 'panic'");
    }
    return typeVoid();
  }

  if (name == "assert") {
    if (call.args.empty() || call.args.size() > 2) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireBool(inferExpr(*call.args[0].value), call.args[0].value->span,
                "The condition of 'assert'");
    if (call.args.size() == 2) {
      TypePtr type = inferExpr(*call.args[1].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[1].value->span,
                       "in the message passed to 'assert'");
      }
    }
    return typeVoid();
  }

  if (name == "assert_eq") {
    if (call.args.size() < 2 || call.args.size() > 3) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr left = inferExpr(*call.args[0].value);
    TypePtr right = inferExpr(*call.args[1].value);
    if (!unify(left, right)) {
      reportMismatch(right, left, call.args[1].value->span, "in the second argument of 'assert_eq'");
    }
    if (call.args.size() == 3) {
      TypePtr type = inferExpr(*call.args[2].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[2].value->span,
                       "in the message passed to 'assert_eq'");
      }
    }
    return typeVoid();
  }

  if (name == "terminal_isatty" || name == "terminal_raw_mode") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeBool();
  }

  if (name == "terminal_read_char" || name == "terminal_read_char_ms") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeOptional(typeString());
  }

  if (name == "terminal_write" || name == "terminal_flush") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeVoid();
  }

  if (name == "terminal_size") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeArray(typeInt(64, true));
  }

  if (name == "terminal_home" || name == "env_get") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeString();
  }

  if (name == "repl_eval") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeInt(64, true);
  }

  if (name == "repl_quiet") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeVoid();
  }

  if (name == "struct_name") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'struct_name'");
    return typeString();
  }

  if (name == "struct_methods") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'struct_methods'");
    return typeArray(typeString());
  }

  if (name == "fn_signature" || name == "fn_doc") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    inferAnyCallArg(call, 0, "to '" + name + "'");
    return typeString();
  }

  return nullptr;
}

}  // namespace a
