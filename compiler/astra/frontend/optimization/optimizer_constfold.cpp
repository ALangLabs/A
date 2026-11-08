#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
OptimizationStats constantFold(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    std::unordered_map<int, TypePtr> valTypes;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts) {
        if (inst.producesValue() && inst.type) valTypes[inst.id] = inst.type;
      }
    }
    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 100) {
      changed = false;
      std::unordered_map<int, Const> known;
      for (Block& block : function.blocks) {
        for (Inst& inst : block.insts) {
          if (isConstOp(inst.op)) {
            known[inst.id] = constOf(inst);
            continue;
          }
          if (inst.op == Op::Copy && inst.operands.size() == 1) {
            auto found = known.find(inst.operands[0]);
            if (found == known.end()) continue;
            applyConst(inst, found->second);
            known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Binary && inst.operands.size() == 2) {
            auto lhs = known.find(inst.operands[0]);
            auto rhs = known.find(inst.operands[1]);
            if (lhs == known.end() || rhs == known.end()) continue;
            Const folded;
            if (!foldBinary(inst.text, lhs->second, rhs->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Unary && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            Const folded;
            if (!foldUnary(inst.text, operand->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Select && inst.operands.size() == 3) {
            auto cond = known.find(inst.operands[0]);
            if (cond == known.end() || !isBool(cond->second)) continue;
            int chosen = cond->second.b ? inst.operands[1] : inst.operands[2];
            Inst copy;
            copy.op = Op::Copy;
            copy.id = inst.id;
            copy.type = inst.type;
            copy.operands = {chosen};
            copy.span = inst.span;
            inst = std::move(copy);
            auto found = known.find(chosen);
            if (found != known.end()) known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Cast && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            auto st = valTypes.find(inst.operands[0]);
            Const folded;
            if (!foldCast(inst.text,
                          st == valTypes.end() ? TypePtr{} : st->second, inst.type,
                          operand->second, folded)) {
              continue;
            }
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Freeze && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            applyConst(inst, operand->second);
            known[inst.id] = operand->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Fma && inst.operands.size() == 3) {
            auto a = known.find(inst.operands[0]);
            auto b = known.find(inst.operands[1]);
            auto c = known.find(inst.operands[2]);
            if (a == known.end() || b == known.end() || c == known.end()) continue;
            Const folded;
            if (!foldFma(a->second, b->second, c->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Phi && !inst.operands.empty()) {
            auto first = known.find(inst.operands.front());
            if (first == known.end()) continue;
            bool allConst = true;
            bool same = true;
            for (std::size_t i = 1; i < inst.operands.size(); ++i) {
              auto other = known.find(inst.operands[i]);
              if (other == known.end()) { allConst = false; break; }
              if (!sameConst(first->second, other->second)) same = false;
            }
            if (!allConst || !same) continue;
            applyConst(inst, first->second);
            known[inst.id] = first->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
        }
      }
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
