#include "type-checker/sema.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>

#include "name-resolution/native_modules.hpp"
#include "tooling/source_text.hpp"
#include "type-checker/debug/magic_identifiers.hpp"
#include "type-checker/method_signatures.hpp"

namespace a {
namespace {

bool isConversionBuiltin(const std::string& name) {
  static const char* names[] = {"i8", "i16",  "i32", "i64", "u8",
                                "u16", "u32", "u64", "f32", "f64"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isMathBuiltin(const std::string& name) {
  static const char* names[] = {"sqrt", "sin",  "cos",   "tan",   "exp",  "log",
                                "floor", "ceil", "round", "trunc", "log2", "log10"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isKnownBuiltin(const std::string& name) {
  static const char* names[] = {
      "print",    "eprint",   "input",     "len",      "str",      "abs",
      "int",      "float",    "min",       "max",      "pow",      "flush",
      "read_file", "write_file",
      "append_file", "file_exists", "list_dir", "remove_file", "create_dir",
      "remove_dir", "rename", "current_dir", "now", "sleep", "panic", "assert",
      "open", "close",
      "assert_eq", "map",  "filter",   "reduce",   "any",
      "all",      "sum",      "join",      "random",   "random_int", "atan2",
      "set",      "unsafe_get", "unsafe_set",
      "arena_new", "arena_pool", "arena_region", "arena_alloc", "arena_release",
      "arena_reset", "arena_used", "arena_total", "move", "copy",
      "borrow", "borrow_mut", "unsafe_address_of", "unsafe_load", "unsafe_store",
      "unsafe_offset",
      "Some", "Ok", "Err", "ic",
      "__file__", "__line__", "__func__", "__function__", "__source_file__", "__source_line__",
      "__repr__", "__debug_repr__", "__debug_time__", "__debug_context__", "__ic_format__",
      // LLVM-style intrinsics
      "ctz", "clz", "popcount", "bswap", "rotl", "rotr",
      "fma", "fabs", "copysign",
      "smin", "smax", "umin", "umax",
      "sadd_overflow", "ssub_overflow", "smul_overflow",
      "uadd_overflow", "usub_overflow", "umul_overflow",
      "expect", "likely", "unlikely"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return isConversionBuiltin(name) || isMathBuiltin(name) || isStdlibBuiltin(name);
}

TypePtr builtinConstantType(const std::string& name) {
  if (name == "PI" || name == "E") return typeFloat(64);
  if (name == "stdout" || name == "stderr") return typeIO();
  struct IntConstant {
    const char* name;
    int bits;
    bool isSigned;
  };
  static const IntConstant ints[] = {
      {"I8_MIN", 8, true},   {"I8_MAX", 8, true},   {"I16_MIN", 16, true},
      {"I16_MAX", 16, true}, {"I32_MIN", 32, true}, {"I32_MAX", 32, true},
      {"I64_MIN", 64, true}, {"I64_MAX", 64, true}, {"U8_MAX", 8, false},
      {"U16_MAX", 16, false}, {"U32_MAX", 32, false}};
  for (const auto& entry : ints) {
    if (name == entry.name) return typeInt(entry.bits, entry.isSigned);
  }
  struct FloatConstant {
    const char* name;
    int bits;
  };
  static const FloatConstant floats[] = {
      {"F32_MIN", 32}, {"F32_MAX", 32}, {"F64_MIN", 64}, {"F64_MAX", 64}};
  for (const auto& entry : floats) {
    if (name == entry.name) return typeFloat(entry.bits);
  }
  return nullptr;
}

bool isBuiltinConstant(const std::string& name) {
  return builtinConstantType(name) != nullptr;
}

// Collapses every whitespace run in an `ic` argument's source slice to a
// single space and trims the ends, so multi-line expressions stay on one
// debug line.
std::string collapseIcWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

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

Sema::ModuleAlias* Sema::findAlias(const std::string& name) {
  auto found = moduleAliases_.find(name);
  return found == moduleAliases_.end() ? nullptr : &found->second;
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
static std::recursive_mutex gVarMutex;

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
    sink_.error(named->span, "Unknown type '" + named->name + "'.",
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
  if (!decl.baseClass.empty()) {
    std::string baseQ = qualified(decl.baseClass);
    auto baseIt = structs_.find(baseQ);
    if (baseIt == structs_.end()) {
      baseIt = structs_.find(decl.baseClass);
    }
    if (baseIt != structs_.end()) {
      for (const auto& m : baseIt->second.methods) {
        sym.methods.insert(m);
      }
    }
  }

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
      sink_.error(method.nameSpan, "Duplicate method '" + method.name + "' in struct '" + decl.name + "'.");
      continue;
    }
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
    index = 1;
  }

  for (std::size_t i = index; i < decl.params.size(); ++i) {
    ParamDecl& param = decl.params[i];
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
  }
  // `comptime N: int` parameters are bound as typed values in the body. They are
  // not passed at call sites yet; specialization is tracked in TODO.md.
  for (const ComptimeParam& param : decl.comptimeParams) {
    TypePtr type = param.type ? resolveTypeExpr(*param.type) : typeInt(64, true);
    declareVar(param.name, param.span, type, false, "compile-time parameter");
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
                            const std::string& what) {
  for (const std::string& name : traits) {
    if (findTrait(name) == nullptr) {
      sink_.error(span.valid() ? span : Span(),
                  "Unknown trait '" + name + "' in the 'where' clause of " + what + ".",
                  "Declare it with 'trait " + name + ":' before it is used as a constraint.");
    }
  }
}

void Sema::checkTraitDecl(TraitDecl& decl) {
  checkWhereClause(decl.parents, decl.nameSpan,
                   "trait '" + decl.name + "'");
  checkWhereClause(decl.whereClause, decl.nameSpan, "trait '" + decl.name + "'");

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

// ---------------------------------------------------------------- analyze

void Sema::registerImports(Module& module) {
  auto aliasStem = [](const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
  };

  for (auto& statement : module.program->statements) {
    if (auto* import = dynamic_cast<ImportStmt*>(statement.get())) {
      std::string alias = import->alias;
      if (alias.empty()) {
        if (import->quoted) alias = aliasStem(import->path);
        else if (import->pythonModuleImport) {
          alias = import->pythonModule;
          std::size_t dot = alias.rfind('.');
          if (dot != std::string::npos) alias = alias.substr(dot + 1);
        } else alias = import->bareName;
      }
      if (alias.empty()) continue;

      ModuleAlias entry;
      entry.native = import->nativeModule;
      entry.python = import->pythonModuleImport;
      entry.moduleId = import->moduleId;
      entry.nativeName = import->bareName;
      entry.pythonName = import->pythonModule;
      if (!entry.native && !entry.python && entry.moduleId < 0) continue;

      if (moduleAliases_.count(alias)) {
        sink_.error(import->span, "Duplicate import alias '" + alias + "'.");
        continue;
      }
      moduleAliases_[alias] = entry;
      continue;
    }

    if (auto* from = dynamic_cast<FromImportStmt*>(statement.get())) {
      for (auto& item : from->items) {
        FromBinding binding;
        binding.span = item.span;
        binding.moduleId = from->moduleId;

        if (from->pythonModuleImport) {
          binding.python = true;
          binding.kind = FromBinding::Kind::Function; // dynamic Python member/callable at runtime
          binding.pythonModule = from->pythonModule;
          binding.pythonMember = item.name;
          { auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; binding.type=t; }
        } else if (from->nativeModule) {
          binding.native = true;
          if (const std::string* fn = nativeFunction(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Function;
            binding.nativeName = *fn;
          } else if (const std::string* constant = nativeConstant(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Value;
            binding.nativeName = *constant;
            binding.type = builtinConstantType(*constant);
            if (!binding.type) {
              sink_.error(item.span, "Unsupported native constant '" + item.name + "'.");
              continue;
            }
          } else {
            sink_.error(item.span,
                        "Module '" + from->bareName + "' has no member '" + item.name + "'.");
            continue;
          }
        } else if (from->moduleId >= 0) {
          ModuleExports& exports = exportsFor(from->moduleId);
          if (auto fn = exports.functions.find(item.name); fn != exports.functions.end()) {
            binding.kind = FromBinding::Kind::Function;
            binding.fn = fn->second;
            auto sig = signatures_.find(binding.fn);
            if (sig != signatures_.end()) {
              binding.type = typeFunction(sig->second.paramTypes, sig->second.retType);
            }
          } else if (auto st = exports.structs.find(item.name); st != exports.structs.end()) {
            binding.kind = FromBinding::Kind::Struct;
            binding.decl = st->second;
            // The struct may be re-exported by a package __init__, so qualify it
            // with the module that actually declared it.
            const auto owning = structModules_.find(st->second);
            const int ownerId = owning != structModules_.end() ? owning->second : from->moduleId;
            binding.structName = qualifiedFor(ownerId, st->second->name);
            binding.type = typeStruct(binding.structName);
          } else if (auto tr = exports.traits.find(item.name); tr != exports.traits.end()) {
            binding.kind = FromBinding::Kind::Trait;
            binding.traitName = tr->second;
          } else if (auto value = exports.values.find(item.name); value != exports.values.end()) {
            binding.kind = FromBinding::Kind::Value;
            binding.type = value->second;
          } else {
            // Value exports are only built by checkModule, which runs after
            // registerImports for every module. Fall back to scanning the
            // target module's top-level assignments so `from m import X`
            // works for plain `X = ...` bindings; the type stays dynamic
            // until the dependency's own checkModule refines it.
            Module& target = graph_.at(from->moduleId);
            bool found = false;
            if (target.program) {
              for (auto& stmt : target.program->statements) {
                if (auto* asg = dynamic_cast<AssignStmt*>(stmt.get())) {
                  if (auto* id = dynamic_cast<IdentExpr*>(asg->target.get())) {
                    if (id->name == item.name) { found = true; break; }
                  }
                } else if (auto* dst = dynamic_cast<DestructureStmt*>(stmt.get())) {
                  for (const auto& n : dst->names) {
                    if (n == item.name) { found = true; break; }
                  }
                  if (found) break;
                }
              }
            }
            if (!found) {
              sink_.error(item.span, "Module '" + exports.name + "' has no exported member '" +
                                         item.name + "'.");
              continue;
            }
            binding.kind = FromBinding::Kind::Value;
            auto t = std::make_shared<Type>();
            t->kind = TypeKind::Var;
            t->allowUnresolved = true;
            binding.type = t;
          }
        } else {
          continue;
        }

        if (fromBindings_.count(item.alias)) {
          sink_.error(item.span, "Duplicate imported name '" + item.alias + "'.");
          continue;
        }
        if (binding.kind == FromBinding::Kind::Struct) {
          structAliases_[item.alias] = binding.structName;
        }
        fromBindings_[item.alias] = std::move(binding);
      }
    }
  }
}

void Sema::buildModuleExports(Module& module) {
  ModuleExports& exports = exportsFor(module.id);
  if (!scopes_.empty()) {
    for (const auto& entry : scopes_.front()) {
      exports.values[entry.first] = entry.second.type;
    }
  }
  exports.structAliases = structAliases_;
}

SemaResult Sema::analyze() {
  for (int id : graph_.order) registerModule(graph_.at(id));
  for (int id : graph_.order) checkModule(graph_.at(id));

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  result.enums = enums_;
  result.modules = moduleExports_;
  result.fnModule = fnModules_;
  result.structModule = structModules_;
  result.fnParamTypes = fnParamTypes_;
  result.fnReturnTypes = fnReturnTypes_;
  for (const DeclaredVarInfo& info : declaredVariables_) {
    result.variables.push_back(VarTypeInfo{info.name, info.span, info.type});
  }
  // Mirror Expr::inferredType into the result map for LSP clients.
  result.exprTypes = exprTypes_;
  return result;
}

SemaResult Sema::analyzeParallel(int jobs) {
  for (int id : graph_.order) registerModule(graph_.at(id));
  if (jobs <= 0) jobs = 4;
  const std::size_t n = graph_.order.size();
  if (jobs <= 1 || n <= 1) {
    for (int id : graph_.order) checkModule(graph_.at(id));
  } else {
    // Each worker clones the registered tables and checks a strided subset;
    // per-worker outputs are merged under a lock. Diagnostics are already
    // thread-safe; unify is serialised inside Sema via a global lock.
    std::mutex mergeMutex;
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    int threads = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(jobs), n));
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([this, &next, &mergeMutex, t, threads] {
        Sema worker(graph_, sink_);
        worker.functionsByName_ = functionsByName_;
        worker.structsByName_ = structsByName_;
        worker.traits_ = traits_;
        worker.traitImpls_ = traitImpls_;
        worker.enums_ = enums_;
        worker.signatures_ = signatures_;
        worker.structs_ = structs_;
        worker.fnModules_ = fnModules_;
        worker.structModules_ = structModules_;
        worker.moduleStates_ = moduleStates_;
        worker.moduleExports_ = moduleExports_;
        worker.nextVarId_ = nextVarId_;
        worker.typeVariables_ = typeVariables_;
        worker.typeAliases_ = typeAliases_;
        while (true) {
          std::size_t i = next.fetch_add(1);
          if (i >= graph_.order.size()) break;
          int id = graph_.order[i];
          worker.checkModule(graph_.at(id));
        }
        std::lock_guard<std::mutex> lock(mergeMutex);
        for (auto& entry : worker.fnParamTypes_) fnParamTypes_[entry.first] = std::move(entry.second);
        for (auto& entry : worker.fnReturnTypes_) fnReturnTypes_[entry.first] = std::move(entry.second);
        for (auto& entry : worker.exprTypes_) exprTypes_[entry.first] = std::move(entry.second);
        declaredVariables_.insert(declaredVariables_.end(), worker.declaredVariables_.begin(),
                                  worker.declaredVariables_.end());
        // typeVariables_ already holds registration vars in the base; workers
        // only defaulted them in place (via checkTypeVariables), so do not
        // merge copies. Body-created vars are already linked into Type
        // objects referenced by fnParamTypes_/exprTypes_.
        checkedFunctions_.insert(checkedFunctions_.end(), worker.checkedFunctions_.begin(),
                                 worker.checkedFunctions_.end());
        // Merge module exports (values) per module id.
        for (std::size_t m = 0; m < worker.moduleExports_.size() && m < moduleExports_.size(); ++m) {
          for (auto& entry : worker.moduleExports_[m].values) {
            moduleExports_[m].values[entry.first] = entry.second;
          }
        }
      });
    }
    for (auto& w : workers) w.join();
  }

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  result.enums = enums_;
  result.modules = moduleExports_;
  result.fnModule = fnModules_;
  result.structModule = structModules_;
  result.fnParamTypes = fnParamTypes_;
  result.fnReturnTypes = fnReturnTypes_;
  for (const DeclaredVarInfo& info : declaredVariables_) {
    result.variables.push_back(VarTypeInfo{info.name, info.span, info.type});
  }
  result.exprTypes = exprTypes_;
  return result;
}

void Sema::analyzeModule(Module& module) {
  registerModule(module);
  checkModule(module);
}

std::shared_ptr<Sema::AsyncHandle> Sema::analyzeAsync(int jobs) {
  auto handle = std::make_shared<AsyncHandle>();
  // Move a clone that owns a copy of the graph/sink references; the caller
  // keeps its graph alive until wait() is called.
  ModuleGraph& graph = graph_;
  DiagnosticSink& sink = sink_;
  handle->future = std::async(std::launch::async, [&graph, &sink, jobs] {
    Sema worker(graph, sink);
    return worker.analyzeParallel(jobs);
  });
  return handle;
}

void Sema::registerModule(Module& module) {
  currentModule_ = module.id;
  scopes_.clear();
  scopes_.push_back({});
  moduleAliases_.clear();
  fromBindings_.clear();
  structAliases_.clear();
  typeAliases_.clear();
  genericParams_.clear();
  loopDepth_ = 0;
  deferDepth_ = 0;
  currentFunction_ = nullptr;
  currentReturnType_ = nullptr;
  sawReturn_ = false;
  // typeVariables_ accumulates across all registrations (signature vars);
  // per-module check vectors are cleared in checkModule() instead.

  registerImports(module);

  ModuleExports& exports = exportsFor(module.id);
  exports.name = module.name;
  auto& statements = module.program->statements;

  // Re-export imported names, so package `__init__.a` files can aggregate
  // their submodules (`from "collections.a" import map_list`). Locally
  // declared names below take precedence on collision.
  for (const auto& entry : fromBindings_) {
    const FromBinding& binding = entry.second;
    switch (binding.kind) {
      case FromBinding::Kind::Function:
        if (!binding.native && binding.fn) exports.functions[entry.first] = binding.fn;
        break;
      case FromBinding::Kind::Struct:
        if (binding.decl) exports.structs[entry.first] = binding.decl;
        break;
      case FromBinding::Kind::Trait:
        exports.traits[entry.first] = binding.traitName;
        break;
      case FromBinding::Kind::Value:
        if (binding.type) exports.values[entry.first] = binding.type;
        break;
    }
  }

  // Pass 1: collect declaration names so duplicates and collisions are clear.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      const std::string q = qualified(decl->name);
      if (structsByName_.count(q) || functionsByName_.count(q)) {
        sink_.error(decl->nameSpan, "Duplicate declaration of '" + decl->name + "'.");
        continue;
      }
      StructSym sym;
      sym.name = q;
      sym.decl = decl;
      structs_[q] = std::move(sym);
      structsByName_[q] = decl;
      structModules_[decl] = currentModule_;
      exports.structs[decl->name] = decl;
    } else if (auto* en = dynamic_cast<EnumDecl*>(stmt.get())) {
      const std::string q = qualified(en->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q)) {
        sink_.error(en->nameSpan, "Duplicate declaration of '" + en->name + "'.");
        continue;
      }
      exports.enums[en->name] = q;
      registerEnum(*en);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q) || typeAliases_.count(q)) {
        sink_.error(ta->nameSpan, "Duplicate declaration of '" + ta->name + "'.");
        continue;
      }
    } else if (auto* tr = dynamic_cast<TraitDecl*>(stmt.get())) {
      const std::string q = qualified(tr->name);
      if (traits_.find(q) != traits_.end() || functionsByName_.count(q) || structsByName_.count(q)) {
        sink_.error(tr->nameSpan, "Duplicate declaration of '" + tr->name + "'.");
        continue;
      }
      traits_[q] = tr;
      exports.traits[tr->name] = q;
    } else if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
      const std::string q = qualified(fn->name);
      if (functionsByName_.count(q) || structsByName_.count(q)) {
        sink_.error(fn->nameSpan, "Duplicate declaration of '" + fn->name + "'.");
        continue;
      }
      functionsByName_[q] = fn;
      fnModules_[fn] = currentModule_;
      fn->moduleId = currentModule_;
      exports.functions[fn->name] = fn;
    }
  }

