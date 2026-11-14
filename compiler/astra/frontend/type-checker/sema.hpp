#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "diagnostics/diagnostics.hpp"
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/types.hpp"

namespace a {

// Per-module exported symbols, keyed by their short names.
struct ModuleExports {
  std::string name;
  StringMap<const FnDecl*> functions;
  StringMap<const StructDecl*> structs;
  StringMap<TypePtr> values;
  StringMap<std::string> structAliases;  // local alias -> qualified
  StringMap<std::string> enums;  // name -> qualified enum name
  StringMap<std::string> traits;  // name -> qualified trait name
};

struct VariantInfo {
  std::string name;
  std::vector<std::pair<std::string, TypePtr>> fields;  // payload fields
};

struct PatternBinding {
  std::string name;
  Span span;
  TypePtr type;
};

struct EnumInfo {
  std::string name;  // qualified
  StringMap<std::int64_t> values;  // plain enums: constant value; algebraic: tag index
  bool algebraic = false;
  // Non-zero when this is the auto-loaded prelude declaration that maps onto
  // a built-in kind: 1 = Option<T> (optional), 2 = Result<T, E> (result).
  int builtinKind = 0;
  std::vector<std::string> typeParams;
  std::vector<TypePtr> typeParamVars;
  std::vector<VariantInfo> variants;  // algebraic only; declaration order = tag order

  const VariantInfo* findVariant(const std::string& name) const {
    for (const VariantInfo& v : variants)
      if (v.name == name) return &v;
    return nullptr;
  }
};

// A declared variable's (possibly inferred) type, for LSP hover/inlay hints
// the way ty/pyrefly show inferred types (e.g. `x: i32` on `x = 1`).
struct VarTypeInfo {
  std::string name;
  Span span;
  TypePtr type;
  // True only for plain `name = ...` variable declarations (not parameters,
  // loop variables, or pattern bindings) — used to limit inlay type hints.
  bool variableDecl = false;
};

struct SemaResult {
  StringMap<const FnDecl*> functions;  // qualified names
  StringMap<const StructDecl*> structs;  // qualified names
  StringMap<EnumInfo> enums;  // qualified names
  std::vector<ModuleExports> modules;
  std::unordered_map<const FnDecl*, int> fnModule;
  std::unordered_map<const StructDecl*, int> structModule;
  std::unordered_map<const FnDecl*, std::vector<TypePtr>> fnParamTypes;
  std::unordered_map<const FnDecl*, TypePtr> fnReturnTypes;
  // All declared variables (locals, params, loop vars) with their inferred
  // types. Types are shared (Var-linked) so they reflect final inference.
  std::vector<VarTypeInfo> variables;
  // Per-expression inferred types, keyed by AST node (mirrors
  // Expr::inferredType for editors that walk the AST).
  std::unordered_map<const Expr*, TypePtr> exprTypes;
};

class Sema {
 public:
  Sema(ModuleGraph& graph, DiagnosticSink& sink);

  SemaResult analyze();
  // Parallel checking: declarations are registered serially, then each
  // module's body checking runs on a worker pool (ty/pyrefly do the same).
  // `jobs <= 0` means hardware-concurrency. Diagnostics are thread-safe;
  // Type-var unification is serialised by a global lock, so results are
  // identical to analyze(), just faster on multi-module programs.
  SemaResult analyzeParallel(int jobs);

 private:
  struct VarSym {
    TypePtr type;
    // The type written in a `x: T = ...` annotation or parameter annotation,
    // when present. Reassignment (`x = ...`) is checked against it even though
    // `type` itself may be rebound to a new inferred type.
    TypePtr declaredType;
    bool mutableBinding = false;
    bool constant = false;
    bool used = false;
    bool warnUnused = false;
    std::string name;
    Span span;
  };

  struct FnSig {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<TypePtr> typeParamVars;
    std::vector<TypePtr> paramTypes;
    // Parallel to paramTypes: nonzero when the parameter has a default value.
    // Defaults are only allowed on trailing parameters (validated at decl).
    std::vector<char> paramHasDefault;
    // `fn f(..., *args)`: the last paramType is a list collecting extra args.
    bool variadic = false;
    TypePtr retType;
    const FnDecl* decl = nullptr;

    // Number of leading parameters that must be supplied at the call site.
    std::size_t requiredArgs() const {
      std::size_t limit = paramTypes.size() - (variadic && !paramTypes.empty() ? 1 : 0);
      for (std::size_t i = 0; i < limit; ++i)
        if (paramHasDefault[i]) return i;
      return limit;
    }
  };

