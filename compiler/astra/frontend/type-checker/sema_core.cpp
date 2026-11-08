#include "type-checker/sema_internal.hpp"

namespace a {
Sema::Sema(ModuleGraph& graph, DiagnosticSink& sink) : graph_(graph), sink_(sink) {
  moduleExports_.resize(graph_.modules.size());
}

std::string Sema::qualified(const std::string& name) const {
  return qualifiedFor(currentModule_, name);
}

std::string Sema::qualifiedFor(int moduleId, const std::string& name) const {
  return graph_.at(moduleId).name + "::" + name;
}

ModuleExports& Sema::exportsFor(int moduleId) {
  return moduleExports_[static_cast<std::size_t>(moduleId)];
}

const FnDecl* Sema::findPreludeFunction(const std::string& name) const {
  if (graph_.preludeId < 0) return nullptr;
  auto it = functionsByName_.find(qualifiedFor(graph_.preludeId, name));
  return it == functionsByName_.end() ? nullptr : it->second;
}

Sema::ModuleAlias* Sema::findAlias(const std::string& name) {
  auto found = moduleAliases_.find(name);
  return found == moduleAliases_.end() ? nullptr : &found->second;
}

namespace {
int editDistance(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  const std::size_t n = a.size(), m = b.size();
  if (!n) return (int)m;
  if (!m) return (int)n;
  std::vector<int> prev(m + 1), cur(m + 1);
  for (std::size_t j = 0; j <= m; ++j) prev[j] = (int)j;
  for (std::size_t i = 1; i <= n; ++i) {
    cur[0] = (int)i;
    for (std::size_t j = 1; j <= m; ++j) {
      int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[m];
}
}  // namespace

std::string Sema::suggestName(const std::string& name) const {
  if (name.empty()) return "";
  std::vector<std::string> candidates;
  auto add = [&candidates](const std::string& c) {
    if (!c.empty()) candidates.push_back(c);
  };
  auto shortName = [](const std::string& q) {
    auto p = q.rfind("::");
    return p == std::string::npos ? q : q.substr(p + 2);
  };
  for (const auto& scope : scopes_)
    for (const auto& kv : scope) add(kv.first);
  for (const auto& kv : functionsByName_) add(shortName(kv.first));
  for (const auto& kv : structsByName_) add(shortName(kv.first));
  for (const auto& kv : structs_) add(shortName(kv.first));
  for (const auto& kv : enums_) add(shortName(kv.first));
  for (const auto& kv : traits_) add(shortName(kv.first));
  for (const auto& kv : fromBindings_) add(kv.first);
  for (const auto& kv : moduleAliases_) add(kv.first);
  for (const auto& kv : typeAliases_) add(kv.first);
  // Builtin/prelude names worth suggesting (typos like 'pritn' -> 'print').
  static const char* kBuiltins[] = {
      "print", "eprint", "len", "str", "int", "float", "bool", "abs", "min",
      "max", "sum", "pow", "sqrt", "sin", "cos", "tan", "asin", "acos",
      "atan", "exp", "log", "floor", "ceil", "round", "trunc", "random",
      "random_int", "range", "set", "tuple", "list", "dict", "map", "filter",
      "any", "all", "some", "ok", "err", "panic", "assert", "assert_eq",
      "input", "read_file", "write_file", "append_file", "file_exists",
      "file_size", "dot", "matvec", "matvec_t", "linalg_dot", "linalg_matvec", "linalg_matvec_t",
      "matmul", "softmax", "attention", "linalg_matmul", "linalg_softmax", "linalg_attention", "list_dir", "remove_file", "create_dir", "remove_dir",
      "rename",
      "current_dir", "repr", "type", "isinstance", "join", "sorted",
      "reversed", "enumerate", "zip", "reduce", "flush", "env_get", "now",
      "sleep", "Some", "Ok", "Err", "None", "True", "False",
  };
  for (const char* b : kBuiltins) add(b);

  int best = -1;
  std::string bestName;
  for (const std::string& c : candidates) {
    if (c == name) continue;
    int d = editDistance(name, c);
    if (d <= 0) continue;
    // Similarity gate ~ Python difflib's 0.6 cutoff: distance <= 40% of len.
    if (best < 0 || d < best ||
        (d == best && c.size() < bestName.size())) {
      if (d * 5 <= (int)name.size() * 2) {
        best = d;
        bestName = c;
      }
    }
  }
  return bestName;
}

TypePtr Sema::moduleMemberType(const ModuleAlias& alias, const std::string& member, Span span,
                               bool report) {
  if (alias.python) {
    (void)report; (void)span;
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (alias.native) {
    if (const std::string* fn = nativeFunction(alias.nativeName, member)) {
      (void)fn;
      if (report) {
        sink_.error(span,
                    "Native function '" + alias.nativeName + "." + member +
                        "' can only be called directly.",
                    "Wrap it in a lambda to pass it as a value, e.g. x => " + alias.nativeName +
                        "." + member + "(x).");
      }
      return typeError();
    }
    if (const std::string* constant = nativeConstant(alias.nativeName, member)) {
      if (TypePtr type = builtinConstantType(*constant)) return type;
    }
    if (report) {
      sink_.error(span, "Module '" + alias.nativeName + "' has no member '" + member + "'.");
    }
    return typeError();
  }

  ModuleExports& exports = exportsFor(alias.moduleId);
  if (auto fn = exports.functions.find(member); fn != exports.functions.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig != signatures_.end()) {
      return typeFunction(sig->second.paramTypes, sig->second.retType);
    }
    return typeError();
  }
  if (exports.structs.count(member)) {
    if (report) {
      sink_.error(span, "'" + exports.name + "." + member + "' is a struct type, not a value.",
                  "Construct it: " + exports.name + "." + member + "(...).");
    }
    return typeError();
  }
  if (auto en = exports.enums.find(member); en != exports.enums.end()) {
    return typeEnum(en->second);
  }
  if (auto value = exports.values.find(member); value != exports.values.end()) {
    return value->second;
  }
  if (report) {
    sink_.error(span, "Module '" + exports.name + "' has no member '" + member + "'.");
  }
  return typeError();
}

TypePtr Sema::moduleMemberCall(const ModuleAlias& alias, const std::string& member,
                               CallExpr& call) {
  (void)member;
  if (alias.python) {
    for (auto& arg : call.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (alias.native) {
    if (const std::string* fn = nativeFunction(alias.nativeName, member)) {
      return checkBuiltinCall(*fn, call);
    }
    if (nativeConstant(alias.nativeName, member)) {
      sink_.error(call.callee->span,
                  "'" + alias.nativeName + "." + member + "' is a constant, not a function.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    sink_.error(call.callee->span,
                "Module '" + alias.nativeName + "' has no function '" + member + "'.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  ModuleExports& exports = exportsFor(alias.moduleId);
  if (auto fn = exports.functions.find(member); fn != exports.functions.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig == signatures_.end()) {
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    return checkFunctionCall(sig->second, call);
  }
  if (exports.structs.count(member)) {
    auto sym = structs_.find(qualifiedFor(alias.moduleId, member));
    if (sym == structs_.end()) return typeError();
    return checkStructConstruction(sym->second, call);
  }
  if (exports.values.count(member)) {
    sink_.error(call.callee->span,
                "'" + exports.name + "." + member + "' is a value, not a function.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  sink_.error(call.callee->span,
              "Module '" + exports.name + "' has no function '" + member + "'.");
  for (auto& arg : call.args) inferExpr(*arg.value);
  return typeError();
}

// ------------------------------------------------------------------ types

TypePtr Sema::freshVar(Constraint constraint, Span span, std::string description,
                       bool allowDefault) {
  auto type = makeType(TypeKind::Var);
  type->varId = nextVarId_++;
  type->constraint = constraint;
  type->allowDefault = allowDefault;
  type->varDescription = std::move(description);
  type->varSpan = span;
  typeVariables_.push_back(VarInfo{type, span, type->varDescription});
  return type;
}

bool Sema::occursIn(TypePtr var, TypePtr type) {
  type = resolveType(type);
  if (!type) return false;
  if (type == var) return true;
  switch (type->kind) {
    case TypeKind::Array:
    case TypeKind::Range:
    case TypeKind::Optional:
    case TypeKind::Result:
      return occursIn(var, type->elem) || occursIn(var, type->errorType);
    case TypeKind::Map:
    case TypeKind::Set:
      if (occursIn(var, type->elem)) return true;
      for (const auto& p : type->params) {
        if (occursIn(var, p)) return true;
      }
      return false;
    case TypeKind::Tuple:
    case TypeKind::Function: {
      for (const auto& p : type->params) {
        if (occursIn(var, p)) return true;
      }
      return type->kind == TypeKind::Function ? occursIn(var, type->ret) : false;
    }
    default:
      return false;
  }
}

// Shared Type-var objects are mutated by unify/bindVar from parallel module
// workers. Serialise those mutations; diagnostics are already locked.
// Recursive because unify() calls bindVar() and vice versa.
bool Sema::bindVar(TypePtr var, TypePtr type) {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  TypePtr resolved = resolveType(type);
  if (!resolved) return true;
  if (resolved->kind == TypeKind::Var) return unify(var, resolved);
  if (resolved->kind == TypeKind::Error) return true;
  if (resolved->kind == TypeKind::Void) return false;

  switch (var->constraint) {
    case Constraint::Integer:
      if (isFloatType(resolved)) { var->link = resolved; return true; }
      if (!isIntegerType(resolved)) return false;
      break;
    case Constraint::Floating:
      if (isIntegerType(resolved)) { var->link = typeFloat(64); return true; }
      if (!isFloatType(resolved)) return false;
      break;
    case Constraint::Numeric:
      if (!isNumericType(resolved)) return false;
      break;
    case Constraint::None:
      break;
  }

  if (occursIn(var, resolved)) return false;
  var->link = resolved;
  return true;
}

bool Sema::unify(TypePtr a, TypePtr b) {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  a = resolveType(a);
  b = resolveType(b);
  if (!a || !b) return true;
  if (a->kind == TypeKind::Error || b->kind == TypeKind::Error) return true;
  if (a == b) return true;

  if (a->kind == TypeKind::Var && b->kind == TypeKind::Var) {
    if (a->varId == b->varId) return true;
    Constraint merged = a->constraint;
    bool conflict = false;
    auto merge = [&](Constraint x, Constraint y) {
      if (x == Constraint::None) return y;
      if (y == Constraint::None) return x;
      if (x == y) return x;
      if (x == Constraint::Numeric) return y;
      if (y == Constraint::Numeric) return x;
      return Constraint::Numeric;
    };
    merged = merge(a->constraint, b->constraint);
    if (conflict) return false;
    b->constraint = merged;
    b->allowDefault = a->allowDefault || b->allowDefault;
    b->allowUnresolved = a->allowUnresolved || b->allowUnresolved;
    a->link = b;
    return true;
  }

  if (a->kind == TypeKind::Var) return bindVar(a, b);
  if (b->kind == TypeKind::Var) return bindVar(b, a);
  if (isNumericType(a) && isNumericType(b)) return true;
  if (a->kind != b->kind) return false;

  switch (a->kind) {
    case TypeKind::Int:
      return true;
    case TypeKind::Float:
      return true;
    case TypeKind::Struct:
      if (a->structName != b->structName) return false;
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return true;
    case TypeKind::Enum:
      if (a->structName != b->structName) return false;
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return true;
    case TypeKind::Array:
      if (a->length >= 0 && b->length >= 0 && a->length != b->length) return false;
      return unify(a->elem, b->elem);
    case TypeKind::Range:
    case TypeKind::Optional:
      return unify(a->elem, b->elem);
    case TypeKind::Set:
      return unify(a->elem, b->elem);
    case TypeKind::Map:
      return unify(a->elem, b->elem) && !a->params.empty() && !b->params.empty() &&
             unify(a->params[0], b->params[0]);
    case TypeKind::Result:
      return unify(a->elem, b->elem) && unify(a->errorType, b->errorType);
    case TypeKind::Tuple: {
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return true;
    }
    case TypeKind::Function: {
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return unify(a->ret, b->ret);
    }
    default:
      return true;
  }
}

bool Sema::constrainVar(TypePtr var, Constraint constraint) {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  TypePtr resolved = resolveType(var);
  if (!resolved) return false;
  if (resolved->kind == TypeKind::Var) {
    if (resolved->constraint == Constraint::None) {
      resolved->constraint = constraint;
      return true;
    }
    if (resolved->constraint == constraint) return true;
    if (resolved->constraint == Constraint::Numeric) {
      resolved->constraint = constraint;
      return true;
    }
    if (constraint == Constraint::Numeric) return true;
    resolved->constraint = Constraint::Numeric;
    return true;
  }
  switch (constraint) {
    case Constraint::Integer: return isNumericType(resolved);
    case Constraint::Floating: return isNumericType(resolved);
    case Constraint::Numeric: return isNumericType(resolved);
    case Constraint::None: return true;
  }
  return false;
}

TypePtr Sema::resolveTypeExpr(TypeExpr& expr) {
  if (auto* array = dynamic_cast<ArrayTypeExpr*>(&expr)) {
    return typeArray(resolveTypeExpr(*array->element), array->length);
  }
  if (auto* optional = dynamic_cast<OptionalTypeExpr*>(&expr)) {
    return typeOptional(resolveTypeExpr(*optional->element));
  }
  if (auto* tuple = dynamic_cast<TupleTypeExpr*>(&expr)) {
    std::vector<TypePtr> elements;
    elements.reserve(tuple->elements.size());
    for (auto& element : tuple->elements) {
      elements.push_back(resolveTypeExpr(*element));
    }
    return typeTuple(std::move(elements));
  }
  if (auto* map = dynamic_cast<MapTypeExpr*>(&expr)) {
    return typeMap(resolveTypeExpr(*map->key), resolveTypeExpr(*map->value));
  }
  if (auto* set = dynamic_cast<SetTypeExpr*>(&expr)) {
    return typeSet(resolveTypeExpr(*set->element));
  }
  if (auto* fn = dynamic_cast<FunctionTypeExpr*>(&expr)) {
    std::vector<TypePtr> params;
    params.reserve(fn->params.size());
    for (auto& param : fn->params) params.push_back(resolveTypeExpr(*param));
    TypePtr ret = fn->returnType ? resolveTypeExpr(*fn->returnType) : typeVoid();
    return typeFunction(std::move(params), std::move(ret));
  }
  auto* named = dynamic_cast<NamedTypeExpr*>(&expr);
  if (!named) return typeError();
  if (named->qualifiers.empty()) {
    if (genericParams_.count(named->name)) {
      return genericParams_[named->name];
    }
    if (TypePtr builtin = builtinTypeByName(named->name)) return builtin;
    const std::string q = qualified(named->name);
    if (structs_.count(q)) {
      TypePtr t = typeStruct(q);
      if (!named->typeArgs.empty()) {
        std::vector<TypePtr> args;
        for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
        t->params = std::move(args);
      }
      return t;
    }
    if (auto eit = enums_.find(q); eit != enums_.end()) return enumTypeFromInfo(eit->second, *named);
    if (auto alias = structAliases_.find(named->name); alias != structAliases_.end()) {
      TypePtr t = typeStruct(alias->second);
      if (!named->typeArgs.empty()) {
        std::vector<TypePtr> args;
        for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
        t->params = std::move(args);
      }
      return t;
    }
    if (typeAliases_.count(q)) return typeAliases_.at(q);
    // Built-in names are recognized last so a user-defined Result or Option
    // (struct, enum, alias, or type alias) shadows the prelude behavior.
    if (named->name == "Result" && !named->typeArgs.empty()) {
      if (named->typeArgs.size() != 2) {
        sink_.error(named->span, "'Result' requires two type arguments: Result<T, E>.");
        return typeError();
      }
      return typeResult(resolveTypeExpr(*named->typeArgs[0]),
                        resolveTypeExpr(*named->typeArgs[1]));
    }
    if (named->name == "Option" && !named->typeArgs.empty()) {
      if (named->typeArgs.size() != 1) {
        sink_.error(named->span, "'Option' takes one type argument: Option<T>.");
        return typeError();
      }
      return typeOptional(resolveTypeExpr(*named->typeArgs[0]));
    }
    std::string unknownType = "Unknown type '" + named->name + "'.";
    if (std::string sug = suggestName(named->name); !sug.empty())
      unknownType += " Did you mean '" + sug + "'?";
    sink_.error(named->span, unknownType,
                "Type names are 'bool', 'char', 'String', 'void', the sized numeric types, "
                "class names, '[T]' for arrays, '(T, U)' for tuples, 'fn(T) -> R' for "
                "functions, or 'T?' for optionals.");
    return typeError();
  }

  const std::string qualifier = named->qualifiers.back();
  ModuleAlias* alias = findAlias(qualifier);
  if (!alias) {
    sink_.error(named->span, "Unknown module alias '" + qualifier + "'.");
    return typeError();
  }
  if (alias->native) {
    sink_.error(named->span, "Native modules do not declare types.");
    return typeError();
  }
  const std::string q = qualifiedFor(alias->moduleId, named->name);
  if (structs_.count(q)) {
    TypePtr t = typeStruct(q);
    if (!named->typeArgs.empty()) {
      std::vector<TypePtr> args;
      for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
      t->params = std::move(args);
    }
    return t;
  }
  if (auto eit = enums_.find(q); eit != enums_.end()) return enumTypeFromInfo(eit->second, *named);
  sink_.error(named->span, "Module '" + graph_.at(alias->moduleId).name + "' has no type named '" +
                               named->name + "'.");
  return typeError();
}

TypePtr Sema::instantiateType(TypePtr type, const std::unordered_map<TypePtr, TypePtr>& mapping) {
  type = resolveType(type);
  if (!type) return type;
  if (type->kind == TypeKind::Var) {
    auto it = mapping.find(type);
    if (it != mapping.end()) return it->second;
    // create a fresh var preserving constraints
    TypePtr fresh = makeType(TypeKind::Var);
    fresh->varId = nextVarId_++;
    fresh->constraint = type->constraint;
    fresh->allowDefault = type->allowDefault;
    fresh->allowUnresolved = type->allowUnresolved;
    fresh->varDescription = type->varDescription;
    fresh->varSpan = type->varSpan;
    return fresh;
  }
  TypePtr result = makeType(type->kind);
  result->bits = type->bits;
  result->isSigned = type->isSigned;
  result->structName = type->structName;
  result->moduleId = type->moduleId;
  result->length = type->length;
  if (type->elem) result->elem = instantiateType(type->elem, mapping);
  if (type->errorType) result->errorType = instantiateType(type->errorType, mapping);
  if (type->ret) result->ret = instantiateType(type->ret, mapping);
  for (const auto& p : type->params) result->params.push_back(instantiateType(p, mapping));
  return result;
}

void Sema::pushScope() { scopes_.push_back({}); }

void Sema::popScope() {
  if (scopes_.size() > 1) {
    reportUnusedLocals(scopes_.back());
    scopes_.pop_back();
  }
}

void Sema::reportUnusedLocals(const StringMap<VarSym>& scope) {
  for (const auto& entry : scope) {
    const VarSym& sym = entry.second;
    if (!sym.warnUnused || sym.used) continue;
    sink_.warning(sym.span, "W0001", "Unused variable '" + sym.name + "'.",
                  "Remove it or use it; prefix the name with '_' to silence this warning.");
  }
}

Sema::VarSym* Sema::lookupVar(const std::string& name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) return &found->second;
  }
  return nullptr;
}

void Sema::declareVar(const std::string& name, Span span, TypePtr type, bool mutableBinding,
                      const char* what, bool constant) {
  (void)mutableBinding;  // Dynamic: bindings are always mutable.
  if (scopes_.empty()) return;
  auto& scope = scopes_.back();
  if (scope.count(name)) {
    // Python-style: redeclaring a name in the same scope is rebinding.
    scope[name].mutableBinding = true;
    scope[name].constant = false;
    return;
  }
  if (scopes_.size() == 1) {
    if (functionsByName_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as a function.");
      return;
    }
    if (structsByName_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as a struct.");
      return;
    }
    if (enums_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as an enum.");
      return;
    }
  }
  VarSym sym;
  sym.type = std::move(type);
  sym.mutableBinding = true;
  sym.constant = false;
  sym.name = name;
  sym.span = span;
  // Loop variables and parameters are frequently intentional even when
  // unread, so only plain local declarations produce W0001.
  sym.warnUnused = !constant && std::string(what) == "variable" && scopes_.size() > 1 &&
                   !name.empty() && name[0] != '_';
  scope[name] = std::move(sym);
  DeclaredVarInfo info;
  info.name = name;
  info.span = span;
  info.type = scope[name].type;
  declaredVariables_.push_back(std::move(info));
}

}  // namespace a