  // Pass 2: resolve struct layouts and method signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      registerStructLayout(*decl);
      registerMethodSignatures(*decl);
    } else if (auto* en = dynamic_cast<EnumDecl*>(stmt.get())) {
      resolveEnumPayloads(*en);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      typeAliases_[q] = resolveTypeExpr(*ta->type);
    }
  }

  // Pass 2b: traits and conformance. Traits are already registered by name in
  // pass 1; struct layouts exist after pass 2, so `impl T for S` can be checked
  // against the struct's method table.
  for (auto& stmt : statements) {
    if (auto* tr = dynamic_cast<TraitDecl*>(stmt.get())) {
      if (traits_.count(qualified(tr->name)) && traits_[qualified(tr->name)] == tr) {
        checkTraitDecl(*tr);
      }
    } else if (auto* impl = dynamic_cast<ImplDecl*>(stmt.get())) {
      checkImpl(*impl);
    }
  }

  // Pass 3: function signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      registerFunctionSignature(*decl);
      checkWhereClause(decl->whereClause, decl->whereSpan,
                       "function '" + decl->name + "'");
    }
  }

  // Snapshot import/alias bindings for the parallel body-check phase.
  ModuleCheckState state;
  state.moduleAliases = moduleAliases_;
  state.fromBindings = fromBindings_;
  state.structAliases = structAliases_;
  state.typeAliases = typeAliases_;
  moduleStates_[module.id] = std::move(state);
}

void Sema::checkModule(Module& module) {
  currentModule_ = module.id;
  auto foundState = moduleStates_.find(module.id);
  if (foundState != moduleStates_.end()) {
    moduleAliases_ = foundState->second.moduleAliases;
    fromBindings_ = foundState->second.fromBindings;
    structAliases_ = foundState->second.structAliases;
    typeAliases_ = foundState->second.typeAliases;
  }
  scopes_.clear();
  scopes_.push_back({});
  genericParams_.clear();
  loopDepth_ = 0;
  deferDepth_ = 0;
  currentFunction_ = nullptr;
  currentReturnType_ = nullptr;
  sawReturn_ = false;
  // typeVariables_ holds signature vars from registration (all modules);
  // keep it so checkTypeVariables() defaults every var once.
  declaredVariables_.clear();
  checkedFunctions_.clear();

  auto& statements = module.program->statements;

  // Pass 4: top-level statements (global variables and executable code).
  for (auto& stmt : statements) {
    if (dynamic_cast<FnDecl*>(stmt.get()) || dynamic_cast<StructDecl*>(stmt.get()) ||
        dynamic_cast<TraitDecl*>(stmt.get()) || dynamic_cast<ImplDecl*>(stmt.get()) ||
        dynamic_cast<EnumDecl*>(stmt.get()) ||
        dynamic_cast<ImportStmt*>(stmt.get()) || dynamic_cast<FromImportStmt*>(stmt.get())) {
      continue;
    }
    checkStatement(*stmt);
  }

  // Pass 5: function and method bodies.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      checkStructMethodBodies(*decl);
    }
  }
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      auto sig = signatures_.find(decl);
      if (sig == signatures_.end()) continue;
      checkFunctionBody(*decl, sig->second);
    }
  }

  checkTypeVariables();
  checkDeclaredVariables();
  checkReturnPaths();

  buildModuleExports(module);
}

void Sema::checkModuleBodies(Module& module) { checkModule(module); }

// ------------------------------------------------------------- statement checks

namespace {

// True when control can never continue past this statement.
bool terminatesFlow(const Stmt& stmt) {
  if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
  if (dynamic_cast<const BreakStmt*>(&stmt)) return true;
  if (dynamic_cast<const ContinueStmt*>(&stmt)) return true;
  if (dynamic_cast<const LoopStmt*>(&stmt)) return true;  // loop ... only exits via break
  if (const auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
    if (node->elseBody.empty()) return false;
    for (const auto& branch : node->branches) {
      if (branch.body.empty() || !terminatesFlow(*branch.body.back())) return false;
    }
    return terminatesFlow(*node->elseBody.back());
  }
  return false;
}

}  // namespace

void Sema::checkStatements(std::vector<StmtPtr>& statements) {
  bool terminated = false;
  bool warned = false;
  for (auto& stmt : statements) {
    if (terminated && !warned) {
      sink_.warning(stmt->span, "W0002", "Unreachable statement.",
                    "Control cannot reach this statement because an earlier one always exits.");
      warned = true;
    }
    checkStatement(*stmt);
    if (!terminated && terminatesFlow(*stmt)) terminated = true;
  }
}

void Sema::checkStatement(Stmt& stmt) {
  if (auto* node = dynamic_cast<LetStmt*>(&stmt)) {
    checkLet(*node);
    return;
  }
  if (auto* node = dynamic_cast<AssignStmt*>(&stmt)) {
    checkAssign(*node);
    return;
  }
  if (auto* node = dynamic_cast<DestructureStmt*>(&stmt)) {
    checkDestructure(*node);
    return;
  }
  if (auto* node = dynamic_cast<ExprStmt*>(&stmt)) {
    inferExpr(*node->expr);
    return;
  }
  if (auto* node = dynamic_cast<IfStmt*>(&stmt)) {
    checkIf(*node);
    return;
  }
  if (auto* node = dynamic_cast<WhileStmt*>(&stmt)) {
    checkWhile(*node);
    return;
  }
  if (auto* node = dynamic_cast<DoWhileStmt*>(&stmt)) {
    checkDoWhile(*node);
    return;
  }
  if (auto* node = dynamic_cast<ForStmt*>(&stmt)) {
    checkFor(*node);
    return;
  }
  if (auto* node = dynamic_cast<LoopStmt*>(&stmt)) {
    checkLoop(*node);
    return;
  }
  if (auto* node = dynamic_cast<ReturnStmt*>(&stmt)) {
    checkReturn(*node);
    return;
  }
  if (auto* node = dynamic_cast<DeferStmt*>(&stmt)) {
    checkDefer(*node);
    return;
  }
  if (auto* node = dynamic_cast<BreakStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'break' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'break' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<ContinueStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'continue' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'continue' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<FnDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Functions can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<TraitDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Traits can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<ImplDecl*>(&stmt)) {
    sink_.error(node->span, "Conformances ('impl ... for') can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<StructDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Structs can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<EnumDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Enums can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<ImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
    return;
  }
  if (auto* node = dynamic_cast<FromImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
    return;
  }
}

void Sema::checkLet(LetStmt& stmt) {
  TypePtr initType = inferExpr(*stmt.init);
  TypePtr variableType = initType;
  if (stmt.type) {
    TypePtr declared = resolveTypeExpr(*stmt.type);
    if (resolveType(declared)->kind == TypeKind::Void) {
      sink_.error(stmt.span, "A variable cannot have type void.");
      declared = typeError();
    }
    if (!unify(initType, declared)) {
      reportMismatch(initType, declared, stmt.init->span,
                     "in the initializer for '" + stmt.name + "'");
    } else {
      checkFixedArrayLength(*stmt.init, declared);
    }
    variableType = declared;
  } else if (resolveType(initType)->kind == TypeKind::Void) {
    sink_.error(stmt.init->span,
                "Cannot initialize '" + stmt.name + "' with a void value.");
    variableType = typeError();
  }

  declareVar(stmt.name, stmt.nameSpan, variableType, stmt.mutableBinding,
             "variable", false);
}

void Sema::checkAssign(AssignStmt& stmt) {
  if (auto* ident = dynamic_cast<IdentExpr*>(stmt.target.get())) {
    VarSym* sym = lookupVar(ident->name);
    if (!sym) {
      if (isBuiltinConstant(ident->name) || ident->name == "None") {
        sink_.error(ident->span,
                    "Cannot assign to the builtin constant '" + ident->name + "'.");
        inferExpr(*stmt.value);
        return;
      }
      if (stmt.op != "=") {
        sink_.error(stmt.target->span,
                    "Undefined variable '" + ident->name + "' in compound assignment.");
        return;
      }
      TypePtr valueType = inferExpr(*stmt.value);
      if (resolveType(valueType)->kind == TypeKind::Void) {
        sink_.error(stmt.value->span,
                    "Cannot initialize '" + ident->name + "' with a void value.");
        valueType = typeError();
      }
      declareVar(ident->name, ident->span, valueType, true);
      return;
    }

    sym->mutableBinding = true;
    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      sym->used = true;
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, sym->type, stmt.opSpan)) return;
      if (!unify(valueType, sym->type)) {
        reportMismatch(valueType, sym->type, stmt.value->span,
                       "in assignment to '" + ident->name + "'");
      } else {
        checkFixedArrayLength(*stmt.value, sym->type);
      }
    } else {
      // Dynamic: reassignment may change type (Python-style).
      sym->type = valueType;
    }
    return;
  }

  if (dynamic_cast<FieldExpr*>(stmt.target.get()) ||
      dynamic_cast<IndexExpr*>(stmt.target.get())) {
    TypePtr targetType = inferExpr(*stmt.target);
    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, targetType, stmt.opSpan)) return;
    }
    if (!unify(valueType, targetType)) {
      reportMismatch(valueType, targetType, stmt.value->span, "in assignment");
    } else {
      checkFixedArrayLength(*stmt.value, targetType);
    }
    return;
  }

  sink_.error(stmt.target->span,
              "Invalid assignment target.",
              "Only variables, struct fields, and array elements can be assigned to.");
}

void Sema::checkDestructure(DestructureStmt& stmt) {
  TypePtr valueType = inferExpr(*stmt.value);
  TypePtr resolved = resolveType(valueType);

  // Dynamic: always bind names (arity/type checked at runtime).
  for (std::size_t i = 0; i < stmt.names.size(); ++i) {
    const std::string& name = stmt.names[i];
    if (name == "_") continue;
    TypePtr element = typeError();
    if (resolved->kind == TypeKind::Tuple && i < resolved->params.size())
      element = resolved->params[i];
    if (resolveType(element)->kind == TypeKind::Void) element = typeError();
    declareVar(name, stmt.nameSpans[i], element, stmt.mutableBinding);
  }
}

std::vector<std::pair<std::string, bool>> Sema::narrowingFacts(Expr& cond, bool truthy) {
  std::vector<std::pair<std::string, bool>> facts;
  auto isNoneExpr = [](Expr* e) -> bool {
    if (!e) return false;
    if (auto* id = dynamic_cast<IdentExpr*>(e)) return id->name == "None";
    return false;
  };
  if (auto* unary = dynamic_cast<UnaryExpr*>(&cond)) {
    if ((unary->op == "not" || unary->op == "!") && unary->operand) {
      return narrowingFacts(*unary->operand, !truthy);
    }
    return facts;
  }
  if (auto* bin = dynamic_cast<BinaryExpr*>(&cond)) {
    if (bin->op == "and") {
      if (truthy) {
        facts = narrowingFacts(*bin->lhs, true);
        auto more = narrowingFacts(*bin->rhs, true);
        facts.insert(facts.end(), more.begin(), more.end());
      }
      // `a and b` false: neither operand is known, no facts.
      return facts;
    }
    if (bin->op == "or") {
      if (!truthy) {
        facts = narrowingFacts(*bin->lhs, false);
        auto more = narrowingFacts(*bin->rhs, false);
        facts.insert(facts.end(), more.begin(), more.end());
      }
      return facts;
    }
    if (bin->op == "==" || bin->op == "!=") {
      std::string varName;
      if (auto* lhs = dynamic_cast<IdentExpr*>(bin->lhs.get())) {
        if (isNoneExpr(bin->rhs.get())) varName = lhs->name;
      } else if (auto* rhs = dynamic_cast<IdentExpr*>(bin->rhs.get())) {
        if (isNoneExpr(bin->lhs.get())) varName = rhs->name;
      }
      if (!varName.empty()) {
        bool nonNone = (bin->op == "!=") == truthy;
        facts.emplace_back(varName, nonNone);
      }
    }
  }
  return facts;
}

std::vector<Sema::NarrowRestore> Sema::applyNarrowing(Expr& cond, bool truthy) {
  std::vector<NarrowRestore> restores;
  for (auto& [name, nonNone] : narrowingFacts(cond, truthy)) {
    VarSym* sym = lookupVar(name);
    if (!sym) continue;
    // Avoid double-narrowing the same symbol in one condition.
    bool already = false;
    for (auto& r : restores) if (r.sym == sym) { already = true; break; }
    if (already) continue;
    TypePtr resolved = resolveType(sym->type);
    if (!nonNone) continue;  // only refine the non-None branch (T? -> T)
    if (resolved && resolved->kind == TypeKind::Optional && resolved->elem) {
      restores.push_back(NarrowRestore{sym, sym->type});
      sym->type = resolved->elem;
    }
  }
  return restores;
}

void Sema::restoreNarrowing(std::vector<NarrowRestore>& restores) {
  for (auto& r : restores) {
    if (r.sym) r.sym->type = r.saved;
  }
  restores.clear();
}

