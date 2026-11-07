#pragma once

#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "collections/transparent_map.hpp"
#include "ownership/ownership.hpp"

namespace a::detail {

// Methods that can change an array's storage. Element writes keep the array's
// length, so they are allowed while a view exists; these are not.
inline bool isStructuralMutator(const std::string& method) {
  static const char* names[] = {"append", "extend", "insert", "pop",  "remove",
                                "clear",  "remove_at"};
  for (const char* name : names) {
    if (method == name) return true;
  }
  return false;
}

struct OwnershipState {
  bool moved = false;
  Span movedAt;
};

enum class BorrowKind { View, Shared, Mutable };

// True when `expr` is a well-formed `borrow(x)` / `borrow_mut(x)` call whose
// result can be tied to the binding being initialized.
inline bool isBorrowExpression(const Expr& expr) {
  if (expr.kind != ExprKind::Call) return false;
  const auto& call = static_cast<const CallExpr&>(expr);
  if (call.callee->kind != ExprKind::Ident) return false;
  const auto& callee = static_cast<const IdentExpr&>(*call.callee);
  if (callee.name != "borrow" && callee.name != "borrow_mut") return false;
  if (call.args.size() != 1) return false;
  return call.args[0].value->kind == ExprKind::Ident;
}

// The variable at the root of a member/index chain, e.g. `a` for `a[i].name`.
inline std::string rootIdentName(const Expr& expr) {
  const Expr* current = &expr;
  while (current) {
    switch (current->kind) {
      case ExprKind::Ident:
        return static_cast<const IdentExpr&>(*current).name;
      case ExprKind::Field:
        current = static_cast<const FieldExpr&>(*current).object.get();
        break;
      case ExprKind::Index:
        current = static_cast<const IndexExpr&>(*current).object.get();
        break;
      case ExprKind::Slice:
        current = static_cast<const SliceExpr&>(*current).object.get();
        break;
      case ExprKind::SafeNav:
        current = static_cast<const SafeNavExpr&>(*current).object.get();
        break;
      default:
        return {};
    }
  }
  return {};
}

struct Scope {
  StringMap<OwnershipState> vars;
  std::vector<std::string> views;    // views declared in this scope
  std::vector<std::string> lenders;  // their lenders (parallel to `views`)
};

// Flow-sensitive ownership analysis: tracks moved values and shared/mutable
// borrows across scopes, branches, and loops. Internal to the ownership
// library; only analyzeOwnership() is part of the public API.
class Analyzer {
 public:
  explicit Analyzer(DiagnosticSink& sink) : sink_(sink) {}

  void analyzeModule(const Module& module);
  void analyzeFunction(const FnDecl& decl);

 private:
  void reset();

  // -------------------------------------------------------------- state

  OwnershipState* findState(const std::string& name);
  void declare(const std::string& name);
  void rebind(const std::string& name);
  static void removeFromList(StringMap<std::vector<std::string>>& lists,
                             const std::string& lender, const std::string& view);
  void releaseBorrow(const std::string& view);
  void addBorrow(const std::string& view, const std::string& lender,
                 BorrowKind kind = BorrowKind::View);
  std::string lenderChain(const std::string& name) const;
  const std::vector<std::string>* liveViews(const std::string& name) const;
  void useVariable(const IdentExpr& ident);

  // First live borrower of `name` other than `except`, or empty.
  std::string firstBorrower(const std::string& name,
                            const std::string& except = {}) const;
  void requireNotBorrowed(const std::string& name, const std::string& action,
                          Span span, const std::string& except = {});

  // --------------------------------------------------------- statements

  void analyzeStatements(const std::vector<StmtPtr>& statements);
  void popScope();
  void analyzeStmt(const Stmt& stmt);
  void analyzeAssign(const AssignStmt& assign);
  void analyzeIf(const IfStmt& stmt);

  // Analyze a loop body once. The state after the loop conservatively includes
  // everything the body may have moved or borrowed.
  void analyzeLoop(const Expr* condition, const std::vector<StmtPtr>& body);

  // --------------------------------------------------------- expressions

  void useExpr(const Expr& expr);
  void analyzeComplexExpr(const Expr& expr);
  void analyzeMove(const CallExpr& call);

  // ------------------------------------------------------------ borrows

  // Registers `borrower` as a shared or mutable borrower of the variable named
  // by a `borrow(...)` / `borrow_mut(...)` initializer, rejecting borrows that
  // would alias a conflicting live borrow.
  void analyzeBorrowInit(const std::string& borrower, const Expr& init);

  // ------------------------------------------------------------- views

  bool isViewExpression(const Expr& expr) const;
  void detectView(const std::string& view, const Expr& init);

  // --------------------------------------------------------- snapshots

  struct Snapshot {
    std::vector<Scope> scopes;
    StringMap<std::string> lenderOf;
    StringMap<std::vector<std::string>> borrows;
    StringMap<std::vector<std::string>> mutableBorrows;
    StringMap<BorrowKind> borrowKind;
  };

  Snapshot snapshot() const;
  void restore(const Snapshot& state);

  // Union the borrow bookkeeping from `source` into the live maps.
  void unionBorrows(const StringMap<std::vector<std::string>>& shared,
                    const StringMap<std::vector<std::string>>& mut);

  // Merge the branch/body state into the current state: a value moved on any
  // path counts as moved afterwards, and borrows are unioned.
  void mergeState(const Snapshot& other);
  void mergeWith(const Snapshot& before);
  void restoreBorrowsPreservingMoves(const Snapshot& before);

  DiagnosticSink& sink_;
  std::vector<Scope> scopes_;
  StringMap<std::string> lenderOf_;                     // borrower/view -> original lender
  StringMap<std::vector<std::string>> borrows_;         // lender -> live shared borrowers
  StringMap<std::vector<std::string>> mutableBorrows_;  // lender -> live mutable borrowers
  StringMap<BorrowKind> borrowKind_;                    // borrower -> shared/mutable
};

}  // namespace a::detail
