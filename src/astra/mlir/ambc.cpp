//===- ambc.cpp - .ambc bytecode package -> object -------------------*- C++ -*-===//
//
// Part of the A project. See docs/mlir-architecture.md and
// docs/mojo-kgen-notes.md (the `.mojoc` analog).
//
// Consumes a precompiled `.ambc` package (written by `a precompile`):
// header/version check -> MLIR BytecodeReader -> ModuleOp -> in-process
// LLVM backend (mlirModuleToObject). This skips the entire frontend
// (parse + comptime + Sema + ownership + MLIR emission) — the Mojo
// `.mojoc` fast path.
//
//===----------------------------------------------------------------------===//

#include "mlir/ambc.hpp"

#include <iosfwd>
#include <string>

#include "mlir/bytecode.hpp"
#include "mlir/codegen.hpp"
#include "mlir/native.hpp"
#include "mlir/pipeline.hpp"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"

namespace a {

int ambcCompileToObject(const std::string& ambcPath, const std::string& arch,
                        int optLevel, const std::string& objPath,
                        std::ostream& err) {
  // Cheap header check first: rejects foreign/stale files without paying
  // for a bytecode parse.
  std::string reason;
  if (!bytecodeIsCompatible(ambcPath, reason)) {
    err << "error: ambc package '" << ambcPath << "' is not usable: "
        << reason << "\n";
    return 1;
  }

  // Fresh context per compile: bytecode modules are self-contained and a
  // clean context avoids cross-package symbol/attribute collisions. Core
  // dialects + LLVM translation interfaces come from registerCoreDialects;
  // registerAllDialects additionally loads the `a` dialect so packages
  // produced by future `a`-dialect emitters still load.
  mlir::MLIRContext ctx;
  registerAllDialects(ctx);
  registerCoreDialects(ctx);

  std::string readError;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      readModuleBytecode(ctx, ambcPath, readError);
  if (!module) {
    err << "error: failed to read bytecode package '" << ambcPath << "': "
        << readError << "\n";
    return 1;
  }

  // Packages may carry `a`-dialect ops (a.runtime.call, a.* boxes): run the
  // same lowering pipeline nativeCompileMlirTextToObject uses so only the
  // llvm dialect reaches translateModuleToLLVMIR.
  mlir::PassManager pm(module->getContext());
  registerAPasses();
  buildAOptPipeline(pm, optLevel, /*emitLlvmDialect=*/true);
  if (failed(pm.run(module->getOperation()))) {
    err << "error: ambc package '" << ambcPath
        << "': MLIR pass pipeline failed\n";
    return 1;
  }

  return mlirModuleToObject(*module, arch, optLevel, objPath, err);
}

} // namespace a