void Sema::checkIf(IfStmt& stmt) {
  bool prevBranchAlwaysReturns = false;
  for (std::size_t i = 0; i < stmt.branches.size(); ++i) {
    auto& branch = stmt.branches[i];
    TypePtr condType = inferExpr(*branch.cond);
    requireBool(condType, branch.cond->span, "The 'if' condition");
    bool isLastBranch = (i + 1 == stmt.branches.size());
    if (!prevBranchAlwaysReturns && isLastBranch && stmt.elseBody.empty() &&
        narrowingFacts(*branch.cond, true).empty()) {
      // No-op: most branches have no narrowing; fall through.
    }
    // Elif: previous branches were false, so their negated facts apply.
    std::vector<NarrowRestore> outerRestores;
    for (std::size_t j = 0; j < i; ++j) {
      auto nested = applyNarrowing(*stmt.branches[j].cond, false);
      outerRestores.insert(outerRestores.end(), nested.begin(), nested.end());
    }
    auto restores = applyNarrowing(*branch.cond, true);
    pushScope();
    checkStatements(branch.body);
    popScope();
    restoreNarrowing(restores);
    restoreNarrowing(outerRestores);
    if (!branch.body.empty() && terminatesFlow(*branch.body.back())) prevBranchAlwaysReturns = true;
  }
  if (!stmt.elseBody.empty()) {
    std::vector<NarrowRestore> outerRestores;
    for (auto& branch : stmt.branches) {
      auto nested = applyNarrowing(*branch.cond, false);
      outerRestores.insert(outerRestores.end(), nested.begin(), nested.end());
    }
    pushScope();
    checkStatements(stmt.elseBody);
    popScope();
    restoreNarrowing(outerRestores);
  }
}

void Sema::checkWhile(WhileStmt& stmt) {
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkDoWhile(DoWhileStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
}

TypePtr Sema::iterableElementType(TypePtr iterableType, Span span, const std::string& hint) {
  TypePtr resolved = resolveType(iterableType);
  if (resolved->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Range ||
      resolved->kind == TypeKind::Set) {
    return resolved->elem;
  }
  if (resolved->kind == TypeKind::Map) return resolved->elem;  // keys
  if (resolved->kind == TypeKind::String) return typeChar();
  sink_.error(span,
              "Cannot iterate over " + typePhrase(resolved) + ".",
              "Loops " + hint + " iterate over arrays, strings, maps (keys), sets, and integer "
              "ranges ('0..10').");
  return typeError();
}

void Sema::checkFor(ForStmt& stmt) {
  TypePtr elementType =
      iterableElementType(inferExpr(*stmt.iterable), stmt.iterable->span, "and comprehensions");

  ++loopDepth_;
  pushScope();
  declareVar(stmt.var, stmt.varSpan, elementType, false, "loop variable");
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkLoop(LoopStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkReturn(ReturnStmt& stmt) {
  if (!currentFunction_) {
    sink_.error(stmt.span, "'return' used outside of a function.");
    return;
  }
  if (deferDepth_ > 0) {
    sink_.error(stmt.span, "'return' cannot be used inside a defer statement.");
  }
  sawReturn_ = true;

  TypePtr expected = currentReturnType_;
  TypePtr resolvedExpected = resolveType(expected);

  if (stmt.value) {
    TypePtr valueType = inferExpr(*stmt.value);
    if (resolvedExpected->kind == TypeKind::Void && currentFunction_->returnType) {
      sink_.error(stmt.value->span,
                  "Function '" + currentFunction_->name +
                      "' has return type void and cannot return a value.");
      return;
    }
    if (!unify(valueType, expected)) {
      reportMismatch(valueType, expected, stmt.value->span,
                     "in the return value of '" + currentFunction_->name + "'");
    } else {
      checkFixedArrayLength(*stmt.value, expected);
    }
  } else {
    if (resolvedExpected->kind != TypeKind::Var &&
        resolvedExpected->kind != TypeKind::Void) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name + "' must return a value of type '" +
                      typeName(resolvedExpected) + "'.");
      return;
    }
    if (!unify(expected, typeVoid())) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name +
                      "' is expected to return a value, but this 'return' has none.");
    }
  }
}

void Sema::checkDefer(DeferStmt& stmt) {
  ++deferDepth_;
  pushScope();
  checkStatement(*stmt.body);
  popScope();
  --deferDepth_;
}

// ------------------------------------------------------------- expressions

TypePtr Sema::inferExpr(Expr& expr) {
  TypePtr type = inferExprImpl(expr);
  expr.inferredType = resolveType(type);
  exprTypes_[&expr] = type;
  return type;
}

TypePtr Sema::inferExprImpl(Expr& expr) {
  if (auto* node = dynamic_cast<IntLitExpr*>(&expr)) {
    (void)node;
    return freshVar(Constraint::Integer, expr.span, "an integer literal", /*allowDefault=*/true);
  }
  if (auto* node = dynamic_cast<FloatLitExpr*>(&expr)) {
    (void)node;
    return freshVar(Constraint::Floating, expr.span, "a float literal", /*allowDefault=*/true);
  }
  if (dynamic_cast<BoolLitExpr*>(&expr)) return typeBool();
  if (dynamic_cast<CharLitExpr*>(&expr)) return typeChar();
  if (auto* node = dynamic_cast<StringLitExpr*>(&expr)) return inferStringLiteral(*node);
  if (auto* node = dynamic_cast<IdentExpr*>(&expr)) return inferIdent(*node);
  if (auto* node = dynamic_cast<UnaryExpr*>(&expr)) return inferUnary(*node);
  if (auto* node = dynamic_cast<BinaryExpr*>(&expr)) return inferBinary(*node);
  if (auto* node = dynamic_cast<CallExpr*>(&expr)) return inferCall(*node);
  if (auto* node = dynamic_cast<IndexExpr*>(&expr)) return inferIndex(*node);
  if (auto* node = dynamic_cast<SliceExpr*>(&expr)) return inferSlice(*node);
  if (auto* node = dynamic_cast<FieldExpr*>(&expr)) return inferField(*node);
  if (auto* node = dynamic_cast<SafeNavExpr*>(&expr)) return inferSafeNav(*node);
  if (auto* node = dynamic_cast<ArrayLitExpr*>(&expr)) return inferArrayLiteral(*node);
  if (auto* node = dynamic_cast<MapLitExpr*>(&expr)) return inferMapLiteral(*node);
  if (auto* node = dynamic_cast<SetLitExpr*>(&expr)) return inferSetLiteral(*node);
  if (auto* node = dynamic_cast<TupleExpr*>(&expr)) return inferTuple(*node);
  if (auto* node = dynamic_cast<LambdaExpr*>(&expr)) return inferLambda(*node);
  if (auto* node = dynamic_cast<MatchExpr*>(&expr)) return inferMatch(*node);
  if (auto* node = dynamic_cast<TryExpr*>(&expr)) return inferTry(*node);
  if (auto* node = dynamic_cast<ComprehensionExpr*>(&expr)) return inferComprehension(*node);
  if (auto* node = dynamic_cast<RangeExpr*>(&expr)) {
    TypePtr start = inferExpr(*node->start);
    TypePtr end = inferExpr(*node->end);
    requireInteger(start, node->start->span, "The range start");
    requireInteger(end, node->end->span, "The range end");
    if (!unify(start, end)) {
      sink_.error(node->span, "Range endpoints must have the same integer type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    }
    if (node->step) {
      TypePtr step = inferExpr(*node->step);
      requireInteger(step, node->step->span, "The range step");
      if (!unify(step, start)) {
        reportMismatch(step, start, node->step->span, "in the range step");
      }
    }
    return typeRange(start);
  }
  if (auto* node = dynamic_cast<TernaryExpr*>(&expr)) {
    requireBool(inferExpr(*node->cond), node->cond->span, "The condition of '\?:'");
    TypePtr thenType = inferExpr(*node->thenValue);
    TypePtr elseType = inferExpr(*node->elseValue);
    if (!unify(thenType, elseType)) {
      sink_.error(node->span,
                  "Both branches of '\?:' must have the same type, found " + typeName(thenType) +
                      " and " + typeName(elseType) + ".");
      return typeError();
    }
    if (resolveType(thenType)->kind == TypeKind::Void) {
      sink_.error(node->span, "The branches of '\?:' cannot produce void.");
      return typeError();
    }
    return thenType;
  }
  return typeError();
}

TypePtr Sema::inferStringLiteral(StringLitExpr& expr) {
  for (auto& part : expr.parts) {
    if (!part.isExpr || !part.expr) continue;
    TypePtr type = inferExpr(*part.expr);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(part.expr->span, "Cannot interpolate a void value into a string.");
    }
  }
  return typeString();
}

TypePtr Sema::inferIdent(IdentExpr& expr) {
  if (auto magic = debug_magic::inferMagicIdentifier(expr.name)) return *magic;
  if (VarSym* sym = lookupVar(expr.name)) {
    sym->used = true;
    return sym->type;
  }
  if (expr.name == "None") {
    TypePtr element = freshVar(Constraint::None, expr.span, "the value type of 'None'");
    element->allowUnresolved = true;
    return typeOptional(element);
  }
  if (auto imported = fromBindings_.find(expr.name); imported != fromBindings_.end()) {
    FromBinding& binding = imported->second;
    switch (binding.kind) {
      case FromBinding::Kind::Function:
        if (binding.native) {
          sink_.error(expr.span,
                      "Native function '" + expr.name + "' can only be called directly.",
                      "Wrap it in a lambda to pass it as a value, e.g. x => " + expr.name + "(x).");
          return typeError();
        }
        if (binding.type) return binding.type;
        return typeError();
      case FromBinding::Kind::Struct:
        sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                    "Construct it: " + expr.name + "(...).");
        return typeError();
      case FromBinding::Kind::Trait:
        sink_.error(expr.span, "'" + expr.name + "' is a trait, not a value.",
                    "Use it as a constraint (where " + expr.name +
                        ") or in 'impl " + expr.name + " for Type:'.");
        return typeError();
      case FromBinding::Kind::Value:
        return binding.type ? binding.type : typeError();
    }
  }
  if (auto alias = moduleAliases_.find(expr.name); alias != moduleAliases_.end()) {
    if (alias->second.python) {
      // Python modules are fully dynamic: members are resolved at runtime.
      auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
    }
    return alias->second.native ? typeNativeModule(alias->second.nativeName)
                                : typeModule(alias->second.moduleId);
  }
  {
    const std::string enumQ = qualified(expr.name);
    if (enums_.count(enumQ)) return typeEnum(enumQ);
  }
  if (TypePtr constant = builtinConstantType(expr.name)) return constant;
  const std::string q = qualified(expr.name);
  if (auto fn = functionsByName_.find(q); fn != functionsByName_.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig != signatures_.end()) {
      return typeFunction(sig->second.paramTypes, sig->second.retType);
    }
    return typeError();
  }
  if (structsByName_.count(q)) {
    sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                "Construct it: " + expr.name + "(...).");
    return typeError();
  }
  sink_.errorWithCode(expr.span, "E0001", "Undefined name '" + expr.name + "'.",
              "Declare it by assigning a value, e.g. " + expr.name + " = 0.");
  return typeError();
}

TypePtr Sema::inferUnary(UnaryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == false) {
    // Prefix ++/-- : operand must be a mutable numeric variable.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        sink_.error(expr.opSpan, "Undefined variable '" + ident->name + "'.");
        return typeError();
      }
      sym->used = true;
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == true) {
    // Postfix i++ / i-- : same rules as prefix.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        sink_.error(expr.opSpan, "Undefined variable '" + ident->name + "'.");
        return typeError();
      }
      sym->used = true;
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if (expr.op == "!") {
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Error) return typeError();
    if (resolved->kind == TypeKind::Optional) return resolved->elem;
    sink_.error(expr.opSpan,
                "'!' can only unwrap an optional value, found " + typePhrase(operand) + ".",
                "Only values of type 'T?' can be unwrapped.");
    return typeError();
  }
  if (expr.op == "not") {
    if (!unify(operand, typeBool())) {
      sink_.error(expr.opSpan,
                  "'not' requires a bool operand, found " + typePhrase(operand) + ".");
      return typeError();
    }
    return typeBool();
  }

  TypePtr resolved = resolveType(operand);
  if (expr.op == "~") {
    if (resolved->kind == TypeKind::Int) return operand;
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Integer)) {
      return operand;
    }
    sink_.error(expr.opSpan,
                "Operator '~' cannot be applied to " + typePhrase(operand) + ".",
                "'~' requires an integer operand.");
    return typeError();
  }
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
    return operand;
  }
  if (resolved->kind == TypeKind::Var) {
    if (constrainVar(operand, Constraint::Numeric)) return operand;
  }
  sink_.error(expr.opSpan,
              "Operator '-' cannot be applied to " + typePhrase(operand) + ".");
  return typeError();
}

