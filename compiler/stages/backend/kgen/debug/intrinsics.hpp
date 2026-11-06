#pragma once

#include <string>
#include <string_view>

#include "backend/kgen/debug/debug_format.hpp"

namespace a::debug_intrinsics {

inline std::string sourceFile(const DiagnosticSink& sink, Span span) {
  if (const SourceFile* source = sink.file(span.fileId)) return source->path();
  return "<unknown>";
}

inline std::int64_t sourceLine(Span span) { return static_cast<std::int64_t>(span.line); }

inline std::string sourceFunction(std::string_view fn) {
  if (fn.empty()) return "<module>";
  const std::size_t sep = fn.rfind("::");
  return sep == std::string_view::npos ? std::string(fn) : std::string(fn.substr(sep + 2));
}

inline std::string debugContext(const DiagnosticSink& sink, Span span, std::string_view fn) {
  return debugfmt::context(sink, span, sourceFunction(fn));
}

}  // namespace a::debug_intrinsics
