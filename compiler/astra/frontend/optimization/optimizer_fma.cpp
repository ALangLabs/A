#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
namespace {

bool isFmaFloat(const TypePtr& type) {
  TypePtr resolved = type ? resolveType(type) : nullptr;
  return resolved && resolved->kind == TypeKind::Float;
}

}  // namespace

// LLVM-style floating-point contraction: `a*b + c` -> `fma(a, b, c)`.
// Only single-use products in the same basic block are fused, and only for
// floating-point results, so integer wrapping semantics never change. The
// now-dead multiply is left for deadCodeElimination to remove.
OptimizationStats fuseFma(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    // Use counts over the whole function (operands + terminator values).
    std::unordered_map<int, int> uses;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts)
        for (int operand : inst.operands)
          if (operand >= 0) ++uses[operand];
      if (block.term.value >= 0) ++uses[block.term.value];
    }
    // Def site lookup: value id -> index within its block.
    std::unordered_map<int, std::pair<int, std::size_t>> defSite;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      Block& block = function.blocks[b];
      for (std::size_t i = 0; i < block.insts.size(); ++i)
        if (block.insts[i].producesValue())
          defSite[block.insts[i].id] = {static_cast<int>(b), i};
    }

    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      Block& block = function.blocks[b];
      const int blockIndex = static_cast<int>(b);
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        Inst& inst = block.insts[i];
        if (inst.op != Op::Binary || inst.text != "+" || inst.operands.size() != 2)
          continue;
        if (!isFmaFloat(inst.type)) continue;
        // Try either operand as the product: (a*b) + c, or c + (a*b).
        for (int side = 0; side < 2; ++side) {
          const int mulId = inst.operands[side];
          const int other = inst.operands[1 - side];
          auto found = defSite.find(mulId);
          if (found == defSite.end()) continue;
          const auto [defBlock, defIndex] = found->second;
          if (defBlock < 0 || static_cast<std::size_t>(defBlock) >= function.blocks.size())
            continue;
          // Require the product to be defined earlier in this same block
          // (so the fused instruction dominates its operands).
          if (defBlock != blockIndex) continue;
          if (defIndex >= i) continue;
          const Inst& mul = function.blocks[defBlock].insts[defIndex];
          if (mul.op != Op::Binary || mul.text != "*" || mul.operands.size() != 2)
            continue;
          if (!isFmaFloat(mul.type)) continue;
          auto used = uses.find(mulId);
          if (used == uses.end() || used->second != 1) continue;  // single-use only
          inst.op = Op::Fma;
          inst.text.clear();
          inst.operands = {mul.operands[0], mul.operands[1], other};
          ++stats.fmaFused;
          break;
        }
      }
    }
  }
  return stats;
}

}  // namespace ir
}  // namespace a