TypePtr Sema::numericBinaryResult(const std::string& op, TypePtr lhs, TypePtr rhs, Span span) {
      (void)span; (void)op;  // Dynamic: promotion happens at runtime.
      TypePtr l = resolveType(lhs);
      TypePtr r = resolveType(rhs);
      bool ln = isNumericType(l) || l->kind == TypeKind::Var;
      bool rn = isNumericType(r) || r->kind == TypeKind::Var;
      if (ln && rn) {
        if (l->kind == TypeKind::Var) constrainVar(lhs, Constraint::Numeric);
        if (r->kind == TypeKind::Var) constrainVar(rhs, Constraint::Numeric);
        if (l->kind == TypeKind::Float || r->kind == TypeKind::Float) return typeFloat(64);
        return lhs;
      }
      if (l->kind == TypeKind::Var || r->kind == TypeKind::Var) return lhs;
      // Dynamic: any operand types accepted (runtime decides / coerces).
      return lhs;
    }

    TypePtr Sema::inferBinary(BinaryExpr& expr) {
  const std::string& op = expr.op;

  TypePtr lhs = inferExpr(*expr.lhs);
  // `x != None and x.field ...`: narrow Optional for the RHS (ty/pyrefly flow).
  std::vector<NarrowRestore> narrowed;
  if (op == "and") narrowed = applyNarrowing(*expr.lhs, true);
  TypePtr rhs = inferExpr(*expr.rhs);
  restoreNarrowing(narrowed);

  if (op == "and" || op == "or") {
    requireBool(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireBool(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    return typeBool();
  }

  if (op == "??") {
    TypePtr leftResolved = resolveType(lhs);
    if (leftResolved->kind == TypeKind::Error) return rhs;
    if (leftResolved->kind != TypeKind::Optional) {
      sink_.error(expr.opSpan,
                  "The left side of '\?\?' must be optional, found " + typePhrase(lhs) + ".",
                  "Use '\?\?' with a value of type 'T?', e.g. maybeName ?? \"guest\".");
      return typeError();
    }
    if (!unify(leftResolved->elem, rhs)) {
      reportMismatch(rhs, leftResolved->elem, expr.rhs->span, "in the fallback of '\?\?'");
    }
    return leftResolved->elem;
  }

  if (op == "..") {
    requireInteger(lhs, expr.lhs->span, "The range start");
    requireInteger(rhs, expr.rhs->span, "The range end");
    if (!unify(lhs, rhs)) {
      (void)0; if (false) sink_.error(expr.opSpan, "Range endpoints must have the same integer type, found " +
                                   typeName(lhs) + " and " + typeName(rhs) + ".");
      return typeError();
    }
    return typeRange(lhs);
  }

  if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
    requireInteger(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireInteger(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    if (!unify(lhs, rhs)) {
      (void)0; if (false) sink_.error(expr.opSpan, "Operator '" + op + "' requires both operands to have the same "
                               "integer type, found " + typePhrase(lhs) + " and " +
                                   typePhrase(rhs) + ".");
      return typeError();
    }
    return lhs;
  }

  if (op == "in" || op == "not in") {
    TypePtr container = resolveType(rhs);
    switch (container->kind) {
      case TypeKind::Array:
      case TypeKind::Set:
        if (!unify(lhs, container->elem)) {
          reportMismatch(lhs, container->elem, expr.lhs->span, "in the left operand of '" + op + "'");
        }
        break;
      case TypeKind::Map:
        if (!container->params.empty() && !unify(lhs, container->elem)) {
          reportMismatch(lhs, container->elem, expr.lhs->span, "in the map key of '" + op + "'");
        }
        break;
      case TypeKind::String:
        if (!unify(lhs, typeString())) {
          reportMismatch(lhs, typeString(), expr.lhs->span, "in the left operand of '" + op + "'");
        }
        break;
      case TypeKind::Range:
        requireInteger(lhs, expr.lhs->span, "The left operand of '" + op + "'");
        break;
      case TypeKind::Var:
      case TypeKind::Error:
        break;
      default:
        sink_.error(expr.rhs->span,
                    "Right operand of '" + op + "' must be an array, map, set, string, or range, "
                    "found " + typePhrase(container) + ".");
        break;
    }
    return typeBool();
  }

  const bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" ||
                          op == "<=" || op == ">=";

  if (comparison) {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    if (!unify(lhs, rhs)) {
      sink_.error(expr.opSpan,
                  "Cannot compare " + typePhrase(lhsResolved) + " and " +
                      typePhrase(rhsResolved) + ".");
      return typeError();
    }
    TypePtr type = resolveType(lhs);
    switch (type->kind) {
      case TypeKind::Int:
      case TypeKind::Float:
      case TypeKind::String:
      case TypeKind::Char:
      case TypeKind::Var:
        return typeBool();
      case TypeKind::Optional: {
        if (op != "==" && op != "!=") {
          sink_.error(expr.opSpan, "Optionals can only be compared with '==' and '!='.");
          return typeError();
        }
        // `x == None` / `x != None` is valid for any Optional (ty/pyrefly
        // allow `is not None` on any type). Other payload comparisons stay
        // dynamic at runtime.
        (void)type;
        return typeBool();
      }
      case TypeKind::Bool:
        if (op == "==" || op == "!=") return typeBool();
        sink_.error(expr.opSpan, "Only '==' and '!=' can compare bool values.");
        return typeError();
      case TypeKind::Enum:
        if (op == "==" || op == "!=") return typeBool();
        sink_.error(expr.opSpan, "Only '==' and '!=' can compare enum values.");
        return typeError();
      default:
        return typeBool();  // dynamic: runtime compares
    }
  }

  if (op == "+") {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    bool lhsString = lhsResolved->kind == TypeKind::String;
    bool rhsString = rhsResolved->kind == TypeKind::String;
    if (lhsString || rhsString) {
      return typeString();
      TypePtr other = lhsString ? rhs : lhs;
      if (resolveType(other)->kind == TypeKind::Var && unify(other, typeString())) {
        return typeString();
      }
      sink_.error(expr.opSpan,
                  "Cannot add a String and " + typePhrase(other) + ".",
                  "Use string interpolation, e.g. \"Price: {100}\".");
      return typeError();
    }
  }

  return numericBinaryResult(op, lhs, rhs, expr.opSpan);
}

TypePtr Sema::inferCall(CallExpr& expr) {
  if (auto* ident = dynamic_cast<IdentExpr*>(expr.callee.get())) {
    const std::string& name = ident->name;
    if (VarSym* sym = lookupVar(name)) {
      return checkFunctionValueCall(name, sym->type, expr);
    }
    if (auto imported = fromBindings_.find(name); imported != fromBindings_.end()) {
      FromBinding& binding = imported->second;
      if (binding.kind == FromBinding::Kind::Struct) {
        auto sym = structs_.find(binding.structName);
        if (sym == structs_.end()) return typeError();
        return checkStructConstruction(sym->second, expr);
      }
      if (binding.python) {
        for (auto& arg : expr.args) inferExpr(*arg.value);
        auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
      }
      if (binding.kind == FromBinding::Kind::Function && !binding.native) {
        auto sigIt = signatures_.find(binding.fn);
        if (sigIt == signatures_.end()) return typeError();
        return checkFunctionCall(sigIt->second, expr);
      }
      if (binding.kind == FromBinding::Kind::Function && binding.native) {
        return checkBuiltinCall(binding.nativeName, expr);
      }
      if (!binding.type) {
        for (auto& arg : expr.args) inferExpr(*arg.value);
        return typeError();
      }
      return checkFunctionValueCall(name, binding.type, expr);
    }
    if (moduleAliases_.count(name)) {
      sink_.error(ident->span, "'" + name + "' is a module, not a function.",
                  "Call a member instead, e.g. " + name + ".function(...).");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    const std::string q = qualified(name);
    auto structIt = structs_.find(q);
    if (structIt != structs_.end()) {
      expr.calleeStruct = structIt->second.decl;
      expr.calleeResolved = true;
      return checkStructConstruction(structIt->second, expr);
    }
    auto fnIt = functionsByName_.find(q);
    if (fnIt != functionsByName_.end()) {
      // Record the declaration so the interpreter can call it directly instead
      // of re-resolving the name through the scope chain on every call.
      expr.calleeFunction = fnIt->second;
      expr.calleeResolved = true;
      auto sigIt = signatures_.find(fnIt->second);
      if (sigIt == signatures_.end()) return typeError();
      return checkFunctionCall(sigIt->second, expr);
    }
    if (isKnownBuiltin(name)) {
      expr.builtinId = builtinIdForName(name);
      return checkBuiltinCall(name, expr);
    }
    if (isBuiltinConstant(name) || name == "None") {
      sink_.error(ident->span, "'" + name + "' is a constant, not a function.");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    sink_.error(ident->span, "Undefined function '" + name + "'.",
                "Functions must be declared before they are called.");
    return typeError();
  }

  if (auto* field = dynamic_cast<FieldExpr*>(expr.callee.get())) {
    TypePtr objectType = inferExpr(*field->object);
    TypePtr object = resolveType(objectType);
    if (object->kind == TypeKind::Module) {
      ModuleAlias alias;
      alias.native = object->moduleId < 0;
      alias.moduleId = object->moduleId;
      alias.nativeName = object->structName;
      return moduleMemberCall(alias, field->field, expr);
    }
    if (object->kind == TypeKind::Enum) {
      auto enumIt = enums_.find(object->structName);
      if (enumIt != enums_.end() && enumIt->second.algebraic &&
          enumNamespaceUse(field->object, object->structName)) {
        return checkVariantConstruction(enumIt->second, *field, expr);
      }
    }
    return checkMethodCall(objectType, *field, expr);
  }

  TypePtr calleeType = inferExpr(*expr.callee);
  TypePtr resolved = resolveType(calleeType);
  if (resolved->kind == TypeKind::Function) {
    return checkFunctionValueCall("this expression", calleeType, expr);
  }
  if (resolved->kind == TypeKind::Var) {
    // Dynamic value: callability is decided at runtime.
    for (auto& arg : expr.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (resolved->kind != TypeKind::Error) {
    sink_.error(expr.callee->span,
                "Values of type '" + typeName(resolved) + "' are not callable.",
                "Only functions and lambdas can be called.");
  }
  for (auto& arg : expr.args) inferExpr(*arg.value);
  return typeError();
}

TypePtr Sema::checkFunctionValueCall(const std::string& name, TypePtr fnType, CallExpr& call) {
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
  if (!sig.typeParams.empty()) {
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < sig.typeParamVars.size(); ++i) {
      TypePtr fresh = freshVar(Constraint::None,
                              sig.typeParamVars[i]->varSpan,
                              "type parameter '" + sig.typeParams[i] + "'");
      fresh->allowUnresolved = true;
      mapping[sig.typeParamVars[i]] = fresh;
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
  if (call.args.size() != effective.paramTypes.size()) {
    sink_.error(call.span,
                "Function '" + effective.name + "' expects " + std::to_string(effective.paramTypes.size()) +
                    " argument(s), found " + std::to_string(call.args.size()) + ".");
    return typeError();
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    checkArgAgainst(*call.args[i].value, effective.paramTypes[i],
                   "in argument " + std::to_string(i + 1) + " to '" + effective.name + "'");
  }
  return effective.retType;
}

TypePtr Sema::checkStructConstruction(const StructSym& sym, CallExpr& call) {
  std::unordered_map<TypePtr, TypePtr> mapping;
  std::vector<TypePtr> typeArgs;
  for (std::size_t i = 0; i < sym.typeParamVars.size(); ++i) {
    TypePtr fresh = freshVar(Constraint::None, sym.typeParamVars[i]->varSpan,
                            "type parameter '" + sym.typeParams[i] + "'");
    fresh->allowUnresolved = true;
    mapping[sym.typeParamVars[i]] = fresh;
    typeArgs.push_back(fresh);
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
  for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
    TypePtr fresh = freshVar(Constraint::None, info.typeParamVars[i]->varSpan,
                             "type parameter '" + info.typeParams[i] + "'");
    fresh->allowUnresolved = true;
    mapping[info.typeParamVars[i]] = fresh;
    typeArgs.push_back(fresh);
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

TypePtr Sema::checkMethodCall(const TypePtr& objectType, FieldExpr& field, CallExpr& call) {
  TypePtr object = resolveType(objectType);
  const std::string& method = field.field;

  auto rejectNamed = [&]() {
    for (auto& arg : call.args) {
      if (!arg.name.empty()) {
        sink_.error(arg.nameSpan, "Named arguments are not supported in method calls.");
        return true;
      }
    }
    return false;
  };

  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */
    for (auto& arg : call.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    auto methodIt = structIt->second.methods.find(method);
    if (methodIt == structIt->second.methods.end()) {
      for (const auto& fieldEntry : structIt->second.fields) {
        if (fieldEntry.first != method) continue;
        TypePtr fieldType = resolveType(fieldEntry.second);
        if (fieldType->kind == TypeKind::Function) {
          return checkFunctionValueCall(method, fieldEntry.second, call);
        }
        sink_.error(field.fieldSpan,
                    "Field '" + method + "' of '" + object->structName +
                        "' is not callable.",
                    "Only fields with a function type can be called.");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      sink_.error(field.fieldSpan,
                  "Struct '" + object->structName + "' has no method named '" + method + "'.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    if (rejectNamed()) return typeError();
    const FnSig& sig = methodIt->second;
    std::size_t expected = sig.paramTypes.size() - 1;  // exclude self
    if (call.args.size() != expected) {
      sink_.error(call.span,
                  "Method '" + method + "' expects " + std::to_string(expected) +
                      " argument(s), found " + std::to_string(call.args.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      checkArgAgainst(*call.args[i].value, sig.paramTypes[i + 1],
                      "in argument " + std::to_string(i + 1) + " to method '" + method + "'");
    }
    return sig.retType;
  }

  if (object->kind == TypeKind::Array) {
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto elementArg = [&](std::size_t index, const std::string& where) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, object->elem)) {
        reportMismatch(type, object->elem, call.args[index].value->span, where);
      }
    };
    auto indexArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      requireInteger(type, call.args[index].value->span, what);
    };
    auto rejectResize = [&](const char* what) {
      if (object->length < 0) return false;
      sink_.error(field.fieldSpan,
                  "'" + method + "' cannot resize a fixed-size array of type " +
                      typeName(object) + ".",
                  std::string(what) + " Use a dynamically sized array ('[T]') when the length "
                  "must change.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return true;
    };

    if (method == "append") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      if (rejectResize("Appending would grow the array.")) return typeVoid();
      elementArg(0, "in 'append'");
      return typeVoid();
    }
    if (method == "extend") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (array)")) return typeError();
      if (rejectResize("Extending would grow the array.")) return typeVoid();
      TypePtr argType = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(argType);
      if (resolved->kind != TypeKind::Array) {
        sink_.error(call.args[0].value->span,
                    "'extend' expects an array, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, object->elem)) {
        reportMismatch(resolved->elem, object->elem, call.args[0].value->span, "in 'extend'");
      }
      return typeVoid();
    }
    if (method == "insert") {
      if (rejectNamed()) return typeError();
      if (!arity(2, 2, "2 arguments (index, value)")) return typeError();
      if (rejectResize("Inserting would grow the array.")) return typeVoid();
      indexArg(0, "The index passed to 'insert'");
      elementArg(1, "in 'insert'");
      return typeVoid();
    }
    if (method == "pop") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 1, "at most 1 argument (index)")) return typeError();
      if (rejectResize("Popping would shrink the array.")) return object->elem;
      if (call.args.size() == 1) indexArg(0, "The index passed to 'pop'");
      return object->elem;
    }
    if (method == "remove") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      if (rejectResize("Removing would shrink the array.")) return typeVoid();
      elementArg(0, "in 'remove'");
      return typeVoid();
    }
    if (method == "clear" || method == "reverse" || method == "sort") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      if (method == "clear" && rejectResize("Clearing would shrink the array.")) return typeVoid();
      if (method == "sort") {
        TypePtr element = resolveType(object->elem);
        if (element->kind != TypeKind::Var && element->kind != TypeKind::Int &&
            element->kind != TypeKind::Float && element->kind != TypeKind::String) {
          sink_.error(field.fieldSpan,
                      "'sort' requires an array of integers, floats, or strings, found " +
                          typePhrase(object) + ".");
        }
      }
      return typeVoid();
    }
    if (method == "copy") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(object->elem, object->length);
    }
    if (method == "count") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0, "in 'count'");
      return typeInt(64, true);
    }
    if (method == "index") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 3, "1 to 3 arguments (value[, start[, end]])")) return typeError();
      elementArg(0, "in 'index'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        indexArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeInt(64, true);
    }
    if (method == "contains") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0, "in 'contains'");
      return typeBool();
    }
    if (method == "join") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (separator)")) return typeError();
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, typeString())) {
        reportMismatch(argType, typeString(), call.args[0].value->span,
                       "in separator argument to 'join'");
      }
      return typeString();
    }
    if (method == "remove_at") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (index)")) return typeError();
      if (rejectResize("Removing would shrink the array.")) return object->elem;
      indexArg(0, "The index passed to 'remove_at'");
      return object->elem;
    }
    if (method == "first" || method == "last") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeOptional(object->elem);
    }
    sink_.error(field.fieldSpan,
                "Array has no method named '" + method + "'.",
                availableMethodsHelp("array", arrayMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Map) {
    if (rejectNamed()) return typeError();
    TypePtr key = object->elem;
    TypePtr value = object->params.empty() ? typeError() : object->params[0];
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto keyArg = [&](std::size_t index) {
      checkArgAgainst(*call.args[index].value, key, "in the key argument to '" + method + "'");
    };

    if (method == "get") {
      if (!arity(1, 2, "1 or 2 arguments (key[, default])")) return typeError();
      keyArg(0);
      if (call.args.size() == 2) {
        checkArgAgainst(*call.args[1].value, value, "in the default argument to 'get'");
        return value;
      }
      return typeOptional(value);
    }
    if (method == "pop") {
      if (!arity(1, 2, "1 or 2 arguments (key[, default])")) return typeError();
      keyArg(0);
      if (call.args.size() == 2) {
        checkArgAgainst(*call.args[1].value, value, "in the default argument to 'pop'");
      }
      return value;
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (key)")) return typeError();
      keyArg(0);
      return typeBool();
    }
    if (method == "keys") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(key);
    }
    if (method == "values") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(value);
    }
    if (method == "items") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(typeTuple({key, value}));
    }
    if (method == "clear") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeVoid();
    }
    if (method == "copy") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return object;
    }
    if (method == "update") {
      if (!arity(1, 1, "1 argument (map)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Map) {
        sink_.error(call.args[0].value->span,
                    "'update' expects a map, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved, object)) {
        sink_.error(call.args[0].value->span,
                    "'update' requires a map with the same key and value types as " +
                        typeName(object) + ".");
      }
      return typeVoid();
    }
    sink_.error(field.fieldSpan,
                "Map has no method named '" + method + "'.",
                availableMethodsHelp("map", mapMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Set) {
    if (rejectNamed()) return typeError();
    TypePtr element = object->elem;
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto elementArg = [&](std::size_t index) {
      checkArgAgainst(*call.args[index].value, element, "in the argument to '" + method + "'");
    };

    if (method == "add") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeVoid();
    }
    if (method == "remove" || method == "discard") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeVoid();
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeBool();
    }
    if (method == "clear") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeVoid();
    }
    if (method == "copy") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return object;
    }
    if (method == "union" || method == "intersection" || method == "difference") {
      if (!arity(1, 1, "1 argument (set)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Set) {
        sink_.error(call.args[0].value->span,
                    "'" + method + "' expects a set, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, element)) {
        reportMismatch(resolved->elem, element, call.args[0].value->span,
                       "in '" + method + "'");
      }
      return object;
    }
    if (method == "issubset" || method == "issuperset") {
      if (!arity(1, 1, "1 argument (set)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Set) {
        sink_.error(call.args[0].value->span,
                    "'" + method + "' expects a set, found " + typePhrase(resolved) + ".");
      }
      return typeBool();
    }
    sink_.error(field.fieldSpan,
                "Set has no method named '" + method + "'.",
                availableMethodsHelp("set", setMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::String) {
    if (rejectNamed()) return typeError();
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto stringArg = [&](std::size_t index, const std::string& where) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[index].value->span, where);
      }
    };
    auto intArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      requireInteger(type, call.args[index].value->span, what);
    };
    auto boolArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, typeBool())) {
        reportMismatch(type, typeBool(), call.args[index].value->span, what);
      }
    };

    if (method == "capitalize" || method == "casefold" || method == "lower" ||
        method == "upper" || method == "swapcase" || method == "title") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeString();
    }
    if (method == "isalnum" || method == "isalpha" || method == "isascii" ||
        method == "isdecimal" || method == "isdigit" || method == "isidentifier" ||
        method == "islower" || method == "isnumeric" || method == "isprintable" ||
        method == "isspace" || method == "istitle" || method == "isupper") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeBool();
    }
    if (method == "center" || method == "ljust" || method == "rjust") {
      if (!arity(1, 2, "1 or 2 arguments (width[, fillchar])")) return typeError();
      intArg(0, "The width argument");
      if (call.args.size() == 2) stringArg(1, "in the fill character");
      return typeString();
    }
    if (method == "count" || method == "find" || method == "rfind" || method == "index" ||
        method == "rindex") {
      if (!arity(1, 3, "1 to 3 arguments (sub[, start[, end]])")) return typeError();
      stringArg(0, "in argument 1 to '" + method + "'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        intArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeInt(64, true);
    }
    if (method == "startswith" || method == "endswith") {
      if (!arity(1, 3, "1 to 3 arguments (affix[, start[, end]])")) return typeError();
      stringArg(0, "in argument 1 to '" + method + "'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        intArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeBool();
    }
    if (method == "expandtabs") {
      if (!arity(0, 1, "at most 1 argument (tabsize)")) return typeError();
      if (call.args.size() == 1) intArg(0, "The tabsize argument");
      return typeString();
    }
    if (method == "strip" || method == "lstrip" || method == "rstrip") {
      if (!arity(0, 1, "at most 1 argument (characters)")) return typeError();
      if (call.args.size() == 1) stringArg(0, "in the characters argument");
      return typeString();
    }
    if (method == "partition" || method == "rpartition") {
      if (!arity(1, 1, "1 argument (separator)")) return typeError();
      stringArg(0, "in the separator argument");
      return typeTuple({typeString(), typeString(), typeString()});
    }
    if (method == "removeprefix" || method == "removesuffix") {
      if (!arity(1, 1, "1 argument (affix)")) return typeError();
      stringArg(0, "in the affix argument");
      return typeString();
    }
    if (method == "replace") {
      if (!arity(2, 3, "2 or 3 arguments (old, new[, count])")) return typeError();
      stringArg(0, "in the 'old' argument");
      stringArg(1, "in the 'new' argument");
      if (call.args.size() == 3) intArg(2, "The count passed to 'replace'");
      return typeString();
    }
    if (method == "split" || method == "rsplit") {
      if (!arity(0, 2, "at most 2 arguments (separator[, maxsplit])")) return typeError();
      if (call.args.size() == 1) {
        TypePtr first = inferExpr(*call.args[0].value);
        if (!unify(first, typeString())) {
          requireInteger(first, call.args[0].value->span, "The maxsplit argument");
        }
      } else if (call.args.size() == 2) {
        stringArg(0, "in the separator argument");
        intArg(1, "The maxsplit argument");
      }
      return typeArray(typeString());
    }
    if (method == "splitlines") {
      if (!arity(0, 1, "at most 1 argument (keepends)")) return typeError();
      if (call.args.size() == 1) boolArg(0, "The 'keepends' argument");
      return typeArray(typeString());
    }
    if (method == "join") {
      if (!arity(1, 1, "1 argument (array)")) return typeError();
      TypePtr argType = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(argType);
      if (resolved->kind != TypeKind::Array) {
        sink_.error(call.args[0].value->span,
                    "'join' expects an array of strings, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, typeString())) {
        reportMismatch(resolved->elem, typeString(), call.args[0].value->span,
                       "in the array passed to 'join'");
      }
      return typeString();
    }
    if (method == "zfill") {
      if (!arity(1, 1, "1 argument (width)")) return typeError();
      intArg(0, "The width argument");
      return typeString();
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (substring)")) return typeError();
      stringArg(0, "in the 'contains' argument");
      return typeBool();
    }
    if (method == "repeat") {
      if (!arity(1, 1, "1 argument (count)")) return typeError();
      intArg(0, "The count passed to 'repeat'");
      return typeString();
    }
    if (method == "chars") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(typeChar());
    }
    sink_.error(field.fieldSpan,
                "String has no method named '" + method + "'.",
                availableMethodsHelp("string", stringMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  sink_.error(field.fieldSpan,
              "Values of type '" + typeName(object) + "' have no methods.",
              "Methods are defined on structs and on arrays and strings.");
  for (auto& arg : call.args) inferExpr(*arg.value);
  return typeError();
}

TypePtr Sema::checkIcCall(CallExpr& call) {
  // Mirrors litprinter/ic(ecream): ic() returns void/None-like output only,
  // ic(x) returns x, ic(x, y, ...) returns a tuple of the inspected values.
  // Named arguments are rejected (reserved for future options).
  std::vector<TypePtr> types;
  types.reserve(call.args.size());
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "'ic' does not accept named arguments.");
      inferExpr(*arg.value);
      continue;
    }
    TypePtr type = inferExpr(*arg.value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(arg.value->span, "Cannot inspect a void value with 'ic'.");
      type = typeError();
    }
    types.push_back(type);
  }

  // Capture the display metadata while source spans are available: the
  // "file:line in fn()" context and one source-text label per argument.
  call.icContext = icContextString(call.span);
  call.icArgLabels.clear();
  call.icArgLabels.reserve(call.args.size());
  std::unordered_map<int, std::unique_ptr<tooling::SourceText>> sources;
  for (auto& arg : call.args) {
    const SourceFile* file = sink_.file(arg.value->span.fileId);
    std::string label;
    if (file && arg.value->span.valid()) {
      auto& slot = sources[arg.value->span.fileId];
      if (!slot) slot = std::make_unique<tooling::SourceText>(file->text());
      label = collapseIcWhitespace(slot->slice(arg.value->span));
    }
    call.icArgLabels.push_back(std::move(label));
  }

  if (types.empty()) return typeVoid();
  if (types.size() == 1) return types[0];
  return typeTuple(std::move(types));
}