  struct StructSym {
    std::string name;
    const StructDecl* decl = nullptr;
    std::vector<std::string> typeParams;
    std::vector<TypePtr> typeParamVars;
    std::vector<std::pair<std::string, TypePtr>> fields;
    StringMap<FnSig> methods;
  };

  struct VarInfo {
    TypePtr var;
    Span span;
    std::string description;
  };

  struct DeclaredVarInfo {
    std::string name;
    Span span;
    TypePtr type;
    bool variableDecl = false;
  };

  struct ModuleAlias {
    bool native = false;
    bool python = false;
    int moduleId = -1;
    std::string nativeName;
    std::string pythonName;
  };

  struct FromBinding {
    enum class Kind { Value, Function, Struct, Trait };
    Kind kind = Kind::Value;
    bool native = false;
    bool python = false;
    std::string sourceName;  // name in the source module (re-export tracking)
    std::string nativeName;  // builtin implementing a native member
    std::string pythonModule;
    std::string pythonMember;
    const FnDecl* fn = nullptr;
    const StructDecl* decl = nullptr;
    std::string structName;  // qualified struct name
    std::string traitName;   // qualified trait name
    TypePtr type;
    int moduleId = -1;
    Span span;
  };

  // program-level
  void analyzeModule(Module& module);
  void registerModule(Module& module);
  void checkModule(Module& module);
  void checkModuleBodies(Module& module);
  // Per-module snapshot of import/alias bindings taken after registration
  // (passes 1-3) so body checking can run in parallel on workers.
  struct ModuleCheckState {
    StringMap<ModuleAlias> moduleAliases;
    StringMap<FromBinding> fromBindings;
    StringMap<std::string> structAliases;
    StringMap<TypePtr> typeAliases;
  };
  std::unordered_map<int, ModuleCheckState> moduleStates_;
  void registerImports(Module& module);
  void buildModuleExports(Module& module);
  std::string qualified(const std::string& name) const;
  std::string qualifiedFor(int moduleId, const std::string& name) const;
  ModuleExports& exportsFor(int moduleId);
  TypePtr moduleMemberType(const ModuleAlias& alias, const std::string& member, Span span,
                           bool report);
  // Follow `from "x.a" import name` value re-exports to the declaring
  // module so exported package values keep their concrete type.
  TypePtr resolveReexportedValue(int moduleId, const std::string& name, int depth);
  TypePtr moduleMemberCall(const ModuleAlias& alias, const std::string& member, CallExpr& call);
  ModuleAlias* findAlias(const std::string& name);

  // types
  TypePtr freshVar(Constraint constraint, Span span, std::string description,
                   bool allowDefault = false);
  bool unify(TypePtr a, TypePtr b);
  bool bindVar(TypePtr var, TypePtr type);
  bool occursIn(TypePtr var, TypePtr type);
  bool constrainVar(TypePtr var, Constraint constraint);
  TypePtr resolveTypeExpr(TypeExpr& expr);
  TypePtr resolveTypeExprImpl(TypeExpr& expr);
  TypePtr instantiateType(TypePtr type, const std::unordered_map<TypePtr, TypePtr>& mapping);
  TypePtr makeSignatureTypes(FnDecl& decl, TypePtr selfType, FnSig& sig);

  // scopes
  void pushScope();
  void popScope();
  void reportUnusedLocals(const StringMap<VarSym>& scope);
  VarSym* lookupVar(const std::string& name);
  void declareVar(const std::string& name, Span span, TypePtr type, bool mutableBinding,
                  const char* what = "variable", bool constant = false);

  // declarations
  void registerStructLayout(StructDecl& decl);
  void registerEnum(EnumDecl& decl);
  void resolveEnumPayloads(EnumDecl& decl);
  void registerFunctionSignature(FnDecl& decl);
  // Validate an `extern fn` declaration's signature against the C-FFI type
  // mapping and record its param/return types for the LLVM backend.
  void checkExternDecl(FnDecl& decl, const FnSig& sig);
  void registerMethodSignatures(StructDecl& decl);
  void checkFunctionBody(FnDecl& decl, const FnSig& sig);
  void checkStructMethodBodies(StructDecl& decl);

  // traits
  const TraitDecl* findTrait(const std::string& name);
  const StructDecl* findStructDecl(const std::string& name);
  void checkTraitDecl(TraitDecl& decl);
  void checkImpl(ImplDecl& decl);
  void checkWhereClause(const std::vector<std::string>& traits, Span span,
                        const std::string& what,
                        const std::vector<Span>* traitSpans = nullptr);
  bool structSatisfiesTrait(const StructDecl& type, const TraitDecl& trait);

