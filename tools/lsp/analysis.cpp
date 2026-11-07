#include "analysis.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "name-resolution/builtin_docs.hpp"
#include "name-resolution/native_modules.hpp"

#include "analysis_internal.hpp"
namespace a::lsp {

const FileIndex* Compilation::entryIndex() const {
  const int entry = graph.entry >= 0 ? graph.entry : 0;
  return indexForModule(entry);
}

const FileIndex* Compilation::indexForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= indexes.size()) return nullptr;
  return &indexes[static_cast<std::size_t>(moduleId)];
}

const std::vector<Token>* Compilation::entryTokens() const {
  const int entry = graph.entry >= 0 ? graph.entry : 0;
  if (entry < 0 || static_cast<std::size_t>(entry) >= tokensByModule.size()) return nullptr;
  return &tokensByModule[static_cast<std::size_t>(entry)];
}

const std::vector<Comment>* Compilation::commentsForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= commentsByModule.size()) {
    return nullptr;
  }
  return &commentsByModule[static_cast<std::size_t>(moduleId)];
}

const std::string* Compilation::textForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= textsByModule.size()) return nullptr;
  return &textsByModule[static_cast<std::size_t>(moduleId)];
}

const std::string* Compilation::textForPath(const std::string& path) const {
  const std::string canonical = canonicalPath(path);
  for (std::size_t i = 0; i < graph.modules.size() && i < textsByModule.size(); ++i) {
    if (canonicalPath(graph.modules[i].path) == canonical) return &textsByModule[i];
  }
  return nullptr;
}

std::string typeExprText(const TypeExpr* type) {
  if (!type) return {};
  if (const auto* named = dynamic_cast<const NamedTypeExpr*>(type)) {
    std::string text;
    for (const std::string& qualifier : named->qualifiers) {
      if (!text.empty()) text += ".";
      text += qualifier;
    }
    if (!text.empty()) text += ".";
    return text + named->name;
  }
  if (const auto* array = dynamic_cast<const ArrayTypeExpr*>(type)) {
    return "[" + typeExprText(array->element.get()) + "]";
  }
  if (const auto* optional = dynamic_cast<const OptionalTypeExpr*>(type)) {
    const std::string inner = typeExprText(optional->element.get());
    if (dynamic_cast<const OptionalTypeExpr*>(optional->element.get()) != nullptr) {
      return "(" + inner + ")?";
    }
    return inner + "?";
  }
  if (const auto* tuple = dynamic_cast<const TupleTypeExpr*>(type)) {
    std::string text = "(";
    for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
      if (i) text += ", ";
      text += typeExprText(tuple->elements[i].get());
    }
    return text + ")";
  }
  if (const auto* fn = dynamic_cast<const FunctionTypeExpr*>(type)) {
    std::string text = "fn(";
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      if (i) text += ", ";
      text += typeExprText(fn->params[i].get());
    }
    text += ")";
    if (fn->returnType) text += " -> " + typeExprText(fn->returnType.get());
    return text;
  }
  return "?";
}

std::string signatureText(const FnDecl& decl) {
  std::string text = "fn " + decl.name + "(";
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    if (i) text += ", ";
    const ParamDecl& param = decl.params[i];
    text += param.name;
    if (param.type) text += ": " + typeExprText(param.type.get());
  }
  text += ")";
  if (decl.returnType) text += " -> " + typeExprText(decl.returnType.get());
  return text;
}

std::string describeSymbol(const SymbolDef& symbol) {
  if (!symbol.detail.empty()) return symbol.detail;
  return symbol.name;
}

}  // namespace a::lsp
