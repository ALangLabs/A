#pragma once
// KGEN-style backend for A (adapted from Mojo ref/Compiler/{Pipeline,ObjectCompiler,KGENCompiler} architecture).
// Pipeline: A frontend/sema -> A IR/ownership -> KGEN lowering -> LLVM IR text/module -> object/binary.
// Note: Full MLIR/KGEN dialect integration requires MLIR/LLVM dev deps; this implementation
// provides the KGEN-structured pipeline and LLVM text/asm emission path for A.
#include <iosfwd>
#include <string>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a::kgen {
bool emitLLVMIR(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);
bool buildExecutable(const ModuleGraph& graph, const SemaResult& sema, const std::string& outputPath, std::ostream& err);
int runModule(const ModuleGraph& graph, const SemaResult& sema, std::istream& in, std::ostream& out, std::ostream& err);
bool compileNativeExecutable(const ModuleGraph& graph, const SemaResult& sema,
                             const std::string& outputBase, std::ostream& err);
int runNativeExecutable(const std::string& outputBase, std::istream& in, std::ostream& out,
                        std::ostream& err);
bool isSupported(const ModuleGraph& graph);
}
