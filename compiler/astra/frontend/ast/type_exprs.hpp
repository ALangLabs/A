#pragma once

#include "ast/fwd.hpp"

namespace a {

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

// Union type written with `|`: `int | str`, `int | str | None`. Members are
// stored flat in source order; `None` members are folded into an Optional by
// sema (so `int | None` means `int?`).
struct UnionTypeExpr : TypeExpr {
  std::vector<TypeExprPtr> elements;
};

// `...` in type-argument position: the variadic marker in `tuple[T, ...]`.
struct EllipsisTypeExpr : TypeExpr {};

// Integer literal in type-argument position: the `4` in `vector[float, 4]`.
// Only meaningful to types that take a comptime lane count; other consumers
// treat it as an error.
struct ConstTypeExpr : TypeExpr {
  std::int64_t value = 0;
};

struct ParamDecl {
  std::string name;
  Span span;
  TypeExprPtr type;  // may be null for inferred parameter types
  ExprPtr defaultValue;  // `fn f(x = 1)`; null when the parameter is required
  bool variadic = false;  // `fn f(*args)`; collects extra args into a list
};

}  // namespace a
