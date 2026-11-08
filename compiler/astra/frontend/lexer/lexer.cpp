#include "lexer/lexer.hpp"
#include "lexer/lexer_internal.hpp"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace a {

const char* tokName(Tok kind) {
  switch (kind) {
    case Tok::Eof: return "end of file";
    case Tok::Newline: return "end of line";
    case Tok::Indent: return "indent";
    case Tok::Dedent: return "dedent";
    case Tok::Ident: return "identifier";
    case Tok::Int: return "integer literal";
    case Tok::Float: return "float literal";
    case Tok::String: return "string literal";
    case Tok::Char: return "character literal";
    case Tok::KwFn: return "'fn'";
    case Tok::KwIf: return "'if'";
    case Tok::KwElif: return "'elif'";
    case Tok::KwElse: return "'else'";
    case Tok::KwWhile: return "'while'";
    case Tok::KwFor: return "'for'";
    case Tok::KwIn: return "'in'";
    case Tok::KwLoop: return "'loop'";
    case Tok::KwBreak: return "'break'";
    case Tok::KwContinue: return "'continue'";
    case Tok::KwReturn: return "'return'";
    case Tok::KwDefer: return "'defer'";
    case Tok::KwClass: return "'class'";
    case Tok::KwMetaclass: return "'metaclass'";
    case Tok::KwPass: return "'pass'";
    case Tok::KwSelf: return "'self'";
    case Tok::KwTrue: return "'true'";
    case Tok::KwFalse: return "'false'";
    case Tok::KwAnd: return "'and'";
    case Tok::KwOr: return "'or'";
    case Tok::KwNot: return "'not'";
    case Tok::KwImport: return "'import'";
    case Tok::KwFrom: return "'from'";
    case Tok::KwAs: return "'as'";
    case Tok::KwMatch: return "'match'";
    case Tok::KwNone: return "'None'";
    case Tok::KwEnum: return "'enum'";
    case Tok::KwDo: return "'do'";
  case Tok::KwType: return "'type'";
    case Tok::KwTrait: return "'trait'";
    case Tok::KwWhere: return "'where'";
    case Tok::KwComptime: return "'comptime'";
    case Tok::KwImpl: return "'impl'";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Colon: return "':'";
    case Tok::Dot: return "'.'";
    case Tok::DotDot: return "'..'";
    case Tok::Arrow: return "'->'";
    case Tok::FatArrow: return "'=>'";
    case Tok::Pipe: return "'|>'";
    case Tok::Bang: return "'!'";
    case Tok::Question: return "'?'";
    case Tok::QuestionDot: return "'?.'";
    case Tok::QuestionQuestion: return "'\?\?'";
    case Tok::Amp: return "'&'";
    case Tok::Bar: return "'|'";
    case Tok::Caret: return "'^'";
    case Tok::Tilde: return "'~'";
    case Tok::Shl: return "'<<'";
    case Tok::Shr: return "'>>'";
    case Tok::PlusPlus: return "'++'";
    case Tok::MinusMinus: return "'--'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::StarStar: return "'**'";
    case Tok::Eq: return "'='";
    case Tok::PlusEq: return "'+='";
    case Tok::MinusEq: return "'-='";
    case Tok::StarEq: return "'*='";
    case Tok::SlashEq: return "'/='";
    case Tok::PercentEq: return "'%='";
    case Tok::AmpEq: return "'&='";
    case Tok::BarEq: return "'|='";
    case Tok::CaretEq: return "'^='";
    case Tok::ShlEq: return "'<<='";
    case Tok::ShrEq: return "'>>='";
    case Tok::EqEq: return "'=='";
    case Tok::NotEq: return "'!='";
    case Tok::Lt: return "'<'";
    case Tok::Gt: return "'>'";
    case Tok::Le: return "'<='";
    case Tok::Ge: return "'>='";
  }
  return "token";
}

