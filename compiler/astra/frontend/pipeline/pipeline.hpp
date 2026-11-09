#pragma once
// v2 pipeline for A. The C++ frontend (lex/parse/sema/ownership/codegen/IR)
// provides `check`, `fmt`, `doc`, and `ir`; execution and native builds emit
// C (frontend/codegen) and delegate compilation to Astra
// (compiler/astra/), the builtin Astra-fork toolchain driver.
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a {
class DiagnosticSink; struct ModuleGraph; struct SemaResult; namespace ir { struct Program; }
struct PipelineConfig {
  bool checkOnly = false, jsonDiagnostics = false, useColor = false, optimize = false;
  int optimizationLevel = 3;
  int jobs = 1;  // 1 = serial; >1 = parallel module checking (ty/pyrefly style)
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  // Cross-compilation target (e.g. "i386-linux-gnu", "aarch64", "native").
  // Empty means the host default. Only the architecture is selectable:
  // astra binaries are bound to the host OS, so a target OS differing from
  // the host is rejected.
  std::string target;
  // `a test --filter <substr>`: only run test_* functions whose name
  // contains this substring. Empty means run every test.
  std::string testFilter;
};
struct CompiledModule { std::unique_ptr<ModuleGraph> graph; std::unique_ptr<SemaResult> sema; bool ok = false; };
CompiledModule runFrontendPipeline(const std::string& path, std::string source, const PipelineConfig& config, DiagnosticSink& sink, std::ostream& diagOut, std::ostream& diagErr);
int executePipeline(const std::string& path, std::string source, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int testPipeline(const std::string& path, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int buildPipeline(const std::string& path, const std::string& outputPath, const PipelineConfig& config, std::ostream& err);
int emitIRPipeline(const std::string& path, const PipelineConfig& config, bool optimize, std::ostream& out, std::ostream& err);

// Interactive REPL session. The driver feeds the full accumulated source on
// every call; each eval is compiled and run by Astra as a fresh process, so
// top-level bindings do not persist between inputs.
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
