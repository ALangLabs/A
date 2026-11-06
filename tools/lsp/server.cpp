#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "analysis.hpp"
#include "json.hpp"
#include "name-resolution/builtin_docs.hpp"
#include "name-resolution/native_modules.hpp"
#include "tooling/formatter.hpp"
#include "type-checker/method_signatures.hpp"

namespace {

namespace fs = std::filesystem;
using a::Diagnostic;
using a::Severity;
using a::Span;
using a::Tok;
using a::Token;
using a::lsp::Compilation;
using a::lsp::FileIndex;
using a::lsp::ResolvedSymbol;
using a::lsp::SymbolDef;
using a::lsp::SymbolKind;

using jsonlite::Value;

// --------------------------------------------------------------- source text

std::string canonicalPath(const std::string& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

std::string pathToUri(const std::string& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  std::string text = error ? path : absolute.string();
  std::replace(text.begin(), text.end(), '\\', '/');
  if (!text.empty() && text.front() != '/') text.insert(text.begin(), '/');

  static const char* hex = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      uri.push_back(static_cast<char>(c));
    } else {
      uri.push_back('%');
      uri.push_back(hex[(c >> 4) & 0xF]);
      uri.push_back(hex[c & 0xF]);
    }
  }
  return uri;
}

std::string uriToPath(const std::string& uri) {
  std::string rest = uri;
  const std::string prefix = "file://";
  if (rest.rfind(prefix, 0) == 0) rest = rest.substr(prefix.size());
  if (!rest.empty() && rest.front() != '/') {
    std::size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(slash);
  }

  std::string path;
  path.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
      auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      path.push_back(static_cast<char>(digit(rest[i + 1]) * 16 + digit(rest[i + 2])));
      i += 2;
    } else {
      path.push_back(rest[i]);
    }
  }
  return path;
}

std::string lineText(const std::string& text, int oneBasedLine) {
  if (oneBasedLine <= 0) return {};
  std::size_t line = 1;
  std::size_t start = 0;
  while (start < text.size() && line < static_cast<std::size_t>(oneBasedLine)) {
    std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) return {};
    start = newline + 1;
    ++line;
  }
  if (line != static_cast<std::size_t>(oneBasedLine)) return {};
  std::size_t end = text.find('\n', start);
  if (end == std::string::npos) end = text.size();
  if (end > start && text[end - 1] == '\r') --end;
  return text.substr(start, end - start);
}

int utf16Units(unsigned codePoint) { return codePoint > 0xFFFF ? 2 : 1; }

int byteToUtf16Column(const std::string& line, int zeroBasedByteColumn) {
  int byteColumn = std::max(0, zeroBasedByteColumn);
  int units = 0;
  std::size_t i = 0;
  while (i < line.size() && static_cast<int>(i) < byteColumn) {
    unsigned char c = static_cast<unsigned char>(line[i]);
    unsigned codePoint = 0xFFFD;
    std::size_t length = 1;
    if (c < 0x80) {
      codePoint = c;
    } else if ((c >> 5) == 0x6) {
      codePoint = c & 0x1F;
      length = 2;
    } else if ((c >> 4) == 0xE) {
      codePoint = c & 0x0F;
      length = 3;
    } else if ((c >> 3) == 0x1E) {
      codePoint = c & 0x07;
      length = 4;
    }
    for (std::size_t j = 1; j < length && i + j < line.size(); ++j) {
      codePoint = (codePoint << 6) | (static_cast<unsigned char>(line[i + j]) & 0x3F);
    }
    i += length;
    units += utf16Units(codePoint);
  }
  return units;
}

int utf16ToByteColumn(const std::string& line, int character) {
  int remaining = std::max(0, character);
  std::size_t i = 0;
  while (i < line.size() && remaining > 0) {
    unsigned char c = static_cast<unsigned char>(line[i]);
    unsigned codePoint = 0xFFFD;
    std::size_t length = 1;
    if (c < 0x80) {
      codePoint = c;
    } else if ((c >> 5) == 0x6) {
      codePoint = c & 0x1F;
      length = 2;
    } else if ((c >> 4) == 0xE) {
      codePoint = c & 0x0F;
      length = 3;
    } else if ((c >> 3) == 0x1E) {
      codePoint = c & 0x07;
      length = 4;
    }
    i += length;
    remaining -= utf16Units(codePoint);
  }
  return static_cast<int>(i);
}

Value positionValue(const std::string& text, Span span) {
  Value position = Value::object();
  int line = span.line > 0 ? span.line : 1;
  std::string textLine = lineText(text, line);
  position["line"] = line - 1;
  position["character"] = byteToUtf16Column(textLine, std::max(0, span.col - 1));
  return position;
}

Value rangeValue(const std::string& text, Span span) {
  Value range = Value::object();
  int startLine = span.line > 0 ? span.line : 1;
  int endLine = span.endLine > 0 ? span.endLine : startLine;
  int startCol = span.col > 0 ? span.col : 1;
  int endCol = span.endCol > 0 ? span.endCol : startCol + 1;

  range["start"] = positionValue(text, Span{startLine, startCol, startLine, startCol, span.fileId});
  range["end"] = positionValue(text, Span{endLine, endCol, endLine, endCol, span.fileId});
  return range;
}

Value locationValue(const Compilation& compilation, const std::string& path, Span span) {
  Value location = Value::object();
  location["uri"] = pathToUri(path);
  const std::string* text = compilation.textForPath(path);
  static const std::string empty;
  location["range"] = rangeValue(text ? *text : empty, span);
  return location;
}

std::vector<std::string> readStringArray(const Value* value) {
  std::vector<std::string> result;
  if (!value || !value->isArray()) return result;
  for (const Value& item : value->asArray()) {
    if (item.isString()) result.push_back(item.asString());
  }
  return result;
}

// --------------------------------------------------------------- shared docs
// Hover, completion, and signature help all read the tables that the type
// checker uses: builtinDocs() for interpreter builtins and the primitive
// method tables in method_signatures.hpp.

constexpr const char* kServerVersion = "0.0.1";

const std::vector<a::MethodSig>* methodTableForType(const std::string& typeName) {
  if (typeName == "array") return &a::arrayMethods();
  if (typeName == "map") return &a::mapMethods();
  if (typeName == "set") return &a::setMethods();
  if (typeName == "String") return &a::stringMethods();
  return nullptr;
}

const a::MethodSig* findMethodSig(const std::vector<a::MethodSig>& table,
                                  const std::string& name) {
  for (const a::MethodSig& method : table) {
    if (name == method.name) return &method;
  }
  return nullptr;
}

// Maps an import alias (`json`, or `import json as j` → `j`) to the native
// module it refers to; empty when the alias is not a native module.
std::string nativeModuleForAlias(const a::lsp::FileIndex& file, const std::string& alias) {
  for (const a::lsp::ImportDef& import : file.imports) {
    if (import.alias == alias && a::isNativeModule(import.modulePath)) return import.modulePath;
  }
  return {};
}

// Splits a signature string into its head, top-level parameters, and tail.
// For `fn name(a: i64, b: String) -> i64` this yields head `fn name`,
// params {`a: i64`, `b: String`}, tail ` -> i64`. Constants (`PI: f64`) and
// anything without parentheses return no parameters.
struct SignatureParts {
  std::string head;
  std::vector<std::string> params;
  std::string tail;
  bool callable = false;
};

SignatureParts splitSignature(const std::string& signature) {
  SignatureParts parts;
  const std::size_t open = signature.find('(');
  if (open == std::string::npos) {
    parts.head = signature;
    return parts;
  }
  int depth = 0;
  std::size_t close = std::string::npos;
  for (std::size_t i = open; i < signature.size(); ++i) {
    const char c = signature[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) { close = i; break; }
    }
  }
  if (close == std::string::npos) {
    parts.head = signature;
    return parts;
  }
  parts.callable = true;
  parts.head = signature.substr(0, open);
  parts.tail = signature.substr(close + 1);
  const std::string inside = signature.substr(open + 1, close - open - 1);
  std::string current;
  depth = 0;
  for (char c : inside) {
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      parts.params.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  const std::string last = current;
  bool blank = true;
  for (char c : last) if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
  if (!blank) parts.params.push_back(last);
  for (std::string& param : parts.params) {
    const std::size_t begin = param.find_first_not_of(" \t");
    const std::size_t end = param.find_last_not_of(" \t\r\n");
    param = (begin == std::string::npos) ? "" : param.substr(begin, end - begin + 1);
  }
  return parts;
}

// Pyright-style layout: signatures with 3+ parameters or longer than ~48
// characters render as a multi-line block with one indented parameter per
// line, e.g.
//   fn print(
//       value
//   )
// Short signatures stay on one line.
std::string formatSignature(const std::string& signature) {
  const SignatureParts parts = splitSignature(signature);
  if (!parts.callable || parts.params.empty()) return signature;
  if (parts.params.size() < 3 && signature.size() <= 48) return signature;
  std::string formatted = parts.head + "(\n";
  for (std::size_t i = 0; i < parts.params.size(); ++i) {
    formatted += "    " + parts.params[i];
    if (i + 1 < parts.params.size()) formatted += ",";
    formatted += "\n";
  }
  formatted += ")" + parts.tail;
  return formatted;
}

std::string trimLine(const std::string& line) {
  const std::size_t begin = line.find_first_not_of(" \t\r");
  if (begin == std::string::npos) return "";
  const std::size_t end = line.find_last_not_of(" \t\r\n");
  return line.substr(begin, end - begin + 1);
}

std::string lowerAscii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// Renders a docstring body with structured sections (Args:/Parameters:,
// Returns:, Example:/Examples:, See Also:) as Pyright/Pylance-style markdown.
// Plain text without section headings is returned unchanged.
std::string renderDocSections(const std::string& documentation) {
  if (documentation.empty()) return "";
  static const std::pair<const char*, const char*> kSections[] = {
      {"args:", "Args:"}, {"arguments:", "Args:"}, {"parameters:", "Parameters:"},
      {"params:", "Parameters:"}, {"returns:", "Returns:"}, {"return:", "Returns:"},
      {"example:", "Example:"}, {"examples:", "Example:"},
      {"see also:", "See Also:"}, {"note:", "Note:"},
  };
  std::vector<std::string> lines;
  {
    std::istringstream stream(documentation);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(trimLine(line));
  }
  auto headingFor = [](const std::string& line) -> const char* {
    const std::string lowered = lowerAscii(line);
    for (const auto& [prefix, label] : kSections) {
      if (lowered.rfind(prefix, 0) == 0) return label;
    }
    return nullptr;
  };
  bool anySection = false;
  for (const std::string& line : lines) if (headingFor(line)) anySection = true;
  if (!anySection) return documentation;

  const char* section = nullptr;
  bool exampleMode = false;
  std::string rendered;
  auto flushBlock = [&]() {
    if (!rendered.empty()) rendered += "\n";
  };
  for (std::string line : lines) {
    if (const char* label = headingFor(line)) {
      if (exampleMode) { rendered += "```\n"; exampleMode = false; }
      flushBlock();
      rendered += std::string("**") + label + "**";
      const std::size_t colon = line.find(':');
      const std::string rest = colon == std::string::npos ? "" : trimLine(line.substr(colon + 1));
      if (!rest.empty()) rendered += " " + rest;
      rendered += "\n";
      section = label;
      continue;
    }
    if (line.empty()) { rendered += "\n"; continue; }
    const bool isExample = section && std::string(section) == "Example:";
    if (isExample) {
      if (!exampleMode) { rendered += "```a\n"; exampleMode = true; }
      rendered += line + "\n";
      continue;
    }
    const bool isParams = section && (std::string(section) == "Args:" || std::string(section) == "Parameters:");
    if (isParams && line.rfind("- ", 0) != 0 && line.rfind("* ", 0) != 0) {
      rendered += "- " + line + "\n";
    } else {
      rendered += line + "\n";
    }
  }
  if (exampleMode) rendered += "```\n";
  while (!rendered.empty() && (rendered.back() == '\n' || rendered.back() == '\r')) rendered.pop_back();
  return rendered;
}

Value hoverMarkdown(const std::string& signature, const std::string& documentation,
                    const std::string& example = "", const std::string& seeAlso = "") {
  std::string markdown = "```a\n" + formatSignature(signature) + "\n```";
  const std::string body = renderDocSections(documentation);
  if (!body.empty()) markdown += "\n\n" + body;
  if (!example.empty()) {
    markdown += "\n\n**Example:**\n\n```a\n" + example + "\n```";
  }
  if (!seeAlso.empty()) {
    markdown += "\n\n**See Also:** ";
    bool first = true;
    std::size_t pos = 0;
    while (pos <= seeAlso.size()) {
      const std::size_t comma = seeAlso.find(',', pos);
      const std::string entry = trimLine(seeAlso.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
      if (!entry.empty()) {
        if (!first) markdown += ", ";
        markdown += "`" + entry + "`";
        first = false;
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  Value contents = Value::object();
  contents["kind"] = "markdown";
  contents["value"] = std::move(markdown);
  Value result = Value::object();
  result["contents"] = std::move(contents);
  return result;
}

// Matches the lexer keyword set (compiler/stages/lexer/token.hpp Kw*
// tokens) so semantic tokens color exactly the real keywords.
const char* const kKeywords[] = {
    "fn", "if", "elif", "else", "while", "do", "for", "in", "loop",
    "break", "continue", "return", "defer", "class", "metaclass", "enum",
    "self", "true", "false", "and", "or", "not", "import", "from", "as", "match",
    "None", "pass", "type", "trait", "impl", "comptime", "where",
};

int completionKind(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::Function: return 3;
    case SymbolKind::Method: return 2;
    case SymbolKind::Struct: return 22;
    case SymbolKind::Class: return 7;
    case SymbolKind::Enum: return 13;
    case SymbolKind::EnumMember: return 20;
    case SymbolKind::Field: return 5;
    case SymbolKind::Constant: return 21;
    case SymbolKind::Module: return 9;
    case SymbolKind::Interface: return 8;
    case SymbolKind::TypeParameter: return 25;
    default: return 6;
  }
}

Value symbolRange(const std::string& text, Span span) { return rangeValue(text, span); }

std::string simpleName(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

// Finds the symbol declared at `line`/`col` (1-based) in the module at `path`.
ResolvedSymbol findSymbolBySpan(const Compilation& compilation, const std::string& path,
                                int line, int col) {
  const std::string canonical = canonicalPath(path);
  for (const FileIndex& file : compilation.indexes) {
    if (canonicalPath(file.path) != canonical) continue;
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.span.line == line && symbol.span.col == col) {
        return ResolvedSymbol{&symbol, &file};
      }
    }
  }
  return {};
}

// Resolves a collected call site's callee token to its declaration, the same
// way resolveAt does for cursor positions.
ResolvedSymbol resolveCallCallee(const Compilation& compilation,
                                 const a::lsp::CallSite& site) {
  if (site.moduleId < 0 ||
      site.moduleId >= static_cast<int>(compilation.tokensByModule.size())) {
    return {};
  }
  const FileIndex* file = compilation.indexForModule(site.moduleId);
  if (!file) return {};
  const std::vector<Token>& tokens =
      compilation.tokensByModule[static_cast<std::size_t>(site.moduleId)];
  for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
    const Token& token = tokens[static_cast<std::size_t>(i)];
    if (token.kind != Tok::Ident) continue;
    if (token.span.line != site.calleeSpan.line || token.span.col != site.calleeSpan.col ||
        token.span.endCol != site.calleeSpan.endCol) {
      continue;
    }
    return a::lsp::resolveToken(compilation, *file, tokens, i);
  }
  return {};
}

// Myers O(ND) diff over source lines that keep their trailing '\n' (the last
// line has none). Returns hunks [oldStart,oldEnd) -> [newStart,newEnd) in
// line indices; applying them to `a` yields `b`.
std::vector<std::array<int, 4>> diffLineHunks(const std::vector<std::string>& a,
                                              const std::vector<std::string>& b) {
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  const int max = n + m;
  const int off = max;
  std::vector<int> v(2 * max + 1, 0);
  std::vector<std::vector<int>> trace;
  int d_final = -1;
  for (int d = 0; d <= max; ++d) {
    trace.push_back(v);
    for (int k = -d; k <= d; k += 2) {
      int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1]
                                                                       : v[off + k - 1] + 1;
      int y = x - k;
      while (x < n && y < m && a[x] == b[y]) {
        ++x;
        ++y;
      }
      v[off + k] = x;
      if (x >= n && y >= m) {
        d_final = d;
        break;
      }
    }
    if (d_final >= 0) break;
  }
  if (d_final <= 0) return {};

  // Backtrack: trace[d] is V before step d. Each step contributes one
  // insertion or deletion; adjacent steps merge into hunks below.
  std::vector<std::array<int, 4>> steps;
  int x = n;
  int y = m;
  for (int d = d_final; d > 0; --d) {
    const std::vector<int>& pv = trace[static_cast<std::size_t>(d)];
    const int k = x - y;
    const int prevK =
        (k == -d || (k != d && pv[off + k - 1] < pv[off + k + 1])) ? k + 1 : k - 1;
    const int prevX = pv[off + prevK];
    const int prevY = prevX - prevK;
    while (x > prevX && y > prevY) {
      --x;
      --y;
    }
    steps.push_back({prevX, x, prevY, y});
    x = prevX;
    y = prevY;
  }
  std::reverse(steps.begin(), steps.end());

  std::vector<std::array<int, 4>> hunks;
  for (const auto& step : steps) {
    if (!hunks.empty()) {
      auto& last = hunks.back();
      if (step[0] == last[1] && step[2] == last[3]) {
        last[1] = step[1];
        last[3] = step[3];
        continue;
      }
    }
    hunks.push_back(step);
  }
  return hunks;
}

