#pragma once

#include <string>

namespace a {

// Escapes text for embedding inside a JSON string literal: '"' and '\\'
// get backslash escapes, '\b' '\f' '\n' '\r' '\t' use their short forms,
// and other C0 control bytes become \u00XX. Bytes >= 0x20 (including UTF-8
// sequences) pass through unchanged. Shared by the --json diagnostic writer
// and `a doc --json` so both emit identical escaping.
inline std::string escapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

}  // namespace a
