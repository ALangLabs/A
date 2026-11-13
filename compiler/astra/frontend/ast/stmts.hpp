#pragma once

#include "ast/exprs.hpp"
#include "ast/fwd.hpp"
#include "ast/kinds.hpp"
#include "ast/type_exprs.hpp"

namespace a {

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
  // `comptime if COND:` — the condition is evaluated at compile time and only
  // the taken branch is kept (see pipeline/comptime.cpp). Always cleared by
  // the time sema/backends see the node: an unevaluable condition degrades
  // to a regular runtime `if`.
  bool comptime = false;
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
  // `comptime for i in <compile-time iterable>:` — unrolled at compile time
  // by pipeline/comptime.cpp; cleared (degraded to a runtime `for`) when the
  // iterable is not a compile-time constant.
  bool comptime = false;
};

struct LoopStmt : Stmt {
  LoopStmt() : Stmt(StmtKind::Loop) {}
  std::vector<StmtPtr> body;
};

struct ComptimeParam {
  std::string name;
  Span span;
  TypeExprPtr type;
  // Index into the combined positional parameter list (comptime and regular
  // params interleaved in source order). The parser records it so call-site
  // specialization knows which argument binds this parameter.
  std::size_t position = 0;
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
  // Parallel to whereClause: the source span of each constraint name, used for
  // per-name diagnostics. Bounds written inline (`fn f[T: Trait]`) share the
  // same list, so their spans land here too.
  std::vector<Span> whereSpans;
  Span whereSpan;
  std::vector<StmtPtr> body;
  int moduleId = -1;  // module owning the function, filled in by Sema
  bool isEntry = false;  // `entry name():` — program entry point
  // `extern ["C"] fn name(...) -> T` — C FFI declaration. No body; calls
  // lower to a direct LLVM `call` on `externName` (defaults to `name`).
  bool isExtern = false;
  std::string externName;  // C symbol name; empty means use `name`
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
  std::vector<Span> whereSpans;           // parallel to whereClause
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