std::string Sema::icContextString(Span span) const {
  return debug_magic::functionDisplayName(currentFunction_ ? currentFunction_->name : std::string()) == "<module>"
             ? debug_magic::sourceFileName(sink_, span) + ":" + std::to_string(span.line)
             : debug_magic::sourceFileName(sink_, span).substr(
                       debug_magic::sourceFileName(sink_, span).find_last_of("/\\") == std::string::npos
                           ? 0
                           : debug_magic::sourceFileName(sink_, span).find_last_of("/\\") + 1) +
                   ":" + std::to_string(span.line) + " in " +
                   debug_magic::functionDisplayName(currentFunction_->name) + "()";
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
  auto inferArgs = [&]() {
    std::vector<TypePtr> types;
    types.reserve(call.args.size());
    for (auto& arg : call.args) types.push_back(inferExpr(*arg.value));
    return types;
  };
  auto arityError = [&](std::size_t expected) {
    sink_.error(call.span, "'" + name + "' expects " + std::to_string(expected) +
                               " argument(s), found " + std::to_string(call.args.size()) + ".");
  };
  if (name == "__file__" || name == "__source_file__" || name == "__func__" ||
      name == "__function__" || name == "__debug_time__" || name == "__debug_context__") {
    if (!call.args.empty()) arityError(0);
    inferArgs();
    if (name == "__debug_time__") return typeString();
    return typeString();
  }
  if (name == "__line__" || name == "__source_line__") {
    if (!call.args.empty()) arityError(0);
    inferArgs();
    return typeInt(64, true);
  }
  if (name == "__repr__" || name == "__debug_repr__") {
    if (call.args.size() != 1) arityError(1);
    inferArgs();
    return typeString();
  }
  if (name == "__ic_format__") {
    if (call.args.size() < 1 || call.args.size() > 3) {
      sink_.error(call.span, "'__ic_format__' expects context/label/value (1 to 3) arguments, found " +
                                 std::to_string(call.args.size()) + ".");
    }
    inferArgs();
    return typeString();
  }
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Builtin functions do not accept named arguments.");
      return typeError();
    }
  }

  // Structured values (Json) are opaque; any non-void value may be serialized.
  auto inferAny = [&](std::size_t index, const std::string& context) {
    TypePtr type = inferExpr(*call.args[index].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[index].value->span,
                  "Cannot pass a void value " + context + ".");
    }
    return type;
  };

  if (name == "open") {
    if (call.args.size() < 1 || call.args.size() > 2) {
      sink_.error(call.span, "'open' expects 1 or 2 argument(s), found " +
                                 std::to_string(call.args.size()) + ".");
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(0);
      inferArgs();
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

  if (name == "len") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::String || resolved->kind == TypeKind::Array ||
        resolved->kind == TypeKind::Map || resolved->kind == TypeKind::Set) {
      return typeInt(64, true);
    }
    if (resolved->kind == TypeKind::Var) {
      /* dynamic: inferred at runtime */ (void)0;
    } else {
      sink_.error(call.args[0].value->span,
                  "'len' cannot be applied to " + typePhrase(resolved) + ".",
                  "'len' works on strings, arrays, maps, and sets.");
    }
    return typeInt(64, true);
  }

  if (name == "set") {
    if (call.args.size() > 1) {
      arityError(1);
      inferArgs();
      return typeSet(typeError());
    }
    if (call.args.empty()) {
      return typeSet(freshVar(Constraint::None, call.span, "the element type of an empty set"));
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Set) return type;
    if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Range) {
      return typeSet(resolved->elem);
    }
    if (resolved->kind == TypeKind::String) return typeSet(typeChar());
    if (resolved->kind == TypeKind::Var) {
      return typeSet(freshVar(Constraint::None, call.span, "the element type of 'set'"));
    }
    sink_.error(call.args[0].value->span,
                "'set' requires an array, range, set, or string, found " + typePhrase(resolved) +
                    ".");
    return typeSet(typeError());
  }

  if (name == "unsafe_get" || name == "unsafe_set") {
    const std::size_t expected = name == "unsafe_get" ? 2 : 3;
    if (call.args.size() != expected) {
      arityError(expected);
      inferArgs();
      return typeError();
    }
    TypePtr sequence = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(sequence);
    if (resolved->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires an array, found " + typePhrase(resolved) + ".",
                  "Unchecked access is supported only for arrays and array views.");
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return typeError();
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to '" + name + "'");
    if (name == "unsafe_get") return resolved->elem;
    checkArgAgainst(*call.args[2].value, resolved->elem, "in 'unsafe_set'");
    return typeVoid();
  }

  if (name == "arena_new" || name == "arena_pool" || name == "arena_region") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeAllocator();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The allocator capacity");
    return typeAllocator();
  }

  if (name == "arena_alloc" || name == "arena_release") {
    const bool allocating = name == "arena_alloc";
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return allocating ? typeInt(64, true) : typeVoid();
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the first argument to '" + name + "'");
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   allocating ? "The allocation size" : "The allocation handle");
    return allocating ? typeInt(64, true) : typeVoid();
  }

  if (name == "arena_reset") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the argument to 'arena_reset'");
    }
    return typeVoid();
  }

  if (name == "arena_used" || name == "arena_total") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the argument to '" + name + "'");
    }
    return typeInt(64, true);
  }

  if (name == "move") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot move a void value.");
    }
    return type;
  }

  if (name == "copy") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot copy a void value.");
      return typeError();
    }
    if (resolved->kind == TypeKind::Var) {
      /* dynamic: inferred at runtime */ (void)0;
    }
    return type;
  }

  if (name == "borrow" || name == "borrow_mut") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    if (call.args[0].value->kind != ExprKind::Ident) {
      sink_.error(call.args[0].value->span,
                  "The argument of '" + name + "' must be a variable.",
                  "Borrows are tied to a binding so the analyzer can track its lifetime.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Void || resolved->kind == TypeKind::Int ||
        resolved->kind == TypeKind::Float || resolved->kind == TypeKind::Bool ||
        resolved->kind == TypeKind::Char) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a reference-like value, found " + typePhrase(resolved) +
                      ".",
                  "Borrowing applies to arrays, maps, sets, strings, and structs.");
      return typeError();
    }
    return type;
  }

  if (name == "unsafe_address_of") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typePointer(typeError());
    }
    TypePtr sequence = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(sequence);
    if (resolved->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_address_of' requires an array, found " + typePhrase(resolved) + ".");
      inferExpr(*call.args[1].value);
      return typePointer(typeError());
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to 'unsafe_address_of'");
    return typePointer(resolved->elem);
  }

  if (name == "unsafe_offset") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typePointer(typeError());
    }
    TypePtr pointer = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(pointer);
    if (resolved->kind != TypeKind::Pointer) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_offset' requires a pointer, found " + typePhrase(resolved) + ".");
      inferExpr(*call.args[1].value);
      return typePointer(typeError());
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The offset passed to 'unsafe_offset'");
    return pointer;
  }

  if (name == "unsafe_load" || name == "unsafe_store") {
    const bool writing = name == "unsafe_store";
    if (call.args.size() != (writing ? 2u : 1u)) {
      arityError(writing ? 2 : 1);
      inferArgs();
      return writing ? typeVoid() : typeError();
    }
    TypePtr pointer = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(pointer);
    if (resolved->kind != TypeKind::Pointer) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a pointer, found " + typePhrase(resolved) + ".");
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return writing ? typeVoid() : typeError();
    }
    if (!writing) return resolved->elem;
    checkArgAgainst(*call.args[1].value, resolved->elem, "in 'unsafe_store'");
    return typeVoid();
  }

  if (name == "str") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeString();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot convert a void value to a string.");
    }
    return typeString();
  }

  if (name == "int" || name == "float") {
    TypePtr result = name == "int" ? typeInt(64, true) : typeFloat(64);
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return result;
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Var) {
      if (!constrainVar(type, Constraint::Numeric)) {
        /* dynamic: inferred at runtime */ (void)0;
      }
    } else if (resolved->kind != TypeKind::Int && resolved->kind != TypeKind::Float &&
               resolved->kind != TypeKind::Bool && resolved->kind != TypeKind::String) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a number, bool, or numeric string, found " +
                      typePhrase(resolved) + ".");
    }
    return result;
  }

  if (name == "abs") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    return requireNumeric(type, call.args[0].value->span, "The argument of 'abs'");
  }

  if (name == "min" || name == "max") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeError();
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    if (!unify(a, b)) {
      sink_.error(call.span, "'" + name + "' requires arguments of the same numeric type.");
      return typeError();
    }
    return requireNumeric(a, call.span, "The arguments of '" + name + "'");
  }

  if (name == "pow") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeFloat(64);
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    requireNumeric(a, call.args[0].value->span, "The base of 'pow'");
    requireNumeric(b, call.args[1].value->span, "The exponent of 'pow'");
    return typeFloat(64);
  }

  if (name == "flush") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeVoid();
  }

  if (name == "read_file") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
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
      arityError(2);
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(2);
      inferArgs();
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
      arityError(0);
      inferArgs();
    }
    return typeString();
  }

  if (name == "now") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeInt(64, true);
  }

  if (name == "sleep") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The duration passed to 'sleep'");
    return typeVoid();
  }

  if (name == "panic") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(2);
      inferArgs();
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

  if (name == "Some") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeOptional(typeError());
    }
    TypePtr value = inferExpr(*call.args[0].value);
    if (resolveType(value)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot wrap a void value in 'Some'.");
    }
    return typeOptional(value);
  }

  if (name == "Ok") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
      e->allowUnresolved = true;
      return typeResult(typeError(), e);
    }
    TypePtr value = inferExpr(*call.args[0].value);
    TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
    e->allowUnresolved = true;
    return typeResult(value, e);
  }

  if (name == "Err") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
      t->allowUnresolved = true;
      return typeResult(t, typeError());
    }
    TypePtr error = inferExpr(*call.args[0].value);
    TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
    t->allowUnresolved = true;
    return typeResult(t, error);
  }

  if (name == "atan2") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeFloat(64);
    }
    requireNumeric(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The y argument of 'atan2'");
    requireNumeric(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The x argument of 'atan2'");
    return typeFloat(64);
  }

  if (name == "random") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeFloat(64);
  }

  if (name == "random_int") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The low bound of 'random_int'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The high bound of 'random_int'");
    return typeInt(64, true);
  }

  if (name == "map" || name == "filter" || name == "any" || name == "all") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
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
      arityError(3);
      inferArgs();
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
      arityError(1);
      inferArgs();
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
      arityError(2);
      inferArgs();
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
  if (name == "json_parse") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeJson();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in the text passed to 'json.parse'");
    return typeJson();
  }
  if (name == "json_stringify" || name == "json_pretty") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeString();
    }
    inferAny(0, "to 'json." + std::string(name == "json_stringify" ? "stringify" : "pretty") + "'");
    return typeString();
  }
  if (name == "json_is_valid") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeBool();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in 'json.is_valid'");
    return typeBool();
  }
  if (name == "json_get" || name == "json_has") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return name == "json_get" ? typeOptional(typeJson()) : typeBool();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json'");
    checkArgAgainst(*call.args[1].value, typeString(), "in the key passed to 'json'");
    return name == "json_get" ? typeOptional(typeJson()) : typeBool();
  }
  if (name == "json_at") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeOptional(typeJson());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json.at'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to 'json.at'");
    return typeOptional(typeJson());
  }
  if (name == "json_len") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.len'");
    return typeInt(64, true);
  }
  if (name == "json_keys") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.keys'");
    return typeArray(typeString());
  }
  if (name == "json_type") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeString();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.type'");
    return typeString();
  }
  if (name == "json_as_str" || name == "json_as_int" || name == "json_as_float" ||
      name == "json_as_bool" || name == "json_as_array" || name == "json_as_map") {
    TypePtr inner = typeJson();
    if (name == "json_as_str") inner = typeString();
    if (name == "json_as_int") inner = typeInt(64, true);
    if (name == "json_as_float") inner = typeFloat(64);
    if (name == "json_as_bool") inner = typeBool();
    if (name == "json_as_array") inner = typeArray(typeJson());
    if (name == "json_as_map") inner = typeMap(typeString(), typeJson());
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeOptional(inner);
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json'");
    return typeOptional(inner);
  }

  // -------------------------------------------------------- binary module
  if (name == "binary_encode") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeArray(typeInt(64, true));
    }
    inferAny(0, "to 'binary.encode'");
    return typeArray(typeInt(64, true));
  }
  if (name == "binary_decode") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeOptional(typeJson());
    }
    checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                    "in the bytes passed to 'binary.decode'");
    return typeOptional(typeJson());
  }

  // ------------------------------------------------------- testing module
  if (name == "testing_seed") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The seed passed to 'testing.seed'");
    return typeVoid();
  }
  if (name == "testing_for_all_int") {
    if (call.args.size() != 4) {
      arityError(4);
      inferArgs();
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
      arityError(4);
      inferArgs();
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
      arityError(2);
      inferArgs();
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
      arityError(3);
      inferArgs();
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
      arityError(2);
      inferArgs();
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
  if (name == "log_set_level") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in 'log.set_level'");
    return typeVoid();
  }
  if (name == "log_set_json") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeBool(), "in 'log.set_json'");
    return typeVoid();
  }
  if (name == "log_debug" || name == "log_info" || name == "log_warn" ||
      name == "log_error") {
    if (call.args.empty()) {
      sink_.error(call.span, "'" + name + "' expects a message.");
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the log message");
    if (call.args.size() == 2) {
      inferAny(1, "as log fields");
    } else if (call.args.size() > 2) {
      if ((call.args.size() - 1) % 2 != 0) {
        sink_.error(call.span,
                    "'" + name +
                        "' expects a message followed by field/value pairs or a fields map.");
        inferArgs();
        return typeVoid();
      }
      for (std::size_t i = 1; i + 1 < call.args.size(); i += 2) {
        checkArgAgainst(*call.args[i].value, typeString(), "as a log field name");
        inferAny(i + 1, "as a log field value");
      }
    }
    return typeVoid();
  }

  // ------------------------------------------------------- metrics module
  if (name == "metrics_counter" || name == "metrics_gauge" || name == "metrics_histogram") {
    if (call.args.size() < 2 || (call.args.size() > 3 && (call.args.size() - 2) % 2 != 0)) {
      sink_.error(call.span,
                  "'" + name +
                      "' expects a name, a value, and optional labels (a map or key/value pairs).");
      inferArgs();
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the metric name");
    if (name == "metrics_counter") {
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The increment passed to 'metrics.counter'");
    } else {
      requireNumeric(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The value passed to '" + name + "'");
    }
    if (call.args.size() == 3) {
      inferAny(2, "as metric labels");
    } else if (call.args.size() > 3) {
      for (std::size_t i = 2; i + 1 < call.args.size(); i += 2) {
        checkArgAgainst(*call.args[i].value, typeString(), "as a metric label name");
        inferAny(i + 1, "as a metric label value");
      }
    }
    return typeVoid();
  }
  if (name == "metrics_render") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeString();
  }
  if (name == "metrics_reset") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeVoid();
  }

  // --------------------------------------------------------- trace module
  if (name == "trace_start") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the span name");
    return typeInt(64, true);
  }
  if (name == "trace_end") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.end'");
    return typeVoid();
  }
  if (name == "trace_current") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeInt(64, true);
  }
  if (name == "trace_set_attribute") {
    if (call.args.size() != 3) {
      arityError(3);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.set_attribute'");
    checkArgAgainst(*call.args[1].value, typeString(), "as the attribute name");
    inferAny(2, "as the attribute value");
    return typeVoid();
  }
  if (name == "trace_add_event") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.add_event'");
    checkArgAgainst(*call.args[1].value, typeString(), "as the event name");
    return typeVoid();
  }
  if (name == "trace_inject") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeString();
  }
  if (name == "trace_extract") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the trace context");
    return typeInt(64, true);
  }
  if (name == "trace_spans") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeArray(typeJson());
  }
  if (name == "trace_reset") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeVoid();
  }

  // ----------------------------------------------------------- net module
  if (name == "net_resolve") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the host name");
    return typeArray(typeString());
  }
  if (name == "net_tcp_listen" || name == "net_tcp_connect" || name == "net_tls_connect") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the host name");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The port number");
    return typeInt(64, true);
  }
  if (name == "net_accept" || name == "net_local_port" || name == "net_close") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return name == "net_close" ? typeVoid() : typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    return name == "net_close" ? typeVoid() : typeInt(64, true);
  }
  if (name == "net_send" || name == "net_recv") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return name == "net_recv" ? typeString() : typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    if (name == "net_send") {
      checkArgAgainst(*call.args[1].value, typeString(), "as the data to send");
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The maximum byte count");
    return typeString();
  }
  if (name == "net_set_timeout") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The timeout in milliseconds");
    return typeVoid();
  }
  if (name == "net_set_nonblocking") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeBool(), "as the non-blocking flag");
    return typeVoid();
  }
  if (name == "net_poll") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeArray(typeInt(64, true));
    }
    checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                    "as the socket handles");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The poll timeout");
    return typeArray(typeInt(64, true));
  }
  if (name == "net_udp_socket") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeInt(64, true);
  }
  if (name == "net_udp_bind") {
    if (call.args.size() != 3) {
      arityError(3);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the bind address");
    requireInteger(inferExpr(*call.args[2].value), call.args[2].value->span,
                   "The port number");
    return typeVoid();
  }
  if (name == "net_udp_send") {
    if (call.args.size() != 4) {
      arityError(4);
      inferArgs();
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the destination host");
    requireInteger(inferExpr(*call.args[2].value), call.args[2].value->span,
                   "The destination port");
    checkArgAgainst(*call.args[3].value, typeString(), "as the datagram");
    return typeInt(64, true);
  }
  if (name == "net_udp_recv") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeTuple({typeString(), typeString(), typeInt(64, true)});
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The maximum byte count");
    return typeTuple({typeString(), typeString(), typeInt(64, true)});
  }
  if (name == "net_tls_available") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeBool();
  }

  // ---------------------------------------------------------- http module
  if (name == "http_get" || name == "http_post") {
    const std::size_t expected = name == "http_get" ? 1 : 2;
    TypePtr response = typeTuple(
        {typeInt(64, true), typeMap(typeString(), typeString()), typeString()});
    if (call.args.size() != expected) {
      arityError(expected);
      inferArgs();
      return response;
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the URL");
    if (name == "http_post") checkArgAgainst(*call.args[1].value, typeString(), "as the body");
    return response;
  }
  if (name == "http_request") {
    TypePtr response = typeTuple(
        {typeInt(64, true), typeMap(typeString(), typeString()), typeString()});
    if (call.args.size() != 5) {
      arityError(5);
      inferArgs();
      return response;
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the HTTP method");
    checkArgAgainst(*call.args[2].value, typeString(), "as the request path");
    checkArgAgainst(*call.args[3].value, typeMap(typeString(), typeString()),
                    "as the request headers");
    checkArgAgainst(*call.args[4].value, typeString(), "as the request body");
    return response;
  }
  if (name == "http_serve" || name == "http_serve_once") {
    TypePtr handler = typeFunction(
        {typeString(), typeString(), typeMap(typeString(), typeString()), typeString()},
        typeTuple({typeInt(64, true), typeMap(typeString(), typeString()), typeString()}));
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return name == "http_serve" ? typeVoid() : typeBool();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The listening socket handle");
    checkArgAgainst(*call.args[1].value, handler, "in the HTTP handler");
    return name == "http_serve" ? typeVoid() : typeBool();
  }

  // ------------------------------------------------------------- kernels
  // CPU kernel builtins behind the `kernels` package. The interpreter validates
  // shapes at run time; here we only fix the result shape so wrappers can stay
  // thin and numeric-generic.
  if (name.rfind("kernels_", 0) == 0) {
    inferArgs();
    const std::string suffix = name.substr(8);
    auto numericElem = [&]() {
      return freshVar(Constraint::Numeric, call.span,
                      "the numeric element type of '" + name + "'");
    };
    if (suffix == "reduce_argmax" || suffix == "reduce_argmin") {
      return typeInt(64, true);
    }
    if (suffix == "reduce_mean" || suffix == "reduce_var" || suffix == "reduce_std" ||
        suffix == "reduce_norm") {
      return typeFloat(64);
    }
    if (suffix == "dot" || suffix == "reduce_sum" || suffix == "reduce_max" ||
        suffix == "reduce_min" || suffix == "reduce_prod" || suffix == "trace") {
      return numericElem();
    }
    if (suffix == "softmax" || suffix == "sigmoid" || suffix == "tanh" ||
        suffix == "normalize" || suffix == "bias_relu" || suffix == "gelu" ||
        suffix == "silu" || suffix == "elu" || suffix == "leaky_relu" ||
        suffix == "softplus" || suffix == "mish" || suffix == "rms_norm" ||
        suffix == "layer_norm" || suffix == "attention_scores" || suffix == "attention" ||
        suffix == "avg_pool1d" || suffix == "dequantize_int8") {
      return typeArray(typeFloat(64));
    }
    if (suffix == "softmax_rows" || suffix == "layer_norm_rows") {
      return typeArray(typeArray(typeFloat(64)));
    }
    if (suffix == "matmul" || suffix == "transpose" || suffix == "outer" ||
        suffix == "eye") {
      return typeArray(typeArray(numericElem()));
    }
    if (suffix == "zeros" || suffix == "ones" || suffix == "full" || suffix == "reshape") {
      return typeArray(typeError());
    }
    if (suffix == "quantize_int8") return typeArray(typeInt(64, true));
    return typeArray(numericElem());
  }

  if (isMathBuiltin(name)) {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeFloat(64);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    return typeFloat(64);
  }

  if (isConversionBuiltin(name)) {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return builtinTypeByName(name);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    return builtinTypeByName(name);
  }

  // --------------------------------------------- LLVM-style intrinsics
  // Bit intrinsics (llvm.cttz/ctlz/ctpop/bswap/fshl-family analogs), float
  // intrinsics (llvm.fma/fabs/copysign), integer min/max, *.with.overflow
  // arithmetic, and llvm.expect-style branch hints.
  {
    auto intTypeOf = [&](std::size_t index, const std::string& context) {
      TypePtr type = inferExpr(*call.args[index].value);
      TypePtr resolved = resolveType(type);
      if (resolved->kind == TypeKind::Int) return resolved;
      if (resolved->kind == TypeKind::Var) return typeInt(64, true);
      sink_.error(call.args[index].value->span,
                  "'" + name + "' expects an integer " + context + ", found " +
                      typePhrase(resolved) + ".");
      return typeInt(64, true);
    };
    static const char* bitUnary[] = {"ctz", "clz", "popcount", "bswap"};
    static const char* bitBinary[] = {"rotl", "rotr"};
    static const char* intMinMax[] = {"smin", "smax", "umin", "umax"};
    static const char* ovf[] = {"sadd_overflow", "ssub_overflow", "smul_overflow",
                                "uadd_overflow", "usub_overflow", "umul_overflow"};
    auto isOne = [&](const char* const* names, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i)
        if (name == names[i]) return true;
      return false;
    };
    if (isOne(bitUnary, 4) || isOne(bitBinary, 2) || isOne(intMinMax, 4) ||
        isOne(ovf, 6)) {
      const std::size_t expected = isOne(bitUnary, 4) ? 1 : 2;
      if (call.args.size() != expected) {
        arityError(expected);
        inferArgs();
        return isOne(ovf, 6) ? typeTuple({typeInt(64, true), typeBool()})
                             : typeInt(64, true);
      }
      TypePtr first = intTypeOf(0, "argument");
      if (expected == 2) (void)intTypeOf(1, "argument");
      if (isOne(ovf, 6)) return typeTuple({first, typeBool()});
      return first;
    }
    if (name == "fma" || name == "copysign") {
      const std::size_t expected = name == "fma" ? 3 : 2;
      if (call.args.size() != expected) {
        arityError(expected);
        inferArgs();
        return typeFloat(64);
      }
      for (std::size_t i = 0; i < expected; ++i) {
        TypePtr type = inferExpr(*call.args[i].value);
        requireNumeric(type, call.args[i].value->span, "The argument of '" + name + "'");
      }
      return typeFloat(64);
    }
    if (name == "fabs") {
      if (call.args.size() != 1) {
        arityError(1);
        inferArgs();
        return typeFloat(64);
      }
      TypePtr type = inferExpr(*call.args[0].value);
      requireNumeric(type, call.args[0].value->span, "The argument of 'fabs'");
      return typeFloat(64);
    }
    if (name == "expect" || name == "likely" || name == "unlikely") {
      const std::size_t maxArgs = name == "expect" ? 2 : 1;
      if (call.args.empty() || call.args.size() > maxArgs) {
        sink_.error(call.span, "'" + name + "' expects 1 to " +
                               std::to_string(maxArgs) + " argument(s), found " +
                               std::to_string(call.args.size()) + ".");
        inferArgs();
        return typeError();
      }
      TypePtr type = inferExpr(*call.args[0].value);
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return type;
    }
  }

  // -------------------------------------------------- terminal/env/repl
  // Interactive-REPL primitives; argument shapes are validated by the
  // interpreter, so the checker only fixes the result type.
  if (name == "terminal_isatty" || name == "terminal_raw_mode") {
    if (!call.args.empty()) inferArgs();
    return typeBool();
  }
  if (name == "terminal_read_char" || name == "terminal_read_char_ms") {
    if (!call.args.empty()) inferArgs();
    return typeOptional(typeString());
  }
  if (name == "terminal_write" || name == "terminal_flush") {
    if (!call.args.empty()) inferArgs();
    return typeVoid();
  }
  if (name == "terminal_size") {
    if (!call.args.empty()) inferArgs();
    return typeArray(typeInt(64, true));
  }
  if (name == "terminal_home" || name == "env_get") {
    if (!call.args.empty()) inferArgs();
    return typeString();
  }
  if (name == "repl_eval") {
    if (!call.args.empty()) inferArgs();
    return typeInt(64, true);
  }
  if (name == "repl_quiet") {
    if (!call.args.empty()) inferArgs();
    return typeVoid();
  }

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

TypePtr Sema::inferIndex(IndexExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr indexType = inferExpr(*expr.index);

  TypePtr object = resolveType(objectType);
  if (object->kind == TypeKind::Map && !object->params.empty()) {
    if (!unify(indexType, object->elem)) {
      reportMismatch(indexType, object->elem, expr.index->span, "in the map key");
    }
    return object->params[0];
  }
  if (object->kind == TypeKind::Map) return typeError();
  // Dynamic: index type checked at runtime (maps use any hashable key).
  if (object->kind == TypeKind::Array) return object->elem;
  if (object->kind == TypeKind::String) return typeChar();
  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (object->kind == TypeKind::Set) {
    sink_.error(expr.object->span, "A set cannot be indexed.",
                "Sets are unordered; use 'in' to test membership.");
    return typeError();
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be indexed.");
  return typeError();
}

TypePtr Sema::inferSlice(SliceExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  if (expr.start) {
    TypePtr startType = inferExpr(*expr.start);
    requireInteger(startType, expr.start->span, "The slice start");
  }
  if (expr.end) {
    TypePtr endType = inferExpr(*expr.end);
    requireInteger(endType, expr.end->span, "The slice end");
  }
  if (expr.step) {
    TypePtr stepType = inferExpr(*expr.step);
    requireInteger(stepType, expr.step->span, "The slice step");
  }

  TypePtr object = resolveType(objectType);
  // A slice is a view over the source storage; its length is only known at
  // run time, so the static type is the dynamic array form.
  if (object->kind == TypeKind::Array) return typeArray(object->elem);
  if (object->kind == TypeKind::String) return typeString();
  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be sliced.");
  return typeError();
}

TypePtr Sema::inferField(FieldExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Module) {
    ModuleAlias alias;
    alias.native = object->moduleId < 0;
    alias.moduleId = object->moduleId;
    alias.nativeName = object->structName;
    return moduleMemberType(alias, expr.field, expr.fieldSpan, true);
  }

  if (object->kind == TypeKind::Enum) {
    auto enumIt = enums_.find(object->structName);
    if (enumIt == enums_.end()) return typeError();
    EnumInfo& info = enumIt->second;
    if (info.builtinKind) {
      sink_.error(expr.fieldSpan,
                  "'" + shortName(object->structName) + "' is a prelude type; its values are "
                  "built-in.",
                  "Write Some(value), None, Ok(value), or Err(error) without a module prefix.");
      return typeError();
    }
    if (!info.algebraic) {
      if (info.values.count(expr.field)) return typeInt(32, true);
      sink_.error(expr.fieldSpan,
                  "Enum '" + shortName(object->structName) + "' has no variant named '" +
                      expr.field + "'.");
      return typeError();
    }
    if (enumNamespaceUse(expr.object, object->structName)) {
      const VariantInfo* variant = info.findVariant(expr.field);
      if (!variant) {
        sink_.error(expr.fieldSpan,
                    "Enum '" + shortName(object->structName) + "' has no variant named '" +
                        expr.field + "'.");
        return typeError();
      }
      if (!variant->fields.empty()) {
        sink_.error(expr.fieldSpan,
                    "Variant '" + shortName(info.name) + "." + variant->name +
                        "' carries a payload and must be constructed.",
                    "Call it: " + shortName(info.name) + "." + variant->name + "(...).");
        return typeError();
      }
      TypePtr t = typeEnum(info.name);
      for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
        TypePtr fresh = freshVar(Constraint::None, expr.fieldSpan,
                                 "type parameter of '" + shortName(info.name) + "'");
        fresh->allowUnresolved = true;
        t->params.push_back(fresh);
      }
      return t;
    }
    // Payload access on a value: the field must belong to exactly one variant.
    const VariantInfo* owner = nullptr;
    const std::pair<std::string, TypePtr>* field = nullptr;
    for (const VariantInfo& v : info.variants) {
      for (const auto& f : v.fields) {
        if (f.first != expr.field) continue;
        if (owner) {
          sink_.error(expr.fieldSpan,
                      "Field '" + expr.field + "' exists on multiple variants of '" +
                          shortName(info.name) + "'.",
                      "Use `match` to select the variant first.");
          return typeError();
        }
        owner = &v;
        field = &f;
      }
    }
    if (!owner) {
      sink_.error(expr.fieldSpan,
                  "No variant of '" + shortName(info.name) + "' has a field named '" +
                      expr.field + "'.");
      return typeError();
    }
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < info.typeParamVars.size() && i < object->params.size(); ++i) {
      mapping[info.typeParamVars[i]] = object->params[i];
    }
    return instantiateType(field->second, mapping);
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < structIt->second.typeParamVars.size() && i < object->params.size(); ++i) {
      mapping[structIt->second.typeParamVars[i]] = object->params[i];
    }
    for (const auto& field : structIt->second.fields) {
      if (field.first == expr.field) {
        return instantiateType(field.second, mapping);
      }
    }
    sink_.error(expr.fieldSpan,
                "Struct '" + object->structName + "' has no field named '" + expr.field + "'.");
    return typeError();
  }

  if (object->kind == TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "Cannot access field '" + expr.field + "' on " + typePhrase(object) + ".",
                "Use '?.' to access fields of an optional value.");
    return typeError();
  }

  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }

  if (object->kind == TypeKind::Array || object->kind == TypeKind::String) {
    std::string help = expr.field == "len" ? "Use len(value) instead." : "";
    sink_.error(expr.fieldSpan,
                "Values of type '" + typeName(object) + "' have no fields.", help);
    return typeError();
  }

  sink_.error(expr.fieldSpan,
              "Values of type '" + typeName(object) + "' have no field '" + expr.field + "'.");
  return typeError();
}

