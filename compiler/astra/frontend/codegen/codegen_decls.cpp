#include "codegen/codegen_emitter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

namespace a::detail {

std::string AsmEmitter::moduleTag(int moduleId) const {
  auto it = moduleTags_.find(moduleId);
  if (it != moduleTags_.end()) return it->second;
  std::string path;
  if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
    path = graph_.at(moduleId).path;
  // Canonicalize weakly so `a ./x.a` and `a x.a` share a tag; fallback to
  // the display path when canonicalization fails.
  std::string canonical = path;
  {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(path, ec);
    if (!ec) canonical = c.string();
    if (canonical.empty()) canonical = path;
  }
  std::string tag = stableTag(canonical);
  moduleTags_[moduleId] = tag;
  return tag;
}
std::string AsmEmitter::modInitSym(int moduleId) {
  return "__a_modinit_" + moduleTag(moduleId);
}
StructInfo* AsmEmitter::findStructByName(const std::string& name) const {
  if (curModuleId_ >= 0) {
    for (StructInfo* st : structList_)
      if (st->name == name && st->moduleId == curModuleId_) return st;
  }
  for (StructInfo* st : structList_)
    if (st->name == name) return st;
  auto it = structs_.find(name);
  return it == structs_.end() ? nullptr : it->second;
}
const EnumVariantInfo* AsmEmitter::findEnumVariant(
    const std::string& enumName, const std::string& variant,
    int preferModule) const {
  int prefer = preferModule == -2 ? curModuleId_ : preferModule;
  const EnumVariantInfo* fallback = nullptr;
  for (const auto& kv : enums_) {
    const EnumVariantInfo& ev = kv.second;
    if (ev.enumName == enumName && ev.variantName == variant) {
      if (ev.moduleId == prefer) return &ev;
      if (!fallback) fallback = &ev;
    }
  }
  return fallback;
}
const EnumVariantInfo* AsmEmitter::findEnumByShort(
    const std::string& variant, int preferModule) const {
  int prefer = preferModule == -2 ? curModuleId_ : preferModule;
  const EnumVariantInfo* fallback = nullptr;
  for (const auto& kv : enums_) {
    const EnumVariantInfo& ev = kv.second;
    if (ev.variantName == variant) {
      if (ev.moduleId == prefer) return &ev;
      if (!fallback) fallback = &ev;
    }
  }
  return fallback;
}

// ---- registration (mirrors Emitter) --------------------------------------
void AsmEmitter::collectGlobals() {
    std::unordered_map<std::string, int> seenSym;  // base -> next suffix
    auto symWithSuffix = [&](const std::string& base) -> std::string {
      int n = seenSym[base]++;
      if (n == 0) return base;
      return base + "_" + std::to_string(n);
    };
    // Precompute module tags once per collect so suffix order is stable.
    for (const Module& mod : graph_.modules) {
      if (mod.id >= 0) (void)moduleTag(mod.id);
    }
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          FnInfo* info = new FnInfo();
          info->decl = fn;
          info->name = fn->name;
          info->moduleId = mod.id;
          {
            std::string mtag = moduleTag(mod.id);
            std::size_t arity = fn->comptimeParams.size() + fn->params.size();
            std::string dh = stableTag(fn->name + "#" + std::to_string(arity));
            info->cname = symWithSuffix("__a_f_" + mtag + "_" + dh);
            info->globalVar = symWithSuffix("__a_gf_" + mtag + "_" + dh);
          }
          for (const ComptimeParam& p : fn->comptimeParams) info->paramNames.push_back(p.name);
          for (const ParamDecl& p : fn->params) info->paramNames.push_back(p.name);
          info->variadic = !fn->params.empty() && fn->params.back().variadic;
          fns_[fn] = info;
          fnList_.push_back(info);
        } else if (s->kind == StmtKind::StructDecl) {
          const StructDecl* st = static_cast<const StructDecl*>(s.get());
          StructInfo* info = new StructInfo();
          info->decl = st;
          info->name = st->name;
          info->moduleId = mod.id;
          {
            std::string mtag = moduleTag(mod.id);
            std::string sh = stableTag(st->name);
            info->ctorCname = symWithSuffix("__a_ctor_" + mtag + "_" + sh);
            info->globalVar = symWithSuffix("__a_gs_" + mtag + "_" + sh);
          }
          for (const FieldDecl& f : st->fields) info->fieldNames.push_back(f.name);
          structs_[st->name] = info;
          structList_.push_back(info);
          for (const auto& m : st->methods) {
            FnInfo* mi = new FnInfo();
            mi->decl = m.get();
            mi->name = m->name;
            mi->moduleId = mod.id;
            mi->isMethod = true;
            mi->structName = st->name;
            {
              std::string mtag = moduleTag(mod.id);
              std::string sh = stableTag(st->name);
              std::size_t arity = m->comptimeParams.size() + m->params.size();
              std::string dh = stableTag(m->name + "#" + std::to_string(arity));
              mi->cname = symWithSuffix("__a_m_" + mtag + "_" + sh + "_" + dh);
              // Methods have no fnon table entry; still unique per struct.
              mi->globalVar = symWithSuffix("__a_gf_" + mtag + "_" + sh + "_" + dh);
            }
            for (const ComptimeParam& p : m->comptimeParams) mi->paramNames.push_back(p.name);
            for (const ParamDecl& p : m->params) mi->paramNames.push_back(p.name);
            mi->variadic = !m->params.empty() && m->params.back().variadic;
            fns_[m.get()] = mi;
            fnList_.push_back(mi);
            info->methods[m->name] = mi;
          }
        } else if (s->kind == StmtKind::EnumDecl) {
          const EnumDecl* e = static_cast<const EnumDecl*>(s.get());
          std::int64_t next = 0;
          int tag = 0;
          for (const EnumVariant& v : e->variants) {
            EnumVariantInfo ev;
            ev.algebraic = e->isAlgebraic();
            ev.tag = tag++;
            ev.value = v.value ? evalConstIntA(v.value.get(), next) : next;
            next = ev.value + 1;
            ev.moduleId = mod.id;
            ev.enumName = e->name;
            ev.variantName = v.name;
            {
              std::string mtag = moduleTag(mod.id);
              std::string eh = stableTag(e->name);
              std::string vh = stableTag(v.name);
              ev.globalVar = symWithSuffix("__a_ev_" + mtag + "_" + eh + "_" + vh);
            }
            for (const FieldDecl& f : v.fields) ev.payloadFields.push_back(f.name);
            enums_[std::to_string(mod.id) + ":" + e->name + "." + v.name] = ev;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          std::string cname = symWithSuffix("__a_top_" + moduleTag(mod.id) + "_" + stableTag(let->name));
          globals_.push_back(cname);
          topLevelGlobals_[std::to_string(mod.id) + ":" + let->name] = cname;
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            std::string key = std::to_string(mod.id) + ":" + nm;
            if (!topLevelGlobals_.count(key)) {
              std::string cname = symWithSuffix("__a_top_" + moduleTag(mod.id) + "_" + stableTag(nm));
              globals_.push_back(cname);
              topLevelGlobals_[key] = cname;
            }
          }
        }
      }
    }
    // Merge inherited fields/methods from baseClass chains. Sema resolves
    // them for type checking; codegen needs the same view so subclass
    // constructors take base fields and method lookup finds base methods.
    // Field order matches sema: farthest ancestor first, own fields last.
    // Resolved per-decl (not via other StructInfo::fieldNames) so the result
    // does not depend on structList_ ordering.
    for (StructInfo* info : structList_) {
      if (!info->decl) continue;
      std::vector<const StructDecl*> chain;
      std::set<std::string> seenNames;
      std::string base = info->decl->baseClass;
      while (!base.empty() && seenNames.insert(base).second) {
        StructInfo* baseInfo = nullptr;
        for (StructInfo* cand : structList_)
          if (cand->name == base && cand->moduleId == info->moduleId) {
            baseInfo = cand; break;
          }
        if (!baseInfo) {
          auto bit = structs_.find(base);
          if (bit == structs_.end() || !bit->second->decl) break;
          baseInfo = bit->second;
        }
        if (!baseInfo->decl) break;
        chain.push_back(baseInfo->decl);
        base = baseInfo->decl->baseClass;
      }
      if (chain.empty()) continue;
      std::vector<std::string> fields;
      std::set<std::string> have;
      for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (const FieldDecl& f : (*it)->fields)
          if (have.insert(f.name).second) fields.push_back(f.name);
      for (const std::string& f : info->fieldNames)
        if (have.insert(f).second) fields.push_back(f);
      info->fieldNames = fields;
      // Methods: nearest ancestor wins over farther ones; own methods were
      // registered first and emplace() never overwrites them.
      for (const StructDecl* anc : chain)
        for (const auto& m : anc->methods) {
          auto fit = fns_.find(m.get());
          if (fit != fns_.end()) info->methods.emplace(m->name, fit->second);
        }
    }
  }

