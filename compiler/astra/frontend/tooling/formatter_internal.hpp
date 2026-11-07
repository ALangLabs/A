#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.hpp"
#include "lexer/token.hpp"
#include "tooling/formatter.hpp"
#include "tooling/source_text.hpp"

namespace a::tooling::detail {

inline int binaryPrecedence(std::string_view op) {
  if (op == "|>") return 1;
  if (op == "??") return 3;
  if (op == "or") return 4;
  if (op == "and") return 5;
  if (op == "in" || op == "not in" || op == "==" || op == "!=" || op == "<" ||
      op == ">" || op == "<=" || op == ">=") {
    return 7;
  }
  if (op == "..") return 8;
  if (op == "|") return 9;
  if (op == "^") return 10;
  if (op == "&") return 11;
  if (op == "<<" || op == ">>") return 12;
  if (op == "+" || op == "-") return 13;
  if (op == "*" || op == "/" || op == "%") return 14;
  if (op == "**") return 16;
  return 20;
}

inline bool isBlockStatement(const Stmt& statement) {
  switch (statement.kind) {
    case StmtKind::FnDecl:
    case StmtKind::StructDecl:
    case StmtKind::EnumDecl:
    case StmtKind::TraitDecl:
    case StmtKind::ImplDecl:
    case StmtKind::If:
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
    case StmtKind::Loop:
      return true;
    default:
      return false;
  }
}

// Statement/expression pretty-printer shared by the formatter translation
// units. The class is internal to the tooling library; only formatSource() is
// part of the public API.
class Formatter {
 public:
  Formatter(std::string source, std::vector<Comment> comments, FormatOptions options);

  std::string run(Program& program);

 private:
  // ------------------------------------------------------------- comments

  std::string indentText(int level) const;
  void ensureGap(int line, int& lastLine);
  void emitComment(const Comment& comment, int level, int& lastLine);
  void emitLeadingComments(int line, int level, int& lastLine);
  void emitInteriorComments(const Span& span, int level, int& lastLine);
  bool pendingTrailingComment(const Span& span, int& commentIndex) const;
  void attachTrailing(const Span& span, int& lastLine);
  void emitLeftoverComments(int limitLine, int level, int& lastLine);

  // --------------------------------------------------------- statements

  int emitStatements(const std::vector<StmtPtr>& body, int level, int limitLine,
                     int headerLine);
  void emitStatement(const Stmt& statement, int level, int nextLine, int& lastLine);
  void emitBlockStatement(const Stmt& statement, int level, int nextLine, int& lastLine);
  void emitHeader(const std::string& text, int level, const Span& span, int& lastLine);
  void emitDocstring(const std::string& doc, int level);
  void emitFunction(const FnDecl& decl, int level, int nextLine, int& lastLine);
  void emitIf(const IfStmt& statement, int level, int nextLine, int& lastLine);
  void emitStruct(const StructDecl& decl, int level, int nextLine, int& lastLine);
  void emitEnum(const EnumDecl& decl, int level, int nextLine, int& lastLine);
  void emitTrait(const TraitDecl& decl, int level, int nextLine, int& lastLine);
  void emitImpl(const ImplDecl& decl, int level, int nextLine, int& lastLine);
  std::string inlineStatementText(const Stmt& statement, int level);

  // -------------------------------------------------------- expressions

  std::string exprText(const Expr& expr, int parentPrecedence, int indent);
  static std::string postfixWrap(const std::string& text, int parent, int precedence);
  std::string unaryText(const UnaryExpr& unary, int parent, int indent);
  std::string binaryText(const BinaryExpr& binary, int parent, int indent);
  std::string callText(const CallExpr& call, int parent, int indent);
  std::string enclosedText(const std::string& open, const std::string& close,
                           const std::vector<std::string>& parts, int parent, int indent);
  std::string arrayText(const ArrayLitExpr& array, int parent, int indent);
  std::string mapText(const MapLitExpr& map, int parent, int indent);
  std::string setText(const SetLitExpr& set, int parent, int indent);
  std::string tupleText(const TupleExpr& tuple, int parent, int indent);
  std::string rangeText(const RangeExpr& range, int parent, int indent);
  std::string sliceText(const SliceExpr& slice, int parent, int indent);
  std::string lambdaText(const LambdaExpr& lambda, int parent, int indent);
  std::string patternText(const Pattern& pattern, int indent);
  std::string matchText(const MatchExpr& match, int parent, int indent);
  static std::string joinList(const std::string& prefix,
                              const std::vector<std::string>& items,
                              const std::string& suffix);
  void reindentContinuation(std::string& text, int level) const;

  SourceText source_;
  std::vector<Comment> comments_;
  FormatOptions options_;
  std::size_t cursor_ = 0;
  std::string output_;
};

}  // namespace a::tooling::detail
