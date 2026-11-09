#include "type-checker/sema_internal.hpp"

#include <algorithm>

namespace a {
// ------------------------------------------------------------ declarations

void Sema::registerStructLayout(StructDecl& decl) {
  auto savedGenericParams = std::move(genericParams_);
  const std::string q = qualified(decl.name);
  StructSym& sym = structs_[q];
  sym.name = q;
  sym.decl = &decl;
  sym.typeParams = decl.typeParams;
  genericParams_.clear();
  for (const auto& name : decl.typeParams) {
    TypePtr var = freshVar(Constraint::None, decl.nameSpan,
                           "type parameter '" + name + "' of '" + decl.name + "'");
    var->allowUnresolved = true;
    genericParams_[name] = var;
    sym.typeParamVars.push_back(var);
  }

  // Inherit fields from baseClass if present
  if (!decl.baseClass.empty()) {
    std::string baseQ = qualified(decl.baseClass);
    auto baseIt = structs_.find(baseQ);
    if (baseIt == structs_.end()) {
      baseIt = structs_.find(decl.baseClass);
    }
    if (baseIt != structs_.end()) {
      for (const auto& f : baseIt->second.fields) {
        sym.fields.push_back(f);
      }
    }
  }

  for (FieldDecl& field : decl.fields) {
    for (const auto& existing : sym.fields) {
      if (existing.first == field.name) {
        sink_.error(field.span, "Duplicate field '" + field.name + "' in struct '" + decl.name + "'.");
      }
    }
    TypePtr type = field.type ? resolveTypeExpr(*field.type) : freshVar(Constraint::None, field.span, "the type of field '" + field.name + "'");
    if (!field.type) resolveType(type)->allowUnresolved = true;
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(field.span, "A struct field cannot have type void.");
      type = typeError();
    }
    sym.fields.emplace_back(field.name, type);
  }
  genericParams_ = std::move(savedGenericParams);
}

void Sema::registerEnum(EnumDecl& decl) {
  const std::string q = qualified(decl.name);
  EnumInfo& info = enums_[q];
  info.name = q;

  if (decl.isAlgebraic()) {
    info.algebraic = true;
    info.typeParams = decl.typeParams;
    std::int64_t tag = 0;
    for (EnumVariant& variant : decl.variants) {
      if (info.values.count(variant.name)) {
        sink_.error(variant.span, "Duplicate variant '" + variant.name + "' in enum '" +
                                      decl.name + "'.");
        continue;
      }
      if (variant.value) {
        sink_.error(variant.value->span,
                    "Algebraic enum variants cannot have integer values.",
                    "Remove '= ...' or give the variant a payload: " + variant.name +
                        "(field: type).");
      }
      VariantInfo vi;
      vi.name = variant.name;
      for (const FieldDecl& field : variant.fields) {
        for (const auto& existing : vi.fields) {
          if (existing.first == field.name) {
            sink_.error(field.span, "Duplicate payload field '" + field.name + "' in variant '" +
                                        variant.name + "'.");
          }
        }
        vi.fields.emplace_back(field.name, nullptr);  // resolved in resolveEnumPayloads
      }
      info.variants.push_back(std::move(vi));
      info.values[variant.name] = tag++;
    }
    if (q == "prelude::Option" && decl.typeParams.size() == 1 && info.variants.size() == 2 &&
        info.variants[0].name == "Some" && info.variants[1].name == "None") {
      info.builtinKind = 1;
    } else if (q == "prelude::Result" && decl.typeParams.size() == 2 &&
               info.variants.size() == 2 && info.variants[0].name == "Ok" &&
               info.variants[1].name == "Err") {
      info.builtinKind = 2;
    }
    return;
  }

  std::int64_t next = 0;
  for (EnumVariant& variant : decl.variants) {
    if (info.values.count(variant.name)) {
      sink_.error(variant.span, "Duplicate variant '" + variant.name + "' in enum '" +
                                    decl.name + "'.");
      continue;
    }
    if (variant.value) {
      std::int64_t value = 0;
      bool ok = false;
      if (auto* literal = dynamic_cast<IntLitExpr*>(variant.value.get())) {
        value = literal->value;
        ok = true;
      } else if (auto* unary = dynamic_cast<UnaryExpr*>(variant.value.get());
                 unary && unary->op == "-") {
        if (auto* literal = dynamic_cast<IntLitExpr*>(unary->operand.get())) {
          value = -literal->value;
          ok = true;
        }
      }
      if (!ok) {
        sink_.error(variant.value->span,
                    "Enum variant values must be integer literals.",
                    "Write " + variant.name + " = 5.");
        continue;
      }
      info.values[variant.name] = value;
      next = value + 1;
    } else {
      info.values[variant.name] = next;
      ++next;
    }
  }
}

