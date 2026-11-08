#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
namespace {

// Folds a single pure instruction against the SCCP constant environment.
// `incomingExec` selects which phi incoming edges count (superset during
// discovery, exact reachable set when applying).
bool sccpFoldInst(const Inst& inst, const std::unordered_map<int, Const>& known,
                  const std::vector<char>& incomingExec,
                  const std::unordered_map<int, TypePtr>& valTypes, Const& out) {
  if (isConstOp(inst.op)) {
    out = constOf(inst);
    return true;
  }
  if (!inst.producesValue()) return false;
  switch (inst.op) {
    case Op::Copy:
      if (inst.operands.size() == 1) {
        auto found = known.find(inst.operands[0]);
        if (found != known.end()) { out = found->second; return true; }
      }
      return false;
    case Op::Binary:
      if (inst.operands.size() == 2) {
        auto lhs = known.find(inst.operands[0]);
        auto rhs = known.find(inst.operands[1]);
        if (lhs != known.end() && rhs != known.end()) {
          return foldBinary(inst.text, lhs->second, rhs->second, out);
        }
      }
      return false;
    case Op::Unary:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) return foldUnary(inst.text, operand->second, out);
      }
      return false;
    case Op::Select:
      if (inst.operands.size() == 3) {
        auto cond = known.find(inst.operands[0]);
        if (cond != known.end() && isBool(cond->second)) {
          auto chosen = known.find(cond->second.b ? inst.operands[1] : inst.operands[2]);
          if (chosen != known.end()) { out = chosen->second; return true; }
        }
      }
      return false;
    case Op::Cast:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) {
          auto st = valTypes.find(inst.operands[0]);
          return foldCast(inst.text, st == valTypes.end() ? TypePtr{} : st->second,
                          inst.type, operand->second, out);
        }
      }
      return false;
    case Op::Freeze:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) { out = operand->second; return true; }
      }
      return false;
    case Op::Fma:
      if (inst.operands.size() == 3) {
        auto a = known.find(inst.operands[0]);
        auto b = known.find(inst.operands[1]);
        auto c = known.find(inst.operands[2]);
        if (a != known.end() && b != known.end() && c != known.end())
          return foldFma(a->second, b->second, c->second, out);
      }
      return false;
    case Op::Phi: {
      if (inst.operands.empty()) return false;
      bool have = false;
      for (std::size_t i = 0; i < inst.operands.size(); ++i) {
        int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
        if (from < 0 || from >= static_cast<int>(incomingExec.size()) ||
            !incomingExec[static_cast<std::size_t>(from)]) {
          continue;
        }
        auto found = known.find(inst.operands[i]);
        if (found == known.end()) return false;
        if (!have) {
          out = found->second;
          have = true;
        } else if (!sameConst(out, found->second)) {
          return false;
        }
      }
      return have;
    }
    default:
      return false;
  }
}

}  // namespace

OptimizationStats conditionalConstantPropagation(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    const std::size_t n = function.blocks.size();
    if (n == 0) continue;
    std::unordered_map<int, TypePtr> valTypes;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts) {
        if (inst.producesValue() && inst.type) valTypes[inst.id] = inst.type;
      }
    }

    // Phase A: grow the executable-block superset and the constant
    // environment to a fixpoint. Monotone, so it terminates.
    std::unordered_map<int, Const> known;
    std::vector<char> exec(n, 0);
    exec[0] = 1;
    std::vector<char> all(n, 1);  // phi incoming check during discovery
    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 200) {
      changed = false;
      for (std::size_t b = 0; b < n; ++b) {
        if (!exec[b]) continue;
        for (Inst& inst : function.blocks[b].insts) {
          if (known.count(inst.id)) continue;
          Const folded;
          if (!sccpFoldInst(inst, known, all, valTypes, folded)) continue;
          known[inst.id] = folded;
          changed = true;
        }
      }
      for (std::size_t b = 0; b < n; ++b) {
        if (!exec[b]) continue;
        const Terminator& term = function.blocks[b].term;
        auto add = [&](int target) {
          if (target >= 0 && static_cast<std::size_t>(target) < n &&
              !exec[static_cast<std::size_t>(target)]) {
            exec[static_cast<std::size_t>(target)] = 1;
            changed = true;
          }
        };
        if (term.kind == Terminator::Kind::Jump) {
          add(term.target);
        } else if (term.kind == Terminator::Kind::Branch) {
          auto cond = known.find(term.value);
          if (cond != known.end() && isBool(cond->second)) {
            add(cond->second.b ? term.target : term.falseTarget);
          } else {
            add(term.target);
            add(term.falseTarget);
          }
        }
      }
    }

    // Phase B: exact reachable set under the discovered constants.
    std::vector<char> reach(n, 0);
    reach[0] = 1;
    std::vector<int> worklist{0};
    while (!worklist.empty()) {
      int b = worklist.back();
      worklist.pop_back();
      const Terminator& term = function.blocks[static_cast<std::size_t>(b)].term;
      auto add = [&](int target) {
        if (target < 0 || static_cast<std::size_t>(target) >= n ||
            reach[static_cast<std::size_t>(target)]) {
          return;
        }
        reach[static_cast<std::size_t>(target)] = 1;
        worklist.push_back(target);
      };
      if (term.kind == Terminator::Kind::Jump) {
        add(term.target);
      } else if (term.kind == Terminator::Kind::Branch) {
        auto cond = known.find(term.value);
        if (cond != known.end() && isBool(cond->second)) {
          add(cond->second.b ? term.target : term.falseTarget);
        } else {
          add(term.target);
          add(term.falseTarget);
        }
      }
    }

    // Phase C: apply folds inside the reachable region only.
    for (std::size_t b = 0; b < n; ++b) {
      if (!reach[b]) continue;
      for (Inst& inst : function.blocks[b].insts) {
        if (isConstOp(inst.op)) continue;
        Const folded;
        if (!sccpFoldInst(inst, known, reach, valTypes, folded)) continue;
        // Phi re-check with the exact predecessor set.
        if (inst.op == Op::Phi) {
          bool have = false;
          bool ok = true;
          for (std::size_t i = 0; i < inst.operands.size() && ok; ++i) {
            int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
            if (from < 0 || !reach[static_cast<std::size_t>(from)]) continue;
            auto found = known.find(inst.operands[i]);
            if (found == known.end() ||
                (have && !sameConst(folded, found->second))) {
              ok = false;
            } else {
              folded = found->second;
              have = true;
            }
          }
          if (!ok || !have) continue;
        }
        applyConst(inst, folded);
        ++stats.constantsFolded;
      }
      Terminator& term = function.blocks[b].term;
      if (term.kind == Terminator::Kind::Branch) {
        auto cond = known.find(term.value);
        if (cond != known.end() && isBool(cond->second)) {
          term.kind = Terminator::Kind::Jump;
          term.target = cond->second.b ? term.target : term.falseTarget;
          term.falseTarget = -1;
          term.value = -1;
          ++stats.branchesFolded;
        }
      }
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
