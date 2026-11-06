#pragma once
// Builtin native backend: the sole production compiler for A.
//
// A -> emitCSource() (C99, aval tagged-union ABI) -> builtin native
// compiler (compiler/astra/) -> EXE output -> standalone ELF/PE/Mach-O.
// No subprocess, no external compiler is ever invoked.
#include <iosfwd>
#include <string>

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a::kgen {

// Builds a standalone executable at outputPath using only the builtin
// native compiler (compiler/astra/). Returns true on success.
bool buildExecutableWithNative(const ModuleGraph& graph, const SemaResult& sema,
                               const std::string& outputPath, std::ostream& err);

}  // namespace a::kgen
