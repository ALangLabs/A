#pragma once

#include <memory>
#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "lexer/token.hpp"
#include "ast/ast.hpp"

namespace a {

class Parser {
 public:
  Parser(std::vector<Token> tokens, DiagnosticSink& sink);

  std::unique_ptr<Program> parseProgram();

  // Parses a single expression (used for the fragments inside string
  // interpolation). Reports diagnostics and returns an error placeholder on
  // failure.
  ExprPtr parseInlineExpression();

 private:
  struct ParseError {};

  const Token& cur() const { return tokens_[pos_]; }
  const Token& peek(std::size_t ahead) const;
  bool check(Tok kind) const { return cur().kind == kind; }
  bool match(Tok kind);
  const Token& advance();
  const Token& expect(Tok kind, const std::string& what);

  // Span of the last consumed token that carries source text (skips the
  // structural NEWLINE/INDENT/DEDENT tokens). Statement spans use this so they
  // end on the statement's final line rather than the following blank line.
  Span previousSpan() const;

  [[noreturn]] void fail(Span span, std::string message, std::string help = {});
  void synchronize();

  StmtPtr parseStatement();
  StmtPtr parseFnDecl(bool entryKw = false);
  StmtPtr parseStructDecl();
  StmtPtr parseEnumDecl();
  StmtPtr parseTraitDecl();
  StmtPtr parseImplDecl();
  StmtPtr parseTypeAliasDecl();
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseDoWhile();
  StmtPtr parseFor();
  StmtPtr parseLoop();
  StmtPtr parseReturn();
  StmtPtr parseBreak();
  StmtPtr parseContinue();
  StmtPtr parseDefer();
  StmtPtr parseDel();
  StmtPtr parseImport();
  StmtPtr parseFromImport();
  StmtPtr parseTypedDeclaration();
  StmtPtr parseSimpleStatement();

  // Parses a string-literal statement inside a struct/trait/enum/impl body as
  // a docstring, storing its text in `text`. Fails when the expression is not
  // a plain (non-interpolated) string literal.
  void parseDocstringStatement(std::string& text);

  std::vector<StmtPtr> parseSuite();
  void expectNewline();

  ParamDecl parseParam(bool allowDefault = false);
  TypeExprPtr parseTypeExpr();
  // Parses a `<...>` generic parameter list (current token is '<'). Each
  // parameter is `Name` or `Name: Bound` / `Name: BoundA + BoundB`; bound
  // trait names are appended to `bounds` (parallel spans in `boundSpans`) so
  // they get the same constraint semantics as a `where` clause.
  void parseTypeParams(std::vector<std::string>& typeParams,
                       std::vector<std::string>& bounds,
                       std::vector<Span>& boundSpans);

  ExprPtr parseExpression(int minPrec = 0);
  ExprPtr parsePrefix();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  ExprPtr parseLambda();
  ExprPtr parseMatch();
  Pattern parsePattern();
  Pattern parsePrimaryPattern();
  ExprPtr parseCallOrIndexPostfix(ExprPtr base);
  std::vector<CallArg> parseCallArgs();
  std::vector<StringPart> parseStringParts(const Token& token);
  std::string decodeEscapes(const std::string& raw, Span span);

  StmtPtr parseDestructureDecl(bool mutableBinding, Span start);
  StmtPtr buildDestructure(ExprPtr target, ExprPtr value, bool mutableBinding, Span span);

  static int binaryPrecedence(Tok kind);
  static bool isRightAssociative(Tok kind);
  static const char* opName(Tok kind);
  static BinOp binOp(Tok kind);
  static bool isAssignmentOp(Tok kind);

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  // Extra statements produced by one source statement (e.g. `x = y = v`
  // chained assignment). Callers append them after the returned statement.
  std::vector<StmtPtr> pendingStmts_;
  // Nesting depth of `fn` bodies being parsed; >0 means a `fn` declaration
  // here is nested inside another function.
  int fnDepth_ = 0;
  DiagnosticSink& sink_;
};

}  // namespace a