std::int64_t AsmEmitter::evalConstIntA(const Expr* e, std::int64_t fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::IntLit) return static_cast<const IntLitExpr*>(e)->value;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      if (u->op == "-") return -evalConstIntA(u->operand.get(), 0);
    }
    if (e->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr*>(e)->name;
      if (const EnumVariantInfo* ev = findEnumByShort(nm))
        return ev->value;
    }
    return fallback;
  }

void AsmEmitter::buildGlobalScope(int preferModule) {
    scopes_.clear();
    scopes_.emplace_back();
    auto& g = scopes_.back();
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + let->name);
          if (it != topLevelGlobals_.end()) {
            ABinding b;
            b.kind = BKind::Global;
            b.sym = it->second;
            g[let->name] = b;
          }
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + nm);
            if (it != topLevelGlobals_.end()) {
              ABinding b;
              b.kind = BKind::Global;
              b.sym = it->second;
              g[nm] = b;
            }
          }
        }
      }
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      ABinding b;
      b.kind = BKind::FnValue;
      b.sym = f->globalVar;
      b.fn = f;
      if (!g.count(f->name)) g[f->name] = b;
    }
    for (StructInfo* st : structList_) {
      ABinding b;
      b.kind = BKind::StructCtor;
      b.sym = st->globalVar;
      b.st = st;
      if (!g.count(st->name)) g[st->name] = b;
    }
    for (auto& kv : enums_) {
      ABinding b;
      b.kind = BKind::EnumValue;
      b.sym = kv.second.globalVar;
      b.en = &kv.second;
      b.aux = kv.second.variantName;
      std::string qual = kv.second.enumName + "." + kv.second.variantName;
      // Keep the enum-qualified key for compatibility, but do not clobber a
      // binding for the same qualified name from a nearer module (first wins
      // deterministically in load order).
      if (!g.count(qual)) g[qual] = b;
      if (!g.count(b.aux)) g[b.aux] = b;
    }
    for (const Module& mod : graph_.modules) {
      ABinding b;
      b.kind = BKind::Module;
      b.moduleId = mod.id;
      if (!g.count(mod.name) && builtinRuntimeId(mod.name) < 0) g[mod.name] = b;
      std::string stem = stemOf(mod.path);
      if (!g.count(stem) && builtinRuntimeId(stem) < 0) g[stem] = b;
    }
    // Native-module stubs go last so a real A package with the same name
    // (e.g. std/json) shadows the native namespace.
    for (const char* nm : {"math", "io", "fs", "strings", "collections", "core", "time",
                           "json", "binary", "testing", "log", "metrics", "trace", "net",
                           "http", "terminal", "env", "repl", "kernels", "unsafe", "arena", "std",
                           "py"}) {
      if (g.count(nm)) continue;
      ABinding b;
      b.kind = BKind::Module;
      b.moduleId = -1;
      b.aux = nm;  // native module name for member lookup
      g[nm] = b;
    }
    // Same-named top-level globals from different modules share one flat
    // scope, so the last module in load order wins. Re-bind the module being
    // emitted so its own `x = ...` writes its own symbol (e.g. std:os
    // constants must not resolve to std:path's `sep`).
    int prefer = preferModule;
    if (prefer < 0) prefer = curModuleId_;
    if (prefer >= 0 && prefer < (int)graph_.modules.size()) {
      const Module& pm = graph_.at(prefer);
      if (pm.program) {
        for (const StmtPtr& s : pm.program->statements) {
          std::string nm;
          if (s->kind == StmtKind::Let) {
            nm = static_cast<const LetStmt*>(s.get())->name;
          } else if (s->kind == StmtKind::Assign) {
            const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
            if (as->target->kind == ExprKind::Ident)
              nm = static_cast<const IdentExpr*>(as->target.get())->name;
          }
          if (nm.empty()) continue;
          auto it = topLevelGlobals_.find(std::to_string(prefer) + ":" + nm);
          if (it == topLevelGlobals_.end()) continue;
          ABinding b;
          b.kind = BKind::Global;
          b.sym = it->second;
          g[nm] = b;
        }
      }
    }
  }

