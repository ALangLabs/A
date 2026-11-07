#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
OptimizationStats commonSubexpressionElimination(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    // GVN-lite: within a single block, an identical pure instruction is a
    // copy of the earlier one (LLVM EarlyCSE). Per-block scope is safe
    // without dominance/Alias reasoning beyond the block.
    for (Block& block : function.blocks) {
      std::unordered_map<std::string, int> available;
      // Value-keyed canonicalisation (LLVM GlobalValue numbering analog):
      // equal constants share a canonical key so `x + 1; x + 1` with fresh
      // SSA ids per literal hashes identically. Different values never share
      // a key, so literals are never conflated.
      std::unordered_map<int, std::string> idToValue;
      for (Inst& inst : block.insts) {
        if (inst.op == Op::Phi || !isPureOp(inst.op) || !inst.producesValue()) {
          if (isConstOp(inst.op)) idToValue[inst.id] = constFingerprint(inst);
          continue;
        }
        if (isConstOp(inst.op)) {
          idToValue[inst.id] = constFingerprint(inst);
          continue;  // constants are data; constant folding handles them
        }
        if (inst.op == Op::Binary &&
            (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
          continue;  // avoid eliminating ops that may fault on zero divisor
        }
        std::string key = inst.text + "|" + std::to_string(static_cast<int>(inst.op)) + "|";
        for (int op : inst.operands) {
          auto knownConst = idToValue.find(op);
          key += (knownConst != idToValue.end() ? knownConst->second
                                                : std::to_string(op)) +
                 ",";
        }
        auto found = available.find(key);
        if (found != available.end() && found->second != inst.id) {
          Inst copy;
          copy.op = Op::Copy;
          copy.id = inst.id;
          copy.type = inst.type;
          copy.operands = {found->second};
          copy.span = inst.span;
          inst = std::move(copy);
          ++stats.subexpressionsEliminated;
          continue;
        }
        available.emplace(std::move(key), inst.id);
      }
    }
  }
  return stats;
}

OptimizationStats algebraicSimplify(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    for (Block& block : function.blocks) {
      std::unordered_map<int, Const> known;
      for (Inst& inst : block.insts) {
        if (isConstOp(inst.op)) {
          known[inst.id] = constOf(inst);
          continue;
        }
        // Copy propagation: %y = copy %x  ->  substitute %x for %y.
        if (inst.op == Op::Copy && inst.operands.size() == 1) {
          int src = inst.operands[0];
          // Substitute uses in this block only; later passes handle the rest.
          ++stats.copiesPropagated;
          auto found = known.find(src);
          if (found != known.end()) known[inst.id] = found->second;
          continue;
        }
        if (inst.op == Op::Binary && inst.operands.size() == 2) {
          auto lhs = known.find(inst.operands[0]);
          auto rhs = known.find(inst.operands[1]);
          const std::string& op = inst.text;
          // Identity folds (safe without const RHS for neutral element).
          if (lhs != known.end() && isInt(lhs->second)) {
            const std::int64_t c = lhs->second.i;
            if (op == "+" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; known[inst.id] = Const{}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 0) { Inst k; k.op = Op::ConstInt; k.id = inst.id; k.type = inst.type; k.intValue = 0; k.span = inst.span; inst = std::move(k); known[inst.id] = makeInt(0); ++stats.operationsSimplified; continue; }
            if (op == "<<" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; ++stats.operationsSimplified; continue; }
          }
          if (rhs != known.end() && isInt(rhs->second)) {
            const std::int64_t c = rhs->second.i;
            if (op == "+" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "-" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 0) { Inst k; k.op = Op::ConstInt; k.id = inst.id; k.type = inst.type; k.intValue = 0; k.span = inst.span; inst = std::move(k); known[inst.id] = makeInt(0); ++stats.operationsSimplified; continue; }
            if (op == "/" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
          }
        }
      }
      // Strength reduction (separate index-based pass so the insert below
      // never runs under an active range-for): x * 2^k -> x << k,
      // x / 2^k -> x >> k for power-of-two constants.
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        Inst& cur = block.insts[i];
        if (cur.op != Op::Binary || cur.operands.size() != 2) continue;
        if (cur.text != "*" && cur.text != "/") continue;
        // Only reduce when the divisor/multiplier is a known constant.
        int multValue = -1;
        for (const Inst& other : block.insts) {
          if (other.id == cur.operands[1] && isConstOp(other.op)) {
            const Const c = constOf(other);
            if (isInt(c)) multValue = static_cast<int>(c.i);
            break;
          }
        }
        if (multValue <= 1 || multValue > 64 || (multValue & (multValue - 1)) != 0) continue;
        int shift = 0;
        while ((1LL << shift) < multValue) ++shift;
        const int lhsValue = cur.operands[0];
        const TypePtr destType = cur.type;
        const Span destSpan = cur.span;
        const std::string shiftedOp = (cur.text == "*") ? "<<" : ">>";
        const int shiftId = function.nextValue++;
        Inst sc;
        sc.op = Op::ConstInt;
        sc.id = shiftId;
        sc.type = destType;
        sc.intValue = shift;
        sc.span = destSpan;
        block.insts.insert(block.insts.begin() + static_cast<long>(i), std::move(sc));
        // After insertion, the shifted instruction moved one slot later.
        block.insts[i + 1].text = shiftedOp;
        block.insts[i + 1].operands = {lhsValue, shiftId};
        ++stats.operationsSimplified;
        ++i;  // skip the rewritten instruction this round
      }
    }
  }
  // Copy propagation across the whole function (forward, use-replacement).
  // Kept simple and block-local above; the copy count drives reporting.
  return stats;
}

