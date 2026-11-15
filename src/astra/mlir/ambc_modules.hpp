//===- ambc_modules.hpp - per-module .ambc package consumer ----------*- C++ -*-===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md and
// the "Consumer spec" in src/astra/mlir/package.hpp.
//
// Consumes a PER-MODULE `.ambc` package directory (written by
// `a precompile <pkg> -o <dir>` / `precompilePackageModules`): reads
// `<dir>/ambc.meta`, loads every listed `.ambc` into ONE shared MLIRContext,
// merges all module ops into a single `module { ... }` ModuleOp, renames the
// per-emitter private string/char globals that would otherwise collide
// (`__a_str<N>`/`__a_chr<N>`/`__a_cstr<N>` counters restart per module),
// deduplicates `llvm.func` declarations that every module repeats, and
// synthesizes the program-global tail that per-module emission omits:
//
//   - the `a_*` runtime `llvm.func` declaration block (kRuntimeSigs, kept in
//     sync with mlir_emit.cpp - see ambc_modules.cpp),
//   - `__a_main_body`: calls `__a_m<id>_init` once per module in ambc.meta's
//     `init_order`, then splices the caller-supplied entry body,
//   - `__a_gc_root_mark` marking every `__a_g_*` global (the only globals that
//     hold avals; `__a_str/__a_chr/__a_cstr` hold raw byte arrays and must
//     NOT be marked),
//   - `main`: a_runtime_init + gc-root registration + a_fib_main trampoline,
//     mirroring whole-graph emitMain.
//
// ENTRY-BODY LIMITATION (verified against mlir_emit.cpp): `emitModuleMLIR`'s
// runModule path emits `__a_m<id>_init` = global inits + top-level stmts ONLY
// (emitModuleInit, mlir_emit.cpp ~line 3248). The `entry` fn's body is emitted
// exclusively inside whole-graph `__a_main_body` (emitMainBody ~line 1101) and
// is NOT present in any per-module `.ambc` - `isEntry` fns are skipped by
// collectTopLevel's fnDecls_ registration. A per-module package therefore
// CANNOT reconstruct the program entry from `.ambc` files alone; the caller
// MUST supply the entry body's MLIR statement text via `entryBodyMlir` (the
// statements of the `entry` fn body, exactly as whole-graph emitMainBody
// would inline them - e.g. produced by an emitter mode that treats the entry
// fn as a regular function body). Without it the merged module links but the
// program does nothing after module init.
//
// `a test` mode is NOT supported in v1: per-module `.ambc` carries no
// testFilter and the test_* harness needs `a_test_main` name/fn tables that
// cannot be recovered reliably from mangled symbols. Passing testMode=true
// returns a clean error.
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_AMBC_MODULES_HPP
#define A_MLIR_AMBC_MODULES_HPP

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "mlir/IR/BuiltinOps.h"   // mlir::ModuleOp (OwningOpRef needs a complete type)
#include "mlir/IR/OwningOpRef.h"

namespace mlir {
class MLIRContext;
} // namespace mlir

namespace a {

/// A loaded + merged per-module package. Owns the MLIRContext (module ops
/// are context-bound) and the merged ModuleOp. Pass `pkg.module.get()` to
/// mlirModuleToObject / the A opt pipeline; keep `pkg` alive until lowering
/// finishes.
struct AmbcModules {
  /// Fresh MLIRContext with core + `a` dialects registered (same set as
  /// ambcCompileToObject). Declared FIRST so reverse-order member
  /// destruction erases `module` while the context is still alive.
  std::unique_ptr<mlir::MLIRContext> context;
  /// The merged module: all per-module ops + synthesized runtime decls,
  /// __a_main_body, __a_gc_root_mark and main.
  mlir::OwningOpRef<mlir::ModuleOp> module;
  /// Module ids in ambc.meta `init_order` (the __a_m<id>_init call order).
  std::vector<int> initOrder;
  /// Entry module id from ambc.meta (`entry=`).
  int entryModule = -1;
};

/// Loads `<pkgDir>/ambc.meta`, reads every listed `.ambc` via
/// readModuleBytecode into one shared context, merges them into a single
/// ModuleOp and synthesizes the global tail described above.
///
/// `entryBodyMlir`: optional MLIR text spliced verbatim into `__a_main_body`
/// after the `__a_m<id>_init` calls - the `entry` fn's body statements (see
/// the ENTRY-BODY LIMITATION note above). May be empty for packages whose
/// entry module has no entry fn.
///
/// `testMode`: `a test` is unsupported (see header note); pass false. If
/// true, returns 1 with a clean diagnostic.
///
/// Returns 0 on success and fills `pkg`; returns 1 on failure with `err`.
int ambcModulesLoad(const std::string& pkgDir,
                    const std::string& entryBodyMlir, bool testMode,
                    AmbcModules& pkg, std::ostream& err);

/// Convenience: load + lower + emit object in one call.
///   arch     - target arch/triple; "" / "host" selects the host default.
///   optLevel - 0..3, mirroring `opt -O<n>`.
///   entryBodyMlir - see ambcModulesLoad.
///   testMode - see ambcModulesLoad (must be false in v1).
/// Returns 0 on success, 1 on failure (diagnostic on `err`).
int ambcModulesCompileToObject(const std::string& pkgDir,
                               const std::string& arch, int optLevel,
                               const std::string& objPath,
                               const std::string& entryBodyMlir,
                               bool testMode, std::ostream& err);

} // namespace a

#endif // A_MLIR_AMBC_MODULES_HPP
