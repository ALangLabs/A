#pragma once
// In-process bridge between the frontend's MLIR text output and the native
// MLIR/LLVM backend (docs/mlir-architecture.md). This module replaces the
// subprocess chain `mlir-translate --mlir-to-llvmir` + `opt`/`llc`/`clang` for
// the front half: parse the emitter's llvm-dialect MLIR text, run a small
// in-process canonicalize/CSE pass pipeline, and hand the ModuleOp to the
// codegen backend (mlirModuleToObject, implemented in codegen.cpp).
//
// Requires MLIR 18 / LLVM 18 dev files; only compiled when
// find_package(MLIR) succeeds (see src/astra/mlir/CMakeLists.txt).

#include <iosfwd>
#include <string>

#include "llvm/ADT/StringRef.h"
#include "mlir/IR/OwningOpRef.h"

namespace mlir {
class MLIRContext;
class ModuleOp;
}  // namespace mlir

namespace a {

// Registers the dialects needed by the bridge on `ctx`: func, llvm, cf, scf,
// arith, index and vector, plus the llvm-dialect -> LLVM IR translation
// interface (via a::initMLIR). Idempotent; safe to call on any context
// before parsing.
void registerCoreDialects(mlir::MLIRContext& ctx);

// Process-wide shared MLIR context with the core dialects registered once.
// NOTE: not safe for concurrent use by multiple threads compiling at once.
mlir::MLIRContext& sharedMlirContext();

// Parses MLIR source text (the llvm-dialect module text produced by
// emitMLIR) into a ModuleOp owned by `ctx`. On failure returns a null
// OwningOpRef and fills `error` with the captured diagnostics. The context
// must already have the relevant dialects loaded (call registerCoreDialects
// or use sharedMlirContext).
mlir::OwningOpRef<mlir::ModuleOp> parseMlirText(mlir::MLIRContext& ctx,
                                               llvm::StringRef text,
                                               std::string& error);

// Full in-process compile of emitter-produced MLIR text to an object file:
// parse -> canonicalizer/CSE -> mlirModuleToObject. Returns 0 on success,
// 1 on failure. Equivalent to the old `mlir-translate` + `opt` + `llc`
// subprocess chain, minus the subprocesses.
int nativeCompileMlirTextToObject(const std::string& mlirText,
                                  const std::string& arch, int optLevel,
                                  const std::string& objPath,
                                  std::ostream& err);

// Translates `module` to LLVM IR and returns the textual IR (equivalent of
// `mlir-translate --mlir-to-llvmir`). Useful for `a ir` and debugging
// parity with the subprocess path. On failure returns "" and fills `error`.
std::string mlirModuleToLlvmIrText(mlir::ModuleOp module,
                                   std::string& error);

}  // namespace a