std::string describeToken(const Token& token) {
  switch (token.kind) {
    case Tok::Ident:
    case Tok::Int:
    case Tok::Float:
      return "'" + token.lexeme + "'";
    case Tok::String:
      return "string literal";
    case Tok::Char:
      return "character literal";
    case Tok::Eof:
      return "end of file";
    case Tok::Newline:
      return "end of line";
    case Tok::Indent:
      return "indent";
    case Tok::Dedent:
      return "dedent";
    default:
      return tokName(token.kind);
  }
}


Lexer::Lexer(std::string text, DiagnosticSink& sink, int fileId, int baseLine, int baseCol,
             bool inlineMode)
    : text_(std::move(text)),
      sink_(sink),
      fileId_(fileId),
      baseLine_(baseLine),
      baseCol_(baseCol),
      inlineMode_(inlineMode) {
  line_ = baseLine_;
  col_ = baseCol_;
  if (inlineMode_) atLineStart_ = false;
}

char Lexer::peek(std::size_t ahead) const {
  if (pos_ + ahead >= text_.size()) return '\0';
  return text_[pos_ + ahead];
}

char Lexer::advance() {
  char c = text_[pos_++];
  if (c == '\n') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  return c;
}

bool Lexer::matchChar(char expected) {
  if (atEnd() || peek() != expected) return false;
  advance();
  return true;
}

Span Lexer::makeSpan(std::size_t start, int startLine, int startCol) const {
  Span span;
  span.line = startLine;
  span.col = startCol;
  span.endLine = line_;
  span.endCol = col_;
  if (start >= text_.size()) {
    span.endLine = startLine;
    span.endCol = startCol;
  }
  return span;
}

void Lexer::error(Span span, std::string message, std::string help) {
  sink_.error(span, std::move(message), std::move(help));
}

void Lexer::add(Tok kind, std::size_t start, int startLine, int startCol) {
  Token token;
  token.kind = kind;
  token.lexeme = text_.substr(start, pos_ - start);
  token.span = makeSpan(start, startLine, startCol);
  tokens_.push_back(std::move(token));
}

void Lexer::skipComment() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  while (!atEnd() && peek() != '\n') advance();

  Comment comment;
  comment.span = makeSpan(start, startLine, startCol);
  comment.text = text_.substr(start, pos_ - start);
  if (!tokens_.empty()) {
    const Token& previous = tokens_.back();
    comment.trailing = previous.kind != Tok::Newline && previous.kind != Tok::Indent &&
                       previous.kind != Tok::Dedent && previous.kind != Tok::Eof &&
                       previous.span.endLine == startLine;
  }
  comments_.push_back(std::move(comment));
}

void Lexer::emitNewline() {
  if (tokens_.empty()) return;
  Tok last = tokens_.back().kind;
  if (last == Tok::Newline || last == Tok::Indent || last == Tok::Dedent ||
      last == Tok::Eof)
    return;
  Token token;
  token.kind = Tok::Newline;
  token.span.line = line_;
  token.span.col = col_;
  token.span.endLine = line_;
  token.span.endCol = col_ + 1;
  tokens_.push_back(std::move(token));
}

bool Lexer::handleLineStart() {
  int indent = 0;
  int indentLine = line_;
  int indentCol = col_;
  while (!atEnd() && (peek() == ' ' || peek() == '\t')) {
    if (peek() == '\t') {
      Span s;
      s.line = line_;
      s.col = col_;
      s.endLine = line_;
      s.endCol = col_ + 1;
      error(s, "Tabs are not allowed for indentation.",
            "Use spaces (4 per level) to indent blocks.");
    }
    advance();
    ++indent;
  }
  if (atEnd()) return false;

  if (peek() == '\n') {
    advance();
    return false;
  }
  if (peek() == '\r') {
    advance();
    return false;
  }
  if (peek() == '#') {
    skipComment();
    if (!atEnd() && peek() == '\n') advance();
    return false;
  }

  if (indent > indentStack_.back()) {
    indentStack_.push_back(indent);
    Token token;
    token.kind = Tok::Indent;
    token.span.line = indentLine;
    token.span.col = indentCol;
    token.span.endLine = indentLine;
    token.span.endCol = indentCol + 1;
    tokens_.push_back(std::move(token));
  } else if (indent < indentStack_.back()) {
    while (!indentStack_.empty() && indent < indentStack_.back()) {
      indentStack_.pop_back();
      Token token;
      token.kind = Tok::Dedent;
      token.span.line = line_;
      token.span.col = col_;
      token.span.endLine = line_;
      token.span.endCol = col_ + 1;
      tokens_.push_back(std::move(token));
    }
    if (indentStack_.empty() || indent != indentStack_.back()) {
      Span s;
      s.line = line_;
      s.col = 1;
      s.endLine = line_;
      s.endCol = indent + 1;
      error(s, "Inconsistent indentation.",
            "This line's indentation does not match any enclosing block.");
      indentStack_.assign(1, 0);
    }
  }
  atLineStart_ = false;
  return true;
}

