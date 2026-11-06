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

// Emits a complete C translation unit (including `int main`) for a checked
// program. Returns false and fills `error` on unsupported constructs.
// In test mode, `main` discovers and runs top-level `fn test_*()` functions.
bool emitC(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
           std::string& error, bool testMode = false);
}  // namespace a
