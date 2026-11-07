#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
namespace {

void replaceValue(Function& function, int from, int to) {
  if (from == to) return;
  for (Block& block : function.blocks) {
    for (Inst& inst : block.insts) {
      for (int& operand : inst.operands) {
        if (operand == from) operand = to;
      }
    }
    if (block.term.value == from) block.term.value = to;
  }
}

bool allPure(const Block& block) {
  for (const Inst& inst : block.insts) {
    if (inst.op == Op::Phi) return false;
    if (!isPureOp(inst.op)) return false;
    if (inst.op == Op::Binary && (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
      return false;
    }
  }
  return true;
}

}  // namespace

OptimizationStats inlineFunctions(Program& program) {
  OptimizationStats stats;
  std::unordered_map<std::string, const Function*> candidates;
  for (const Function& function : program.functions) {
    if (function.isEntry) continue;
    if (function.blocks.size() != 1) continue;
    const Block& block = function.blocks.front();
    if (block.term.kind != Terminator::Kind::Return) continue;
    if (!allPure(block)) continue;
    candidates.emplace(function.name, &function);
    if (function.decl) candidates.emplace(function.decl->name, &function);
  }
  if (candidates.empty()) return stats;

  for (Function& caller : program.functions) {
    for (Block& block : caller.blocks) {
      std::vector<Inst> rewritten;
      rewritten.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        auto candidate = inst.op == Op::Call ? candidates.find(inst.text) : candidates.end();
        if (candidate == candidates.end()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Function& callee = *candidate->second;
        if (inst.operands.size() != callee.params.size()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Block& calleeBlock = callee.blocks.front();
        std::unordered_map<int, int> env;
        std::size_t paramIndex = 0;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param && paramIndex < inst.operands.size()) {
            env[calleeInst.id] = inst.operands[paramIndex++];
          }
        }
        bool ok = true;
        std::unordered_set<int> defined;
        for (const auto& entry : env) defined.insert(entry.first);
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          for (int operand : calleeInst.operands) {
            if (operand >= 0 && !defined.count(operand)) { ok = false; break; }
          }
          if (!ok) break;
          defined.insert(calleeInst.id);
        }
        if (!ok) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        int returnValue = calleeBlock.term.value;
        if (returnValue < 0) {
          if (inst.id >= 0) {
            // Void call: drop it entirely, its result is never used.
            ++stats.functionsInlined;
            continue;
          }
          rewritten.push_back(std::move(inst));
          continue;
        }
        int mappedReturn = env.count(returnValue) ? env[returnValue] : -1;
        if (mappedReturn >= 0) {
          replaceValue(caller, inst.id, mappedReturn);
          ++stats.functionsInlined;
          continue;
        }
        // Clone the callee body, reusing the call's value id for the returned
        // instruction so downstream users keep referring to the same value.
        std::vector<Inst> clones;
        int produced = -1;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          Inst clone = calleeInst;
          for (int& operand : clone.operands) {
            if (operand >= 0) operand = env[operand];
          }
          int targetId = calleeInst.id == returnValue ? inst.id : caller.nextValue++;
          env[calleeInst.id] = targetId;
          clone.id = targetId;
          if (calleeInst.id == returnValue) produced = targetId;
          clones.push_back(std::move(clone));
        }
        if (produced < 0) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        for (Inst& clone : clones) rewritten.push_back(std::move(clone));
        ++stats.functionsInlined;
      }
      block.insts = std::move(rewritten);
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