void Lexer::finalize() {
  if (tokens_.empty()) {
    Token eof;
    eof.kind = Tok::Eof;
    eof.span.line = line_;
    eof.span.col = col_;
    eof.span.endLine = line_;
    eof.span.endCol = col_;
    eof.span.fileId = fileId_;
    tokens_.push_back(std::move(eof));
    return;
  }
  Tok last = tokens_.back().kind;
  if (last != Tok::Newline && last != Tok::Dedent && last != Tok::Indent &&
      last != Tok::Eof) {
    Token token;
    token.kind = Tok::Newline;
    token.span.line = line_;
    token.span.col = col_;
    token.span.endLine = line_;
    token.span.endCol = col_ + 1;
    token.span.fileId = fileId_;
    tokens_.push_back(std::move(token));
  }
  while (indentStack_.size() > 1) {
    indentStack_.pop_back();
    Token token;
    token.kind = Tok::Dedent;
    token.span.line = line_;
    token.span.col = col_;
    token.span.endLine = line_;
    token.span.endCol = col_ + 1;
    token.span.fileId = fileId_;
    tokens_.push_back(std::move(token));
  }
  Token eof;
  eof.kind = Tok::Eof;
  eof.span.line = line_;
  eof.span.col = col_;
  eof.span.endLine = line_;
  eof.span.endCol = col_;
  eof.span.fileId = fileId_;
  tokens_.push_back(std::move(eof));
}

