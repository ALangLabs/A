#pragma once

#include <cstdint>

namespace a {

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
  // Slice component inside a multi-index `x[a..b, i]`: start/end/step may be
  // null (open bound). Only produced by the parser inside IndexExpr brackets;
  // never a standalone value.
  SliceSpec,
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
  FloorDiv,
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

}  // namespace a
