#include "ownership/ownership_internal.hpp"

#include <utility>
#include <vector>

namespace a::detail {

// --------------------------------------------------------- statements

void Analyzer::analyzeStatements(const std::vector<StmtPtr>& statements) {
    scopes_.emplace_back();
    for (const auto& stmt : statements) analyzeStmt(*stmt);
    popScope();
  }

void Analyzer::popScope() {
    for (std::size_t i = 0; i < scopes_.back().views.size(); ++i) {
      releaseBorrow(scopes_.back().views[i]);
    }
    scopes_.pop_back();
  }

void Analyzer::analyzeStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        const bool borrowInit = let.init && isBorrowExpression(*let.init);
        if (let.init && !borrowInit) useExpr(*let.init);
        releaseBorrow(let.name);
        if (let.init) {
          if (borrowInit) {
            analyzeBorrowInit(let.name, *let.init);
          } else {
            detectView(let.name, *let.init);
          }
        }
        scopes_.back().vars[let.name] = OwnershipState{};
        return;
      }
      case StmtKind::Assign: {
        analyzeAssign(static_cast<const AssignStmt&>(stmt));
        return;
      }
      case StmtKind::Expr: {
        useExpr(*static_cast<const ExprStmt&>(stmt).expr);
        return;
      }
      case StmtKind::Destructure: {
        const auto& destructure = static_cast<const DestructureStmt&>(stmt);
        useExpr(*destructure.value);
        for (const auto& name : destructure.names) {
          if (name != "_") scopes_.back().vars[name] = OwnershipState{};
        }
        return;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) useExpr(*ret.value);
        return;
      }
      case StmtKind::Break:
      case StmtKind::Continue:
        return;
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(stmt);
        Snapshot saved = snapshot();
        if (defer.body) analyzeStmt(*defer.body);
        restore(saved);
        return;
      }
      case StmtKind::If: {
        analyzeIf(static_cast<const IfStmt&>(stmt));
        return;
      }
      case StmtKind::While: {
        analyzeLoop(static_cast<const WhileStmt&>(stmt).cond.get(),
                    static_cast<const WhileStmt&>(stmt).body);
        return;
      }
      case StmtKind::DoWhile: {
        const auto& loop = static_cast<const DoWhileStmt&>(stmt);
        analyzeLoop(nullptr, loop.body);
        if (loop.cond) useExpr(*loop.cond);
        return;
      }
      case StmtKind::For: {
        const auto& loop = static_cast<const ForStmt&>(stmt);
        useExpr(*loop.iterable);
        Snapshot before = snapshot();
        scopes_.emplace_back();
        declare(loop.var);
        for (const auto& inner : loop.body) analyzeStmt(*inner);
        popScope();
        mergeWith(before);
        return;
      }
      case StmtKind::Loop: {
        const auto& loop = static_cast<const LoopStmt&>(stmt);
        analyzeLoop(nullptr, loop.body);
        return;
      }
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
    }
  }

void Analyzer::analyzeAssign(const AssignStmt& assign) {
    if (assign.target->kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(*assign.target);
      if (assign.op == "=") {
        // Reassigning a borrower releases it, so it must not count as a
        // conflicting borrow of its own lender.
        requireNotBorrowed(lenderChain(ident.name), "reassign", assign.opSpan, ident.name);
        const bool borrowInit = isBorrowExpression(*assign.value);
        if (!borrowInit) useExpr(*assign.value);
        rebind(ident.name);
        if (borrowInit) {
          analyzeBorrowInit(ident.name, *assign.value);
        } else if (isViewExpression(*assign.value)) {
          detectView(ident.name, *assign.value);
        }
        return;
      }
      // Compound assignment mutates the value through the binding.
      auto kind = borrowKind_.find(ident.name);
      if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
        sink_.error(assign.opSpan,
                    "Cannot modify '" + ident.name + "' through a shared borrow.",
                    "Use borrow_mut to obtain a mutable borrow.");
      } else {
        // `xs += v` may grow the storage, so it conflicts with every live
        // borrower of the lender (views, shared borrows, and mutable borrows
        // held by someone else), not just shared borrows.
        requireNotBorrowed(lenderChain(ident.name), "modify",
                           assign.opSpan, ident.name);
      }
      useVariable(ident);
      useExpr(*assign.value);
      return;
    }
    const std::string root = rootIdentName(*assign.target);
    if (!root.empty()) {
      auto kind = borrowKind_.find(root);
      if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
        sink_.error(assign.opSpan,
                    "Cannot modify '" + root + "' through a shared borrow.",
                    "Use borrow_mut to obtain a mutable borrow.");
      } else {
        const std::string lender = lenderChain(root);
        if (assign.target->kind == ExprKind::Index) {
          // Element writes keep the storage length, so views allow them;
          // a shared borrow() still makes the whole value read-only.
          requireNotSharedBorrowed(lender, "modify",
                                   assign.target->span, root);
        } else {
          // Field (re)binds and other targets can detach or regrow storage,
          // so they conflict with every live borrower, matching `x = v`.
          requireNotBorrowed(lender, "modify", assign.target->span,
                             root);
        }
      }
    }
    useExpr(*assign.target);
    useExpr(*assign.value);
  }

void Analyzer::analyzeIf(const IfStmt& stmt) {
    Snapshot before = snapshot();
    std::vector<Snapshot> outcomes;
    bool hasElse = !stmt.elseBody.empty();
    for (const auto& branch : stmt.branches) {
      restore(before);
      useExpr(*branch.cond);
      Snapshot condState = snapshot();
      scopes_.emplace_back();
      for (const auto& inner : branch.body) analyzeStmt(*inner);
      popScope();
      outcomes.push_back(snapshot());
      (void)condState;
    }
    if (hasElse) {
      restore(before);
      scopes_.emplace_back();
      for (const auto& inner : stmt.elseBody) analyzeStmt(*inner);
      popScope();
      outcomes.push_back(snapshot());
    } else {
      outcomes.push_back(before);
    }
    restore(outcomes.front());
    for (std::size_t i = 1; i < outcomes.size(); ++i) mergeState(outcomes[i]);
  }

// Analyze a loop body once. The state after the loop conservatively includes
// everything the body may have moved or borrowed.
void Analyzer::analyzeLoop(const Expr* condition, const std::vector<StmtPtr>& body) {
    if (condition) useExpr(*condition);
    Snapshot before = snapshot();
    scopes_.emplace_back();
    for (const auto& inner : body) analyzeStmt(*inner);
    popScope();
    mergeWith(before);
    if (condition) restoreBorrowsPreservingMoves(before);
  }

}  // namespace a::detail