void Sema::resolveEnumPayloads(EnumDecl& decl) {
  const std::string q = qualified(decl.name);
  auto infoIt = enums_.find(q);
  if (infoIt == enums_.end() || !infoIt->second.algebraic) return;
  EnumInfo& info = infoIt->second;

  auto savedGenericParams = std::move(genericParams_);
  genericParams_.clear();
  if (info.typeParamVars.empty()) {
    for (const auto& name : decl.typeParams) {
      TypePtr var = freshVar(Constraint::None, decl.nameSpan,
                             "type parameter '" + name + "' of '" + decl.name + "'");
      var->allowUnresolved = true;
      genericParams_[name] = var;
      info.typeParamVars.push_back(var);
    }
  } else {
    for (std::size_t i = 0; i < decl.typeParams.size() && i < info.typeParamVars.size(); ++i) {
      genericParams_[decl.typeParams[i]] = info.typeParamVars[i];
    }
  }

  for (EnumVariant& variant : decl.variants) {
    VariantInfo* target = nullptr;
    for (VariantInfo& vi : info.variants) {
      if (vi.name == variant.name) {
        target = &vi;
        break;
      }
    }
    if (!target) continue;  // duplicate variant, already reported
    for (std::size_t f = 0; f < variant.fields.size() && f < target->fields.size(); ++f) {
      if (target->fields[f].second) continue;  // already resolved (duplicate enum decl)
      TypePtr type = variant.fields[f].type ? resolveTypeExpr(*variant.fields[f].type) : freshVar(Constraint::None, variant.fields[f].span, "the type of payload field '" + variant.fields[f].name + "'");
      if (!variant.fields[f].type) resolveType(type)->allowUnresolved = true;
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(variant.fields[f].span, "A payload field cannot have type void.");
        type = typeError();
      }
      target->fields[f].second = type;
    }
  }
  genericParams_ = std::move(savedGenericParams);
}

void Sema::registerFunctionSignature(FnDecl& decl) {
  auto saved = std::move(genericParams_);
  FnSig sig;
  sig.typeParams = decl.typeParams;
  for (const auto& name : decl.typeParams) {
    TypePtr var = freshVar(Constraint::None, decl.nameSpan,
                           "type parameter '" + name + "' of '" + decl.name + "'");
    var->allowUnresolved = true;
    genericParams_[name] = var;
    sig.typeParamVars.push_back(var);
  }
  makeSignatureTypes(decl, nullptr, sig);
  genericParams_ = std::move(saved);
  signatures_[&decl] = std::move(sig);
}

