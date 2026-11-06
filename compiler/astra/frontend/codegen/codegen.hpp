#pragma once
// C code generation from the frontend AST. This replaces the retired
// compiler/astra/a/ C frontend: the C++ frontend (lex/parse/sema) is the
// single source of truth, and its output is plain C that calls the public
// runtime API in compiler/astra/runtime/a_runtime.h. Astra (the TinyCC fork)
// then compiles that C to machine code like any other C file.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Emits x86-64 assembly (AT&T syntax) for astra to assemble directly to
// machine code — the only codegen backend (A -> asm -> machine code).
// In test mode, `main` discovers and runs top-level `fn test_*()` functions
// through the runtime a_test_main harness. Returns false on constructs the
// asm backend does not cover.
bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode = false);
}  // namespace a
