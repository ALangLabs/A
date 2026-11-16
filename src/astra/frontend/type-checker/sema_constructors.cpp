#include "type-checker/sema_internal.hpp"

namespace a {
TypePtr Sema::checkFunctionValueCall(const std::string& name, TypePtr fnType, CallExpr& call) {
  if (!call.typeArgs.empty()) {
    // Function values (variables, lambdas, parameters) carry no generic
    // signature, so there is nothing to instantiate the arguments into.
    sink_.error(call.span,
                "'" + name + "' is a function value; explicit type arguments "
                "are only supported on generic function declarations.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    for (auto& ta : call.typeArgs) resolveTypeExpr(*ta);
    return typeError();
  }
  TypePtr resolved = resolveType(fnType);
  if (resolved->kind == TypeKind::Error) {
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  if (resolved->kind == TypeKind::Var) {
    // Dynamic value: callability and signature are decided at runtime.
    for (auto& arg : call.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (resolved->kind != TypeKind::Function) {
    sink_.error(call.callee->span,
                "'" + name + "' is a value of type '" + typeName(resolved) +
                    "', not a function.",
                "Only functions and lambdas can be called.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are not supported for function calls.");
      return typeError();
    }
  }
  if (call.args.size() != resolved->params.size()) {
    sink_.error(call.span,
                "This function expects " + std::to_string(resolved->params.size()) +
                    " argument(s), found " + std::to_string(call.args.size()) + ".");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    checkArgAgainst(*call.args[i].value, resolved->params[i],
                    "in argument " + std::to_string(i + 1) + " to '" + name + "'");
  }
  return resolved->ret;
}

TypePtr Sema::checkFunctionCall(const FnSig& sig, CallExpr& call) {
  FnSig effective = sig;
  // Explicit call-site type arguments (`f<i64>(x)`) pin the signature's type
  // parameters directly instead of leaving them to be inferred from the
  // arguments. Anything not pinned still gets a fresh variable per call.
  std::vector<TypePtr> explicitTypes;
  if (!call.typeArgs.empty()) {
    if (sig.typeParams.empty()) {
      sink_.error(call.span,
                  "Function '" + sig.name + "' is not generic; it takes no "
                  "type arguments.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      for (auto& ta : call.typeArgs) resolveTypeExpr(*ta);
      return typeError();
    }
    if (call.typeArgs.size() != sig.typeParamVars.size()) {
      sink_.error(call.span,
                  "Function '" + sig.name + "' expects " +
                      std::to_string(sig.typeParamVars.size()) +
                      " type argument(s), found " +
                      std::to_string(call.typeArgs.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      for (auto& ta : call.typeArgs) resolveTypeExpr(*ta);
      return typeError();
    }
    explicitTypes.reserve(call.typeArgs.size());
    for (auto& ta : call.typeArgs) explicitTypes.push_back(resolveTypeExpr(*ta));
  }
  if (!sig.typeParams.empty()) {
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < sig.typeParamVars.size(); ++i) {
      TypePtr inst;
      if (i < explicitTypes.size()) {
        inst = explicitTypes[i];
      } else {
        inst = freshVar(Constraint::None,
                        sig.typeParamVars[i]->varSpan,
                        "type parameter '" + sig.typeParams[i] + "'");
        inst->allowUnresolved = true;
      }
      mapping[sig.typeParamVars[i]] = inst;
    }
    std::vector<TypePtr> params;
    params.reserve(sig.paramTypes.size());
    for (const auto& p : sig.paramTypes) params.push_back(instantiateType(p, mapping));
    effective.paramTypes = std::move(params);
    effective.retType = instantiateType(sig.retType, mapping);
  }

  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are only supported for struct construction.");
      return typeError();
    }
  }
  std::size_t required = effective.requiredArgs();
  std::size_t total = effective.paramTypes.size();
  bool arityOk = call.args.size() >= required &&
                 (effective.variadic || call.args.size() <= total);
  if (!arityOk) {
    std::string expect;
    if (effective.variadic) {
      expect = required == 0 ? "any number of argument(s)"
                             : "at least " + std::to_string(required) + " argument(s)";
    } else if (required == total) {
      expect = std::to_string(required) + " argument(s)";
    } else {
      expect = "between " + std::to_string(required) + " and " +
               std::to_string(total) + " argument(s)";
    }
    sink_.error(call.span,
                "Function '" + effective.name + "' expects " + expect +
                    ", found " + std::to_string(call.args.size()) + ".");
    return typeError();
  }
  // Element type of the variadic list parameter (null when untyped).
  TypePtr varElem;
  if (effective.variadic && total > 0) {
    TypePtr rt = resolveType(effective.paramTypes.back());
    if (rt && rt->kind == TypeKind::Array) varElem = rt->elem;
  }
  // A parameter written without a type annotation is dynamically typed: the
  // shared signature variable is inferred from the first call site, so it must
  // not turn into a hard constraint for later call sites. Only enforce
  // arguments where the declaration actually names a type.
  auto declaredParamTyped = [&](std::size_t i) -> bool {
    return sig.decl && i < sig.decl->params.size() && sig.decl->params[i].type != nullptr;
  };
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    if (effective.variadic && i + 1 >= total) {
      if (varElem) {
        checkArgAgainst(*call.args[i].value, varElem,
                        "in variadic argument " + std::to_string(i + 1) + " to '" + effective.name + "'");
      } else {
        inferExpr(*call.args[i].value);
      }
      continue;
    }
    if (!declaredParamTyped(i)) {
      // Dynamically typed parameter: refine its inferred type from the value but
      // do not turn the shared signature variable into a hard constraint.
      unify(inferExpr(*call.args[i].value), effective.paramTypes[i]);
      continue;
    }
    checkArgAgainst(*call.args[i].value, effective.paramTypes[i],
                   "in argument " + std::to_string(i + 1) + " to '" + effective.name + "'");
  }
  return effective.retType;
}

TypePtr Sema::checkStructConstruction(const StructSym& sym, CallExpr& call) {
  std::unordered_map<TypePtr, TypePtr> mapping;
  std::vector<TypePtr> typeArgs;
  if (!call.typeArgs.empty()) {
    // `Point<i64>(...)` pins the struct's type parameters explicitly.
    if (call.typeArgs.size() != sym.typeParamVars.size()) {
      sink_.error(call.span,
                  "Struct '" + sym.name + "' expects " +
                      std::to_string(sym.typeParamVars.size()) +
                      " type argument(s), found " +
                      std::to_string(call.typeArgs.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      for (auto& ta : call.typeArgs) resolveTypeExpr(*ta);
      return typeStruct(sym.name);
    }
    for (auto& ta : call.typeArgs) typeArgs.push_back(resolveTypeExpr(*ta));
    for (std::size_t i = 0; i < sym.typeParamVars.size(); ++i) {
      mapping[sym.typeParamVars[i]] = typeArgs[i];
    }
  } else {
    for (std::size_t i = 0; i < sym.typeParamVars.size(); ++i) {
      TypePtr fresh = freshVar(Constraint::None, sym.typeParamVars[i]->varSpan,
                              "type parameter '" + sym.typeParams[i] + "'");
      fresh->allowUnresolved = true;
      mapping[sym.typeParamVars[i]] = fresh;
      typeArgs.push_back(fresh);
    }
  }
  std::vector<std::pair<std::string, TypePtr>> fields;
  fields.reserve(sym.fields.size());
  for (const auto& f : sym.fields) {
    fields.emplace_back(f.first, instantiateType(f.second, mapping));
  }

  bool anyNamed = false;
  bool allNamed = true;
  for (auto& arg : call.args) {
    if (!arg.name.empty()) anyNamed = true;
    else allNamed = false;
  }

  if (anyNamed && !allNamed) {
    sink_.error(call.span, "Cannot mix named and positional arguments in a struct constructor.");
    return typeStruct(sym.name);
  }

  if (!anyNamed) {
    if (call.args.size() != fields.size()) {
      sink_.error(call.span,
                  "Struct '" + sym.name + "' expects " + std::to_string(fields.size()) +
                      " field(s), found " + std::to_string(call.args.size()) + ".");
      return typeStruct(sym.name);
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      TypePtr argType = inferExpr(*call.args[i].value);
      if (!unify(argType, fields[i].second)) {
        reportMismatch(argType, fields[i].second, call.args[i].value->span,
                       "in field '" + fields[i].first + "' of '" + sym.name + "'");
      }
    }
    TypePtr result = typeStruct(sym.name);
    if (!typeArgs.empty()) result->params = typeArgs;
    return result;
  }

  std::vector<bool> provided(fields.size(), false);
  for (auto& arg : call.args) {
    std::size_t index = fields.size();
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].first == arg.name) {
        index = i;
        break;
      }
    }
    if (index == fields.size()) {
      sink_.error(arg.nameSpan, "'" + sym.name + "' has no field named '" + arg.name + "'.");
      inferExpr(*arg.value);
      continue;
    }
    if (provided[index]) {
      sink_.error(arg.nameSpan, "Field '" + arg.name + "' is provided more than once.");
      inferExpr(*arg.value);
      continue;
    }
    provided[index] = true;
    TypePtr argType = inferExpr(*arg.value);
    if (!unify(argType, fields[index].second)) {
      reportMismatch(argType, fields[index].second, arg.value->span,
                     "in field '" + arg.name + "' of '" + sym.name + "'");
    }
  }
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (!provided[i]) {
      sink_.error(call.span, "Missing field '" + fields[i].first + "' in '" + sym.name +
                                 "' constructor.");
    }
  }
  TypePtr result = typeStruct(sym.name);
  if (!typeArgs.empty()) result->params = typeArgs;
  return result;
}

TypePtr Sema::enumTypeFromInfo(const EnumInfo& info, NamedTypeExpr& named) {
  if (info.builtinKind == 1) {  // prelude Option<T> maps onto the built-in optional.
    if (named.typeArgs.size() > 1) {
      sink_.error(named.span, "'Option' takes one type argument: Option<T>.");
      return typeError();
    }
    TypePtr elem;
    if (named.typeArgs.empty()) {
      elem = freshVar(Constraint::None, named.span, "element type of 'Option'");
      elem->allowUnresolved = true;
    } else {
      elem = resolveTypeExpr(*named.typeArgs[0]);
    }
    return typeOptional(elem);
  }
  if (info.builtinKind == 2) {  // prelude Result<T, E> maps onto the built-in result.
    if (named.typeArgs.empty()) {
      TypePtr value = freshVar(Constraint::None, named.span, "value type of 'Result'");
      TypePtr error = freshVar(Constraint::None, named.span, "error type of 'Result'");
      value->allowUnresolved = true;
      error->allowUnresolved = true;
      return typeResult(value, error);
    }
    if (named.typeArgs.size() != 2) {
      sink_.error(named.span, "'Result' requires two type arguments: Result<T, E>.");
      return typeError();
    }
    return typeResult(resolveTypeExpr(*named.typeArgs[0]), resolveTypeExpr(*named.typeArgs[1]));
  }
  if (!info.algebraic) return typeInt(32, true);  // plain enums are i32-backed
  TypePtr t = typeEnum(info.name);
  if (named.typeArgs.empty()) {
    for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
      TypePtr fresh = freshVar(Constraint::None, named.span,
                               "type parameter of '" + shortName(info.name) + "'");
      fresh->allowUnresolved = true;
      t->params.push_back(fresh);
    }
    return t;
  }
  if (named.typeArgs.size() != info.typeParams.size()) {
    sink_.error(named.span,
                "'" + shortName(info.name) + "' expects " +
                    std::to_string(info.typeParams.size()) + " type argument(s), found " +
                    std::to_string(named.typeArgs.size()) + ".");
    return typeError();
  }
  for (auto& arg : named.typeArgs) t->params.push_back(resolveTypeExpr(*arg));
  return t;
}

