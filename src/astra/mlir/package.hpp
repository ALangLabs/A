//===- package.hpp - .ambc precompiled-package API ------------------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md
// ("Bytecode format" section). Mojo `.mojoc` analog: `a precompile` runs the
// frontend pipeline (parse -> comptime -> Sema -> ownership), lowers the
// checked ModuleGraph to MLIR text via emitMLIR, parses it into a ModuleOp
// through the native bridge, and serializes it with writeModuleBytecode.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>

namespace mlir {
class MLIRContext;
class ModuleOp;
template <typename OpT>
class OwningOpRef;
} // namespace mlir

namespace a {

struct PipelineConfig; // pipeline/pipeline.hpp

// ---------------------------------------------------------------------------
// Package layout decision (v1)
//
// emitMLIR(graph, sema, ...) flattens EVERY transitively imported module into
// ONE `module { }` op: function/global names are mangled per module id
// (`__a_f_m<id>_<name>`, `__a_g_<id>_<name>`), and cross-module calls are
// direct. There is no supported way to emit a single ModuleGraph module in
// isolation, so per-module `.ambc` files (one per .astra) are NOT possible
// without first splitting the emitter. v1 therefore writes ONE `.ambc` per
// `precompilePackage` call, containing the whole checked module graph of the
// given entry/package. Default output path: the resolved entry source with
// its extension swapped to `.ambc` (`foo/bar.astra` -> `foo/bar.ambc`,
// `pkg/__init__.astra` -> `pkg/__init__.ambc`); `-o` overrides.
//
// A directory argument is treated as a package: `precompilePackage` tries
// `<dir>/__init__.astra` first, then `<dir>/<dirname>.astra` (mirroring how
// `import pkg` resolves `pkg/__init__.astra` in ModuleLoader), then errors.
// ---------------------------------------------------------------------------

// Runs the frontend pipeline far enough to obtain checked IR and writes it
// as MLIR bytecode to `outPath`.
//
// Steps: resolve `packageDirOrFile` to a concrete entry `.astra` (see layout
// note above), read it, runFrontendPipeline() (ModuleLoader::load ->
// runComptimePass -> Sema::analyze/analyzeParallel -> analyzeOwnership),
// emitMLIR() -> parseMlirText(sharedMlirContext()) -> writeModuleBytecode().
//
// Returns 0 on success. On failure returns 1 (kCErr convention used across
// the frontend) or 66 (kNoIn) when the entry cannot be read/resolved, and
// `err` receives the diagnostic. Warnings are flushed to `err` as well.
int precompilePackage(const std::string& packageDirOrFile,
                      const std::string& outPath, const PipelineConfig& config,
                      std::ostream& err);

// ---------------------------------------------------------------------------
// AmbcIndex: module-path -> .ambc lookup for the ModuleLoader / driver hook.
//
// `bySource` maps the canonical (weakly_canonical) path of an `.astra` source
// file to the `.ambc` file that contains its checked MLIR. Because one `.ambc`
// holds a whole flattened module graph, the index should map the ENTRY source
// (the file the user asked to compile) to its `.ambc`; imports inside the
// package are already inside that ModuleOp and must not be looked up
// individually (see integration notes below).
// ---------------------------------------------------------------------------
struct AmbcIndex {
  // Root the index was built from (directory scanned, or "" for a single
  // file). Informational; lookups only consult `bySource`.
  std::string root;
  // canonical .astra path -> .ambc path
  std::unordered_map<std::string, std::string> bySource;

  // Returns the .ambc path for `canonicalSourcePath`, or nullptr.
  const std::string* find(const std::string& canonicalSourcePath) const;
  // Registers one mapping. `sourcePath` is canonicalized internally.
  void add(const std::string& sourcePath, const std::string& ambcPath);
};

// Builds an AmbcIndex under `root`:
//   * `root` is a regular `.ambc` file -> single entry, source = sibling
//     `.astra` (same basename, extension swapped).
//   * `root` is a directory -> first reads an optional manifest
//     `<root>/ambc.index` (one `ambc_path<TAB>source_path` per line, paths
//     relative to `root`; `#` comments allowed), then recursively scans for
//     `*.ambc` files and maps each to its sibling `.astra` (entries already
//     present from the manifest win).
// Returns false only on hard I/O errors; a missing root simply yields an
// empty index (returns true).
bool loadAmbcIndex(const std::string& root, AmbcIndex& index);

// Convenience: sibling `.ambc` path for a source path
// (`foo/bar.astra` -> `foo/bar.ambc`). Used by both `precompilePackage`'s
// default output and the suggested loader/driver hook.
std::string ambcSiblingPath(const std::string& sourcePath);

// Resolves `packageDirOrFile` to its entry `.astra` (same rules as
// precompilePackage) and returns the default `.ambc` output path for it.
// Returns "" if no entry can be resolved. Used by the CLI to report the
// output path when `-o` was not given.
std::string defaultAmbcOutputPath(const std::string& packageDirOrFile);

// Canonicalizes `path` the same way ModuleLoader::canonicalKey does
// (weakly_canonical, falling back to absolute). Exposed so callers can key
// AmbcIndex entries identically to ModuleLoader::byPath_.
std::string canonicalSourceKey(const std::string& path);

// ---------------------------------------------------------------------------
// Integration points (for the integrator - do NOT wire here)
//
// (a) CLI dispatch, src/astra/frontend/cli/main.cpp:
//     In main(), next to the other command dispatches (~line 346), add:
//         if (options.command == "precompile") {
//           return a::runPrecompileCli(argc, argv, std::cout, std::cerr);
//         }
//     Also add a usage line in printUsage() and relax the '-o' gate at
//     ~line 277 to allow precompile. runPrecompileCli expects the ORIGINAL
//     argv (argv[1] == "precompile"); it re-parses argv[2..] itself, so
//     Options/parseOptions do not need to know about it.
//
// (b) Consuming .ambc - RECOMMENDED (entry-level short-circuit):
//     In pipeline.cpp executePipeline()/buildPipeline(), BEFORE calling
//     runFrontendPipeline, canonicalize `path` and check
//     bytecodeIsCompatible(ambcSiblingPath(path), reason). On success call
//     readModuleBytecode(sharedMlirContext(), ambc, err) and hand the
//     ModuleOp straight to mlirModuleToObject()/nativeCompile - skipping
//     parse+sema+emitMLIR entirely (the Mojo .mojoc model). Gate behind
//     env A_AMBC=1 (or reuse A_MLIR_NATIVE) until validated. `a check`/`fmt`/
//     `doc` still need the AST, so they must keep using source.
//
// (c) Alternative (per-module, inside ModuleLoader::load): after
//     resolveQuoted/resolveBare produce `X.astra`, check `X.ambc` sibling.
//     NOT recommended for v1: a .ambc contains the entry's whole flattened
//     graph (including main/init code), so splicing it into a larger
//     ModuleGraph would duplicate the entry's dependencies and drag in a
//     second `main`. It only becomes viable if emitMLIR grows a
//     per-module/subgraph mode; revisit then.
// ---------------------------------------------------------------------------

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
