#include "type-checker/sema_internal.hpp"

namespace a {
// ---------------------------------------------------------------- analyze

void Sema::registerImports(Module& module) {
  auto aliasStem = [](const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
  };

  for (auto& statement : module.program->statements) {
    if (auto* import = dynamic_cast<ImportStmt*>(statement.get())) {
      std::string alias = import->alias;
      if (alias.empty()) {
        if (import->quoted) alias = aliasStem(import->path);
        else if (import->pythonModuleImport) {
          alias = import->pythonModule;
          std::size_t dot = alias.rfind('.');
          if (dot != std::string::npos) alias = alias.substr(dot + 1);
        } else alias = import->bareName;
      }
      if (alias.empty()) continue;

      ModuleAlias entry;
      entry.native = import->nativeModule;
      entry.python = import->pythonModuleImport;
      entry.moduleId = import->moduleId;
      entry.nativeName = import->bareName;
      entry.pythonName = import->pythonModule;
      if (!entry.native && !entry.python && entry.moduleId < 0) continue;

      if (moduleAliases_.count(alias)) {
        sink_.error(import->span, "Duplicate import alias '" + alias + "'.");
        continue;
      }
      moduleAliases_[alias] = entry;
      continue;
    }

    if (auto* from = dynamic_cast<FromImportStmt*>(statement.get())) {
      for (auto& item : from->items) {
        FromBinding binding;
        binding.span = item.span;
        binding.moduleId = from->moduleId;

        if (from->pythonModuleImport) {
          binding.python = true;
          binding.kind = FromBinding::Kind::Function; // dynamic Python member/callable at runtime
          binding.pythonModule = from->pythonModule;
          binding.pythonMember = item.name;
          { auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; binding.type=t; }
        } else if (from->nativeModule) {
          binding.native = true;
          if (const std::string* fn = nativeFunction(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Function;
            binding.nativeName = *fn;
          } else if (const std::string* constant = nativeConstant(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Value;
            binding.nativeName = *constant;
            binding.type = builtinConstantType(*constant);
            if (!binding.type) {
              sink_.error(item.span, "Unsupported native constant '" + item.name + "'.");
              continue;
            }
          } else {
            sink_.error(item.span,
                        "Module '" + from->bareName + "' has no member '" + item.name + "'.");
            continue;
          }
        } else if (from->moduleId >= 0) {
          ModuleExports& exports = exportsFor(from->moduleId);
          if (auto fn = exports.functions.find(item.name); fn != exports.functions.end()) {
            binding.kind = FromBinding::Kind::Function;
            binding.fn = fn->second;
            auto sig = signatures_.find(binding.fn);
            if (sig != signatures_.end()) {
              binding.type = typeFunction(sig->second.paramTypes, sig->second.retType);
            }
          } else if (auto st = exports.structs.find(item.name); st != exports.structs.end()) {
            binding.kind = FromBinding::Kind::Struct;
            binding.decl = st->second;
            // The struct may be re-exported by a package __init__, so qualify it
            // with the module that actually declared it.
            const auto owning = structModules_.find(st->second);
            const int ownerId = owning != structModules_.end() ? owning->second : from->moduleId;
            binding.structName = qualifiedFor(ownerId, st->second->name);
            binding.type = typeStruct(binding.structName);
          } else if (auto tr = exports.traits.find(item.name); tr != exports.traits.end()) {
            binding.kind = FromBinding::Kind::Trait;
            binding.traitName = tr->second;
          } else if (auto value = exports.values.find(item.name); value != exports.values.end()) {
            binding.kind = FromBinding::Kind::Value;
            binding.type = value->second;
          } else {
            // Value exports are only built by checkModule, which runs after
            // registerImports for every module. Fall back to scanning the
            // target module's top-level assignments so `from m import X`
            // works for plain `X = ...` bindings; the type stays dynamic
            // until the dependency's own checkModule refines it.
            Module& target = graph_.at(from->moduleId);
            bool found = false;
            if (target.program) {
              for (auto& stmt : target.program->statements) {
                if (auto* asg = dynamic_cast<AssignStmt*>(stmt.get())) {
                  if (auto* id = dynamic_cast<IdentExpr*>(asg->target.get())) {
                    if (id->name == item.name) { found = true; break; }
                  }
                } else if (auto* dst = dynamic_cast<DestructureStmt*>(stmt.get())) {
                  for (const auto& n : dst->names) {
                    if (n == item.name) { found = true; break; }
                  }
                  if (found) break;
                }
              }
            }
            if (!found) {
              sink_.error(item.span, "Module '" + exports.name + "' has no exported member '" +
                                         item.name + "'.");
              continue;
            }
            binding.kind = FromBinding::Kind::Value;
            auto t = std::make_shared<Type>();
            t->kind = TypeKind::Var;
            t->allowUnresolved = true;
            binding.type = t;
          }
        } else {
          continue;
        }

        if (fromBindings_.count(item.alias)) {
          sink_.error(item.span, "Duplicate imported name '" + item.alias + "'.");
          continue;
        }
        if (binding.kind == FromBinding::Kind::Struct) {
          structAliases_[item.alias] = binding.structName;
        }
        fromBindings_[item.alias] = std::move(binding);
      }
    }
  }
}

void Sema::buildModuleExports(Module& module) {
  ModuleExports& exports = exportsFor(module.id);
  if (!scopes_.empty()) {
    for (const auto& entry : scopes_.front()) {
      exports.values[entry.first] = entry.second.type;
    }
  }
  exports.structAliases = structAliases_;
}

SemaResult Sema::analyze() {
  for (int id : graph_.order) registerModule(graph_.at(id));
  for (int id : graph_.order) checkModule(graph_.at(id));

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  result.enums = enums_;
  result.modules = moduleExports_;
  result.fnModule = fnModules_;
  result.structModule = structModules_;
  result.fnParamTypes = fnParamTypes_;
  result.fnReturnTypes = fnReturnTypes_;
  for (const DeclaredVarInfo& info : declaredVariables_) {
    result.variables.push_back(VarTypeInfo{info.name, info.span, info.type});
  }
  // Mirror Expr::inferredType into the result map for LSP clients.
  result.exprTypes = exprTypes_;
  return result;
}

SemaResult Sema::analyzeParallel(int jobs) {
  for (int id : graph_.order) registerModule(graph_.at(id));
  if (jobs <= 0) jobs = 4;
  const std::size_t n = graph_.order.size();
  if (jobs <= 1 || n <= 1) {
    for (int id : graph_.order) checkModule(graph_.at(id));
  } else {
    // Each worker clones the registered tables and checks a strided subset;
    // per-worker outputs are merged under a lock. Diagnostics are already
    // thread-safe; unify is serialised inside Sema via a global lock.
    std::mutex mergeMutex;
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    int threads = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(jobs), n));
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([this, &next, &mergeMutex, t, threads] {
        Sema worker(graph_, sink_);
        worker.functionsByName_ = functionsByName_;
        worker.structsByName_ = structsByName_;
        worker.traits_ = traits_;
        worker.traitImpls_ = traitImpls_;
        worker.enums_ = enums_;
        worker.signatures_ = signatures_;
        worker.structs_ = structs_;
        worker.fnModules_ = fnModules_;
        worker.structModules_ = structModules_;
        worker.moduleStates_ = moduleStates_;
        worker.moduleExports_ = moduleExports_;
        worker.nextVarId_ = nextVarId_;
        worker.typeVariables_ = typeVariables_;
        worker.typeAliases_ = typeAliases_;
        while (true) {
          std::size_t i = next.fetch_add(1);
          if (i >= graph_.order.size()) break;
          int id = graph_.order[i];
          worker.checkModule(graph_.at(id));
        }
        std::lock_guard<std::mutex> lock(mergeMutex);
        for (auto& entry : worker.fnParamTypes_) fnParamTypes_[entry.first] = std::move(entry.second);
        for (auto& entry : worker.fnReturnTypes_) fnReturnTypes_[entry.first] = std::move(entry.second);
        for (auto& entry : worker.exprTypes_) exprTypes_[entry.first] = std::move(entry.second);
        declaredVariables_.insert(declaredVariables_.end(), worker.declaredVariables_.begin(),
                                  worker.declaredVariables_.end());
        // typeVariables_ already holds registration vars in the base; workers
        // only defaulted them in place (via checkTypeVariables), so do not
        // merge copies. Body-created vars are already linked into Type
        // objects referenced by fnParamTypes_/exprTypes_.
        checkedFunctions_.insert(checkedFunctions_.end(), worker.checkedFunctions_.begin(),
                                 worker.checkedFunctions_.end());
        // Merge module exports (values) per module id.
        for (std::size_t m = 0; m < worker.moduleExports_.size() && m < moduleExports_.size(); ++m) {
          for (auto& entry : worker.moduleExports_[m].values) {
            moduleExports_[m].values[entry.first] = entry.second;
          }
        }
      });
    }
    for (auto& w : workers) w.join();
  }

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  result.enums = enums_;
  result.modules = moduleExports_;
  result.fnModule = fnModules_;
  result.structModule = structModules_;
  result.fnParamTypes = fnParamTypes_;
  result.fnReturnTypes = fnReturnTypes_;
  for (const DeclaredVarInfo& info : declaredVariables_) {
    result.variables.push_back(VarTypeInfo{info.name, info.span, info.type});
  }
  result.exprTypes = exprTypes_;
  return result;
}

