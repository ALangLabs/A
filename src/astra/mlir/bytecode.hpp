//===- bytecode.hpp - .ambc MLIR bytecode packaging ----------------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "driver/version.hpp" // a::kVersion

#include <cstdint>
#include <string>

#include "llvm/ADT/StringRef.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"

namespace mlir {
class ModuleOp;
} // namespace mlir

namespace a {

/// Container format version for `.ambc` files. Bump when the on-disk header
/// layout changes; readers reject files with a different version.
inline constexpr uint32_t kAmbcFormatVersion = 1;

/// Compiler version stamped into every `.ambc` producer record. Tied to
/// `a::kVersion` (src/astra/frontend/driver/version.hpp) so stale bytecode
/// produced by a different compiler build is rejected.
inline constexpr const char *kAmbcVersion = kVersion;

/// Magic bytes at offset 0 of every `.ambc` file: "AMBC".
inline constexpr char kAmbcMagic[4] = {'A', 'M', 'B', 'C'};

/// Serialize `module` to MLIR bytecode at `path`, producing an `.ambc` file:
/// a small fixed header (magic, format version, producer string, dialect
/// checksum) followed by the raw `mlir::BytecodeWriter` payload.
///
/// The header is a file-level prefix rather than a module attribute so that
/// (a) the serialized IR is not mutated, and (b) `bytecodeIsCompatible` can
/// reject stale packages without invoking the bytecode parser.
///
/// The producer record embeds `kAmbcVersion` plus the MLIR version; the
/// dialect checksum is an FNV-1a hash over the sorted namespaces of dialects
/// loaded in the module's context, recorded for finer-grained compatibility
/// checks by callers that can construct a matching context.
///
/// Returns true on success; on failure `error` is filled and no partial file
/// is guaranteed.
bool writeModuleBytecode(mlir::ModuleOp module, llvm::StringRef path,
                         std::string &error);

/// Read an `.ambc` file back into an `mlir::ModuleOp` owned by the returned
/// `OwningOpRef`. `ctx` must already have all dialects used by the module
/// registered (e.g. the `a` dialect and its dependencies).
///
/// Lazy-materialization strategy (v1): eager. `mlir::BytecodeReader` is
/// constructed with `lazyLoad=false` and the default `lazyOps` callback, so
/// every region is materialized during `readTopLevel` and `finalize` is a
/// no-op. To go lazy later: construct the reader with `lazyLoad=true`, pass
/// a `lazyOps` callback that defers non-entry-point ops (e.g. only materialize
/// ops reachable from exported `a.func`s), keep the backing `MemoryBuffer`
/// alive via the `bufferOwnerRef`/`SourceMgr` overload, and call `materialize`
/// on demand after `finalize`.
///
/// On failure returns a null `OwningOpRef` and fills `error`.
mlir::OwningOpRef<mlir::ModuleOp>
readModuleBytecode(mlir::MLIRContext &ctx, llvm::StringRef path,
                   std::string &error);

/// Cheap compatibility check: verifies the `.ambc` magic, format version,
/// and that the embedded producer stamp matches this toolchain's
/// `kAmbcVersion`/MLIR version, plus that the payload starts with the MLIR
/// bytecode magic. Does not fully parse the file. On failure returns false
/// and fills `reason` with a human-readable explanation (suitable for
/// deciding to fall back to the `.astra` source).
bool bytecodeIsCompatible(llvm::StringRef path, std::string &reason);

/// FNV-1a 64-bit checksum over the sorted namespaces of the dialects loaded
/// in `ctx`. Exposed so callers (e.g. `a precompile` / ModuleLoader) can
/// compare the checksum embedded in an `.ambc` header against their own
/// context when finer-grained dialect compatibility is required.
uint64_t bytecodeDialectChecksum(mlir::MLIRContext &ctx);

} // namespace a
