# IR optimizer

`compiler/astra/frontend/optimization/` implements an LLVM-style pass pipeline
over the SSA IR from `ir/`. Used by `a ir --opt` / `-O` flags and by
`tests/ir/ir_tests.cpp`.

## Files

| File | Role |
| --- | --- |
| `optimizer.hpp` | Public API: `enum class OptLevel {O0,O1,O2,O3,Os=-1,Oz=-2,Ofast=4}`, `parseOptLevel`/`optLevelName`/`optLevelToInt`/`optLevelFromInt`, `struct OptimizationStats` (per-pass counters + `merge`/`isClean`), pass entry points, `runAll`, `runOptPipeline(program, level)`. |
| `optimizer_pipeline.cpp` / `optimizer_level.cpp` | Pass selection per `OptLevel`, mirroring clang `-O0`…`-Ofast`. |
| `optimizer_constfold.cpp` | Constant folding/propagation to fixpoint. |
| `optimizer_dce.cpp` | Unreachable-block removal + dead pure instructions. |
| `optimizer_inline.cpp` | Inlines single-block pure functions. |
| `optimizer_licm.cpp` | Loop-invariant code motion into natural-loop preheaders. |
| `optimizer_instcombine.cpp` | LLVM InstCombine-lite: def-use map, cast folding/chaining, same-operand ops, select/unary involutions, tuple extract/insert folding, copy-chain collapse. |
| `optimizer_simplify.cpp` | InstSimplify-lite: copy propagation, identity ops (`x+0`, `x*1`, `x*0`), strength reduction (`x*2^k` → `x<<k`). |
| `optimizer_sccp.cpp` | Sparse conditional constant propagation: propagates constants through pure ops/phis, folds constant branches. |
| `optimizer_internal.hpp` | Shared helpers: `isConstOp`/`isPureOp`/`isEffectfulOp` classification, `struct Const`, `sameConst`/`constOf`/`constFingerprint`/`applyConst`, `foldBinary`/`foldUnary`/`foldCast` (trunc/zext/sext/fptrunc/fpext/fptosi/fptoui/sitofp/uitofp/bitcast), `wrapAdd/Sub/Mul`, `compareInts`/`compareFloats`. |

## Pass set

`constantFold`, `deadCodeElimination`, `inlineFunctions`,
`loopInvariantCodeMotion`, `commonSubexpressionElimination` (EarlyCSE/GVN-lite),
`algebraicSimplify`, `branchFolding` (SimplifyCFG-lite),
`deadStoreElimination`, `instCombine`, `conditionalConstantPropagation`
(SCCP-lite). `runAll` iterates to a bounded fixpoint; `runOptPipeline` selects
the subset by level.

Related: [IR](frontend-ir.md), [Driver/CLI](frontend-driver.md),
[overview](overview.md).
