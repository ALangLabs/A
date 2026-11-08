#pragma once

#include "ast/builtins.hpp"
#include "ast/fwd.hpp"
#include "ast/kinds.hpp"
#include "ast/type_exprs.hpp"

namespace a {

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

  // Set by Sema when the callee resolved to a runtime builtin (either by its
  // bare name or a native-module from-import). Codegen must dispatch on it
  // directly instead of the ambiguous bare-name binding table, where an
  // unrelated module's function with the same name wins (e.g. a bare
  // `matmul(...)` call while `kernels` is imported).
  // `calleeBuiltin` holds the builtin name the runtime id is resolved from
  // (the from-import's native name, e.g. "unsafe_set" for an aliased
  // `from std:unsafe import set`).
  mutable bool calleeIsBuiltin = false;
  mutable std::string calleeBuiltin;
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
  // Set by sema: true when the accessed field is itself optional (so the
  // attribute value already is `None`/`Some(...)` and must not be wrapped
  // again). Keeps `a?.b` flattening instead of producing `Some(None)`.
  bool fieldOptional = false;
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
  std::vector<std::string> variantFields;  // payload field names (Variant patterns)
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

}  // namespace a
