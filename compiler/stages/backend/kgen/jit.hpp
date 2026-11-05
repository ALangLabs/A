#pragma once
// In-process JIT backend (Goose-style): the C emitter's output is compiled by
// the vendored TinyCC directly into this process and its main() is called —
// no subprocess, no linker, ~1ms instead of the ~50ms gcc round-trip.
#include <iosfwd>
#include <string>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a::kgen {

// Returns the JIT-compiled program's exit code, or -1 when the JIT is
// unavailable or compilation fails — callers should then fall back to the
// cached-native or interpreter paths.
int runJitModule(const ModuleGraph& graph, const SemaResult& sema, std::ostream& err);

}  // namespace a::kgen
