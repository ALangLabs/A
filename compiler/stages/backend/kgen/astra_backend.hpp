#pragma once
// Astra backend: the sole production native compiler for A.
//
// A -> emitCSource() (C99, aval tagged-union ABI) -> Astra
// (compiler/astra/, builtin TinyCC fork) -> EXE or in-process JIT.
// No LLVM, MLIR, x86-asm emitter, or external compiler is involved.
#include <iosfwd>
#include <string>

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a {

// Emits C99 source for the native subset (consumed by Astra).
bool emitCSource(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);

// Compiles the module graph to a native binary via Astra.
bool buildNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& outputPath, std::ostream& err);

// Compiles via the Astra pipeline; alias kept for call-site clarity.
bool compileNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                         const std::string& outputBase, std::ostream& err);

// Executes a binary produced by buildNativeModule/compileNativeModule.
int runBuiltNativeModule(const std::string& outputBase, std::istream& in, std::ostream& out,
                         std::ostream& err);

// Compiles via Astra and executes the resulting program directly.
int runNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                    std::istream& in, std::ostream& out, std::ostream& err);

// Returns true if the AST contains only constructs the Astra native subset
// can lower (everything else falls back to the interpreter engine).
bool isNativeSupported(const ModuleGraph& graph);

}  // namespace a