// True when the expression before `.` denotes the enum's namespace (the type
// name itself) rather than a value of that enum type.
bool Sema::enumNamespaceUse(ExprPtr& object, const std::string& enumQ) {
  if (auto* id = dynamic_cast<IdentExpr*>(object.get())) {
    if (lookupVar(id->name)) return false;
    if (moduleAliases_.count(id->name)) return false;
    if (qualified(id->name) == enumQ) return true;
    auto alias = structAliases_.find(id->name);
    return alias != structAliases_.end() && alias->second == enumQ;
  }
  if (auto* fe = dynamic_cast<FieldExpr*>(object.get())) {
    TypePtr inner = resolveType(inferExpr(*fe->object));
    if (inner->kind == TypeKind::Module) {
      if (inner->moduleId < 0) return false;
      const StringMap<std::string>& enumExports = exportsFor(inner->moduleId).enums;
      return enumExports.count(fe->field) && enumExports.at(fe->field) == enumQ;
    }
    if (inner->kind == TypeKind::Enum && inner->structName == enumQ) {
      return enumNamespaceUse(fe->object, inner->structName);
    }
  }
  return false;
}

TypePtr Sema::checkVariantConstruction(EnumInfo& info, FieldExpr& field, CallExpr& call) {
  if (info.builtinKind) {
    sink_.error(field.fieldSpan,
                "'" + shortName(info.name) + "' is a prelude type; its values are built-in.",
                "Write Some(value), None, Ok(value), or Err(error) without a module prefix.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  const VariantInfo* variant = info.findVariant(field.field);
  if (!variant) {
    sink_.error(field.fieldSpan,
                "Enum '" + shortName(info.name) + "' has no variant named '" + field.field + "'.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are only supported for struct construction.");
      break;
    }
  }

  std::unordered_map<TypePtr, TypePtr> mapping;
  std::vector<TypePtr> typeArgs;
  if (!call.typeArgs.empty()) {
    if (call.typeArgs.size() != info.typeParamVars.size()) {
      sink_.error(call.span,
                  "Enum '" + shortName(info.name) + "' expects " +
                      std::to_string(info.typeParamVars.size()) +
                      " type argument(s), found " +
                      std::to_string(call.typeArgs.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      for (auto& ta : call.typeArgs) resolveTypeExpr(*ta);
      TypePtr result = typeEnum(info.name);
      for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
        TypePtr fresh = freshVar(Constraint::None, info.typeParamVars[i]->varSpan,
                                 "type parameter '" + info.typeParams[i] + "'");
        fresh->allowUnresolved = true;
        result->params.push_back(fresh);
      }
      return result;
    }
    for (auto& ta : call.typeArgs) typeArgs.push_back(resolveTypeExpr(*ta));
    for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
      mapping[info.typeParamVars[i]] = typeArgs[i];
    }
  } else {
    for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
      TypePtr fresh = freshVar(Constraint::None, info.typeParamVars[i]->varSpan,
                               "type parameter '" + info.typeParams[i] + "'");
      fresh->allowUnresolved = true;
      mapping[info.typeParamVars[i]] = fresh;
      typeArgs.push_back(fresh);
    }
  }

  std::vector<std::pair<std::string, TypePtr>> fields;
  fields.reserve(variant->fields.size());
  for (const auto& f : variant->fields) {
    fields.emplace_back(f.first, instantiateType(f.second, mapping));
  }

  if (call.args.size() != fields.size()) {
    sink_.error(call.span,
                "Variant '" + shortName(info.name) + "." + variant->name + "' expects " +
                    std::to_string(fields.size()) + " payload argument(s), found " +
                    std::to_string(call.args.size()) + ".");
    for (auto& arg : call.args) inferExpr(*arg.value);
    TypePtr result = typeEnum(info.name);
    result->params = typeArgs;
    return result;
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    TypePtr argType = inferExpr(*call.args[i].value);
    if (!unify(argType, fields[i].second)) {
      reportMismatch(argType, fields[i].second, call.args[i].value->span,
                     "in payload field '" + fields[i].first + "' of '" + variant->name + "'");
    }
  }
  TypePtr result = typeEnum(info.name);
  result->params = typeArgs;
  return result;
}

}  // namespace a
