#pragma once

#include "ir/ir.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"

namespace a::ir {

TypePtr intType();
bool lookupEnv(const StringMap<int>& env, const std::string& name, int& out);
void collectAssigned(const std::vector<StmtPtr>& stmts, std::set<std::string>& names);
void collectAssignedStmt(const Stmt& stmt, std::set<std::string>& names);
void collectAssignedExpr(const Expr& expr, std::set<std::string>& names);
std::string castKindFor(const TypePtr& srcIn, const TypePtr& dstIn);
bool isConversionName(const std::string& name);

namespace detail {

// Lowers Sema-checked AST into core IR functions/blocks. Internal to the IR
// library; only lowerProgram() is part of the public API.
class Builder {
 public:
  explicit Builder(Function& function) : fn_(function) {}

  void lowerParameters();
  void lowerBody(const std::vector<StmtPtr>& body);
  void lowerTopLevel(const std::vector<StmtPtr>& statements);

 private:
  struct LoopContext {
    int header = -1;
    int exit = -1;
    int continueTarget = -1;
    std::vector<std::pair<std::string, int>> headerPhis;  // empty name = synthetic
  };
  struct Edge {
    int from = -1;
    StringMap<int> env;
  };

  void finish();
  int addBlock(std::string label);
  std::string uniqueLabel(const std::string& base);
  Block& block(int index);
  Inst& instruction(int id);
  int emit(Inst inst);
  int emitConstInt(std::int64_t value, TypePtr type = intType());
  int emitConst(const Expr& expr);
  int emitBinary(const std::string& op, int lhs, int rhs, TypePtr type, Span span);
  TypePtr typeOf(int value) const;
  int lookup(const std::string& name) const;
  bool assignVar(const std::string& name, int value);

  StringMap<int> snapshotEnv() const;
  void applyEnv(const StringMap<int>& env);
  void lowerStatements(const std::vector<StmtPtr>& body);
  void lowerStatementsRaw(const std::vector<StmtPtr>& body);

  void recordEdge(int target, const StringMap<int>& env, int extra = -1);
  void jumpTo(int target, const StringMap<int>& env, int extra = -1);
  void branchTo(int condition, int trueTarget, int falseTarget,
                const StringMap<int>& env);
  int pushPhi(int blockIndex, TypePtr type, int value, int from);
  std::size_t materializeJoin(int blockIndex);

  void lowerStmt(const Stmt& stmt);
  void lowerLet(const LetStmt& let);
  void lowerAssign(const AssignStmt& assign);
  void storeInto(const Expr& target, int value, Span span);
  void lowerDestructure(const DestructureStmt& stmt);
  void lowerIf(const IfStmt& stmt);
  StringMap<int> createHeaderPhis(int header, int preheader,
                                  const std::vector<std::string>& names);
  std::vector<std::string> assignedNames(const std::vector<StmtPtr>& body);
  void lowerWhile(const WhileStmt& stmt);
  void lowerDoWhile(const DoWhileStmt& stmt);
  void lowerLoop(const LoopStmt& stmt);
  void lowerFor(const ForStmt& stmt);
  void lowerForRange(const ForStmt& stmt, const RangeExpr& range);
  TypePtr iterableElement(const TypePtr& type);
  void lowerForIterable(const ForStmt& stmt);

  int lowerExpr(const Expr& expr);
  int lowerString(const StringLitExpr& literal);
  int lowerIdent(const IdentExpr& ident);
  int lowerUnary(const UnaryExpr& unary);
  int lowerBinary(const BinaryExpr& binary);
  int lowerCall(const CallExpr& call);

  Function& fn_;
  std::vector<StringMap<int>> scopes_{{}};
  std::unordered_map<int, std::pair<int, std::size_t>> defs_;
  std::unordered_map<int, TypePtr> types_;
  std::unordered_map<int, std::vector<Edge>> blockIncoming_;
  std::vector<LoopContext> loops_;
  int current_ = 0;
  bool terminated_ = false;
  int labelCounter_ = 0;
};

}  // namespace detail

}  // namespace a::ir