  // statements
  void checkStatements(std::vector<StmtPtr>& statements);
  void checkStatement(Stmt& stmt);
  void checkLet(LetStmt& stmt);
  void checkAssign(AssignStmt& stmt);
  void checkDestructure(DestructureStmt& stmt);
  void checkIf(IfStmt& stmt);
  void checkWhile(WhileStmt& stmt);
  void checkDoWhile(DoWhileStmt& stmt);
  void checkFor(ForStmt& stmt);
  TypePtr iterableElementType(TypePtr iterableType, Span span, const std::string& hint);
  void checkLoop(LoopStmt& stmt);
  void checkReturn(ReturnStmt& stmt);
  void checkDefer(DeferStmt& stmt);

  // expressions
  TypePtr inferExpr(Expr& expr);
  TypePtr inferExprImpl(Expr& expr);
  TypePtr inferIdent(IdentExpr& expr);
  TypePtr inferUnary(UnaryExpr& expr);
  TypePtr inferBinary(BinaryExpr& expr);
  TypePtr inferCall(CallExpr& expr);
  TypePtr inferIndex(IndexExpr& expr);
  TypePtr inferSlice(SliceExpr& expr);
  TypePtr inferField(FieldExpr& expr);
  TypePtr inferSafeNav(SafeNavExpr& expr);
  TypePtr inferArrayLiteral(ArrayLitExpr& expr);
  TypePtr inferMapLiteral(MapLitExpr& expr);
  TypePtr inferSetLiteral(SetLitExpr& expr);
  TypePtr inferStringLiteral(StringLitExpr& expr);
  TypePtr inferTuple(TupleExpr& expr);
  TypePtr inferLambda(LambdaExpr& expr);
  TypePtr inferMatch(MatchExpr& expr);
  TypePtr inferTry(TryExpr& expr);
  TypePtr inferComprehension(ComprehensionExpr& expr);
  void checkPattern(Pattern& pattern, TypePtr subjectType, std::vector<PatternBinding>& bindings);
  void collectPatternCoverage(Pattern& pattern, const EnumInfo& info,
                              std::vector<std::string>& covered, bool& catchAll);
  void checkLambdaAgainst(LambdaExpr& expr, TypePtr expected);
  void checkArgAgainst(Expr& expr, TypePtr expected, const std::string& context);
  void checkFixedArrayLength(Expr& value, TypePtr expected);
  // When `expected` is vector[T, N] and the already-inferred `expr` is an
  // array literal of N elements or a numeric scalar, accept it as a vector
  // (broadcast for scalars) and rewrite expr.inferredType so the backend
  // emits unboxed <N x ty> IR. Returns false when no coercion applied.
  bool coerceVectorInit(Expr& expr, TypePtr expected);

  TypePtr checkFunctionValueCall(const std::string& name, TypePtr fnType, CallExpr& call);

