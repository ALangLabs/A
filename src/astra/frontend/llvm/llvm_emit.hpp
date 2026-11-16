#pragma once
// LLVM IR backend (A -> .ll -> opt/llc/clang -> machine code). Values are
// aval words (i64) exactly like the C runtime ABI: every A function has the
// uniform signature `i64 f(ptr argv)` with argv[0] = closure env
// (A_NONE for plain functions) and argv[1..n] = parameters; all semantics go
// through the a_* runtime entry points in compiler/astra/runtime/. LLVM's
// optimizer (mem2reg, instcombine, loop opts, ...) replaces the retired
// hand-rolled SSA IR + optimizer.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Emits one LLVM IR module (.ll text) covering every module in the graph.
// Returns false and fills `error` on constructs not yet lowered.
bool emitLLVM(const ModuleGraph& graph, const SemaResult& sema,
              std::string& out, std::string& error, bool testMode = false,
              const std::string* testFilter = nullptr);
}  // namespace a
