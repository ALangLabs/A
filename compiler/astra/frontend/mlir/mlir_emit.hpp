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
}  // namespace a
