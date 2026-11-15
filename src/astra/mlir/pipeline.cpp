//===- pipeline.cpp - A MLIR pass pipeline implementation -------*- C++ -*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
//===----------------------------------------------------------------------===//

#include "pipeline.hpp"

#include "passes/ALowerToLLVM.hpp"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

namespace mlir {
namespace a {
// Defined by the dialect workstream (src/astra/mlir/dialect/*.cpp); the
// canonical declaration lives in include/A/A.h, which cannot be included
// here because it pulls in TableGen-generated .inc headers.
void registerDialect(DialectRegistry &registry);
} // namespace a
} // namespace mlir

namespace a {

void registerADialect(mlir::MLIRContext &ctx) {
  // The `a` dialect keeps its registration hook in mlir::a (see A/A.h).
  // Forward-declared above: append its dialect allocator to the context's
  // registry, then force-load it.
  mlir::DialectRegistry registry;
  mlir::a::registerDialect(registry);
  ctx.appendDialectRegistry(registry);
  ctx.getOrLoadDialect("a");
}

void registerAllDialects(mlir::MLIRContext &ctx) {
  ctx.loadDialect<mlir::LLVM::LLVMDialect, mlir::func::FuncDialect,
                  mlir::scf::SCFDialect, mlir::cf::ControlFlowDialect,
                  mlir::arith::ArithDialect, mlir::index::IndexDialect,
                  mlir::vector::VectorDialect>();
  registerADialect(ctx);
}

void registerAPasses() {
  static bool registered = [] {
    mlir::registerPass([] { return createALowerToLLVMPass(); });
    return true;
  }();
  (void)registered;
}

void buildAOptPipeline(mlir::PassManager &pm, int optLevel,
                       bool emitLlvmDialect) {
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());

  if (optLevel > 0) {
    // Promote promotable alloca/loads/stores to SSA values. Legal only on
    // modules whose memory ops use standard memref/LLVM address semantics;
    // the pass leaves anything it cannot promote untouched.
    pm.addPass(mlir::createMem2Reg());
    pm.addPass(mlir::createSROA());
    pm.addPass(mlir::createCanonicalizerPass());
    pm.addPass(mlir::createCSEPass());
  }

  if (emitLlvmDialect) {
    registerAPasses();
    pm.addPass(createALowerToLLVMPass());
    pm.addPass(mlir::createCanonicalizerPass());
    pm.addPass(mlir::createCSEPass());
  }
}

} // namespace a