TypePtr Sema::inferArrayLiteral(ArrayLitExpr& expr) {
  if (expr.elements.empty()) {
    TypePtr element = freshVar(Constraint::None, expr.span, "the element type of an empty array");
    element->allowUnresolved = true;
    return typeArray(element);
  }

  TypePtr elementType = inferExpr(*expr.elements[0]);
  for (std::size_t i = 1; i < expr.elements.size(); ++i) {
    TypePtr nextType = inferExpr(*expr.elements[i]);
    if (!unify(elementType, nextType)) {
      sink_.error(expr.elements[i]->span,
                  "All array elements must have the same type, found " + typeName(elementType) +
                      " and " + typeName(nextType) + ".");
      return typeError();
    }
  }
  return typeArray(elementType);
}

TypePtr Sema::inferMapLiteral(MapLitExpr& expr) {
  if (expr.entries.empty()) {
    TypePtr key = freshVar(Constraint::None, expr.span, "the key type of an empty map");
    TypePtr value = freshVar(Constraint::None, expr.span, "the value type of an empty map");
    key->allowUnresolved = true; value->allowUnresolved = true;
    return typeMap(key, value);
  }

  TypePtr keyType = inferExpr(*expr.entries[0].first);
  TypePtr valueType = inferExpr(*expr.entries[0].second);
  for (std::size_t i = 1; i < expr.entries.size(); ++i) {
    TypePtr nextKey = inferExpr(*expr.entries[i].first);
    TypePtr nextValue = inferExpr(*expr.entries[i].second);
    if (!unify(keyType, nextKey)) {
      sink_.error(expr.entries[i].first->span,
                  "All map keys must have the same type, found " + typeName(keyType) + " and " +
                      typeName(nextKey) + ".");
      keyType = typeError();
    }
    if (!unify(valueType, nextValue)) {
      sink_.error(expr.entries[i].second->span,
                  "All map values must have the same type, found " + typeName(valueType) +
                      " and " + typeName(nextValue) + ".");
      valueType = typeError();
    }
  }
  return typeMap(keyType, valueType);
}

