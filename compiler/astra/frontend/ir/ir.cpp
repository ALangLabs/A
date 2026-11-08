#include "ir/ir.hpp"
#include "ir/ir_internal.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "name-resolution/module_loader.hpp"
#include "strings/utf8.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace ir {

TypePtr defaultType(const TypePtr& type) {
  TypePtr resolved = resolveType(type);
  if (!resolved) return nullptr;
  if (resolved->kind != TypeKind::Var) return resolved;
  if (resolved->constraint == Constraint::Integer) return typeInt(64, true);
  if (resolved->constraint == Constraint::Floating) return typeFloat(64);
  return resolved;
}


Program lowerProgram(const ModuleGraph& graph, const SemaResult& sema) {
  Program program;

  auto functionName = [&](const FnDecl& decl) {
    auto module = sema.fnModule.find(&decl);
    if (module == sema.fnModule.end()) return decl.name;
    return graph.at(module->second).name + "::" + decl.name;
  };

  for (const auto& module : graph.modules) {
    if (!module.program) continue;
    for (const auto& stmt : module.program->statements) {
      const auto* decl = dynamic_cast<const FnDecl*>(stmt.get());
      if (!decl) continue;
      Function function;
      function.name = functionName(*decl);
      function.decl = decl;
      auto params = sema.fnParamTypes.find(decl);
      for (std::size_t i = 0; i < decl->params.size(); ++i) {
        Parameter parameter;
        parameter.name = decl->params[i].name;
        if (params != sema.fnParamTypes.end() && i < params->second.size()) {
          parameter.type = defaultType(params->second[i]);
        }
        function.params.push_back(std::move(parameter));
      }
      auto ret = sema.fnReturnTypes.find(decl);
      function.returnType = ret != sema.fnReturnTypes.end() ? defaultType(ret->second) : nullptr;
      detail::Builder builder(function);
      builder.lowerParameters();
      builder.lowerBody(decl->body);
      program.functions.push_back(std::move(function));
    }
  }

  if (graph.entry >= 0 && graph.at(graph.entry).program) {
    Function entry;
    entry.name = "main";
    entry.isEntry = true;
    entry.returnType = typeVoid();
    detail::Builder builder(entry);
    builder.lowerParameters();
    builder.lowerTopLevel(graph.at(graph.entry).program->statements);
    program.functions.push_back(std::move(entry));
  }
  return program;
}

}  // namespace ir
}  // namespace a
