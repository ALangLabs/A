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

// Emits ONLY the entry function's body as MLIR text — the same statements
// whole-graph emitMLIR inlines into __a_main_body (global inits already
// live in per-module __a_m<id>_init fns, so this emits just the entry
// portion: hoisted allocas first, then the entry statements, exactly as
// emitMainBody produces them). The output is splice-ready: it is meant to
// be inserted verbatim inside the consumer's synthesized __a_main_body
// after the __a_m<id>_init calls. Used for per-module .ambc packages'
// entry.mlir. Empty when the graph has no entry fn.
//
// Module-level artifacts the entry body creates (literal globals, lambda/
// parfor helpers, auto-vivified globals) are NOT in `out`; they are
// emitted under "__a_e"-flavored names (__a_estr*/__a_echr*/__a_ecstr*/
// __a_elam_*/__a_epf_*) so they cannot collide with the per-module
// __a_str/__a_chr/__a_cstr/__a_lam_/__a_pf_ counters (which restart per
// module). Retrieve them with emitEntrySupportMLIR and merge into the
// entry module's .ambc at precompile time.
bool emitEntryBodyMLIR(const ModuleGraph& graph, const SemaResult& sema,
                       std::string& out, std::string& error);

// Companion to emitEntryBodyMLIR: the module-level ops the entry body
// needs — private literal globals, auto-vivified __a_g_ globals, and
// lambda/parfor helper function definitions — emitted as a standalone
// `module { ... }` document for the precompile step to merge into the
// entry module's .ambc before serialization. Deterministic: a second run
// produces identical symbol names to the emitEntryBodyMLIR run.
bool emitEntrySupportMLIR(const ModuleGraph& graph, const SemaResult& sema,
                          std::string& out, std::string& error);
}  // namespace a
