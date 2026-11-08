#include "lexer/lexer_internal.hpp"

namespace a {
void Lexer::scanIdentifier() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  advance();
  while (!atEnd() && isIdentContinue(peek())) advance();
  std::string lexeme = text_.substr(start, pos_ - start);
  auto it = keywordTable().find(lexeme);
  Tok kind = it != keywordTable().end() ? it->second : Tok::Ident;
  add(kind, start, startLine, startCol);
}

void Lexer::scanNumber() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  bool isFloat = false;

  // Check for hex (0x / 0X) or binary (0b / 0B) or octal (0o / 0O)
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    advance(); // '0'
    advance(); // 'x'
    std::string hexDigits;
    while (!atEnd() && (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
      char c = advance();
      if (c != '_') hexDigits.push_back(c);
    }
    Token token;
    token.kind = Tok::Int;
    token.lexeme = text_.substr(start, pos_ - start);
    token.span = makeSpan(start, startLine, startCol);
    if (hexDigits.empty()) {
      error(token.span, "Hexadecimal literal has no digits.", "Write digits after '0x', e.g. '0xFF'.");
      token.intValue = 0;
    } else {
      errno = 0;
      char* end = nullptr;
      unsigned long long val = std::strtoull(hexDigits.c_str(), &end, 16);
      if (errno == ERANGE) {
        error(token.span, "Hexadecimal literal is too large.", "Literal must fit in 64 bits.");
        val = 0;
      }
      token.intValue = static_cast<std::int64_t>(val);
    }
    tokens_.push_back(std::move(token));
    return;
  }

  if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
    advance(); // '0'
    advance(); // 'b'
    std::string binDigits;
    while (!atEnd() && (peek() == '0' || peek() == '1' || peek() == '_')) {
      char c = advance();
      if (c != '_') binDigits.push_back(c);
    }
    Token token;
    token.kind = Tok::Int;
    token.lexeme = text_.substr(start, pos_ - start);
    token.span = makeSpan(start, startLine, startCol);
    if (binDigits.empty()) {
      error(token.span, "Binary literal has no digits.", "Write binary digits after '0b', e.g. '0b1010'.");
      token.intValue = 0;
    } else {
      errno = 0;
      char* end = nullptr;
      unsigned long long val = std::strtoull(binDigits.c_str(), &end, 2);
      if (errno == ERANGE) {
        error(token.span, "Binary literal is too large.", "Literal must fit in 64 bits.");
        val = 0;
      }
      token.intValue = static_cast<std::int64_t>(val);
    }
    tokens_.push_back(std::move(token));
    return;
  }

  if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
    advance(); // '0'
    advance(); // 'o'
    std::string octDigits;
    while (!atEnd() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
      char c = advance();
      if (c != '_') octDigits.push_back(c);
    }
    Token token;
    token.kind = Tok::Int;
    token.lexeme = text_.substr(start, pos_ - start);
    token.span = makeSpan(start, startLine, startCol);
    if (octDigits.empty()) {
      error(token.span, "Octal literal has no digits.", "Write octal digits after '0o', e.g. '0o17'.");
      token.intValue = 0;
    } else {
      errno = 0;
      char* end = nullptr;
      unsigned long long val = std::strtoull(octDigits.c_str(), &end, 8);
      if (errno == ERANGE) {
        error(token.span, "Octal literal is too large.", "Literal must fit in 64 bits.");
        val = 0;
      }
      token.intValue = static_cast<std::int64_t>(val);
    }
    tokens_.push_back(std::move(token));
    return;
  }

  while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();

  // A '.' starts a fractional part when the next character is a digit, or
  // when it cannot begin a field name or another '.', so that `1..5` lexes
  // as `1`, `..`, `5` and `1.foo` stays a field access, while `1.` followed
  // by `)`, `,`, an operator, etc. is a float literal like in Python.
  if (peek() == '.' &&
      (isDigit(peek(1)) || (peek(1) != '.' && !isIdentStart(peek(1))))) {
    isFloat = true;
    advance();
    while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
  }

  if ((peek() == 'e' || peek() == 'E')) {
    std::size_t save = pos_;
    int saveLine = line_;
    int saveCol = col_;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (isDigit(peek())) {
      isFloat = true;
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    } else {
      pos_ = save;
      line_ = saveLine;
      col_ = saveCol;
    }
  }

  std::string lexeme = text_.substr(start, pos_ - start);
  std::string cleaned;
  cleaned.reserve(lexeme.size());
  for (char c : lexeme) {
    if (c != '_') cleaned.push_back(c);
  }

  Token token;
  token.kind = isFloat ? Tok::Float : Tok::Int;
  token.lexeme = lexeme;
  token.span = makeSpan(start, startLine, startCol);
  if (isFloat) {
    token.floatValue = std::strtod(cleaned.c_str(), nullptr);
  } else {
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(cleaned.c_str(), &end, 10);
    if (errno == ERANGE) {
      error(token.span, "Integer literal is too large.", "Integer literals must fit in 64 bits.");
      value = 0;
    }
    token.intValue = value;
  }
  tokens_.push_back(std::move(token));
}

