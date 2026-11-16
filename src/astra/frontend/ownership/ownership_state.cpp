#include "ownership/ownership_internal.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace a::detail {

void Analyzer::reset() {
    scopes_.clear();
    lenderOf_.clear();
    borrows_.clear();
    mutableBorrows_.clear();
    borrowKind_.clear();
  }

// -------------------------------------------------------------- state

OwnershipState* Analyzer::findState(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->vars.find(name);
      if (found != it->vars.end()) return &found->second;
    }
    return nullptr;
  }

void Analyzer::declare(const std::string& name) {
    releaseBorrow(name);
    scopes_.back().vars[name] = OwnershipState{};
  }

void Analyzer::rebind(const std::string& name) {
    releaseBorrow(name);
    if (OwnershipState* state = findState(name)) {
      *state = OwnershipState{};
      return;
    }
    declare(name);
  }

void Analyzer::removeFromList(StringMap<std::vector<std::string>>& lists,
                             const std::string& lender, const std::string& view) {
    auto list = lists.find(lender);
    if (list == lists.end()) return;
    auto& views = list->second;
    views.erase(std::remove(views.begin(), views.end(), view), views.end());
    if (views.empty()) lists.erase(list);
  }

void Analyzer::releaseBorrow(const std::string& view) {
    auto lender = lenderOf_.find(view);
    if (lender == lenderOf_.end()) return;
    removeFromList(borrows_, lender->second, view);
    removeFromList(mutableBorrows_, lender->second, view);
    borrowKind_.erase(view);
    lenderOf_.erase(lender);
  }

void Analyzer::addBorrow(const std::string& view, const std::string& lender,
                 BorrowKind kind) {
    releaseBorrow(view);
    lenderOf_[view] = lender;
    borrowKind_[view] = kind;
    if (kind == BorrowKind::Mutable) {
      mutableBorrows_[lender].push_back(view);
    } else {
      borrows_[lender].push_back(view);
    }
  }

std::string Analyzer::lenderChain(const std::string& name) const {
    std::string current = name;
    for (int guard = 0; guard < 64; ++guard) {
      auto found = lenderOf_.find(current);
      if (found == lenderOf_.end()) break;
      current = found->second;
    }
    return current;
  }

const std::vector<std::string>* Analyzer::liveViews(const std::string& name) const {
    auto found = borrows_.find(name);
    return found == borrows_.end() ? nullptr : &found->second;
  }

void Analyzer::useVariable(const IdentExpr& ident) {
    OwnershipState* state = findState(ident.name);
    if (state && state->moved) {
      sink_.error(ident.span, "Use of moved value '" + ident.name + "'.",
                  "Assign a new value to '" + ident.name +
                      "' before using it, or remove the earlier move().");
      return;
    }
    // A mutable borrow gives its holder exclusive access: nobody else may
    // read or write the lender while it is live.
    const std::string root = lenderChain(ident.name);
    auto found = mutableBorrows_.find(root);
    if (found == mutableBorrows_.end()) return;
    for (const std::string& borrower : found->second) {
      if (borrower == ident.name) continue;
      sink_.error(ident.span,
                  "Cannot use '" + ident.name + "' while '" + borrower +
                      "' holds a mutable borrow.",
                  "Release the mutable borrow first, or read through '" + borrower + "'.");
      return;
    }
  }

// First live borrower of `name` other than `except`, or empty.
std::string Analyzer::firstBorrower(const std::string& name, const std::string& except) const {
    if (const std::vector<std::string>* views = liveViews(name)) {
      for (const std::string& view : *views) {
        if (view != except) return view;
      }
    }
    auto found = mutableBorrows_.find(name);
    if (found != mutableBorrows_.end() && !found->second.empty()) {
      for (const std::string& borrower : found->second) {
        if (borrower != except) return borrower;
      }
    }
    return {};
  }

void Analyzer::requireNotBorrowed(const std::string& name, const std::string& action, Span span,
                          const std::string& except) {
    const std::string borrower = firstBorrower(name, except);
    if (borrower.empty()) return;
    sink_.error(span,
                "Cannot " + action + " '" + name + "' while '" + borrower + "' borrows it.",
                "Let '" + borrower +
                    "' go out of scope (or reassign it) before modifying the value's storage.");
  }

std::string Analyzer::firstSharedBorrower(const std::string& name,
                                          const std::string& except) const {
    if (const std::vector<std::string>* views = liveViews(name)) {
      for (const std::string& view : *views) {
        if (view == except) continue;
        auto kind = borrowKind_.find(view);
        if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) return view;
      }
    }
    return {};
  }

void Analyzer::requireNotSharedBorrowed(const std::string& name, const std::string& action,
                                        Span span, const std::string& except) {
    const std::string borrower = firstSharedBorrower(name, except);
    if (borrower.empty()) return;
    sink_.error(span,
                "Cannot " + action + " '" + name + "' while '" + borrower + "' borrows it.",
                "Let '" + borrower +
                    "' go out of scope (or reassign it) before modifying the value's storage.");
  }

}  // namespace a::detail
