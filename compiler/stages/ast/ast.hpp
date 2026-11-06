#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lexer/span.hpp"

namespace a {

struct Expr;
struct Stmt;
struct TypeExpr;
struct FnDecl;
struct StructDecl;
struct Type;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeExprPtr = std::unique_ptr<TypeExpr>;
using TypePtr = std::shared_ptr<Type>;

// ---------------------------------------------------------------- types

// Node tags let the backend dispatch with a single compare instead of an RTTI
// chain. Every node sets its tag in its constructor; the interpreter and any
// future bytecode compiler switch on the tag.
enum class ExprKind : std::uint8_t {
  IntLit,
  FloatLit,
  BoolLit,
  CharLit,
  StringLit,
  Ident,
  Unary,
  Binary,
  Call,
  Index,
  Slice,
  Field,
  SafeNav,
  ArrayLit,
  Range,
  Ternary,
  Tuple,
  Lambda,
  Comprehension,
  Match,
  Try,
  MapLit,
  SetLit,
};

enum class StmtKind : std::uint8_t {
  Let,
  Assign,
  Expr,
  Destructure,
  Return,
  Break,
  Continue,
  Defer,
  Import,
  FromImport,
  If,
  While,
  DoWhile,
  For,
  Loop,
  FnDecl,
  StructDecl,
  EnumDecl,
  TypeAlias,
  TraitDecl,
  ImplDecl,
};

// Arithmetic/logical operator identity for the backend. `op` still carries the
// source spelling for diagnostics and semantic analysis; `binop` is what the
// interpreter switches on.
enum class BinOp : std::uint8_t {
  Invalid,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Pow,
  Eq,
  Ne,
  Lt,
  Gt,
  Le,
  Ge,
  BitAnd,
  BitOr,
  BitXor,
  Shl,
  Shr,
  And,
  Or,
  Coalesce,
  Range,
  In,
  NotIn,
};

// Statically resolved builtin identity. `Sema` marks each call site whose
// callee is not shadowed by a variable, so the interpreter can dispatch a
// builtin without re-scanning name strings on every call (the same idea as
// specialized call opcodes in dynamic-language interpreters).
enum class BuiltinId : std::uint8_t {
  None,
  Print,
  Eprint,
  Len,
  Str,
  Sqrt,
  F64,
  I64,
  Abs,
  Round,
  Floor,
  Ceil,
  Pow,
  Join,
  Map,
  Filter,
  Sum,
  Reduce,
  Any,
  All,
  Some,
  Ok,
  Err,
  Panic,
  Assert,
  AssertEq,
  Ic,
};

inline BuiltinId builtinIdForName(std::string_view name) {
  if (name == "print") return BuiltinId::Print;
  if (name == "eprint") return BuiltinId::Eprint;
  if (name == "len") return BuiltinId::Len;
  if (name == "str") return BuiltinId::Str;
  if (name == "sqrt") return BuiltinId::Sqrt;
  if (name == "f64") return BuiltinId::F64;
  if (name == "i64") return BuiltinId::I64;
  if (name == "abs") return BuiltinId::Abs;
  if (name == "round") return BuiltinId::Round;
  if (name == "floor") return BuiltinId::Floor;
  if (name == "ceil") return BuiltinId::Ceil;
  if (name == "pow") return BuiltinId::Pow;
  if (name == "join") return BuiltinId::Join;
  if (name == "map") return BuiltinId::Map;
  if (name == "filter") return BuiltinId::Filter;
  if (name == "sum") return BuiltinId::Sum;
  if (name == "reduce") return BuiltinId::Reduce;
  if (name == "any") return BuiltinId::Any;
  if (name == "all") return BuiltinId::All;
  if (name == "Some") return BuiltinId::Some;
  if (name == "Ok") return BuiltinId::Ok;
  if (name == "Err") return BuiltinId::Err;
  if (name == "panic") return BuiltinId::Panic;
  if (name == "assert") return BuiltinId::Assert;
  if (name == "assert_eq") return BuiltinId::AssertEq;
  if (name == "ic") return BuiltinId::Ic;
  return BuiltinId::None;
}

struct TypeExpr {
  Span span;
  virtual ~TypeExpr() = default;
};

struct NamedTypeExpr : TypeExpr {
  std::vector<std::string> qualifiers;  // module alias qualifiers: 'u' in 'u.Point'
  std::string name;
  std::vector<TypeExprPtr> typeArgs;  // generic arguments: 'Foo<T>'
};

struct ArrayTypeExpr : TypeExpr {
  TypeExprPtr element;
  std::int64_t length = -1;  // -1 for a dynamically sized array
};

struct OptionalTypeExpr : TypeExpr {
  TypeExprPtr element;
};

struct TupleTypeExpr : TypeExpr {
  std::vector<TypeExprPtr> elements;
};

struct MapTypeExpr : TypeExpr {
  TypeExprPtr key;
  TypeExprPtr value;
};

struct SetTypeExpr : TypeExpr {
  TypeExprPtr element;
};

struct FunctionTypeExpr : TypeExpr {
  std::vector<TypeExprPtr> params;
  TypeExprPtr returnType;
};

struct ParamDecl {
  std::string name;
  Span span;
  TypeExprPtr type;  // may be null for inferred parameter types
};

// ---------------------------------------------------------------- expressions

struct Expr {
  Span span;
  ExprKind kind;
  // Filled in by Sema. The IR lowering reads it to give every SSA value a
  // static type without re-running type inference.
  TypePtr inferredType;
  explicit Expr(ExprKind k) : kind(k) {}
  virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
  IntLitExpr() : Expr(ExprKind::IntLit) {}
  std::int64_t value = 0;
};

struct FloatLitExpr : Expr {
  FloatLitExpr() : Expr(ExprKind::FloatLit) {}
  double value = 0.0;
};

struct BoolLitExpr : Expr {
  BoolLitExpr() : Expr(ExprKind::BoolLit) {}
  bool value = false;
};

struct CharLitExpr : Expr {
  CharLitExpr() : Expr(ExprKind::CharLit) {}
  char32_t value = 0;
};

struct StringPart {
  bool isExpr = false;
  std::string text;  // decoded literal text
  ExprPtr expr;
};

struct StringLitExpr : Expr {
  StringLitExpr() : Expr(ExprKind::StringLit) {}
  std::vector<StringPart> parts;
};

struct IdentExpr : Expr {
  IdentExpr() : Expr(ExprKind::Ident) {}
  std::string name;
};

struct UnaryExpr : Expr {
  UnaryExpr() : Expr(ExprKind::Unary) {}
  std::string op;  // "-", "not", "!", "~", "++", or "--"
  bool postfix = false;  // true for i++ / i--, false for ++i / --i / -x / !x / ~x
  Span opSpan;
  ExprPtr operand;
};

struct BinaryExpr : Expr {
  BinaryExpr() : Expr(ExprKind::Binary) {}
  std::string op;
  BinOp binop = BinOp::Invalid;
  Span opSpan;
  ExprPtr lhs;
  ExprPtr rhs;
};

struct CallArg {
  std::string name;  // empty for positional arguments
  Span nameSpan;
  ExprPtr value;
};

struct CallExpr : Expr {
  CallExpr() : Expr(ExprKind::Call) {}
  ExprPtr callee;
  std::vector<CallArg> args;