std::vector<Token> Lexer::tokenize() {
  while (!atEnd()) {
    if (!inlineMode_ && atLineStart_ && bracketDepth_ == 0) {
      if (!handleLineStart()) continue;
    }

    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r') {
      advance();
      continue;
    }
    if (c == '\n') {
      advance();
      if (!inlineMode_ && bracketDepth_ == 0) {
        // A line that continues with the pipeline operator belongs to the
        // previous statement, so suppress the newline and indentation.
        std::size_t look = pos_;
        while (look < text_.size() &&
               (text_[look] == ' ' || text_[look] == '\t' || text_[look] == '\r')) {
          ++look;
        }
        bool continuation = look + 1 < text_.size() && text_[look] == '|' &&
                            text_[look + 1] == '>';
        if (!continuation) {
          emitNewline();
          atLineStart_ = true;
        }
      }
      continue;
    }
    if (c == '#') {
      skipComment();
      continue;
    }

    if (isIdentStart(c)) {
      scanIdentifier();
      continue;
    }
    if (isDigit(c)) {
      scanNumber();
      continue;
    }

    std::size_t start = pos_;
    int startLine = line_;
    int startCol = col_;

    char ch = peek();
    auto emit1 = [&](Tok kind) {
      advance();
      add(kind, start, startLine, startCol);
    };
    auto emit2 = [&](Tok kind) {
      advance();
      advance();
      add(kind, start, startLine, startCol);
    };
    auto emit3 = [&](Tok kind) {
      advance();
      advance();
      advance();
      add(kind, start, startLine, startCol);
    };
    switch (ch) {
      case '"':
        scanString();
        break;
      case '\'':
        scanChar();
        break;
      case '(':
        ++bracketDepth_;
        emit1(Tok::LParen);
        break;
      case ')':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RParen);
        break;
      case '[':
        ++bracketDepth_;
        emit1(Tok::LBracket);
        break;
      case ']':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RBracket);
        break;
      case '{':
        ++bracketDepth_;
        emit1(Tok::LBrace);
        break;
      case '}':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RBrace);
        break;
      case ',':
        emit1(Tok::Comma);
        break;
      case ':':
        emit1(Tok::Colon);
        break;
      case '+':
        if (peek(1) == '+') emit2(Tok::PlusPlus);
        else if (peek(1) == '=') emit2(Tok::PlusEq);
        else emit1(Tok::Plus);
        break;
      case '-':
        if (peek(1) == '-') emit2(Tok::MinusMinus);
        else if (peek(1) == '=') emit2(Tok::MinusEq);
        else if (peek(1) == '>') emit2(Tok::Arrow);
        else emit1(Tok::Minus);
        break;
      case '*':
        peek(1) == '*' ? emit2(Tok::StarStar)
                       : peek(1) == '=' ? emit2(Tok::StarEq) : emit1(Tok::Star);
        break;
      case '/':
        peek(1) == '=' ? emit2(Tok::SlashEq) : emit1(Tok::Slash);
        break;
      case '%':
        peek(1) == '=' ? emit2(Tok::PercentEq) : emit1(Tok::Percent);
        break;
      case '=':
        peek(1) == '>' ? emit2(Tok::FatArrow)
                       : peek(1) == '=' ? emit2(Tok::EqEq) : emit1(Tok::Eq);
        break;
      case '!':
        peek(1) == '=' ? emit2(Tok::NotEq) : emit1(Tok::Bang);
        break;
      case '?':
        peek(1) == '.' ? emit2(Tok::QuestionDot)
                       : peek(1) == '?' ? emit2(Tok::QuestionQuestion) : emit1(Tok::Question);
        break;
      case '|':
        if (peek(1) == '>') {
          emit2(Tok::Pipe);
        } else if (peek(1) == '=') {
          emit2(Tok::BarEq);
        } else {
          emit1(Tok::Bar);
        }
        break;
      case '&':
        peek(1) == '=' ? emit2(Tok::AmpEq) : emit1(Tok::Amp);
        break;
      case '^':
        peek(1) == '=' ? emit2(Tok::CaretEq) : emit1(Tok::Caret);
        break;
      case '~':
        emit1(Tok::Tilde);
        break;
      case '<':
        if (peek(1) == '<') {
          peek(2) == '=' ? emit3(Tok::ShlEq) : emit2(Tok::Shl);
        } else {
          peek(1) == '=' ? emit2(Tok::Le) : emit1(Tok::Lt);
        }
        break;
      case '>':
        if (peek(1) == '>') {
          peek(2) == '=' ? emit3(Tok::ShrEq) : emit2(Tok::Shr);
        } else {
          peek(1) == '=' ? emit2(Tok::Ge) : emit1(Tok::Gt);
        }
        break;
      case '.':
        peek(1) == '.' ? emit2(Tok::DotDot) : emit1(Tok::Dot);
        break;
      default: {
        advance();
        char bad[2] = {ch, '\0'};
        std::string shown;
        if (static_cast<unsigned char>(ch) >= 32 && static_cast<unsigned char>(ch) < 127) {
          shown = std::string("'") + bad + "'";
        } else {
          shown = "this byte";
        }
        error(makeSpan(start, startLine, startCol), "Unexpected character " + shown + ".");
        break;
      }
    }
  }

  for (auto& token : tokens_) token.span.fileId = fileId_;

  if (inlineMode_) {
    Token eof;
    eof.kind = Tok::Eof;
    eof.span.line = line_;
    eof.span.col = col_;
    eof.span.endLine = line_;
    eof.span.endCol = col_;
    eof.span.fileId = fileId_;
    tokens_.push_back(std::move(eof));
    return tokens_;
  }

  finalize();
  return tokens_;
}

}  // namespace a
