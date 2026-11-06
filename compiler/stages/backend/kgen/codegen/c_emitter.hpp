#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

enum class VT : char { Int = 0, Float = 1, Unknown = 2 };
namespace a {

class CEmitter {
public:
  CEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);
  bool emit();
private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  bool inMain_ = false;
  int tmpTop_ = 0;
  std::set<std::string> globalNames_;
  std::unordered_map<std::string, VT> globalType_;
  std::set<std::string> locals_;
  std::unordered_map<std::string, VT> slotType_;
  std::vector<std::string> stringLits_;
  std::unordered_map<std::string, int> stringLitIdx_;
  std::vector<const LambdaExpr*> lambdas_;
  std::unordered_map<const LambdaExpr*, int> lambdaIdx_;
  std::vector<const Stmt*> deferStack_;
  void emitDefers();
  static std::string cIdent(const std::string& name);
  void collectGlobals();
// Static type of an expression: Int, Float, or Unknown (runtime tag decides).
  VT exprType(const Expr& e);
// Pre-type globals so functions (emitted above main) know whether a global
// holds a double. Walks top-level statements in order.
  void pretypeGlobals();
  void collectLocalNames(const std::vector<StmtPtr>& body);
  void collectExprStrings(const Expr& e);
  void collectStmtStrings(const Stmt& s);
  void collectStrings();
// ---- lambda collection (each becomes a real C function) ----
  void collectExprLambdas(const Expr& e);
  void collectStmtLambdas(const Stmt& s);
  void collectLambdas();
// Emits each collected lambda as a standalone C function afn_l<N>.
  void emitLambdaFunctions();
  std::string lvalue(const std::string& rawName);
  std::string identA(const std::string& rawName);
  bool isGlobalLV(const std::string& lv);
  std::string floatLit(double v);
  std::string escapeC(const std::string& s);
// Fresh temp name; the aval _tN slots are declared at each function top.
  std::string tmp();
// aval result of `l <op> r` where l/r are aval expressions and vt is the
// statically-known result requirement (Int/Float emit direct ops; Unknown
// emits a runtime tag dispatch into a temp).
  std::string avalBin(const std::string& op, const std::string& l,
                      const std::string& r, VT want);
  std::string evalA(const Expr& e);
  std::string evalI(const Expr& e);
  std::string evalD(const Expr& e);
  void emitPrint(const Expr& arg);
// One printed object into `stream` (no separator/newline).
  void emitPrintValue(const std::string& stream, const Expr& arg);
// Full print/eprint call with *objects plus sep/end/file/flush keywords.
  void emitPrintCall(const CallExpr& c, bool toStderr);
  void emitFunction(const FnDecl& fn);
  void setType(const std::string& lv, const std::string& name, VT vt);
  void emitStmt(const Stmt& stmt);
  void emitMain();
};

}  // namespace a
