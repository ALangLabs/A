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
      "sleep", "Some", "Ok", "Err", "None", "True", "False", "Bytes",
      "divmod",
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

TypePtr Sema::resolveReexportedValue(int moduleId, const std::string& name, int depth) {
  if (depth > 16 || moduleId < 0 || moduleId >= (int)graph_.modules.size()) return nullptr;
  ModuleExports& ex = exportsFor(moduleId);
  auto it = ex.values.find(name);
  if (it != ex.values.end() && it->second && it->second->kind != TypeKind::Var)
    return it->second;
  const Module& mod = graph_.at(moduleId);
  if (mod.program) {
    for (auto& s : mod.program->statements) {
      auto* im = dynamic_cast<FromImportStmt*>(s.get());
      if (!im || im->nativeModule || im->pythonModuleImport || im->moduleId < 0) continue;
      for (const auto& item : im->items) {
        if (item.name != name && item.alias != name) continue;
        if (TypePtr t = resolveReexportedValue(im->moduleId, item.name, depth + 1)) return t;
      }
    }
  }
  return it != ex.values.end() ? it->second : nullptr;
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
    if (value->second && value->second->kind == TypeKind::Var) {
      if (TypePtr t = resolveReexportedValue(alias.moduleId, member, 0)) {
        if (t->kind != TypeKind::Var) return t;
      }
    }
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
    case TypeKind::Vector:
      return occursIn(var, type->elem) || occursIn(var, type->errorType);
    case TypeKind::Map:
    case TypeKind::Set:
      if (occursIn(var, type->elem)) return true;
      for (const auto& p : type->params) {
        if (occursIn(var, p)) return true;
      }
      return false;
    case TypeKind::Tuple:
    case TypeKind::Function:
    case TypeKind::Union: {
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

  // A constrained literal var (e.g. the `2` in `f(2)`) satisfies `T?` through
  // its payload type: bind it to the element instead of the Optional itself,
  // which would fail the constraint check below. Unconstrained vars still
  // bind to the whole Optional so `x = maybe` keeps its optional type.
  if (resolved->kind == TypeKind::Optional && var->constraint != Constraint::None) {
    return unify(var, resolved->elem);
  }

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
  // `any` is the dynamic top type: it unifies with everything and never
  // binds (checked before Var handling so `unify(var, any)` leaves `var`
  // free to be constrained by other uses).
  if (a->kind == TypeKind::Any || b->kind == TypeKind::Any) return true;

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
  // `Json` is the dynamic payload type produced by the json/binary modules; a
  // value of any type may flow in and out of it (the runtime validates on use).
  if (a->kind == TypeKind::Json || b->kind == TypeKind::Json) return true;
  // Implicit optional wrapping: a `T` is accepted where a `T?` is expected
  // (and an optional value where its payload type is expected), mirroring the
  // way `return x` satisfies `-> T?` and `f(point)` satisfies `p: Point?`.
  if (a->kind == TypeKind::Optional && b->kind != TypeKind::Optional) return unify(a->elem, b);
  if (b->kind == TypeKind::Optional && a->kind != TypeKind::Optional) return unify(a, b->elem);

  // Union types (`T | U`, `Union[T, U]`). Direction matters: a union-typed
  // *value* is assignable only when every member fits the target, while a
  // union-typed *target* accepts the value when any member does.
  const bool aUnion = a->kind == TypeKind::Union;
  const bool bUnion = b->kind == TypeKind::Union;
  if (aUnion || bUnion) {
    if (aUnion && bUnion) {
      // Every member of `a` must fit some member of `b`.
      for (const auto& ma : a->params) {
        bool fits = false;
        for (const auto& mb : b->params) {
          if (unify(ma, mb)) { fits = true; break; }
        }
        if (!fits) return false;
      }
      return true;
    }
    if (aUnion) {
      for (const auto& ma : a->params) {
        if (!unify(ma, b)) return false;
      }
      return true;
    }
    for (const auto& mb : b->params) {
      if (unify(a, mb)) return true;
    }
    return false;
  }

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
    case TypeKind::Vector:
      return a->length == b->length && unify(a->elem, b->elem);
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
      // `tuple` (bare) resolves to Tuple{any}: a wildcard matching tuples of
      // any arity, like Python's bare `tuple` annotation.
      auto anyTuple = [](const TypePtr& t) {
        return t->params.size() == 1 &&
               resolveType(t->params[0])->kind == TypeKind::Any;
      };
      if (anyTuple(a) || anyTuple(b)) return true;
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

namespace {
// Mark a type tree (and everything nested in it) as written explicitly in
// source, so the checker knows which mismatches are annotation violations.
void markFromAnnotation(const TypePtr& t) {
  if (!t) return;
  t->fromAnnotation = true;
  markFromAnnotation(t->elem);
  markFromAnnotation(t->errorType);
  markFromAnnotation(t->ret);
  for (const auto& p : t->params) markFromAnnotation(p);
}
// `None` written in type position (the parser carries it as a NamedTypeExpr
// named "None" because `None` is a keyword, not an identifier).
bool isNoneTypeExpr(const TypeExpr& expr) {
  const auto* named = dynamic_cast<const NamedTypeExpr*>(&expr);
  return named && named->qualifiers.empty() && named->name == "None" &&
         named->typeArgs.empty();
}
}  // namespace

TypePtr Sema::resolveTypeExpr(TypeExpr& expr) {
  TypePtr type = resolveTypeExprImpl(expr);
  markFromAnnotation(type);
  return type;
}

TypePtr Sema::resolveTypeExprImpl(TypeExpr& expr) {
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
  if (auto* uni = dynamic_cast<UnionTypeExpr*>(&expr)) {
    // `T | U | ...`. A `None` member folds into an Optional around the rest
    // (`int | None` == `int?`, `int | str | None` == `(int | str)?`), matching
    // Python's `Optional`/`Union` semantics. Nested unions flatten.
    std::vector<TypePtr> members;
    bool hasNone = false;
    for (auto& element : uni->elements) {
      if (isNoneTypeExpr(*element)) {
        hasNone = true;
        continue;
      }
      TypePtr member = resolveTypeExpr(*element);
      TypePtr resolved = resolveType(member);
      if (resolved && resolved->kind == TypeKind::Union) {
        for (const auto& sub : resolved->params) members.push_back(sub);
        continue;
      }
      members.push_back(member);
    }
    TypePtr result;
    if (members.empty()) {
      // `x: None` / `None | None`: the type of the `None` literal itself —
      // an optional whose payload stays dynamic.
      TypePtr elem = freshVar(Constraint::None, expr.span,
                              "the value type of 'None'");
      elem->allowUnresolved = true;
      result = typeOptional(elem);
    } else if (members.size() == 1) {
      result = members.front();
    } else {
      result = typeUnion(std::move(members));
    }
    if (hasNone) result = typeOptional(result);
    return result;
  }
  if (dynamic_cast<EllipsisTypeExpr*>(&expr)) {
    sink_.error(expr.span, "'...' is only valid inside a type argument list, "
                           "e.g. tuple[int, ...].");
    return typeError();
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
    // Fixed-width SIMD vectors: `vector[T, N]` (alias `simd[T, N]`) where T
    // is a numeric element type and N is a comptime lane count (1..64).
    if ((named->name == "vector" || named->name == "simd") &&
        !named->typeArgs.empty()) {
      if (named->typeArgs.size() != 2) {
        sink_.error(named->span,
                    "'" + named->name + "' takes two type arguments: " +
                        named->name + "[T, N] (element type, lane count).");
        return typeError();
      }
      TypePtr elem = resolveTypeExpr(*named->typeArgs[0]);
      TypePtr elemResolved = resolveType(elem);
      if (!elemResolved ||
          (elemResolved->kind != TypeKind::Int &&
           elemResolved->kind != TypeKind::Float)) {
        sink_.error(named->typeArgs[0]->span,
                    "Vector element type must be numeric (int or float), found " +
                        typeName(elem) + ".");
        return typeError();
      }
      auto* lanesArg = dynamic_cast<ConstTypeExpr*>(named->typeArgs[1].get());
      if (!lanesArg) {
        sink_.error(named->typeArgs[1]->span,
                    "Vector lane count must be an integer literal, e.g. " +
                        named->name + "[float, 4].");
        return typeError();
      }
      if (lanesArg->value < 1 || lanesArg->value > 64) {
        sink_.error(named->typeArgs[1]->span,
                    "Vector lane count must be between 1 and 64, found " +
                        std::to_string(lanesArg->value) + ".");
        return typeError();
      }
      return typeVector(elem, lanesArg->value);
    }

    // ---- Python-typing compatibility names -------------------------------
    // These are recognized after user-defined structs/enums/aliases so a
    // user type with the same name still shadows them.
    auto typeArgCount = [&](std::size_t expected, const char* usage) -> bool {
      if (named->typeArgs.size() == expected) return true;
      sink_.error(named->span,
                  "'" + named->name + "' expects " + usage + ", found " +
                      std::to_string(named->typeArgs.size()) + " type argument(s).");
      return false;
    };
    auto resolveArgs = [&]() {
      std::vector<TypePtr> args;
      args.reserve(named->typeArgs.size());
      for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
      return args;
    };
    // `T` or bare `name` (no arguments): bare forms use `any` element types,
    // matching Python's bare `list`/`dict`/`set`/`tuple` annotations.
    auto containerElem = [&](std::size_t i, const std::vector<TypePtr>& args) {
      return i < args.size() ? args[i] : typeAny();
    };

    if (named->name == "any" || named->name == "Any") {
      if (!named->typeArgs.empty()) {
        sink_.error(named->span, "'any' does not take type arguments.");
        return typeError();
      }
      return typeAny();
    }
    if (named->name == "None") {
      if (!named->typeArgs.empty()) {
        sink_.error(named->span, "'None' does not take type arguments.");
        return typeError();
      }
      // The type of the `None` literal: an optional with a dynamic payload.
      TypePtr elem = freshVar(Constraint::None, named->span,
                              "the value type of 'None'");
      elem->allowUnresolved = true;
      return typeOptional(elem);
    }
    if (named->name == "Optional") {
      if (!typeArgCount(1, "one type argument: Optional[T]")) return typeError();
      return typeOptional(resolveArgs().front());
    }
    if (named->name == "Union") {
      if (named->typeArgs.empty()) {
        sink_.error(named->span, "'Union' expects at least one type argument: Union[T, U].");
        return typeError();
      }
      std::vector<TypePtr> members = resolveArgs();
      bool hasNone = false;
      std::vector<TypePtr> kept;
      for (auto& m : members) {
        TypePtr r = resolveType(m);
        if (r && r->kind == TypeKind::Optional &&
            r->elem && resolveType(r->elem)->kind == TypeKind::Var &&
            resolveType(r->elem)->allowUnresolved) {
          // A resolved `None` type argument (Optional<dynamic>) marks the
          // union as optional.
          hasNone = true;
          continue;
        }
        if (r && r->kind == TypeKind::Union) {
          for (const auto& sub : r->params) kept.push_back(sub);
          continue;
        }
        kept.push_back(m);
      }
      TypePtr result;
      if (kept.empty()) {
        TypePtr elem = freshVar(Constraint::None, named->span,
                                "the value type of 'None'");
        elem->allowUnresolved = true;
        result = typeOptional(elem);
      } else if (kept.size() == 1) {
        result = kept.front();
      } else {
        result = typeUnion(std::move(kept));
      }
      if (hasNone) result = typeOptional(result);
      return result;
    }
    if (named->name == "list" || named->name == "List" ||
        named->name == "Sequence") {
      if (named->typeArgs.size() > 1) {
        sink_.error(named->span, "'" + named->name +
                                 "' expects at most one type argument: " +
                                 named->name + "[T].");
        return typeError();
      }
      std::vector<TypePtr> args = resolveArgs();
      return typeArray(containerElem(0, args));
    }
    if (named->name == "dict" || named->name == "Dict" ||
        named->name == "Mapping") {
      if (named->typeArgs.size() > 2) {
        sink_.error(named->span, "'" + named->name +
                                 "' expects at most two type arguments: " +
                                 named->name + "[K, V].");
        return typeError();
      }
      std::vector<TypePtr> args = resolveArgs();
      return typeMap(containerElem(0, args), containerElem(1, args));
    }
    if (named->name == "set" || named->name == "Set" ||
        named->name == "frozenset" || named->name == "FrozenSet") {
      if (named->typeArgs.size() > 1) {
        sink_.error(named->span, "'" + named->name +
                                 "' expects at most one type argument: " +
                                 named->name + "[T].");
        return typeError();
      }
      std::vector<TypePtr> args = resolveArgs();
      return typeSet(containerElem(0, args));
    }
    if (named->name == "tuple" || named->name == "Tuple") {
      std::vector<TypePtr> args = resolveArgs();
      // `tuple[T, ...]` is a homogeneous variable-length tuple; A's dynamic
      // array is the closest existing type.
      if (!args.empty() &&
          dynamic_cast<EllipsisTypeExpr*>(named->typeArgs.back().get())) {
        if (args.size() != 2) {
          sink_.error(named->typeArgs.back()->span,
                      "'tuple[T, ...]' takes exactly one element type before '...'.");
          return typeError();
        }
        return typeArray(args[0]);
      }
      for (auto& a : named->typeArgs) {
        if (dynamic_cast<EllipsisTypeExpr*>(a.get())) {
          sink_.error(a->span, "'...' is only allowed as the last tuple type argument.");
          return typeError();
        }
      }
      if (args.empty()) return typeTuple({typeAny()});
      return typeTuple(std::move(args));
    }
    if (named->name == "Literal") {
      sink_.error(named->span,
                  "Literal types are not supported.",
                  "Annotate the parameter with the value's type instead, e.g. int or str.");
      return typeError();
    }
    if (named->name == "Callable") {
      sink_.error(named->span,
                  "Callable[...] is not supported; use the function type syntax instead.",
                  "Write fn(T1, T2) -> R, e.g. fn(int, str) -> bool.");
      return typeError();
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
  if (!type) return type;
  // Consult the mapping on the unresolved node first: a mapped type variable
  // must substitute even if it has already been linked by an earlier use.
  auto direct = mapping.find(type);
  if (direct != mapping.end()) return direct->second;
  type = resolveType(type);
  if (!type) return type;
  auto resolved = mapping.find(type);
  if (resolved != mapping.end()) return resolved->second;
  if (type->kind == TypeKind::Var) {
    // create a fresh var preserving constraints
    TypePtr fresh = makeType(TypeKind::Var);
    fresh->varId = nextVarId_++;
    fresh->constraint = type->constraint;
    fresh->allowDefault = type->allowDefault;
    fresh->allowUnresolved = type->allowUnresolved;
    fresh->fromAnnotation = type->fromAnnotation;
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
  result->fromAnnotation = type->fromAnnotation;
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
  info.variableDecl = std::string(what) == "variable";
  declaredVariables_.push_back(std::move(info));
}

}  // namespace a
