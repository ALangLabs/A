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

#include "bytecode.hpp"
#include "diagnostics/diagnostics.hpp"
#include "mlir/mlir_emit.hpp"
#include "name-resolution/module_loader.hpp"
#include "native.hpp"
#include "pipeline/pipeline.hpp"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"

namespace a {
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
    const std::string key = canonicalKey(src.string());
    if (index.bySource.count(key)) continue; // manifest already mapped it
    index.bySource[key] = p.string();
  }
  return true;
}

// ---------------------------------------------------------------------------
// precompilePackage
// ---------------------------------------------------------------------------

int precompilePackage(const std::string& packageDirOrFile,
                      const std::string& outPath, const PipelineConfig& config,
                      std::ostream& err) {
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
