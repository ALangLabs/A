#pragma once

#include <optional>
#include <string>

#include "ast/ast.hpp"
#include "diagnostics/diagnostics.hpp"
#include "type-checker/types.hpp"

namespace a::debug_magic {

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
