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

struct ParamDecl {
  std::string name;
  Span span;
  TypeExprPtr type;  // may be null for inferred parameter types
};

}  // namespace a