  // Resolved lazily by the interpreter. A plain identifier callee is fixed by
  // name resolution, so the classification (function, struct, or builtin) can
  // be cached per call site instead of rebuilt on every execution.
  mutable bool calleeResolved = false;
  mutable const FnDecl* calleeFunction = nullptr;
  mutable const StructDecl* calleeStruct = nullptr;

  // Set by Sema when the callee is a builtin that no local binding shadows.
  BuiltinId builtinId = BuiltinId::None;

  // Populated by Sema for BuiltinId::Ic call sites: the call-site context
  // ("file.a:3 in fn()") and one source-text label per argument, used by the
  // interpreter and native emitters to render litprinter-style output.
  std::string icContext;
  std::vector<std::string> icArgLabels;
};

// `ic(...)` argument classification, shared by Sema (labels) and the
// backends (layout). Mirrors litprinter: literals, strings and bare names
// are "simple" enough to suppress call-site context; bare literals print
// without an "expr:" label. Unary minus on a literal counts as a literal.
inline bool icExprIsLiteralLike(const Expr& e) {
  switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
    case ExprKind::StringLit:
      return true;
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(e);
      return u.op == "-" && !u.postfix && icExprIsLiteralLike(*u.operand);
    }
    default:
      return false;
  }
}

inline bool icExprIsSimple(const Expr& e) {
  return e.kind == ExprKind::Ident || icExprIsLiteralLike(e);
}

