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
// `a test` mode IS supported: the consumer scans the merged module for the
// emitter's per-module test-fn symbols (`__a_f_m<id>_test_*_<seq>` llvm.func
// definitions owned by the entry module) and synthesizes the
// a_test_main(name, fnptr) harness tail that whole-graph emitTestMain emits.
// testFilter is applied at COMPILE time by the emitter (a_test_main takes no
// filter argument), so the consumer applies the same substring predicate
// over the recovered test names; pass "" for unfiltered `a test`.
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
  /// __a_main_body, __a_gc_root_mark and main (or the a_test_main harness
  /// `main` when loaded with testMode=true).
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
/// `testMode`: synthesize the `a test` tail (a_test_main harness over the
/// entry module's `test_*` fns) instead of the normal `main` trampoline, and
/// skip the `entryBodyMlir` splice (whole-graph emitMainBody drops the entry
/// body under test mode too — module inits still run via __a_main_body).
///
/// `testFilter`: compile-time filter, mirroring emitMLIR's testFilter —
/// only `test_*` fns whose recovered source name contains the substring are
/// registered with a_test_main ("" = all tests). There is NO runtime
/// filtering: a_test_main receives the final name/fnptr tables.
///
/// Returns 0 on success and fills `pkg`; returns 1 on failure with `err`.
int ambcModulesLoad(const std::string& pkgDir,
                    const std::string& entryBodyMlir, bool testMode,
                    AmbcModules& pkg, std::ostream& err,
                    const std::string& testFilter = "");

/// Convenience: load + lower + emit object in one call.
///   arch     - target arch/triple; "" / "host" selects the host default.
///   optLevel - 0..3, mirroring `opt -O<n>`.
///   entryBodyMlir - see ambcModulesLoad (ignored when testMode).
///   testMode - see ambcModulesLoad.
///   testFilter - see ambcModulesLoad (default "" = unfiltered `a test`).
/// Both trailing parameters are defaulted so the existing call signature
/// (and the pipeline.cpp weak fallback stub) stays source-compatible.
/// Returns 0 on success, 1 on failure (diagnostic on `err`).
int ambcModulesCompileToObject(const std::string& pkgDir,
                               const std::string& arch, int optLevel,
                               const std::string& objPath,
                               const std::string& entryBodyMlir,
                               bool testMode, std::ostream& err,
                               const std::string& testFilter = "");

} // namespace a

#endif // A_MLIR_AMBC_MODULES_HPP
