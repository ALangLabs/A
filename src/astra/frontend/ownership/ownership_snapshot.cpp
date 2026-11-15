#include "ownership/ownership_internal.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace a::detail {

Analyzer::Snapshot Analyzer::snapshot() const {
    return Snapshot{scopes_, lenderOf_, borrows_, mutableBorrows_, borrowKind_};
  }

void Analyzer::restore(const Analyzer::Snapshot& state) {
    scopes_ = state.scopes;
    lenderOf_ = state.lenderOf;
    borrows_ = state.borrows;
    mutableBorrows_ = state.mutableBorrows;
    borrowKind_ = state.borrowKind;
  }

// Union the borrow bookkeeping from `source` into the live maps.
void Analyzer::unionBorrows(const StringMap<std::vector<std::string>>& shared,
                    const StringMap<std::vector<std::string>>& mut) {
    auto add = [&](const StringMap<std::vector<std::string>>& lists, bool mutableBorrow) {
      for (const auto& entry : lists) {
        auto& target = mutableBorrow ? mutableBorrows_[entry.first] : borrows_[entry.first];
        for (const std::string& borrower : entry.second) {
          if (std::find(target.begin(), target.end(), borrower) == target.end()) {
            target.push_back(borrower);
          }
        }
      }
    };
    add(shared, false);
    add(mut, true);
  }

// Merge the branch/body state into the current state: a value moved on any
// path counts as moved afterwards, and borrows are unioned.
void Analyzer::mergeState(const Analyzer::Snapshot& other) {
    for (const auto& scope : other.scopes) {
      for (const auto& entry : scope.vars) {
        if (!entry.second.moved) continue;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
          auto found = it->vars.find(entry.first);
          if (found == it->vars.end()) continue;
          found->second.moved = true;
          found->second.movedAt = entry.second.movedAt;
          break;
        }
      }
    }
    unionBorrows(other.borrows, other.mutableBorrows);
    for (const auto& entry : other.lenderOf) lenderOf_[entry.first] = entry.second;
    for (const auto& entry : other.borrowKind) borrowKind_[entry.first] = entry.second;
  }

void Analyzer::mergeWith(const Analyzer::Snapshot& before) {
    // Keep the current (post-body) state, then pull in anything moved before.
    mergeState(before);
  }

void Analyzer::restoreBorrowsPreservingMoves(const Analyzer::Snapshot& before) {
    // Conditions run against the pre-loop state; views created earlier are
    // still live, so restore their bookkeeping while keeping moved flags.
    StringMap<std::vector<std::string>> mergedBorrows = borrows_;
    StringMap<std::vector<std::string>> mergedMut = mutableBorrows_;
    StringMap<std::string> mergedLenderOf = lenderOf_;
    StringMap<BorrowKind> mergedKind = borrowKind_;
    borrows_ = before.borrows;
    mutableBorrows_ = before.mutableBorrows;
    lenderOf_ = before.lenderOf;
    borrowKind_ = before.borrowKind;
    unionBorrows(mergedBorrows, mergedMut);
    for (const auto& entry : mergedLenderOf) lenderOf_[entry.first] = entry.second;
    for (const auto& entry : mergedKind) borrowKind_[entry.first] = entry.second;
  }

}  // namespace a::detail
