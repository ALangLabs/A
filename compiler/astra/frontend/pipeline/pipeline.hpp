#pragma once
// v2 pipeline for A. The C++ frontend (lex/parse/sema/ownership) provides
// `check`, `fmt`, `doc`; execution and native builds lower the checked AST
// to LLVM IR (frontend/llvm/) and compile it with the system LLVM
// toolchain (opt/llc/clang) against the prebuilt libaruntime.a.
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a {
class DiagnosticSink; struct ModuleGraph; struct SemaResult;
struct PipelineConfig {
  bool checkOnly = false, jsonDiagnostics = false, useColor = false;
  int optimizationLevel = 3;
  int jobs = 1;  // 1 = serial; >1 = parallel module checking (ty/pyrefly style)
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  // Cross-compilation target (e.g. "i386-linux-gnu", "aarch64", "native").
  // Empty means the host default. The LLVM backend is host-only: a target
  // arch different from the host is rejected at codegen time.
  std::string target;
  // `a test --filter <substr>`: only run test_* functions whose name
  // contains this substring. Empty means run every test.
  std::string testFilter;
  // `a run/test --vm`: interpret the checked module graph in the VM
  // backend (compiler/astra/frontend/vm/) instead of emitting LLVM IR
  // and linking a native binary. Host-only; `--native` selects the
  // default codegen path explicitly.
  bool vm = false;
  // Native codegen backend: "llvm" is the only native backend
  // (AST -> LLVM IR -> opt/llc/clang). "vm" is accepted for compatibility
  // but execution is governed by `vm` above.
  std::string backend = "llvm";
  // Internal: scan mode (`a test` over a directory/codebase) treats a
  // per-file filter that matches no tests as "skip this file" instead of an
  // error, so `--filter` can select matching tests across files.
  bool scanIgnoreFilterMismatch = false;
};
struct CompiledModule { std::unique_ptr<ModuleGraph> graph; std::unique_ptr<SemaResult> sema; bool ok = false; };
CompiledModule runFrontendPipeline(const std::string& path, std::string source, const PipelineConfig& config, DiagnosticSink& sink, std::ostream& diagOut, std::ostream& diagErr);
int executePipeline(const std::string& path, std::string source, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int testPipeline(const std::string& path, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
// `a test` on a file, directory, or (empty path) the current directory:
// discovers test files recursively and runs them, aggregating results.
int testPath(const std::string& path, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int buildPipeline(const std::string& path, const std::string& outputPath, const PipelineConfig& config, std::ostream& err);

// Interactive REPL session. The driver feeds the full accumulated source on
// every call; each eval is compiled and run by the LLVM toolchain as a
// fresh process, so top-level bindings do not persist between inputs.
struct ReplSession {
  ~ReplSession();
  ReplSession() = default;
  ReplSession(const ReplSession&) = delete;
  ReplSession& operator=(const ReplSession&) = delete;
};
// Returns 0 on success, 1 on compile failure, 2 on runtime error.
int replEval(ReplSession& session, const std::string& source, const PipelineConfig& config,
             std::istream& in, std::ostream& out, std::ostream& err);
}
