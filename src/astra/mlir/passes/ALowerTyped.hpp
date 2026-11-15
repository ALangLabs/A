//===- ALowerTyped.hpp - `a` boxing ops -> `llvm` lowering --------*- C++-*-===//
//
// Part of the A project. See docs/mlir-typed-values-design.md (M1).
//
// Lowers the phase-2 boxing boundary ops `a.to_aval` / `a.from_aval` into
// `llvm` dialect ops implementing the tagged-i64 aval ABI
// (src/astra/runtime/a_runtime.h, src/astra/frontend/mlir/mlir_emit.cpp).
// The pass runs before `a-lower-to-llvm` so typed scalars never reach the
// boxed-ABI lowering.
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_PASSES_A_LOWER_TYPED_HPP
#define A_MLIR_PASSES_A_LOWER_TYPED_HPP

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringRef.h"

#include <memory>

namespace a {

/// Lowers `a.to_aval` / `a.from_aval` to `llvm` dialect ops. All other ops
/// (including the rest of the `a` dialect) are left untouched for
/// `a-lower-to-llvm`.
struct ALowerTypedPass
    : public mlir::PassWrapper<ALowerTypedPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ALowerTypedPass)

  ALowerTypedPass() = default;

  llvm::StringRef getArgument() const final { return "a-lower-typed"; }
  llvm::StringRef getDescription() const final {
    return "Lower a.to_aval/a.from_aval boxing ops to the llvm dialect";
  }

  void getDependentDialects(mlir::DialectRegistry &registry) const override;
  void runOnOperation() override;
};

/// Factory used by pipeline.hpp / PassManager::addPass.
std::unique_ptr<mlir::Pass> createALowerTypedPass();

} // namespace a

#endif // A_MLIR_PASSES_A_LOWER_TYPED_HPP
