#pragma once
// Internal execution engine for dynamic A semantics (test/run support while KGEN lowering matures).
#include <iosfwd>
#include "ast/ast.hpp"
#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a::kgen {
int runWithEngine(const ModuleGraph& g, const SemaResult& s, DiagnosticSink& sink, std::istream& in, std::ostream& out, std::ostream& err);
int testWithEngine(const ModuleGraph& g, const SemaResult& s, DiagnosticSink& sink, std::istream& in, std::ostream& out, std::ostream& err);
}
