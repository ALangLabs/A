#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
OptimizationStats runAll(Program& program) {
  OptimizationStats total;
  for (int round = 0; round < 8; ++round) {
    OptimizationStats pass;
    pass.merge(constantFold(program));
    pass.merge(algebraicSimplify(program));
    pass.merge(instCombine(program));
    pass.merge(commonSubexpressionElimination(program));
    pass.merge(branchFolding(program));
    pass.merge(conditionalConstantPropagation(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(inlineFunctions(program));
    pass.merge(constantFold(program));
    pass.merge(deadStoreElimination(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(loopInvariantCodeMotion(program));
    pass.merge(constantFold(program));
    pass.merge(instCombine(program));
    pass.merge(deadCodeElimination(program));
    total.merge(pass);
    if (pass.isClean()) break;
  }
  return total;
}

OptimizationStats runOptPipeline(Program& program, OptLevel level) {
  switch (level) {
    case OptLevel::O0:
      return OptimizationStats{};  // optnone
    case OptLevel::O1: {
      // LLVM -O1: still always-on InstSimplify, DCE, SimplifyCFG.
      OptimizationStats total;
      total.merge(constantFold(program));
      total.merge(algebraicSimplify(program));
      total.merge(instCombine(program));
      total.merge(commonSubexpressionElimination(program));
      total.merge(branchFolding(program));
      total.merge(conditionalConstantPropagation(program));
      total.merge(deadCodeElimination(program));
      return total;
    }
    case OptLevel::Os:
    case OptLevel::Oz: {
      // LLVM -Os/-Oz: O2-ish but size-tuned (same passes, no aggressive
      // inlining/unrolling heuristics in this pipeline).
      return runOptPipeline(program, OptLevel::O2);
    }
    case OptLevel::Ofast:
      return runOptPipeline(program, OptLevel::O3);
    case OptLevel::O2:
    case OptLevel::O3: {
      OptimizationStats total = runAll(program);
      if (level == OptLevel::O3) {
        // -O3 adds a second wave (extra CSE + LICM after inlining may expose
        // new common subexpressions / invariant motions).
        total.merge(commonSubexpressionElimination(program));
        total.merge(loopInvariantCodeMotion(program));
        total.merge(branchFolding(program));
        total.merge(constantFold(program));
        total.merge(deadCodeElimination(program));
      }
      return total;
    }
  }
  return OptimizationStats{};
}
}  // namespace ir
}  // namespace a