void Sema::analyzeModule(Module& module) {
  registerModule(module);
  checkModule(module);
}

std::shared_ptr<Sema::AsyncHandle> Sema::analyzeAsync(int jobs) {
  auto handle = std::make_shared<AsyncHandle>();
  // Move a clone that owns a copy of the graph/sink references; the caller
  // keeps its graph alive until wait() is called.
  ModuleGraph& graph = graph_;
  DiagnosticSink& sink = sink_;
  handle->future = std::async(std::launch::async, [&graph, &sink, jobs] {
    Sema worker(graph, sink);
    return worker.analyzeParallel(jobs);
  });
  return handle;
}

void Sema::registerModule(Module& module) {
  currentModule_ = module.id;
  scopes_.clear();
  scopes_.push_back({});
  moduleAliases_.clear();
  fromBindings_.clear();
  structAliases_.clear();
  typeAliases_.clear();
  genericParams_.clear();
  loopDepth_ = 0;
  deferDepth_ = 0;
  currentFunction_ = nullptr;
  currentReturnType_ = nullptr;
  sawReturn_ = false;
  // typeVariables_ accumulates across all registrations (signature vars);
  // per-module check vectors are cleared in checkModule() instead.

  registerImports(module);

  ModuleExports& exports = exportsFor(module.id);
  exports.name = module.name;
  auto& statements = module.program->statements;

  // Re-export imported names, so package `__init__.a` files can aggregate
  // their submodules (`from "collections.a" import map_list`). Locally
  // declared names below take precedence on collision.
  for (const auto& entry : fromBindings_) {
    const FromBinding& binding = entry.second;
    switch (binding.kind) {
      case FromBinding::Kind::Function:
        if (!binding.native && binding.fn) exports.functions[entry.first] = binding.fn;
        break;
      case FromBinding::Kind::Struct:
        if (binding.decl) exports.structs[entry.first] = binding.decl;
        break;
      case FromBinding::Kind::Trait:
        exports.traits[entry.first] = binding.traitName;
        break;
      case FromBinding::Kind::Value:
        if (binding.type) exports.values[entry.first] = binding.type;
        break;
    }
  }

  // Pass 1: collect declaration names so duplicates and collisions are clear.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      const std::string q = qualified(decl->name);
      if (structsByName_.count(q) || functionsByName_.count(q)) {
        sink_.error(decl->nameSpan, "Duplicate declaration of '" + decl->name + "'.");
        continue;
      }
      StructSym sym;
      sym.name = q;
      sym.decl = decl;
      structs_[q] = std::move(sym);
      structsByName_[q] = decl;
      structModules_[decl] = currentModule_;
      exports.structs[decl->name] = decl;
    } else if (auto* en = dynamic_cast<EnumDecl*>(stmt.get())) {
      const std::string q = qualified(en->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q)) {
        sink_.error(en->nameSpan, "Duplicate declaration of '" + en->name + "'.");
        continue;
      }
      exports.enums[en->name] = q;
      registerEnum(*en);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q) || typeAliases_.count(q)) {
        sink_.error(ta->nameSpan, "Duplicate declaration of '" + ta->name + "'.");
        continue;
      }
    } else if (auto* tr = dynamic_cast<TraitDecl*>(stmt.get())) {
      const std::string q = qualified(tr->name);
      if (traits_.find(q) != traits_.end() || functionsByName_.count(q) || structsByName_.count(q)) {
        sink_.error(tr->nameSpan, "Duplicate declaration of '" + tr->name + "'.");
        continue;
      }
      traits_[q] = tr;
      exports.traits[tr->name] = q;
    } else if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
      const std::string q = qualified(fn->name);
      if (functionsByName_.count(q) || structsByName_.count(q)) {
        sink_.error(fn->nameSpan, "Duplicate declaration of '" + fn->name + "'.");
        continue;
      }
      functionsByName_[q] = fn;
      fnModules_[fn] = currentModule_;
      fn->moduleId = currentModule_;
      exports.functions[fn->name] = fn;
    }
  }

  // Pass 2: resolve struct layouts and method signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      registerStructLayout(*decl);
      registerMethodSignatures(*decl);
    } else if (auto* en = dynamic_cast<EnumDecl*>(stmt.get())) {
      resolveEnumPayloads(*en);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      typeAliases_[q] = resolveTypeExpr(*ta->type);
    }
  }

  // Pass 2b: traits and conformance. Traits are already registered by name in
  // pass 1; struct layouts exist after pass 2, so `impl T for S` can be checked
  // against the struct's method table.
  for (auto& stmt : statements) {
    if (auto* tr = dynamic_cast<TraitDecl*>(stmt.get())) {
      if (traits_.count(qualified(tr->name)) && traits_[qualified(tr->name)] == tr) {
        checkTraitDecl(*tr);
      }
    } else if (auto* impl = dynamic_cast<ImplDecl*>(stmt.get())) {
      checkImpl(*impl);
    }
  }

  // Pass 3: function signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      registerFunctionSignature(*decl);
      checkWhereClause(decl->whereClause, decl->whereSpan,
                       "function '" + decl->name + "'");
    }
  }

  // Snapshot import/alias bindings for the parallel body-check phase.
  ModuleCheckState state;
  state.moduleAliases = moduleAliases_;
  state.fromBindings = fromBindings_;
  state.structAliases = structAliases_;
  state.typeAliases = typeAliases_;
  moduleStates_[module.id] = std::move(state);
}

