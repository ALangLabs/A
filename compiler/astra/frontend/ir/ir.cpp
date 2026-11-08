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

const char* opName(Op op) {
  switch (op) {
    case Op::ConstInt: return "const";
    case Op::ConstFloat: return "const";
    case Op::ConstBool: return "const";
    case Op::ConstChar: return "const";
    case Op::ConstString: return "const";
    case Op::Param: return "param";
    case Op::Phi: return "phi";
    case Op::Copy: return "copy";
    case Op::Binary: return "binary";
    case Op::Unary: return "unary";
    case Op::Select: return "select";
    case Op::Call: return "call";
    case Op::CallValue: return "call_value";
    case Op::Index: return "index";
    case Op::Slice: return "slice";
    case Op::Field: return "field";
    case Op::Array: return "array";
    case Op::Tuple: return "tuple";
    case Op::Map: return "map";
    case Op::Set: return "set";
    case Op::Range: return "range";
    case Op::StringConcat: return "concat";
    case Op::IterInit: return "iter_init";
    case Op::IterHasNext: return "iter_has_next";
    case Op::IterNext: return "iter_next";
    case Op::TupleGet: return "tuple_get";
    case Op::StoreIndex: return "store_index";
    case Op::StoreField: return "store_field";
    case Op::Global: return "global";
    case Op::FuncRef: return "func";
    case Op::Lambda: return "lambda";
    case Op::Cast: return "cast";
    case Op::GetElementPtr: return "getelementptr";
    case Op::ExtractValue: return "extractvalue";
    case Op::InsertValue: return "insertvalue";
    case Op::Freeze: return "freeze";
    case Op::Fma: return "fma";
    case Op::Opaque: return "opaque";
  }
  return "?";
}

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