TypePtr Sema::inferSetLiteral(SetLitExpr& expr) {
  if (expr.elements.empty()) {
    TypePtr element = freshVar(Constraint::None, expr.span, "the element type of an empty set");
    element->allowUnresolved = true;
    return typeSet(element);
  }

  TypePtr elementType = inferExpr(*expr.elements[0]);
  for (std::size_t i = 1; i < expr.elements.size(); ++i) {
    TypePtr nextType = inferExpr(*expr.elements[i]);
    if (!unify(elementType, nextType)) {
      sink_.error(expr.elements[i]->span,
                  "All set elements must have the same type, found " + typeName(elementType) +
                      " and " + typeName(nextType) + ".");
      return typeError();
    }
  }
  return typeSet(elementType);
}

TypePtr Sema::inferTuple(TupleExpr& expr) {
  std::vector<TypePtr> elements;
  elements.reserve(expr.elements.size());
  for (auto& element : expr.elements) {
    TypePtr type = inferExpr(*element);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(element->span, "A tuple element cannot be a void value.");
      type = typeError();
    }
    elements.push_back(type);
  }
  return typeTuple(std::move(elements));
}

TypePtr Sema::inferLambda(LambdaExpr& expr) {
  std::vector<TypePtr> params;
  params.reserve(expr.params.size());
  for (auto& param : expr.params) {
    TypePtr type;
    if (param.type) {
      type = resolveTypeExpr(*param.type);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(param.span, "Lambda parameter '" + param.name + "' cannot have type void.");
        type = typeError();
      }
    } else {
      type = freshVar(Constraint::None, param.span,
                      "the type of lambda parameter '" + param.name + "'");
    }
    params.push_back(type);
  }

  TypePtr ret = expr.returnType
                    ? resolveTypeExpr(*expr.returnType)
                    : freshVar(Constraint::None, expr.span, "the return type of a lambda");

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    declareVar(expr.params[i].name, expr.params[i].span, params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, ret)) {
    reportMismatch(bodyType, ret, expr.body->span, "in the lambda result");
  }
  popScope();

  return typeFunction(std::move(params), std::move(ret));
}

void Sema::checkLambdaAgainst(LambdaExpr& expr, TypePtr expected) {
  TypePtr fn = resolveType(expected);
  if (fn->kind != TypeKind::Function) {
    inferLambda(expr);
    return;
  }
  if (expr.params.size() != fn->params.size()) {
    sink_.error(expr.span,
                "This lambda declares " + std::to_string(expr.params.size()) +
                    " parameter(s), but the expected function type takes " +
                    std::to_string(fn->params.size()) + ".");
    inferLambda(expr);
    return;
  }

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    if (expr.params[i].type) {
      TypePtr declared = resolveTypeExpr(*expr.params[i].type);
      if (!unify(declared, fn->params[i])) {
        reportMismatch(declared, fn->params[i], expr.params[i].span,
                       "in parameter '" + expr.params[i].name + "' of the lambda");
      }
    }
    declareVar(expr.params[i].name, expr.params[i].span, fn->params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, fn->ret)) {
    reportMismatch(bodyType, fn->ret, expr.body->span, "in the lambda result");
  }
  popScope();
}

void Sema::checkArgAgainst(Expr& expr, TypePtr expected, const std::string& context) {
  if (auto* lambda = dynamic_cast<LambdaExpr*>(&expr)) {
    checkLambdaAgainst(*lambda, expected);
    return;
  }
  TypePtr actual = inferExpr(expr);
  if (!unify(actual, expected)) {
    reportMismatch(actual, expected, expr.span, context);
    return;
  }
  checkFixedArrayLength(expr, expected);
}

// A fixed-size array type with a literal initializer must match exactly. The
// check recurses so `i32[2][3]` also validates the nested rows.
void Sema::checkFixedArrayLength(Expr& value, TypePtr expected) {
  TypePtr resolved = resolveType(expected);
  if (!resolved || resolved->kind != TypeKind::Array || resolved->length < 0) return;
  auto* literal = dynamic_cast<ArrayLitExpr*>(&value);
  if (!literal) return;
  const std::int64_t actual = static_cast<std::int64_t>(literal->elements.size());
  if (actual != resolved->length) {
    sink_.error(literal->span,
                "Fixed-size array " + typeName(resolved) + " requires " +
                    std::to_string(resolved->length) + " element(s), found " +
                    std::to_string(actual) + ".");
    return;
  }
  for (auto& element : literal->elements) checkFixedArrayLength(*element, resolved->elem);
}

TypePtr Sema::inferSafeNav(SafeNavExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Error) return typeError();
  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ (void)0;
    return typeError();
  }
  if (object->kind != TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "The left side of '?.' must be optional, found " + typePhrase(object) + ".",
                "Use '.' to access fields of a non-optional value.");
    return typeError();
  }
  TypePtr inner = resolveType(object->elem);
  if (inner->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ (void)0;
    return typeError();
  }
  if (inner->kind != TypeKind::Struct) {
    sink_.error(expr.fieldSpan,
                "'?.' can only access struct fields, but the optional holds " +
                    typePhrase(inner) + ".");
    return typeError();
  }
  auto structIt = structs_.find(inner->structName);
  if (structIt == structs_.end()) return typeError();
  for (const auto& field : structIt->second.fields) {
    if (field.first != expr.field) continue;
    if (resolveType(field.second)->kind == TypeKind::Optional) return field.second;
    return typeOptional(field.second);
  }
  sink_.error(expr.fieldSpan,
              "Struct '" + inner->structName + "' has no field named '" + expr.field + "'.");
  return typeError();
}

namespace {

bool patternIrrefutable(const Pattern& p) {
  switch (p.kind) {
    case Pattern::Kind::Wildcard:
    case Pattern::Kind::Binding:
      return true;
    case Pattern::Kind::Literal:
    case Pattern::Kind::Range:
      return false;
    case Pattern::Kind::Variant:
    case Pattern::Kind::Tuple:
    case Pattern::Kind::Or:
      for (const Pattern& sub : p.subpatterns) {
        if (!patternIrrefutable(sub)) return false;
      }
      return true;
  }
  return false;
}

}  // namespace

