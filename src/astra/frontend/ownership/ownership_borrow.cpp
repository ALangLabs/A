#include "ownership/ownership_internal.hpp"

#include <utility>

namespace a::detail {

// ------------------------------------------------------------ borrows

// Registers `borrower` as a shared or mutable borrower of the variable named
// by a `borrow(...)` / `borrow_mut(...)` initializer, rejecting borrows that
// would alias a conflicting live borrow.
void Analyzer::analyzeBorrowInit(const std::string& borrower, const Expr& init) {
    const auto& call = static_cast<const CallExpr&>(init);
    const auto& callee = static_cast<const IdentExpr&>(*call.callee);
    const BorrowKind kind =
        callee.name == "borrow_mut" ? BorrowKind::Mutable : BorrowKind::Shared;
    const auto& target = static_cast<const IdentExpr&>(*call.args[0].value);

    OwnershipState* state = findState(target.name);
    if (state && state->moved) {
      sink_.error(target.span, "Use of moved value '" + target.name + "'.",
                  "The value was already moved out of this variable.");
      return;
    }

    const std::string lender = lenderChain(target.name);
    if (kind == BorrowKind::Mutable) {
      if (const std::vector<std::string>* views = liveViews(lender)) {
        if (!views->empty()) {
          sink_.error(init.span,
                      "Cannot mutably borrow '" + target.name + "' while '" + views->front() +
                          "' borrows it.",
                      "Release every other borrow before taking a mutable one.");
          return;
        }
      }
      auto existing = mutableBorrows_.find(lender);
      if (existing != mutableBorrows_.end() && !existing->second.empty()) {
        sink_.error(init.span,
                    "Cannot mutably borrow '" + target.name + "' while '" +
                        existing->second.front() + "' holds a mutable borrow.",
                    "Only one mutable borrow may be live at a time.");
        return;
      }
    } else {
      auto existing = mutableBorrows_.find(lender);
      if (existing != mutableBorrows_.end() && !existing->second.empty()) {
        sink_.error(init.span,
                    "Cannot borrow '" + target.name + "' while '" + existing->second.front() +
                        "' holds a mutable borrow.",
                    "Release the mutable borrow before creating a shared one.");
        return;
      }
    }

    addBorrow(borrower, lender, kind);
    scopes_.back().views.push_back(borrower);
    scopes_.back().lenders.push_back(lender);
  }

// ------------------------------------------------------------- views

bool Analyzer::isViewExpression(const Expr& expr) const {
    if (expr.kind != ExprKind::Slice) return false;
    const auto& slice = static_cast<const SliceExpr&>(expr);
    if (!slice.step) return true;  // unit step
    if (slice.step->kind != ExprKind::IntLit) return false;
    return static_cast<const IntLitExpr&>(*slice.step).value == 1;
  }

void Analyzer::detectView(const std::string& view, const Expr& init) {
    if (!isViewExpression(init)) return;
    const auto& slice = static_cast<const SliceExpr&>(init);
    // Track the binding at the root of the sliced path (`box.items[0..3]`
    // borrows `box`, `xs[i][0..3]` borrows `xs`) so that mutation anywhere
    // under the root is rejected while the view is live.
    const std::string root = rootIdentName(*slice.object);
    if (root.empty()) return;
    const std::string lender = lenderChain(root);
    addBorrow(view, lender);
    scopes_.back().views.push_back(view);
    scopes_.back().lenders.push_back(lender);
  }

}  // namespace a::detail
