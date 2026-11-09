#include "parser/parser_internal.hpp"
#include "lexer/lexer.hpp"

namespace a {
ExprPtr Parser::parseInlineExpression() {
  if (check(Tok::Eof)) {
    fail(cur().span, "Expected an expression inside string interpolation.");
  }
  auto expr = parseExpression();
  if (!check(Tok::Eof)) {
    fail(cur().span, "Unexpected " + describeToken(cur()) + " inside string interpolation.");
  }
  return expr;
}

std::string Parser::decodeEscapes(const std::string& raw, Span span) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= raw.size()) {
      sink_.error(span, "Unterminated escape sequence in string literal.");
      break;
    }
    char esc = raw[++i];
    switch (esc) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'a': out.push_back('\a'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'v': out.push_back('\v'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '\'': out.push_back('\''); break;
      case '{': out.push_back('{'); break;
      case '}': out.push_back('}'); break;
      case 'x': {
        int digits = 0;
        unsigned value = 0;
        while (digits < 2 && i + 1 < raw.size() &&
               std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
          char d = raw[++i];
          value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(d))
                                                          ? d - '0'
                                                          : (std::tolower(d) - 'a' + 10));
          ++digits;
        }
        if (digits == 0) {
          sink_.error(span, "Escape '\\x' needs two hexadecimal digits.",
                      "Write '\\x41' for 'A'.");
        }
        out.push_back(static_cast<char>(value));
        break;
      }
      case 'u': {
        unsigned value = 0;
        int digits = 0;
        if (i + 1 < raw.size() && raw[i + 1] == '{') {
          ++i;
          while (i + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
            char d = raw[++i];
            value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(d))
                                                            ? d - '0'
                                                            : (std::tolower(d) - 'a' + 10));
            ++digits;
          }
          if (i + 1 < raw.size() && raw[i + 1] == '}') ++i;
        }
        if (digits == 0 || value > 0x10FFFF) {
          sink_.error(span, "Invalid Unicode escape in string literal.",
                      "Write at most six hex digits, e.g. '\\u{1F600}'.");
          break;
        }
        if (value < 0x80) {
          out.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (value >> 6)));
          out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value < 0x10000) {
          out.push_back(static_cast<char>(0xE0 | (value >> 12)));
          out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | (value >> 18)));
          out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
        break;
      }
      default:
        sink_.error(span, std::string("Unknown escape sequence '\\") + esc + "'.",
                    "Supported escapes: \\n \\t \\r \\a \\b \\f \\v \\0 \\xNN \\u{...} \\\\ \\\" \\' \\{ \\}.");
        out.push_back(esc);
        break;
    }
  }
  return out;
}

std::vector<StringPart> Parser::parseStringParts(const Token& token) {
  const std::string& raw = token.strValue;
  std::vector<StringPart> parts;
  if (token.rawString) {
    StringPart part;
    part.isExpr = false;
    part.text = raw;
    if (!part.text.empty()) parts.push_back(std::move(part));
    return parts;
  }
  std::string literal;
  std::size_t i = 0;

  auto flushLiteral = [&]() {
    if (literal.empty()) return;
    StringPart part;
    part.isExpr = false;
    part.text = decodeEscapes(literal, token.span);
    literal.clear();
    parts.push_back(std::move(part));
  };

  while (i < raw.size()) {
    char c = raw[i];
    if (c == '\\') {
      if (i + 2 < raw.size() && raw[i + 1] == 'u' && raw[i + 2] == '{') {
        std::size_t j = i + 3;
        while (j < raw.size() && raw[j] != '}') ++j;
        if (j < raw.size()) {
          literal.append(raw, i, j - i + 1);
          i = j + 1;
          continue;
        }
      }
      if (i + 1 < raw.size()) {
        if (raw[i + 1] == '{') {
          literal += '{';
          i += 2;
          continue;
        }
        literal += c;
        literal += raw[i + 1];
        i += 2;
        continue;
      }
      literal += c;
      ++i;
      continue;
    }
    if (c == '{' && i + 1 < raw.size() && raw[i + 1] == '{') {
      literal += '{';
      i += 2;
      continue;
    }
    if (c == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
      literal += '}';
      i += 2;
      continue;
    }
    if (c != '{') {
      literal += c;
      ++i;
      continue;
    }

    // Find the matching '}' while respecting nested braces and strings.
    int depth = 1;
    bool inString = false;
    std::size_t j = i + 1;
    while (j < raw.size() && depth > 0) {
      char d = raw[j];
      if (inString) {
        if (d == '\\') {
          ++j;
        } else if (d == '"') {
          inString = false;
        }
      } else if (d == '"') {
        inString = true;
      } else if (d == '{') {
        ++depth;
      } else if (d == '}') {
        --depth;
      }
      ++j;
    }
    if (depth != 0) {
      sink_.error(Span::join(token.span, token.span),
                  "Unterminated '{' in string interpolation.",
                  "Close the expression with '}'. Write '\\{' for a literal brace.");
      break;
    }

    std::string inner = raw.substr(i + 1, j - i - 2);
    flushLiteral();

    Span base = token.span;
    base.line = token.span.line;
    base.col = token.span.col + 1 + static_cast<int>(i + 1);

    StringPart part;
    part.isExpr = true;
    try {
      Lexer subLexer(inner, sink_, base.fileId, base.line, base.col, /*inlineMode=*/true);
      Parser subParser(subLexer.tokenize(), sink_);
      part.expr = subParser.parseInlineExpression();
    } catch (ParseError&) {
      auto placeholder = std::make_unique<IntLitExpr>();
      placeholder->span = base;
      placeholder->value = 0;
      part.expr = std::move(placeholder);
    }
    parts.push_back(std::move(part));
    i = j;
  }

  flushLiteral();
  return parts;
}
}  // namespace a
