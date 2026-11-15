#pragma once
// Direct MLIR backend (A -> .mlir llvm dialect -> mlir-translate -> .ll ->
// opt/llc/clang). MVP subset; see mlir_emit.cpp header for coverage.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

bool emitMLIR(const ModuleGraph& graph, const SemaResult& sema,
              std::string& out, std::string& error, bool testMode = false,
              const std::string* testFilter = nullptr);

// Emits ONE module's worth of MLIR: a self-contained `module { }` holding
// only the decls owned by `moduleId` — its functions (`__a_f_m<id>_*`),
// struct ctors (`__a_ctor_*`), globals (`__a_g_<id>_*`), and a
// `__a_m<id>_init` fn with its global inits + top-level statements. This is
// the prerequisite for Mojo-style per-module `.ambc` packages.
//
// What the consumer must supply when linking per-module outputs:
//  - the `a_*` runtime decls block (the `sigs[]` table in emitDecls —
//    omitted here on purpose; re-declare once per final link unit),
//  - `__a_gc_root_mark` covering the module's GC-visible globals
//    (`__a_g_<id>_*` syms — the same set whole-graph emitGlobals collects),
//  - program orchestration: `__a_main_body` calls `__a_m<id>_init` once per
//    module in `graph.order` (dependency order — imports first), then runs
//    the entry module's entry-fn body; `main`/`a_test_main` wrap that.
//
// Cross-module references stay as direct calls to the mangled
// `__a_f_m<id>_*`/`__a_m*`/`__a_ctor_*` symbols: every such symbol is
// emitted as an `llvm.func` declaration (no body) in this unit, so the
// output is valid standalone MLIR and links at object time. Mangled names
// are identical to whole-graph emitMLIR output.
//
// SemaResult is accepted for API symmetry with emitMLIR; the emitter
// resolves everything from the AST + ModuleGraph (sema only fills in
// Expr::inferredType / FnDecl::moduleId annotations already present).
bool emitModuleMLIR(const ModuleGraph& graph, const SemaResult& sema,
                    int moduleId, std::string& out, std::string& error);
}  // namespace a
