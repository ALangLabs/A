#pragma once

#include <memory>
#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "lexer/token.hpp"
#include "ast/ast.hpp"

namespace a {

struct PendingDoc;

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
  StmtPtr parseFnDecl(bool entryKw = false, bool externFn = false);
  StmtPtr parseExternDecl();
  StmtPtr parseStructDecl();
  StmtPtr parseEnumDecl();
  StmtPtr parseTraitDecl();
  StmtPtr parseImplDecl();
  StmtPtr parseTypeAliasDecl();
  StmtPtr parseIf();
  StmtPtr parseComptimeIf();
  StmtPtr parseComptimeFor();
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

  // Handles a docstring statement inside a declaration body: consumes it
  // when the current token is a string literal and routes the text through
  // `docs` (first docstring before any member becomes the type doc, later
  // ones attach to the next member). `hasMembers` reports whether any member
  // has been parsed so far. Returns true when a docstring was consumed.
  bool consumeBodyDocstring(PendingDoc& docs, bool hasMembers);

  // Parses the `python:<pkg>` / `std:<pkg>` module prefix shared by
  // `import` and `from ... import` (current token is the 'python'/'std'
  // identifier). Stores the joined span in `targetSpan` and the dotted
  // module path in `out`.
  void parseModulePrefix(Span& targetSpan, std::string& out);

  // Parses the module source shared by `import` and `from ... import`:
  // a quoted path, a `python:`/`std:` package path, or a bare module name.
  // Fills the fields common to ImportStmt and FromImportStmt. `keyword` is
  // the statement keyword ("import"/"from") and `help` the diagnostic help
  // text for a missing module source.
  template <typename StmtT>
  void parseModuleSource(StmtT& stmt, const char* keyword, const char* help);

  std::vector<StmtPtr> parseSuite();
  void expectNewline();

  ParamDecl parseParam(bool allowDefault = false);
  // `T | U | ...` union types sit at the top level; everything else (arrays,
  // tuples, maps, sets, fn types, named types, `?` suffixes) is an atom.
  TypeExprPtr parseTypeExpr();
  TypeExprPtr parseTypeExprAtom();
  // Parses a `<...>` generic parameter list (current token is '<'). Each
  // parameter is `Name` or `Name: Bound` / `Name: BoundA + BoundB`; bound
  // trait names are appended to `bounds` (parallel spans in `boundSpans`) so
  // they get the same constraint semantics as a `where` clause.
  void parseTypeParams(std::vector<std::string>& typeParams,
                       std::vector<std::string>& bounds,
                       std::vector<Span>& boundSpans);
  // Parses a `<T, U, ...>` type parameter list without trait bounds
  // (current token is '<'). Used by struct/class and enum declarations.
  void parseTypeParamNames(std::vector<std::string>& typeParams);
  // Parses one type argument inside a '<...>' or '[...]' list: an integer
  // literal (comptime value such as the lane count in `vector[float, 4]`),
  // `...` (the variadic marker in `tuple[T, ...]`, only when allowEllipsis),
  // or a full type expression.
  TypeExprPtr parseTypeArgument(bool allowEllipsis);

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
  // Builds the CallExpr for `callee(args)` (or the `?.` desugaring of
  // `obj?.m(args)`), attaching any explicit `<T, ...>` type arguments.
  ExprPtr buildCallExpr(ExprPtr callee, std::vector<CallArg> args,
                        std::vector<TypeExprPtr> typeArgs, Span endSpan);
  // Lookahead for `expr<T, ...>(`: returns the token index just past the
  // closing '>' when the tokens at `pos` form a well-formed type argument
  // list followed by '(', or ~std::size_t(0) otherwise (the '<' is then a
  // comparison operator).
  std::size_t scanCallTypeArgsEnd(std::size_t pos) const;
  // Consumes a '>' in type-argument position; '>>' lexes as a single token,
  // so it is split into two '>' tokens on demand (see splitShrGt).
  bool matchTypeArgGt();
  void splitShrGt();
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