// Splits text into lines that keep their trailing '\n'; a trailing newline
// produces a final empty part so EOF positions stay representable.
std::vector<std::string> splitLinesKeep(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, nl - start + 1));
    start = nl + 1;
  }
  if (text.empty() || text.back() == '\n') lines.push_back("");
  return lines;
}

// ------------------------------------------------------------ language server

struct Document {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
  int compiledVersion = -1;
  std::unique_ptr<Compilation> compilation;
};

class LanguageServer {
 public:
  LanguageServer() = default;

  int run() {
    startWorker();
    std::string body;
    while (readMessage(std::cin, body)) {
      std::string error;
      Value message = jsonlite::parse(body, &error);
      if (!message.isObject()) {
        sendError(Value(), -32700, "parse error: " + error);
        continue;
      }
      if (!handle(message)) break;
    }
    stopWorker();
    return shutdownReceived_ ? 0 : 1;
  }

 private:
  bool logging() const {
    static const bool enabled = std::getenv("A_LSP_LOG") != nullptr;
    return enabled;
  }

  void log(const std::string& message) {
    if (logging()) std::cerr << "[a-lsp] " << message << "\n";
  }

  void reply(const Value& id, Value result) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    writeMessage(response);
  }

  void replyNull(const Value& id) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = Value();
    writeMessage(response);
  }

  void sendError(const Value& id, int code, const std::string& message) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    Value error = Value::object();
    error["code"] = code;
    error["message"] = message;
    response["error"] = std::move(error);
    writeMessage(response);
  }

  void notify(const std::string& method, Value params) {
    Value message = Value::object();
    message["jsonrpc"] = "2.0";
    message["method"] = method;
    message["params"] = std::move(params);
    writeMessage(message);
  }

  static bool readMessage(std::istream& input, std::string& body) {
    std::size_t contentLength = 0;
    bool sawHeader = false;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) break;
      sawHeader = true;
      std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == "content-length") {
        try {
          contentLength = static_cast<std::size_t>(std::stoul(value));
        } catch (...) {
          return false;
        }
      }
    }
    if (!sawHeader && input.eof()) return false;
    if (contentLength == 0) return !input.eof();
    body.resize(contentLength);
    input.read(body.data(), static_cast<std::streamsize>(contentLength));
    return static_cast<std::size_t>(input.gcount()) == contentLength;
  }

  void writeMessage(const Value& message) {
    std::string body = message.dump();
    std::lock_guard<std::mutex> lock(outMutex_);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
    std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
    std::cout.flush();
  }

  bool handle(const Value& message) {
    const Value* methodValue = message.find("method");
    const Value* idValue = message.find("id");
    const Value* params = message.find("params");
    const std::string method = methodValue && methodValue->isString() ? methodValue->asString() : "";
    const bool isRequest = idValue != nullptr;

    if (method == "initialize") {
      handleInitialize(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "initialized") return true;
    if (method == "shutdown") {
      shutdownReceived_ = true;
      replyNull(idValue ? *idValue : Value());
      return true;
    }
    if (method == "exit") return false;
    if (method == "textDocument/didOpen") {
      didOpen(params);
      return true;
    }
    if (method == "textDocument/didChange") {
      didChange(params);
      return true;
    }
    if (method == "textDocument/didSave") {
      didSave(params);
      return true;
    }
    if (method == "textDocument/didClose") {
      didClose(params);
      return true;
    }
    if (method == "workspace/didChangeConfiguration") {
      handleConfiguration(params);
      return true;
    }
    if (method == "$/setTrace" || method == "$/cancelRequest" || method == "workspace/didChangeWatchedFiles") {
      return true;
    }

    // Async checking: notifications enqueue background compiles (200ms
    // debounce); requests that need the index synchronously bring the
    // document up to date on demand (stale = just-edited, worker pending).
    static const std::unordered_set<std::string> kNeedsCompilation = {
        "textDocument/definition", "textDocument/hover", "textDocument/completion",
        "textDocument/signatureHelp", "textDocument/references", "textDocument/documentSymbol",
        "textDocument/formatting", "textDocument/rename", "textDocument/documentHighlight",
        "textDocument/foldingRange", "textDocument/prepareRename",
        "textDocument/semanticTokens/full", "textDocument/semanticTokens/range",
        "textDocument/inlayHint", "textDocument/codeAction", "textDocument/selectionRange",
        "textDocument/declaration", "textDocument/typeDefinition",
        "textDocument/implementation", "textDocument/prepareCallHierarchy",
        "textDocument/prepareTypeHierarchy", "textDocument/codeLens",
        "textDocument/rangeFormatting", "textDocument/documentLink",
        "textDocument/diagnostic",
    };
    if (kNeedsCompilation.count(method)) {
      if (Document* document = documentFrom(params)) ensureCompiled(document);
    } else if (method == "workspace/symbol") {
      for (auto& entry : documents_) ensureCompiled(&entry.second);
    }

    if (method == "textDocument/definition") {
      definition(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/hover") {
      hover(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/completion") {
      completion(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "completionItem/resolve") {
      completionResolve(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/signatureHelp") {
      signatureHelp(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/references") {
      references(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/documentSymbol") {
      documentSymbols(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/formatting") {
      formatting(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/rename") {
      rename(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/documentHighlight") {
      documentHighlights(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/foldingRange") {
      foldingRanges(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "workspace/symbol") {
      workspaceSymbols(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/prepareRename") {
      prepareRename(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/semanticTokens/full" ||
        method == "textDocument/semanticTokens/range") {
      semanticTokens(idValue ? *idValue : Value(), params,
                     method == "textDocument/semanticTokens/range");
      return true;
    }
    if (method == "textDocument/inlayHint") {
      inlayHints(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/codeAction") {
      codeActions(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/selectionRange") {
      selectionRanges(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/declaration" || method == "textDocument/typeDefinition" ||
        method == "textDocument/implementation") {
      definition(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/prepareCallHierarchy") {
      prepareCallHierarchy(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "callHierarchy/incomingCalls") {
      incomingCalls(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "callHierarchy/outgoingCalls") {
      outgoingCalls(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/prepareTypeHierarchy") {
      prepareTypeHierarchy(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "typeHierarchy/supertypes") {
      typeHierarchyRelated(idValue ? *idValue : Value(), params, true);
      return true;
    }
    if (method == "typeHierarchy/subtypes") {
      typeHierarchyRelated(idValue ? *idValue : Value(), params, false);
      return true;
    }
    if (method == "textDocument/codeLens") {
      codeLens(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/rangeFormatting") {
      rangeFormatting(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/diagnostic") {
      documentDiagnostic(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/moniker") {
      moniker(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/documentLink") {
      documentLinks(idValue ? *idValue : Value(), params);
      return true;
    }

    if (isRequest) {
      sendError(*idValue, -32601, "method not found: " + method);
    }
    return true;
  }

  void handleInitialize(const Value& id, const Value* params) {
    modulePaths_.clear();
    std::string root;
    if (params) {
      if (const Value* options = params->find("initializationOptions")) {
        for (const std::string& path : readStringArray(options->find("modulePaths"))) {
          modulePaths_.push_back(path);
        }
        if (const Value* stdlib = options->find("stdlibRoot"); stdlib && stdlib->isString()) {
          stdlibRoot_ = stdlib->asString();
        }
      }
      if (const Value* folders = params->find("workspaceFolders"); folders && folders->isArray()) {
        for (const Value& folder : folders->asArray()) {
          if (const Value* uri = folder.find("uri"); uri && uri->isString()) {
            root = uriToPath(uri->asString());
            break;
          }
        }
      }
      if (root.empty()) {
        if (const Value* rootUri = params->find("rootUri"); rootUri && rootUri->isString()) {
          root = uriToPath(rootUri->asString());
        } else if (const Value* rootPath = params->find("rootPath");
                   rootPath && rootPath->isString()) {
          root = rootPath->asString();
        }
      }
    }
    if (!root.empty()) {
      modulePaths_.insert(modulePaths_.begin(), root);
      std::error_code error;
      fs::path build = fs::path(root) / "build";
      if (fs::is_directory(build, error)) modulePaths_.push_back(build.string());
    }

    Value capabilities = Value::object();
    capabilities["positionEncoding"] = "utf-16";
    Value sync = Value::object();
    sync["openClose"] = true;
    sync["change"] = 2;  // incremental sync (ty/pyrefly do the same)
    Value save = Value::object();
    save["includeText"] = true;
    sync["save"] = std::move(save);
    capabilities["textDocumentSync"] = std::move(sync);
    capabilities["definitionProvider"] = true;
    capabilities["declarationProvider"] = true;
    capabilities["typeDefinitionProvider"] = true;
    capabilities["implementationProvider"] = true;
    capabilities["referencesProvider"] = true;
    Value hover = Value::object();
    hover["workDoneProgress"] = false;
    capabilities["hoverProvider"] = std::move(hover);
    capabilities["documentSymbolProvider"] = true;
    capabilities["workspaceSymbolProvider"] = true;
    capabilities["documentFormattingProvider"] = true;
    capabilities["documentHighlightProvider"] = true;
    capabilities["foldingRangeProvider"] = true;
    capabilities["selectionRangeProvider"] = true;
    capabilities["inlayHintProvider"] = true;
    Value rename = Value::object();
    rename["prepareProvider"] = true;
    capabilities["renameProvider"] = std::move(rename);
    Value completion = Value::object();
    Value triggers = Value::array();
    triggers.push(".");
    triggers.push("\"");
    triggers.push(":");
    completion["triggerCharacters"] = std::move(triggers);
    completion["resolveProvider"] = true;
    capabilities["completionProvider"] = std::move(completion);
    Value signatureHelp = Value::object();
    Value signatureTriggers = Value::array();
    signatureTriggers.push("(");
    signatureTriggers.push(",");
    Value retrig = Value::array();
    retrig.push("(");
    retrig.push(",");
    signatureHelp["triggerCharacters"] = std::move(signatureTriggers);
    signatureHelp["retriggerCharacters"] = std::move(retrig);
    capabilities["signatureHelpProvider"] = std::move(signatureHelp);
    // Semantic tokens: legend [namespace,type,class,enum,function,method,
    // parameter,variable,property,keyword,comment,string,number,operator].
    Value tokenLegend = Value::object();
    Value tokenTypes = Value::array();
    for (const char* t : {"namespace", "type", "class", "enum", "function", "method",
                          "parameter", "variable", "property", "keyword", "comment",
                          "string", "number", "operator"}) tokenTypes.push(t);
    tokenLegend["tokenTypes"] = std::move(tokenTypes);
    Value tokenModifiers = Value::array();
    tokenModifiers.push("declaration");
    tokenModifiers.push("readonly");
    tokenModifiers.push("static");
    tokenLegend["tokenModifiers"] = std::move(tokenModifiers);
    Value semanticTokens = Value::object();
    semanticTokens["legend"] = std::move(tokenLegend);
    semanticTokens["full"] = true;
    semanticTokens["range"] = true;
    capabilities["semanticTokensProvider"] = std::move(semanticTokens);
    // Code actions: quick fixes and source fixes.
    Value codeAction = Value::object();
    Value actionKinds = Value::array();
    actionKinds.push("quickfix");
    actionKinds.push("refactor");
    actionKinds.push("source.fixAll");
    codeAction["codeActionKinds"] = std::move(actionKinds);
    capabilities["codeActionProvider"] = std::move(codeAction);
    Value callHierarchy = Value::object();
    callHierarchy["prepareProvider"] = true;
    capabilities["callHierarchyProvider"] = std::move(callHierarchy);
    Value typeHierarchy = Value::object();
    typeHierarchy["prepareProvider"] = true;
    capabilities["typeHierarchyProvider"] = std::move(typeHierarchy);
    Value codeLens = Value::object();
    codeLens["resolveProvider"] = false;
    capabilities["codeLensProvider"] = std::move(codeLens);
    capabilities["documentRangeFormattingProvider"] = true;
    Value diagnosticProvider = Value::object();
    diagnosticProvider["identifier"] = "a";
    diagnosticProvider["interFileDependencies"] = true;
    diagnosticProvider["workspaceDiagnostics"] = false;
    capabilities["diagnosticProvider"] = std::move(diagnosticProvider);
    capabilities["monikerProvider"] = true;
    Value documentLink = Value::object();
    documentLink["resolveProvider"] = false;
    capabilities["documentLinkProvider"] = std::move(documentLink);

    Value result = Value::object();
    result["capabilities"] = std::move(capabilities);
    Value info = Value::object();
    info["name"] = "a-language-server";
    info["version"] = kServerVersion;
    result["serverInfo"] = std::move(info);
    reply(id, std::move(result));
  }

  void handleConfiguration(const Value* params) {
    if (!params) return;
    const Value* settings = params->find("settings");
    if (!settings) return;
    const Value* aSettings = settings->find("a");
    if (!aSettings) return;
    if (const Value* paths = aSettings->find("modulePaths")) {
      modulePaths_ = readStringArray(paths);
    }
    if (const Value* stdlib = aSettings->find("stdlibRoot"); stdlib && stdlib->isString()) {
      stdlibRoot_ = stdlib->asString();
    }
    for (auto& entry : documents_) enqueueCompile(entry.second);
  }

  // ---- background (async) type checking ---------------------------------
  // didOpen/didChange only snapshot text and enqueue; a worker thread runs
  // analyzeDocument (Sema) and publishes diagnostics. Requests that need a
  // compilation call ensureCompiled() first, which synchronously fills any
  // staleness left by the debounce (like ty/pyrefly: async diagnostics,
  // request-time analysis for completeness).
  struct OpenDocSnapshot {
    std::string uri;
    std::string path;
    int version = 0;
    std::string text;
  };
  struct CompileJob {
    std::string uri;
    std::string path;
    std::string text;
    int version = 0;
    uint64_t generation = 0;
    std::vector<std::string> modulePaths;
    std::string stdlibRoot;
    std::vector<OpenDocSnapshot> open;
  };

  static constexpr int kDebounceMs = 200;

  void startWorker() {
    if (worker_.joinable()) return;
    worker_ = std::thread([this] { workerLoop(); });
  }

  void stopWorker() {
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      stop_ = true;
    }
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void enqueueCompile(Document& document) {
    CompileJob job;
    job.uri = document.uri;
    job.path = document.path;
    job.text = document.text;
    job.version = document.version;
    job.modulePaths = modulePaths_;
    job.stdlibRoot = stdlibRoot_;
    for (const auto& entry : documents_) {
      job.open.push_back(OpenDocSnapshot{entry.second.uri, entry.second.path,
                                         entry.second.version, entry.second.text});
    }
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      job.generation = ++generation_;
      pendingJob_ = std::move(job);
    }
    jobCv_.notify_all();
  }

  void workerLoop() {
    while (!stop_) {
      CompileJob job;
      {
        std::unique_lock<std::mutex> lock(jobMutex_);
        if (!jobCv_.wait_for(lock, std::chrono::milliseconds(kDebounceMs),
                             [&] { return stop_ || pendingJob_.has_value(); })) {
          continue;  // debounce settled with nothing pending
        }
        if (stop_ && !pendingJob_.has_value()) return;
        if (!pendingJob_.has_value()) continue;
        job = std::move(*pendingJob_);
        pendingJob_.reset();
      }
      auto compilation = a::lsp::analyzeDocument(job.path, job.text, job.modulePaths,
                                                  job.stdlibRoot);
      {
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (job.generation != generation_) continue;  // stale; a newer edit won
      }
      // Publish diagnostics for every open doc (mirrors publishDiagnosticsFor
      // but from snapshots so the worker never touches Document state).
      std::map<std::string, std::vector<const Diagnostic*>> byPath;
      for (const Diagnostic& diagnostic : compilation->sink.diagnostics()) {
        if (!diagnostic.span.valid()) continue;
        const a::SourceFile* source = compilation->sink.file(diagnostic.span.fileId);
        if (!source) continue;
        byPath[canonicalPath(source->path())].push_back(&diagnostic);
      }
      for (const OpenDocSnapshot& open : job.open) {
        Value params = Value::object();
        params["uri"] = open.uri;
        if (open.version > 0) params["version"] = open.version;
        Value diagnostics = Value::array();
        auto found = byPath.find(canonicalPath(open.path));
        if (found != byPath.end()) {
          for (const Diagnostic* diagnostic : found->second) {
            const a::SourceFile* source = compilation->sink.file(diagnostic->span.fileId);
            const std::string& text = source ? source->text() : open.text;
            Value item = Value::object();
            item["range"] = rangeValue(text, diagnostic->span);
            item["severity"] = diagnostic->severity == Severity::Error ? 1 : 2;
            item["message"] = diagnostic->message;
            item["source"] = "a";
            if (!diagnostic->code.empty()) {
              item["code"] = diagnostic->code;
              Value desc = Value::object();
              desc["href"] = "https://github.com/search?q=" + diagnostic->code + "+repo:own/a-lang";
              item["codeDescription"] = std::move(desc);
            }
            Value tags = Value::array();
            if (diagnostic->code == "W0002") tags.push(1);
            if (!tags.isArray() || tags.asArray().empty()) {} else item["tags"] = std::move(tags);
            if (!diagnostic->help.empty()) {
              Value related = Value::array();
              Value info = Value::object();
              info["location"] = locationValue(*compilation, open.path, diagnostic->span);
              info["message"] = diagnostic->help;
              related.push(std::move(info));
              item["relatedInformation"] = std::move(related);
            }
            diagnostics.push(std::move(item));
          }
        }
        params["diagnostics"] = std::move(diagnostics);
        notify("textDocument/publishDiagnostics", std::move(params));
      }
      log("compiled " + job.path + " (async, errors: " +
          std::to_string(compilation->sink.errorCount()) + ")");
    }
  }

  // Synchronously bring Document.compilation up to date on demand. Worker
  // results are diagnostics-only; request handlers need the full index.
  void ensureCompiled(Document* document) {
    if (!document) return;
    if (document->compilation && document->compiledVersion == document->version) return;
    document->compilation = a::lsp::analyzeDocument(document->path, document->text,
                                                     modulePaths_, stdlibRoot_);
    document->compiledVersion = document->version;
  }

  Document* documentFor(const std::string& uri) {
    auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
  }

  std::optional<std::pair<int, int>> positionFrom(const Value* params, int* lineOut,
                                                  int* characterOut) {
    if (!params) return std::nullopt;
    const Value* position = params->find("position");
    if (!position) return std::nullopt;
    const Value* line = position->find("line");
    const Value* character = position->find("character");
    if (!line || !character) return std::nullopt;
    *lineOut = static_cast<int>(line->asInt());
    *characterOut = static_cast<int>(character->asInt());
    return {{*lineOut, *characterOut}};
  }

  Document* documentFrom(const Value* params) {
    if (!params) return nullptr;
    const Value* textDocument = params->find("textDocument");
    if (!textDocument) return nullptr;
    const Value* uri = textDocument->find("uri");
    if (!uri || !uri->isString()) return nullptr;
    return documentFor(uri->asString());
  }

  void didOpen(const Value* params) {
    if (!params) return;
    const Value* textDocument = params->find("textDocument");
    if (!textDocument) return;
    const Value* uri = textDocument->find("uri");
    const Value* text = textDocument->find("text");
    if (!uri || !uri->isString() || !text || !text->isString()) return;

    Document document;
    document.uri = uri->asString();
    document.path = uriToPath(document.uri);
    document.text = text->asString();
    if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
      document.version = static_cast<int>(version->asInt());
    }
    log("open " + document.path);
    Document& stored = documents_[document.uri];
    stored = std::move(document);
    enqueueCompile(stored);
  }

  void didChange(const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !params) return;
    const Value* textDocument = params->find("textDocument");
    if (textDocument) {
      if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
        document->version = static_cast<int>(version->asInt());
      }
    }
    const Value* changes = params->find("contentChanges");
    if (!changes || !changes->isArray() || changes->asArray().empty()) return;
    // Incremental sync (kind=2): apply each change's range splice in order.
    // A change without a range is a full-document replacement (kind=1).
    for (const Value& change : changes->asArray()) {
      const Value* text = change.find("text");
      if (!text || !text->isString()) continue;
      const Value* range = change.find("range");
      if (!range) {
        document->text = text->asString();
        continue;
      }
      const Value* start = range->find("start");
      const Value* end = range->find("end");
      if (!start || !end) { document->text = text->asString(); continue; }
      const Value* sl = start->find("line");
      const Value* sc = start->find("character");
      const Value* el = end->find("line");
      const Value* ec = end->find("character");
      if (!sl || !sc || !el || !ec) { document->text = text->asString(); continue; }
      // Convert (utf-16 line/character) to byte offsets.
      auto offsetOf = [&](int line0, int char0) -> std::size_t {
        std::string lineContent = lineText(document->text, line0 + 1);
        int byteCol0 = utf16ToByteColumn(lineContent, char0);
        std::size_t pos = 0;
        for (int l = 0; l < line0; ++l) {
          std::size_t nl = document->text.find('\n', pos);
          if (nl == std::string::npos) return document->text.size();
          pos = nl + 1;
        }
        return pos + static_cast<std::size_t>(byteCol0);
      };
      std::size_t s = offsetOf(static_cast<int>(sl->asInt()), static_cast<int>(sc->asInt()));
      std::size_t e = offsetOf(static_cast<int>(el->asInt()), static_cast<int>(ec->asInt()));
      if (s > document->text.size()) s = document->text.size();
      if (e > document->text.size()) e = document->text.size();
      if (e < s) std::swap(s, e);
      document->text.replace(s, e - s, text->asString());
    }
    enqueueCompile(*document);
  }

  void didSave(const Value* params) {
    Document* document = documentFrom(params);
    if (!document) return;
    if (params) {
      const Value* text = params->find("text");
      if (text && text->isString()) document->text = text->asString();
    }
    enqueueCompile(*document);
  }

  void didClose(const Value* params) {
    Document* document = documentFrom(params);
    if (!document) return;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      ++generation_;  // cancel any pending compile for this document
      pendingJob_.reset();
    }
    publishDiagnostics(*document, {});
    documents_.erase(document->uri);
  }

  // Legacy synchronous path (no async publish). Handlers now use
  // ensureCompiled() which reuses the worker's debounce instead.
  void recompile(Document& document) {
    document.compilation = a::lsp::analyzeDocument(document.path, document.text, modulePaths_,
                                                    stdlibRoot_);
    document.compiledVersion = document.version;
    log("compiled " + document.path + " (sync, errors: " +
        std::to_string(document.compilation ? document.compilation->sink.errorCount() : 0) + ")");
    publishDiagnosticsFor(document);
  }

  void publishDiagnosticsFor(const Document& document) {
    if (!document.compilation) return;
    const Compilation& compilation = *document.compilation;

    std::map<std::string, std::vector<const Diagnostic*>> byPath;
    for (const Diagnostic& diagnostic : compilation.sink.diagnostics()) {
      if (!diagnostic.span.valid()) continue;
      const a::SourceFile* source = compilation.sink.file(diagnostic.span.fileId);
      if (!source) continue;
      byPath[canonicalPath(source->path())].push_back(&diagnostic);
    }

    for (auto& entry : documents_) {
      Document& open = entry.second;
      Value params = Value::object();
      params["uri"] = open.uri;
      if (open.version > 0) params["version"] = open.version;
      Value diagnostics = Value::array();

      auto found = byPath.find(canonicalPath(open.path));
      if (found != byPath.end()) {
        for (const Diagnostic* diagnostic : found->second) {
          const a::SourceFile* source = compilation.sink.file(diagnostic->span.fileId);
          const std::string& text = source ? source->text() : open.text;
          Value item = Value::object();
          item["range"] = rangeValue(text, diagnostic->span);
          item["severity"] = diagnostic->severity == Severity::Error ? 1 : 2;
          // Keep message bare; clients show `help` via relatedInformation
          // (ty/pic shows rich context the same way). We keep help appended
          // for clients without relatedInformation support.
          item["message"] = diagnostic->message;
          item["source"] = "a";
          if (!diagnostic->code.empty()) {
            item["code"] = diagnostic->code;
            Value desc = Value::object();
            desc["href"] = "https://github.com/search?q=" + diagnostic->code + "+repo:own/a-lang";
            item["codeDescription"] = std::move(desc);
          }
          Value tags = Value::array();
          if (diagnostic->code == "W0002") tags.push(1);  // Unnecessary
          if (!tags.isArray() || tags.asArray().empty()) {} else item["tags"] = std::move(tags);
          if (!diagnostic->help.empty()) {
            Value related = Value::array();
            Value info = Value::object();
            info["location"] = locationValue(compilation, open.path, diagnostic->span);
            info["message"] = diagnostic->help;
            related.push(std::move(info));
            item["relatedInformation"] = std::move(related);
          }
          diagnostics.push(std::move(item));
        }
      }
      params["diagnostics"] = std::move(diagnostics);
      notify("textDocument/publishDiagnostics", std::move(params));
    }
  }

  void publishDiagnostics(const Document& document, const std::vector<Value>& diagnostics) {
    Value params = Value::object();
    params["uri"] = document.uri;
    if (document.version > 0) params["version"] = document.version;
    Value array = Value::array();
    for (const Value& item : diagnostics) array.push(item);
    params["diagnostics"] = std::move(array);
    notify("textDocument/publishDiagnostics", std::move(params));
  }

  // Token lookup by (1-based line, 1-based byte column).
  //
  // Spans are half-open [col, endCol) (see lexer/span.hpp) and layout tokens
  // (Newline, Indent, Dedent) are zero-width; the lexer attaches a Newline to
  // the start of the following line (col 1..2) and Dedent tokens can sit at
  // line starts too. If they were considered, hovering the first character
  // of an identifier at line start (or a member right after '.') returned the
  // preceding token and hover gave up -- hover failed on the most common
  // stdlib calls (json.parse, math.sqrt, print at line start, ...). Skip
  // layout tokens and use half-open containment so the point belongs to the
  // token that starts there; matches resolveAt's identifier-only semantics.
  const Token* tokenAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
    for (const Token& token : tokens) {
      if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
          token.kind == Tok::Dedent) {
        continue;
      }
      if (token.span.line != line) continue;
      if (byteColumn < token.span.col || byteColumn >= token.span.endCol) continue;
      return &token;
    }
    return nullptr;
  }

  int tokenIndexAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const Token& token = tokens[i];
      if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
          token.kind == Tok::Dedent) {
        continue;
      }
      if (token.span.line != line) continue;
      if (byteColumn < token.span.col || byteColumn >= token.span.endCol) continue;
      return static_cast<int>(i);
    }
    return -1;
  }

  int tokenIndexBefore(const std::vector<Token>& tokens, int line, int byteColumn) const {
    int best = -1;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const Token& token = tokens[i];
      if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
          token.kind == Tok::Dedent) {
        continue;
      }
      if (token.span.line > line) break;
      if (token.span.line == line && token.span.col > byteColumn) break;
      best = static_cast<int>(i);
    }
    return best;
  }

  std::string extractDocstringAbove(const std::string& source, int declLine) const {
    // Look backward from line immediately above declLine for closing """ or '''.
    std::vector<std::string> lines;
    {
      std::istringstream stream(source);
      std::string l;
      while (std::getline(stream, l)) lines.push_back(l);
    }
    int checkLine = declLine - 2; // 0-based index for line before declLine
    while (checkLine >= 0 && trimLine(lines[static_cast<std::size_t>(checkLine)]).empty()) {
      --checkLine;
    }
    if (checkLine < 0) return {};
    const std::string& endLineText = lines[static_cast<std::size_t>(checkLine)];
    std::string quoteType;
    std::size_t closePos = endLineText.rfind("\"\"\"");
    if (closePos != std::string::npos) {
      quoteType = "\"\"\"";
    } else {
      closePos = endLineText.rfind("'''");
      if (closePos != std::string::npos) quoteType = "'''";
    }
    if (quoteType.empty()) return {};

    // Check if it was a single-line docstring: """doc"""
    if (closePos >= 3) {
      std::size_t openPos = endLineText.rfind(quoteType, closePos - 3);
      if (openPos != std::string::npos) {
        return trimLine(endLineText.substr(openPos + 3, closePos - openPos - 3));
      }
    }

    // Multi-line: scan backwards for the opening quote
    std::vector<std::string> docLines;
    docLines.push_back(endLineText.substr(0, closePos));
    int startLineIdx = checkLine - 1;
    bool foundOpen = false;
    while (startLineIdx >= 0) {
      const std::string& cur = lines[static_cast<std::size_t>(startLineIdx)];
      std::size_t openPos = cur.find(quoteType);
      if (openPos != std::string::npos) {
        docLines.push_back(cur.substr(openPos + 3));
        foundOpen = true;
        break;
      }
      docLines.push_back(cur);
      --startLineIdx;
    }
    if (!foundOpen) return {};
    std::reverse(docLines.begin(), docLines.end());

    std::size_t minIndent = std::string::npos;
    for (std::size_t i = 1; i < docLines.size(); ++i) {
      const std::string& l = docLines[i];
      const std::size_t nb = l.find_first_not_of(" \t\r\n");
      if (nb != std::string::npos) {
        if (minIndent == std::string::npos || nb < minIndent) minIndent = nb;
      }
    }
    if (minIndent == std::string::npos) minIndent = 0;

    std::string result;
    for (std::size_t i = 0; i < docLines.size(); ++i) {
      std::string l = docLines[i];
      while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
      if (i > 0 && l.size() >= minIndent) l = l.substr(minIndent);
      if (i == 0 && trimLine(l).empty()) continue;
      if (!result.empty()) result += "\n";
      result += l;
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
      result.pop_back();
    return result;
  }

  std::string extractDocstring(const std::string& source, int startLine) const {
    // startLine is 1-based line of the declaration (or body).
    // Look for """ or ''' starting on startLine or succeeding lines.
    std::istringstream stream(source);
    std::string line;
    int currentLine = 1;
    bool foundStart = false;
    std::string quoteType;
    std::vector<std::string> docLines;

    while (std::getline(stream, line)) {
      if (currentLine >= startLine) {
        if (!foundStart) {
          const std::size_t firstNonBlank = line.find_first_not_of(" \t\r\n");
          if (firstNonBlank != std::string::npos) {
            std::string rem = line.substr(firstNonBlank);
            if (rem.rfind("\"\"\"", 0) == 0) {
              quoteType = "\"\"\"";
            } else if (rem.rfind("'''", 0) == 0) {
              quoteType = "'''";
            }
            if (!quoteType.empty()) {
              foundStart = true;
              std::string after = rem.substr(3);
              const std::size_t closing = after.find(quoteType);
              if (closing != std::string::npos) {
                // Single-line docstring: """doc"""
                return trimLine(after.substr(0, closing));
              }
              docLines.push_back(after);
            } else if (currentLine > startLine + 1) {
              // Reached other statements without finding docstring
              break;
            }
          }
        } else {
          // Inside multi-line docstring
          const std::size_t closing = line.find(quoteType);
          if (closing != std::string::npos) {
            docLines.push_back(line.substr(0, closing));
            break;
          }
          docLines.push_back(line);
        }
      }
      ++currentLine;
    }

    if (!foundStart || docLines.empty()) return {};

    // Dedent common leading whitespace (like Python textwrap.dedent)
    std::size_t minIndent = std::string::npos;
    for (std::size_t i = 1; i < docLines.size(); ++i) {
      const std::string& l = docLines[i];
      const std::size_t nb = l.find_first_not_of(" \t\r\n");
      if (nb != std::string::npos) {
        if (minIndent == std::string::npos || nb < minIndent) minIndent = nb;
      }
    }
    if (minIndent == std::string::npos) minIndent = 0;

    std::string result;
    for (std::size_t i = 0; i < docLines.size(); ++i) {
      std::string l = docLines[i];
      while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
      if (i > 0 && l.size() >= minIndent) {
        l = l.substr(minIndent);
      }
      if (i == 0 && trimLine(l).empty()) continue;
      if (!result.empty()) result += "\n";
      result += l;
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
      result.pop_back();
    return result;
  }

  std::string docCommentText(const Compilation& compilation, const FileIndex& file,
                             const Span& span) const {
    if (!span.valid()) return {};
    int moduleId = -1;
    const std::string canonical = canonicalPath(file.path);
    for (std::size_t i = 0; i < compilation.graph.modules.size(); ++i) {
      if (canonicalPath(compilation.graph.modules[i].path) == canonical) {
        moduleId = static_cast<int>(i);
        break;
      }
    }

    // 1. Check for leading `##` doc comments above the symbol.
    const std::vector<a::Comment>* comments = compilation.commentsForModule(moduleId);
    if (comments) {
      std::vector<std::string> lines;
      int expected = span.line - 1;
      std::size_t start = comments->size();
      for (std::size_t i = 0; i < comments->size(); ++i) {
        if ((*comments)[i].span.line == expected) { start = i; break; }
        if ((*comments)[i].span.line > expected) break;
      }
      if (start < comments->size()) {
        for (std::size_t index = start + 1; index > 0; --index) {
          const a::Comment& comment = (*comments)[index - 1];
          if (!comment.doc || comment.trailing || comment.span.line != expected) break;
          std::string line = comment.text;
          while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
          if (line.rfind("##", 0) == 0) line.erase(0, 2);
          if (!line.empty() && line.front() == ' ') line.erase(0, 1);
          lines.push_back(std::move(line));
          --expected;
        }
        if (!lines.empty()) {
          std::string text;
          for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (!text.empty()) text += '\n';
            text += *it;
          }
          return text;
        }
      }
    }

    // 2. Check for Python-style `"""docstring"""` or `'''docstring'''` right
    // above or as the first statement in the function / class / struct body.
    const std::string* source = compilation.textForPath(file.path);
    if (source) {
      // Check immediately inside body (e.g. line span.line + 1)
      std::string ds = extractDocstring(*source, span.line + 1);
      if (!ds.empty()) return ds;
      // Or immediately above declaration
      if (span.line > 1) {
        ds = extractDocstringAbove(*source, span.line);
        if (!ds.empty()) return ds;
      }
    }

    return {};
  }

  static bool validIdentifier(const std::string& name) {
    if (name.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    for (char c : name) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    for (const char* keyword : kKeywords) {
      if (name == keyword) return false;
    }
    return true;
  }

  std::vector<std::pair<std::string, Span>> collectReferences(
      const Compilation& compilation, const ResolvedSymbol& target) const {
    std::vector<std::pair<std::string, Span>> references;
    if (!target.valid()) return references;
    const std::string targetPath = canonicalPath(target.file->path);
    for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
      if (moduleId >= compilation.tokensByModule.size()) continue;
      const FileIndex& file = compilation.indexes[moduleId];
      const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
      for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& token = tokens[static_cast<std::size_t>(i)];
        if (token.kind != Tok::Ident) continue;
        ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
        if (!resolved.valid()) continue;
        if (resolved.symbol->span.line != target.symbol->span.line) continue;
        if (resolved.symbol->span.col != target.symbol->span.col) continue;
        if (canonicalPath(resolved.file->path) != targetPath) continue;
        references.emplace_back(resolved.file->path, token.span);
      }
    }
    return references;
  }

  Value fullDocumentRange(const std::string& text) const {
    int lines = 0;
    for (char c : text) {
      if (c == '\n') ++lines;
    }
    std::string lastLine = lineText(text, lines + 1);
    Value range = Value::object();
    Value start = Value::object();
    start["line"] = 0;
    start["character"] = 0;
    Value end = Value::object();
    end["line"] = lines;
    end["character"] = byteToUtf16Column(lastLine, static_cast<int>(lastLine.size()));
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    return range;
  }

  void formatting(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);

    a::tooling::FormatOptions options;
    if (params) {
      if (const Value* formatOptions = params->find("options")) {
        if (const Value* tabSize = formatOptions->find("tabSize"); tabSize && tabSize->isNumber()) {
          const int size = static_cast<int>(tabSize->asInt());
          if (size >= 1 && size <= 16) options.indentWidth = size;
        }
      }
    }

    a::DiagnosticSink sink;
    sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
    a::tooling::FormatResult result =
        a::tooling::formatSource(document->text, sink, options);

    Value edits = Value::array();
    if (!result.ok || sink.hasErrors() || result.text == document->text) {
      reply(id, std::move(edits));
      return;
    }

    Value edit = Value::object();
    edit["range"] = fullDocumentRange(document->text);
    edit["newText"] = result.text;
    edits.push(std::move(edit));
    reply(id, std::move(edits));
  }

  void rename(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation || !params) return replyNull(id);
    const Value* newNameValue = params->find("newName");
    if (!newNameValue || !newNameValue->isString()) return replyNull(id);
    const std::string newName = newNameValue->asString();
    if (!validIdentifier(newName)) {
      sendError(id, -32602, "invalid identifier: " + newName);
      return;
    }

    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol target =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    const auto references = collectReferences(compilation, target);
    if (references.empty()) return replyNull(id);

    Value changes = Value::object();
    for (const auto& [path, span] : references) {
      const std::string uri = pathToUri(path);
      if (!changes.contains(uri)) changes[uri] = Value::array();
      const std::string* text = compilation.textForPath(path);
      static const std::string empty;
      Value edit = Value::object();
      edit["range"] = rangeValue(text ? *text : empty, span);
      edit["newText"] = newName;
      changes[uri].push(std::move(edit));
    }

    Value workspaceEdit = Value::object();
    workspaceEdit["changes"] = std::move(changes);
    reply(id, std::move(workspaceEdit));
  }

  void documentHighlights(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol target =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    Value highlights = Value::array();
    for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind != Tok::Ident) continue;
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, i);
      if (!resolved.valid()) continue;
      if (resolved.symbol->span.line != target.symbol->span.line) continue;
      if (resolved.symbol->span.col != target.symbol->span.col) continue;
      if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
      Value highlight = Value::object();
      highlight["range"] = rangeValue(document->text, token.span);
      highlight["kind"] = 1;  // Text
      highlights.push(std::move(highlight));
    }
    reply(id, std::move(highlights));
  }

  void foldingRanges(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const std::vector<Token>* tokens = document->compilation->entryTokens();
    if (!tokens) return replyNull(id);

    Value result = Value::array();
    std::vector<int> starts;
    int lastCodeLine = 0;
    auto addRange = [&](int startLine, int endLine) {
      if (startLine <= 0 || endLine <= startLine) return;
      Value range = Value::object();
      range["startLine"] = startLine - 1;
      range["endLine"] = endLine - 1;
      range["kind"] = "region";
      result.push(std::move(range));
    };

    for (const Token& token : *tokens) {
      switch (token.kind) {
        case Tok::Indent:
          starts.push_back(lastCodeLine > 0 ? lastCodeLine : token.span.line);
          break;
        case Tok::Dedent:
          if (!starts.empty()) {
            const int startLine = starts.back();
            starts.pop_back();
            addRange(startLine, lastCodeLine);
          }
          break;
        case Tok::Newline:
        case Tok::Eof:
          break;
        default:
          if (token.span.endLine > 0) lastCodeLine = token.span.endLine;
          break;
      }
    }
    while (!starts.empty()) {
      addRange(starts.back(), lastCodeLine);
      starts.pop_back();
    }
    reply(id, std::move(result));
  }

  void definition(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol resolved = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (resolved.valid()) {
      reply(id, locationValue(compilation, resolved.file->path, resolved.symbol->span));
      return;
    }

    // Pyright-style fallback: ctrl+click on the module alias in the import
    // statement (or as a member receiver, `utils.helper()`) opens the module.
    // `from m import n` resolves to the member when it can be found.
    const Token* token = tokenAt(*tokens, line + 1, byteColumn);
    if (token && token->kind == Tok::Ident) {
      auto importModulePath = [&](const a::lsp::ImportDef& def) -> const std::string* {
        if (def.moduleId < 0 ||
            static_cast<std::size_t>(def.moduleId) >= compilation.graph.modules.size()) {
          return nullptr;
        }
        return &compilation.graph.modules[static_cast<std::size_t>(def.moduleId)].path;
      };
      auto jumpToModule = [&](const a::lsp::ImportDef& def, const std::string& path) {
        if (!def.wholeModule && !def.member.empty()) {
          const a::lsp::FileIndex* target = compilation.indexForModule(def.moduleId);
          if (target) {
            for (const a::lsp::SymbolDef& sym : target->symbols) {
              if (sym.name == def.member) {
                reply(id, locationValue(compilation, path, sym.span));
                return true;
              }
            }
          }
        }
        reply(id, locationValue(compilation, path, Span{1, 1, 1, 2, 0}));
        return true;
      };
      // The cursor sits inside an import statement's own span. For a
      // from-import, `span` covers the member and `moduleSpan` the module.
      for (const a::lsp::ImportDef& def : file->imports) {
        auto covers = [&](const Span& span) {
          return span.valid() && span.line == line + 1 && byteColumn >= span.col &&
                 byteColumn < span.endCol;
        };
        if (covers(def.moduleSpan)) {
          const std::string* path = importModulePath(def);
          if (path) {
            reply(id, locationValue(compilation, *path, Span{1, 1, 1, 2, 0}));
            return;
          }
        }
        if (covers(def.span)) {
          const std::string* path = importModulePath(def);
          if (path && jumpToModule(def, *path)) return;
        }
      }
      // A whole-module alias used as the base of a member access.
      for (const a::lsp::ImportDef& def : file->imports) {
        if (!def.wholeModule || def.alias != token->lexeme) continue;
        const std::string* path = importModulePath(def);
        if (path && jumpToModule(def, *path)) return;
      }
    }
    return replyNull(id);
  }

  void hover(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol resolved = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (resolved.valid()) {
      std::string description = a::lsp::describeSymbol(*resolved.symbol);
      // Show the inferred type the way ty/pyrefly do (`x: i32`) when the
      // declaration has no explicit annotation.
      std::string inferred;
      for (const auto& var : compilation.sema.variables) {
        if (var.span.line == resolved.symbol->span.line &&
            var.span.col == resolved.symbol->span.col &&
            var.span.fileId == resolved.symbol->span.fileId && var.type) {
          std::string t = a::typeName(var.type);
          if (!t.empty() && t != "error" && description.find(": " + t) == std::string::npos) {
            bool annotated = resolved.symbol->detail.find(": ") != std::string::npos;
            if (!annotated || description.find(":") > description.find(t)) {
              description = resolved.symbol->name + ": " + t + "  (inferred)";
              if (!resolved.symbol->detail.empty() && resolved.symbol->detail != resolved.symbol->name)
                description += "\n" + resolved.symbol->detail;
            }
          }
          break;
        }
      }
      std::string markdown = "```a\n" + formatSignature(description) + "\n```";
      const std::string documentation =
          docCommentText(compilation, *resolved.file, resolved.symbol->span);
      const std::string body = renderDocSections(documentation);
      if (!body.empty()) markdown += "\n\n" + body;
      // Definition link, like pyrefly hover.
      if (resolved.file->path != file->path || resolved.symbol->span.line != line + 1) {
        markdown += "\n\n[Definition](file://" + resolved.file->path + "#L" +
                    std::to_string(resolved.symbol->span.line) + ")";
      }
      const std::string* text = compilation.textForPath(resolved.file->path);
      Value result = Value::object();
      Value contents = Value::object();
      contents["kind"] = "markdown";
      contents["value"] = markdown;
      result["contents"] = std::move(contents);
      if (text) result["range"] = rangeValue(*text, resolved.symbol->span);
      reply(id, std::move(result));
      return;
    }

    const Token* token = tokenAt(*tokens, line + 1, byteColumn);
    if (token && token->kind == Tok::Ident) {
      int index = tokenIndexAt(*tokens, line + 1, byteColumn);

      // `module.member` and primitive `receiver.method` forms.
      if (index >= 2) {
        const Token& dot = (*tokens)[static_cast<std::size_t>(index - 1)];
        const Token& baseToken = (*tokens)[static_cast<std::size_t>(index - 2)];
        if ((dot.kind == Tok::Dot || dot.kind == Tok::QuestionDot) &&
            baseToken.kind == Tok::Ident) {
          const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
          if (!moduleName.empty()) {
            const std::string* builtinName = a::nativeFunction(moduleName, token->lexeme);
            if (!builtinName) builtinName = a::nativeConstant(moduleName, token->lexeme);
            if (builtinName) {
              if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) {
                reply(id, hoverMarkdown(doc->signature, doc->documentation,
                                        doc->example ? doc->example : "",
                                        doc->seeAlso ? doc->seeAlso : ""));
                return;
              }
            }
          }
          ResolvedSymbol base =
              a::lsp::resolveToken(compilation, *file, *tokens, index - 2);
          if (base.valid()) {
            if (const std::vector<a::MethodSig>* table =
                    methodTableForType(base.symbol->typeName)) {
              if (const a::MethodSig* method = findMethodSig(*table, token->lexeme)) {
                reply(id, hoverMarkdown(method->signature, method->documentation,
                                        method->example ? method->example : ""));
                return;
              }
            }
          }
        }
      }

      if (!nativeModuleForAlias(*file, token->lexeme).empty()) {
        const std::string moduleName = nativeModuleForAlias(*file, token->lexeme);
        reply(id, hoverMarkdown("import " + moduleName,
                                "Native standard-library module."));
        return;
      }
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(token->lexeme)) {
        reply(id, hoverMarkdown(doc->signature, doc->documentation,
                                doc->example ? doc->example : "",
                                doc->seeAlso ? doc->seeAlso : ""));
        return;
      }
    }
    replyNull(id);
  }

  void references(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* entry = compilation.entryIndex();
    const std::vector<Token>* entryTokens = compilation.entryTokens();
    if (!entry || !entryTokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol target = a::lsp::resolveAt(compilation, *entry, *entryTokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    bool includeDeclaration = true;
    if (params) {
      if (const Value* context = params->find("context")) {
        if (const Value* include = context->find("includeDeclaration");
            include && include->isBool()) {
          includeDeclaration = include->asBool();
        }
      }
    }

    Value result = Value::array();
    for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
      const FileIndex& file = compilation.indexes[moduleId];
      if (moduleId >= compilation.tokensByModule.size()) continue;
      const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
      for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& token = tokens[static_cast<std::size_t>(i)];
        if (token.kind != Tok::Ident) continue;
        if (!includeDeclaration && token.span.line == target.symbol->span.line &&
            token.span.col == target.symbol->span.col &&
            token.span.endCol == target.symbol->span.endCol) {
          continue;
        }
        ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
        if (!resolved.valid()) continue;
        if (resolved.symbol->span.line != target.symbol->span.line) continue;
        if (resolved.symbol->span.col != target.symbol->span.col) continue;
        if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
        result.push(locationValue(compilation, resolved.file->path, token.span));
      }
    }
    reply(id, std::move(result));
  }

  Value documentSymbolFromStatement(const Compilation& compilation, const std::string& text,
                                    const a::Stmt& statement) {
    Value symbol = Value::object();
    switch (statement.kind) {
      case a::StmtKind::FnDecl: {
        const auto& fn = static_cast<const a::FnDecl&>(statement);
        symbol["name"] = fn.name;
        symbol["detail"] = a::lsp::signatureText(fn);
        symbol["kind"] = static_cast<int>(SymbolKind::Function);
        symbol["range"] = symbolRange(text, fn.span);
        symbol["selectionRange"] = symbolRange(text, fn.nameSpan);
        return symbol;
      }
      case a::StmtKind::StructDecl: {
        const auto& decl = static_cast<const a::StructDecl&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = "class";
        symbol["kind"] = static_cast<int>(SymbolKind::Class);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        Value children = Value::array();
        for (const auto& field : decl.fields) {
          Value child = Value::object();
          child["name"] = field.name;
          child["detail"] = a::lsp::typeExprText(field.type.get());
          child["kind"] = static_cast<int>(SymbolKind::Field);
          child["range"] = symbolRange(text, field.span);
          child["selectionRange"] = symbolRange(text, field.span);
          children.push(std::move(child));
        }
        for (const auto& method : decl.methods) {
          if (!method) continue;
          Value child = Value::object();
          child["name"] = method->name;
          child["detail"] = a::lsp::signatureText(*method);
          child["kind"] = static_cast<int>(SymbolKind::Method);
          child["range"] = symbolRange(text, method->span);
          child["selectionRange"] = symbolRange(text, method->nameSpan);
          children.push(std::move(child));
        }
        symbol["children"] = std::move(children);
        return symbol;
      }
      case a::StmtKind::EnumDecl: {
        const auto& decl = static_cast<const a::EnumDecl&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = "enum";
        symbol["kind"] = static_cast<int>(SymbolKind::Enum);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        Value children = Value::array();
        for (const auto& variant : decl.variants) {
          Value child = Value::object();
          child["name"] = variant.name;
          child["detail"] = decl.name + "." + variant.name;
          child["kind"] = static_cast<int>(SymbolKind::EnumMember);
          child["range"] = symbolRange(text, variant.span);
          child["selectionRange"] = symbolRange(text, variant.span);
          children.push(std::move(child));
        }
        symbol["children"] = std::move(children);
        return symbol;
      }
      case a::StmtKind::Let: {
        const auto& decl = static_cast<const a::LetStmt&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = a::lsp::typeExprText(decl.type.get());
        symbol["kind"] = static_cast<int>(SymbolKind::Variable);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        return symbol;
      }
      case a::StmtKind::Assign: {
        const auto& assign = static_cast<const a::AssignStmt&>(statement);
        if (!assign.target || assign.target->kind != a::ExprKind::Ident) return Value();
        const auto& ident = static_cast<const a::IdentExpr&>(*assign.target);
        symbol["name"] = ident.name;
        symbol["kind"] = static_cast<int>(SymbolKind::Variable);
        symbol["range"] = symbolRange(text, assign.span);
        symbol["selectionRange"] = symbolRange(text, ident.span);
        return symbol;
      }
      case a::StmtKind::Destructure: {
        const auto& decl = static_cast<const a::DestructureStmt&>(statement);
        if (decl.names.size() != 1) return Value();
        symbol["name"] = decl.names.front();
        symbol["kind"] = static_cast<int>(SymbolKind::Variable);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] =
            symbolRange(text, decl.nameSpans.empty() ? decl.span : decl.nameSpans.front());
        return symbol;
      }
      case a::StmtKind::TraitDecl: {
        const auto& decl = static_cast<const a::TraitDecl&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = "trait";
        symbol["kind"] = static_cast<int>(SymbolKind::Enum);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        Value children = Value::array();
        for (const auto& method : decl.methods) {
          Value child = Value::object();
          child["name"] = method.name;
          child["detail"] = decl.name + "." + method.name;
          child["kind"] = static_cast<int>(SymbolKind::Method);
          child["range"] = symbolRange(text, method.span);
          child["selectionRange"] = symbolRange(text, method.span);
          children.push(std::move(child));
        }
        symbol["children"] = std::move(children);
        return symbol;
      }
      default:
        return Value();
    }
    (void)compilation;
  }

  void documentSymbols(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
    if (entry < 0 || static_cast<std::size_t>(entry) >= compilation.graph.modules.size()) {
      return replyNull(id);
    }
    const a::Module& module = compilation.graph.modules[static_cast<std::size_t>(entry)];
    if (!module.program) return replyNull(id);

    Value result = Value::array();
    for (const auto& statement : module.program->statements) {
      if (!statement) continue;
      Value symbol = documentSymbolFromStatement(compilation, document->text, *statement);
      if (!symbol.isNull()) result.push(std::move(symbol));
    }
    reply(id, std::move(result));
  }

  void workspaceSymbols(const Value& id, const Value* params) {
    std::string query;
    if (params) {
      const Value* queryValue = params->find("query");
      if (queryValue && queryValue->isString()) query = queryValue->asString();
    }
    std::string lowered = query;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    Value result = Value::array();
    std::unordered_set<std::string> seen;
    for (const auto& entry : documents_) {
      const Document& document = entry.second;
      if (!document.compilation) continue;
      const FileIndex* file = document.compilation->entryIndex();
      if (!file) continue;
      for (const SymbolDef& symbol : file->symbols) {
        if (!symbol.topLevel) continue;
        std::string nameLower = symbol.name;
        for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!lowered.empty() && nameLower.find(lowered) == std::string::npos) continue;
        std::string key = file->path + ":" + std::to_string(symbol.span.line) + ":" +
                          std::to_string(symbol.span.col);
        if (!seen.insert(key).second) continue;
        Value item = Value::object();
        item["name"] = symbol.name;
        item["kind"] = static_cast<int>(symbol.kind);
        item["location"] =
            locationValue(*document.compilation, file->path, symbol.span);
        result.push(std::move(item));
      }
    }
    reply(id, std::move(result));
  }

  void completion(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    std::unordered_set<std::string> seen;
    Value items = Value::array();

    auto pushItem = [&](const std::string& name, int kind, const std::string& detail,
                        const std::string& documentation = {}) {
      if (!seen.insert(name).second) return;
      Value item = Value::object();
      item["label"] = name;
      item["kind"] = kind;
      item["insertText"] = name;
      item["filterText"] = name;
      // Snippets for control flow (ty/pyrefly offer these).
      if (name == "if") { item["insertText"] = "if ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
      else if (name == "while") { item["insertText"] = "while ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
      else if (name == "for") { item["insertText"] = "for ${1:item} in ${2:iterable}:\n    $0"; item["insertTextFormat"] = 2; }
      else if (name == "fn") { item["insertText"] = "fn ${1:name}(${2:params}) -> ${3:void}:\n    $0"; item["insertTextFormat"] = 2; }
      if (!detail.empty()) item["detail"] = detail;
      if (!documentation.empty()) {
        Value data = Value::object();
        data["documentation"] = documentation;
        item["data"] = std::move(data);
        item["documentation"] = Value::object();
        item["documentation"]["kind"] = "markdown";
        item["documentation"]["value"] = documentation;
      }
      items.push(std::move(item));
    };

    auto pushMethods = [&](const std::vector<a::MethodSig>& table) {
      for (const a::MethodSig& method : table) {
        pushItem(method.name, 2, method.signature, method.documentation);
      }
    };

    // Member completion after "." or "?.".
    int previous = tokenIndexBefore(*tokens, line + 1, byteColumn - 1);
    int base = -1;
    if (previous >= 0) {
      const Token& token = (*tokens)[static_cast<std::size_t>(previous)];
      if (token.kind == Tok::Dot || token.kind == Tok::QuestionDot) {
        if (previous >= 1) base = previous - 1;
      } else if (previous >= 1) {
        const Token& before =
            (*tokens)[static_cast<std::size_t>(previous - 1)];
        if ((before.kind == Tok::Dot || before.kind == Tok::QuestionDot) && previous >= 2) {
          base = previous - 2;
        }
      }
    }

    if (base >= 0) {
      const Token& baseToken = (*tokens)[static_cast<std::size_t>(base)];
      for (const a::lsp::ImportDef& import : file->imports) {
        if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
        const FileIndex* target = compilation.indexForModule(import.moduleId);
        if (!target) continue;
        for (const SymbolDef& symbol : target->symbols) {
          if (symbol.topLevel) {
            pushItem(symbol.name, completionKind(symbol.kind), symbol.detail);
          }
        }
      }

      if (baseToken.kind == Tok::Ident) {
        const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
        if (!moduleName.empty()) {
          for (const a::lsp::MemberInfo& member : a::lsp::nativeModuleMembers(moduleName)) {
            pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                     member.documentation);
          }
        }
        ResolvedSymbol resolved =
            a::lsp::resolveToken(compilation, *file, *tokens, base);
        if (resolved.valid()) {
          std::string typeName = resolved.symbol->typeName;
          if (typeName.empty() &&
              (resolved.symbol->kind == SymbolKind::Struct ||
               resolved.symbol->kind == SymbolKind::Class ||
               resolved.symbol->kind == SymbolKind::Enum)) {
            typeName = resolved.symbol->name;
          }
          if (const std::vector<a::MethodSig>* table = methodTableForType(typeName)) {
            pushMethods(*table);
          } else if (!typeName.empty()) {
            for (const SymbolDef* member :
                 a::lsp::structMembers(compilation, *resolved.file, typeName)) {
              pushItem(member->name, completionKind(member->kind), member->detail);
            }
          }
        }
      } else if (baseToken.kind == Tok::String) {
        pushMethods(a::stringMethods());
      } else if (baseToken.kind == Tok::RBracket) {
        pushMethods(a::arrayMethods());
      } else if (baseToken.kind == Tok::RBrace) {
        // Distinguish `{key: value}` maps from `{element}` sets by looking
        // for a colon at the literal's top level.
        bool isMap = false;
        int depth = 0;
        for (int i = base - 1; i >= 0; --i) {
          const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
          if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
              inner.kind == Tok::RParen) {
            ++depth;
          } else if (inner.kind == Tok::LBrace) {
            if (depth == 0) break;
            --depth;
          } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
            --depth;
          } else if (inner.kind == Tok::Colon && depth == 0) {
            isMap = true;
            break;
          }
        }
        pushMethods(isMap ? a::mapMethods() : a::setMethods());
      }

      // Never fall back to the global list after a dot; empty is correct
      // for receivers with no members.
      Value result = Value::object();
      result["isIncomplete"] = false;
      result["items"] = std::move(items);
      reply(id, std::move(result));
      return;
    }

    for (const char* keyword : kKeywords) pushItem(keyword, 14, "keyword");
    for (const auto& entry : a::builtinDocs()) {
      const std::string& name = entry.first;
      if (a::isStdlibBuiltin(name) || name == "PI" || name == "E") continue;
      pushItem(name, 3, entry.second.signature);
    }
    for (const SymbolDef* symbol :
         a::lsp::visibleSymbols(compilation, *file, line + 1, byteColumn)) {
      pushItem(symbol->name, completionKind(symbol->kind), symbol->detail);
    }
    for (const a::lsp::ImportDef& import : file->imports) {
      pushItem(import.alias, import.wholeModule ? 9 : 18, import.modulePath);
    }

    // `import <TAB>`, `from <TAB>`, and `from module import <TAB>`.
    std::string currentLine = lineText(document->text, line + 1);
    std::string prefix = currentLine.substr(
        0, std::min<std::size_t>(byteColumn - 1, currentLine.size()));
    const bool insideWord =
        !prefix.empty() && !std::isspace(static_cast<unsigned char>(prefix.back()));
    std::vector<std::string> words;
    {
      std::istringstream stream(prefix);
      std::string word;
      while (stream >> word) words.push_back(word);
    }
    std::string previousWord;
    if (!words.empty()) {
      previousWord =
          insideWord && words.size() >= 2 ? words[words.size() - 2] : words.back();
    }
    if (previousWord == "import" || previousWord == "from") {
      const std::string& typed = insideWord ? words.back() : std::string();
      if (typed.rfind("std:", 0) == 0) {
        // `import std:<TAB>` / `from std:<TAB>`: complete stdlib packages.
        for (const auto& module : a::nativeModules()) {
          pushItem(module.first, 9, "native module");
        }
      } else if (previousWord == "from" && words.size() >= 2) {
        const std::size_t needed = insideWord ? 3 : 2;
        if (words.size() >= needed) {
          const std::size_t moduleIndex = insideWord ? words.size() - 3 : words.size() - 2;
          std::string moduleName = words[moduleIndex];
          if (moduleName.rfind("std:", 0) == 0) moduleName.erase(0, 4);
          if (a::isNativeModule(moduleName)) {
            for (const a::lsp::MemberInfo& member :
                 a::lsp::nativeModuleMembers(moduleName)) {
              pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                       member.documentation);
            }
          }
        }
      } else {
        pushItem("std:", 9, "stdlib package");
        for (const auto& module : a::nativeModules()) {
          pushItem(module.first, 9, "native module");
        }
      }
    }

    Value result = Value::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    reply(id, std::move(result));
  }

  void completionResolve(const Value& id, const Value* params) {
    if (!params || !params->isObject()) return replyNull(id);
    const Value* item = params;
    if (const Value* nested = params->find("item"); nested && nested->isObject()) item = nested;
    Value result = *item;
    const Value* data = item->find("data");
    const Value* documentation = data && data->isObject() ? data->find("documentation") : nullptr;
    if (documentation && documentation->isString()) {
      Value contents = Value::object();
      contents["kind"] = "markdown";
      contents["value"] = documentation->asString();
      result["documentation"] = std::move(contents);
    }
    reply(id, std::move(result));
  }

  void signatureHelp(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    const int cursorLine = line + 1;
    const int byteColumn = utf16ToByteColumn(lineText(document->text, cursorLine), character) + 1;

    // Innermost unmatched bracket whose opening is before the cursor.
    std::vector<int> stack;
    for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind == Tok::Eof) break;
      if (token.span.line > cursorLine ||
          (token.span.line == cursorLine && token.span.col >= byteColumn)) {
        break;
      }
      if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
        stack.push_back(i);
      } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
                 token.kind == Tok::RBrace) {
        if (!stack.empty()) stack.pop_back();
      }
    }
    if (stack.empty() || (*tokens)[static_cast<std::size_t>(stack.back())].kind != Tok::LParen) {
      return replyNull(id);
    }
    const int openIndex = stack.back();
    if (openIndex == 0) return replyNull(id);

    // Callee: `name(`, `module.member(`, or `receiver.method(`.
    std::string label;
    const Token& callee = (*tokens)[static_cast<std::size_t>(openIndex - 1)];
    if (callee.kind != Tok::Ident) return replyNull(id);
    const bool qualified = openIndex >= 3 &&
                           ((*tokens)[static_cast<std::size_t>(openIndex - 2)].kind == Tok::Dot ||
                            (*tokens)[static_cast<std::size_t>(openIndex - 2)].kind ==
                                Tok::QuestionDot);
    const Token& baseToken =
        qualified ? (*tokens)[static_cast<std::size_t>(openIndex - 3)] : callee;
    const bool qualifiedOnDot =
        qualified && (baseToken.kind == Tok::Ident || baseToken.kind == Tok::String ||
                      baseToken.kind == Tok::RBracket || baseToken.kind == Tok::RBrace);
    if (!qualified && baseToken.kind != Tok::Ident) return replyNull(id);

    const std::string moduleAlias =
        qualifiedOnDot && baseToken.kind == Tok::Ident ? nativeModuleForAlias(*file, baseToken.lexeme)
                                                       : std::string();
    if (!moduleAlias.empty()) {
      const std::string* builtinName = a::nativeFunction(moduleAlias, callee.lexeme);
      if (!builtinName) builtinName = a::nativeConstant(moduleAlias, callee.lexeme);
      if (builtinName) {
        if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) label = doc->signature;
      }
    }
    if (label.empty()) {
      const bool primitiveBase = baseToken.kind != Tok::Ident &&
                                 baseToken.kind != Tok::Eof &&
                                 (baseToken.kind == Tok::String ||
                                  baseToken.kind == Tok::RBracket ||
                                  baseToken.kind == Tok::RBrace);
      std::string typeName;
      if (primitiveBase) {
        typeName = baseToken.kind == Tok::String
                       ? "String"
                       : (baseToken.kind == Tok::RBracket ? "array" : "map");
        if (baseToken.kind == Tok::RBrace) {
          int depth = 0;
          bool isMap = false;
          for (int i = openIndex - 4; i >= 0; --i) {
            const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
            if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
                inner.kind == Tok::RParen) {
              ++depth;
            } else if (inner.kind == Tok::LBrace) {
              if (depth == 0) break;
              --depth;
            } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
              --depth;
            } else if (inner.kind == Tok::Colon && depth == 0) {
              isMap = true;
              break;
            }
          }
          typeName = isMap ? "map" : "set";
        }
      } else if (baseToken.kind == Tok::Ident) {
        ResolvedSymbol resolved =
            a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 3);
        if (resolved.valid()) typeName = resolved.symbol->typeName;
      }
      const std::vector<a::MethodSig>* table = methodTableForType(typeName);
      if (table) {
        if (const a::MethodSig* method = findMethodSig(*table, callee.lexeme)) {
          label = method->signature;
        }
      }
    }
    if (label.empty() && baseToken.kind == Tok::Ident) {
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 1);
      if (resolved.valid() && !resolved.symbol->detail.empty()) {
        label = resolved.symbol->detail;
      }
    }
    if (label.empty() && qualifiedOnDot && baseToken.kind == Tok::Ident) {
      auto found = compilation.sema.functions.find(baseToken.lexeme + "." + callee.lexeme);
      if (found == compilation.sema.functions.end()) {
        found = compilation.sema.functions.find(callee.lexeme);
      }
      if (found != compilation.sema.functions.end() && found->second) {
        label = a::lsp::signatureText(*found->second);
      }
    }
    if (label.empty() && !qualifiedOnDot) {
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(callee.lexeme)) label = doc->signature;
    }
    if (label.empty()) return replyNull(id);

    // Split the parameter list of the label into display segments.
    std::vector<std::string> parameters;
    std::size_t start = label.find('(');
    if (start != std::string::npos) {
      int depth = 0;
      std::size_t segmentStart = start + 1;
      for (std::size_t i = start; i < label.size(); ++i) {
        char c = label[i];
        if (c == '(' || c == '[' || c == '{') {
          ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
          --depth;
          if (depth == 0) {
            std::string last = label.substr(segmentStart, i - segmentStart);
            if (!last.empty()) parameters.push_back(last);
            break;
          }
        } else if (c == ',' && depth == 1) {
          parameters.push_back(label.substr(segmentStart, i - segmentStart));
          segmentStart = i + 1;
        }
      }
    }

    // Active parameter: top-level commas between the open paren and cursor.
    int activeParameter = 0;
    int depth = 0;
    for (int i = openIndex + 1; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind == Tok::Eof) break;
      if (token.span.line > cursorLine ||
          (token.span.line == cursorLine && token.span.col >= byteColumn)) {
        break;
      }
      if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
        ++depth;
      } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
                 token.kind == Tok::RBrace) {
        if (depth == 0) break;
        --depth;
      } else if (token.kind == Tok::Comma && depth == 0) {
        ++activeParameter;
      }
    }
    if (activeParameter > 0 && parameters.empty()) activeParameter = 0;
    if (static_cast<std::size_t>(activeParameter) >= parameters.size() && !parameters.empty()) {
      activeParameter = static_cast<int>(parameters.size()) - 1;
    }

    Value signature = Value::object();
    signature["label"] = label;
    Value parameterValues = Value::array();
    for (const std::string& parameter : parameters) {
      Value entry = Value::object();
      std::string text = parameter;
      while (!text.empty() && text.front() == ' ') text.erase(text.begin());
      entry["label"] = text;
      parameterValues.push(std::move(entry));
    }
    if (!parameters.empty()) signature["parameters"] = std::move(parameterValues);
    Value signatures = Value::array();
    signatures.push(std::move(signature));

    Value result = Value::object();
    result["signatures"] = std::move(signatures);
    result["activeSignature"] = 0;
    result["activeParameter"] = activeParameter;
    reply(id, std::move(result));
  }

  void prepareRename(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);
    int line = 0, character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol target = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);
    reply(id, rangeValue(document->text, target.symbol->span));
  }

  // Semantic tokens: delta-encode [type, modifiers] per token, per the
  // legend in initialize. Mirrors pyrefly's semanticTokens.
  void semanticTokens(const Value& id, const Value* params, bool rangeOnly) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation || !params) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);
    int rangeStartLine = -1, rangeEndLine = -1;
    if (rangeOnly) {
      const Value* range = params->find("range");
      if (range) {
        const Value* s = range->find("start");
        const Value* e = range->find("end");
        if (s && e) {
          rangeStartLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0) + 1;
          rangeEndLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0) + 1;
        }
      }
    }
    // Legend indices: 0 namespace,1 type,2 class,3 enum,4 function,
    // 5 method,6 parameter,7 variable,8 property,9 keyword,10 comment,
    // 11 string,12 number,13 operator.
    struct LexInfo { int line, col, len, type, mod; };
    std::vector<LexInfo> lexemes;
    auto pushLex = [&](Span span, int type, int mod = 0) {
      if (!span.valid()) return;
      if (rangeOnly && rangeStartLine >= 0 && (span.line < rangeStartLine || span.line > rangeEndLine)) return;
      std::string line = lineText(document->text, span.line);
      int startChar = byteToUtf16Column(line, std::max(0, span.col - 1));
      int endCol = span.endCol > span.col ? span.endCol : span.col + 1;
      if (span.endLine != span.line) endCol = (int)line.size() + 1;
      int len = byteToUtf16Column(line, std::max(0, endCol - 1)) - startChar;
      if (len <= 0) len = 1;
      lexemes.push_back({span.line - 1, startChar, len, type, mod});
    };
    // Precompute definition spans for classification.
    std::unordered_set<long long> defKeys;
    auto keyOf = [](Span s) { return (long long)s.fileId * 100000000 + (long long)s.line * 10000 + s.col; };
    for (const SymbolDef& sym : file->symbols) {
      int t = 7;
      if (sym.kind == SymbolKind::Struct || sym.kind == SymbolKind::Class) t = 2;
      else if (sym.kind == SymbolKind::Enum) t = 3;
      else if (sym.kind == SymbolKind::Function) t = 4;
      else if (sym.kind == SymbolKind::Method) t = 5;
      else if (sym.kind == SymbolKind::Field || sym.kind == SymbolKind::Property) t = 8;
      else if (sym.kind == SymbolKind::Module) t = 0;
      else if (sym.kind == SymbolKind::Constant) t = 7;
      defKeys.insert(keyOf(sym.span));
      pushLex(sym.span, t, 1);  // declaration modifier
    }
    for (const Token& tok : *tokens) {
      if (tok.kind == Tok::Eof) continue;
      if (defKeys.count(keyOf(tok.span))) continue;
      int t = -1;
      switch (tok.kind) {
        case Tok::String: t = 11; break;
        case Tok::Int: case Tok::Float: t = 12; break;
        case Tok::Char: t = 11; break;
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
        case Tok::Percent: case Tok::EqEq: case Tok::NotEq: case Tok::Lt:
        case Tok::Gt: case Tok::Le: case Tok::Ge: case Tok::Eq: case Tok::Bang:
        case Tok::Question: case Tok::Dot: case Tok::Comma: case Tok::Colon:
        case Tok::QuestionDot: case Tok::QuestionQuestion: t = 13; break;
        default: break;
      }
      if (t >= 0) { pushLex(tok.span, t); continue; }
      // Keywords are lexed as Idents with keyword lexemes in A.
      bool isKw = false;
      for (const char* kw : kKeywords) if (tok.lexeme == kw) { isKw = true; break; }
      if (isKw) { pushLex(tok.span, 9); continue; }
      if (tok.kind == Tok::Ident) {
        // Type name? builtin type names classify as `type`.
        if (a::builtinTypeByName(tok.lexeme)) { pushLex(tok.span, 1); continue; }
        if (a::findBuiltinDoc(tok.lexeme)) { pushLex(tok.span, 4); continue; }
        ResolvedSymbol r = a::lsp::resolveToken(compilation, *file, *tokens,
            (int)(&tok - &(*tokens)[0]));
        if (r.valid()) {
          int tt = 7;
          switch (r.symbol->kind) {
            case SymbolKind::Struct: case SymbolKind::Class: tt = 2; break;
            case SymbolKind::Enum: tt = 3; break;
            case SymbolKind::Function: tt = 4; break;
            case SymbolKind::Method: case SymbolKind::Field: case SymbolKind::Property: tt = 8; break;
            case SymbolKind::Module: tt = 0; break;
            default: tt = 7; break;
          }
          pushLex(tok.span, tt);
        }
      }
    }
    // Comments get type 10.
    if (const std::vector<a::Comment>* comments = compilation.commentsForModule(
            compilation.graph.entry >= 0 ? compilation.graph.entry : 0)) {
      for (const a::Comment& c : *comments) pushLex(c.span, 10);
    }
    std::sort(lexemes.begin(), lexemes.end(), [](const LexInfo& a, const LexInfo& b) {
      if (a.line != b.line) return a.line < b.line;
      return a.col < b.col;
    });
    Value data = Value::array();
    int prevLine = 0, prevCol = 0;
    for (const LexInfo& lex : lexemes) {
      int deltaLine = lex.line - prevLine;
      int deltaCol = deltaLine == 0 ? lex.col - prevCol : lex.col;
      data.push(deltaLine); data.push(deltaCol); data.push(lex.len);
      data.push(lex.type); data.push(lex.mod);
      prevLine = lex.line; prevCol = lex.col;
    }
    Value result = Value::object();
    result["data"] = std::move(data);
    reply(id, std::move(result));
  }

  // Inlay hints: inferred type after `let` without annotation, and
  // parameter names at call sites with multiple args (ty/pyrefly style).
  void inlayHints(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation || !params) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    if (!file) return replyNull(id);
    const Value* rangeVal = params->find("range");
    int startLine = 0, endLine = INT_MAX;
    if (rangeVal) {
      const Value* s = rangeVal->find("start");
      const Value* e = rangeVal->find("end");
      if (s && e) {
        startLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0);
        endLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0);
      }
    }
    Value hints = Value::array();
    // Inferred types for variables without annotations.
    for (const auto& var : compilation.sema.variables) {
      if (!var.type) continue;
      if (var.span.line <= 0) continue;
      int varLine = var.span.line - 1;
      if (varLine < startLine || varLine > endLine) continue;
      const SymbolDef* sym = nullptr;
      for (const SymbolDef& s : file->symbols) {
        if (s.span.line == var.span.line && s.span.col == var.span.col) { sym = &s; break; }
      }
      if (!sym) continue;
      // Skip if already annotated (detail contains ": ").
      if (sym->detail.find(": ") != std::string::npos) continue;
      std::string t = a::typeName(var.type);
      if (t.empty() || t == "error" || t == "unknown") continue;
      // Skip unresolved type variables (`maybe = None` infers `?`).
      if (t.find("unknown") != std::string::npos || t.find("an ") != std::string::npos) continue;
      Value hint = Value::object();
      Value pos = Value::object();
      std::string lt = lineText(document->text, var.span.line);
      pos["line"] = var.span.line - 1;
      pos["character"] = byteToUtf16Column(lt, std::max(0, var.span.endCol - 1));
      hint["position"] = std::move(pos);
      hint["label"] = ": " + t;
      hint["kind"] = 1;  // Type
      hint["paddingRight"] = true;
      hint["tooltip"] = "Inferred type (from a check)";
      hints.push(std::move(hint));
    }
    // Parameter-name hints at call sites (ty/pyrefly style): `add(x, y)`
    // shows `lhs:`/`rhs:` before positional arguments whose callee resolves
    // to a known function or struct constructor.
    const int entryModule = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
    for (const a::lsp::CallSite& site : a::lsp::collectCallSites(compilation)) {
      if (site.moduleId != entryModule || site.positionalArgSpans.empty()) continue;
      ResolvedSymbol callee = resolveCallCallee(compilation, site);
      if (!callee.valid()) continue;
      const std::vector<std::string> names = paramNamesForSymbol(compilation, callee);
      if (names.empty()) continue;
      const std::string* moduleText = compilation.textForModule(site.moduleId);
      const std::string& source = moduleText ? *moduleText : document->text;
      const std::size_t argCount =
          std::min(names.size(), site.positionalArgSpans.size());
      for (std::size_t i = 0; i < argCount; ++i) {
        const std::string& name = names[i];
        if (name.empty() || name == "_" || name == "self") continue;
        const Span& argSpan = site.positionalArgSpans[i];
        if (!argSpan.valid()) continue;
        Value hint = Value::object();
        Value pos = Value::object();
        pos["line"] = argSpan.line - 1;
        pos["character"] =
            byteToUtf16Column(lineText(source, argSpan.line),
                              std::max(0, argSpan.col - 1));
        hint["position"] = std::move(pos);
        hint["label"] = name + ": ";
        hint["kind"] = 2;  // Parameter
        hint["paddingRight"] = false;
        hints.push(std::move(hint));
      }
    }
    Value result = Value::array();
    // Return array of hints directly.
    reply(id, std::move(hints));
  }

  // Code actions: quick fixes tied to diagnostics (like ty/pyrefly).
  void codeActions(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    Value actions = Value::array();
    if (!document || !document->compilation || !params) { reply(id, std::move(actions)); return; }
    const Compilation& compilation = *document->compilation;
    const Value* rangeVal = params->find("range");
    int reqLine = -1;
    if (rangeVal) {
      const Value* s = rangeVal->find("start");
      if (s && s->find("line")) reqLine = static_cast<int>(s->find("line")->asInt()) + 1;
    }
    const Value* context = params->find("context");
    std::vector<std::string> requestedKinds;
    if (context) {
      const Value* kinds = context->find("only");
      if (kinds && kinds->isArray()) for (auto& k : kinds->asArray()) if (k.isString()) requestedKinds.push_back(k.asString());
    }
    auto wantKind = [&](const std::string& k) {
      if (requestedKinds.empty()) return true;
      for (auto& r : requestedKinds) if (r == k) return true;
      return false;
    };
    auto hasSourceFixAll = [&](const std::string& k) {
      if (requestedKinds.empty()) return true;
      for (auto& r : requestedKinds) if (r == "source.fixAll" || r == "source.fixAll.a" || r == k) return true;
      return false;
    };
    if (!wantKind("quickfix")) { reply(id, std::move(actions)); return; }
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (file && tokens && reqLine > 0) {
      // Unused variable (W0001): offer prefix with `_`.
      for (const auto& d : compilation.sink.diagnostics()) {
        if (d.span.line != reqLine || d.code != "W0001") continue;
        const Token* tok = tokenAt(*tokens, d.span.line, d.span.col);
        if (!tok || tok->kind != Tok::Ident) continue;
        std::string newName = "_" + tok->lexeme;
        Value action = Value::object();
        action["title"] = "Prefix with '_': " + newName;
        action["kind"] = "quickfix";
        Value edit = Value::object();
        edit["range"] = rangeValue(document->text, tok->span);
        edit["newText"] = newName;
        Value changes = Value::object();
        Value eps = Value::array(); eps.push(std::move(edit));
        changes[document->uri] = std::move(eps);
        Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
        action["edit"] = std::move(wsEdit);
        action["diagnostics"] = Value::array();
        actions.push(std::move(action));
        break;
      }
      // Any diagnostic: offer "Suppress with '# a: ignore[CODE]'".
      for (const auto& d : compilation.sink.diagnostics()) {
        if (d.span.line != reqLine) continue;
        std::string tail = d.code.empty() ? " # a: ignore" : " # a: ignore[" + d.code + "]";
        Value action = Value::object();
        action["title"] = "Suppress" + (d.code.empty() ? std::string() : " " + d.code) + " on this line";
        action["kind"] = "quickfix";
        std::string lt = lineText(document->text, reqLine);
        Value edit = Value::object();
        Value insertPos = Value::object();
        insertPos["line"] = reqLine - 1;
        insertPos["character"] = byteToUtf16Column(lt, (int)lt.size());
        edit["range"] = rangeValue(document->text, Span{reqLine, (int)lt.size() + 1, reqLine, (int)lt.size() + 1, 0});
        edit["newText"] = tail;
        Value changes = Value::object();
        Value eps = Value::array(); eps.push(std::move(edit));
        changes[document->uri] = std::move(eps);
        Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
        action["edit"] = std::move(wsEdit);
        actions.push(std::move(action));
        break;
      }
    }
    if (hasSourceFixAll("source.fixAll")) {
      // No bulk fix available yet; return what we have.
    }
    reply(id, std::move(actions));
  }

  // Selection range: identifier -> enclosing indent block -> next block.
  void selectionRanges(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    Value result = Value::array();
    if (!document || !document->compilation || !params) { reply(id, std::move(result)); return; }
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) { reply(id, std::move(result)); return; }
    std::vector<std::pair<int,int>> positions;
    const Value* posArr = params->find("positions");
    if (posArr && posArr->isArray()) {
      for (auto& p : posArr->asArray()) {
        int l = static_cast<int>(p.find("line") ? p.find("line")->asInt() : 0);
        int c = static_cast<int>(p.find("character") ? p.find("character")->asInt() : 0);
        positions.emplace_back(l, c);
      }
    }
    for (auto& [line0, char0] : positions) {
      int line = line0 + 1;
      int byteCol = utf16ToByteColumn(lineText(document->text, line), char0) + 1;
      Value entry = Value::object();
      Value current = Value::object();
      const Token* tok = tokenAt(*tokens, line, byteCol);
      if (tok) current["range"] = rangeValue(document->text, tok->span);
      else { Value r = Value::object(); r["start"] = positionValue(document->text, Span{line, byteCol, line, byteCol, 0}); r["end"] = r["start"]; current["range"] = std::move(r); }
      // Outward: expand to the indent block containing the line.
      int indent = 0;
      std::string lt = lineText(document->text, line);
      while (indent < (int)lt.size() && lt[indent] == ' ') ++indent;
      int blockStartLine = line;
      while (blockStartLine > 1) {
        std::string prev = lineText(document->text, blockStartLine - 1);
        int pi = 0; while (pi < (int)prev.size() && prev[pi] == ' ') ++pi;
        if (pi >= indent && !prev.empty()) --blockStartLine;
        else break;
      }
      int blockEndLine = line;
      int totalLines = 0; for (char c : document->text) if (c == '\n') ++totalLines;
      while (blockEndLine <= totalLines) {
        std::string next = lineText(document->text, blockEndLine + 1);
        if (next.empty()) break;
        int ni = 0; while (ni < (int)next.size() && next[ni] == ' ') ++ni;
        if (ni > indent) ++blockEndLine;
        else break;
      }
      Value ranges = Value::array();
      ranges.push(current);
      Value outer = Value::object();
      Span blockSpan{blockStartLine, 1, blockEndLine, (int)lineText(document->text, blockEndLine).size() + 1, 0};
      outer["range"] = rangeValue(document->text, blockSpan);
      Value outerArr = Value::array(); outerArr.push(std::move(outer));
      current["parent"] = std::move(outerArr);
      entry["ranges"] = std::move(ranges);
      result.push(std::move(entry));
    }
    reply(id, std::move(result));
  }

  // ---- call / type hierarchy --------------------------------------------

  std::string modulePathForId(const Compilation& compilation, int moduleId) const {
    if (moduleId < 0 ||
        static_cast<std::size_t>(moduleId) >= compilation.graph.modules.size()) {
      return {};
    }
    return compilation.graph.modules[static_cast<std::size_t>(moduleId)].path;
  }

  Value itemDataFor(const std::string& path, int line, int col) const {
    Value data = Value::object();
    data["path"] = path;
    data["line"] = line;
    data["col"] = col;
    return data;
  }

  // Reads the `data` blob hierarchy items carry between prepare/incoming/
  // outgoing requests: the declaration's file path and name-span position.
  bool itemTargetFromData(const Value* item, std::string* path, int* line, int* col) const {
    if (!item) return false;
    const Value* data = item->find("data");
    if (!data) return false;
    const Value* p = data->find("path");
    const Value* l = data->find("line");
    const Value* c = data->find("col");
    if (!p || !p->isString() || !l || !l->isNumber() || !c || !c->isNumber()) return false;
    *path = p->asString();
    *line = static_cast<int>(l->asInt());
    *col = static_cast<int>(c->asInt());
    return true;
  }

  // CallHierarchyItem for a resolved declaration (the "to" side, and the item
  // prepareCallHierarchy returns).
  Value callItemForSymbol(const Compilation& compilation, const ResolvedSymbol& resolved) const {
    const SymbolDef& symbol = *resolved.symbol;
    int declModule = -1;
    const a::Stmt* decl =
        a::lsp::findDeclAt(compilation, resolved.file->path, symbol.span.line,
                           symbol.span.col, &declModule);
    const std::string* text = compilation.textForPath(resolved.file->path);
    static const std::string empty;
    const std::string& source = text ? *text : empty;
    Value item = Value::object();
    item["name"] = symbol.name;
    item["kind"] = static_cast<int>(symbol.kind);
    if (!symbol.detail.empty()) item["detail"] = symbol.detail;
    item["uri"] = pathToUri(resolved.file->path);
    item["range"] = rangeValue(source, decl ? decl->span : symbol.span);
    item["selectionRange"] = rangeValue(source, symbol.span);
    item["data"] = itemDataFor(resolved.file->path, symbol.span.line, symbol.span.col);
    return item;
  }

  // CallHierarchyItem for the enclosing callable of a call site (the "from"
  // side of incomingCalls).
  Value callItemForSite(const Compilation& compilation, const a::lsp::CallSite& site) const {
    const std::string path = modulePathForId(compilation, site.moduleId);
    const std::string* text = compilation.textForModule(site.moduleId);
    static const std::string empty;
    const std::string& source = text ? *text : empty;
    Value item = Value::object();
    item["name"] = site.callableName;
    item["kind"] = static_cast<int>(site.callableKind);
    if (!site.callableDetail.empty()) item["detail"] = site.callableDetail;
    item["uri"] = pathToUri(path);
    item["range"] = rangeValue(source, site.callableSpan);
    item["selectionRange"] = rangeValue(source, site.callableNameSpan);
    item["data"] = itemDataFor(path, site.callableNameSpan.line, site.callableNameSpan.col);
    return item;
  }

  // CallHierarchyItem for the function an outgoingCalls request targets.
  Value callItemForFnDecl(const Compilation& compilation, const a::FnDecl& fn,
                          const std::string& path, int kind) const {
    const std::string* text = compilation.textForPath(path);
    static const std::string empty;
    const std::string& source = text ? *text : empty;
    Value item = Value::object();
    item["name"] = fn.name;
    item["kind"] = kind;
    item["detail"] = a::lsp::signatureText(fn);
    item["uri"] = pathToUri(path);
    item["range"] = rangeValue(source, fn.span);
    item["selectionRange"] = rangeValue(source, fn.nameSpan);
    item["data"] = itemDataFor(path, fn.nameSpan.line, fn.nameSpan.col);
    return item;
  }

  void prepareCallHierarchy(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);
    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn =
        utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol resolved =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!resolved.valid() ||
        (resolved.symbol->kind != SymbolKind::Function &&
         resolved.symbol->kind != SymbolKind::Method)) {
      return replyNull(id);
    }
    Value result = Value::array();
    result.push(callItemForSymbol(compilation, resolved));
    reply(id, std::move(result));
  }

  // callHierarchy/incomingCalls and callHierarchy/outgoingCalls take only
  // {item} (no textDocument), so they ensure every open document is compiled
  // and search all of their module graphs.
  void incomingCalls(const Value& id, const Value* params) {
    Value result = Value::array();
    const Value* item = params ? params->find("item") : nullptr;
    std::string path;
    int line = 0;
    int col = 0;
    if (!itemTargetFromData(item, &path, &line, &col)) {
      reply(id, std::move(result));
      return;
    }

    for (auto& entry : documents_) ensureCompiled(&entry.second);
    std::unordered_map<const Compilation*, std::vector<a::lsp::CallSite>> sitesCache;
    std::unordered_set<std::string> seenSites;
    for (auto& entry : documents_) {
      Document& document = entry.second;
      if (!document.compilation) continue;
      const Compilation& compilation = *document.compilation;
      ResolvedSymbol target = findSymbolBySpan(compilation, path, line, col);
      if (!target.valid()) continue;
      auto& sites = sitesCache[&compilation];
      if (sites.empty()) sites = a::lsp::collectCallSites(compilation);
      for (const a::lsp::CallSite& site : sites) {
        const std::string modulePath = modulePathForId(compilation, site.moduleId);
        const std::string key =
            canonicalPath(modulePath) + "|" + std::to_string(site.callSpan.line) + ":" +
            std::to_string(site.callSpan.col) + ":" + std::to_string(site.callSpan.endCol) +
            "|" + std::to_string(site.calleeSpan.line) + ":" +
            std::to_string(site.calleeSpan.col);
        if (!seenSites.insert(key).second) continue;
        if (!site.inCallable) continue;
        ResolvedSymbol callee = resolveCallCallee(compilation, site);
        if (!callee.valid()) continue;
        if (callee.symbol->span.line != target.symbol->span.line ||
            callee.symbol->span.col != target.symbol->span.col ||
            canonicalPath(callee.file->path) != canonicalPath(target.file->path)) {
          continue;
        }
        Value incoming = Value::object();
        incoming["from"] = callItemForSite(compilation, site);
        Value fromRanges = Value::array();
        const std::string* text = compilation.textForPath(modulePath);
        static const std::string empty;
        fromRanges.push(rangeValue(text ? *text : empty, site.callSpan));
        incoming["fromRanges"] = std::move(fromRanges);
        incoming["to"] = callItemForSymbol(compilation, target);
        result.push(std::move(incoming));
      }
    }
    reply(id, std::move(result));
  }

  void outgoingCalls(const Value& id, const Value* params) {
    Value result = Value::array();
    const Value* item = params ? params->find("item") : nullptr;
    std::string path;
    int line = 0;
    int col = 0;
    if (!itemTargetFromData(item, &path, &line, &col)) {
      reply(id, std::move(result));
      return;
    }

    for (auto& entry : documents_) ensureCompiled(&entry.second);
    const std::string canonical = canonicalPath(path);
    // Prefer the compilation of the document the item was prepared in; any
    // open document whose graph contains the module works as a fallback.
    const Compilation* compilation = nullptr;
    for (auto& entry : documents_) {
      if (!entry.second.compilation) continue;
      bool owns = canonicalPath(entry.second.path) == canonical;
      for (const a::Module& module : entry.second.compilation->graph.modules) {
        if (canonicalPath(module.path) == canonical) {
          if (owns || !compilation) compilation = entry.second.compilation.get();
          break;
        }
      }
      if (compilation && canonicalPath(entry.second.path) == canonical) break;
    }
    if (!compilation) {
      reply(id, std::move(result));
      return;
    }

    int moduleId = -1;
    const a::Stmt* decl = a::lsp::findDeclAt(*compilation, path, line, col, &moduleId);
    if (!decl || decl->kind != a::StmtKind::FnDecl) {
      reply(id, std::move(result));
      return;
    }
    const a::FnDecl& fn = static_cast<const a::FnDecl&>(*decl);
    const std::string modulePath = modulePathForId(*compilation, moduleId);
    int kind = 12;  // SymbolKind::Function
    if (item) {
      if (const Value* k = item->find("kind"); k && k->isNumber()) {
        kind = static_cast<int>(k->asInt());
      }
    }
    const Value fromItem = callItemForFnDecl(*compilation, fn, modulePath, kind);

    for (const a::lsp::CallSite& site : a::lsp::collectCallSites(*compilation)) {
      if (!site.inCallable || site.moduleId != moduleId) continue;
      if (site.callableNameSpan.line != fn.nameSpan.line ||
          site.callableNameSpan.col != fn.nameSpan.col) {
        continue;
      }
      ResolvedSymbol callee = resolveCallCallee(*compilation, site);
      if (!callee.valid()) continue;
      Value outgoing = Value::object();
      outgoing["from"] = fromItem;
      Value fromRanges = Value::array();
      const std::string* text = compilation->textForPath(modulePath);
      static const std::string empty;
      fromRanges.push(rangeValue(text ? *text : empty, site.callSpan));
      outgoing["fromRanges"] = std::move(fromRanges);
      outgoing["to"] = callItemForSymbol(*compilation, callee);
      result.push(std::move(outgoing));
    }
    reply(id, std::move(result));
  }

  // ---- type hierarchy -----------------------------------------------------

  // TypeHierarchyItem for a collected type declaration.
  Value typeItemForDecl(const Compilation& compilation,
                        const a::lsp::TypeDeclInfo& decl) const {
    const std::string path = modulePathForId(compilation, decl.moduleId);
    const std::string* text = compilation.textForModule(decl.moduleId);
    static const std::string empty;
    const std::string& source = text ? *text : empty;
    Value item = Value::object();
    item["name"] = decl.name;
    item["kind"] = static_cast<int>(decl.kind);
    if (!decl.detail.empty()) item["detail"] = decl.detail;
    item["uri"] = pathToUri(path);
    item["range"] = rangeValue(source, decl.span);
    item["selectionRange"] = rangeValue(source, decl.nameSpan);
    item["data"] = itemDataFor(path, decl.nameSpan.line, decl.nameSpan.col);
    return item;
  }

  using TypeDeclPair = std::pair<const Compilation*, a::lsp::TypeDeclInfo>;

  // All type declarations visible to any open document (deduplicated by
  // declaration span so shared modules appear once).
  std::vector<TypeDeclPair> allTypeDecls() {
    for (auto& entry : documents_) ensureCompiled(&entry.second);
    std::vector<TypeDeclPair> decls;
    std::unordered_set<std::string> seen;
    for (auto& entry : documents_) {
      if (!entry.second.compilation) continue;
      const Compilation& compilation = *entry.second.compilation;
      for (const a::lsp::TypeDeclInfo& info : a::lsp::collectTypeDecls(compilation)) {
        const std::string path = modulePathForId(compilation, info.moduleId);
        const std::string key = canonicalPath(path) + "|" +
                                std::to_string(info.nameSpan.line) + ":" +
                                std::to_string(info.nameSpan.col);
        if (!seen.insert(key).second) continue;
        decls.emplace_back(&compilation, info);
      }
    }
    return decls;
  }

  const TypeDeclPair* findTypeDecl(const std::vector<TypeDeclPair>& decls,
                                   const std::string& path, int line, int col) const {
    const std::string canonical = canonicalPath(path);
    for (const TypeDeclPair& pair : decls) {
      const std::string declPath = modulePathForId(*pair.first, pair.second.moduleId);
      if (canonicalPath(declPath) != canonical) continue;
      if (pair.second.nameSpan.line == line && pair.second.nameSpan.col == col) {
        return &pair;
      }
    }
    return nullptr;
  }

  const TypeDeclPair* findTypeDeclByName(const std::vector<TypeDeclPair>& decls,
                                         const std::string& name) const {
    // Exact match first, then last-segment match for qualified base names.
    for (const TypeDeclPair& pair : decls) {
      if (pair.second.name == name) return &pair;
    }
    const std::string leaf = simpleName(name);
    for (const TypeDeclPair& pair : decls) {
      if (simpleName(pair.second.name) == leaf) return &pair;
    }
    return nullptr;
  }

  void prepareTypeHierarchy(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);
    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn =
        utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol resolved =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!resolved.valid() ||
        (resolved.symbol->kind != SymbolKind::Struct &&
         resolved.symbol->kind != SymbolKind::Class &&
         resolved.symbol->kind != SymbolKind::Enum)) {
      return replyNull(id);
    }
    const std::vector<TypeDeclPair> decls = allTypeDecls();
    if (const TypeDeclPair* pair =
            findTypeDecl(decls, resolved.file->path, resolved.symbol->span.line,
                         resolved.symbol->span.col)) {
      Value result = Value::array();
      result.push(typeItemForDecl(*pair->first, pair->second));
      reply(id, std::move(result));
      return;
    }
    // Fallback: the index knows the symbol but no AST declaration matched
    // (e.g. analysis stopped early). Still return a usable item.
    const std::string* text = compilation.textForPath(resolved.file->path);
    static const std::string empty;
    const std::string& source = text ? *text : empty;
    Value item = Value::object();
    item["name"] = resolved.symbol->name;
    item["kind"] = static_cast<int>(resolved.symbol->kind);
    if (!resolved.symbol->detail.empty()) item["detail"] = resolved.symbol->detail;
    item["uri"] = pathToUri(resolved.file->path);
    item["range"] = rangeValue(source, resolved.symbol->span);
    item["selectionRange"] = rangeValue(source, resolved.symbol->span);
    item["data"] = itemDataFor(resolved.file->path, resolved.symbol->span.line,
                             resolved.symbol->span.col);
    Value result = Value::array();
    result.push(std::move(item));
    reply(id, std::move(result));
  }

  void typeHierarchyRelated(const Value& id, const Value* params, bool supertypes) {
    Value result = Value::array();
    const Value* item = params ? params->find("item") : nullptr;
    std::string path;
    int line = 0;
    int col = 0;
    if (!itemTargetFromData(item, &path, &line, &col)) {
      reply(id, std::move(result));
      return;
    }
    const std::vector<TypeDeclPair> decls = allTypeDecls();
    const TypeDeclPair* self = findTypeDecl(decls, path, line, col);
    if (!self) {
      reply(id, std::move(result));
      return;
    }
    if (supertypes) {
      for (const std::string& parent : self->second.parents) {
        if (const TypeDeclPair* base = findTypeDeclByName(decls, parent)) {
          result.push(typeItemForDecl(*base->first, base->second));
        }
      }
    } else {
      const std::string selfPath = modulePathForId(*self->first, self->second.moduleId);
      for (const TypeDeclPair& pair : decls) {
        const std::string pairPath = modulePathForId(*pair.first, pair.second.moduleId);
        if (canonicalPath(pairPath) == canonicalPath(selfPath) &&
            pair.second.nameSpan.line == self->second.nameSpan.line &&
            pair.second.nameSpan.col == self->second.nameSpan.col) {
          continue;  // never report the type as its own subtype
        }
        for (const std::string& parent : pair.second.parents) {
          if (pair.second.name == self->second.name ||
              simpleName(parent) == simpleName(self->second.name) ||
              parent == self->second.name) {
            result.push(typeItemForDecl(*pair.first, pair.second));
            break;
          }
        }
      }
    }
    reply(id, std::move(result));
  }

  // ---- misc providers -----------------------------------------------------

  // Parameter names for a resolved call target: FnDecl params (methods drop
  // `self`) or StructDecl fields for constructor calls.
  std::vector<std::string> paramNamesForSymbol(const Compilation& compilation,
                                               const ResolvedSymbol& resolved) const {
    int moduleId = -1;
    const a::Stmt* decl =
        a::lsp::findDeclAt(compilation, resolved.file->path, resolved.symbol->span.line,
                           resolved.symbol->span.col, &moduleId);
    if (!decl) return {};
    if (decl->kind == a::StmtKind::FnDecl) {
      const auto& fn = static_cast<const a::FnDecl&>(*decl);
      std::vector<std::string> names;
      for (const auto& param : fn.params) names.push_back(param.name);
      if (!names.empty() && names.front() == "self") names.erase(names.begin());
      return names;
    }
    if (decl->kind == a::StmtKind::StructDecl) {
      std::vector<std::string> names;
      for (const auto& field : static_cast<const a::StructDecl&>(*decl).fields) {
        names.push_back(field.name);
      }
      return names;
    }
    return {};
  }

  void codeLens(const Value& id, const Value* params) {
    Value result = Value::array();
    Document* document = documentFrom(params);
    if (!document || !document->compilation) {
      reply(id, std::move(result));
      return;
    }
    const Compilation& compilation = *document->compilation;
    const FileIndex* entry = compilation.entryIndex();
    if (!entry) {
      reply(id, std::move(result));
      return;
    }

    // Count resolved occurrences of every symbol across all modules, the same
    // scan references() performs, grouped by declaration site.
    std::unordered_map<std::string, int> refCounts;
    for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
      if (moduleId >= compilation.tokensByModule.size()) continue;
      const FileIndex& file = compilation.indexes[moduleId];
      const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
      for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& token = tokens[static_cast<std::size_t>(i)];
        if (token.kind != Tok::Ident) continue;
        ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
        if (!resolved.valid()) continue;
        const std::string key = canonicalPath(resolved.file->path) + "|" +
                                std::to_string(resolved.symbol->span.line) + ":" +
                                std::to_string(resolved.symbol->span.col);
        ++refCounts[key];
      }
    }

    const std::string entryPath = canonicalPath(entry->path);
    for (const SymbolDef& symbol : entry->symbols) {
      const bool interesting = symbol.kind == SymbolKind::Function ||
                               symbol.kind == SymbolKind::Method ||
                               symbol.kind == SymbolKind::Struct ||
                               symbol.kind == SymbolKind::Class ||
                               symbol.kind == SymbolKind::Enum;
      if (!interesting) continue;
      const std::string key = entryPath + "|" + std::to_string(symbol.span.line) + ":" +
                              std::to_string(symbol.span.col);
      const int usages = std::max(0, refCounts[key] - 1);  // minus the declaration
      if (usages <= 0) continue;
      std::string lt = lineText(document->text, symbol.span.line);
      Value lens = Value::object();
      Value range = Value::object();
      Value start = Value::object();
      start["line"] = symbol.span.line - 1;
      start["character"] = 0;
      Value end = Value::object();
      end["line"] = symbol.span.line - 1;
      end["character"] = byteToUtf16Column(lt, static_cast<int>(lt.size()));
      range["start"] = std::move(start);
      range["end"] = std::move(end);
      lens["range"] = std::move(range);
      Value command = Value::object();
      command["title"] = std::to_string(usages) + " reference" + (usages == 1 ? "" : "s");
      command["command"] = "editor.action.showReferences";
      Value arguments = Value::array();
      arguments.push(pathToUri(entry->path));
      Value position = Value::object();
      position["line"] = symbol.span.line - 1;
      position["character"] =
          byteToUtf16Column(lt, std::max(0, symbol.span.col - 1));
      arguments.push(std::move(position));
      arguments.push(Value::array());
      command["arguments"] = std::move(arguments);
      lens["command"] = std::move(command);
      result.push(std::move(lens));
    }
    reply(id, std::move(result));
  }

  void rangeFormatting(const Value& id, const Value* params) {
    Value edits = Value::array();
    Document* document = documentFrom(params);
    if (!document || !document->compilation || !params) {
      reply(id, std::move(edits));
      return;
    }
    const Value* rangeVal = params->find("range");
    if (!rangeVal) {
      reply(id, std::move(edits));
      return;
    }
    const Value* reqStart = rangeVal->find("start");
    const Value* reqEnd = rangeVal->find("end");
    if (!reqStart || !reqEnd) {
      reply(id, std::move(edits));
      return;
    }
    const int reqStartLine = static_cast<int>(reqStart->find("line")
                                                   ? reqStart->find("line")->asInt()
                                                   : 0);
    const int reqStartChar = static_cast<int>(reqStart->find("character")
                                                  ? reqStart->find("character")->asInt()
                                                  : 0);
    const int reqEndLine = static_cast<int>(
        reqEnd->find("line") ? reqEnd->find("line")->asInt() : 0);
    const int reqEndChar = static_cast<int>(reqEnd->find("character")
                                                ? reqEnd->find("character")->asInt()
                                                : 0);

    a::tooling::FormatOptions options;
    if (const Value* formatOptions = params->find("options")) {
      if (const Value* tabSize = formatOptions->find("tabSize");
          tabSize && tabSize->isNumber()) {
        const int size = static_cast<int>(tabSize->asInt());
        if (size >= 1 && size <= 16) options.indentWidth = size;
      }
    }

    a::DiagnosticSink sink;
    sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
    a::tooling::FormatResult result =
        a::tooling::formatSource(document->text, sink, options);
    if (!result.ok || sink.hasErrors() || result.text == document->text) {
      reply(id, std::move(edits));
      return;
    }

    const std::vector<std::string> oldLines = splitLinesKeep(document->text);
    const std::vector<std::string> newLines = splitLinesKeep(result.text);
    const int lineCount = static_cast<int>(oldLines.size());
    auto endOfFilePos = [&]() {
      Value pos = Value::object();
      pos["line"] = lineCount - 1;
      pos["character"] =
          byteToUtf16Column(oldLines.back(), static_cast<int>(oldLines.back().size()));
      return pos;
    };
    auto insideRange = [&](int startLine, int startChar, int endLine, int endChar) {
      const bool afterStart = startLine > reqStartLine ||
                              (startLine == reqStartLine && startChar >= reqStartChar);
      const bool beforeEnd = endLine < reqEndLine ||
                             (endLine == reqEndLine && endChar <= reqEndChar);
      return afterStart && beforeEnd;
    };

    for (const auto& hunk : diffLineHunks(oldLines, newLines)) {
      const int oldStart = hunk[0];
      const int oldEnd = hunk[1];
      // Edit range: start of oldStart line through start of oldEnd line (or
      // EOF when the hunk reaches the last line).
      Value edit = Value::object();
      Value range = Value::object();
      Value start = Value::object();
      if (oldStart < lineCount) {
        start["line"] = oldStart;
        start["character"] = 0;
      } else {
        start = endOfFilePos();
      }
      Value end = Value::object();
      int endLine = 0;
      int endChar = 0;
      if (oldEnd < lineCount) {
        endLine = oldEnd;
        endChar = 0;
      } else {
        end = endOfFilePos();
      }
      end["line"] = endLine;
      end["character"] = endChar;
      range["start"] = std::move(start);
      range["end"] = std::move(end);
      if (!insideRange(static_cast<int>(range["start"]["line"].asInt()),
                       static_cast<int>(range["start"]["character"].asInt()),
                       static_cast<int>(range["end"]["line"].asInt()),
                       static_cast<int>(range["end"]["character"].asInt()))) {
        continue;
      }
      std::string newText;
      for (int i = hunk[2]; i < hunk[3]; ++i) newText += newLines[static_cast<std::size_t>(i)];
      edit["range"] = std::move(range);
      edit["newText"] = std::move(newText);
      edits.push(std::move(edit));
    }
    reply(id, std::move(edits));
  }

  // LSP 3.17 pull diagnostics: same items publishDiagnostics sends, scoped to
  // this document's path.
  void documentDiagnostic(const Value& id, const Value* params) {
    Value result = Value::object();
    result["kind"] = "full";
    Value items = Value::array();
    Document* document = documentFrom(params);
    if (document && document->compilation) {
      const Compilation& compilation = *document->compilation;
      const std::string canonical = canonicalPath(document->path);
      for (const Diagnostic& diagnostic : compilation.sink.diagnostics()) {
        if (!diagnostic.span.valid()) continue;
        const a::SourceFile* source = compilation.sink.file(diagnostic.span.fileId);
        if (!source || canonicalPath(source->path()) != canonical) continue;
        const std::string& text = source->text();
        Value item = Value::object();
        item["range"] = rangeValue(text, diagnostic.span);
        item["severity"] = diagnostic.severity == Severity::Error ? 1 : 2;
        item["message"] = diagnostic.message;
        item["source"] = "a";
        if (!diagnostic.code.empty()) {
          item["code"] = diagnostic.code;
          Value desc = Value::object();
          desc["href"] =
              "https://github.com/search?q=" + diagnostic.code + "+repo:own/a-lang";
          item["codeDescription"] = std::move(desc);
        }
        Value tags = Value::array();
        if (diagnostic.code == "W0002") tags.push(1);
        if (!tags.asArray().empty()) item["tags"] = std::move(tags);
        if (!diagnostic.help.empty()) {
          Value related = Value::array();
          Value info = Value::object();
          info["location"] = locationValue(compilation, document->path, diagnostic.span);
          info["message"] = diagnostic.help;
          related.push(std::move(info));
          item["relatedInformation"] = std::move(related);
        }
        items.push(std::move(item));
      }
    }
    result["items"] = std::move(items);
    reply(id, std::move(result));
  }

  void moniker(const Value& id, const Value* params) {
    const Value* textDocument = params ? params->find("textDocument") : nullptr;
    const Value* uri = textDocument ? textDocument->find("uri") : nullptr;
    if (!uri || !uri->isString()) return replyNull(id);
    Value result = Value::object();
    result["scheme"] = "a";
    result["uri"] = uri->asString();
    result["unique"] = canonicalPath(uriToPath(uri->asString()));
    reply(id, std::move(result));
  }

  // Links for http(s) URLs inside comments, like clangd's comment links.
  void documentLinks(const Value& id, const Value* params) {
    Value result = Value::array();
    Document* document = documentFrom(params);
    if (!document || !document->compilation) {
      reply(id, std::move(result));
      return;
    }
    const Compilation& compilation = *document->compilation;
    const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
    const std::vector<a::Comment>* comments = compilation.commentsForModule(entry);
    if (!comments) {
      reply(id, std::move(result));
      return;
    }
    for (const a::Comment& comment : *comments) {
      std::size_t pos = 0;
      while ((pos = comment.text.find("http", pos)) != std::string::npos) {
        const bool isHttp = comment.text.compare(pos, 7, "http://") == 0;
        const bool isHttps = comment.text.compare(pos, 8, "https://") == 0;
        if (!isHttp && !isHttps) {
          ++pos;
          continue;
        }
        std::size_t end = pos;
        while (end < comment.text.size() &&
               !std::isspace(static_cast<unsigned char>(comment.text[end])) &&
               comment.text[end] != ')' && comment.text[end] != ']' &&
               comment.text[end] != '>' && comment.text[end] != '"' &&
               comment.text[end] != '\'') {
          ++end;
        }
        // Trim trailing punctuation that is almost never part of the URL.
        while (end > pos && (comment.text[end - 1] == '.' || comment.text[end - 1] == ',' ||
                             comment.text[end - 1] == ';' || comment.text[end - 1] == ':')) {
          --end;
        }
        const std::string url = comment.text.substr(pos, end - pos);
        Value link = Value::object();
        // comment.span covers the whole comment including '#'; text columns
        // map 1:1 onto source columns (comments are single-line).
        const int startCol = comment.span.col + static_cast<int>(pos);
        const int endCol = comment.span.col + static_cast<int>(end);
        link["range"] = rangeValue(
            document->text,
            Span{comment.span.line, startCol, comment.span.line, endCol, comment.span.fileId});
        link["target"] = url;
        result.push(std::move(link));
        pos = end;
      }
    }
    reply(id, std::move(result));
  }

  std::map<std::string, Document> documents_;
  std::vector<std::string> modulePaths_;
  std::string stdlibRoot_;
  bool shutdownReceived_ = false;

  // Async-checking worker state (debounced, latest-wins).
  std::mutex outMutex_;
  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::optional<CompileJob> pendingJob_;
  uint64_t generation_ = 0;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--version" || argument == "-V") {
      std::cout << "a-language-server " << kServerVersion << "\n";
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: a-lsp [--stdio]\n"
                   "\n"
                   "A language server for the A programming language. It speaks the\n"
                   "Language Server Protocol over stdin/stdout. Editors normally start it\n"
                   "automatically; pass --stdio when they require an explicit transport.\n";
      return 0;
    }
  }

  std::ios::sync_with_stdio(false);
  LanguageServer server;
  return server.run();
}