inline bool icExprIsBare(const Expr& e) { return icExprIsLiteralLike(e); }

struct IndexExpr : Expr {
  IndexExpr() : Expr(ExprKind::Index) {}
  ExprPtr object;
  ExprPtr index;
  Span bracketSpan;
};

struct SliceExpr : Expr {
  SliceExpr() : Expr(ExprKind::Slice) {}
  ExprPtr object;
  ExprPtr start;  // may be null
  ExprPtr end;    // may be null
  ExprPtr step;   // may be null (defaults to 1)
  Span bracketSpan;
};

struct MapLitExpr : Expr {
  MapLitExpr() : Expr(ExprKind::MapLit) {}
  std::vector<std::pair<ExprPtr, ExprPtr>> entries;  // key -> value
  Span bracketSpan;
};

struct SetLitExpr : Expr {
  SetLitExpr() : Expr(ExprKind::SetLit) {}
  std::vector<ExprPtr> elements;
  Span bracketSpan;
};

struct FieldExpr : Expr {
  FieldExpr() : Expr(ExprKind::Field) {}
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

// `object?.field`: None propagates, Some(struct) accesses `field` and wraps it.
struct SafeNavExpr : Expr {
  SafeNavExpr() : Expr(ExprKind::SafeNav) {}
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

struct ArrayLitExpr : Expr {
  ArrayLitExpr() : Expr(ExprKind::ArrayLit) {}
  std::vector<ExprPtr> elements;
};

struct RangeExpr : Expr {
  RangeExpr() : Expr(ExprKind::Range) {}
  ExprPtr start;
  ExprPtr end;
  ExprPtr step;  // may be null (defaults to 1)
};

struct TernaryExpr : Expr {
  TernaryExpr() : Expr(ExprKind::Ternary) {}
  ExprPtr cond;
  ExprPtr thenValue;
  ExprPtr elseValue;
};

struct TupleExpr : Expr {
  TupleExpr() : Expr(ExprKind::Tuple) {}
  std::vector<ExprPtr> elements;
};

struct LambdaExpr : Expr {
  LambdaExpr() : Expr(ExprKind::Lambda) {}
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  ExprPtr body;
};

struct ComprehensionExpr : Expr {
  ComprehensionExpr() : Expr(ExprKind::Comprehension) {}
  ExprPtr element;
  std::string var;
  Span varSpan;
  ExprPtr iterable;
  ExprPtr condition;  // may be null
  bool isSet = false;  // `{expr for ...}` builds a set; `[expr for ...]` a list
};

struct Pattern {
  enum class Kind { Literal, Range, Wildcard, Variant, Binding, Tuple, Or };
  Kind kind = Kind::Wildcard;
  Span span;
  ExprPtr value;       // Literal patterns
  ExprPtr rangeStart;  // Range patterns (inclusive)
  ExprPtr rangeEnd;
  ExprPtr ctor;                    // Variant patterns: dotted FieldExpr chain, e.g. Shape.Circle
  std::vector<Pattern> subpatterns;  // Variant/Tuple/Or sub-patterns
  std::string name;                // Binding patterns
};

struct MatchArm {
  Pattern pattern;
  ExprPtr guard;   // may be null
  ExprPtr result;
};

struct MatchExpr : Expr {
  MatchExpr() : Expr(ExprKind::Match) {}
  ExprPtr subject;
  std::vector<MatchArm> arms;
};

struct TryExpr : Expr {
  TryExpr() : Expr(ExprKind::Try) {}
  ExprPtr operand;
};

// ---------------------------------------------------------------- statements

struct Stmt {
  Span span;
  StmtKind kind;
  explicit Stmt(StmtKind k) : kind(k) {}
  virtual ~Stmt() = default;
};

struct LetStmt : Stmt {
  LetStmt() : Stmt(StmtKind::Let) {}
  std::string name;
  Span nameSpan;
  TypeExprPtr type;  // may be null
  bool mutableBinding = false;
  ExprPtr init;
};

struct AssignStmt : Stmt {
  AssignStmt() : Stmt(StmtKind::Assign) {}
  std::string op;  // "=", "+=", ...
  BinOp binop = BinOp::Invalid;  // base operator for compound assignment
  Span opSpan;
  ExprPtr target;
  ExprPtr value;
};

struct ExprStmt : Stmt {
  ExprStmt() : Stmt(StmtKind::Expr) {}
  ExprPtr expr;
};

struct DestructureStmt : Stmt {
  DestructureStmt() : Stmt(StmtKind::Destructure) {}
  std::vector<std::string> names;  // "_" discards the slot
  std::vector<Span> nameSpans;
  bool mutableBinding = false;
  ExprPtr value;
};

struct ReturnStmt : Stmt {
  ReturnStmt() : Stmt(StmtKind::Return) {}
  ExprPtr value;  // may be null
};

struct BreakStmt : Stmt {
  BreakStmt() : Stmt(StmtKind::Break) {}
};
struct ContinueStmt : Stmt {
  ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct DeferStmt : Stmt {
  DeferStmt() : Stmt(StmtKind::Defer) {}
  StmtPtr body;
};

// `import "lib/util.a"`, `import "lib/util.a" as u`, `import math`, or
struct ImportStmt : Stmt {
  ImportStmt() : Stmt(StmtKind::Import) {}
  std::string path;        // quoted path, empty for native/bare/python imports
  std::string alias;       // explicit alias, or empty
  std::string bareName;    // identifier for bare imports
  std::string pythonModule;  // Python module path for `python:<package>`
  std::string stdModule;     // stdlib package for `std:<package>`
  bool quoted = false;
  bool nativeModule = false;  // resolved to a built-in native module
  bool pythonModuleImport = false;
  bool stdModuleImport = false;
  int moduleId = -1;          // resolved module id
  Span targetSpan;
};

// `from "lib/util.a" import add, Vec as Point` or `from python:<pkg> import x`.
struct FromImportStmt : Stmt {
  FromImportStmt() : Stmt(StmtKind::FromImport) {}
  struct Item {
    std::string name;
    std::string alias;
    Span span;
  };
  std::string path;
  std::string bareName;
  std::string pythonModule;
  std::string stdModule;
  bool quoted = false;
  bool nativeModule = false;
  bool pythonModuleImport = false;
  bool stdModuleImport = false;
  int moduleId = -1;
  std::vector<Item> items;
  Span targetSpan;
};

struct IfStmt : Stmt {
  IfStmt() : Stmt(StmtKind::If) {}
  struct Branch {
    ExprPtr cond;
    std::vector<StmtPtr> body;
  };
  std::vector<Branch> branches;
  std::vector<StmtPtr> elseBody;
};

struct WhileStmt : Stmt {
  WhileStmt() : Stmt(StmtKind::While) {}
  ExprPtr cond;
  std::vector<StmtPtr> body;
};

struct DoWhileStmt : Stmt {
  DoWhileStmt() : Stmt(StmtKind::DoWhile) {}
  std::vector<StmtPtr> body;
  ExprPtr cond;
};

struct ForStmt : Stmt {
  ForStmt() : Stmt(StmtKind::For) {}
  std::string var;
  Span varSpan;
  ExprPtr iterable;
  std::vector<StmtPtr> body;
};

struct LoopStmt : Stmt {
  LoopStmt() : Stmt(StmtKind::Loop) {}
  std::vector<StmtPtr> body;
};

struct ComptimeParam {
  std::string name;
  Span span;
  TypeExprPtr type;
};

struct FnDecl : Stmt {
  FnDecl() : Stmt(StmtKind::FnDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::vector<ComptimeParam> comptimeParams;  // compile-time params: comptime N: int
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  std::vector<std::string> whereClause;  // trait constraints after `where`
  Span whereSpan;
  std::vector<StmtPtr> body;
  int moduleId = -1;  // module owning the function, filled in by Sema
  // Docstring attached from a string-literal statement inside an enclosing
  // type/impl body (methods only). Top-level and body docstrings are recovered
  // from the statement stream instead.
  std::string doc;
};

struct FieldDecl {
  std::string name;
  Span span;
  TypeExprPtr type;
  std::string doc;  // docstring from a preceding string-literal statement
};

struct StructDecl : Stmt {
  StructDecl() : Stmt(StmtKind::StructDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::string baseClass;  // base class name if class(Base)
  std::string metaclass;  // metaclass name if specified
  std::vector<FieldDecl> fields;
  std::vector<std::unique_ptr<FnDecl>> methods;
  std::string doc;  // docstring: first string-literal statement in the body
};

struct EnumVariant {
  std::string name;
  Span span;
  ExprPtr value;  // may be null (auto-incremented from the previous variant)
  std::vector<FieldDecl> fields;  // non-empty for payload (algebraic) variants
  std::string doc;  // docstring from a preceding string-literal statement
};

struct EnumDecl : Stmt {
  EnumDecl() : Stmt(StmtKind::EnumDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::vector<EnumVariant> variants;
  std::string doc;  // docstring: first string-literal statement in the body

  bool isAlgebraic() const {
    if (!typeParams.empty()) return true;
    for (const EnumVariant& v : variants)
      if (!v.fields.empty()) return true;
    return false;
  }
};

// One method signature inside a `trait` body. Traits describe behaviour; a
// conforming struct supplies the bodies (see ImplDecl).
struct TraitSig {
  std::string name;
  Span span;
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  std::string doc;  // docstring from a preceding string-literal statement
};

// `trait Equatable:` declares a set of method signatures. A struct opts in with
// `impl Equatable for Point:`; Sema then verifies the struct provides every
// method and records the conformance (see Sema::checkImpl).
struct TraitDecl : Stmt {
  TraitDecl() : Stmt(StmtKind::TraitDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::vector<TraitSig> methods;
  std::vector<std::string> parents;       // trait inheritance: trait B(A)
  std::vector<std::string> whereClause;   // `trait C where A` constraints
  std::string doc;  // docstring: first string-literal statement in the body
};

// `impl Trait for Type:` (optionally with a body of methods). The body is
// parsed so future work can attach foreign methods; conformance checking
// currently looks at the target struct's own methods.
struct ImplDecl : Stmt {
  ImplDecl() : Stmt(StmtKind::ImplDecl) {}
  std::string traitName;
  Span traitSpan;
  std::string targetName;
  Span targetSpan;
  std::vector<std::unique_ptr<FnDecl>> methods;
};

struct TypeAliasDecl : Stmt {
  TypeAliasDecl() : Stmt(StmtKind::TypeAlias) {}
  std::string name;
  Span nameSpan;
  TypeExprPtr type;
};

struct Program {
  std::vector<StmtPtr> statements;
};

// If `expr` is a plain string literal (no interpolation parts), writes its
// decoded text to `out` and returns true. Docstrings are recognized through
// this so interpolated strings never count as documentation.
inline bool plainStringText(const ExprPtr& expr, std::string& out) {
  if (!expr || expr->kind != ExprKind::StringLit) return false;
  const auto& str = static_cast<const StringLitExpr&>(*expr);
  std::string text;
  for (const StringPart& part : str.parts) {
    if (part.isExpr) return false;
    text += part.text;
  }
  out = std::move(text);
  return true;
}

// If `stmt` is a bare string-literal expression statement (a docstring),
// writes its text to `out` and returns true.
inline bool docstringText(const Stmt& stmt, std::string& out) {
  if (stmt.kind != StmtKind::Expr) return false;
  return plainStringText(static_cast<const ExprStmt&>(stmt).expr, out);
}

}  // namespace a
