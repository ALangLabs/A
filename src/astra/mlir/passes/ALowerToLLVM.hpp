//===- ALowerToLLVM.hpp - `a` dialect -> `llvm` dialect lowering --*- C++-*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Phase-1 lowering pass: rewrites `a` dialect ops into `llvm` dialect ops so
// the module can be handed to the LLVM IR translator / opt pipeline. The
// pass matches `a` ops by name so it compiles before the TableGen dialect
// headers are generated; once they land the dispatch in ALowerToLLVM.cpp
// should move to typed accessors (see INTEGRATION POINTS there).
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_PASSES_A_LOWER_TO_LLVM_HPP
#define A_MLIR_PASSES_A_LOWER_TO_LLVM_HPP

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringRef.h"

#include <memory>

namespace a {

/// Lowers `a` dialect ops to the `llvm` dialect. Phase-1 semantics mirror
/// the boxed-i64 ABI emitted by src/astra/frontend/mlir/mlir_emit.cpp.
struct ALowerToLLVMPass
    : public mlir::PassWrapper<ALowerToLLVMPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ALowerToLLVMPass)

  ALowerToLLVMPass() = default;

  llvm::StringRef getArgument() const final { return "a-lower-to-llvm"; }
  llvm::StringRef getDescription() const final {
    return "Lower the `a` dialect to the `llvm` dialect";
  }

  void getDependentDialects(mlir::DialectRegistry &registry) const override;
  void runOnOperation() override;
};

/// Factory used by pipeline.hpp / PassManager::addPass.
std::unique_ptr<mlir::Pass> createALowerToLLVMPass();

} // namespace a

#endif // A_MLIR_PASSES_A_LOWER_TO_LLVM_HPP