void Sema::checkModule(Module& module) {
  currentModule_ = module.id;
  auto foundState = moduleStates_.find(module.id);
  if (foundState != moduleStates_.end()) {
    moduleAliases_ = foundState->second.moduleAliases;
    fromBindings_ = foundState->second.fromBindings;
    structAliases_ = foundState->second.structAliases;
    typeAliases_ = foundState->second.typeAliases;
  }
  scopes_.clear();
  scopes_.push_back({});
  genericParams_.clear();
  loopDepth_ = 0;
  deferDepth_ = 0;
  currentFunction_ = nullptr;
  currentReturnType_ = nullptr;
  sawReturn_ = false;
  // typeVariables_ holds signature vars from registration (all modules);
  // keep it so checkTypeVariables() defaults every var once.
  declaredVariables_.clear();
  checkedFunctions_.clear();

  auto& statements = module.program->statements;

  // Pass 4: top-level statements (global variables and executable code).
  for (auto& stmt : statements) {
    if (dynamic_cast<FnDecl*>(stmt.get()) || dynamic_cast<StructDecl*>(stmt.get()) ||
        dynamic_cast<TraitDecl*>(stmt.get()) || dynamic_cast<ImplDecl*>(stmt.get()) ||
        dynamic_cast<EnumDecl*>(stmt.get()) ||
        dynamic_cast<ImportStmt*>(stmt.get()) || dynamic_cast<FromImportStmt*>(stmt.get())) {
      continue;
    }
    checkStatement(*stmt);
  }

  // Pass 5: function and method bodies.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      checkStructMethodBodies(*decl);
    }
  }
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      auto sig = signatures_.find(decl);
      if (sig == signatures_.end()) continue;
      checkFunctionBody(*decl, sig->second);
    }
  }

  checkTypeVariables();
  checkDeclaredVariables();
  checkReturnPaths();

  buildModuleExports(module);
}

void Sema::checkModuleBodies(Module& module) { checkModule(module); }

}  // namespace a
