#pragma once
// v2 pipeline for A (single LLVM-oriented backend; architecture guided by compiler/ref/Compiler/Pipeline).
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
#include "backend/kgen/execution_engine_impl.hpp"
namespace a {
class DiagnosticSink; struct ModuleGraph; struct SemaResult; namespace ir { struct Program; }
struct PipelineConfig {
  bool checkOnly = false, jsonDiagnostics = false, useColor = false, optimize = false;
  int optimizationLevel = 2;
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
};
struct CompiledModule { std::unique_ptr<ModuleGraph> graph; std::unique_ptr<SemaResult> sema; bool ok = false; };
CompiledModule runFrontendPipeline(const std::string& path, std::string source, const PipelineConfig& config, DiagnosticSink& sink, std::ostream& diagOut, std::ostream& diagErr);
int executePipeline(const std::string& path, std::string source, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int testPipeline(const std::string& path, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err);
int buildPipeline(const std::string& path, const std::string& outputPath, const PipelineConfig& config, std::ostream& err);
int emitLLVMPipeline(const std::string& path, const std::string& outputPath, const PipelineConfig& config, std::ostream& err);
int emitIRPipeline(const std::string& path, const PipelineConfig& config, bool optimize, std::ostream& out, std::ostream& err);

// Interactive REPL session. The driver feeds the full accumulated source on
// every call; the session recompiles it and executes only the statements
// added since the previous call, so bindings persist like a Python REPL.
// Frontend failures leave the session untouched (the driver discards the
// rejected input); runtime errors mark the input consumed.
struct ReplSession {
  ~ReplSession();
  ReplSession() = default;
  ReplSession(const ReplSession&) = delete;
  ReplSession& operator=(const ReplSession&) = delete;
  std::unique_ptr<ModuleGraph> graph;
  std::unique_ptr<SemaResult> sema;
  std::unique_ptr<class KGENExecutionEngine> engine;
  std::unique_ptr<DiagnosticSink> sink;  // runtime-error diagnostics land here
  bool primed = false;
};
// Returns 0 on success, 1 on frontend (compile) failure, 2 on runtime error.
int replEval(ReplSession& session, const std::string& source, const PipelineConfig& config,
             std::istream& in, std::ostream& out, std::ostream& err);

// Runs the self-hosted A REPL UI (the `stdlib/std/repl` package entry file)
// in the interpreter. Registers the `repl.eval` bridge first so the UI can
// evaluate user input in a persistent session.
int runReplUi(const std::string& entryPath, const PipelineConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err);
}
