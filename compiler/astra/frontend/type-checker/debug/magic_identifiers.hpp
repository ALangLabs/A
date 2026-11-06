#pragma once

#include <optional>
#include <string>

#include "ast/ast.hpp"
#include "diagnostics/diagnostics.hpp"
#include "type-checker/types.hpp"

namespace a::debug_magic {

inline std::optional<TypePtr> inferMagicIdentifier(const std::string& name) {
  if (name == "__file__" || name == "__source_file__") return typeString();
  if (name == "__line__" || name == "__source_line__") return typeInt(64, true);
  if (name == "__func__" || name == "__function__") return typeString();
  return std::nullopt;
}

inline std::string sourceFileName(const DiagnosticSink& sink, Span span) {
  if (const SourceFile* source = sink.file(span.fileId)) return source->path();
  return "<unknown>";
}

inline std::string functionDisplayName(const std::string& qualified) {
  if (qualified.empty()) return "<module>";
  const std::size_t sep = qualified.rfind("::");
  return sep == std::string::npos ? qualified : qualified.substr(sep + 2);
}

}  // namespace a::debug_magic