// ---- functions / main -------------------------------------------------------
// Bind `from <native-module> import member [as x]` items into the current
// scope: functions become BKind::Builtin (dispatched via a_bi_call),
// constants become BKind::NativeConst. Members missing from the
// nativeModules() spec are skipped here; sema already reported them.
void AsmEmitter::bindNativeFromImport(const FromImportStmt* im) {
    for (const auto& item : im->items) {
      if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
        int bid = builtinRuntimeId(*bn);
        if (bid < 0) continue;
        ABinding b;
        b.kind = BKind::Builtin;
        b.builtinId = bid;
        scopes_.back()[item.alias] = b;
      } else if (const std::string* cn = nativeConstant(im->bareName, item.name)) {
        ABinding b;
        b.kind = BKind::NativeConst;
        b.aux = *cn;
        scopes_.back()[item.alias] = b;
      }
    }
  }

// Register a module's import aliases (import x / from x import y) into the
// current scope so function bodies can resolve module.member calls.
void AsmEmitter::bindModuleImports(int moduleId) {
    if (moduleId < 0) return;
    const Module& mod = graph_.at(moduleId);
    if (!mod.program) return;
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          // import python:<mod> [as x]: dynamic module, calls go through
          // the runtime py_call builtin.
          std::string alias = !im->alias.empty() ? im->alias : stemOf(im->pythonModule);
          ABinding b;
          b.kind = BKind::PyModule;
          b.aux = im->pythonModule;
          scopes_.back()[alias] = b;
          continue;
        }
        std::string alias = !im->alias.empty() ? im->alias
                            : !im->bareName.empty() ? im->bareName
                            : stemOf(im->path);
        int mid = im->moduleId;
        if (im->nativeModule) {
          // Native namespace wins only when no A package provides the name;
          // stdlib A packages (e.g. std/json) are the real implementation.
          // Packages load as <pkg>/__init__.a so match the parent dir too.
          for (const Module& cand : graph_.modules) {
            std::string pkg = stemOf(cand.path);
            if (pkg == "__init__" || pkg.empty()) {
              auto slash = cand.path.find_last_of('/');
              if (slash != std::string::npos) {
                auto slash2 = cand.path.find_last_of('/', slash - 1);
                pkg = stemOf(cand.path.substr(slash2 == std::string::npos ? 0 : slash2 + 1,
                                              slash - (slash2 == std::string::npos ? 0 : slash2 + 1)));
              }
            }
            if (cand.name == im->bareName || stemOf(cand.path) == im->bareName ||
                pkg == im->bareName) {
              mid = cand.id;
              break;
            }
          }
        }
        if (mid >= 0) {
          ABinding b;
          b.kind = BKind::Module;
          b.moduleId = mid;
          scopes_.back()[alias] = b;
        } else if (im->nativeModule) {
          ABinding b;
          b.kind = BKind::Module;
          b.moduleId = -1;
          b.aux = im->bareName;
          scopes_.back()[alias] = b;
        }
      } else if (s->kind == StmtKind::FromImport) {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          // from python:<mod> import name [as x]: dynamic member call.
          for (const auto& item : im->items) {
            ABinding b;
            b.kind = BKind::PyFunc;
            b.aux = im->pythonModule;
            b.pyMember = item.name;
            scopes_.back()[item.alias] = b;
          }
          continue;
        }
        if (im->moduleId >= 0) {
          bindFileModuleFromImport(im);
        } else if (im->nativeModule) {
          // from <native> import member: bind member -> native builtin name
          bindNativeFromImport(im);
        }
      }
    }
  }

// Bind `from "<file>" import name [as x]` items into the current scope:
// each listed member binds under its alias (item.name when no `as`).
// Functions/structs resolve through the module's decl lists; top-level
// values bind to the module's global slot.
void AsmEmitter::bindFileModuleFromImport(const FromImportStmt* im) {
    const Module& src = graph_.at(im->moduleId);
    if (!src.program) return;
    for (const auto& item : im->items) {
      for (const StmtPtr& ss : src.program->statements) {
        if (ss->kind == StmtKind::FnDecl) {
          const FnDecl* fd = static_cast<const FnDecl*>(ss.get());
          if (fd->name != item.name) continue;
          for (FnInfo* fi : fnList_) {
            if (fi->decl == fd) {
              ABinding b;
              b.kind = BKind::FnValue;
              b.sym = fi->globalVar;
              b.fn = fi;
              scopes_.back()[item.alias] = b;
            }
          }
        } else if (ss->kind == StmtKind::StructDecl) {
          const StructDecl* sd = static_cast<const StructDecl*>(ss.get());
          if (sd->name != item.name) continue;
          for (StructInfo* sti : structList_) {
            if (sti->decl == sd) {
              ABinding b;
              b.kind = BKind::StructCtor;
              b.sym = sti->globalVar;
              b.st = sti;
              scopes_.back()[item.alias] = b;
            }
          }
        } else if (ss->kind == StmtKind::Let || ss->kind == StmtKind::Assign) {
          std::string vn;
          if (ss->kind == StmtKind::Let) {
            vn = static_cast<const LetStmt*>(ss.get())->name;
          } else {
            const AssignStmt* as = static_cast<const AssignStmt*>(ss.get());
            if (as->target->kind != ExprKind::Ident) continue;
            vn = static_cast<const IdentExpr*>(as->target.get())->name;
          }
          if (vn != item.name) continue;
          auto it = topLevelGlobals_.find(std::to_string(src.id) + ":" + vn);
          if (it != topLevelGlobals_.end()) {
            ABinding b;
            b.kind = BKind::Global;
            b.sym = it->second;
            scopes_.back()[item.alias] = b;
          }
        }
      }
    }
  }

