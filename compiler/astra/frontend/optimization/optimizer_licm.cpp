#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
namespace {

std::vector<std::vector<int>> computePredecessors(const Function& function) {
  std::vector<std::vector<int>> preds(function.blocks.size());
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    const Terminator& term = function.blocks[b].term;
    auto add = [&](int target) {
      if (target >= 0 && static_cast<std::size_t>(target) < preds.size()) {
        preds[static_cast<std::size_t>(target)].push_back(static_cast<int>(b));
      }
    };
    if (term.kind == Terminator::Kind::Jump) add(term.target);
    if (term.kind == Terminator::Kind::Branch) {
      add(term.target);
      add(term.falseTarget);
    }
  }
  return preds;
}

std::vector<std::unordered_set<int>> computeDominators(const Function& function,
                                                       const std::vector<std::vector<int>>& preds) {
  const std::size_t count = function.blocks.size();
  std::vector<std::unordered_set<int>> dom(count);
  std::unordered_set<int> all;
  for (std::size_t b = 0; b < count; ++b) all.insert(static_cast<int>(b));
  for (std::size_t b = 0; b < count; ++b) {
    dom[b] = b == 0 ? std::unordered_set<int>{0} : all;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t b = 1; b < count; ++b) {
      std::unordered_set<int> next;
      bool first = true;
      for (int pred : preds[b]) {
        if (first) {
          next = dom[static_cast<std::size_t>(pred)];
          first = false;
        } else {
          for (auto it = next.begin(); it != next.end();) {
            if (!dom[static_cast<std::size_t>(pred)].count(*it)) it = next.erase(it);
            else ++it;
          }
        }
      }
      if (first) next.clear();
      next.insert(static_cast<int>(b));
      if (next != dom[b]) {
        dom[b] = std::move(next);
        changed = true;
      }
    }
  }
  return dom;
}

}  // namespace

OptimizationStats loopInvariantCodeMotion(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    if (function.blocks.size() < 2) continue;
    const auto preds = computePredecessors(function);
    const auto dom = computeDominators(function, preds);
    std::unordered_map<int, int> defBlock;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      for (const Inst& inst : function.blocks[b].insts) defBlock[inst.id] = static_cast<int>(b);
    }

    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      const Terminator& term = function.blocks[b].term;
      std::vector<int> headers;
      if (term.kind == Terminator::Kind::Jump) headers.push_back(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        headers.push_back(term.target);
        headers.push_back(term.falseTarget);
      }
      for (int header : headers) {
        if (header < 0 || static_cast<std::size_t>(header) >= function.blocks.size()) continue;
        if (!dom[b].count(header)) continue;  // not a back edge
        // Collect the natural loop body: reverse reachability from the latch
        // back to the header.
        std::unordered_set<int> body;
        body.insert(header);
        std::vector<int> worklist{static_cast<int>(b)};
        while (!worklist.empty()) {
          int node = worklist.back();
          worklist.pop_back();
          if (!body.insert(node).second) continue;
          if (node == header) continue;
          for (int pred : preds[static_cast<std::size_t>(node)]) worklist.push_back(pred);
        }
        // The preheader must be a unique non-loop predecessor that only jumps
        // into the header.
        int preheader = -1;
        for (int pred : preds[static_cast<std::size_t>(header)]) {
          if (body.count(pred)) continue;
          if (preheader >= 0) { preheader = -1; break; }
          preheader = pred;
        }
        if (preheader < 0) continue;
        const Terminator& preTerm = function.blocks[static_cast<std::size_t>(preheader)].term;
        if (preTerm.kind != Terminator::Kind::Jump || preTerm.target != header) continue;

        std::vector<int> blockOrder(body.begin(), body.end());
        std::sort(blockOrder.begin(), blockOrder.end());
        for (int loopBlock : blockOrder) {
          Block& block = function.blocks[static_cast<std::size_t>(loopBlock)];
          std::vector<Inst> kept;
          kept.reserve(block.insts.size());
          for (Inst& inst : block.insts) {
            if (inst.op == Op::Phi || !isPureOp(inst.op) || !inst.producesValue()) {
              kept.push_back(std::move(inst));
              continue;
            }
            if (inst.op == Op::Binary &&
                (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
              kept.push_back(std::move(inst));
              continue;
            }
            bool invariant = true;
            for (int operand : inst.operands) {
              if (operand < 0) continue;
              auto found = defBlock.find(operand);
              if (found == defBlock.end()) { invariant = false; break; }
              int operandBlock = found->second;
              if (body.count(operandBlock)) { invariant = false; break; }
              if (operandBlock != preheader &&
                  !dom[static_cast<std::size_t>(preheader)].count(operandBlock)) {
                invariant = false;
                break;
              }
            }
            if (!invariant) {
              kept.push_back(std::move(inst));
              continue;
            }
            defBlock[inst.id] = preheader;
            function.blocks[static_cast<std::size_t>(preheader)].insts.push_back(
                std::move(inst));
            ++stats.loopsHoisted;
          }
          block.insts = std::move(kept);
        }
      }
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
