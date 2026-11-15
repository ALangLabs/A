//===- pipeline.hpp - A MLIR pass pipeline entry points --------*- C++ -*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Central place that registers dialects and builds the optimization/lowering
// PassManager pipelines for `a` dialect modules. Consumers (driver, `a build`
// native path, tools) should call registerAllDialects()/buildAOptPipeline()
// instead of assembling passes by hand.
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_PIPELINE_HPP
#define A_MLIR_PIPELINE_HPP

namespace mlir {
class MLIRContext;
class PassManager;
} // namespace mlir

namespace a {

/// Registers the `a` dialect on `ctx`. Implemented in pipeline.cpp; depends
/// on the TableGen dialect under src/astra/mlir/include/A/ (its
/// mlir::a::registerDialect hook, declared in A/A.h).
void registerADialect(mlir::MLIRContext &ctx);

/// Registers all dialects needed by the A MLIR pipeline on `ctx`:
/// llvm, func, scf, cf, arith, index, vector plus the `a` dialect.
/// Idempotent: dialects already loaded on the context are skipped by MLIR.
void registerAllDialects(mlir::MLIRContext &ctx);

/// Registers the A-specific passes (currently `a-lower-to-llvm`) with the
/// global pass registry so PassManager string pipelines and
/// mlir::parsePassPipeline can find them. Idempotent.
void registerAPasses();

/// Builds the standard A optimization/lowering pipeline on `pm`:
///   - canonicalizer + CSE always;
///   - mem2reg + SROA when optLevel > 0 (followed by another
///     canonicalize/CSE cleanup);
///   - the `a` -> `llvm` lowering pass when `emitLlvmDialect` is true
///     (followed by canonicalize/CSE).
/// `optLevel` mirrors `-O<n>` (0..3). Does NOT add nested pipelines; callers
/// composing this into a larger pipeline may add their own.
void buildAOptPipeline(mlir::PassManager &pm, int optLevel,
                       bool emitLlvmDialect);

} // namespace a

#endif // A_MLIR_PIPELINE_HPP