void AsmEmitter::emitStructCtor(StructInfo* st) {
    // Slots: 0=argv ptr, 1=struct, 2+2i=key str, 3+2i=field value,
    // nameS/methS/fnsS hold the class-name string, method-name list and
    // method-name->fn dict passed to a_struct_new (the dict powers
    // runtime method dispatch for `var`/generic receivers); per-method
    // temps follow at methTmpS+2i/+2i+1.
    int nameS = 2 + 2 * (int)st->fieldNames.size();
    int methS = nameS + 1;
    int fnsS = methS + 1;
    int methTmpS = fnsS + 1;
    int nm = (int)st->methods.size();
    int need = methTmpS + 2 * nm;
    slotCounter_ = need;
    emit("    .p2align 4\n    .text\n    .globl " + st->ctorCname + "\n" + st->ctorCname + ":\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frameBytes()) + ", %rsp\n");
    emit("    mov %rdi, " + slotRef(0) + "\n");
    // class display name: strip any module qualifier ("mod::Point" -> "Point")
    std::string dispName = st->name;
    std::size_t sep = dispName.rfind("::");
    if (sep != std::string::npos) dispName = dispName.substr(sep + 2);
    {
      std::string nlbl = emitStrData(dispName, false);
      emit("    lea " + nlbl + "(%rip), %rdi\n");
      movImm(dispName.size(), "%rsi");
      emit("    call a_str_new\n    mov %rax, " + slotRef(nameS) + "\n");
    }
    // method-name list (sorted for deterministic introspection output)
    emit("    xor %edi, %edi\n    xor %esi, %esi\n    call a_list_new\n    mov %rax, " + slotRef(methS) + "\n");
    {
      std::vector<std::string> mnames;
      for (const auto& kv : st->methods) mnames.push_back(kv.first);
      std::sort(mnames.begin(), mnames.end());
      for (const std::string& mn : mnames) {
        std::string mlbl = emitStrData(mn, false);
        emit("    lea " + mlbl + "(%rip), %rdi\n");
        movImm(mn.size(), "%rsi");
        emit("    call a_str_new\n");
        loadSlot(methS, "%rdi");
        emit("    mov %rax, %rsi\n    call a_list_append\n");
      }
    }
    // method-name -> fn-value dict: dynamic dispatch (a_method on
    // `var`/generic receivers) resolves against the receiver's own class,
    // so same-named methods on different classes cannot collide.
    emit("    call a_dict_new\n    mov %rax, " + slotRef(fnsS) + "\n");
    {
      int mi = 0;
      for (const auto& kv : st->methods) {
        const std::string& mn = kv.first;
        FnInfo* mf = kv.second;
        int ks = methTmpS + 2 * mi, vs = ks + 1;
        std::string klbl = emitStrData(mn, false);
        emit("    lea " + klbl + "(%rip), %rdi\n");
        movImm(mn.size(), "%rsi");
        emit("    call a_str_new\n    mov %rax, " + slotRef(ks) + "\n");
        // fn value: entry = method code addr, env = A_NONE, name = cstr.
        std::string nlbl = emitStrData(mn, true);
        emit("    lea " + mf->cname + "(%rip), %rdi\n");
        movImm(kNone, "%rsi");
        emit("    lea " + nlbl + "(%rip), %rdx\n    call a_fnon_new\n");
        emit("    mov %rax, " + slotRef(vs) + "\n");
        loadSlot(fnsS, "%rdi");
        loadSlot(ks, "%rsi");
        loadSlot(vs, "%rdx");
        emit("    call a_dict_set\n");
        ++mi;
      }
    }
    loadSlot(nameS, "%rdi");
    loadSlot(methS, "%rsi");
    loadSlot(fnsS, "%rdx");
    emit("    call a_struct_new\n    mov %rax, " + slotRef(1) + "\n");
    for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
      int ks = 2 + 2 * (int)i, vs = ks + 1;
      std::string lbl = emitStrData(st->fieldNames[i], false);
      emit("    lea " + lbl + "(%rip), %rdi\n");
      movImm(st->fieldNames[i].size(), "%rsi");
      emit("    call a_str_new\n    mov %rax, " + slotRef(ks) + "\n");
      loadSlot(0, "%rax");
      emit("    mov " + std::to_string(8 * (i + 1)) + "(%rax), %rax\n    mov %rax, " + slotRef(vs) + "\n");
      call3slot("a_dict_set", 1, ks, vs);
    }
    // The ctor has no frame/region of its own: its allocations live in the
    // caller's young region, so the returned struct must be promoted or
    // the caller's region sweep would free it (and its fields).
    call1slot("a_promote", 1);
    loadSlot(1);
    emit("    leave\n    ret\n");
    slotCounter_ = 0;
  }

