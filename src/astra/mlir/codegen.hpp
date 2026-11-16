#pragma once
// In-process MLIR -> LLVM IR -> object backend (Mojo ObjectCompiler analog).
// Replaces the mlir-translate/opt/llc subprocess chain: translates the llvm
// dialect module to an LLVM IR module, runs the LLVM opt pipeline in-process,
// and emits the object via a TargetMachine MC stream. See
// docs/mlir-architecture.md.
//
// Requires MLIR 18 / LLVM 18 dev files. Callers should invoke initMLIR() and
// initLLVM() once before use (both are idempotent and safe to call early).

#include <iosfwd>
#include <string>

namespace mlir {
class ModuleOp;
}

namespace a {

// Registers MLIR translations (including dialect -> LLVM IR translation
// interfaces). Must be called before an MLIRContext that expects LLVM IR
// translation is created. Idempotent.
void initMLIR();

// Initializes native LLVM targets, asm printers/parsers and MC for the host.
// Idempotent.
void initLLVM();

// One-shot init for both subsystems.
void initMLIRBackend();

// Lowers `module` (which must contain llvm-dialect operations) to a machine
// object file at `objPath`.
//   arch     - target triple or arch name; "" / "host" / "native" select the
//              host default triple (cross compile is unsupported).
//   optLevel - 0..3, mirroring `opt -O<n>` / `llc -O<n>`.
//   err      - receives diagnostics on failure.
// Returns 0 on success, 1 on failure.
int mlirModuleToObject(mlir::ModuleOp module, const std::string& arch,
                       int optLevel, const std::string& objPath,
                       std::ostream& err);

// GPU offload extraction (Mojo-style compile_offload): compiles llvm.funcs
// stamped with `a.offload` into a device module (NVPTX->PTX, AMDGPU->code
// object), strips them from the host module, and writes <objPath>.offload.bin
// + <objPath>.offload.c next to the host object for linkWithRuntime to
// compile/link. Returns 0 on success (including "no offloads present").
int prepareGpuOffload(mlir::ModuleOp module, const std::string& objPath,
                      int optLevel, std::ostream& err);

// Links an object produced by mlirModuleToObject with the staged A runtime
// archive by invoking the clang driver, mirroring pipeline.cpp's llvmBuild
// link stage (including the mold fast path). `runtimeLib` must be the
// resolved path to libaruntime.a (see findRuntime in pipeline.cpp).
// Returns 0 on success, 1 on failure.
int linkWithRuntime(const std::string& objPath, const std::string& outputPath,
                    const std::string& runtimeLib, std::ostream& err);

}  // namespace a
