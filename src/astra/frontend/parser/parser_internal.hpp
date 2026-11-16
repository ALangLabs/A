#pragma once

#include "parser/parser.hpp"

#include <cctype>
#include <utility>

namespace a {

inline Span spanOf(const ExprPtr& expr) { return expr ? expr->span : Span{}; }

inline ExprPtr makeIdent(std::string name, Span span) {
  auto expr = std::make_unique<IdentExpr>();
  expr->name = std::move(name);
  expr->span = span;
  return expr;
}

inline ExprPtr makeFieldChain(const std::vector<std::string>& parts, const std::vector<Span>& spans) {
  ExprPtr object = makeIdent(parts.front(), spans.front());
  for (std::size_t i = 1; i < parts.size(); ++i) {
    auto field = std::make_unique<FieldExpr>();
    field->object = std::move(object);
    field->field = parts[i];
    field->fieldSpan = spans[i];
    field->span = Span::join(field->object->span, spans[i]);
    object = std::move(field);
  }
  return object;
}

}  // namespace a
