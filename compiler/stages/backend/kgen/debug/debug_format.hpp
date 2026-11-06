#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "backend/kgen/execution_engine_impl.hpp"

namespace a::debugfmt {

inline std::string basename(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? std::string(path) : std::string(path.substr(slash + 1));
}

inline std::string timeNow() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << tm.tm_hour << ':' << std::setw(2) << tm.tm_min
      << ':' << std::setw(2) << tm.tm_sec << '.' << std::setw(3) << ms.count();
  return out.str();
}

inline std::string alignContinuations(const std::string& text, std::size_t width) {
  if (text.find('\n') == std::string::npos) return text;
  std::string pad(width + 2, ' ');
  std::string out;
  out.reserve(text.size() + 16);
  bool first = true;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    std::string_view line = end == std::string::npos ? std::string_view(text).substr(start)
                                                      : std::string_view(text).substr(start, end - start);
    if (first) {
      out.append(line);
      first = false;
    } else if (line.empty()) {
      out.push_back('\n');
    } else {
      out.push_back('\n');
      out += pad;
      out.append(line);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

inline std::string context(const DiagnosticSink& sink, Span span, std::string_view functionName) {
  std::string file = "<unknown>";
  if (const SourceFile* source = sink.file(span.fileId)) file = basename(source->path());
  std::string out = file + ":" + std::to_string(span.line);
  if (!functionName.empty() && functionName != "<module>") out += " in " + std::string(functionName) + "()";
  return out;
}

}  // namespace a::debugfmt
