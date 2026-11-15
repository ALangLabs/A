#include "ownership/ownership_internal.hpp"

#include "ast/ast.hpp"

namespace a::detail {

void Analyzer::analyzeModule(const Module& module) {
    reset();
    scopes_.emplace_back();
    for (const auto& stmt : module.program->statements) {
      if (dynamic_cast<const FnDecl*>(stmt.get()) ||
          dynamic_cast<const StructDecl*>(stmt.get()) ||
          dynamic_cast<const EnumDecl*>(stmt.get()) ||
          dynamic_cast<const TypeAliasDecl*>(stmt.get()) ||
          dynamic_cast<const ImportStmt*>(stmt.get()) ||
          dynamic_cast<const FromImportStmt*>(stmt.get())) {
        continue;
      }
      analyzeStmt(*stmt);
    }
    scopes_.pop_back();
  }

void Analyzer::analyzeFunction(const FnDecl& decl) {
    reset();
    scopes_.emplace_back();
    for (const auto& param : decl.params) declare(param.name);
    analyzeStatements(decl.body);
    scopes_.pop_back();
  }

}  // namespace a::detail

namespace a {

void analyzeOwnership(const ModuleGraph& graph, DiagnosticSink& sink) {
  for (const auto& module : graph.modules) {
    if (!module.program) continue;
    detail::Analyzer analyzer(sink);
    analyzer.analyzeModule(module);
    for (const auto& stmt : module.program->statements) {
      if (const auto* fn = dynamic_cast<const FnDecl*>(stmt.get())) {
        detail::Analyzer functionAnalyzer(sink);
        functionAnalyzer.analyzeFunction(*fn);
      }
    }
  }
}

}  // namespace a
