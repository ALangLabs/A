// x86-64 assembly backend (A -> .s -> astra). Values are aval words passed
// in %rax / held in rbp-relative frame slots; runtime calls use the *_slow
// externs from a_runtime.h (the header fast paths are C-only). Constructs
// outside the covered subset fail emission (no C fallback exists).
#include "codegen/codegen.hpp"
#include "codegen/codegen_emitter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

namespace a::detail {

bool AsmEmitter::run(std::string& out, std::string& error) {
    error_ = &error;
    collectGlobals();
    buildGlobalScope();
    for (StructInfo* st : structList_) emitStructCtor(st);
    for (FnInfo* f : fnList_) {
      if (!failed_) emitFunction(f);
    }
    emitMain();
    // Lambda bodies are emitted after everything else so nested lambdas
    // discovered during their own emission are drained too.
    while (!lambdas_.empty()) {
      PendingLambda pl = std::move(lambdas_.back());
      lambdas_.pop_back();
      if (!failed_) emitLambdaFn(pl);
    }
    if (failed_) return false;
    out = bss_.str() + rodata_.str() + text_.str();
    return true;
  }

}  // namespace a::detail

namespace a {

bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode) {
  detail::AsmEmitter emitter(graph, sema, testMode);
  return emitter.run(out, error);
}

}  // namespace a
