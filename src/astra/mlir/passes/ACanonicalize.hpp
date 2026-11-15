//===- ACanonicalize.hpp - `a` dialect canonicalization pass ---*- C++-*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Canonicalization/folding for the `a` dialect -- the Mojo equivalent of
// dialect folders/canonicalizers. The rewrite patterns live in
// ACanonicalize.cpp and are exposed two ways:
//
//   * `a-canonicalize` -- a standalone greedy-rewrite pass (this header's
//     ACanonicalizePass), registered via registerACanonicalizePass();
//   * populateACanonicalizePatterns() -- adds the same patterns to any
//     RewritePatternSet so the integrator can fold them into
//     buildAOptPipeline()'s canonicalize passes.
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_PASSES_A_CANONICALIZE_HPP
#define A_MLIR_PASSES_A_CANONICALIZE_HPP

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringRef.h"

#include <memory>

namespace mlir {
class RewritePatternSet;
} // namespace mlir

namespace a {

/// Canonicalizes `a` dialect ops: boxing round-trips, identity `a.to_aval`,
/// constant folding through `a.from_aval`, and zero-offset `a.gep`.
struct ACanonicalizePass
    : public mlir::PassWrapper<ACanonicalizePass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ACanonicalizePass)

  ACanonicalizePass() = default;

  llvm::StringRef getArgument() const final { return "a-canonicalize"; }
  llvm::StringRef getDescription() const final {
    return "Canonicalize and fold `a` dialect ops";
  }

  void getDependentDialects(mlir::DialectRegistry &registry) const override;
  void runOnOperation() override;
};

/// Factory used by pipeline.hpp / PassManager::addPass.
std::unique_ptr<mlir::Pass> createACanonicalizePass();

/// Adds the `a` dialect canonicalization patterns to `patterns`. Intended
/// for use inside a canonicalize-style pass (e.g. via
/// `populateAndApplyPatterns` or `applyPatternsAndFoldGreedily`); the same
/// patterns back the standalone `a-canonicalize` pass.
void populateACanonicalizePatterns(mlir::RewritePatternSet &patterns);

/// Registers `a-canonicalize` with the global pass registry so
/// PassManager string pipelines and `mlir::parsePassPipeline` can find it.
/// Idempotent; call alongside a::registerAPasses().
void registerACanonicalizePass();

} // namespace a

#endif // A_MLIR_PASSES_A_CANONICALIZE_HPP