void Sema::registerMethodSignatures(StructDecl& decl) {
  const std::string q = qualified(decl.name);
  StructSym& sym = structs_[q];
  TypePtr selfType = typeStruct(q);

  // Inherit methods from baseClass if present
  std::vector<std::string> inherited;
  if (!decl.baseClass.empty()) {
    std::string baseQ = qualified(decl.baseClass);
    auto baseIt = structs_.find(baseQ);
    if (baseIt == structs_.end()) {
      baseIt = structs_.find(decl.baseClass);
    }
    if (baseIt != structs_.end()) {
      for (const auto& m : baseIt->second.methods) {
        sym.methods.insert(m);
        inherited.push_back(m.first);
      }
    }
  }
  std::vector<std::string> ownMethods;
  for (auto& methodPtr : decl.methods) {
    FnDecl& method = *methodPtr;
    fnModules_[&method] = currentModule_;
    method.moduleId = currentModule_;
    if (method.params.empty() || method.params.front().name != "self") {
      sink_.error(method.nameSpan,
                  "Method '" + method.name + "' must declare 'self' as its first parameter.",
                  "Write: fn " + method.name + "(self, ...):");
      continue;
    }
    if (method.params.front().type) {
      sink_.error(method.params.front().span,
                  "The 'self' parameter does not take a type annotation.");
    }
    if (sym.methods.count(method.name)) {
      bool overridesInherited =
          std::find(inherited.begin(), inherited.end(), method.name) != inherited.end();
      bool duplicateOwn =
          std::find(ownMethods.begin(), ownMethods.end(), method.name) != ownMethods.end();
      if (duplicateOwn || !overridesInherited) {
        sink_.error(method.nameSpan, "Duplicate method '" + method.name + "' in struct '" + decl.name + "'.");
        continue;
      }
    }
    ownMethods.push_back(method.name);
    for (const auto& field : sym.fields) {
      if (field.first == method.name) {
        sink_.error(method.nameSpan, "Method '" + method.name + "' conflicts with a field of the same name.");
      }
    }
    FnSig sig;
    sig.typeParams = method.typeParams;
    // Inherit generic parameters from the owning struct.
    for (std::size_t i = 0; i < sym.typeParams.size() && i < sym.typeParamVars.size(); ++i) {
      sig.typeParams.push_back(sym.typeParams[i]);
      sig.typeParamVars.push_back(sym.typeParamVars[i]);
    }
    auto savedGeneric = std::move(genericParams_);
    genericParams_.clear();
    for (std::size_t i = 0; i < sym.typeParams.size() && i < sym.typeParamVars.size(); ++i) {
      genericParams_[sym.typeParams[i]] = sym.typeParamVars[i];
    }
    for (const auto& name : method.typeParams) {
      TypePtr var = freshVar(Constraint::None, method.nameSpan,
                             "type parameter '" + name + "' of '" + method.name + "'");
      var->allowUnresolved = true;
      genericParams_[name] = var;
      sig.typeParamVars.push_back(var);
    }
    makeSignatureTypes(method, selfType, sig);
    genericParams_ = std::move(savedGeneric);
    signatures_[&method] = sig;
    sym.methods[method.name] = std::move(sig);
  }
}

TypePtr Sema::makeSignatureTypes(FnDecl& decl, TypePtr selfType, FnSig& sig) {
  sig.name = decl.name;
  sig.decl = &decl;

  std::size_t index = 0;
  if (selfType) {
    sig.paramTypes.push_back(selfType);
    sig.paramHasDefault.push_back(0);
    index = 1;
  }

  bool seenDefault = false;
  for (std::size_t i = index; i < decl.params.size(); ++i) {
    ParamDecl& param = decl.params[i];
    if (param.variadic) {
      sig.variadic = true;
      TypePtr elem;
      if (param.type) {
        elem = resolveTypeExpr(*param.type);
        if (resolveType(elem)->kind == TypeKind::Void) {
          sink_.error(param.span, "A variadic parameter cannot have element type void.");
          elem = typeError();
        }
      } else {
        elem = freshVar(Constraint::None, param.span,
                        "the element type of parameter '" + param.name + "' of '" + decl.name + "'");
        resolveType(elem)->allowUnresolved = true;
      }
      sig.paramTypes.push_back(typeArray(elem, -1));
      sig.paramHasDefault.push_back(0);
      continue;
    }
    if (param.defaultValue) {
      seenDefault = true;
    } else if (seenDefault) {
      sink_.error(param.span,
                  "Parameter '" + param.name + "' has no default value but follows a parameter that does.",
                  "Give every parameter after the first default a default value too.");
    }
    sig.paramHasDefault.push_back(param.defaultValue ? 1 : 0);
    TypePtr type;
    if (param.type) {
      type = resolveTypeExpr(*param.type);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(param.span, "Parameter '" + param.name + "' cannot have type void.");
        type = typeError();
      }
    } else {
      type = freshVar(Constraint::None, param.span,
                      "the type of parameter '" + param.name + "' of '" + decl.name + "'");
    }
    sig.paramTypes.push_back(type);
  }

  if (decl.returnType) {
    sig.retType = resolveTypeExpr(*decl.returnType);
  } else {
    sig.retType = freshVar(Constraint::None, decl.nameSpan,
                           "the return type of '" + decl.name + "'");
  }
  return sig.retType;
}