void AsmEmitter::emitFunction(FnInfo* f) {
    // Two-pass: body first into a scratch stream so the frame size is known
    // before the prologue `sub $N, %rsp` is emitted.
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    buildGlobalScope(f->moduleId);
    pushScope();
    bindModuleImports(f->moduleId);
    int argvSlot = allocSlot();
    emit("    mov %rdi, " + slotRef(argvSlot) + "\n");
    emitFramePush(f->isMethod ? f->structName + "." + f->name : f->name,
                  f->moduleId, f->decl ? f->decl->span.line : 0);
    for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
      int ps = allocSlot();
      loadSlot(argvSlot, "%rax");
      emit("    mov " + std::to_string(8 * (i + 1)) + "(%rax), %rax\n    mov %rax, " + slotRef(ps) + "\n");
      ABinding b;
      b.kind = BKind::Param;
      b.slot = ps;
      scopes_.back()[f->paramNames[i]] = b;
    }
    int savedModule = curModuleId_;
    curModuleId_ = f->moduleId;
    bool savedInFn = inFunction_;
    inFunction_ = true;
    // Default parameters: any argv slot still holding kMissing is replaced by
    // its default expression, evaluated in order so a default may reference
    // earlier (already-substituted) parameters.
    if (f->decl) {
      std::size_t offset = f->paramNames.size() - f->decl->params.size();
      for (std::size_t i = offset; i < f->paramNames.size(); ++i) {
        const ParamDecl& p = f->decl->params[i - offset];
        if (!p.defaultValue) continue;
        ABinding* pb = lookup(f->paramNames[i]);
        if (!pb || pb->kind != BKind::Param) continue;
        int ps = pb->slot;
        std::string skipL = newLabel("def");
        loadSlot(ps);
        movImm(kMissing, "%rcx");
        emit("    cmp %rcx, %rax\n    jne " + skipL + "\n");
        emitExpr(p.defaultValue.get());
        storeRax(ps);
        emitLabel(skipL);
      }
    }
    auto savedPromote = std::move(promoteNames_);
    auto savedSpec = std::move(specNames_);
    auto savedRanges = std::move(nameRanges_);
    promoteNames_.clear();
    specNames_.clear();
    nameRanges_.clear();
    computePromoteSet(f->decl);
    if (const char* dbg = std::getenv("A_DEBUG_PROMOTE"); dbg && *dbg) {
      std::string line = "promote " + f->name + ":";
      for (const std::string& nm : promoteNames_)
        line += " " + nm + (specNames_.count(nm) ? "(spec)" : "");
      std::fprintf(stderr, "%s\n", line.c_str());
      for (const auto& kv : nameRanges_)
        std::fprintf(stderr, "  range %s: %s %lld..%lld\n", kv.first.c_str(),
                     kv.second.known ? "known" : "unknown",
                     (long long)kv.second.lo, (long long)kv.second.hi);
    }
    // Strength reduction scans pre-loop writes via the current body.
    const std::vector<StmtPtr>* savedBody = curFnBody_;
    curFnBody_ = f->decl ? &f->decl->body : nullptr;
    emitBody(f->decl->body);
    curFnBody_ = savedBody;
    promoteNames_ = std::move(savedPromote);
    specNames_ = std::move(savedSpec);
    nameRanges_ = std::move(savedRanges);
    emitFramePop();
    emit("    mov $" + u64s(kNone) + ", %rax\n    leave\n    ret\n");
    int frame = frameBytes();
    std::ostringstream emitted;
    emitted.swap(text_);
    text_.swap(saved);
    emit("    .p2align 4\n    .text\n    .globl " + f->cname + "\n" + f->cname + ":\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
    text_ << emitted.str();
    scopes_.clear();
    curModuleId_ = savedModule;
    inFunction_ = savedInFn;
  }

// Render "name(p1, p2, ...)" signature text for a function's runtime
// metadata (inspect.signature / core.fn_signature).
static std::string fnSignatureText(const FnInfo* f) {
    std::string sig = f->name + "(";
    bool first = true;
    if (f->decl) {
      for (const ParamDecl& p : f->decl->params) {
        if (!first) sig += ", ";
        first = false;
        sig += p.name;
        if (p.variadic) sig += "...";
      }
    } else {
      for (const std::string& pn : f->paramNames) {
        if (!first) sig += ", ";
        first = false;
        sig += pn;
      }
    }
    sig += ")";
    return sig;
}

// Docstring text for a function: methods carry decl->doc; top-level
// functions keep it as a leading string-literal body statement.
static std::string fnDocText(const FnInfo* f) {
    if (!f->decl) return "";
    if (!f->decl->doc.empty()) return f->decl->doc;
    if (!f->decl->body.empty()) {
      std::string out;
      if (docstringText(*f->decl->body.front(), out)) return out;
    }
    return "";
}