void Sema::checkPattern(Pattern& pattern, TypePtr subjectType, std::vector<PatternBinding>& bindings) {
  switch (pattern.kind) {
    case Pattern::Kind::Wildcard:
      return;
    case Pattern::Kind::Literal: {
      TypePtr valueType = inferExpr(*pattern.value);
      if (!unify(valueType, subjectType)) {
        sink_.error(pattern.span,
                    "This pattern has type " + typeName(valueType) + ", but the match subject has "
                    "type " + typeName(subjectType) + ".");
      }
      return;
    }
    case Pattern::Kind::Range:
      break;
    case Pattern::Kind::Binding: {
      for (const PatternBinding& b : bindings) {
        if (b.name == pattern.name) {
          sink_.error(pattern.span, "Duplicate pattern binding '" + pattern.name + "'.",
                      "Each name bound by one pattern must be unique.");
          return;
        }
      }
      PatternBinding binding;
      binding.name = pattern.name;
      binding.span = pattern.span;
      binding.type = subjectType;
      bindings.push_back(std::move(binding));
      return;
    }
    case Pattern::Kind::Tuple: {
      TypePtr subject = resolveType(subjectType);
      if (subject->kind != TypeKind::Tuple) {
        if (subject->kind != TypeKind::Error) {
          sink_.error(pattern.span, "A tuple pattern cannot match a value of type " +
                                        typeName(subjectType) + ".");
        }
        return;
      }
      if (subject->params.size() != pattern.subpatterns.size()) {
        sink_.error(pattern.span,
                    "This tuple has " + std::to_string(subject->params.size()) +
                        " element(s), but the pattern binds " +
                        std::to_string(pattern.subpatterns.size()) + ".");
        return;
      }
      for (std::size_t i = 0; i < pattern.subpatterns.size(); ++i) {
        checkPattern(pattern.subpatterns[i], subject->params[i], bindings);
      }
      return;
    }
    case Pattern::Kind::Or: {
      std::vector<PatternBinding> common;
      bool first = true;
      for (Pattern& alt : pattern.subpatterns) {
        std::vector<PatternBinding> local;
        checkPattern(alt, subjectType, local);
        if (first) {
          common = std::move(local);
          first = false;
          continue;
        }
        bool mismatch = local.size() != common.size();
        for (const PatternBinding& b : local) {
          bool found = false;
          for (const PatternBinding& c : common) {
            if (c.name == b.name && unify(b.type, c.type)) {
              found = true;
              break;
            }
          }
          if (!found) mismatch = true;
        }
        if (mismatch) {
          sink_.error(alt.span, "All '|' pattern alternatives must bind the same names and types.",
                      "Add bindings to the other alternatives, or use '_'.");
          return;
        }
      }
      for (PatternBinding& b : common) bindings.push_back(std::move(b));
      return;
    }
    case Pattern::Kind::Variant:
      break;
  }

  if (pattern.kind == Pattern::Kind::Variant) {
    TypePtr subject = resolveType(subjectType);
    if (subject->kind != TypeKind::Enum) {
      if (subject->kind != TypeKind::Error) {
        sink_.error(pattern.span,
                    "A variant pattern cannot match a value of type " + typeName(subjectType) +
                        ".",
                    "Variant patterns require a subject of algebraic enum type.");
      }
      return;
    }
    auto infoIt = enums_.find(subject->structName);
    if (infoIt == enums_.end() || !infoIt->second.algebraic) {
      sink_.error(pattern.span,
                  "A variant pattern cannot match a value of type " + typeName(subjectType) + ".",
                  "Only enums with payload variants support destructuring patterns.");
      return;
    }
    EnumInfo& info = infoIt->second;
    auto* ctor = dynamic_cast<FieldExpr*>(pattern.ctor.get());
    if (!ctor) {
      sink_.error(pattern.span, "Variant patterns must be qualified, e.g. Shape.Circle(...).");
      return;
    }
    if (!enumNamespaceUse(ctor->object, info.name)) {
      sink_.error(pattern.span,
                  "Variant pattern '" + ctor->field + "' does not belong to enum '" +
                      shortName(subject->structName) + "'.");
      return;
    }
    const VariantInfo* variant = info.findVariant(ctor->field);
    if (!variant) {
      sink_.error(ctor->fieldSpan, "Enum '" + shortName(info.name) + "' has no variant named '" +
                                       ctor->field + "'.");
      return;
    }
    if (variant->fields.size() != pattern.subpatterns.size()) {
      sink_.error(pattern.span,
                  "Variant '" + shortName(info.name) + "." + variant->name + "' has " +
                      std::to_string(variant->fields.size()) + " payload field(s), but the pattern "
                      "binds " +
                      std::to_string(pattern.subpatterns.size()) + ".",
                  "Bind each payload field, or use '_' for fields you do not need.");
      return;
    }
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < info.typeParamVars.size() && i < subject->params.size(); ++i) {
      mapping[info.typeParamVars[i]] = subject->params[i];
    }
    for (std::size_t i = 0; i < pattern.subpatterns.size(); ++i) {
      TypePtr fieldType =
          mapping.empty() ? variant->fields[i].second
                          : instantiateType(variant->fields[i].second, mapping);
      checkPattern(pattern.subpatterns[i], fieldType, bindings);
    }
    return;
  }

  // Range pattern.
  TypePtr start = inferExpr(*pattern.rangeStart);
  TypePtr end = inferExpr(*pattern.rangeEnd);
  requireInteger(start, pattern.rangeStart->span, "The range pattern start");
  requireInteger(end, pattern.rangeEnd->span, "The range pattern end");
  if (!unify(start, end)) {
    sink_.error(pattern.span, "Range pattern endpoints must have the same type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    return;
  }
  if (!unify(start, subjectType)) {
    sink_.error(pattern.span,
                "This range pattern has type " + typeName(start) + ", but the match subject has "
                "type " + typeName(subjectType) + ".");
    return;
  }
}

void Sema::collectPatternCoverage(Pattern& pattern, const EnumInfo& info,
                                  std::vector<std::string>& covered, bool& catchAll) {
  if (pattern.kind == Pattern::Kind::Wildcard) {
    catchAll = true;
    return;
  }
  if (pattern.kind == Pattern::Kind::Literal) {
    auto* field = dynamic_cast<FieldExpr*>(pattern.value.get());
    if (!field || !enumNamespaceUse(field->object, info.name)) return;
    const VariantInfo* variant = info.findVariant(field->field);
    if (!variant || !variant->fields.empty()) return;
    if (std::find(covered.begin(), covered.end(), variant->name) == covered.end()) {
      covered.push_back(variant->name);
    }
    return;
  }
  if (!patternIrrefutable(pattern)) return;
  if (pattern.kind == Pattern::Kind::Variant) {
    auto* ctor = dynamic_cast<FieldExpr*>(pattern.ctor.get());
    const VariantInfo* variant = ctor ? info.findVariant(ctor->field) : nullptr;
    if (variant && std::find(covered.begin(), covered.end(), variant->name) == covered.end()) {
      covered.push_back(variant->name);
    }
    return;
  }
  if (pattern.kind == Pattern::Kind::Or) {
    for (Pattern& alt : pattern.subpatterns) {
      collectPatternCoverage(alt, info, covered, catchAll);
    }
  }
}

TypePtr Sema::inferMatch(MatchExpr& expr) {
  TypePtr subject = inferExpr(*expr.subject);
  TypePtr result;
  bool sawResult = false;

  const EnumInfo* enumInfo = nullptr;
  TypePtr subjectResolved = resolveType(subject);
  if (subjectResolved->kind == TypeKind::Enum) {
    auto found = enums_.find(subjectResolved->structName);
    if (found != enums_.end() && found->second.algebraic) enumInfo = &found->second;
  }

  for (auto& arm : expr.arms) {
    pushScope();
    std::vector<PatternBinding> bindings;
    checkPattern(arm.pattern, subject, bindings);
    for (const PatternBinding& binding : bindings) {
      declareVar(binding.name, binding.span, binding.type, false, "pattern binding");
    }
    if (arm.guard) {
      requireBool(inferExpr(*arm.guard), arm.guard->span, "A match guard");
    }
    TypePtr armType = inferExpr(*arm.result);
    if (resolveType(armType)->kind == TypeKind::Void) {
      sink_.error(arm.result->span, "A match arm must produce a value.");
      armType = typeError();
    }
    if (!sawResult) {
      result = armType;
      sawResult = true;
    } else if (!unify(result, armType)) {
      sink_.error(arm.result->span,
                  "All match arms must produce the same type, found " + typeName(result) +
                      " and " + typeName(armType) + ".");
    }
    popScope();
  }

  if (enumInfo) {
    std::vector<std::string> covered;
    bool catchAll = false;
    for (auto& arm : expr.arms) {
      if (arm.guard) continue;
      collectPatternCoverage(arm.pattern, *enumInfo, covered, catchAll);
      if (catchAll) break;
    }
    if (!catchAll) {
      std::vector<std::string> missing;
      for (const VariantInfo& variant : enumInfo->variants) {
        if (std::find(covered.begin(), covered.end(), variant.name) == covered.end()) {
          missing.push_back(variant.name);
        }
      }
      if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) {
          if (i) list += i + 1 == missing.size() ? " and " : ", ";
          list += "'" + missing[i] + "'";
        }
        sink_.error(expr.span, std::string("Non-exhaustive match: no arm covers variant") +
                                  (missing.size() == 1 ? "" : "s") + " " + list + ".",
                    "Add arms for the missing variant(s), or a final '_ => ...' arm.");
      }
    }
  } else {
    const MatchArm& last = expr.arms.back();
    if (last.pattern.kind != Pattern::Kind::Wildcard || last.guard) {
      sink_.error(expr.span,
                  "A match expression must end with an unguarded '_' arm.",
                  "Add '_ => ...' so every value is handled.");
    }
  }

  return sawResult ? result : typeError();
}

TypePtr Sema::inferTry(TryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  TypePtr resolved = resolveType(operand);
  if (resolved->kind == TypeKind::Optional) {
    if (currentReturnType_) {
      TypePtr ret = resolveType(currentReturnType_);
      if (ret->kind == TypeKind::Optional) {
        if (!unify(ret->elem, resolved->elem)) {
          sink_.error(expr.span, "The value produced by '? does not match the function's "
                                 "optional return type.");
        }
      } else if (ret->kind != TypeKind::Error) {
        sink_.error(expr.span,
                    "Using '?' on an optional requires the function to return an optional.",
                    "Return '" + typeName(resolved) + "' or change the function's return type "
                    "to " + typeName(resolved) + ".");
      }
    }
    return resolved->elem;
  }
  if (resolved->kind != TypeKind::Result) {
    if (resolved->kind != TypeKind::Error) {
      std::string message =
          "The '?' operator requires a Result or Optional value, found '" +
          typeName(resolved) + "'.";
      if (resolved->kind == TypeKind::Enum) {
        sink_.error(expr.operand->span, message,
                    "Match on the enum value, or return a Result from a helper instead.");
      } else {
        sink_.error(expr.operand->span, message);
      }
    }
    return typeError();
  }
  // The enclosing function must return a compatible Result.
  if (currentReturnType_) {
    TypePtr ret = resolveType(currentReturnType_);
    if (ret->kind == TypeKind::Result) {
      if (!unify(ret->errorType, resolved->errorType)) {
        sink_.error(expr.span, "The error type of '?' does not match the function's return type.");
      }
    } else if (ret->kind == TypeKind::Optional) {
      sink_.error(expr.span,
                  "Using '?' on a Result inside a function returning an optional loses the "
                  "error value.",
                  "Return a Result, or map the error to a value first.");
    }
  }
  return resolved->elem;
}

TypePtr Sema::inferComprehension(ComprehensionExpr& expr) {
  TypePtr elementType =
      iterableElementType(inferExpr(*expr.iterable), expr.iterable->span, "and comprehensions");

  pushScope();
  declareVar(expr.var, expr.varSpan, elementType, false, "loop variable");
  if (expr.condition) {
    requireBool(inferExpr(*expr.condition), expr.condition->span, "The comprehension condition");
  }
  TypePtr result = inferExpr(*expr.element);
  if (resolveType(result)->kind == TypeKind::Void) {
    sink_.error(expr.element->span, "A comprehension element must produce a value.");
    result = typeError();
  }
  popScope();
  return expr.isSet ? typeSet(result) : typeArray(result);
}

// ---------------------------------------------------------------- helpers

void Sema::requireBool(TypePtr type, Span span, const std::string& what) {
      TypePtr resolved = resolveType(type);
      if (resolved->kind == TypeKind::Bool) return;
      if (resolved->kind == TypeKind::Var) { unify(type, typeBool()); return; }
      if (resolved->kind == TypeKind::Error) return;
      sink_.error(span, what + " must be bool, found " + typePhrase(type) + ".");
    }

    void Sema::requireInteger(TypePtr type, Span span, const std::string& what) {
  (void)type; (void)span; (void)what;
  // Dynamic: runtime validates/coerces.
}

TypePtr Sema::requireNumeric(TypePtr type, Span span, const std::string& what) {
  TypePtr resolved = resolveType(type);
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) return type;
  if (resolved->kind == TypeKind::Var && constrainVar(type, Constraint::Numeric)) return type;
  sink_.error(span, what + " must be numeric, found " + typePhrase(type) + ".");
  return typeError();
}

bool Sema::checkCompoundOp(const std::string& op, TypePtr targetType, Span span) {
      (void)op; (void)span;
      TypePtr type = resolveType(targetType);
      if (type->kind == TypeKind::Var) { constrainVar(targetType, Constraint::Numeric); return true; }
      return true;
    }

    void Sema::reportMismatch(TypePtr actual, TypePtr expected, Span span,
                              const std::string& context) {
      (void)actual; (void)expected; (void)span; (void)context;
    }

    std::string Sema::typePhrase(const TypePtr& type) const {
  TypePtr resolved = resolveType(type);
  if (!resolved) return "a value";
  if (resolved->kind == TypeKind::Var) {
    switch (resolved->constraint) {
      case Constraint::Integer: return "an integer";
      case Constraint::Floating: return "a float";
      case Constraint::Numeric: return "a number";
      case Constraint::None: return "a value of unknown type";
    }
  }
  std::string name = typeName(resolved);
  char first = name.empty() ? 'x' : name[0];
  bool vowelSound = first == 'a' || first == 'e' || first == 'i' || first == 'o';
  return (vowelSound ? "an " : "a ") + name;
}

void Sema::checkTypeVariables() {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  for (auto& info : typeVariables_) {
    TypePtr resolved = resolveType(info.var);
    if (!resolved || resolved->kind != TypeKind::Var) continue;
    if (resolved->allowDefault) {
      if (resolved->constraint == Constraint::Integer) {
        resolved->link = typeInt(32, true);
        continue;
      }
      if (resolved->constraint == Constraint::Floating) {
        resolved->link = typeFloat(64);
        continue;
      }
    }
    if (resolved->constraint == Constraint::Integer) resolved->link = typeInt(32, true);
    else if (resolved->constraint == Constraint::Floating) resolved->link = typeFloat(64);
    else if (resolved->constraint == Constraint::Numeric) resolved->link = typeInt(64, true);
    continue;
  }
}

void Sema::checkDeclaredVariables() {
  for (auto& info : declaredVariables_) {
    TypePtr resolved = resolveType(info.type);
    if (resolved && resolved->kind == TypeKind::Void) {
      sink_.error(info.span, "Variable '" + info.name + "' cannot have type void.");
    }
  }
}

void Sema::checkReturnPaths() {
  for (auto& entry : checkedFunctions_) {
    const FnDecl* decl = entry.first;
    TypePtr ret = resolveType(entry.second);
    if (!ret || ret->kind == TypeKind::Void || ret->kind == TypeKind::Var) continue;
    if (!blockReturns(decl->body)) {
      sink_.error(decl->nameSpan,
                  "Not all paths in function '" + decl->name + "' return a value of type '" +
                      typeName(ret) + "'.",
                  "Add a 'return' at the end, or use if/else so that every branch returns.");
    }
  }
}

bool Sema::blockReturns(const std::vector<StmtPtr>& body) {
  if (body.empty()) return false;
  // Statements after a terminator are unreachable; the last reachable one
  // decides whether the block returns.
  const Stmt* last = nullptr;
  for (const auto& stmt : body) {
    if (last && terminatesFlow(*last)) break;
    last = stmt.get();
  }
  return last && statementReturns(*last);
}

bool Sema::statementReturns(const Stmt& stmt) {
  if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
  if (auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
    if (node->elseBody.empty()) return false;
    for (const auto& branch : node->branches) {
      if (!blockReturns(branch.body)) return false;
    }
    return blockReturns(node->elseBody);
  }
  if (dynamic_cast<const LoopStmt*>(&stmt)) return true;
  return false;
}

}  // namespace a