void Sema::checkFunctionBody(FnDecl& decl, const FnSig& sig) {
  auto savedScopes = std::move(scopes_);
  auto savedGenericParams = std::move(genericParams_);
  scopes_.clear();
  if (!savedScopes.empty()) {
    scopes_.push_back(savedScopes.front());
  } else {
    scopes_.push_back({});
  }
  genericParams_.clear();
  for (std::size_t i = 0; i < decl.typeParams.size() && i < sig.typeParamVars.size(); ++i) {
    genericParams_[decl.typeParams[i]] = sig.typeParamVars[i];
  }

  FnDecl* savedFunction = currentFunction_;
  TypePtr savedReturn = currentReturnType_;
  bool savedSawReturn = sawReturn_;
  int savedLoopDepth = loopDepth_;
  int savedDeferDepth = deferDepth_;

  currentFunction_ = &decl;
  currentReturnType_ = sig.retType;
  sawReturn_ = false;
  loopDepth_ = 0;
  deferDepth_ = 0;

  pushScope();
  for (std::size_t i = 0; i < decl.params.size() && i < sig.paramTypes.size(); ++i) {
    declareVar(decl.params[i].name, decl.params[i].span, sig.paramTypes[i], false, "parameter");
    // Annotated parameters keep their declared type on reassignment; params
    // without an annotation stay dynamically rebindable.
    if (decl.params[i].type) {
      if (VarSym* sym = lookupVar(decl.params[i].name)) {
        sym->declaredType = sig.paramTypes[i];
      }
    }
  }
  // Default values are checked with earlier parameters in scope, so a
  // default may reference a preceding parameter (e.g. `fn f(a, b = a + 1)`).
  for (std::size_t i = 0; i < decl.params.size() && i < sig.paramTypes.size(); ++i) {
    ParamDecl& param = decl.params[i];
    if (!param.defaultValue) continue;
    checkArgAgainst(*param.defaultValue, sig.paramTypes[i],
                    "in the default value of parameter '" + param.name + "' of '" + decl.name + "'");
  }
  // `comptime N: int` parameters are bound as typed values in the body. They are
  // not passed at call sites yet; specialization is tracked in TODO.md.
  for (const ComptimeParam& param : decl.comptimeParams) {
    TypePtr type = param.type ? resolveTypeExpr(*param.type) : typeInt(64, true);
    declareVar(param.name, param.span, type, false, "compile-time parameter");
    if (VarSym* sym = lookupVar(param.name)) {
      if (param.type) sym->declaredType = type;
    }
  }
  checkStatements(decl.body);
  reportUnusedLocals(scopes_.back());

  if (!sawReturn_) {
    // Dynamic: no return required; default to void if still unresolved.
    TypePtr ret = resolveType(sig.retType);
    if (ret && ret->kind == TypeKind::Var) unify(sig.retType, typeVoid());
  }

  checkedFunctions_.push_back({&decl, sig.retType});
  fnParamTypes_[&decl] = sig.paramTypes;
  fnReturnTypes_[&decl] = sig.retType;

  currentFunction_ = savedFunction;
  currentReturnType_ = savedReturn;
  sawReturn_ = savedSawReturn;
  loopDepth_ = savedLoopDepth;
  deferDepth_ = savedDeferDepth;

  scopes_ = std::move(savedScopes);
  genericParams_ = std::move(savedGenericParams);
}

void Sema::checkStructMethodBodies(StructDecl& decl) {
  for (auto& methodPtr : decl.methods) {
    auto found = signatures_.find(methodPtr.get());
    if (found == signatures_.end()) continue;
    checkFunctionBody(*methodPtr, found->second);
  }
}

// ---------------------------------------------------------------- traits

namespace {

std::string_view shortDeclName(const std::string& qualifiedName) {
  const std::size_t sep = qualifiedName.rfind("::");
  return sep == std::string::npos ? std::string_view(qualifiedName)
                                  : std::string_view(qualifiedName).substr(sep + 2);
}

}  // namespace

const TraitDecl* Sema::findTrait(const std::string& name) {
  auto found = traits_.find(qualified(name));
  if (found != traits_.end()) return found->second;

  // Fall back to a unique short-name match so imported traits resolve.
  const std::string_view query = shortDeclName(name);
  const TraitDecl* match = nullptr;
  for (const auto& entry : traits_) {
    if (shortDeclName(entry.first) != query) continue;
    if (match && match != entry.second) return nullptr;  // ambiguous
    match = entry.second;
  }
  return match;
}

