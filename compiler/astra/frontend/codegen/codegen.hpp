#pragma once
// x86-64 assembly code generation from the frontend AST. The C++ frontend
// (lex/parse/sema/ownership) lowers A to AT&T-syntax assembly; the runtime ABI
// lives in compiler/astra/runtime/a_runtime.h and generated code calls the
// a_* runtime helpers (compiled from compiler/astra/runtime/a_runtime.c by the
// Astra backend). The pipeline hands the staged .s and runtime source to the
// `astra` driver to produce a native executable.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Emits x86-64 assembly (AT&T syntax) for astra to assemble/compile and link
// into machine code — the only codegen backend (A -> asm -> machine code).
// In test mode, `main` discovers and runs top-level `fn test_*()` functions
// through the runtime a_test_main harness. Returns false on constructs the
// asm backend does not cover.
bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode = false);
}  // namespace a
