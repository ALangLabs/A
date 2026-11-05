#pragma once
// v2 pipeline for A (single LLVM-oriented backend; architecture guided by compiler/ref/Compiler/Pipeline).
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
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
}
