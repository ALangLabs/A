#pragma once

#include "parser/parser.hpp"

#include <cctype>
#include <utility>

namespace a {

inline Span spanOf(const ExprPtr& expr) { return expr ? expr->span : Span{}; }

// Routes docstrings inside a declaration body (struct/trait/enum/impl):
// the first docstring seen before any member becomes the declaration's own
// doc; later docstrings are held until the next member takes them via
// takePending().
struct PendingDoc {
  bool allowTypeDoc = true;
  std::string typeDoc;
  bool typeDocSet = false;
  std::string pending;

  void route(std::string doc, bool hasMembers) {
    if (allowTypeDoc && !hasMembers && !typeDocSet) {
      typeDoc = std::move(doc);
      typeDocSet = true;
    } else {
      pending = std::move(doc);
    }
  }

  // Returns the pending member doc (empty when none) and clears it.
  std::string takePending() { return std::exchange(pending, {}); }
};

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
