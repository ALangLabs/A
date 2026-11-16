//===- package.hpp - .ambc precompiled-package API ------------------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md
// ("Bytecode format" section). Mojo `.mojoc` analog: `a precompile` runs the
// frontend pipeline (parse -> comptime -> Sema -> ownership), lowers checked
// MLIR to text (whole-graph emitMLIR, or per-module emitModuleMLIR), parses
// it into ModuleOp(s) through the native bridge, and serializes each with
// writeModuleBytecode.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <iosfwd>
#include <string>

namespace mlir {
class MLIRContext;
class ModuleOp;
template <typename OpT>
class OwningOpRef;
} // namespace mlir

namespace a {

struct PipelineConfig; // pipeline/pipeline.hpp

// ---------------------------------------------------------------------------
// Package layout decision (v2: per-module `.ambc`, Mojo `.mojoc` model)
//
// Two output modes, selected by `precompilePackage`'s `outPath`:
//
//  * SINGLE-FILE (default, backward compatible): `outPath` empty or naming a
//    `.ambc` file -> ONE `.ambc` containing the whole checked module graph,
//    lowered by the whole-graph `emitMLIR(graph, sema, ...)`. Default output
//    is the resolved entry source with its extension swapped to `.ambc`
//    (`foo/bar.astra` -> `foo/bar.ambc`, `pkg/__init__.astra` ->
//    `pkg/__init__.ambc`); `-o file.ambc` overrides.
//
//  * PER-MODULE DIRECTORY (true `.mojoc` model): `outPath` names an existing
//    directory, ends with '/' or '\\', or ends with the suffix ".ambc.d" ->
//    `precompilePackageModules` writes ONE `.ambc` per ModuleGraph module
//    into that directory, plus `ambc.index` and `ambc.meta` (formats below).
//    This is the mode `a precompile -o <dir>` / `-o <name>.ambc.d` selects.
//
// A directory argument as `packageDirOrFile` is treated as a package:
// `precompilePackage` tries `<dir>/__init__.astra` first, then
// `<dir>/<dirname>.astra`, then `<parent>/<dirname>.astra` (mirroring how
// `import pkg` resolves `pkg/__init__.astra` in ModuleLoader).
//
// ---------------------------------------------------------------------------
// Per-module package layout (`precompilePackageModules`)
//
// Given `outDir`, for every `Module` in `ModuleGraph::modules` (including the
// auto-loaded prelude) the pipeline emits that module's checked MLIR via
// `emitModuleMLIR(graph, sema, moduleId, text, error)`, parses the text with
// parseMlirText(sharedMlirContext()), and serializes with
// writeModuleBytecode to `<outDir>/<moduleStem>.ambc`.
//
// File naming: `<moduleStem>` is `stemOf(module.path)` (basename without
// extension, "module" when empty). When two or more modules share a stem the
// colliding files are disambiguated as `<stem>-m<moduleId>.ambc`; unique stems
// keep the bare `<stem>.ambc`. All files live flat in `outDir`.
//
// `ambc.index` (consumed by loadAmbcIndex): one line per module,
//   <ambc_file_name><TAB><absolute_source_path>
// `#` comments and blank lines are allowed; paths may be relative to `outDir`
// (the writer emits bare file names for the .ambc side and absolute paths for
// the source side).
//
// `ambc.meta` (module-attribute sidecar, `ambc-modules-v1`):
//   # comment lines start with '#'
//   format=ambc-modules-v1
//   compiler=<kAmbcVersion>          (a::kVersion stamp, same as .ambc header)
//   entry=<moduleId>                 (ModuleGraph::entry)
//   prelude=<moduleId|-1>            (ModuleGraph::preludeId)
//   init_order=<id>,<id>,...         (ModuleGraph::order verbatim: dependency
//                                    order, imports before importers; this is
//                                    the module-init call sequence the
//                                    consumer must synthesize into
//                                    __a_main_body / module init calls)
//   module_count=<n>
//   module <id>\t<ambc_file>\t<source_path>\t<module.name>
// one `module` line per emitted .ambc, in ModuleGraph::modules order.
//
// CONSUMER SPEC (pipeline / driver side, not implemented here):
//   1. loadAmbcIndex(outDir) yields canonical source -> .ambc for every
//      module; ambc.meta supplies entry/prelude/init_order without parsing
//      sources.
//   2. For each module id in `init_order` (then any remaining ids), read its
//      .ambc via readModuleBytecode(sharedMlirContext(), ...) and collect the
//      resulting ModuleOps. Each per-module ModuleOp contains ONLY that
//      module's functions/globals, already name-mangled per module id
//      (`__a_f_m<id>_<name>`, `__a_g_<id>_<name>`); cross-module references
//      are direct symbol references to those mangled names, so ALL module
//      ops must be merged (clone ops into one ModuleOp, or link ModuleOps)
//      BEFORE lowering — symbol resolution requires every module present.
//   3. After merging, synthesize the global tail that whole-graph emitMLIR
//      would have produced: runtime function declarations, the per-module
//      init functions called in `init_order` sequence, `__a_main_body`
//      dispatching to `entry`'s init/main, GC root registration, and `main`.
//      The `entry` id from ambc.meta identifies which module's init is the
//      program entry point.
// ---------------------------------------------------------------------------

// Runs the frontend pipeline far enough to obtain checked IR and writes it
// as MLIR bytecode to `outPath`.
//
// Steps: resolve `packageDirOrFile` to a concrete entry `.astra` (see layout
// note above), read it, runFrontendPipeline() (ModuleLoader::load ->
// runComptimePass -> Sema::analyze/analyzeParallel -> analyzeOwnership).
//
// Output mode (see "Package layout decision" above):
//   * `outPath` empty or a non-directory file path -> whole-graph
//     emitMLIR -> parseMlirText -> writeModuleBytecode -> single `.ambc`.
//   * `outPath` an existing directory, ending in '/' or '\\', or ending in
//     ".ambc.d" -> delegates to precompilePackageModules (per-module
//     `.ambc` + ambc.index + ambc.meta in that directory).
//
// Returns 0 on success. On failure returns 1 (kCErr convention used across
// the frontend) or 66 (kNoIn) when the entry cannot be read/resolved, and
// `err` receives the diagnostic. Warnings are flushed to `err` as well.
int precompilePackage(const std::string& packageDirOrFile,
                      const std::string& outPath, const PipelineConfig& config,
                      std::ostream& err);

// Per-module `.ambc` package writer (Mojo `.mojoc` model). Runs the frontend
// pipeline ONCE, then for each module in the resulting ModuleGraph calls
// emitModuleMLIR(graph, sema, moduleId, ...) -> parseMlirText ->
// writeModuleBytecode into `outDir` (created if missing), and finally writes
// `outDir/ambc.index` + `outDir/ambc.meta` (formats documented above).
//
// `emitModuleMLIR` is provided by the frontend MLIR emitter
// (src/astra/frontend/mlir/mlir_emit.*); package.cpp forward-declares it so
// this TU compiles before that symbol lands.
//
// Same return convention as precompilePackage. If any single module fails to
// emit/parse/serialize, the function fails (partial output may remain in
// outDir; callers should treat the directory as unusable).
int precompilePackageModules(const std::string& packageDirOrFile,
                             const std::string& outDir,
                             const PipelineConfig& config,
                             std::ostream& err);

// Convenience: sibling `.ambc` path for a source path
// (`foo/bar.astra` -> `foo/bar.ambc`). Used by both `precompilePackage`'s
// default output and the suggested loader/driver hook.
std::string ambcSiblingPath(const std::string& sourcePath);

// Resolves `packageDirOrFile` to its entry `.astra` (same rules as
// precompilePackage) and returns the default `.ambc` output path for it.
// Returns "" if no entry can be resolved. Used by the CLI to report the
// output path when `-o` was not given.
std::string defaultAmbcOutputPath(const std::string& packageDirOrFile);

// `a precompile` CLI entry point. Parses argv itself:
//   a precompile <file.astra|dir> [-o out.ambc] [-I dir]... [--stdlib dir]
//       [--jobs n|--parallel] [--no-color] [-h]
// Expects the ORIGINAL process argv with argv[1] == "precompile" (it also
// tolerates argv[1] being the path if the integrator strips the subcommand).
// Returns 0 on success, 1 on compile/precompile failure, 64 on usage error
// (kExitUsage convention from cli/main.cpp). `out` receives the success
// line; `err` receives diagnostics.
int runPrecompileCli(int argc, char** argv, std::ostream& out,
                     std::ostream& err);

} // namespace a
