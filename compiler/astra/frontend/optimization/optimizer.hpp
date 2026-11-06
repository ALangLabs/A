#pragma once

#include "ir/ir.hpp"

namespace a {
namespace ir {

// LLVM-style optimization level (mirrors clang -O levels / LLVM New Pass
// Manager OptimizationLevel). Ordering matters: O0 < O1 < O2 < O3.
enum class OptLevel : int { O0 = 0, O1 = 1, O2 = 2, O3 = 3, Os = -1, Oz = -2, Ofast = 4 };

// Parses "-O0".." -O3", "-Os", "-Oz", "-Ofast" (and "--opt-level=N").
// Returns false on an unrecognized spelling.
bool parseOptLevel(const std::string& flag, OptLevel& level);
const char* optLevelName(OptLevel level);
int optLevelToInt(OptLevel level);
bool optLevelFromInt(int value, OptLevel& level);

// Optimization statistics collected while a pass walks the program.
struct OptimizationStats {
  int constantsFolded = 0;
  int deadInstructions = 0;
  int deadBlocks = 0;
  int functionsInlined = 0;
  int loopsHoisted = 0;
  int copiesPropagated = 0;
  int subexpressionsEliminated = 0;
  int operationsSimplified = 0;
  int branchesFolded = 0;
  int deadStoresEliminated = 0;

  void merge(const OptimizationStats& other) {
    constantsFolded += other.constantsFolded;
    deadInstructions += other.deadInstructions;
    deadBlocks += other.deadBlocks;
    functionsInlined += other.functionsInlined;
    loopsHoisted += other.loopsHoisted;
    copiesPropagated += other.copiesPropagated;
    subexpressionsEliminated += other.subexpressionsEliminated;
    operationsSimplified += other.operationsSimplified;
    branchesFolded += other.branchesFolded;
    deadStoresEliminated += other.deadStoresEliminated;
  }

  bool isClean() const {
    return constantsFolded == 0 && deadInstructions == 0 && deadBlocks == 0 &&
           functionsInlined == 0 && loopsHoisted == 0 && copiesPropagated == 0 &&
           subexpressionsEliminated == 0 && operationsSimplified == 0 &&
           branchesFolded == 0 && deadStoresEliminated == 0;
  }
};

// Constant folding and propagation across binaries, unaries, selects, phis,
// and string concatenation. Runs to a fixpoint.
OptimizationStats constantFold(Program& program);

// Removes unreachable blocks and pure instructions whose results are unused.
OptimizationStats deadCodeElimination(Program& program);

// Inlines single-block functions whose bodies contain only pure instructions.
OptimizationStats inlineFunctions(Program& program);

// Hoists loop-invariant pure instructions into natural-loop preheaders.
OptimizationStats loopInvariantCodeMotion(Program& program);

// LLVM EarlyCSE / GVN-lite: replaces a pure instruction whose opcode and
// operands were already computed by a Copy of the earlier value.
OptimizationStats commonSubexpressionElimination(Program& program);

// LLVM InstSimplify lite: copy propagation, identity operations (x+0, x*1,
// x*0), and strength reduction (x*2^k -> x<<k, x/2^k -> x>>k for powers of
// two and power-of-two constants).
OptimizationStats algebraicSimplify(Program& program);

// LLVM SimplifyCFG lite: folds branch-on-constant to a jump, threads jumps
// through empty blocks, and merges an empty fall-through block.
OptimizationStats branchFolding(Program& program);

// LLVM DeadStoreElimination lite: removes a store whose location is
// // overwritten by a later store of the same receiver/index before any read.
OptimizationStats deadStoreElimination(Program& program);

// LLVM InstCombine lite: whole-function algebraic simplification using a
// def-use map — cast folding/chaining, same-operand comparisons and bitwise
// ops, select/unary involutions, aggregate (tuple) extract/insert folding,
// and copy-chain collapsing.
OptimizationStats instCombine(Program& program);

// LLVM SCCP lite: sparse conditional constant propagation. Propagates
// constants through pure ops and phi nodes, discovers which branches are
// constant under the propagated facts, and folds instructions plus
// branch-on-constant terminators in the surviving reachable region.
OptimizationStats conditionalConstantPropagation(Program& program);

// Runs all passes until the program stops changing (bounded).
OptimizationStats runAll(Program& program);

// LLVM-style level-aware pipeline (New PassManager analog): the pass set is
// selected by OptLevel, mirroring -O0 (off) through -Ofast (aggressive).
OptimizationStats runOptPipeline(Program& program, OptLevel level);

}  // namespace ir
}  // namespace a