void AsmEmitter::emitMain() {
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    buildGlobalScope(graph_.entry);
    pushScope();
    allocSlot();  // slot 0 reserved (unused scratch)
    emit("    call a_runtime_init\n");
    emitFramePush("<module>", graph_.entry, 0);
    for (StructInfo* st : structList_) {
      emit("    lea " + st->ctorCname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
      std::string lbl = emitStrData(st->name, true);
      emit("    lea " + lbl + "(%rip), %rdx\n");
      std::string sig = st->name + "(";
      for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
        if (i) sig += ", ";
        sig += st->fieldNames[i];
      }
      sig += ")";
      std::string sl = emitStrData(sig, true);
      std::string dl = emitStrData(st->decl ? st->decl->doc : "", true);
      emit("    lea " + sl + "(%rip), %rcx\n    lea " + dl + "(%rip), %r8\n    call a_fnon_new_ex\n");
      emit("    mov %rax, " + st->globalVar + "(%rip)\n");
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      emit("    lea " + f->cname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
      std::string lbl = emitStrData(f->name, true);
      emit("    lea " + lbl + "(%rip), %rdx\n");
      std::string sl = emitStrData(fnSignatureText(f), true);
      std::string dl = emitStrData(fnDocText(f), true);
      emit("    lea " + sl + "(%rip), %rcx\n    lea " + dl + "(%rip), %r8\n    call a_fnon_new_ex\n");
      emit("    mov %rax, " + f->globalVar + "(%rip)\n");
    }
    for (auto& kv : enums_) {
      if (kv.second.algebraic) continue;
      movImm((uint64_t)kv.second.value, "%rdi");
      emit("    call a_i64n\n    mov %rax, " + kv.second.globalVar + "(%rip)\n");
    }
    // Module top-level statements and per-module imports run in every
    // mode, `a test` included: test_* functions observe the globals and
    // initializers the module body establishes. Only the `fn main`
    // invocation below is skipped under `a test`.
    std::vector<int> order = graph_.order;
    if (order.empty()) order.push_back(graph_.entry);
    int savedModule = curModuleId_;
    for (int mid : order) {
      if (mid < 0 || failed_) continue;
      const Module& mod = graph_.at(mid);
      if (!mod.program) continue;
      if (mod.id != graph_.entry && mod.name == "prelude") continue;
      curModuleId_ = mod.id;
      // Per-module imports: register aliases in the module-import scope.
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::Import) {
          const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
          if (im->pythonModuleImport) {
            std::string alias = !im->alias.empty() ? im->alias
                                : stemOf(im->pythonModule);
            ABinding b;
            b.kind = BKind::PyModule;
            b.aux = im->pythonModule;
            scopes_.back()[alias] = b;
            continue;
          }
          std::string alias = !im->alias.empty() ? im->alias
                              : !im->bareName.empty() ? im->bareName
                              : stemOf(im->path);
          if (im->moduleId >= 0) {
            ABinding b;
            b.kind = BKind::Module;
            b.moduleId = im->moduleId;
            scopes_.back()[alias] = b;
          }
        } else if (s->kind == StmtKind::FromImport) {
          const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
          if (im->pythonModuleImport) {
            for (const auto& item : im->items) {
              ABinding b;
              b.kind = BKind::PyFunc;
              b.aux = im->pythonModule;
              b.pyMember = item.name;
              scopes_.back()[item.alias] = b;
            }
            continue;
          }
          if (im->moduleId >= 0) {
            bindFileModuleFromImport(im);
          } else if (im->nativeModule) {
            // from <native> import member: bind member -> native builtin name
            bindNativeFromImport(im);
          }
        }
      }
      promoteNames_.clear();  // top-level bindings are globals, never raw
      specNames_.clear();
      nameRanges_.clear();
      emitBody(mod.program->statements, true);
      }
      curModuleId_ = savedModule;
      if (testMode_) {
      // `a test`: emit a table of top-level test_* functions and let the
      // runtime harness (a_test_main) fork/pipe each one, matching the old
      // C-emitter protocol.
      std::vector<FnInfo*> tests;
      for (FnInfo* f : fnList_) {
        if (!f->isMethod && f->moduleId == graph_.entry &&
            f->name.rfind("test_", 0) == 0 &&
            (testFilter_.empty() ||
             f->name.find(testFilter_) != std::string::npos)) {
          tests.push_back(f);
        }
      }
      std::sort(tests.begin(), tests.end(),
                [](const FnInfo* a, const FnInfo* b) { return a->name < b->name; });
      // Emit all string labels first: emitStrData switches back to .rodata,
      // which would corrupt an interleaved .data table.
      std::vector<std::string> nameLbls;
      for (FnInfo* f : tests) nameLbls.push_back(emitStrData(f->name, true));
      rodata_ << "    .section .data\n    .align 8\n"
                 "    .globl __a_test_names\n__a_test_names:\n";
      for (const std::string& lbl : nameLbls) rodata_ << "    .quad " << lbl << "\n";
      rodata_ << "    .globl __a_test_fns\n__a_test_fns:\n";
      for (FnInfo* f : tests) rodata_ << "    .quad " << f->cname << "\n";
      emit("    mov $" + std::to_string(tests.size()) + ", %rdi\n");
      emit("    lea __a_test_names(%rip), %rsi\n");
      emit("    lea __a_test_fns(%rip), %rdx\n");
      emit("    call a_test_main\n");
      int testRc = allocSlot();
      emit("    mov %rax, " + slotRef(testRc) + "\n");
      emitFramePop();
      loadSlot(testRc);
      emit("    leave\n    ret\n");
    } else {
      // `fn main()` entry point: invoked once after all module top-levels,
      // entry module only. Never runs for imports or under `a test`.
      for (FnInfo* f : fnList_) {
        if (!f->isMethod && f->moduleId == graph_.entry && f->name == "main") {
          std::vector<int> ps(f->paramNames.size(), -1);
          if (f->variadic && !ps.empty()) {
            // main is invoked with no arguments: *args collects an empty list
            // (kMissing would leave the slot unreadable and crash len(args)).
            movImm(0, "%rdi");
            emit("    xor %esi, %esi\n    call a_list_new\n");
            int ls = allocSlot();
            storeRax(ls);
            ps.back() = ls;
          }
          emitDirectCall(f->cname.c_str(), ps);
          break;
        }
      }
      emitFramePop();
      emit("    xor %eax, %eax\n    leave\n    ret\n");
    }
    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .p2align 4\n    .text\n    .globl main\nmain:\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
    text_ << body.str();
    for (const std::string& g : globals_) bss_ << "    .section .bss\n    .balign 8\n    .globl " << g << "\n" << g << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (FnInfo* f : fnList_) if (!f->isMethod) bss_ << "    .section .bss\n    .balign 8\n    .globl " << f->globalVar << "\n" << f->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (StructInfo* st : structList_) bss_ << "    .section .bss\n    .balign 8\n    .globl " << st->globalVar << "\n" << st->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (auto& kv : enums_) if (!kv.second.algebraic) bss_ << "    .section .bss\n    .balign 8\n    .globl " << kv.second.globalVar << "\n" << kv.second.globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    emitGcRootsTable();
    scopes_.clear();
  }

// Build the algebraic-enum variant dict ({__tag, __name}) into %rax.
void AsmEmitter::emitVariantDict(const EnumVariantInfo& ev, const std::string& name) {
    emit("    call a_dict_new\n");
    int dS = allocSlot();
    storeRax(dS);
    int kS = strLitSlot("__tag");
    int vS = allocSlot();
    movImm(mkintA(ev.tag), "%rcx");
    emit("    mov %rcx, " + slotRef(vS));
    call3slot("a_dict_set", dS, kS, vS);
    int k2 = strLitSlot("__name");
    int v2 = strLitSlot(name);
    call3slot("a_dict_set", dS, k2, v2);
    loadSlot(dS);
  }


