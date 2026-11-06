#include "diagnostics/diagnostics.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace a {
namespace {

struct Colors {
  const char* reset;
  const char* bold;
  const char* red;
  const char* yellow;
  const char* blue;
};

const Colors kPlain = {"", "", "", "", ""};
const Colors kAnsi = {"\x1b[0m", "\x1b[1m", "\x1b[31m", "\x1b[33m", "\x1b[34m"};

std::string repeat(char c, int count) {
  if (count <= 0) return {};
  return std::string(static_cast<std::size_t>(count), c);
}

std::string escapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
          out += hex[static_cast<unsigned char>(c) & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

FileId DiagnosticSink::addFile(std::shared_ptr<const SourceFile> source) {
  std::lock_guard<std::mutex> lock(mutex_);
  files_.push_back(std::move(source));
  return static_cast<FileId>(files_.size()) - 1;
}

const SourceFile* DiagnosticSink::file(FileId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return fileLocked(id);
}

const SourceFile* DiagnosticSink::fileLocked(FileId id) const {
  if (id >= 0 && static_cast<std::size_t>(id) < files_.size()) return files_[id].get();
  return files_.empty() ? nullptr : files_.front().get();
}

void DiagnosticSink::error(Span span, std::string message, std::string help) {
  errorWithCode(span, "", std::move(message), std::move(help));
}

void DiagnosticSink::errorWithCode(Span span, std::string code, std::string message, std::string help) {
  if (isSuppressed(span, Severity::Error, code)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.push_back(
      Diagnostic{Severity::Error, std::move(code), span, std::move(message), std::move(help)});
  ++errors_;
}

void DiagnosticSink::warning(Span span, std::string code, std::string message,
                             std::string help) {
  if (isSuppressed(span, Severity::Warning, code)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.push_back(
      Diagnostic{Severity::Warning, std::move(code), span, std::move(message), std::move(help)});
}

std::vector<Diagnostic> DiagnosticSink::diagnosticsCopy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}

bool DiagnosticSink::isSuppressed(Span span, const std::string& code) const {
  return isSuppressed(span, Severity::Error, code);
}

bool DiagnosticSink::isSuppressed(Span span, Severity, const std::string& code) const {
  if (!span.valid()) return false;
  const SourceFile* source = file(span.fileId);
  if (!source) return false;
  std::string line = source->lineText(span.line);
  std::size_t hash = line.find('#');
  if (hash == std::string::npos) return false;
  std::string comment = line.substr(hash);
  const char* markers[] = {"# a: ignore", "# type: ignore", "# ty: ignore",
                           "# pyrefly: ignore", "# mypy: ignore"};
  bool matched = false;
  for (const char* m : markers) {
    if (comment.find(m) != std::string::npos) { matched = true; break; }
  }
  if (!matched) return false;
  // Optional `[CODE]` filter: `# a: ignore[W0001]`.
  std::size_t open = comment.find('[');
  if (open != std::string::npos) {
    std::size_t close = comment.find(']', open);
    if (close != std::string::npos) {
      std::string filter = comment.substr(open + 1, close - open - 1);
      // Multiple codes separated by commas/spaces.
      std::size_t start = 0;
      while (start < filter.size()) {
        std::size_t comma = filter.find_first_of(", ", start);
        std::string one = filter.substr(start, comma == std::string::npos ? comma : comma - start);
        if (!one.empty() && one == code) return true;
        if (!one.empty() && code.empty()) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      return false;
    }
  }
  return true;
}

void DiagnosticSink::flush(std::ostream& os, bool useColor) {
  std::lock_guard<std::mutex> lock(mutex_);
  const Colors& colors = useColor ? kAnsi : kPlain;
  for (; flushed_ < diagnostics_.size(); ++flushed_) {
    const Diagnostic& diagnostic = diagnostics_[flushed_];
    const bool isError = diagnostic.severity == Severity::Error;

    os << colors.bold << (isError ? colors.red : colors.yellow)
       << (isError ? "error" : "warning");
    if (!diagnostic.code.empty()) os << "[" << diagnostic.code << "]";
    os << colors.reset << colors.bold << ": " << diagnostic.message << colors.reset << "\n";

    if (!diagnostic.span.valid()) continue;

    const SourceFile* source = fileLocked(diagnostic.span.fileId);
    if (!source) continue;

    const std::string lineNumber = std::to_string(diagnostic.span.line);
    const int gutter = static_cast<int>(lineNumber.size());
    os << colors.blue << "  --> " << colors.reset << source->path() << ":"
       << diagnostic.span.line << ":" << diagnostic.span.col << "\n";
    os << colors.blue << repeat(' ', gutter + 1) << "|\n" << colors.reset;

    const std::string text = source->lineText(diagnostic.span.line);
    os << colors.blue << lineNumber << " | " << colors.reset << text << "\n";
    os << colors.blue << repeat(' ', gutter + 1) << "| " << colors.reset;

    const int caretCol = std::max(1, diagnostic.span.col);
    int width = 1;
    if (diagnostic.span.endLine == diagnostic.span.line &&
        diagnostic.span.endCol > diagnostic.span.col) {
      width = diagnostic.span.endCol - diagnostic.span.col;
    }
    width = std::max(1, std::min(width, static_cast<int>(text.size()) - caretCol + 2));
    os << repeat(' ', caretCol - 1) << (isError ? colors.red : colors.yellow)
       << repeat('^', width) << colors.reset << "\n";

    if (!diagnostic.help.empty()) {
      os << colors.blue << repeat(' ', gutter + 1) << "| " << colors.reset << "\n";
      os << colors.blue << repeat(' ', gutter + 1) << "= " << colors.reset << colors.bold
         << "help:" << colors.reset << " " << diagnostic.help << "\n";
    }
    os << "\n";
  }
  os.flush();
}

void DiagnosticSink::writeJson(std::ostream& os) const {
  std::lock_guard<std::mutex> lock(mutex_);
  os << "{\"diagnostics\":[";
  bool first = true;
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (!first) os << ",";
    first = false;
    const SourceFile* source = fileLocked(diagnostic.span.fileId);
    os << "{\"severity\":\""
       << (diagnostic.severity == Severity::Error ? "error" : "warning") << "\""
       << ",\"code\":\"" << escapeJson(diagnostic.code) << "\""
       << ",\"file\":\"" << escapeJson(source ? source->path() : std::string()) << "\""
       << ",\"line\":" << diagnostic.span.line << ",\"column\":" << diagnostic.span.col
       << ",\"message\":\"" << escapeJson(diagnostic.message) << "\""
       << ",\"help\":\"" << escapeJson(diagnostic.help) << "\"}";
  }
  os << "]}\n";
  os.flush();
}

}  // namespace a
