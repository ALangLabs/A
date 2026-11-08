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

// Follow `from "x.a" import name` re-exports (package __init__ modules) to
// the module that actually declares `name`. Returns {moduleId, name};
// unchanged when there is no re-export. Native/python re-exports are not
// followed (shortcut: module values have no runtime representation).
// Shared by emitC (Emitter::resolveModuleMember) and emitAsm.
std::pair<int, std::string> resolveModuleMember(const ModuleGraph& graph,
                                              int moduleId,
                                              const std::string& name) {
  std::string cur = name;
  for (int depth = 0; moduleId >= 0 && depth < 16; ++depth) {
    if (moduleId >= (int)graph.modules.size()) break;
    const Module& mod = graph.at(moduleId);
    if (!mod.program) break;
    bool declared = false;
    for (const StmtPtr& s : mod.program->statements) {
      if ((s->kind == StmtKind::FnDecl &&
           static_cast<const FnDecl*>(s.get())->name == cur) ||
          (s->kind == StmtKind::StructDecl &&
           static_cast<const StructDecl*>(s.get())->name == cur) ||
          (s->kind == StmtKind::EnumDecl &&
           static_cast<const EnumDecl*>(s.get())->name == cur) ||
          (s->kind == StmtKind::Let &&
           static_cast<const LetStmt*>(s.get())->name == cur)) {
        declared = true;
        break;
      }
    }
    if (declared) break;
    int nextId = -1;
    std::string nextName;
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind != StmtKind::FromImport) continue;
      const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
      if (im->nativeModule || im->pythonModuleImport || im->moduleId < 0)
        continue;
      for (const auto& item : im->items) {
        // `import x as y` re-exports under the alias; recurse on the
        // source name in the source module.
        if (item.name == cur || (!item.alias.empty() && item.alias == cur)) {
          nextId = im->moduleId;
          nextName = item.name;
          break;
        }
      }
      if (nextId >= 0) break;
    }
    if (nextId < 0) break;
    moduleId = nextId;
    cur = nextName;
  }
  return {moduleId, cur};
}

bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode) {
  detail::AsmEmitter emitter(graph, sema, testMode);
  return emitter.run(out, error);
}

}  // namespace a
