#pragma once

#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"

namespace a {

// Compile-time metaprogramming pass. Runs after ModuleLoader::load and before
// Sema::analyze so every backend (LLVM IR, VM) sees ordinary specialized
// functions:
//
//  * Call-site specialization: `fn f(comptime N: int, x: int)` called as
//    `f(4, x)` produces a cloned `FnDecl` named `f__ct<k>` with the comptime
//    parameters removed and every use of `N` in the body replaced by the
//    compile-time literal `4`. The clone is appended to the declaring
//    module's statement list (so Sema registers and the backends emit it
//    like any other function) and the call site is rewritten to pass only
//    the runtime arguments.
//  * `comptime if COND:` evaluates COND at compile time (pure literal
//    arithmetic; comptime parameters have already been substituted) and
//    splices in only the taken branch. An unevaluable condition degrades to
//    a regular runtime `if`.
//  * `comptime for i in <literal iterable>:` unrolls the loop at compile
//    time, substituting `i` with each element's literal. Unevaluable
//    iterables degrade to a runtime `for`.
//
// All `comptime` flags are cleared during the pass, so Sema and the backends
// never see them.
void runComptimePass(ModuleGraph& graph, DiagnosticSink& sink);

}  // namespace a