// ---- per-module init / main split (Phase 2) --------------------------------
void AsmEmitter::emitModuleBss(int moduleId) {
  // Top-level globals belonging to this module (map holds the live copy;
  // duplicate `let` shadows are lost in the map but were unreferenced).
  std::vector<std::string> tops;
  for (const auto& kv : topLevelGlobals_) {
    const std::string& key = kv.first;
    auto colon = key.find(':');
    if (colon == std::string::npos) continue;
    std::string midStr = key.substr(0, colon);
    if (midStr != std::to_string(moduleId)) continue;
    tops.push_back(kv.second);
  }
  std::sort(tops.begin(), tops.end());
  std::sort(tops.begin(), tops.end(), [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
  });
  // Deterministic: dedupe in case two names share one symbol.
  tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
  for (const std::string& g : tops)
    bss_ << "    .section .bss\n    .balign 8\n    .globl " << g << "\n" << g << ":\n    .byte 0,0,0,0,0,0,0,0\n";
  for (FnInfo* f : fnList_)
    if (!f->isMethod && f->moduleId == moduleId)
      bss_ << "    .section .bss\n    .balign 8\n    .globl " << f->globalVar << "\n" << f->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
  for (StructInfo* st : structList_)
    if (st->moduleId == moduleId)
      bss_ << "    .section .bss\n    .balign 8\n    .globl " << st->globalVar << "\n" << st->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
  for (auto& kv : enums_)
    if (!kv.second.algebraic && kv.second.moduleId == moduleId)
      bss_ << "    .section .bss\n    .balign 8\n    .globl " << kv.second.globalVar << "\n" << kv.second.globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
}

// GC roots: one .data table of pointers to every aval-holding global slot.
// The symbol set mirrors what emitModuleBss/emitMain define in .bss across
// ALL modules: top-level globals (__a_top_*), non-method fn-value slots
// (__a_gf_*), struct ctor values (__a_gs_*) and non-algebraic enum values
// (__a_ev_*). Method globalVars and algebraic enums never get .bss slots,
// so they are excluded (over-inclusion would reference undefined symbols).
// Always emitted — even with zero globals — so the runtime's weak externs
// always resolve.
void AsmEmitter::emitGcRootsTable() {
  std::set<std::string> roots;
  for (const auto& kv : topLevelGlobals_) roots.insert(kv.second);
  for (FnInfo* f : fnList_)
    if (!f->isMethod) roots.insert(f->globalVar);
  for (StructInfo* st : structList_) roots.insert(st->globalVar);
  for (auto& kv : enums_)
    if (!kv.second.algebraic) roots.insert(kv.second.globalVar);
  bss_ << "    .section .data\n    .align 8\n"
          "    .globl __a_gc_roots\n__a_gc_roots:\n";
  for (const std::string& s : roots) bss_ << "    .quad " << s << "\n";
  bss_ << "    .globl __a_gc_nroots\n__a_gc_nroots:\n"
          "    .quad " << roots.size() << "\n";
}

void AsmEmitter::drainLambdasFor(int moduleId) {
  while (!failed_) {
    int idx = -1;
    for (int i = (int)lambdas_.size() - 1; i >= 0; --i) {
      if (lambdas_[(std::size_t)i].moduleId == moduleId) { idx = i; break; }
    }
    if (idx < 0) break;
    PendingLambda pl = std::move(lambdas_[(std::size_t)idx]);
    lambdas_.erase(lambdas_.begin() + idx);
    emitLambdaFn(pl);
  }
}

void AsmEmitter::emitModuleInitBody(int moduleId) {
  std::ostringstream saved;
  saved.swap(text_);
  slotCounter_ = 0;
  scopes_.clear();
  buildGlobalScope(moduleId);
  pushScope();
  allocSlot();  // slot 0 reserved (mirrors emitMain scratch)
  emitFramePush("<module>", moduleId, 0);
  int savedModule = curModuleId_;
  curModuleId_ = moduleId;
  const Module* modPtr = nullptr;
  if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
    modPtr = &graph_.at(moduleId);
  bool isPreludeSkip = false;
  if (modPtr && modPtr->id != graph_.entry && modPtr->name == "prelude")
    isPreludeSkip = true;
  if (modPtr && modPtr->program && !isPreludeSkip) {
    const Module& mod = *modPtr;
    // Per-module imports: same bindings emitMain used for top-level code.
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          std::string alias = !im->alias.empty() ? im->alias
                              : stemOf(im->pythonModule);
          ABinding b;
          b.kind = BKind::PyModule;
          b.aux = im->pythonModule;
          scopes_.back()[alias] = b;
          continue;
        }
        std::string alias = !im->alias.empty() ? im->alias
                            : !im->bareName.empty() ? im->bareName
                            : stemOf(im->path);
        if (im->moduleId >= 0) {
          ABinding b;
          b.kind = BKind::Module;
          b.moduleId = im->moduleId;
          scopes_.back()[alias] = b;
        }
      } else if (s->kind == StmtKind::FromImport) {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          for (const auto& item : im->items) {
            ABinding b;
            b.kind = BKind::PyFunc;
            b.aux = im->pythonModule;
            b.pyMember = item.name;
            scopes_.back()[item.alias] = b;
          }
          continue;
        }
        if (im->moduleId >= 0) {
          bindFileModuleFromImport(im);
        } else if (im->nativeModule) {
          bindNativeFromImport(im);
        }
      }
    }
    promoteNames_.clear();
    specNames_.clear();
    nameRanges_.clear();
    emitBody(mod.program->statements, true);
  }
  curModuleId_ = savedModule;
  emitFramePop();
  emit("    xor %eax, %eax\n    leave\n    ret\n");
  int frame = frameBytes();
  std::ostringstream body;
  body.swap(text_);
  text_.swap(saved);
  std::string sym = modInitSym(moduleId);
  emit("    .p2align 4\n    .text\n    .globl " + sym + "\n" + sym + ":\n");
  emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
  text_ << body.str();
  scopes_.clear();
}