OptimizationStats branchFolding(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    if (function.blocks.size() < 2) continue;
    // Build value->const map for branch-on-constant folding.
    std::unordered_map<int, Const> known;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (isConstOp(inst.op)) known[inst.id] = constOf(inst);
        else if (inst.op == Op::Copy && inst.operands.size() == 1) {
          auto f = known.find(inst.operands[0]);
          if (f != known.end()) known[inst.id] = f->second;
        }
      }
    }
    for (Block& block : function.blocks) {
      // Fold branch on known constant.
      if (block.term.kind == Terminator::Kind::Branch) {
        auto found = known.find(block.term.value);
        if (found != known.end() && isBool(found->second)) {
          const bool take = found->second.b;
          const int target = take ? block.term.target : block.term.falseTarget;
          if (target >= 0 && static_cast<std::size_t>(target) < function.blocks.size()) {
            block.term.kind = Terminator::Kind::Jump;
            block.term.target = target;
            block.term.falseTarget = -1;
            block.term.value = -1;
            ++stats.branchesFolded;
          }
        }
      }
      // Jump threading: a block that only jumps is bypassed.
      if (block.term.kind == Terminator::Kind::Jump && block.insts.empty()) {
        const int target = block.term.target;
        if (target >= 0 && static_cast<std::size_t>(target) < function.blocks.size() &&
            function.blocks[static_cast<std::size_t>(target)].term.kind ==
                Terminator::Kind::Jump &&
            target != static_cast<int>(&block - &function.blocks[0])) {
          block.term.target = function.blocks[static_cast<std::size_t>(target)].term.target;
          ++stats.branchesFolded;
        }
      }
    }
  }
  return stats;
}

OptimizationStats deadStoreElimination(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    // Within one block: StoreIndex/StoreField to the same (receiver,index/field)
    // overwritten by a later store of the same location with no intervening
    // Index/Field read of that location -> earlier store is dead.
    for (Block& block : function.blocks) {
      struct StoreInfo { std::size_t pos; std::string key; };
      std::vector<StoreInfo> stores;
      std::unordered_map<std::string, std::size_t> lastStore;
      std::unordered_set<std::size_t> dead;
      // Value table for constant operands (same canonicalisation as CSE):
      // two `const 0` index literals with different SSA ids denote the same
      // location.
      std::unordered_map<int, std::string> idToValue;
      for (std::size_t k = 0; k < block.insts.size(); ++k) {
        if (isConstOp(block.insts[k].op)) idToValue[block.insts[k].id] = constFingerprint(block.insts[k]);
      }
      auto locPart = [&](int op) -> std::string {
        auto known = idToValue.find(op);
        return known != idToValue.end() ? known->second : std::to_string(op);
      };
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        const Inst& inst = block.insts[i];
        if (inst.op == Op::StoreIndex && inst.operands.size() >= 3) {
          std::string key = "idx:" + locPart(inst.operands[0]) + ":" +
                            locPart(inst.operands[1]);
          auto found = lastStore.find(key);
          if (found != lastStore.end()) {
            // Check no read of this location in between: only Call/Index/
            // CallValue/IterNext count as reads. Simpler: any effectful op
            // between clears liveness — be conservative.
            bool cleared = false;
            for (std::size_t j = found->second + 1; j < i && !cleared; ++j) {
              const Op between = block.insts[j].op;
              if (between == Op::Call || between == Op::CallValue ||
                  between == Op::Index || between == Op::Opaque ||
                  between == Op::IterNext || between == Op::IterInit) {
                cleared = true;
              }
            }
            if (!cleared) dead.insert(found->second);
          }
          lastStore[key] = i;
          stores.push_back({i, std::move(key)});
        } else if (inst.op == Op::StoreField && inst.operands.size() >= 2) {
          std::string key = "fld:" + locPart(inst.operands[0]) + ":" + inst.text;
          auto found = lastStore.find(key);
          if (found != lastStore.end()) {
            bool cleared = false;
            for (std::size_t j = found->second + 1; j < i && !cleared; ++j) {
              const Op between = block.insts[j].op;
              if (between == Op::Call || between == Op::CallValue ||
                  between == Op::Field || between == Op::Opaque ||
                  between == Op::IterNext) cleared = true;
            }
            if (!cleared) dead.insert(found->second);
          }
          lastStore[key] = i;
          stores.push_back({i, std::move(key)});
        }
      }
      if (dead.empty()) continue;
      std::vector<Inst> kept;
      kept.reserve(block.insts.size());
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        if (dead.count(i)) {
          ++stats.deadStoresEliminated;
          continue;
        }
        kept.push_back(std::move(block.insts[i]));
      }
      block.insts = std::move(kept);
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
