#include "parser/parser_internal.hpp"

namespace a {
Parser::Parser(std::vector<Token> tokens, DiagnosticSink& sink)
    : tokens_(std::move(tokens)), sink_(sink) {}

const Token& Parser::peek(std::size_t ahead) const {
  std::size_t index = pos_ + ahead;
  if (index >= tokens_.size()) index = tokens_.size() - 1;
  return tokens_[index];
}

bool Parser::match(Tok kind) {
  if (!check(kind)) return false;
  advance();
  return true;
}

const Token& Parser::advance() {
  const Token& token = tokens_[pos_];
  if (pos_ + 1 < tokens_.size()) ++pos_;
  return token;
}

const Token& Parser::expect(Tok kind, const std::string& what) {
  if (check(kind)) return advance();
  fail(cur().span, "Expected " + what + ", found " + describeToken(cur()) + ".");
}

Span Parser::previousSpan() const {
  for (std::size_t i = pos_; i > 0; --i) {
    Tok kind = tokens_[i - 1].kind;
    if (kind == Tok::Newline || kind == Tok::Indent || kind == Tok::Dedent ||
        kind == Tok::Eof) {
      continue;
    }
    return tokens_[i - 1].span;
  }
  return Span{};
}


void Parser::fail(Span span, std::string message, std::string help) {
  sink_.error(span, std::move(message), std::move(help));
  throw ParseError{};
}

void Parser::synchronize() {
  pendingStmts_.clear();
  while (!check(Tok::Eof)) {
    if (check(Tok::Dedent)) return;
    if (check(Tok::Newline)) {
      advance();
      return;
    }
    advance();
  }
}

std::unique_ptr<Program> Parser::parseProgram() {
  auto program = std::make_unique<Program>();
  while (!check(Tok::Eof)) {
    if (match(Tok::Newline) || match(Tok::Dedent)) continue;
    try {
      if (check(Tok::Indent)) {
        fail(cur().span, "Unexpected indentation.",
             "Only blocks introduced by ':' may be indented.");
      }
      program->statements.push_back(parseStatement());
      for (auto& extra : pendingStmts_) program->statements.push_back(std::move(extra));
      pendingStmts_.clear();
    } catch (ParseError&) {
      synchronize();
    }
  }
  return program;
}

StmtPtr Parser::parseStatement() {
  // `entry name(...):` — program entry point (alternative to `fn`). The
  // Ident+Ident+LParen lookahead keeps `entry` usable as a normal name:
  // `entry = 1`, `entry(x)`, `entry.main` etc. never match this shape.
  if (cur().kind == Tok::Ident && cur().lexeme == "entry" &&
      peek(1).kind == Tok::Ident && peek(2).kind == Tok::LParen) {
    return parseFnDecl(true);
  }
  switch (cur().kind) {
    case Tok::KwFn:
      return parseFnDecl();
    case Tok::KwExtern:
      return parseExternDecl();
    case Tok::KwClass:
    case Tok::KwMetaclass:
      return parseStructDecl();
    case Tok::KwPass: {
      Span passSpan = cur().span;
      advance();
      expectNewline();
      auto passExpr = std::make_unique<ExprStmt>();
      passExpr->span = passSpan;
      passExpr->expr = std::make_unique<IntLitExpr>();
      passExpr->expr->span = passSpan;
      return passExpr;
    }
     case Tok::KwEnum:
       return parseEnumDecl();
     case Tok::KwTrait:
       return parseTraitDecl();
     case Tok::KwImpl:
       return parseImplDecl();
     case Tok::KwType:
       // 'type(...)' is the builtin type-name call; 'type Name = ...' is an
       // alias declaration. The '(' disambiguates.
       if (peek(1).kind == Tok::LParen) return parseSimpleStatement();
       return parseTypeAliasDecl();
    case Tok::KwComptime:
      // `comptime if COND:` / `comptime for i in xs:` — compile-time
      // control flow, resolved by the comptime pass before sema.
      if (peek(1).kind == Tok::KwIf) return parseComptimeIf();
      if (peek(1).kind == Tok::KwFor) return parseComptimeFor();
      fail(cur().span, "Expected 'if' or 'for' after 'comptime'.",
           "Write comptime if COND: or comptime for i in xs:.");
    case Tok::KwIf:
      return parseIf();
    case Tok::KwWhile:
      return parseWhile();
    case Tok::KwDo:
      return parseDoWhile();
    case Tok::KwFor:
      return parseFor();
    case Tok::KwLoop:
      return parseLoop();
    case Tok::KwReturn:
      return parseReturn();
    case Tok::KwBreak:
      return parseBreak();
    case Tok::KwContinue:
      return parseContinue();
    case Tok::KwDefer:
      return parseDefer();
    case Tok::KwDel:
      return parseDel();
    case Tok::KwImport:
      return parseImport();
    case Tok::KwFrom:
      return parseFromImport();
    case Tok::KwElif:
    case Tok::KwElse:
      fail(cur().span, std::string("Unexpected ") + tokName(cur().kind) + ".",
           "This branch has no matching 'if'.");
    case Tok::Indent:
      fail(cur().span, "Unexpected indentation.");
    default:
      break;
  }

  if (check(Tok::Ident) && peek(1).kind == Tok::Colon) {
    return parseTypedDeclaration();
  }
  return parseSimpleStatement();
}

void Parser::expectNewline() {
  if (check(Tok::Eof)) return;
  // Block-shaped expressions (match) consume their closing newline, so a
  // dedent or a just-closed block already ends the statement.
  if (check(Tok::Dedent)) return;
  if (pos_ > 0 && tokens_[pos_ - 1].kind == Tok::Dedent) return;
  expect(Tok::Newline, "end of line");
}

std::vector<StmtPtr> Parser::parseSuite() {
  std::vector<StmtPtr> body;
  if (match(Tok::Newline)) {
    expect(Tok::Indent, "an indented block");
    while (!check(Tok::Dedent) && !check(Tok::Eof)) {
      if (match(Tok::Newline)) continue;
      body.push_back(parseStatement());
      for (auto& extra : pendingStmts_) body.push_back(std::move(extra));
      pendingStmts_.clear();
    }
    expect(Tok::Dedent, "the end of the block");
  } else {
    body.push_back(parseStatement());
    for (auto& extra : pendingStmts_) body.push_back(std::move(extra));
    pendingStmts_.clear();
  }
  return body;
}

void Parser::parseDocstringStatement(std::string& text) {
  ExprPtr expr = parseExpression();
  if (!plainStringText(expr, text)) {
    fail(expr ? expr->span : cur().span, "Expected a string literal docstring.");
  }
  expectNewline();
}

}  // namespace a