void AsmEmitter::emitMainShared() {
  std::ostringstream saved;
  saved.swap(text_);
  slotCounter_ = 0;
  scopes_.clear();
  buildGlobalScope(graph_.entry);
  pushScope();
  allocSlot();  // slot 0 reserved (unused scratch)
  emit("    call a_runtime_init\n");
  emitFramePush("<module>", graph_.entry, 0);
  for (StructInfo* st : structList_) {
    emit("    lea " + st->ctorCname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
    std::string lbl = emitStrData(st->name, true);
    emit("    lea " + lbl + "(%rip), %rdx\n");
    std::string sig = st->name + "(";
    for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
      if (i) sig += ", ";
      sig += st->fieldNames[i];
    }
    sig += ")";
    std::string sl = emitStrData(sig, true);
    std::string dl = emitStrData(st->decl ? st->decl->doc : "", true);
    emit("    lea " + sl + "(%rip), %rcx\n    lea " + dl + "(%rip), %r8\n    call a_fnon_new_ex\n");
    emit("    mov %rax, " + st->globalVar + "(%rip)\n");
  }
  for (FnInfo* f : fnList_) {
    if (f->isMethod) continue;
    emit("    lea " + f->cname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
    std::string lbl = emitStrData(f->name, true);
    emit("    lea " + lbl + "(%rip), %rdx\n");
    std::string sl = emitStrData(fnSignatureText(f), true);
    std::string dl = emitStrData(fnDocText(f), true);
    emit("    lea " + sl + "(%rip), %rcx\n    lea " + dl + "(%rip), %r8\n    call a_fnon_new_ex\n");
    emit("    mov %rax, " + f->globalVar + "(%rip)\n");
  }
  for (auto& kv : enums_) {
    if (kv.second.algebraic) continue;
    movImm((uint64_t)kv.second.value, "%rdi");
    emit("    call a_i64n\n    mov %rax, " + kv.second.globalVar + "(%rip)\n");
  }
  // Run every module's top-level init (__a_modinit_<mtag>, emitted by
  // emitModuleInitBody in that module's unit) in graph order — under
  // `a test` too, so test_* functions see initialized globals. Only the
  // `fn main` invocation is skipped in test mode.
  {
    std::vector<int> order = graph_.order;
    if (order.empty() && graph_.entry >= 0) order.push_back(graph_.entry);
    for (int mid : order) {
      if (mid < 0 || failed_) continue;
      if (mid >= (int)graph_.modules.size()) continue;
      const Module& mod = graph_.at(mid);
      if (!mod.program) continue;
      if (mod.id != graph_.entry && mod.name == "prelude") continue;
      // Call __a_modinit_<mtag> (defined in that module's unit).
      std::vector<int> noArgs;
      emitDirectCall(modInitSym(mid).c_str(), noArgs);
    }
  }
  if (testMode_) {
    std::vector<FnInfo*> tests;
    for (FnInfo* f : fnList_) {
      if (!f->isMethod && f->moduleId == graph_.entry &&
          f->name.rfind("test_", 0) == 0 &&
          (testFilter_.empty() ||
           f->name.find(testFilter_) != std::string::npos)) {
        tests.push_back(f);
      }
    }
    std::sort(tests.begin(), tests.end(),
              [](const FnInfo* a, const FnInfo* b) { return a->name < b->name; });
    std::vector<std::string> nameLbls;
    for (FnInfo* f : tests) nameLbls.push_back(emitStrData(f->name, true));
    rodata_ << "    .section .data\n    .align 8\n"
               "    .globl __a_test_names\n__a_test_names:\n";
    for (const std::string& lbl : nameLbls) rodata_ << "    .quad " << lbl << "\n";
    rodata_ << "    .globl __a_test_fns\n__a_test_fns:\n";
    for (FnInfo* f : tests) rodata_ << "    .quad " << f->cname << "\n";
    emit("    mov $" + std::to_string(tests.size()) + ", %rdi\n");
    emit("    lea __a_test_names(%rip), %rsi\n");
    emit("    lea __a_test_fns(%rip), %rdx\n");
    emit("    call a_test_main\n");
    int testRc = allocSlot();
    emit("    mov %rax, " + slotRef(testRc) + "\n");
    emitFramePop();
    loadSlot(testRc);
    emit("    leave\n    ret\n");
  } else {
    for (FnInfo* f : fnList_) {
      if (!f->isMethod && f->moduleId == graph_.entry && f->name == "main") {
        std::vector<int> ps(f->paramNames.size(), -1);
        if (f->variadic && !ps.empty()) {
          movImm(0, "%rdi");
          emit("    xor %esi, %esi\n    call a_list_new\n");
          int ls = allocSlot();
          storeRax(ls);
          ps.back() = ls;
        }
        emitDirectCall(f->cname.c_str(), ps);
        break;
      }
    }
    emitFramePop();
    emit("    xor %eax, %eax\n    leave\n    ret\n");
  }
  int frame = frameBytes();
  std::ostringstream body;
  body.swap(text_);
  text_.swap(saved);
  emit("    .p2align 4\n    .text\n    .globl main\nmain:\n");
  emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
  text_ << body.str();
  scopes_.clear();
}

void AsmEmitter::emitUnit(int moduleId, std::string& out) {
  text_ = std::ostringstream();
  rodata_ = std::ostringstream();
  bss_ = std::ostringstream();
  labelCounter_ = 0;
  dataCounter_ = 0;
  lambdaSeq_.erase(moduleId);
  for (StructInfo* st : structList_)
    if (st->moduleId == moduleId) emitStructCtor(st);
  for (FnInfo* f : fnList_) {
    if (failed_) break;
    if (f->moduleId == moduleId) emitFunction(f);
  }
  if (!failed_) emitModuleInitBody(moduleId);
  if (!failed_ && moduleId == graph_.entry) emitMainShared();
  if (!failed_) drainLambdasFor(moduleId);
  if (failed_) return;
  emitModuleBss(moduleId);
  // The GC roots table is emitted once, in the entry unit: it references
  // every module's global slots as extern .globl symbols, resolved when
  // the per-module objects are linked together.
  if (moduleId == graph_.entry) emitGcRootsTable();
  out = bss_.str() + rodata_.str() + text_.str();
}

}  // namespace a::detail
