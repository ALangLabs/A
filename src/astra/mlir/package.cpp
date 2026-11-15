//===- package.cpp - .ambc precompiled-package implementation --------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md
// ("Bytecode format") and package.hpp for the layout decision and the
// integration-point spec.
//
// Link deps (integrator): this TU calls into the frontend pipeline
// (runFrontendPipeline -> a_pipeline), the MLIR text emitter (emitMLIR ->
// frontend a_mlir), and the native/bytecode bridge (parseMlirText /
// sharedMlirContext / writeModuleBytecode / bytecodeIsCompatible ->
// a_mlir_core). Whatever library owns package.cpp must link
// a::pipeline + a::mlir (frontend emitter) + a::mlir_core.
//
//===----------------------------------------------------------------------===//

#include "package.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "bytecode.hpp"
#include "diagnostics/diagnostics.hpp"
#include "mlir/mlir_emit.hpp"
#include "name-resolution/module_loader.hpp"
#include "native.hpp"
#include "pipeline/pipeline.hpp"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"

namespace a {

// Per-module emitter provided by the frontend MLIR backend
// (src/astra/frontend/mlir/mlir_emit.cpp, owned by a parallel workstream).
// Forward-declared here so this TU compiles before that symbol lands; the
// declaration matches the one being added to mlir/mlir_emit.hpp.
bool emitModuleMLIR(const ModuleGraph& graph, const SemaResult& sema,
                    int moduleId, std::string& out, std::string& error);

namespace fs = std::filesystem;
namespace {

// Same exit-code convention as the frontend pipeline / cli/main.cpp.
constexpr int kOk = 0;
constexpr int kCErr = 1;
constexpr int kNoIn = 66;

// Mirrors ModuleLoader::canonicalKey (module_loader.cpp): weakly_canonical
// with an absolute-path fallback so AmbcIndex keys match byPath_ keys.
std::string canonicalKey(const std::string& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

bool readTextFile(const std::string& path, std::string& out,
                  std::ostream& err) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    err << "error: '" << path << "' is a directory, not a source file\n";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot read '" << path << "'\n";
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// Resolves a package directory or a plain source file to the concrete entry
// `.astra` path. Directory candidates mirror ModuleLoader's package
// resolution (`import pkg` -> `pkg/__init__.astra`):
//   1. <dir>/__init__.astra
//   2. <dir>/<dirname>.astra        (e.g. pkg/math.astra inside pkg/)
//   3. <parent>/<dirname>.astra     (sibling file next to the directory,
//                                  e.g. `re.astra` beside `re/`)
bool resolvePackageEntry(const std::string& dirOrFile, std::string& entry,
                         std::ostream& err) {
  std::error_code ec;
  if (!fs::is_directory(dirOrFile, ec)) {
    entry = dirOrFile;
    return true;
  }
  const fs::path dir = fs::absolute(dirOrFile, ec);
  const std::string dirname = dir.filename().string();
  const std::vector<fs::path> candidates = {
      dir / "__init__.astra",
      dir / (dirname + "." + kSourceExt),
      dir.parent_path() / (dirname + "." + kSourceExt),
  };
  for (const fs::path& candidate : candidates) {
    std::error_code ec2;
    if (fs::is_regular_file(candidate, ec2) && !ec2) {
      entry = candidate.string();
      return true;
    }
  }
  err << "error: no entry module found in package directory '" << dirOrFile
      << "' (looked for __init__.astra and " << dirname << "." << kSourceExt
      << ")\n";
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// AmbcIndex
// ---------------------------------------------------------------------------

const std::string* AmbcIndex::find(const std::string& canonicalSourcePath) const {
  auto it = bySource.find(canonicalSourcePath);
  if (it != bySource.end()) return &it->second;
  // Forgiving lookup: canonicalize the query and retry, so callers may pass
  // a non-canonical path.
  const std::string key = canonicalKey(canonicalSourcePath);
  it = bySource.find(key);
  return it == bySource.end() ? nullptr : &it->second;
}

void AmbcIndex::add(const std::string& sourcePath,
                    const std::string& ambcPath) {
  bySource[canonicalKey(sourcePath)] = ambcPath;
}

std::string ambcSiblingPath(const std::string& sourcePath) {
  fs::path p(sourcePath);
  if (p.extension().empty()) return sourcePath + ".ambc";
  p.replace_extension("ambc");
  return p.string();
}

std::string canonicalSourceKey(const std::string& path) {
  return canonicalKey(path);
}

std::string defaultAmbcOutputPath(const std::string& packageDirOrFile) {
  std::string entry;
  std::ostringstream devnull;
  if (!resolvePackageEntry(packageDirOrFile, entry, devnull)) return {};
  return ambcSiblingPath(entry);
}

bool loadAmbcIndex(const std::string& root, AmbcIndex& index) {
  index = AmbcIndex{};
  index.root = root;
  std::error_code ec;
  const fs::path rootPath(root);

  if (fs::is_regular_file(rootPath, ec) && !ec) {
    // Single .ambc file: source is the sibling .astra.
    const std::string ambc = fs::absolute(rootPath, ec).string();
    fs::path src = rootPath;
    src.replace_extension(kSourceExt);
    index.add(src.string(), ambc);
    return true;
  }
  if (!fs::is_directory(rootPath, ec) || ec) return true; // empty index

  const fs::path absRoot = fs::absolute(rootPath, ec);

  // Optional manifest: <root>/ambc.index, lines of
  //   <ambc_path><TAB><source_path>
  // Paths are relative to `root` unless absolute. `#` starts a comment.
  {
    std::ifstream manifest(absRoot / "ambc.index");
    std::string line;
    while (std::getline(manifest, line)) {
      const std::size_t hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      const std::size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      std::string ambc = line.substr(0, tab);
      std::string src = line.substr(tab + 1);
      auto trim = [](std::string& s) {
        const auto b = s.find_first_not_of(" \t\r");
        const auto e = s.find_last_not_of(" \t\r");
        s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
      };
      trim(ambc);
      trim(src);
      if (ambc.empty() || src.empty()) continue;
      fs::path a = fs::path(ambc).is_absolute() ? fs::path(ambc)
                                                : absRoot / ambc;
      fs::path s = fs::path(src).is_absolute() ? fs::path(src)
                                               : absRoot / src;
      index.add(s.string(), a.string());
    }
  }

  // Recursive scan for *.ambc; each maps to its sibling .astra. Manifest
  // entries win (add() would overwrite, so skip existing keys).
  std::error_code itEc;
  for (fs::recursive_directory_iterator it(absRoot, itEc), end;
       !itEc && it != end; it.increment(itEc)) {
    const fs::path& p = it->path();
    if (!it->is_regular_file(itEc) || itEc) continue;
    if (p.extension() != ".ambc") continue;
    fs::path src = p;
    src.replace_extension(kSourceExt);
    // Skip .ambc files with no .astra sibling: in per-module packages the
    // flat <stem>[-m<id>].ambc names never match a real source, and the
    // ambc.index manifest already carries the authoritative mapping.
    // (Whole-graph .ambc files sit next to their .astra, so this does not
    // change existing behavior.)
    std::error_code srcEc;
    if (!fs::is_regular_file(src, srcEc) || srcEc) continue;
    const std::string key = canonicalKey(src.string());
    if (index.bySource.count(key)) continue; // manifest already mapped it
    index.bySource[key] = p.string();
  }
  return true;
}

// ---------------------------------------------------------------------------
// precompilePackageModules - per-module .ambc package (.mojoc model)
// ---------------------------------------------------------------------------

namespace {

// Absolute-ize a module path for index/meta records. module.path is the
// loader's display path and may be relative when the entry was given
// relative to the cwd; canonicalKey only weakly canonicalizes, so make the
// path absolute first for stable cross-process lookups.
std::string absoluteModulePath(const std::string& path) {
  std::error_code ec;
  const fs::path abs = fs::absolute(path, ec);
  return ec ? path : abs.string();
}

// Moves every op out of `src`'s body into `dst`'s body (append order).
void moveModuleOpsInto(mlir::ModuleOp dst, mlir::ModuleOp src) {
  mlir::Block* dstBody = &dst.getBodyRegion().front();
  mlir::Block* srcBody = &src.getBodyRegion().front();
  while (!srcBody->empty()) {
    mlir::Operation* op = &srcBody->front();
    op->moveBefore(dstBody, dstBody->end());
  }
}

// `<stem>.ambc` for unique stems, `<stem>-m<id>.ambc` for stems shared by
// more than one module (see package.hpp "Per-module package layout").
std::string moduleAmbcFileName(const Module& module, int stemCollisions) {
  std::string stem = stemOf(module.path);
  if (stemCollisions > 1) {
    stem += "-m" + std::to_string(module.id);
  }
  return stem + ".ambc";
}

bool writeAmbcMeta(const fs::path& outDir, const ModuleGraph& graph,
                   const std::vector<std::string>& ambcFiles,
                   const std::vector<std::string>& sourcePaths,
                   std::ostream& err) {
  std::ofstream meta(outDir / "ambc.meta", std::ios::binary | std::ios::trunc);
  if (!meta) {
    err << "error: cannot write '" << (outDir / "ambc.meta") << "'\n";
    return false;
  }
  meta << "# ambc.meta v1 - per-module .ambc package descriptor\n"
       << "# format documented in src/astra/mlir/package.hpp\n"
       << "format=ambc-modules-v1\n"
       << "compiler=" << kAmbcVersion << "\n"
       << "entry=" << graph.entry << "\n"
       << "prelude=" << graph.preludeId << "\n"
       << "init_order=";
  for (std::size_t i = 0; i < graph.order.size(); ++i) {
    if (i) meta << ",";
    meta << graph.order[i];
  }
  meta << "\n"
       << "module_count=" << ambcFiles.size() << "\n";
  // `module` records in ModuleGraph::modules (deque) order; ambcFiles and
  // sourcePaths are indexed by module id.
  for (const Module& m : graph.modules) {
    const int id = m.id;
    if (id < 0 || static_cast<std::size_t>(id) >= ambcFiles.size() ||
        ambcFiles[static_cast<std::size_t>(id)].empty()) {
      continue;
    }
    meta << "module " << id << "\t" << ambcFiles[static_cast<std::size_t>(id)]
         << "\t" << sourcePaths[static_cast<std::size_t>(id)] << "\t"
         << m.name << "\n";
  }
  return static_cast<bool>(meta);
}

} // namespace

int precompilePackageModules(const std::string& packageDirOrFile,
                             const std::string& outDir,
                             const PipelineConfig& config,
                             std::ostream& err) {
  std::string entry;
  if (!resolvePackageEntry(packageDirOrFile, entry, err)) return kNoIn;

  std::string source;
  if (!readTextFile(entry, source, err)) return kNoIn;

  // Full frontend pipeline once; per-module emission reuses the same
  // checked ModuleGraph + SemaResult for every module id.
  DiagnosticSink sink;
  CompiledModule compiled =
      runFrontendPipeline(entry, std::move(source), config, sink, err, err);
  if (!compiled.ok || !compiled.graph || !compiled.sema) return kCErr;

  ModuleGraph& graph = *compiled.graph;

  std::error_code ec;
  fs::create_directories(outDir, ec);
  if (ec) {
    err << "error: cannot create output directory '" << outDir << "': "
        << ec.message() << "\n";
    return kCErr;
  }
  const fs::path outDirPath = fs::absolute(outDir, ec);
  const fs::path outRoot = ec ? fs::path(outDir) : outDirPath;

  // Stem collision counts across all modules (prelude included).
  std::unordered_map<std::string, int> stemCounts;
  for (const Module& m : graph.modules) ++stemCounts[stemOf(m.path)];

  const std::size_t moduleCount = graph.modules.size();
  std::vector<std::string> ambcFiles(moduleCount);
  std::vector<std::string> sourcePaths(moduleCount);

  mlir::MLIRContext& ctx = sharedMlirContext();

  // Emit + serialize each module. Iterate the deque (load order); ids are
  // dense 0..n-1 so they index ambcFiles/sourcePaths directly.
  for (const Module& m : graph.modules) {
    const int id = m.id;
    if (id < 0 || static_cast<std::size_t>(id) >= moduleCount) continue;

    std::string mlirText;
    std::string mlirError;
    if (!emitModuleMLIR(graph, *compiled.sema, id, mlirText, mlirError)) {
      err << "error: per-module MLIR emission failed for '" << m.path
          << "' (module " << id << "): " << mlirError << "\n";
      return kCErr;
    }

    std::string parseError;
    mlir::OwningOpRef<mlir::ModuleOp> module =
        parseMlirText(ctx, mlirText, parseError);
    if (!module) {
      err << "error: failed to parse emitted MLIR for '" << m.path
          << "' (module " << id << "): " << parseError << "\n";
      return kCErr;
    }

    // Entry module only: merge the entry body's module-level support ops
    // (literal globals, lambda/parfor helpers, auto-vivified globals)
    // into its .ambc so the entry.mlir statements' symbol references
    // resolve when the consumer links the per-module units.
    // emitEntrySupportMLIR is deterministic with emitEntryBodyMLIR —
    // both runs mint identical "__a_e" symbol names.
    if (id == graph.entry) {
      std::string supportText, supportError;
      if (!emitEntrySupportMLIR(graph, *compiled.sema, supportText,
                                supportError)) {
        err << "error: entry-body support emission failed for '" << m.path
            << "': " << supportError << "\n";
        return kCErr;
      }
      if (!supportText.empty()) {
        std::string supParseError;
        mlir::OwningOpRef<mlir::ModuleOp> supportModule =
            parseMlirText(ctx, supportText, supParseError);
        if (!supportModule) {
          err << "error: failed to parse entry-body support MLIR for '"
              << m.path << "': " << supParseError << "\n";
          return kCErr;
        }
        moveModuleOpsInto(*module, *supportModule);
      }
    }

    const std::string fileName =
        moduleAmbcFileName(m, stemCounts[stemOf(m.path)]);
    const fs::path outFile = outRoot / fileName;
    std::string bcError;
    if (!writeModuleBytecode(*module, outFile.string(), bcError)) {
      err << "error: failed to write bytecode package '" << outFile.string()
          << "': " << bcError << "\n";
      return kCErr;
    }

    ambcFiles[static_cast<std::size_t>(id)] = fileName;
    sourcePaths[static_cast<std::size_t>(id)] = absoluteModulePath(m.path);
  }

  // entry.mlir: the entry fn's body statements (no func wrapper), which
  // the .ambc consumer splices verbatim into its synthesized
  // __a_main_body after the __a_m<id>_init calls. Empty when the graph
  // has no entry fn.
  {
    std::string entryBody, entryBodyError;
    if (!emitEntryBodyMLIR(graph, *compiled.sema, entryBody, entryBodyError)) {
      err << "error: entry-body emission failed: " << entryBodyError << "\n";
      return kCErr;
    }
    std::ofstream entryFile(outRoot / "entry.mlir",
                            std::ios::binary | std::ios::trunc);
    if (!entryFile) {
      err << "error: cannot write '" << (outRoot / "entry.mlir") << "'\n";
      return kCErr;
    }
    entryFile << entryBody;
    if (!entryFile) {
      err << "error: failed writing '" << (outRoot / "entry.mlir") << "'\n";
      return kCErr;
    }
  }

  // ambc.index: `<ambc_file><TAB><absolute_source_path>` per line, the exact
  // format loadAmbcIndex parses (paths relative to outDir on the .ambc side).
  {
    std::ofstream index(outRoot / "ambc.index",
                        std::ios::binary | std::ios::trunc);
    if (!index) {
      err << "error: cannot write '" << (outRoot / "ambc.index") << "'\n";
      return kCErr;
    }
    for (std::size_t id = 0; id < ambcFiles.size(); ++id) {
      if (ambcFiles[id].empty()) continue;
      index << ambcFiles[id] << "\t" << sourcePaths[id] << "\n";
    }
    if (!index) {
      err << "error: failed writing '" << (outRoot / "ambc.index") << "'\n";
      return kCErr;
    }
  }

  if (!writeAmbcMeta(outRoot, graph, ambcFiles, sourcePaths, err)) {
    return kCErr;
  }
  return kOk;
}

// ---------------------------------------------------------------------------
// precompilePackage
// ---------------------------------------------------------------------------

// True when `outPath` selects per-module directory output: an existing
// directory, a trailing path separator, or the ".ambc.d" suffix (e.g.
// `-o foo.ambc.d`).
bool isPerModuleOutputPath(const std::string& outPath) {
  if (outPath.empty()) return false;
  if (!outPath.empty() &&
      (outPath.back() == '/' || outPath.back() == '\\')) {
    return true;
  }
  if (outPath.size() >= 7 &&
      outPath.compare(outPath.size() - 7, 7, ".ambc.d") == 0) {
    return true;
  }
  std::error_code ec;
  return fs::is_directory(outPath, ec) && !ec;
}

int precompilePackage(const std::string& packageDirOrFile,
                      const std::string& outPath, const PipelineConfig& config,
                      std::ostream& err) {
  // Per-module (.mojoc-style) mode: `-o` names a directory.
  if (isPerModuleOutputPath(outPath)) {
    return precompilePackageModules(packageDirOrFile, outPath, config, err);
  }

  std::string entry;
  if (!resolvePackageEntry(packageDirOrFile, entry, err)) return kNoIn;

  std::string source;
  if (!readTextFile(entry, source, err)) return kNoIn;

  // Full frontend pipeline through ownership analysis — identical to what
  // `a build`/`a run` feed to emitMLIR, minus codegen.
  DiagnosticSink sink;
  CompiledModule compiled =
      runFrontendPipeline(entry, std::move(source), config, sink, err, err);
  if (!compiled.ok || !compiled.graph || !compiled.sema) return kCErr;

  std::string mlirText;
  std::string mlirError;
  if (!emitMLIR(*compiled.graph, *compiled.sema, mlirText, mlirError)) {
    err << "error: MLIR emission failed for '" << entry << "': " << mlirError
        << "\n";
    return kCErr;
  }

  // Parse the llvm-dialect text into a ModuleOp on the shared context
  // (core dialects + LLVM translation interfaces already registered).
  mlir::MLIRContext& ctx = sharedMlirContext();
  std::string parseError;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      parseMlirText(ctx, mlirText, parseError);
  if (!module) {
    err << "error: failed to parse emitted MLIR for '" << entry << "': "
        << parseError << "\n";
    return kCErr;
  }

  const std::string output =
      outPath.empty() ? ambcSiblingPath(entry) : outPath;
  std::string bcError;
  if (!writeModuleBytecode(*module, output, bcError)) {
    err << "error: failed to write bytecode package '" << output << "': "
        << bcError << "\n";
    return kCErr;
  }
  return kOk;
}

} // namespace a