const StructDecl* Sema::findStructDecl(const std::string& name) {
  auto found = structsByName_.find(qualified(name));
  if (found != structsByName_.end()) return found->second;

  const std::string_view query = shortDeclName(name);
  const StructDecl* match = nullptr;
  for (const auto& entry : structsByName_) {
    if (shortDeclName(entry.first) != query) continue;
    if (match && match != entry.second) return nullptr;
    match = entry.second;
  }
  return match;
}

void Sema::checkWhereClause(const std::vector<std::string>& traits, Span span,
                            const std::string& what,
                            const std::vector<Span>* traitSpans) {
  for (std::size_t i = 0; i < traits.size(); ++i) {
    Span at = span;
    if (traitSpans && i < traitSpans->size() && (*traitSpans)[i].valid()) {
      at = (*traitSpans)[i];
    }
    if (findTrait(traits[i]) == nullptr) {
      sink_.error(at.valid() ? at : Span(),
                  "Unknown trait '" + traits[i] + "' in the 'where' clause of " + what + ".",
                  "Declare it with 'trait " + traits[i] + ":' before it is used as a constraint.");
    }
  }
}

void Sema::checkTraitDecl(TraitDecl& decl) {
  checkWhereClause(decl.parents, decl.nameSpan,
                   "trait '" + decl.name + "'");
  checkWhereClause(decl.whereClause, decl.nameSpan, "trait '" + decl.name + "'",
                   &decl.whereSpans);

  StringMap<Span> seen;
  for (const TraitSig& sig : decl.methods) {
    if (seen.count(sig.name)) {
      sink_.error(sig.span, "Duplicate method '" + sig.name + "' in trait '" + decl.name + "'.");
      continue;
    }
    seen[sig.name] = sig.span;
  }
}

bool Sema::structSatisfiesTrait(const StructDecl& type, const TraitDecl& trait) {
  for (const TraitSig& sig : trait.methods) {
    bool found = false;
    for (const auto& method : type.methods) {
      if (!method || method->name != sig.name) continue;
      // Trait signatures and struct methods both spell `self` explicitly, so the
      // arities can be compared directly.
      if (method->params.size() != sig.params.size()) continue;
      found = true;
      break;
    }
    if (!found) return false;
  }
  for (const std::string& parentName : trait.parents) {
    const TraitDecl* parent = findTrait(parentName);
    if (!parent) continue;  // reported by checkTraitDecl
    if (!structSatisfiesTrait(type, *parent)) return false;
  }
  return true;
}

void Sema::checkImpl(ImplDecl& decl) {
  const TraitDecl* trait = findTrait(decl.traitName);
  if (!trait) {
    sink_.error(decl.traitSpan, "Unknown trait '" + decl.traitName + "' in 'impl'.",
                "Declare it with 'trait " + decl.traitName + ":' first.");
    return;
  }
  const StructDecl* target = findStructDecl(decl.targetName);
  if (!target) {
    sink_.error(decl.targetSpan,
                "Unknown type '" + decl.targetName + "' in 'impl " + decl.traitName + " for'.",
                "A struct must be declared before it conforms to a trait.");
    return;
  }

  const std::string traitQ = qualified(trait->name);
  const std::string targetQ = qualified(target->name);
  auto& conformed = traitImpls_[targetQ];
  if (std::find(conformed.begin(), conformed.end(), traitQ) != conformed.end()) {
    sink_.error(decl.span, "Duplicate conformance: '" + target->name +
                               "' already implements '" + trait->name + "'.");
    return;
  }

  for (const TraitSig& sig : trait->methods) {
    bool found = false;
    for (const auto& method : target->methods) {
      if (!method || method->name != sig.name) continue;
      if (method->params.size() != sig.params.size()) continue;
      found = true;
      break;
    }
    if (!found) {
      std::string expected = "fn " + sig.name + "(";
      for (std::size_t i = 0; i < sig.params.size(); ++i) {
        if (i) expected += ", ";
        expected += sig.params[i].name;
      }
      expected += ")";
      sink_.error(decl.span,
                  "Struct '" + target->name + "' does not satisfy trait '" + trait->name +
                      "': missing '" + expected + "' (" + std::to_string(sig.params.size()) +
                      " parameter(s)).",
                  "Add the method to the struct, or remove the 'impl' declaration.");
      return;
    }
  }

  conformed.push_back(traitQ);
}

}  // namespace a
