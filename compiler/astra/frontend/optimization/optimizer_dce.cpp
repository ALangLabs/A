#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
OptimizationStats deadCodeElimination(Program& program) {
  OptimizationStats stats;

  // Remove blocks that are not reachable from the entry block.
  for (Function& function : program.functions) {
    if (function.blocks.empty()) continue;
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<int> worklist{0};
    reachable[0] = true;
    while (!worklist.empty()) {
      int index = worklist.back();
      worklist.pop_back();
      const Terminator& term = function.blocks[static_cast<std::size_t>(index)].term;
      auto visit = [&](int target) {
        if (target < 0 || static_cast<std::size_t>(target) >= function.blocks.size()) return;
        if (reachable[static_cast<std::size_t>(target)]) return;
        reachable[static_cast<std::size_t>(target)] = true;
        worklist.push_back(target);
      };
      if (term.kind == Terminator::Kind::Jump) visit(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        visit(term.target);
        visit(term.falseTarget);
      }
    }
    bool removedBlock = false;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      if (!reachable[b]) {
        removedBlock = true;
        ++stats.deadBlocks;
      }
    }
    if (removedBlock) {
      std::vector<int> remap(function.blocks.size(), -1);
      std::vector<Block> kept;
      kept.reserve(function.blocks.size());
      for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        remap[b] = static_cast<int>(kept.size());
        kept.push_back(std::move(function.blocks[b]));
      }
      function.blocks = std::move(kept);
      for (Block& block : function.blocks) {
        if (block.term.target >= 0) block.term.target = remap[block.term.target];
        if (block.term.falseTarget >= 0) block.term.falseTarget = remap[block.term.falseTarget];
        for (Inst& inst : block.insts) {
          if (inst.op != Op::Phi) continue;
          std::vector<int> operands;
          std::vector<int> incoming;
          for (std::size_t i = 0; i < inst.operands.size(); ++i) {
            int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
            if (from < 0) continue;
            int mapped = remap[from];
            if (mapped < 0) continue;
            operands.push_back(inst.operands[i]);
            incoming.push_back(mapped);
          }
          inst.operands = std::move(operands);
          inst.incomingBlocks = std::move(incoming);
        }
      }
    }

    // Value-level DCE: mark everything reachable from effectful instructions,
    // terminators, and other roots.
    std::unordered_map<int, Inst*> defs;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) defs[inst.id] = &inst;
    }
    std::unordered_set<int> live;
    std::vector<int> liveWorklist;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (isEffectfulOp(inst.op) && inst.producesValue()) {
          live.insert(inst.id);
          liveWorklist.push_back(inst.id);
        }
      }
      if (block.term.value >= 0 && live.insert(block.term.value).second) {
        liveWorklist.push_back(block.term.value);
      }
    }
    while (!liveWorklist.empty()) {
      int value = liveWorklist.back();
      liveWorklist.pop_back();
      auto found = defs.find(value);
      if (found == defs.end()) continue;
      for (int operand : found->second->operands) {
        if (operand >= 0 && live.insert(operand).second) liveWorklist.push_back(operand);
      }
    }
    for (Block& block : function.blocks) {
      std::vector<Inst> keptInsts;
      keptInsts.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        if (inst.producesValue() && live.count(inst.id) == 0) {
          ++stats.deadInstructions;
          continue;
        }
        keptInsts.push_back(std::move(inst));
      }
      block.insts = std::move(keptInsts);
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