void Lexer::scanString() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;

  if (peek(1) == '"' && peek(2) == '"') {
    advance();
    advance();
    advance();
    if (peek() == '\r' && peek(1) == '\n') {
      advance();
      advance();
    } else if (peek() == '\n') {
      advance();
    }
    std::size_t contentStart = pos_;
    bool terminated = false;
    while (!atEnd()) {
      if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
        std::size_t contentEnd = pos_;
        advance();
        advance();
        advance();
        terminated = true;
        Token token;
        token.kind = Tok::String;
        token.rawString = true;
        token.span = makeSpan(start, startLine, startCol);
        token.lexeme = text_.substr(start, pos_ - start);
        token.strValue = text_.substr(contentStart, contentEnd - contentStart);
        if (!terminated) {
          error(token.span, "Unterminated triple-quoted string literal.",
                "Close the raw string with '\"\"\"'.");
        }
        tokens_.push_back(std::move(token));
        return;
      }
      advance();
    }
    Token token;
    token.kind = Tok::String;
    token.rawString = true;
    token.span = makeSpan(start, startLine, startCol);
    token.lexeme = text_.substr(start, pos_ - start);
    token.strValue = text_.substr(contentStart);
    error(token.span, "Unterminated triple-quoted string literal.",
          "Close the raw string with '\"\"\"'.");
    tokens_.push_back(std::move(token));
    return;
  }

  advance();  // opening quote
  std::string raw;
  bool terminated = false;
  while (!atEnd()) {
    char c = peek();
    if (c == '"') {
      advance();
      terminated = true;
      break;
    }
    if (c == '\n') break;
    if (c == '\\') {
      raw.push_back(c);
      advance();
      if (atEnd()) break;
      if (peek() == '\n') break;
      raw.push_back(peek());
      advance();
      continue;
    }
    raw.push_back(c);
    advance();
  }
  Token token;
  token.kind = Tok::String;
  token.span = makeSpan(start, startLine, startCol);
  token.lexeme = text_.substr(start, pos_ - start);
  token.strValue = std::move(raw);
  if (!terminated) {
    error(token.span, "Unterminated string literal.", "Add a closing '\"' on the same line.");
  }
  tokens_.push_back(std::move(token));
}

void Lexer::scanChar() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  advance();  // opening quote
  if (atEnd() || peek() == '\n') {
    Token token;
    token.kind = Tok::Char;
    token.span = makeSpan(start, startLine, startCol);
    error(token.span, "Unterminated character literal.", "Write a character like 'a'.");
    tokens_.push_back(std::move(token));
    return;
  }

  char32_t value = 0;
  bool ok = true;
  if (peek() == '\\') {
    advance();
    if (atEnd()) {
      ok = false;
    } else {
      char esc = advance();
      switch (esc) {
        case 'n': value = '\n'; break;
        case 't': value = '\t'; break;
        case 'r': value = '\r'; break;
        case 'a': value = '\a'; break;
        case 'b': value = '\b'; break;
        case 'f': value = '\f'; break;
        case 'v': value = '\v'; break;
        case '0': value = 0; break;
        case '\\': value = '\\'; break;
        case '\'': value = '\''; break;
        case '"': value = '"'; break;
        case 'x': {
          char32_t parsed = 0;
          int digits = 0;
          while (digits < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
            char d = advance();
            parsed = parsed * 16 + static_cast<char32_t>(std::isdigit(static_cast<unsigned char>(d))
                                                             ? d - '0'
                                                             : (std::tolower(d) - 'a' + 10));
            ++digits;
          }
          if (digits == 0) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Escape '\\x' needs two hexadecimal digits.", "Write '\\x41' for 'A'.");
          }
          value = parsed;
          break;
        }
        case 'u': {
          if (!matchChar('{')) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Unicode escapes use the form '\\u{1F600}'.");
            break;
          }
          char32_t parsed = 0;
          int digits = 0;
          while (std::isxdigit(static_cast<unsigned char>(peek()))) {
            char d = advance();
            parsed = parsed * 16 + static_cast<char32_t>(std::isdigit(static_cast<unsigned char>(d))
                                                             ? d - '0'
                                                             : (std::tolower(d) - 'a' + 10));
            ++digits;
          }
          if (digits == 0 || !matchChar('}') || parsed > 0x10FFFF) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Invalid Unicode escape in character literal.",
                  "Write at most six hex digits, e.g. '\\u{1F600}'.");
          }
          value = parsed;
          break;
        }
        default:
          ok = false;
          error(makeSpan(start, startLine, startCol), "Unknown escape sequence in character literal.",
                "Supported escapes: \\n \\t \\r \\a \\b \\f \\v \\0 \\xNN \\u{...} \\\\ \\' \\\".");
          value = static_cast<unsigned char>(esc);
          break;
      }
    }
  } else {
    unsigned char byte = static_cast<unsigned char>(advance());
    if (byte < 0x80) {
      value = byte;
    } else {
      ok = false;
      error(makeSpan(start, startLine, startCol),
            "Non-ASCII character literal is not supported yet.",
            "Use a string literal for Unicode text.");
      value = byte;
    }
  }

  bool closed = matchChar('\'');
  Token token;
  token.kind = Tok::Char;
  token.span = makeSpan(start, startLine, startCol);
  token.lexeme = text_.substr(start, pos_ - start);
  token.charValue = value;
  if (!closed) {
    error(token.span, "Unterminated character literal.", "Add a closing '\\'' after one character.");
  }
  if (!ok) {
    // keep going; diagnostic already reported
  }
  tokens_.push_back(std::move(token));
}

}  // namespace a
