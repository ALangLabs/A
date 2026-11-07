#include "type-checker/sema_internal.hpp"

namespace a {

std::vector<TypePtr> Sema::inferCallArgTypes(CallExpr& call) {
  std::vector<TypePtr> types;
  types.reserve(call.args.size());
  for (auto& arg : call.args) types.push_back(inferExpr(*arg.value));
  return types;
}

void Sema::builtinArityError(const std::string& name, CallExpr& call,
                             std::size_t expected) {
  sink_.error(call.span, "'" + name + "' expects " + std::to_string(expected) +
                             " argument(s), found " + std::to_string(call.args.size()) + ".");
}

TypePtr Sema::inferAnyCallArg(CallExpr& call, std::size_t index,
                              const std::string& context) {
  TypePtr type = inferExpr(*call.args[index].value);
  if (resolveType(type)->kind == TypeKind::Void) {
    sink_.error(call.args[index].value->span,
                "Cannot pass a void value " + context + ".");
  }
  return type;
}

TypePtr Sema::checkBuiltinCall(const std::string& name, CallExpr& call) {
  // print/eprint take *objects plus Python-style keyword options
  // (sep/end/file/flush); every other builtin stays positional-only.

  if (name == "print" || name == "eprint") {
    return checkPrintCall(name, call);
  }

  if (name == "ic") {
    return checkIcCall(call);
  }

  if (name == "__file__" || name == "__source_file__" || name == "__func__" ||
      name == "__function__" || name == "__debug_time__" || name == "__debug_context__") {
    if (!call.args.empty()) builtinArityError(name, call, 0);
    inferCallArgTypes(call);
    if (name == "__debug_time__") return typeString();
    return typeString();
  }

  if (name == "__line__" || name == "__source_line__") {
    if (!call.args.empty()) builtinArityError(name, call, 0);
    inferCallArgTypes(call);
    return typeInt(64, true);
  }

  if (name == "__repr__" || name == "__debug_repr__") {
    if (call.args.size() != 1) builtinArityError(name, call, 1);
    inferCallArgTypes(call);
    return typeString();
  }

  if (name == "__ic_format__") {
    if (call.args.size() < 1 || call.args.size() > 3) {
      sink_.error(call.span, "'__ic_format__' expects context/label/value (1 to 3) arguments, found " +
                                 std::to_string(call.args.size()) + ".");
    }
    inferCallArgTypes(call);
    return typeString();
  }

  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Builtin functions do not accept named arguments.");
      return typeError();
    }
  }

  if (TypePtr result = checkBuiltinValues(name, call)) return result;
  if (TypePtr result = checkBuiltinIo(name, call)) return result;
  if (TypePtr result = checkBuiltinMemory(name, call)) return result;
  if (TypePtr result = checkBuiltinNumeric(name, call)) return result;
  if (TypePtr result = checkBuiltinSystem(name, call)) return result;
  if (TypePtr result = checkBuiltinCollections(name, call)) return result;
  if (TypePtr result = checkBuiltinJson(name, call)) return result;
  if (TypePtr result = checkBuiltinTesting(name, call)) return result;
  if (TypePtr result = checkBuiltinLog(name, call)) return result;
  if (TypePtr result = checkBuiltinMetrics(name, call)) return result;
  if (TypePtr result = checkBuiltinTrace(name, call)) return result;
  if (TypePtr result = checkBuiltinNet(name, call)) return result;
  if (TypePtr result = checkBuiltinKernels(name, call)) return result;

  return typeError();
}

TypePtr Sema::checkPrintCall(const std::string& name, CallExpr& call) {
  bool seenSep = false, seenEnd = false, seenFile = false, seenFlush = false;
  bool seenNamed = false;
  for (auto& arg : call.args) {
    if (arg.name.empty()) {
      if (seenNamed) {
        sink_.error(arg.value->span, "Positional arguments must come before keyword arguments in '" + name + "'.");
      }
      TypePtr type = inferExpr(*arg.value);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(arg.value->span, "Cannot print a void value.");
      }
      continue;
    }
    seenNamed = true;
    TypePtr expected;
    bool* seen = nullptr;
    if (arg.name == "sep") { seen = &seenSep; expected = typeString(); }
    else if (arg.name == "end") { seen = &seenEnd; expected = typeString(); }
    else if (arg.name == "file") { seen = &seenFile; expected = typeIO(); }
    else if (arg.name == "flush") { seen = &seenFlush; expected = typeBool(); }
    if (seen == nullptr) {
      sink_.error(arg.nameSpan, "'" + name + "() got an unexpected keyword argument '" + arg.name + "'.",
                  "Accepted keywords: sep, end, file, flush.");
      inferExpr(*arg.value);
      continue;
    }
    if (*seen) {
      sink_.error(arg.nameSpan, "Multiple values for keyword argument '" + arg.name + "' in '" + name + "'.");
    }
    *seen = true;
    TypePtr type = inferExpr(*arg.value);
    if (!unify(type, expected)) {
      reportMismatch(type, expected, arg.value->span,
                     "in the '" + arg.name + "' argument passed to '" + name + "'");
    }
  }
  return typeVoid();
}

}  // namespace a
