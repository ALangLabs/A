#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinIo(const std::string& name, CallExpr& call) {

  if (name == "open") {
    if (call.args.size() < 1 || call.args.size() > 2) {
      sink_.error(call.span, "'open' expects 1 or 2 argument(s), found " +
                                 std::to_string(call.args.size()) + ".");
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr path = inferExpr(*call.args[0].value);
    if (!unify(path, typeString())) {
      reportMismatch(path, typeString(), call.args[0].value->span, "in the path passed to 'open'");
    }
    if (call.args.size() == 2) {
      TypePtr mode = inferExpr(*call.args[1].value);
      if (!unify(mode, typeString())) {
        reportMismatch(mode, typeString(), call.args[1].value->span, "in the mode passed to 'open'");
      }
    }
    return typeIO();
  }

  if (name == "close") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr stream = inferExpr(*call.args[0].value);
    if (!unify(stream, typeIO())) {
      reportMismatch(stream, typeIO(), call.args[0].value->span, "in the stream passed to 'close'");
    }
    return typeVoid();
  }

  if (name == "input") {
    if (call.args.size() > 1) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
      return typeString();
    }
    if (call.args.size() == 1) {
      TypePtr type = inferExpr(*call.args[0].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[0].value->span, "in the input prompt");
      }
    }
    return typeString();
  }

  if (name == "flush") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeVoid();
  }

  if (name == "read_file") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span, "in the path passed to 'read_file'");
    }
    return typeString();
  }

  if (name == "write_file" || name == "append_file") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    for (std::size_t i = 0; i < 2; ++i) {
      TypePtr type = inferExpr(*call.args[i].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[i].value->span,
                       "in argument " + std::to_string(i + 1) + " to '" + name + "'");
      }
    }
    return typeVoid();
  }

  if (name == "file_exists") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeBool();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to 'file_exists'");
    }
    return typeBool();
  }

  if (name == "list_dir") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeString());
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to 'list_dir'");
    }
    return typeArray(typeString());
  }

  if (name == "remove_file" || name == "create_dir" || name == "remove_dir") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to '" + name + "'");
    }
    return typeVoid();
  }

  if (name == "rename") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    for (std::size_t i = 0; i < 2; ++i) {
      TypePtr type = inferExpr(*call.args[i].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[i].value->span,
                       "in argument " + std::to_string(i + 1) + " to 'rename'");
      }
    }
    return typeVoid();
  }

  if (name == "current_dir") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeString();
  }

  return nullptr;
}

}  // namespace a
