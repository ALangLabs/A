// Source-text helpers: paths <-> URIs, lines <-> UTF-16 columns, LSP JSON
// builders, and the line diff used by range formatting. See text_utils.hpp.

#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace a::lsp {

namespace fs = std::filesystem;

namespace {

// UTF-16 code units for one Unicode code point.
int utf16Units(unsigned codePoint) { return codePoint > 0xFFFF ? 2 : 1; }

}  // namespace

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

std::string trimLine(const std::string& line) {
  const std::size_t begin = line.find_first_not_of(" \t\r");
  if (begin == std::string::npos) return "";
  const std::size_t end = line.find_last_not_of(" \t\r\n");
  return line.substr(begin, end - begin + 1);
}

Value symbolRange(const std::string& text, Span span) { return rangeValue(text, span); }

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

}  // namespace a::lsp