  TypePtr checkStructConstruction(const StructSym& sym, CallExpr& call);
  TypePtr checkVariantConstruction(EnumInfo& info, FieldExpr& field, CallExpr& call);
  TypePtr enumTypeFromInfo(const EnumInfo& info, NamedTypeExpr& named);
  bool enumNamespaceUse(ExprPtr& object, const std::string& enumQ);
  TypePtr checkFunctionCall(const FnSig& sig, CallExpr& call);
  // Prelude fallback: unqualified calls that match a prelude function resolve
  // to it, giving prelude decls Python-builtin-like visibility.
  const FnDecl* findPreludeFunction(const std::string& name) const;
  TypePtr checkBuiltinCall(const std::string& name, CallExpr& call);
  TypePtr checkPrintCall(const std::string& name, CallExpr& call);
  TypePtr checkIcCall(CallExpr& call);
  TypePtr checkCastCall(CallExpr& call);
  std::string icContextString(Span span) const;
  // Builtin domains: each returns nullptr when `name` is not handled so the
  // dispatcher can try the next domain.
  TypePtr checkBuiltinValues(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinIo(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinMemory(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinNumeric(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinSystem(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinCollections(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinJson(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinTesting(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinLog(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinMetrics(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinTrace(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinNet(const std::string& name, CallExpr& call);
  TypePtr checkBuiltinKernels(const std::string& name, CallExpr& call);
  std::vector<TypePtr> inferCallArgTypes(CallExpr& call);
  void builtinArityError(const std::string& name, CallExpr& call, std::size_t expected);
  TypePtr inferAnyCallArg(CallExpr& call, std::size_t index, const std::string& context);
  TypePtr checkMethodCall(const TypePtr& objectType, FieldExpr& field, CallExpr& call);

  // flow narrowing (ty/pyrefly style): `if x != None:` refines `x: T?` to
  // `x: T` in the then-branch; `if x == None:` does the opposite in the else.
  struct NarrowRestore {
    VarSym* sym = nullptr;
    TypePtr saved;
  };
  // Collect (varName, isNonNone) facts from `cond` assuming it evaluates to
  // `truthy`. Handles `x != None`, `x == None`, `not (...)`, `and`/`or`.
  std::vector<std::pair<std::string, bool>> narrowingFacts(Expr& cond, bool truthy);
  std::vector<NarrowRestore> applyNarrowing(Expr& cond, bool truthy);
  void restoreNarrowing(std::vector<NarrowRestore>& restores);

  // helpers
  // Closest visible name to `name` (edit distance), or "" — powers the
  // Python-style "Did you mean 'x'?" hint on undefined-name errors.
  std::string suggestName(const std::string& name) const;
  void requireBool(TypePtr type, Span span, const std::string& what);
  void requireInteger(TypePtr type, Span span, const std::string& what);
  TypePtr requireNumeric(TypePtr type, Span span, const std::string& what);
  TypePtr numericBinaryResult(const std::string& op, TypePtr lhs, TypePtr rhs, Span span);
  bool checkCompoundOp(const std::string& op, TypePtr targetType, Span span);
  void reportMismatch(TypePtr actual, TypePtr expected, Span span, const std::string& context);
  // Payload of `T?` when T is still an unresolved inference variable — the
  // genuinely-dynamic optional produced by e.g. `map.get` on an untyped
  // receiver or a bare `None`. Returns nullptr for concrete optionals.
  TypePtr dynamicOptionalElem(const TypePtr& type) const;
  std::string typePhrase(const TypePtr& type) const;
  void checkTypeVariables();
  void checkDeclaredVariables();
  void checkReturnPaths();
  // Range-checks numeric literals recorded since `begin` now that their type
  // variables have been resolved/defaulted (e.g. `x: u8 = 300` is an error).
  void checkLiteralRanges(std::size_t begin);
  // Reports an error when `value` cannot be represented by sized int `type`.
  // Returns true when an error was reported.
  bool checkSizedIntRange(Span span, std::int64_t value, bool negated,
                          const TypePtr& type);
  bool blockReturns(const std::vector<StmtPtr>& body);
  bool statementReturns(const Stmt& stmt);

  ModuleGraph& graph_;
  DiagnosticSink& sink_;
  std::vector<ModuleExports> moduleExports_;
  int currentModule_ = -1;

  StringMap<ModuleAlias> moduleAliases_;
  StringMap<FromBinding> fromBindings_;
  StringMap<std::string> structAliases_;  // local alias -> qualified name
  StringMap<TypePtr> genericParams_;  // current generic type parameters

  std::vector<StringMap<VarSym>> scopes_;
  StringMap<const FnDecl*> functionsByName_;
  StringMap<const StructDecl*> structsByName_;
  StringMap<const TraitDecl*> traits_;  // qualified name -> trait declaration
  StringMap<std::vector<std::string>> traitImpls_;  // qualified struct -> conformed traits
  StringMap<EnumInfo> enums_;
  std::unordered_map<const FnDecl*, FnSig> signatures_;
  StringMap<StructSym> structs_;
  StringMap<TypePtr> typeAliases_;
  std::unordered_map<const FnDecl*, int> fnModules_;
  std::unordered_map<const StructDecl*, int> structModules_;
  std::unordered_map<const FnDecl*, std::vector<TypePtr>> fnParamTypes_;
  std::unordered_map<const FnDecl*, TypePtr> fnReturnTypes_;
  std::vector<VarInfo> typeVariables_;
  // Numeric literal expressions seen during body checking, in order. Checked
  // for sized-type range errors at the end of each module's checkModule.
  std::vector<const Expr*> numericLiterals_;
  // Effective (sign-adjusted) values for literals behind unary '-', keyed by
  // the IntLitExpr node. `-(-5)` flips the sign back to positive.
  std::unordered_map<const Expr*, std::int64_t> literalValueOverrides_;
  std::vector<DeclaredVarInfo> declaredVariables_;
  std::vector<std::pair<const FnDecl*, TypePtr>> checkedFunctions_;
  std::unordered_map<const Expr*, TypePtr> exprTypes_;

  int nextVarId_ = 1;
  int loopDepth_ = 0;
  int deferDepth_ = 0;
  FnDecl* currentFunction_ = nullptr;
  TypePtr currentReturnType_;
  bool sawReturn_ = false;
};

}  // namespace a
