// LLVM IR backend (A -> .ll). Every A value is an aval word (i64) and every
// A function has the uniform runtime signature `i64 f(ptr argv)` with
// argv[0] = closure env (A_NONE for plain functions) and argv[1..n] =
// parameters — the same ABI a_runtime.h documents. All semantics go through
// the a_* runtime entry points; LLVM's optimizer (mem2reg promotes the
// allocas used for locals, instcombine/GVN/loop opts clean up the rest).
#include "llvm/llvm_emit.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "ast/builtins.hpp"
#include "codegen/codegen_internal.hpp"  // FnInfo/StructInfo/BKind/builtin ids
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace {

// aval constants (a_runtime.h).
const std::string kNone = "-281474976710654";        // A_NONE
const std::string kFalse = "-281474976710652";       // A_FALSE
const std::string kTrue = "-281474976710650";        // A_TRUE
const std::string kMissing = "-281474976710656";     // A_TAG (missing arg)
constexpr std::int64_t kInlineLim = INT64_C(1) << 46;

struct LlBinding {
  BKind kind = BKind::Local;
  std::string sym;    // Global/FnValue/StructCtor/EnumValue: LLVM global name
  std::string slot;   // Local/Param: alloca name
  FnInfo* fn = nullptr;
  StructInfo* st = nullptr;
  const EnumVariantInfo* en = nullptr;
  int builtinId = -1;
  int moduleId = -1;
  std::string aux;        // Module: native module name / NativeConst name
  std::string pyMember;
  FnInfo* dropFn = nullptr;  // struct `drop` method for RAII cleanup
};

class LlvmEmitter {
 public:
  LlvmEmitter(const ModuleGraph& graph, const SemaResult& sema,
              bool testMode, const std::string* testFilter)
      : graph_(graph), sema_(sema), testMode_(testMode),
        testFilter_(testFilter ? *testFilter : "") {}
  ~LlvmEmitter() {
    for (FnInfo* f : fnList_) delete f;
    for (StructInfo* s : structList_) delete s;
  }

  bool run(std::string& out, std::string& error) {
    error_ = &error;
    collectGlobals();
    emitPreamble();
    for (FnInfo* f : fnList_) emitFunction(f);
    for (StructInfo* st : structList_) emitStructCtorBody(st);
    for (int mid : moduleOrder()) emitModInit(mid);
    emitMain();
    // Lambdas can be queued from any function, module init, or main body;
    // drain after all of them so every lambda gets a definition (forward
    // references in LLVM IR resolve regardless of order).
    drainLambdas();
    emitGlobalDefs();
    emitGcRoots();
    if (failed_) return false;
    out = pre_.str() + helpers_.str() + body_.str();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::string* error_ = nullptr;
  bool failed_ = false;
  bool testMode_ = false;
  std::string testFilter_;

  std::ostringstream pre_;      // target decl, runtime declares, globals
  std::ostringstream helpers_;  // internal @a.* helper functions
  std::ostringstream body_;     // all emitted functions
  std::ostringstream fn_;       // current function body

  std::unordered_map<const FnDecl*, FnInfo*> fns_;
  std::vector<FnInfo*> fnList_;
  std::unordered_map<std::string, StructInfo*> structs_;
  std::vector<StructInfo*> structList_;
  std::unordered_map<std::string, EnumVariantInfo> enums_;
  std::vector<std::string> globals_;
  std::unordered_map<std::string, std::string> topLevelGlobals_;
  std::unordered_map<int, std::string> moduleTags_;

  std::vector<std::map<std::string, LlBinding>> scopes_;
  std::vector<std::string> loopBreak_, loopCont_;
  // `defer`/RAII support: each scope frame collects deferred statements
  // (run LIFO) and owned struct values (drop() on scope exit) — mirrors
  // AsmEmitter::ScopeFrame.
  struct DropRec {
    std::string slot;      // alloca holding the value (locals); "" = use sym
    std::string sym;       // global symbol (top-level); "" = use slot
    FnInfo* fn = nullptr;  // struct `drop` method
  };
  struct ScopeFrame {
    std::vector<const Stmt*> defers;
    std::vector<DropRec> drops;
  };
  std::vector<ScopeFrame> deferStack_;
  std::vector<int> loopDeferDepth_;
  std::vector<std::pair<std::string, std::string>> pendingAllocas_;
  int curModuleId_ = -1;
  bool term_ = false;
  int tmpCounter_ = 0, lblCounter_ = 0, strCounter_ = 0, lamCounter_ = 0;

  struct PendingLambda {
    const LambdaExpr* expr = nullptr;
    int moduleId = -1;
    std::string cname;
    std::vector<std::pair<std::string, std::string>> captures;  // name, alloca
  };
  std::vector<PendingLambda> lambdas_;

  // ---- small utilities -----------------------------------------------------
  void fail(const std::string& msg) {
    if (failed_) return;
    failed_ = true;
    if (error_) *error_ = "llvm backend: " + msg;
  }
  void emit(const std::string& s) { fn_ << s << "\n"; }
  std::string T() { return "%t" + std::to_string(tmpCounter_++); }
  std::string newLabel(const std::string& p) {
    return "L." + p + std::to_string(lblCounter_++);
  }
  std::string moduleTag(int moduleId) {
    auto it = moduleTags_.find(moduleId);
    if (it != moduleTags_.end()) return it->second;
    std::string path;
    if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
      path = graph_.at(moduleId).path;
    std::string canonical = path;
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(path, ec);
    if (!ec) canonical = c.string();
    if (canonical.empty()) canonical = path;
    std::string tag = stableTag(canonical);
    moduleTags_[moduleId] = tag;
    return tag;
  }
  std::string modInitSym(int moduleId) {
    return "__a_modinit_" + moduleTag(moduleId);
  }
  std::vector<int> moduleOrder() {
    std::vector<int> order = graph_.order;
    if (order.empty() && graph_.entry >= 0) order.push_back(graph_.entry);
    std::set<int> in(order.begin(), order.end());
    for (const Module& m : graph_.modules)
      if (!in.count(m.id)) order.push_back(m.id);
    return order;
  }

  // ---- allocas / constants --------------------------------------------------
  // Records an alloca in the current function's entry block; returns "%vN".
  int allocaCounter_ = 0;
  std::string alloca(const std::string& ty) {
    std::string name = "%v" + std::to_string(allocaCounter_++);
    pendingAllocas_.push_back({name, ty});
    return name;
  }
  // aval of a small-int literal: inline encoding when in range.
  std::string avalIntLit(std::int64_t v) {
    if (v >= -kInlineLim && v < kInlineLim) {
      std::uint64_t p = ((std::uint64_t)v << 1) | 1ULL;
      p &= 281474976710655ULL;
      std::uint64_t a = -281474976710656ULL | p;
      return std::to_string(a);
    }
    std::string r = T();
    emit("  " + r + " = call i64 @a_i64n(i64 " + std::to_string(v) + ")");
    return r;
  }
  // aval of a double literal: raw bits, NaN-canonicalized like a_mkdouble.
  std::string avalDoubleLit(double d) {
    if (d != d) return "9218868437227405312";
    std::uint64_t u;
    std::memcpy(&u, &d, 8);
    return std::to_string((std::int64_t)u);
  }
  // Emit a NUL-terminated or counted byte-string constant; returns symbol.
  std::string cstrGlobal(const std::string& bytes, bool nulTerm) {
    std::string name = "@.str" + std::to_string(strCounter_++);
    std::string esc;
    for (unsigned char c : bytes) {
      // Hex escapes for '"' and '\': LLVM's c"" parser is unreliable with
      // backslash-letter escapes like \" (it truncates the literal).
      if (c == '"') esc += "\\22";
      else if (c == '\\') esc += "\\5C";
      else if (c >= 0x20 && c < 0x7F) esc.push_back((char)c);
      else {
        char b[8];
        std::snprintf(b, sizeof b, "\\%02X", c);
        esc += b;
      }
    }
    if (nulTerm) esc += "\\00";
    pre_ << "  " << name << " = private unnamed_addr constant ["
         << bytes.size() + (nulTerm ? 1 : 0) << " x i8] c\"" << esc << "\"\n";
    return name;
  }
  // aval string value from raw bytes: a_str_new(ptr, len).
  std::string avalStrBytes(const std::string& text) {
    std::string sym = cstrGlobal(text, false);
    std::string r = T();
    emit("  " + r + " = call i64 @a_str_new(ptr " + sym + ", i64 " +
         std::to_string(text.size()) + ")");
    return r;
  }
  // Call a runtime function returning aval with i64 args.
  std::string callRt(const std::string& sym, std::vector<std::string> args) {
    std::string r = T();
    std::string s = "  " + r + " = call i64 @" + sym + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i) s += ", ";
      s += "i64 " + args[i];
    }
    s += ")";
    emit(s);
    return r;
  }
  // Runtime call with mixed arg types: tys like "ipi" (i64/ptr per arg).
  std::string callRtA(const std::string& sym, const std::string& tys,
                      const std::vector<std::string>& args) {
    std::string r = T();
    std::string s = "  " + r + " = call i64 @" + sym + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i) s += ", ";
      s += (tys[i] == 'p' ? "ptr " : "i64 ") + args[i];
    }
    s += ")";
    emit(s);
    return r;
  }
  // Build an [n x i64] argv array on the stack; returns (ptrval, baseIdx0).
  // Stores are emitted immediately; caller emits the call afterwards.
  std::string argvArray(int n) {
    std::string p = alloca("[" + std::to_string(n) + " x i64]");
    return p;
  }
  void argvStore(const std::string& arr, int idx, const std::string& val) {
    std::string g = T();
    emit("  " + g + " = getelementptr " + arrType(arr) + ", ptr " + arr +
         ", i64 0, i64 " + std::to_string(idx));
    emit("  store i64 " + val + ", ptr " + g);
  }
  // Alloca type recorded in pendingAllocas_.
  std::string arrType(const std::string& arr) {
    for (auto& kv : pendingAllocas_)
      if (kv.first == arr) return kv.second;
    return "[1 x i64]";
  }
  // Full call to an A function value symbol: argv[0]=A_NONE, argv[1..]=args.
  // args has one entry per param slot; empty string = kMissing.
  std::string callAFunction(const std::string& cname,
                            const std::vector<std::string>& args) {
    int n = (int)args.size() + 1;
    std::string av = argvArray(n);
    argvStore(av, 0, kNone);
    for (int i = 0; i < (int)args.size(); ++i)
      argvStore(av, i + 1, args[i].empty() ? kMissing : args[i]);
    std::string r = T();
    emit("  " + r + " = call i64 @" + cname + "(ptr " + av + ")");
    return r;
  }
  // Runtime call taking (…, ptr argv): a_bi_call/a_call/a_method style.
  std::string callRtArgv(const std::string& sym,
                         const std::vector<std::string>& prefix,
                         const std::vector<std::string>& args) {
    int n = (int)args.size();
    std::string av = argvArray(n > 0 ? n : 1);
    for (int i = 0; i < n; ++i) argvStore(av, i, args[i]);
    std::vector<std::string> all = prefix;
    all.push_back(n == 0 ? "null" : av);
    // callRt emits i64 args only; for mixed signatures emit manually.
    std::string r = T();
    std::string s = "  " + r + " = call i64 @" + sym + "(";
    bool first = true;
    for (const std::string& a : all) {
      if (!first) s += ", ";
      first = false;
      // prefix entries are raw type+value pairs handled by caller via "i64 v";
      s += a;
    }
    s += ")";
    emit(s);
    return r;
  }

  // ---- registration (mirrors AsmEmitter::collectGlobals) --------------------
  std::int64_t evalConstIntA(const Expr* e, std::int64_t fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::IntLit)
      return static_cast<const IntLitExpr*>(e)->value;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      if (u->op == "-") return -evalConstIntA(u->operand.get(), 0);
    }
    if (e->kind == ExprKind::Ident) {
      auto it = enumShortVals_.find(static_cast<const IdentExpr*>(e)->name);
      if (it != enumShortVals_.end()) return it->second;
    }
    return fallback;
  }
  std::unordered_map<std::string, std::int64_t> enumShortVals_;

  void collectGlobals() {
    std::unordered_map<std::string, int> seenSym;
    auto symWithSuffix = [&](const std::string& base) -> std::string {
      int n = seenSym[base]++;
      return n == 0 ? base : base + "_" + std::to_string(n);
    };
    for (const Module& mod : graph_.modules)
      if (mod.id >= 0) (void)moduleTag(mod.id);
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          FnInfo* info = new FnInfo();
          info->decl = fn;
          info->name = fn->name;
          info->moduleId = mod.id;
          std::string mtag = moduleTag(mod.id);
          std::size_t arity = fn->comptimeParams.size() + fn->params.size();
          std::string dh = stableTag(fn->name + "#" + std::to_string(arity));
          info->cname = symWithSuffix("__a_f_" + mtag + "_" + dh);
          info->globalVar = symWithSuffix("__a_gf_" + mtag + "_" + dh);
          for (const ComptimeParam& p : fn->comptimeParams)
            info->paramNames.push_back(p.name);
          for (const ParamDecl& p : fn->params) info->paramNames.push_back(p.name);
          info->variadic = !fn->params.empty() && fn->params.back().variadic;
          info->isEntry = fn->isEntry;
          fns_[fn] = info;
          fnList_.push_back(info);
        } else if (s->kind == StmtKind::StructDecl) {
          const StructDecl* st = static_cast<const StructDecl*>(s.get());
          StructInfo* info = new StructInfo();
          info->decl = st;
          info->name = st->name;
          info->moduleId = mod.id;
          std::string mtag = moduleTag(mod.id);
          std::string sh = stableTag(st->name);
          info->ctorCname = symWithSuffix("__a_ctor_" + mtag + "_" + sh);
          info->globalVar = symWithSuffix("__a_gs_" + mtag + "_" + sh);
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
            std::size_t arity = m->comptimeParams.size() + m->params.size();
            std::string dh = stableTag(m->name + "#" + std::to_string(arity));
            mi->cname = symWithSuffix("__a_m_" + mtag + "_" + sh + "_" + dh);
            mi->globalVar = symWithSuffix("__a_gf_" + mtag + "_" + sh + "_" + dh);
            for (const ComptimeParam& p : m->comptimeParams)
              mi->paramNames.push_back(p.name);
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
            std::string mtag = moduleTag(mod.id);
            ev.globalVar = symWithSuffix("__a_ev_" + mtag + "_" +
                                         stableTag(e->name) + "_" +
                                         stableTag(v.name));
            for (const FieldDecl& f : v.fields) ev.payloadFields.push_back(f.name);
            enums_[std::to_string(mod.id) + ":" + e->name + "." + v.name] = ev;
            enumShortVals_[v.name] = ev.value;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          std::string cname = symWithSuffix("__a_top_" + moduleTag(mod.id) + "_" +
                                            stableTag(let->name));
          globals_.push_back(cname);
          topLevelGlobals_[std::to_string(mod.id) + ":" + let->name] = cname;
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            std::string key = std::to_string(mod.id) + ":" + nm;
            if (!topLevelGlobals_.count(key)) {
              std::string cname = symWithSuffix("__a_top_" + moduleTag(mod.id) + "_" +
                                                stableTag(nm));
              globals_.push_back(cname);
              topLevelGlobals_[key] = cname;
            }
          }
        }
      }
    }
    // Merge inherited fields/methods from baseClass chains (far ancestor
    // first, own last — mirrors Sema's view for ctor arity).
    for (StructInfo* info : structList_) {
      if (!info->decl || info->decl->baseClass.empty()) continue;
      std::vector<std::string> baseFields;
      std::vector<std::pair<std::string, FnInfo*>> baseMethods;
      std::set<std::string> seen{info->name};
      std::string base = info->decl->baseClass;
      std::vector<const StructDecl*> chain;
      while (!base.empty() && seen.insert(base).second) {
        StructInfo* bi = nullptr;
        for (StructInfo* cand : structList_)
          if (cand->name == base && cand->moduleId == info->moduleId) {
            bi = cand;
            break;
          }
        if (!bi && structs_.count(base)) bi = structs_[base];
        if (!bi || !bi->decl) break;
        chain.push_back(bi->decl);
        base = bi->decl->baseClass;
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const FieldDecl& f : (*it)->fields)
          baseFields.push_back(f.name);
        for (const auto& m : (*it)->methods) {
          auto mi = fns_.find(m.get());
          if (mi != fns_.end()) baseMethods.push_back({m->name, mi->second});
        }
      }
      info->fieldNames.insert(info->fieldNames.begin(), baseFields.begin(),
                              baseFields.end());
      for (auto& bm : baseMethods)
        if (!info->methods.count(bm.first)) info->methods[bm.first] = bm.second;
    }
  }

  // ---- scope / bindings -----------------------------------------------------
  void pushScope() { scopes_.emplace_back(); }
  void popScope() {
    deferStack_.pop_back();
    scopes_.pop_back();
  }
  LlBinding* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void bind(const std::string& name, const LlBinding& b) {
    if (scopes_.empty()) pushScope();
    scopes_.back()[name] = b;
  }

  // Global scope: top-level lets/assigns, fn values, struct ctors, enum
  // values, module names, native-module stubs (mirrors buildGlobalScope).
  void resetGlobalScope(int preferModule) {
    scopes_.clear();
    scopes_.emplace_back();
    auto& g = scopes_.back();
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        std::string nm;
        if (s->kind == StmtKind::Let)
          nm = static_cast<const LetStmt*>(s.get())->name;
        else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident)
            nm = static_cast<const IdentExpr*>(as->target.get())->name;
        }
        if (nm.empty()) continue;
        auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + nm);
        if (it == topLevelGlobals_.end()) continue;
        LlBinding b;
        b.kind = BKind::Global;
        b.sym = it->second;
        if (!g.count(nm)) g[nm] = b;
      }
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      LlBinding b;
      b.kind = BKind::FnValue;
      b.sym = f->globalVar;
      b.fn = f;
      if (!g.count(f->name)) g[f->name] = b;
    }
    for (StructInfo* st : structList_) {
      LlBinding b;
      b.kind = BKind::StructCtor;
      b.sym = st->globalVar;
      b.st = st;
      if (!g.count(st->name)) g[st->name] = b;
    }
    for (auto& kv : enums_) {
      LlBinding b;
      b.kind = BKind::EnumValue;
      b.sym = kv.second.globalVar;
      b.en = &kv.second;
      std::string qual = kv.second.enumName + "." + kv.second.variantName;
      if (!g.count(qual)) g[qual] = b;
      if (!g.count(kv.second.variantName)) g[kv.second.variantName] = b;
    }
    for (const Module& mod : graph_.modules) {
      LlBinding b;
      b.kind = BKind::Module;
      b.moduleId = mod.id;
      if (!g.count(mod.name) && builtinRuntimeId(mod.name) < 0)
        g[mod.name] = b;
      std::string stem = stemOf(mod.path);
      if (!g.count(stem) && builtinRuntimeId(stem) < 0) g[stem] = b;
    }
    for (const char* nm :
         {"math", "io", "fs", "strings", "collections", "core", "time", "json",
          "binary", "testing", "log", "metrics", "trace", "net", "http",
          "terminal", "env", "repl", "kernels", "unsafe", "arena", "std",
          "py"}) {
      if (g.count(nm)) continue;
      LlBinding b;
      b.kind = BKind::Module;
      b.moduleId = -1;
      b.aux = nm;
      g[nm] = b;
    }
    // Re-bind the preferred module's own globals last (same-name shadowing).
    if (preferModule >= 0 && preferModule < (int)graph_.modules.size()) {
      const Module& pm = graph_.at(preferModule);
      if (pm.program) {
        for (const StmtPtr& s : pm.program->statements) {
          std::string nm;
          if (s->kind == StmtKind::Let)
            nm = static_cast<const LetStmt*>(s.get())->name;
          else if (s->kind == StmtKind::Assign) {
            const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
            if (as->target->kind == ExprKind::Ident)
              nm = static_cast<const IdentExpr*>(as->target.get())->name;
          }
          if (nm.empty()) continue;
          auto it = topLevelGlobals_.find(std::to_string(preferModule) + ":" + nm);
          if (it == topLevelGlobals_.end()) continue;
          LlBinding b;
          b.kind = BKind::Global;
          b.sym = it->second;
          g[nm] = b;
        }
      }
    }
  }

  // ---- imports ---------------------------------------------------------------
  void bindNativeFromImport(const FromImportStmt* im) {
    for (const auto& item : im->items) {
      if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
        int bid = builtinRuntimeId(*bn);
        if (bid < 0) continue;
        LlBinding b;
        b.kind = BKind::Builtin;
        b.builtinId = bid;
        scopes_.back()[item.alias] = b;
      } else if (const std::string* cn = nativeConstant(im->bareName, item.name)) {
        LlBinding b;
        b.kind = BKind::NativeConst;
        b.aux = *cn;
        scopes_.back()[item.alias] = b;
      }
    }
  }
  void bindFileModuleFromImport(const FromImportStmt* im) {
    const Module& src = graph_.at(im->moduleId);
    if (!src.program) return;
    for (const auto& item : im->items) {
      for (const StmtPtr& ss : src.program->statements) {
        if (ss->kind == StmtKind::FnDecl) {
          const FnDecl* fd = static_cast<const FnDecl*>(ss.get());
          if (fd->name != item.name) continue;
          auto it = fns_.find(fd);
          if (it != fns_.end()) {
            LlBinding b;
            b.kind = BKind::FnValue;
            b.sym = it->second->globalVar;
            b.fn = it->second;
            scopes_.back()[item.alias] = b;
          }
        } else if (ss->kind == StmtKind::StructDecl) {
          const StructDecl* sd = static_cast<const StructDecl*>(ss.get());
          if (sd->name != item.name) continue;
          auto it = structs_.find(sd->name);
          if (it != structs_.end() && it->second->moduleId == src.id) {
            LlBinding b;
            b.kind = BKind::StructCtor;
            b.sym = it->second->globalVar;
            b.st = it->second;
            scopes_.back()[item.alias] = b;
          }
        } else if (ss->kind == StmtKind::Let || ss->kind == StmtKind::Assign) {
          std::string vn;
          if (ss->kind == StmtKind::Let)
            vn = static_cast<const LetStmt*>(ss.get())->name;
          else {
            const AssignStmt* as = static_cast<const AssignStmt*>(ss.get());
            if (as->target->kind != ExprKind::Ident) continue;
            vn = static_cast<const IdentExpr*>(as->target.get())->name;
          }
          if (vn != item.name) continue;
          auto it = topLevelGlobals_.find(std::to_string(src.id) + ":" + vn);
          if (it != topLevelGlobals_.end()) {
            LlBinding b;
            b.kind = BKind::Global;
            b.sym = it->second;
            scopes_.back()[item.alias] = b;
          }
        } else if (ss->kind == StmtKind::EnumDecl) {
          const EnumDecl* ed = static_cast<const EnumDecl*>(ss.get());
          for (const EnumVariant& v : ed->variants) {
            if (v.name != item.name) continue;
            auto it = enums_.find(std::to_string(src.id) + ":" + ed->name + "." + v.name);
            if (it != enums_.end()) {
              LlBinding b;
              b.kind = BKind::EnumValue;
              b.sym = it->second.globalVar;
              b.en = &it->second;
              scopes_.back()[item.alias] = b;
            }
          }
        }
      }
    }
  }
  void bindModuleImports(int moduleId) {
    if (moduleId < 0) return;
    const Module& mod = graph_.at(moduleId);
    if (!mod.program) return;
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          std::string alias = !im->alias.empty() ? im->alias : stemOf(im->pythonModule);
          LlBinding b;
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
          for (const Module& cand : graph_.modules) {
            std::string pkg = stemOf(cand.path);
            if (pkg == "__init__" || pkg.empty()) {
              auto slash = cand.path.find_last_of('/');
              if (slash != std::string::npos) {
                auto slash2 = cand.path.find_last_of('/', slash - 1);
                pkg = stemOf(cand.path.substr(
                    slash2 == std::string::npos ? 0 : slash2 + 1,
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
          LlBinding b;
          b.kind = BKind::Module;
          b.moduleId = mid;
          scopes_.back()[alias] = b;
        } else if (im->nativeModule) {
          LlBinding b;
          b.kind = BKind::Module;
          b.moduleId = -1;
          b.aux = im->bareName;
          scopes_.back()[alias] = b;
        }
      } else if (s->kind == StmtKind::FromImport) {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          for (const auto& item : im->items) {
            LlBinding b;
            b.kind = BKind::PyFunc;
            b.aux = im->pythonModule;
            b.pyMember = item.name;
            scopes_.back()[item.alias] = b;
          }
          continue;
        }
        if (im->moduleId >= 0)
          bindFileModuleFromImport(im);
        else if (im->nativeModule)
          bindNativeFromImport(im);
      }
    }
  }

  // ---- declarations ----------------------------------------------------------
  void emitPreamble() {
    pre_ << "target triple = \"" << hostTriple() << "\"\n\n";
    static const char* kDecls = R"LLVM(
declare i64 @a_add_slow(i64, i64)
declare i64 @a_sub_slow(i64, i64)
declare i64 @a_mul_slow(i64, i64)
declare i64 @a_div_slow(i64, i64)
declare i64 @a_floordiv_slow(i64, i64)
declare i64 @a_mod_slow(i64, i64)
declare i64 @a_pow(i64, i64)
declare i64 @a_eq_slow(i64, i64)
declare i64 @a_ne_slow(i64, i64)
declare i64 @a_lt_slow(i64, i64)
declare i64 @a_le_slow(i64, i64)
declare i64 @a_gt_slow(i64, i64)
declare i64 @a_ge_slow(i64, i64)
declare i64 @a_band_slow(i64, i64)
declare i64 @a_bor_slow(i64, i64)
declare i64 @a_bxor_slow(i64, i64)
declare i64 @a_shl_slow(i64, i64)
declare i64 @a_shr_slow(i64, i64)
declare i64 @a_neg_slow(i64)
declare i64 @a_bnot_slow(i64)
declare i64 @a_not(i64)
declare i64 @a_truthy_raw_slow(i64)
declare i64 @a_add_fast(i64, i64)
declare i64 @a_sub_fast(i64, i64)
declare i64 @a_mul_fast(i64, i64)
declare i64 @a_div_fast(i64, i64)
declare i64 @a_floordiv_fast(i64, i64)
declare i64 @a_mod_fast(i64, i64)
declare i64 @a_eq_fast(i64, i64)
declare i64 @a_ne_fast(i64, i64)
declare i64 @a_lt_fast(i64, i64)
declare i64 @a_le_fast(i64, i64)
declare i64 @a_gt_fast(i64, i64)
declare i64 @a_ge_fast(i64, i64)
declare i64 @a_band_fast(i64, i64)
declare i64 @a_bor_fast(i64, i64)
declare i64 @a_bxor_fast(i64, i64)
declare i64 @a_shl_fast(i64, i64)
declare i64 @a_shr_fast(i64, i64)
declare i64 @a_neg_fast(i64)
declare i64 @a_bnot_fast(i64)
declare i64 @a_truthy_raw_fast(i64)
declare i64 @a_pow_fast(i64, i64)
declare i64 @a_unwrap(i64)
declare i64 @a_some_new(i64)
declare i64 @a_try_result(i64)
declare i32 @a_is_option(i64)
declare i32 @a_is_propagate(i64)
declare i64 @a_str(i64)
declare i64 @a_repr(i64)
declare i64 @a_len(i64)
declare i64 @a_int(i64)
declare i64 @a_float(i64)
declare i64 @a_bool(i64)
declare i64 @a_type_name(i64)
declare i64 @a_int_full(i64)
declare i64 @a_i64n(i64)
declare i64 @a_str_new(ptr, i64)
declare i64 @a_char_new(ptr, i64)
declare i64 @a_str_concat(i64, i64)
declare i64 @a_str_join(i64, ptr)
declare i64 @a_str_joinsep(i64, i64, ptr)
declare i64 @a_list_new(i64, ptr)
declare i64 @a_list_append(i64, i64)
declare i64 @a_list_append_int(i64, i64)
declare i64 @a_list_from_iter(i64)
declare i64 @a_tuple_new(i64, ptr)
declare i64 @a_dict_new()
declare i64 @a_dict_set(i64, i64, i64)
declare i64 @a_dict_get(i64, i64, i64)
declare i64 @a_dict_has(i64, i64)
declare i64 @a_set_new()
declare i64 @a_set_add(i64, i64)
declare i64 @a_range_new(i64, i64, i64)
declare i64 @a_getitem(i64, i64)
declare i64 @a_setitem(i64, i64, i64)
declare i64 @a_slice(i64, i64, i64, i64)
declare i64 @a_iter_new(i64)
declare i64 @a_iter_next(i64, ptr)
declare i64 @a_contains(i64, i64)
declare i64 @a_in_range(i64, i64, i64)
declare i64 @a_in_range_inclusive(i64, i64, i64)
declare i64 @a_fnon_new(i64, i64, ptr)
declare i64 @a_fnon_new_ex(i64, i64, ptr, ptr, ptr)
declare i64 @a_closure_env_new(i64)
declare i64 @a_env_set(i64, i64, i64)
declare i64 @a_env_get(i64, i64)
declare i64 @a_struct_new(i64, i64, i64)
declare i64 @a_builtin(i64)
declare i64 @a_bi_call(i64, i64, ptr)
declare i64 @a_call(i64, i64, ptr)
declare i64 @a_method(i64, ptr, i64, i64, ptr)
declare i64 @a_getattr(i64, ptr)
declare i64 @a_setattr(i64, ptr, i64)
declare i64 @a_promote(i64)
declare i64 @a_panic_msg(ptr)
declare i64 @a_py_import_module(ptr)
declare i64 @a_py_getattr(i64, ptr)
declare i64 @a_py_setattr(i64, ptr, i64)
declare i64 @a_printv(i64, ptr)
declare i64 @a_eprintv(i64, ptr)
declare i64 @a_join_len(i64, i64)
declare i64 @a_match_fail(i64)
declare void @a_frame_push(ptr, ptr, i32)
declare void @a_frame_pop()
declare void @a_runtime_init()
declare i32 @a_test_main(i64, ptr, ptr)
@a_stmt_line = external global i32
)LLVM";
    pre_ << kDecls << "\n";
    // Internal truthiness helper: i1 version of a_truthy_raw.
    helpers_ << R"LLVM(define internal i1 @a.truthy(i64 %v) {
entry:
  %tag = and i64 %v, -281474976710656
  %isT = icmp eq i64 %tag, -281474976710656
  %pay = and i64 %v, 281474976710655
  %sh = lshr i64 %pay, 1
  %sg = shl i64 %sh, 17
  %iv = ashr i64 %sg, 17
  %odd = icmp ne i64 %v, 0
  %low1 = and i64 %v, 1
  %low = icmp ne i64 %low1, 0
  %isI = and i1 %isT, %low
  br i1 %isI, label %isint, label %notint
isint:
  %nz = icmp ne i64 %iv, 0
  ret i1 %nz
notint:
  %isD = icmp ne i64 %tag, -281474976710656
  br i1 %isD, label %isdbl, label %tagged
isdbl:
  %d = bitcast i64 %v to double
  %t = fcmp une double %d, 0.0
  ret i1 %t
tagged:
  %cT = icmp eq i64 %v, -281474976710650
  %cF = icmp eq i64 %v, -281474976710652
  %cN = icmp eq i64 %v, -281474976710654
  %h1 = or i1 %cT, %cF
  %h2 = or i1 %h1, %cN
  br i1 %h2, label %small, label %slow
small:
  ret i1 %cT
slow:
  %r = call i64 @a_truthy_raw_fast(i64 %v)
  %rnz = icmp ne i64 %r, 0
  ret i1 %rnz
}

)LLVM";
    emitFastHelpers();
  }

  // ---- numeric fast-path helpers ------------------------------------------
  // LLVM-IR mirrors of compiler/astra/runtime/fastpath.c: the aval tag
  // dispatch happens inside IR so opt/llc can inline it and fold the
  // branches for statically numeric code. Every helper falls back to the
  // same a_*_slow runtime entry the C *_fast functions use, so results are
  // identical to calling the a_*_fast exports directly.
  //
  // aval layout (a_runtime.h): tagged iff u64(v) >= 0xFFFF000000000000
  // (== -281474976710656 as i64); inline int iff tagged && (v & 1), value =
  // sign-extended 47-bit payload>>1; double iff !tagged (raw bits).
  void emitFastHelpers() {
    helpers_ << R"LLVM(declare double @llvm.floor.f64(double)
declare double @fmod(double, double)

define internal i1 @a.isintv(i64 %v) {
  %tag = icmp uge i64 %v, -281474976710656
  %bit = and i64 %v, 1
  %bitnz = icmp ne i64 %bit, 0
  %r = and i1 %tag, %bitnz
  ret i1 %r
}

define internal i1 @a.isnumv(i64 %v) {
  %isi = call i1 @a.isintv(i64 %v)
  %notag = icmp ult i64 %v, -281474976710656
  %r = or i1 %isi, %notag
  ret i1 %r
}

define internal i64 @a.ival(i64 %v) {
  %pay = and i64 %v, 281474976710655
  %sh = lshr i64 %pay, 1
  %sg = shl i64 %sh, 17
  %iv = ashr i64 %sg, 17
  ret i64 %iv
}

define internal double @a.nval(i64 %v) {
entry:
  %isi = call i1 @a.isintv(i64 %v)
  br i1 %isi, label %ii, label %dd
ii:
  %iv = call i64 @a.ival(i64 %v)
  %di = sitofp i64 %iv to double
  ret double %di
dd:
  %dv = bitcast i64 %v to double
  ret double %dv
}

define internal i64 @a.mint(i64 %i) {
  %s = shl i64 %i, 1
  %o = or i64 %s, 1
  %r = or i64 %o, -281474976710656
  ret i64 %r
}

define internal i64 @a.mdouble(double %d) {
  %nan = fcmp uno double %d, %d
  %bits = bitcast double %d to i64
  %r = select i1 %nan, i64 9218868437227405312, i64 %bits
  ret i64 %r
}

define internal i1 @a.inrange(i64 %v) {
  %lo = icmp sge i64 %v, -70368744177664
  %hi = icmp slt i64 %v, 70368744177664
  %r = and i1 %lo, %hi
  ret i1 %r
}

)LLVM";
    // Shared prologue for binary numeric helpers: both operands inline int
    // -> intp, both numeric (int or double) -> dblp, else slow. Identical
    // dispatch to the C *_fast functions.
    auto head = [](const char* name) {
      return std::string("define internal i64 @") + name +
             "(i64 %l, i64 %r) {\n"
             "entry:\n"
             "  %li = call i1 @a.isintv(i64 %l)\n"
             "  %ri = call i1 @a.isintv(i64 %r)\n"
             "  %ln = call i1 @a.isnumv(i64 %l)\n"
             "  %rn = call i1 @a.isnumv(i64 %r)\n"
             "  %bothI = and i1 %li, %ri\n"
             "  br i1 %bothI, label %intp, label %numchk\n"
             "numchk:\n"
             "  %bothN = and i1 %ln, %rn\n"
             "  br i1 %bothN, label %dblp, label %slow\n";
    };
    auto tail = [](const char* slow) {
      return std::string("slow:\n  %s = call i64 @") + slow +
             "(i64 %l, i64 %r)\n  ret i64 %s\n}\n\n";
    };
    // add/sub: 47-bit operands cannot overflow i64, so the C int64-overflow
    // pre-check is dead; only the inline-range check remains.
    auto arithNN = [&](const char* name, const char* slow, const char* iop,
                       const char* fop) {
      helpers_ << head(name) << "intp:\n"
               << "  %a = call i64 @a.ival(i64 %l)\n"
               << "  %b = call i64 @a.ival(i64 %r)\n"
               << "  %res = " << iop << " i64 %a, %b\n"
               << "  %ok = call i1 @a.inrange(i64 %res)\n"
               << "  br i1 %ok, label %mint, label %slow\n"
               << "mint:\n"
               << "  %m = call i64 @a.mint(i64 %res)\n"
               << "  ret i64 %m\n"
               << "dblp:\n"
               << "  %da = call double @a.nval(i64 %l)\n"
               << "  %db = call double @a.nval(i64 %r)\n"
               << "  %dr = " << fop << " double %da, %db\n"
               << "  %bx = call i64 @a.mdouble(double %dr)\n"
               << "  ret i64 %bx\n" << tail(slow);
    };
    arithNN("a.add.fast", "a_add_slow", "add", "fadd");
    arithNN("a.sub.fast", "a_sub_slow", "sub", "fsub");
    // mul: guard |a| <= LIM/|b| before multiplying (i64 mul would poison on
    // overflow), then the inline-range check — same as a_mul_fast.
    helpers_ << head("a.mul.fast") << R"LLVM(intp:
  %a = call i64 @a.ival(i64 %l)
  %b = call i64 @a.ival(i64 %r)
  %az = icmp eq i64 %a, 0
  %bz = icmp eq i64 %b, 0
  %zz = or i1 %az, %bz
  br i1 %zz, label %zero, label %nz
zero:
  ret i64 -281474976710655
nz:
  %an = icmp slt i64 %a, 0
  %na = sub i64 0, %a
  %ua = select i1 %an, i64 %na, i64 %a
  %bn = icmp slt i64 %b, 0
  %nb = sub i64 0, %b
  %ub = select i1 %bn, i64 %nb, i64 %b
  %q = sdiv i64 70368744177664, %ub
  %ok = icmp sle i64 %ua, %q
  br i1 %ok, label %pmul, label %slow
pmul:
  %res = mul i64 %a, %b
  %inr = call i1 @a.inrange(i64 %res)
  br i1 %inr, label %mint, label %slow
mint:
  %m = call i64 @a.mint(i64 %res)
  ret i64 %m
dblp:
  %da = call double @a.nval(i64 %l)
  %db = call double @a.nval(i64 %r)
  %dr = fmul double %da, %db
  %bx = call i64 @a.mdouble(double %dr)
  ret i64 %bx
)LLVM" << tail("a_mul_slow");
    // div: b==0 -> slow (panic path); INT64_MIN/-1 cannot occur for 47-bit
    // inline ints. Double division by 0.0 must go slow (panic, not inf).
    helpers_ << head("a.div.fast") << R"LLVM(intp:
  %a = call i64 @a.ival(i64 %l)
  %b = call i64 @a.ival(i64 %r)
  %bz = icmp eq i64 %b, 0
  br i1 %bz, label %slow, label %go
go:
  %res = sdiv i64 %a, %b
  %ok = call i1 @a.inrange(i64 %res)
  br i1 %ok, label %mint, label %slow
mint:
  %m = call i64 @a.mint(i64 %res)
  ret i64 %m
dblp:
  %da = call double @a.nval(i64 %l)
  %db = call double @a.nval(i64 %r)
  %z = fcmp oeq double %db, 0.0
  br i1 %z, label %slow, label %god
god:
  %dr = fdiv double %da, %db
  %bx = call i64 @a.mdouble(double %dr)
  ret i64 %bx
)LLVM" << tail("a_div_slow");
    // floordiv: truncated sdiv/srem then adjust when signs differ.
    helpers_ << head("a.floordiv.fast") << R"LLVM(intp:
  %a = call i64 @a.ival(i64 %l)
  %b = call i64 @a.ival(i64 %r)
  %bz = icmp eq i64 %b, 0
  br i1 %bz, label %slow, label %go
go:
  %q = sdiv i64 %a, %b
  %rem = srem i64 %a, %b
  %rnz = icmp ne i64 %rem, 0
  %an = icmp slt i64 %a, 0
  %bn = icmp slt i64 %b, 0
  %dif = icmp ne i1 %an, %bn
  %adj = and i1 %rnz, %dif
  %q1 = sub i64 %q, 1
  %res = select i1 %adj, i64 %q1, i64 %q
  %ok = call i1 @a.inrange(i64 %res)
  br i1 %ok, label %mint, label %slow
mint:
  %m = call i64 @a.mint(i64 %res)
  ret i64 %m
dblp:
  %da = call double @a.nval(i64 %l)
  %db = call double @a.nval(i64 %r)
  %z = fcmp oeq double %db, 0.0
  br i1 %z, label %slow, label %god
god:
  %dr = fdiv double %da, %db
  %fl = call double @llvm.floor.f64(double %dr)
  %bx = call i64 @a.mdouble(double %fl)
  ret i64 %bx
)LLVM" << tail("a_floordiv_slow");
    // mod: b==-1 -> 0; b==0 -> slow; float mod is fmod with the divisor's
    // sign (adjust when signs differ and result nonzero).
    helpers_ << head("a.mod.fast") << R"LLVM(intp:
  %a = call i64 @a.ival(i64 %l)
  %b = call i64 @a.ival(i64 %r)
  %bm1 = icmp eq i64 %b, -1
  br i1 %bm1, label %zero, label %chk
zero:
  ret i64 -281474976710655
chk:
  %bz = icmp eq i64 %b, 0
  br i1 %bz, label %slow, label %go
go:
  %res = srem i64 %a, %b
  %ok = call i1 @a.inrange(i64 %res)
  br i1 %ok, label %mint, label %slow
mint:
  %m = call i64 @a.mint(i64 %res)
  ret i64 %m
dblp:
  %da = call double @a.nval(i64 %l)
  %db = call double @a.nval(i64 %r)
  %z = fcmp oeq double %db, 0.0
  br i1 %z, label %slow, label %gom
gom:
  %fm = call double @fmod(double %da, double %db)
  %mz = fcmp une double %fm, 0.0
  %mg = fcmp ogt double %fm, 0.0
  %bg = fcmp ogt double %db, 0.0
  %dif = icmp ne i1 %mg, %bg
  %cond = and i1 %mz, %dif
  %sum = fadd double %fm, %db
  %rs = select i1 %cond, double %sum, double %fm
  %bx = call i64 @a.mdouble(double %rs)
  ret i64 %bx
)LLVM" << tail("a_mod_slow");
    // Ordering comparisons: int-int -> icmp, mixed/numeric -> fcmp on the
    // double values (exact for 47-bit ints), else slow.
    auto cmpOrd = [&](const char* name, const char* slow, const char* ipred,
                      const char* fpred) {
      helpers_ << head(name) << "intp:\n"
               << "  %a = call i64 @a.ival(i64 %l)\n"
               << "  %b = call i64 @a.ival(i64 %r)\n"
               << "  %c = icmp " << ipred << " i64 %a, %b\n"
               << "  br i1 %c, label %yes, label %no\n"
               << "yes:\n"
               << "  ret i64 -281474976710650\n"
               << "no:\n"
               << "  ret i64 -281474976710652\n"
               << "dblp:\n"
               << "  %da = call double @a.nval(i64 %l)\n"
               << "  %db = call double @a.nval(i64 %r)\n"
               << "  %c2 = fcmp " << fpred << " double %da, %db\n"
               << "  br i1 %c2, label %yes2, label %no2\n"
               << "yes2:\n"
               << "  ret i64 -281474976710650\n"
               << "no2:\n"
               << "  ret i64 -281474976710652\n" << tail(slow);
    };
    cmpOrd("a.lt.fast", "a_lt_slow", "slt", "olt");
    cmpOrd("a.le.fast", "a_le_slow", "sle", "ole");
    cmpOrd("a.gt.fast", "a_gt_slow", "sgt", "ogt");
    cmpOrd("a.ge.fast", "a_ge_slow", "sge", "oge");
    // eq/ne: int-int and double-double only (no mixed numeric compare —
    // that goes slow, like a_eq_fast); small-const identity shortcut.
    auto cmpEq = [&](const char* name, const char* slow, bool isEq) {
      const char* ip = isEq ? "eq" : "ne";
      const char* fp = isEq ? "oeq" : "une";
      const char* hitRet = isEq ? "-281474976710650" : "-281474976710652";
      helpers_ << "define internal i64 @" << name << "(i64 %l, i64 %r) {\n"
               << "entry:\n"
               << "  %li = call i1 @a.isintv(i64 %l)\n"
               << "  %ri = call i1 @a.isintv(i64 %r)\n"
               << "  %bothI = and i1 %li, %ri\n"
               << "  br i1 %bothI, label %intp, label %dblchk\n"
               << "dblchk:\n"
               << "  %ldt = icmp ult i64 %l, -281474976710656\n"
               << "  %rdt = icmp ult i64 %r, -281474976710656\n"
               << "  %bothD = and i1 %ldt, %rdt\n"
               << "  br i1 %bothD, label %dblp, label %constchk\n"
               << "intp:\n"
               << "  %a = call i64 @a.ival(i64 %l)\n"
               << "  %b = call i64 @a.ival(i64 %r)\n"
               << "  %c = icmp " << ip << " i64 %a, %b\n"
               << "  br i1 %c, label %yes, label %no\n"
               << "yes:\n"
               << "  ret i64 -281474976710650\n"
               << "no:\n"
               << "  ret i64 -281474976710652\n"
               << "dblp:\n"
               << "  %da = bitcast i64 %l to double\n"
               << "  %db = bitcast i64 %r to double\n"
               << "  %c2 = fcmp " << fp << " double %da, %db\n"
               << "  br i1 %c2, label %yes2, label %no2\n"
               << "yes2:\n"
               << "  ret i64 -281474976710650\n"
               << "no2:\n"
               << "  ret i64 -281474976710652\n"
               << "constchk:\n"
               << "  %lr = icmp eq i64 %l, %r\n"
               << "  %cT = icmp eq i64 %l, -281474976710650\n"
               << "  %cF = icmp eq i64 %l, -281474976710652\n"
               << "  %cN = icmp eq i64 %l, -281474976710654\n"
               << "  %k1 = or i1 %cT, %cF\n"
               << "  %k2 = or i1 %k1, %cN\n"
               << "  %hit = and i1 %lr, %k2\n"
               << "  br i1 %hit, label %yes3, label %slow\n"
               << "yes3:\n"
               << "  ret i64 " << hitRet << "\n"
               << tail(slow);
    };
    cmpEq("a.eq.fast", "a_eq_slow", true);
    cmpEq("a.ne.fast", "a_ne_slow", false);
    // Bitwise ops: inline-int operands only; everything else -> slow
    // (no double path, matching a_band_fast & friends).
    auto bitOp = [&](const char* name, const char* slow, const char* op) {
      helpers_ << "define internal i64 @" << name << "(i64 %l, i64 %r) {\n"
               << "entry:\n"
               << "  %li = call i1 @a.isintv(i64 %l)\n"
               << "  %ri = call i1 @a.isintv(i64 %r)\n"
               << "  %bothI = and i1 %li, %ri\n"
               << "  br i1 %bothI, label %intp, label %slow\n"
               << "intp:\n"
               << "  %a = call i64 @a.ival(i64 %l)\n"
               << "  %b = call i64 @a.ival(i64 %r)\n"
               << "  %res = " << op << " i64 %a, %b\n"
               << "  %ok = call i1 @a.inrange(i64 %res)\n"
               << "  br i1 %ok, label %mint, label %slow\n"
               << "mint:\n"
               << "  %m = call i64 @a.mint(i64 %res)\n"
               << "  ret i64 %m\n"
               << "slow:\n"
               << "  %s = call i64 @" << slow << "(i64 %l, i64 %r)\n"
               << "  ret i64 %s\n}\n\n";
    };
    bitOp("a.band.fast", "a_band_slow", "and");
    bitOp("a.bor.fast", "a_bor_slow", "or");
    bitOp("a.bxor.fast", "a_bxor_slow", "xor");
    auto shiftOp = [&](const char* name, const char* slow, const char* op) {
      helpers_ << "define internal i64 @" << name << "(i64 %l, i64 %r) {\n"
               << "entry:\n"
               << "  %li = call i1 @a.isintv(i64 %l)\n"
               << "  %ri = call i1 @a.isintv(i64 %r)\n"
               << "  %bothI = and i1 %li, %ri\n"
               << "  br i1 %bothI, label %intp, label %slow\n"
               << "intp:\n"
               << "  %a = call i64 @a.ival(i64 %l)\n"
               << "  %s = call i64 @a.ival(i64 %r)\n"
               << "  %sok = icmp sge i64 %s, 0\n"
               << "  %sok2 = icmp sle i64 %s, 63\n"
               << "  %rng = and i1 %sok, %sok2\n"
               << "  br i1 %rng, label %go, label %slow\n"
               << "go:\n"
               << "  %res = " << op << " i64 %a, %s\n"
               << "  %ok = call i1 @a.inrange(i64 %res)\n"
               << "  br i1 %ok, label %mint, label %slow\n"
               << "mint:\n"
               << "  %m = call i64 @a.mint(i64 %res)\n"
               << "  ret i64 %m\n"
               << "slow:\n"
               << "  %s2 = call i64 @" << slow << "(i64 %l, i64 %r)\n"
               << "  ret i64 %s2\n}\n\n";
    };
    shiftOp("a.shl.fast", "a_shl_slow", "shl");
    shiftOp("a.shr.fast", "a_shr_slow", "ashr");
    // Unary ops.
    helpers_ << R"LLVM(define internal i64 @a.neg.fast(i64 %v) {
entry:
  %isi = call i1 @a.isintv(i64 %v)
  br i1 %isi, label %ii, label %nd
ii:
  %i = call i64 @a.ival(i64 %v)
  %lim = icmp eq i64 %i, -70368744177664
  br i1 %lim, label %slow, label %go
go:
  %ni = sub i64 0, %i
  %r = call i64 @a.mint(i64 %ni)
  ret i64 %r
nd:
  %notag = icmp ult i64 %v, -281474976710656
  br i1 %notag, label %dd, label %slow
dd:
  %d = bitcast i64 %v to double
  %ndv = fneg double %d
  %r2 = call i64 @a.mdouble(double %ndv)
  ret i64 %r2
slow:
  %s = call i64 @a_neg_slow(i64 %v)
  ret i64 %s
}

define internal i64 @a.bnot.fast(i64 %v) {
entry:
  %isi = call i1 @a.isintv(i64 %v)
  br i1 %isi, label %ii, label %slow
ii:
  %i = call i64 @a.ival(i64 %v)
  %r0 = xor i64 %i, -1
  %ok = call i1 @a.inrange(i64 %r0)
  br i1 %ok, label %mint, label %slow
mint:
  %m = call i64 @a.mint(i64 %r0)
  ret i64 %m
slow:
  %s = call i64 @a_bnot_slow(i64 %v)
  ret i64 %s
}

define internal i64 @a.not.fast(i64 %v) {
entry:
  %isi = call i1 @a.isintv(i64 %v)
  br i1 %isi, label %ii, label %nd
ii:
  %i = call i64 @a.ival(i64 %v)
  %z = icmp eq i64 %i, 0
  %bt = select i1 %z, i64 -281474976710650, i64 -281474976710652
  ret i64 %bt
nd:
  %notag = icmp ult i64 %v, -281474976710656
  br i1 %notag, label %dd, label %cc
dd:
  %d = bitcast i64 %v to double
  %z2 = fcmp oeq double %d, 0.0
  %bt2 = select i1 %z2, i64 -281474976710650, i64 -281474976710652
  ret i64 %bt2
cc:
  %cN = icmp eq i64 %v, -281474976710654
  %cF = icmp eq i64 %v, -281474976710652
  %k = or i1 %cN, %cF
  br i1 %k, label %yes, label %ct
yes:
  ret i64 -281474976710650
ct:
  %cT = icmp eq i64 %v, -281474976710650
  br i1 %cT, label %no, label %slow
no:
  ret i64 -281474976710652
slow:
  %s = call i64 @a_not(i64 %v)
  ret i64 %s
}

)LLVM";
  }
  static std::string hostTriple() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64-unknown-linux-gnu";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386-unknown-linux-gnu";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64-unknown-linux-gnu";
#else
    return "";
#endif
  }

  // ---- functions -------------------------------------------------------------
  void emitFunction(FnInfo* f) {
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    resetGlobalScope(f->moduleId);
    pushScope();
    bindModuleImports(f->moduleId);
    int savedModule = curModuleId_;
    curModuleId_ = f->moduleId;
    inFunctionBody_ = true;
    // Params: load argv[i+1] into a fresh i64 alloca each.
    std::vector<std::string> paramSlots;
    for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
      std::string slot = alloca("i64");
      std::string g = T(), v = T();
      emit("  " + g + " = getelementptr i64, ptr %argv, i64 " +
           std::to_string(i + 1));
      emit("  " + v + " = load i64, ptr " + g);
      emit("  store i64 " + v + ", ptr " + slot);
      paramSlots.push_back(slot);
      LlBinding b;
      b.kind = BKind::Param;
      b.slot = slot;
      scopes_.back()[f->paramNames[i]] = b;
    }
    // Default params: argv slot == kMissing -> evaluate default.
    if (f->decl) {
      std::size_t offset = f->paramNames.size() - f->decl->params.size();
      for (std::size_t i = offset; i < f->paramNames.size(); ++i) {
        const ParamDecl& p = f->decl->params[i - offset];
        if (!p.defaultValue) continue;
        LlBinding* pb = lookup(f->paramNames[i]);
        if (!pb || pb->kind != BKind::Param) continue;
        std::string skip = newLabel("def");
        std::string cur = T(), isMiss = T();
        emit("  " + cur + " = load i64, ptr " + pb->slot);
        emit("  " + isMiss + " = icmp eq i64 " + cur + ", " + kMissing);
        emit("  br i1 " + isMiss + ", label %" + skip + ".t, label %" +
             skip + ".e");
        emit("  " + skip + ".t:");
        std::string dv = eval(p.defaultValue.get());
        emit("  store i64 " + dv + ", ptr " + pb->slot);
        emit("  br label %" + skip + ".e");
        emit("  " + skip + ".e:");
      }
    }
    inFunctionBody_ = true;
    emitFramePush(f->isMethod ? f->structName + "." + f->name : f->name,
                  f->moduleId, f->decl ? f->decl->span.line : 0);
    deferStack_.emplace_back();
    emitStmts(f->decl->body);
    if (!term_) {
      emitDefers();
      emitFramePop();
      emit("  ret i64 " + kNone);
    }
    term_ = false;
    clearDeferFrames();
    finishFunctionBody(f->cname);
    inFunctionBody_ = false;
    curModuleId_ = savedModule;
    scopes_.clear();
  }

  void finishFunctionBody(const std::string& cname) {
    // Emit collected allocas at top of fn_, then the body.
    std::ostringstream head;
    for (auto& kv : pendingAllocas_)
      head << "  " << kv.first << " = alloca " << kv.second << "\n";
    std::string body = fn_.str();
    // Strip the placeholder alloca ptr line emitted earlier (see emitFunction).
    body_ << "define i64 @" << cname << "(ptr %argv) {\n"
           << head.str() << body << "}\n\n";
  }

  void emitFramePush(const std::string& name, int moduleId, int line) {
    std::string nf = cstrGlobal(name, true);
    std::string ff = cstrGlobal(moduleFile(moduleId), true);
    std::string r = T();
    (void)r;
    emit("  call void @a_frame_push(ptr " + nf + ", ptr " +
         ff + ", i32 " + std::to_string(line) + ")");
  }
  void emitFramePop() { emit("  call void @a_frame_pop()"); }
  // Scope exit: run this frame's deferred stmts LIFO, then its RAII drops
  // LIFO, and clear the frame (popScope removes the emptied frame).
  void emitDefers() {
    if (deferStack_.empty()) return;
    ScopeFrame frame = deferStack_.back();  // copy: bodies may push defers
    deferStack_.back().defers.clear();
    deferStack_.back().drops.clear();
    for (auto it = frame.defers.rbegin(); it != frame.defers.rend(); ++it)
      if (*it) emitStmt(**it);
    for (auto it = frame.drops.rbegin(); it != frame.drops.rend(); ++it)
      emitDropRec(*it);
  }
  // Return/jump path: emit defers+drops of frames [depth, size)
  // innermost-first WITHOUT popping (mirrors AsmEmitter::emitDeferredPeek*).
  void emitDeferredPeekDownTo(int depth) {
    if (depth < 0) depth = 0;
    std::vector<ScopeFrame> frames(deferStack_.begin() + depth,
                                   deferStack_.end());
    for (auto fi = frames.rbegin(); fi != frames.rend(); ++fi) {
      for (auto it = fi->defers.rbegin(); it != fi->defers.rend(); ++it)
        if (*it) emitStmt(**it);
      for (auto it = fi->drops.rbegin(); it != fi->drops.rend(); ++it)
        emitDropRec(*it);
      if (failed_) return;
    }
  }
  void emitDeferredPeekAll() { emitDeferredPeekDownTo(0); }
  // Drop every pending defer/drop record. Called at the end of each
  // function-like emission so frames left behind by bodies ending in an
  // unconditional transfer (return/break -> term_) cannot leak into the
  // next function's emitDeferredPeekAll.
  void clearDeferFrames() {
    for (ScopeFrame& f : deferStack_) {
      f.defers.clear();
      f.drops.clear();
    }
  }

  // ---- RAII drops (mirrors AsmEmitter) ---------------------------------------
  StructInfo* findStructByNameLl(const std::string& name) const {
    if (curModuleId_ >= 0) {
      for (StructInfo* st : structList_)
        if (st->name == name && st->moduleId == curModuleId_) return st;
    }
    for (StructInfo* st : structList_)
      if (st->name == name) return st;
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : it->second;
  }
  // If `e`'s type is a struct with a `drop` method, return its FnInfo.
  FnInfo* dropFnFor(const Expr* e) {
    if (!e) return nullptr;
    TypePtr t = resolveType(e->inferredType);
    if (!t || t->kind != TypeKind::Struct) return nullptr;
    StructInfo* st = findStructByNameLl(shortName(t->structName));
    if (!st) return nullptr;
    auto mit = st->methods.find("drop");
    return mit == st->methods.end() ? nullptr : mit->second;
  }
  void emitDropRec(const DropRec& d) {
    if (!d.fn || failed_) return;
    std::string self = T();
    if (!d.sym.empty())
      emit("  " + self + " = load i64, ptr @" + d.sym);
    else
      emit("  " + self + " = load i64, ptr " + d.slot);
    (void)callAFunction(d.fn->cname, {self});
  }
  // Find the pending drop record for a binding across all live frames.
  DropRec* findDropRec(const LlBinding& b) {
    for (auto fi = deferStack_.rbegin(); fi != deferStack_.rend(); ++fi) {
      for (auto it = fi->drops.rbegin(); it != fi->drops.rend(); ++it) {
        if (b.kind == BKind::Local || b.kind == BKind::Param ||
            b.kind == BKind::Capture) {
          if (it->sym.empty() && it->slot == b.slot) return &*it;
        } else if (!b.sym.empty() && it->sym == b.sym) {
          return &*it;
        }
      }
    }
    return nullptr;
  }
  // Register a drop record for a freshly bound value (mirrors the
  // conditions in AsmEmitter::emitStmt/emitAssign).
  void recordDrop(const LlBinding& b, FnInfo* dropFn) {
    if (!dropFn || deferStack_.empty()) return;
    if (!inFunctionBody_ &&
        (b.kind == BKind::Local || b.kind == BKind::Global)) {
      if (b.kind == BKind::Global)
        deferStack_.back().drops.push_back({"", b.sym, dropFn});
      else
        deferStack_.back().drops.push_back({b.slot, "", dropFn});
    } else if (inFunctionBody_ && b.kind == BKind::Local) {
      deferStack_.back().drops.push_back({b.slot, "", dropFn});
    }
  }

  std::string moduleFile(int moduleId) {
    if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
      return graph_.at(moduleId).path;
    return "<unknown>";
  }

  // ---- statements ------------------------------------------------------------
  void emitStmts(const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& s : stmts) {
      if (failed_) return;
      emitStmt(*s);
    }
  }
  void emitStmt(const Stmt& s) {
    if (term_) {
      emit("  " + newLabel("dead") + ":");
      term_ = false;
    }
    // Line tracking for panic tracebacks (mirrors a_stmt_line stores).
    emit("  store i32 " + std::to_string(s.span.line) + ", ptr @a_stmt_line");
    switch (s.kind) {
      case StmtKind::Let: emitLet(static_cast<const LetStmt&>(s)); break;
      case StmtKind::Assign: emitAssign(static_cast<const AssignStmt&>(s)); break;
      case StmtKind::Expr: {
        const auto& e = static_cast<const ExprStmt&>(s);
        if (e.expr) (void)eval(e.expr.get());
        break;
      }
      case StmtKind::Destructure: emitDestructure(static_cast<const DestructureStmt&>(s)); break;
      case StmtKind::Return: {
        const auto& r = static_cast<const ReturnStmt&>(s);
        std::string v = r.value ? eval(r.value.get()) : kNone;
        emitDeferredPeekAll();
        emitFramePop();
        emit("  ret i64 " + v);
        term_ = true;
        break;
      }
      case StmtKind::Break: {
        if (loopBreak_.empty()) { fail("'break' outside loop"); return; }
        emitDeferredPeekDownTo(
            loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back());
        emit("  br label %" + loopBreak_.back());
        term_ = true;
        break;
      }
      case StmtKind::Continue: {
        if (loopCont_.empty()) { fail("'continue' outside loop"); return; }
        emitDeferredPeekDownTo(
            loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back());
        emit("  br label %" + loopCont_.back());
        term_ = true;
        break;
      }
      case StmtKind::Defer:
        if (deferStack_.empty()) deferStack_.emplace_back();
        deferStack_.back().defers.push_back(
            static_cast<const DeferStmt&>(s).body.get());
        break;
      case StmtKind::If: emitIf(static_cast<const IfStmt&>(s)); break;
      case StmtKind::While: emitWhile(static_cast<const WhileStmt&>(s)); break;
      case StmtKind::DoWhile: emitDoWhile(static_cast<const DoWhileStmt&>(s)); break;
      case StmtKind::For: emitFor(static_cast<const ForStmt&>(s)); break;
      case StmtKind::Loop: emitLoop(static_cast<const LoopStmt&>(s)); break;
      // Declarations/imports are registered globally; nothing to emit.
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::TypeAlias:
      case StmtKind::Import:
      case StmtKind::FromImport:
        break;
    }
  }

  void emitLet(const LetStmt& let) {
    std::string v = let.init ? eval(let.init.get()) : kNone;
    FnInfo* dropFn = dropFnFor(let.init.get());
    // Top-level lets in a module init store into their global slot.
    if (curModuleId_ >= 0 && !inFunctionBody_) {
      auto it = topLevelGlobals_.find(std::to_string(curModuleId_) + ":" + let.name);
      if (it != topLevelGlobals_.end()) {
        emit("  store i64 " + v + ", ptr @" + it->second);
        LlBinding b;
        b.kind = BKind::Global;
        b.sym = it->second;
        b.dropFn = dropFn;
        bind(let.name, b);
        recordDrop(b, dropFn);
        return;
      }
    }
    std::string slot = alloca("i64");
    emit("  store i64 " + v + ", ptr " + slot);
    LlBinding b;
    b.kind = BKind::Local;
    b.slot = slot;
    b.dropFn = dropFn;
    bind(let.name, b);
    recordDrop(b, dropFn);
  }
  bool inFunctionBody_ = false;

  void emitAssign(const AssignStmt& as) {
    const Expr* target = as.target.get();
    std::string v;
    if (as.op == "=") {
      v = eval(as.value.get());
    } else {
      // Compound assign: target op= value.
      std::string cur;
      if (target->kind == ExprKind::Ident) {
        const std::string& nm = static_cast<const IdentExpr*>(target)->name;
        LlBinding* b = lookup(nm);
        if (!b) { fail("undefined variable '" + nm + "'"); return; }
        cur = bindingValue(*b);
      } else {
        cur = eval(target);
      }
      std::string rv = eval(as.value.get());
      v = binopValue(as.binop, cur, rv, as.opSpan);
      if (v.empty()) return;
    }
    // Store back.
    if (target->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr*>(target)->name;
      LlBinding* b = lookup(nm);
      // Non-lvalue bindings (module/native/builtin/python names) do not
      // accept assignment: a bare `name = v` shadows them with a new local
      // (mirrors Python-style rebinding; fixes `dirs = []` shadowing the
      // `dirs` module stem).
      if (b && (b->kind == BKind::Module || b->kind == BKind::NativeConst ||
                b->kind == BKind::Builtin || b->kind == BKind::PyFunc ||
                b->kind == BKind::PyModule))
        b = nullptr;
      if (!b) {
        // Bare `name = v` in a function body declares a new local
        // (Python-style); at module level it targets the global slot.
        if (inFunctionBody_) {
          std::string slot = alloca("i64");
          emit("  store i64 " + v + ", ptr " + slot);
          LlBinding nb;
          nb.kind = BKind::Local;
          nb.slot = slot;
          nb.dropFn = dropFnFor(as.value.get());
          bind(nm, nb);
          recordDrop(nb, nb.dropFn);
          return;
        }
        if (curModuleId_ >= 0) {
          auto it = topLevelGlobals_.find(std::to_string(curModuleId_) + ":" + nm);
          if (it != topLevelGlobals_.end()) {
            emit("  store i64 " + v + ", ptr @" + it->second);
            LlBinding gb;
            gb.kind = BKind::Global;
            gb.sym = it->second;
            gb.dropFn = dropFnFor(as.value.get());
            bind(nm, gb);
            recordDrop(gb, gb.dropFn);
            return;
          }
        }
        fail("undefined variable '" + nm + "'");
        return;
      }
      // RAII: drop the previous owned value before overwriting (the new
      // value was already evaluated above, matching AsmEmitter ordering).
      if (b->dropFn &&
          (b->kind == BKind::Local || b->kind == BKind::Param ||
           b->kind == BKind::Global)) {
        if (DropRec* old = findDropRec(*b)) emitDropRec(*old);
      }
      if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
          b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
        emit("  store i64 " + v + ", ptr @" + b->sym);
      } else {
        emit("  store i64 " + v + ", ptr " + b->slot);
      }
      // Update this binding's pending drop record in place, or register one.
      FnInfo* newDrop = dropFnFor(as.value.get());
      b->dropFn = newDrop;
      if (DropRec* rec = findDropRec(*b)) {
        rec->fn = newDrop;
      } else {
        recordDrop(*b, newDrop);
      }
    } else if (target->kind == ExprKind::Index) {
      const IndexExpr& ix = static_cast<const IndexExpr&>(*target);
      std::string obj = eval(ix.object.get());
      std::string idx = eval(ix.index.get());
      callRt("a_setitem", {obj, idx, v});
    } else if (target->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*target);
      // `mod.member = v` stores into the member module's global slot.
      if (f.object->kind == ExprKind::Ident) {
        LlBinding* mb = lookup(
            static_cast<const IdentExpr*>(f.object.get())->name);
        if (mb && mb->kind == BKind::Module) {
          std::string gsym = moduleMemberGlobal(*mb, f.field);
          if (!gsym.empty()) {
            emit("  store i64 " + v + ", ptr @" + gsym);
            return;
          }
        }
      }
      std::string obj = eval(f.object.get());
      callRtA("a_setattr", "ipi", {obj, cstrGlobal(f.field, true), v});
    } else {
      fail("unsupported assignment target");
    }
  }

  void emitDestructure(const DestructureStmt& d) {
    std::string v = eval(d.value.get());
    for (std::size_t i = 0; i < d.names.size(); ++i) {
      if (d.names[i] == "_") continue;
      std::string iv = avalIntLit((std::int64_t)i);
      std::string el = callRt("a_getitem", {v, iv});
      std::string slot = alloca("i64");
      emit("  store i64 " + el + ", ptr " + slot);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = slot;
      bind(d.names[i], b);
    }
  }

  void emitIf(const IfStmt& s) {
    std::string end = newLabel("ifend");
    for (std::size_t i = 0; i < s.branches.size(); ++i) {
      const auto& br = s.branches[i];
      std::string cv = eval(br.cond.get());
      std::string t = T();
      emit("  " + t + " = call i1 @a.truthy(i64 " + cv + ")");
      std::string lThen = newLabel("ifb"), lNext = newLabel("ifn");
      emit("  br i1 " + t + ", label %" + lThen + ", label %" + lNext);
      emit("  " + lThen + ":");
      pushScope();
      deferStack_.emplace_back();
      emitStmts(br.body);
      emitDefers();
      if (!term_) emit("  br label %" + end);
      term_ = false;
      popScope();
      emit("  " + lNext + ":");
    }
    if (!s.elseBody.empty()) {
      pushScope();
      deferStack_.emplace_back();
      emitStmts(s.elseBody);
      emitDefers();
      popScope();
    }
    if (!term_) emit("  br label %" + end);
    term_ = false;
    emit("  " + end + ":");
  }

  void emitWhile(const WhileStmt& s) {
    std::string cond = newLabel("whc"), body = newLabel("whb"),
                end = newLabel("whe");
    loopBreak_.push_back(end);
    loopCont_.push_back(cond);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emit("  br label %" + cond);
    emit("  " + cond + ":");
    std::string cv = eval(s.cond.get());
    std::string t = T();
    emit("  " + t + " = call i1 @a.truthy(i64 " + cv + ")");
    emit("  br i1 " + t + ", label %" + body + ", label %" + end);
    emit("  " + body + ":");
    pushScope();
    deferStack_.emplace_back();
    emitStmts(s.body);
    emitDefers();
    if (!term_) emit("  br label %" + cond);
    term_ = false;
    popScope();
    emit("  " + end + ":");
    loopBreak_.pop_back();
    loopCont_.pop_back();
    loopDeferDepth_.pop_back();
  }

  void emitDoWhile(const DoWhileStmt& s) {
    std::string body = newLabel("dwb"), cond = newLabel("dwc"),
                end = newLabel("dwe");
    loopBreak_.push_back(end);
    loopCont_.push_back(cond);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emit("  br label %" + body);
    emit("  " + body + ":");
    pushScope();
    deferStack_.emplace_back();
    emitStmts(s.body);
    emitDefers();
    if (!term_) emit("  br label %" + cond);
    term_ = false;
    popScope();
    emit("  " + cond + ":");
    std::string cv = eval(s.cond.get());
    std::string t = T();
    emit("  " + t + " = call i1 @a.truthy(i64 " + cv + ")");
    emit("  br i1 " + t + ", label %" + body + ", label %" + end);
    emit("  " + end + ":");
    loopBreak_.pop_back();
    loopCont_.pop_back();
    loopDeferDepth_.pop_back();
  }

  void emitLoop(const LoopStmt& s) {
    std::string body = newLabel("lpb"), end = newLabel("lpe");
    loopBreak_.push_back(end);
    loopCont_.push_back(body);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emit("  br label %" + body);
    emit("  " + body + ":");
    pushScope();
    deferStack_.emplace_back();
    emitStmts(s.body);
    emitDefers();
    if (!term_) emit("  br label %" + body);
    term_ = false;
    popScope();
    emit("  " + end + ":");
    loopBreak_.pop_back();
    loopCont_.pop_back();
    loopDeferDepth_.pop_back();
  }

  void emitFor(const ForStmt& s) {
    // it = a_iter_new(iterable); loop { next(it,&item) -> bind var; body }
    std::string it = callRt("a_iter_new", {eval(s.iterable.get())});
    std::string itSlot = alloca("i64");
    emit("  store i64 " + it + ", ptr " + itSlot);
    std::string itemSlot = alloca("i64");
    std::string head = newLabel("frh"), body = newLabel("frb"),
                end = newLabel("fre");
    loopBreak_.push_back(end);
    loopCont_.push_back(head);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emit("  br label %" + head);
    emit("  " + head + ":");
    std::string itv = T(), outp = T(), nx = T(), t = T();
    emit("  " + itv + " = load i64, ptr " + itSlot);
    emit("  " + outp + " = getelementptr i64, ptr " + itemSlot + ", i64 0");
    emit("  " + nx + " = call i64 @a_iter_next(i64 " + itv + ", ptr " + outp + ")");
    emit("  " + t + " = icmp ne i64 " + nx + ", 0");
    emit("  br i1 " + t + ", label %" + body + ", label %" + end);
    emit("  " + body + ":");
    pushScope();
    deferStack_.emplace_back();
    {
      std::string iv = T();
      emit("  " + iv + " = load i64, ptr " + itemSlot);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = itemSlot;
      // Store fresh value into the slot first.
      emit("  store i64 " + iv + ", ptr " + itemSlot);
      bind(s.var, b);
    }
    emitStmts(s.body);
    emitDefers();
    emit("  br label %" + head);
    popScope();
    emit("  " + end + ":");
    loopBreak_.pop_back();
    loopCont_.pop_back();
    loopDeferDepth_.pop_back();
  }

  // ---- expressions -------------------------------------------------------------
  // eval returns an LLVM value name ("%tN") or an integer constant.
  std::string eval(const Expr* e) {
    if (!e) return kNone;
    switch (e->kind) {
      case ExprKind::IntLit:
        return avalIntLit(static_cast<const IntLitExpr*>(e)->value);
      case ExprKind::FloatLit:
        return avalDoubleLit(static_cast<const FloatLitExpr*>(e)->value);
      case ExprKind::BoolLit:
        return static_cast<const BoolLitExpr*>(e)->value ? kTrue : kFalse;
      case ExprKind::CharLit: {
        char32_t cp = static_cast<const CharLitExpr*>(e)->value;
        char buf[4];
        int n = 0;
        if (cp < 0x80) buf[n++] = (char)cp;
        else if (cp < 0x800) {
          buf[n++] = (char)(0xC0 | (cp >> 6));
          buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          buf[n++] = (char)(0xE0 | (cp >> 12));
          buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
          buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
          buf[n++] = (char)(0xF0 | (cp >> 18));
          buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
          buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
          buf[n++] = (char)(0x80 | (cp & 0x3F));
        }
        std::string sym = cstrGlobal(std::string(buf, n), false);
        return callRtA("a_char_new", "pi", {sym, std::to_string(n)});
      }
      case ExprKind::StringLit: return evalStringLit(*static_cast<const StringLitExpr*>(e));
      case ExprKind::Ident: return evalIdent(*static_cast<const IdentExpr*>(e));
      case ExprKind::Unary: return evalUnary(*static_cast<const UnaryExpr*>(e));
      case ExprKind::Binary: return evalBinary(*static_cast<const BinaryExpr*>(e));
      case ExprKind::Index: {
        const auto& x = static_cast<const IndexExpr&>(*e);
        return callRt("a_getitem", {eval(x.object.get()), eval(x.index.get())});
      }
      case ExprKind::Slice: {
        const auto& x = static_cast<const SliceExpr&>(*e);
        std::string lo = x.start ? eval(x.start.get()) : kNone;
        std::string hi = x.end ? eval(x.end.get()) : kNone;
        std::string st = x.step ? eval(x.step.get()) : kNone;
        return callRt("a_slice", {eval(x.object.get()), lo, hi, st});
      }
      case ExprKind::Field: return evalField(*static_cast<const FieldExpr*>(e));
      case ExprKind::SafeNav: return evalSafeNav(*static_cast<const SafeNavExpr*>(e));
      case ExprKind::ArrayLit: {
        const auto& x = static_cast<const ArrayLitExpr&>(*e);
        return litItems(x.elements, "a_list_new");
      }
      case ExprKind::Tuple: {
        const auto& x = static_cast<const TupleExpr&>(*e);
        return litItems(x.elements, "a_tuple_new");
      }
      case ExprKind::MapLit: {
        const auto& x = static_cast<const MapLitExpr&>(*e);
        std::string d = callRt("a_dict_new", {});
        for (const auto& kv : x.entries) {
          callRt("a_dict_set", {d, eval(kv.first.get()), eval(kv.second.get())});
        }
        return d;
      }
      case ExprKind::SetLit: {
        const auto& x = static_cast<const SetLitExpr&>(*e);
        std::string s = callRt("a_set_new", {});
        for (const ExprPtr& el : x.elements)
          callRt("a_set_add", {s, eval(el.get())});
        return s;
      }
      case ExprKind::Range: {
        const auto& x = static_cast<const RangeExpr&>(*e);
        std::string st = x.step ? eval(x.step.get()) : avalIntLit(1);
        return callRt("a_range_new", {eval(x.start.get()), eval(x.end.get()), st});
      }
      case ExprKind::Ternary: return evalTernary(*static_cast<const TernaryExpr*>(e));
      case ExprKind::Lambda: return evalLambda(*static_cast<const LambdaExpr*>(e));
      case ExprKind::Call: return evalCall(*static_cast<const CallExpr*>(e));
      case ExprKind::Comprehension: return evalComprehension(*static_cast<const ComprehensionExpr*>(e));
      case ExprKind::Match: return evalMatch(*static_cast<const MatchExpr*>(e));
      case ExprKind::Try: return evalTry(*static_cast<const TryExpr*>(e));
    }
    fail("unsupported expression kind");
    return kNone;
  }

  std::string litItems(const std::vector<ExprPtr>& elems, const char* sym) {
    int n = (int)elems.size();
    if (n == 0) {
      std::string r = T();
      emit("  " + r + " = call i64 @" + sym + "(i64 0, ptr null)");
      return r;
    }
    std::string av = argvArray(n);
    for (int i = 0; i < n; ++i) argvStore(av, i, eval(elems[i].get()));
    return callRtA(sym, "ip", {std::to_string(n), av});
  }

  std::string evalStringLit(const StringLitExpr& x) {
    if (x.parts.empty()) return avalStrBytes("");
    if (x.parts.size() == 1 && !x.parts[0].isExpr)
      return avalStrBytes(x.parts[0].text);
    int n = (int)x.parts.size();
    std::string av = argvArray(n);
    for (int i = 0; i < n; ++i) {
      const StringPart& p = x.parts[i];
      std::string v = p.isExpr ? callRt("a_str", {eval(p.expr.get())})
                               : avalStrBytes(p.text);
      argvStore(av, i, v);
    }
    return callRtA("a_str_join", "ip", {std::to_string(n), av});
  }

  std::string evalIdent(const IdentExpr& x) {
    const std::string& nm = x.name;
    static const std::unordered_map<std::string, std::string> kConsts = {
        {"PI", avalDoubleLit(3.14159265358979323846)},
        {"E", avalDoubleLit(2.71828182845904523536)},
        {"I8_MIN", avalIntLit(-128)}, {"I8_MAX", avalIntLit(127)},
        {"I16_MIN", avalIntLit(-32768)}, {"I16_MAX", avalIntLit(32767)},
        {"I32_MIN", avalIntLit(-2147483648LL)}, {"I32_MAX", avalIntLit(2147483647LL)},
        {"U8_MAX", avalIntLit(255)}, {"U16_MAX", avalIntLit(65535)},
        {"U32_MAX", avalIntLit(4294967295LL)},
        {"F32_MIN", avalDoubleLit(-3.4028234663852886e38)},
        {"F32_MAX", avalDoubleLit(3.4028234663852886e38)},
        {"F64_MIN", avalDoubleLit(-1.7976931348623157e308)},
        {"F64_MAX", avalDoubleLit(1.7976931348623157e308)},
    };
    if (nm == "None") return kNone;
    if (nm == "I64_MIN" || nm == "I64_MAX") {
      std::int64_t v = nm == "I64_MIN" ? INT64_MIN : INT64_MAX;
      return callRt("a_i64n", {std::to_string(v)});
    }
    auto ci = kConsts.find(nm);
    if (ci != kConsts.end()) return ci->second;
    LlBinding* b = lookup(nm);
    if (b) return bindingValue(*b);
    int bid = builtinRuntimeId(nm);
    if (bid >= 0) return callRt("a_builtin", {std::to_string(bid)});
    fail("undefined name '" + nm + "'");
    return kNone;
  }

  // Load the aval value a binding refers to.
  std::string bindingValue(const LlBinding& b) {
    switch (b.kind) {
      case BKind::Local:
      case BKind::Param:
      case BKind::Capture: {
        std::string r = T();
        emit("  " + r + " = load i64, ptr " + b.slot);
        return r;
      }
      case BKind::Global:
      case BKind::FnValue:
      case BKind::StructCtor:
      case BKind::EnumValue: {
        std::string r = T();
        emit("  " + r + " = load i64, ptr @" + b.sym);
        return r;
      }
      case BKind::Builtin:
        return callRt("a_builtin", {std::to_string(b.builtinId)});
      case BKind::NativeConst: {
        // Mirrors the VM: PI/E constants, anything else 0.0.
        double v = b.aux == "PI"   ? 3.14159265358979323846
                  : b.aux == "E"    ? 2.71828182845904523536
                                      : 0.0;
        return avalDoubleLit(v);
      }
      case BKind::Module:
        fail("module '" + b.aux + "' is not a value");
        return kNone;
      case BKind::PyModule: {
        // `import python:<mod>` as a value: the imported module object
        // itself (a T_PYOBJ aval; attribute access/calls bridge through
        // the runtime py helpers).
        return callRtA("a_py_import_module", "p",
                       {cstrGlobal(b.aux, true)});
      }
      case BKind::PyFunc: {
        // `from python:<mod> import name` as a value: getattr on the
        // imported module yields the bound member (callable T_PYOBJ).
        std::string m = callRtA("a_py_import_module", "p",
                                {cstrGlobal(b.aux, true)});
        return callRtA("a_py_getattr", "ip",
                       {m, cstrGlobal(b.pyMember, true)});
      }
    }
    return kNone;
  }

  std::string evalUnary(const UnaryExpr& x) {
    if (x.op == "++" || x.op == "--") {
      if (x.operand->kind != ExprKind::Ident) {
        fail("operand of '" + x.op + "' must be a variable");
        return kNone;
      }
      const std::string& nm = static_cast<const IdentExpr*>(x.operand.get())->name;
      LlBinding* b = lookup(nm);
      if (!b) { fail("undefined variable '" + nm + "'"); return kNone; }
      std::string cur = bindingValue(*b);
      std::string delta = avalIntLit(x.op == "++" ? 1 : -1);
      std::string nxt = callRt("a.add.fast", {cur, delta});
      if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
          b->kind == BKind::StructCtor || b->kind == BKind::EnumValue)
        emit("  store i64 " + nxt + ", ptr @" + b->sym);
      else
        emit("  store i64 " + nxt + ", ptr " + b->slot);
      return x.postfix ? cur : nxt;
    }
    std::string v = eval(x.operand.get());
    if (x.op == "-") return callRt("a.neg.fast", {v});
    if (x.op == "~") return callRt("a.bnot.fast", {v});
    if (x.op == "!") return callRt("a_unwrap", {v});
    if (x.op == "not") return callRt("a.not.fast", {v});
    fail("unsupported unary operator '" + x.op + "'");
    return kNone;
  }

  // Binary op -> LLVM fast-path helper name ("" when it needs control flow).
  // The @a.*.fast helpers are emitted by emitFastHelpers() and mirror the
  // C a_*_fast runtime functions instruction-for-instruction, so calling
  // them is semantically identical while giving opt/llc the tag dispatch.
  static const char* binopSym(BinOp op) {
    switch (op) {
      case BinOp::Add: return "a.add.fast";
      case BinOp::Sub: return "a.sub.fast";
      case BinOp::Mul: return "a.mul.fast";
      case BinOp::Div: return "a.div.fast";
      case BinOp::FloorDiv: return "a.floordiv.fast";
      case BinOp::Mod: return "a.mod.fast";
      case BinOp::Pow: return "a_pow";
      case BinOp::Eq: return "a.eq.fast";
      case BinOp::Ne: return "a.ne.fast";
      case BinOp::Lt: return "a.lt.fast";
      case BinOp::Le: return "a.le.fast";
      case BinOp::Gt: return "a.gt.fast";
      case BinOp::Ge: return "a.ge.fast";
      case BinOp::BitAnd: return "a.band.fast";
      case BinOp::BitOr: return "a.bor.fast";
      case BinOp::BitXor: return "a.bxor.fast";
      case BinOp::Shl: return "a.shl.fast";
      case BinOp::Shr: return "a.shr.fast";
      default: return nullptr;
    }
  }
  // For compound assignment (op string may differ from binop spelling).
  std::string binopValue(BinOp op, const std::string& l, const std::string& r,
                         const Span& /*sp*/) {
    if (const char* sym = binopSym(op)) return callRt(sym, {l, r});
    if (op == BinOp::In) return callRt("a_contains", {r, l});
    if (op == BinOp::NotIn) return callRt("a.not.fast", {callRt("a_contains", {r, l})});
    if (op == BinOp::Range) return callRt("a_range_new", {l, r, avalIntLit(1)});
    fail("unsupported binary operator in assignment");
    return "";
  }

  std::string evalBinary(const BinaryExpr& x) {
    switch (x.binop) {
      case BinOp::And: {
        std::string l = eval(x.lhs.get());
        std::string t = T();
        emit("  " + t + " = call i1 @a.truthy(i64 " + l + ")");
        std::string lRhs = newLabel("and.rhs"), lEnd = newLabel("and.end");
        std::string phi = alloca("i64");
        emit("  br i1 " + t + ", label %" + lRhs + ", label %" + lEnd + ".f");
        emit("  " + lEnd + ".f:");
        emit("  store i64 " + l + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lRhs + ":");
        std::string r = eval(x.rhs.get());
        emit("  store i64 " + r + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lEnd + ":");
        std::string out = T();
        emit("  " + out + " = load i64, ptr " + phi);
        return out;
      }
      case BinOp::Or: {
        std::string l = eval(x.lhs.get());
        std::string t = T();
        emit("  " + t + " = call i1 @a.truthy(i64 " + l + ")");
        std::string lRhs = newLabel("or.rhs"), lEnd = newLabel("or.end");
        std::string phi = alloca("i64");
        emit("  br i1 " + t + ", label %" + lEnd + ".t, label %" + lRhs);
        emit("  " + lEnd + ".t:");
        emit("  store i64 " + l + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lRhs + ":");
        std::string r = eval(x.rhs.get());
        emit("  store i64 " + r + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lEnd + ":");
        std::string out = T();
        emit("  " + out + " = load i64, ptr " + phi);
        return out;
      }
      case BinOp::Coalesce: {
        std::string l = eval(x.lhs.get());
        std::string isNone = T();
        emit("  " + isNone + " = icmp eq i64 " + l + ", " + kNone);
        std::string lRhs = newLabel("coal.rhs"), lEnd = newLabel("coal.end");
        std::string phi = alloca("i64");
        emit("  br i1 " + isNone + ", label %" + lRhs + ", label %" + lEnd + ".t");
        emit("  " + lEnd + ".t:");
        std::string un = callRt("a_unwrap", {l});
        emit("  store i64 " + un + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lRhs + ":");
        std::string r = eval(x.rhs.get());
        emit("  store i64 " + r + ", ptr " + phi);
        emit("  br label %" + lEnd);
        emit("  " + lEnd + ":");
        std::string out = T();
        emit("  " + out + " = load i64, ptr " + phi);
        return out;
      }
      case BinOp::In:
      case BinOp::NotIn: {
        std::string item = eval(x.lhs.get());
        std::string cont = eval(x.rhs.get());
        std::string r = callRt("a_contains", {cont, item});
        if (x.binop == BinOp::NotIn) r = callRt("a.not.fast", {r});
        return r;
      }
      case BinOp::Range: {
        std::string lo = eval(x.lhs.get());
        std::string hi = eval(x.rhs.get());
        return callRt("a_range_new", {lo, hi, avalIntLit(1)});
      }
      default: break;
    }
    std::string l = eval(x.lhs.get());
    std::string r = eval(x.rhs.get());
    if (const char* sym = binopSym(x.binop)) return callRt(sym, {l, r});
    if (x.op == "||") return callRt("a.bor.fast", {l, r});
    if (x.op == "&&") return callRt("a.band.fast", {l, r});
    fail("unsupported binary operator '" + x.op + "'");
    return kNone;
  }

  // ---- enum variant values ---------------------------------------------------
  // Flatten a FieldExpr chain (a.b.c) into ["a","b","c"]; false when the base
  // is not a plain identifier.
  static bool flattenFieldChain(const Expr* e, std::vector<std::string>& parts) {
    const Expr* cur = e;
    while (cur && cur->kind == ExprKind::Field) {
      parts.push_back(static_cast<const FieldExpr*>(cur)->field);
      cur = static_cast<const FieldExpr*>(cur)->object.get();
    }
    if (!cur || cur->kind != ExprKind::Ident || parts.empty()) return false;
    parts.push_back(static_cast<const IdentExpr*>(cur)->name);
    std::reverse(parts.begin(), parts.end());
    return true;
  }
  const EnumVariantInfo* findEnumVariantLl(const std::string& enumName,
                                         const std::string& variant,
                                         int preferModule) const {
    int prefer = preferModule < 0 ? curModuleId_ : preferModule;
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
  const EnumVariantInfo* findEnumByShortLl(const std::string& variant,
                                         int preferModule) const {
    int prefer = preferModule < 0 ? curModuleId_ : preferModule;
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
  // Algebraic variant dict {__tag,__name} — same shape the VM/native backends
  // build for `Enum.Variant` used as a value.
  std::string emitVariantDict(const EnumVariantInfo& ev,
                              const std::string& name) {
    std::string d = callRt("a_dict_new", {});
    callRt("a_dict_set", {d, avalStrBytes("__tag"), avalIntLit(ev.tag)});
    callRt("a_dict_set", {d, avalStrBytes("__name"), avalStrBytes(name)});
    return d;
  }
  std::string enumVariantValue(const EnumVariantInfo& ev,
                               const std::string& name) {
    if (!ev.algebraic) {
      std::string r = T();
      emit("  " + r + " = load i64, ptr @" + ev.globalVar);
      return r;
    }
    return emitVariantDict(ev, name);
  }
  // Resolve `Color.Red` / `mod.Color.Red` / `Shape.Empty` used as a value.
  // Returns "" when the chain is not an enum variant reference.
  std::string evalEnumVariantField(const FieldExpr& x) {
    std::vector<std::string> parts;
    if (!flattenFieldChain(&x, parts) || parts.size() < 2) return "";
    const std::string& first = parts.front();
    LlBinding* fb = lookup(first);
    if (fb && fb->kind == BKind::Module && fb->moduleId >= 0) {
      if (parts.size() >= 3) {
        // mod.Enum.Variant
        if (const EnumVariantInfo* ev = findEnumVariantLl(
                parts[parts.size() - 2], parts.back(), fb->moduleId))
          return enumVariantValue(*ev, parts.back());
      } else {
        // mod.Variant (variant re-exported at module top level)
        if (const EnumVariantInfo* ev =
                findEnumByShortLl(parts.back(), fb->moduleId))
          return enumVariantValue(*ev, parts.back());
      }
      return "";
    }
    if (parts.size() == 2) {
      // Enum.Variant in the current module's (or any) enum namespace.
      if (const EnumVariantInfo* ev =
              findEnumVariantLl(parts[0], parts[1], -1))
        return enumVariantValue(*ev, parts[1]);
    }
    return "";
  }

  // ---- match expressions -----------------------------------------------------
  // Branch to `failL` when the pattern does not match `subj` (an i64 aval SSA
  // value); falls through on match. Mirrors AsmEmitter::emitPatternTest.
  void emitPatternEqFail(const std::string& lhs, const std::string& rhs,
                         const std::string& failL) {
    std::string eq = callRt("a.eq.fast", {lhs, rhs});
    std::string t = T();
    emit("  " + t + " = call i1 @a.truthy(i64 " + eq + ")");
    std::string pass = newLabel("pat.ok");
    emit("  br i1 " + t + ", label %" + pass + ", label %" + failL);
    emit("  " + pass + ":");
  }
  const EnumVariantInfo* patternVariantInfo(const Pattern& p) {
    std::vector<std::string> parts;
    if (!flattenFieldChain(p.ctor.get(), parts) || parts.size() < 2)
      return nullptr;
    LlBinding* fb = lookup(parts.front());
    if (parts.size() >= 3) {
      if (fb && fb->kind == BKind::Module && fb->moduleId >= 0)
        return findEnumVariantLl(parts[parts.size() - 2], parts.back(),
                                 fb->moduleId);
      return nullptr;
    }
    if (const EnumVariantInfo* ev =
            findEnumVariantLl(parts[0], parts[1], -1))
      return ev;
    return findEnumByShortLl(parts[1], -1);
  }
  void emitPatternTest(const Pattern& p, const std::string& subj,
                       const std::string& failL) {
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
      case Pattern::Kind::Binding:
        return;  // irrefutable; binding happens after the test phase
      case Pattern::Kind::Literal: {
        emitPatternEqFail(subj, eval(p.value.get()), failL);
        return;
      }
      case Pattern::Kind::Range: {
        std::string lo = eval(p.rangeStart.get());
        std::string hi = eval(p.rangeEnd.get());
        std::string r = callRt("a_in_range_inclusive", {subj, lo, hi});
        std::string t = T();
        emit("  " + t + " = call i1 @a.truthy(i64 " + r + ")");
        std::string pass = newLabel("pat.ok");
        emit("  br i1 " + t + ", label %" + pass + ", label %" + failL);
        emit("  " + pass + ":");
        return;
      }
      case Pattern::Kind::Tuple: {
        std::string len = callRt("a_len", {subj});
        emitPatternEqFail(len, avalIntLit((std::int64_t)p.subpatterns.size()),
                          failL);
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string el = callRt("a_getitem", {subj, avalIntLit((std::int64_t)i)});
          emitPatternTest(p.subpatterns[i], el, failL);
        }
        return;
      }
      case Pattern::Kind::Variant: {
        const EnumVariantInfo* ev = patternVariantInfo(p);
        if (!ev || !ev->algebraic) {
          // Simple variant (or unresolvable ctor): compare whole values.
          emitPatternEqFail(subj, eval(p.ctor.get()), failL);
          return;
        }
        // Algebraic variant: subject.__tag == variant tag, then payload
        // subpatterns against the named fields (VM semantics).
        std::string tag = callRt("a_dict_get",
                                 {subj, avalStrBytes("__tag"), kNone});
        emitPatternEqFail(tag, avalIntLit(ev->tag), failL);
        std::size_t n = std::min(p.subpatterns.size(), p.variantFields.size());
        for (std::size_t i = 0; i < n; ++i) {
          std::string fv = callRt("a_dict_get",
                                  {subj, avalStrBytes(p.variantFields[i]), kNone});
          emitPatternTest(p.subpatterns[i], fv, failL);
        }
        return;
      }
      case Pattern::Kind::Or: {
        std::string orMatch = newLabel("ormat");
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string altFail = newLabel("altfail");
          emitPatternTest(p.subpatterns[i], subj, altFail);
          emit("  br label %" + orMatch);
          emit("  " + altFail + ":");
        }
        emit("  br label %" + failL);
        emit("  " + orMatch + ":");
        return;
      }
    }
    fail("unsupported match pattern kind");
  }
  // Bind pattern variables after a successful test (mirrors
  // AsmEmitter::bindPatternVariables).
  void bindPatternVariables(const Pattern& p, const std::string& subj) {
    switch (p.kind) {
      case Pattern::Kind::Binding: {
        std::string slot = alloca("i64");
        emit("  store i64 " + subj + ", ptr " + slot);
        LlBinding b;
        b.kind = BKind::Local;
        b.slot = slot;
        bind(p.name, b);
        return;
      }
      case Pattern::Kind::Tuple: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string el = callRt("a_getitem", {subj, avalIntLit((std::int64_t)i)});
          bindPatternVariables(p.subpatterns[i], el);
        }
        return;
      }
      case Pattern::Kind::Variant: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string key = i < p.variantFields.size()
                                ? p.variantFields[i]
                                : p.subpatterns[i].name;
          if (key.empty()) continue;
          std::string fv = callRt("a_dict_get",
                                  {subj, avalStrBytes(key), kNone});
          bindPatternVariables(p.subpatterns[i], fv);
        }
        return;
      }
      case Pattern::Kind::Or: {
        for (const Pattern& sub : p.subpatterns)
          bindPatternVariables(sub, subj);
        return;
      }
      default:
        return;
    }
  }
  std::string evalMatch(const MatchExpr& m) {
    std::string subj = eval(m.subject.get());
    std::string res = alloca("i64");
    std::string end = newLabel("match.end");
    // Each arm's failure label is emitted lazily: a label with no
    // instructions is not a valid LLVM basic block.
    std::string pendingFail;
    bool hasCatchAll = false;
    for (const MatchArm& arm : m.arms) {
      if (!pendingFail.empty()) {
        emit("  " + pendingFail + ":");
        pendingFail.clear();
      }
      std::string nextArm = newLabel("arm.fail");
      pushScope();
      deferStack_.emplace_back();
      emitPatternTest(arm.pattern, subj, nextArm);
      bindPatternVariables(arm.pattern, subj);
      if (arm.guard) {
        std::string gv = eval(arm.guard.get());
        std::string gt = T();
        emit("  " + gt + " = call i1 @a.truthy(i64 " + gv + ")");
        std::string pass = newLabel("guard.ok");
        emit("  br i1 " + gt + ", label %" + pass + ", label %" + nextArm);
        emit("  " + pass + ":");
      }
      std::string rv = eval(arm.result.get());
      emitDefers();
      emit("  store i64 " + rv + ", ptr " + res);
      emit("  br label %" + end);
      term_ = false;
      popScope();
      pendingFail = nextArm;
      term_ = false;
      if (arm.pattern.kind == Pattern::Kind::Wildcard && !arm.guard)
        hasCatchAll = true;
    }
    if (!pendingFail.empty()) emit("  " + pendingFail + ":");
    if (!hasCatchAll) {
      (void)callRt("a_match_fail", {subj});
      emit("  store i64 " + kNone + ", ptr " + res);
    }
    emit("  br label %" + end);
    emit("  " + end + ":");
    term_ = false;
    std::string out = T();
    emit("  " + out + " = load i64, ptr " + res);
    return out;
  }

  // `expr?`: unwrap ok/err (and Some); a propagating value returns early from
  // the enclosing function after running defers (mirrors AsmEmitter::emitTry).
  std::string evalTry(const TryExpr& x) {
    std::string t = callRt("a_try_result", {eval(x.operand.get())});
    if (!inFunctionBody_) return t;
    std::string isP = T();
    emit("  " + isP + " = call i32 @a_is_propagate(i64 " + t + ")");
    std::string isPc = T();
    emit("  " + isPc + " = icmp ne i32 " + isP + ", 0");
    std::string lProp = newLabel("try.prop"), lOk = newLabel("try.ok");
    emit("  br i1 " + isPc + ", label %" + lProp + ", label %" + lOk);
    emit("  " + lProp + ":");
    emitDeferredPeekAll();
    (void)callRt("a_promote", {t});  // region escape; returns void
    emitFramePop();
    emit("  ret i64 " + t);
    emit("  " + lOk + ":");
    term_ = false;
    return t;
  }

  std::string evalTernary(const TernaryExpr& x) {
    std::string cv = eval(x.cond.get());
    std::string t = T();
    emit("  " + t + " = call i1 @a.truthy(i64 " + cv + ")");
    std::string lT = newLabel("tern.t"), lE = newLabel("tern.e"),
                lEnd = newLabel("tern.end");
    std::string phi = alloca("i64");
    emit("  br i1 " + t + ", label %" + lT + ", label %" + lE);
    emit("  " + lT + ":");
    emit("  store i64 " + eval(x.thenValue.get()) + ", ptr " + phi);
    emit("  br label %" + lEnd);
    emit("  " + lE + ":");
    emit("  store i64 " + eval(x.elseValue.get()) + ", ptr " + phi);
    emit("  br label %" + lEnd);
    emit("  " + lEnd + ":");
    std::string out = T();
    emit("  " + out + " = load i64, ptr " + phi);
    return out;
  }

  std::string evalField(const FieldExpr& x) {
    // Enum variant values: Color.Red, Shape.Empty, mod.Color.Red.
    if (std::string ev = evalEnumVariantField(x); !ev.empty()) return ev;
    // Module.member -> resolve statically; otherwise a_getattr.
    if (x.object->kind == ExprKind::Ident) {
      LlBinding* mb = lookup(static_cast<const IdentExpr*>(x.object.get())->name);
      if (mb && mb->kind == BKind::Module) {
        std::string v = moduleMemberValue(*mb, x.field);
        if (!v.empty()) return v;
      }
      if (mb && mb->kind == BKind::NativeConst) {
        // native module constant
      }
      if (mb && (mb->kind == BKind::PyModule)) {
        // python:<mod>.member as a value: getattr on the imported module.
        std::string m = callRtA("a_py_import_module", "p",
                                {cstrGlobal(mb->aux, true)});
        return callRtA("a_py_getattr", "ip",
                       {m, cstrGlobal(x.field, true)});
      }
      if (mb && mb->kind == BKind::Module && mb->moduleId < 0) {
        // Native module member as a value.
        if (const std::string* cn = nativeConstant(mb->aux, x.field)) {
          double d = *cn == "PI"   ? 3.14159265358979323846
                    : *cn == "E"    ? 2.71828182845904523536
                                    : 0.0;
          return avalDoubleLit(d);
        }
        if (const std::string* fn = nativeFunction(mb->aux, x.field)) {
          int bid = builtinRuntimeId(*fn);
          if (bid >= 0) return callRt("a_builtin", {std::to_string(bid)});
        }
        int bid = builtinRuntimeId(x.field);
        if (bid >= 0) return callRt("a_builtin", {std::to_string(bid)});
        fail("unknown native module member '" + x.field + "'");
        return kNone;
      }
    }
    return callRtA("a_getattr", "ip", {eval(x.object.get()), cstrGlobal(x.field, true)});
  }

  // Global-slot symbol for `mod.field` when field is a top-level let/assign
  // in that module. "" when not a plain global.
  std::string moduleMemberGlobal(const LlBinding& mb,
                                 const std::string& field) {
    int mid = mb.moduleId;
    if (mid < 0 || mid >= (int)graph_.modules.size()) return "";
    const Module& mod = graph_.at(mid);
    if (!mod.program) return "";
    for (const StmtPtr& s : mod.program->statements) {
      std::string vn;
      if (s->kind == StmtKind::Let) {
        vn = static_cast<const LetStmt*>(s.get())->name;
      } else if (s->kind == StmtKind::Assign &&
                 s->kind == StmtKind::Assign) {
        const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
        if (as->target->kind == ExprKind::Ident)
          vn = static_cast<const IdentExpr*>(as->target.get())->name;
      }
      if (vn != field) continue;
      auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + vn);
      if (it != topLevelGlobals_.end()) return it->second;
    }
    return "";
  }

  // Resolve `mod.field` used as a value. Returns "" when unresolvable.
  std::string moduleMemberValue(const LlBinding& mb, const std::string& field) {
    int mid = mb.moduleId;
    if (mid < 0 || mid >= (int)graph_.modules.size()) return "";
    const Module& mod = graph_.at(mid);
    if (!mod.program) return "";
    // Re-export following (like resolveModuleMember).
    for (int depth = 0; depth < 16; ++depth) {
      bool declared = false;
      for (const StmtPtr& s : mod.program->statements) {
        if ((s->kind == StmtKind::FnDecl &&
             static_cast<const FnDecl*>(s.get())->name == field) ||
            (s->kind == StmtKind::StructDecl &&
             static_cast<const StructDecl*>(s.get())->name == field) ||
            (s->kind == StmtKind::Let &&
             static_cast<const LetStmt*>(s.get())->name == field)) {
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
          if (item.name == field || (!item.alias.empty() && item.alias == field)) {
            nextId = im->moduleId;
            nextName = item.name;
            break;
          }
        }
        if (nextId >= 0) break;
      }
      if (nextId < 0) break;
      // Recurse into source module.
      const Module& nm = graph_.at(nextId);
      if (!nm.program) return "";
      // Look up nextName in nm (non-recursive inner search).
      for (const StmtPtr& s : nm.program->statements) {
        if (s->kind == StmtKind::FnDecl &&
            static_cast<const FnDecl*>(s.get())->name == nextName) {
          auto it = fns_.find(static_cast<const FnDecl*>(s.get()));
          if (it != fns_.end()) {
            std::string r = T();
            emit("  " + r + " = load i64, ptr @" + it->second->globalVar);
            return r;
          }
        } else if (s->kind == StmtKind::StructDecl &&
                   static_cast<const StructDecl*>(s.get())->name == nextName) {
          auto it = structs_.find(nextName);
          if (it != structs_.end()) {
            std::string r = T();
            emit("  " + r + " = load i64, ptr @" + it->second->globalVar);
            return r;
          }
        } else if ((s->kind == StmtKind::Let || s->kind == StmtKind::Assign)) {
          std::string vn = s->kind == StmtKind::Let
                               ? static_cast<const LetStmt*>(s.get())->name
                               : static_cast<const IdentExpr*>(
                                     static_cast<const AssignStmt*>(s.get())
                                         ->target.get())
                                     ->name;
          if (vn == nextName) {
            auto it = topLevelGlobals_.find(std::to_string(nm.id) + ":" + vn);
            if (it != topLevelGlobals_.end()) {
              std::string r = T();
              emit("  " + r + " = load i64, ptr @" + it->second);
              return r;
            }
          }
        }
      }
      return "";
    }
    // Direct lookup in `mod`.
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::FnDecl &&
          static_cast<const FnDecl*>(s.get())->name == field) {
        auto it = fns_.find(static_cast<const FnDecl*>(s.get()));
        if (it != fns_.end()) {
          std::string r = T();
          emit("  " + r + " = load i64, ptr @" + it->second->globalVar);
          return r;
        }
      } else if (s->kind == StmtKind::StructDecl &&
                 static_cast<const StructDecl*>(s.get())->name == field) {
        auto it = structs_.find(field);
        if (it != structs_.end()) {
          std::string r = T();
          emit("  " + r + " = load i64, ptr @" + it->second->globalVar);
          return r;
        }
      } else if (s->kind == StmtKind::Let &&
                 static_cast<const LetStmt*>(s.get())->name == field) {
        auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + field);
        if (it != topLevelGlobals_.end()) {
          std::string r = T();
          emit("  " + r + " = load i64, ptr @" + it->second);
          return r;
        }
      } else if (s->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt*>(s.get())->target->kind ==
                     ExprKind::Ident &&
                 static_cast<const IdentExpr*>(
                     static_cast<const AssignStmt*>(s.get())
                         ->target.get())
                         ->name == field) {
        auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + field);
        if (it != topLevelGlobals_.end()) {
          std::string r = T();
          emit("  " + r + " = load i64, ptr @" + it->second);
          return r;
        }
      } else if (s->kind == StmtKind::EnumDecl) {
        const EnumDecl* ed = static_cast<const EnumDecl*>(s.get());
        for (const EnumVariant& v : ed->variants) {
          if (v.name != field) continue;
          auto it = enums_.find(std::to_string(mod.id) + ":" + ed->name + "." + v.name);
          if (it != enums_.end() && !it->second.algebraic) {
            std::string r = T();
            emit("  " + r + " = load i64, ptr @" + it->second.globalVar);
            return r;
          }
        }
      }
    }
    int bid = builtinRuntimeId(field);
    if (bid >= 0) return callRt("a_builtin", {std::to_string(bid)});
    return "";
  }

  std::string evalSafeNav(const SafeNavExpr& x) {
    std::string obj = eval(x.object.get());
    std::string isNone = T();
    emit("  " + isNone + " = icmp eq i64 " + obj + ", " + kNone);
    std::string lGet = newLabel("sn.get"), lEnd = newLabel("sn.end");
    std::string phi = alloca("i64");
    emit("  br i1 " + isNone + ", label %" + lEnd + ".n, label %" + lGet);
    emit("  " + lEnd + ".n:");
    emit("  store i64 " + kNone + ", ptr " + phi);
    emit("  br label %" + lEnd);
    emit("  " + lGet + ":");
    std::string inner = callRt("a_unwrap", {obj});
    std::string v = callRtA("a_getattr", "ip", {inner, cstrGlobal(x.field, true)});
    std::string wrap;
    if (x.fieldOptional) {
      wrap = v;
    } else {
      // v == None -> None; already-option -> v; else Some(v).
      std::string isN = T();
      emit("  " + isN + " = icmp eq i64 " + v + ", " + kNone);
      std::string lOpt = newLabel("sn.opt"), lWrap = newLabel("sn.wrap");
      emit("  br i1 " + isN + ", label %" + lEnd + ".n2, label %" + lOpt);
      emit("  " + lEnd + ".n2:");
      emit("  store i64 " + kNone + ", ptr " + phi);
      emit("  br label %" + lEnd);
      emit("  " + lOpt + ":");
      std::string isOpt = T();
      emit("  " + isOpt + " = call i32 @a_is_option(i64 " + v + ")");
      std::string isO = T();
      emit("  " + isO + " = icmp ne i32 " + isOpt + ", 0");
      emit("  br i1 " + isO + ", label %" + lWrap + ".p, label %" + lWrap);
      emit("  " + lWrap + ".p:");
      emit("  store i64 " + v + ", ptr " + phi);
      emit("  br label %" + lEnd);
      emit("  " + lWrap + ":");
      std::string sv = callRt("a_some_new", {v});
      emit("  store i64 " + sv + ", ptr " + phi);
      emit("  br label %" + lEnd);
      wrap = "";
    }
    if (!wrap.empty()) {
      emit("  store i64 " + wrap + ", ptr " + phi);
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    term_ = false;
    std::string out = T();
    emit("  " + out + " = load i64, ptr " + phi);
    return out;
  }

  std::string evalLambda(const LambdaExpr& x) {
    // Captures: every visible Local/Param binding (innermost wins, sorted for
    // determinism). Globals/builtins are not captured.
    std::map<std::string, std::string> caps;  // name -> alloca
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      for (auto& kv : *it)
        if ((kv.second.kind == BKind::Local || kv.second.kind == BKind::Param) &&
            !caps.count(kv.first))
          caps[kv.first] = kv.second.slot;
    PendingLambda pl;
    pl.expr = &x;
    pl.moduleId = curModuleId_;
    pl.cname = "__a_lam_" + moduleTag(curModuleId_ < 0 ? 0 : curModuleId_) + "_" +
               std::to_string(lamCounter_++);
    for (auto& kv : caps) pl.captures.push_back(kv);
    lambdas_.push_back(pl);
    // env = a_closure_env_new(n); a_env_set(env, i, load cap)
    std::string env = callRt("a_closure_env_new", {std::to_string(pl.captures.size())});
    for (std::size_t i = 0; i < pl.captures.size(); ++i) {
      std::string v = T();
      emit("  " + v + " = load i64, ptr " + pl.captures[i].second);
      callRt("a_env_set", {env, std::to_string(i), v});
    }
    return fixFnonNew("", pl.cname, env, "<lambda>");
  }

  // a_fnon_new's first arg is uint64 code address; emit correct call.
  std::string fixFnonNew(const std::string& /*prev*/, const std::string& cname,
                         const std::string& env, const std::string& name) {
    std::string nm = cstrGlobal(name, true);
    std::string r = T();
    emit("  " + r + " = call i64 @a_fnon_new(i64 ptrtoint (ptr @" + cname +
         " to i64), i64 " + env + ", ptr " + nm + ")");
    return r;
  }

  void drainLambdas() {
    while (!failed_ && !lambdas_.empty()) {
      PendingLambda pl = lambdas_.back();
      lambdas_.pop_back();
      emitLambdaFn(pl);
    }
  }

  void emitLambdaFn(const PendingLambda& pl) {
    const LambdaExpr& x = *pl.expr;
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    resetGlobalScope(pl.moduleId);
    pushScope();
    bindModuleImports(pl.moduleId);
    // env = argv[0] (closure env aval, not A_NONE).
    std::string envv = T();
    emit("  " + envv + " = load i64, ptr %argv");  // argv is ptr; load i64* ?
    // argv is `ptr` to array of i64: argv[0] via GEP.
    // Fix: emit proper GEP.
    fn_ = std::ostringstream();  // restart cleanly below
    emit("  " + envv + " = getelementptr i64, ptr %argv, i64 0");
    std::string env = T();
    emit("  " + env + " = load i64, ptr " + envv);
    // Captures.
    for (std::size_t i = 0; i < pl.captures.size(); ++i) {
      std::string slot = alloca("i64");
      std::string cv = callRt("a_env_get", {env, std::to_string(i)});
      emit("  store i64 " + cv + ", ptr " + slot);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = slot;
      bind(pl.captures[i].first, b);
    }
    // Params.
    for (std::size_t i = 0; i < x.params.size(); ++i) {
      std::string slot = alloca("i64");
      std::string g = T(), v = T();
      emit("  " + g + " = getelementptr i64, ptr %argv, i64 " +
           std::to_string(i + 1));
      emit("  " + v + " = load i64, ptr " + g);
      emit("  store i64 " + v + ", ptr " + slot);
      LlBinding b;
      b.kind = BKind::Param;
      b.slot = slot;
      bind(x.params[i].name, b);
    }
    // Defaults.
    for (std::size_t i = 0; i < x.params.size(); ++i) {
      const ParamDecl& p = x.params[i];
      if (!p.defaultValue) continue;
      LlBinding* pb = lookup(p.name);
      if (!pb) continue;
      std::string skip = newLabel("ldef");
      std::string cur = T(), isMiss = T();
      emit("  " + cur + " = load i64, ptr " + pb->slot);
      emit("  " + isMiss + " = icmp eq i64 " + cur + ", " + kMissing);
      emit("  br i1 " + isMiss + ", label %" + skip + ".t, label %" + skip + ".e");
      emit("  " + skip + ".t:");
      std::string dv = eval(p.defaultValue.get());
      emit("  store i64 " + dv + ", ptr " + pb->slot);
      emit("  br label %" + skip + ".e");
      emit("  " + skip + ".e:");
    }
    deferStack_.emplace_back();
    bool savedInFnBody = inFunctionBody_;
    inFunctionBody_ = true;  // `?` propagates out of the lambda like a return
    std::string rv = eval(x.body.get());
    emitDefers();
    inFunctionBody_ = savedInFnBody;
    emit("  ret i64 " + rv);
    clearDeferFrames();
    std::ostringstream head;
    for (auto& kv : pendingAllocas_)
      head << "  " << kv.first << " = alloca " << kv.second << "\n";
    body_ << "define i64 @" << pl.cname << "(ptr %argv) {\n"
          << head.str() << fn_.str() << "}\n\n";
    scopes_.clear();
  }

  std::string evalComprehension(const ComprehensionExpr& x) {
    std::string it = callRt("a_iter_new", {eval(x.iterable.get())});
    std::string itSlot = alloca("i64");
    emit("  store i64 " + it + ", ptr " + itSlot);
    std::string itemSlot = alloca("i64");
    std::string acc = x.isSet ? callRt("a_set_new", {})
                              : callRtA("a_list_new", "ip", {"0", "null"});
    std::string head = newLabel("cmph"), body = newLabel("cmpb"),
                end = newLabel("cmpe"), skip = newLabel("cmps");
    emit("  br label %" + head);
    emit("  " + head + ":");
    std::string itv = T(), outp = T(), nx = T(), t = T();
    emit("  " + itv + " = load i64, ptr " + itSlot);
    emit("  " + outp + " = getelementptr i64, ptr " + itemSlot + ", i64 0");
    emit("  " + nx + " = call i64 @a_iter_next(i64 " + itv + ", ptr " + outp + ")");
    emit("  " + t + " = icmp ne i64 " + nx + ", 0");
    emit("  br i1 " + t + ", label %" + body + ", label %" + end);
    emit("  " + body + ":");
    pushScope();
    deferStack_.emplace_back();
    {
      std::string iv = T();
      emit("  " + iv + " = load i64, ptr " + itemSlot);
      emit("  store i64 " + iv + ", ptr " + itemSlot);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = itemSlot;
      bind(x.var, b);
    }
    if (x.condition) {
      std::string cv = eval(x.condition.get());
      std::string ct = T();
      emit("  " + ct + " = call i1 @a.truthy(i64 " + cv + ")");
      emit("  br i1 " + ct + ", label %" + body + ".k, label %" + skip);
      emit("  " + body + ".k:");
      std::string ev = eval(x.element.get());
      callRt(x.isSet ? "a_set_add" : "a_list_append", {acc, ev});
      emit("  br label %" + skip);
      emit("  " + skip + ":");
    } else {
      std::string ev = eval(x.element.get());
      callRt(x.isSet ? "a_set_add" : "a_list_append", {acc, ev});
    }
    emitDefers();
    emit("  br label %" + head);
    term_ = false;
    popScope();
    emit("  " + end + ":");
    return acc;
  }

  // ---- calls -------------------------------------------------------------------
  // Place call args into param slots (values or "" for missing), honoring
  // names and variadic packing — mirrors AsmEmitter::placeArgs.
  void placeArgs(const CallExpr& call, const std::vector<std::string>& paramNames,
                 std::vector<std::string>& slots, int offset, int variadicSlot) {
    std::size_t pos = 0;
    std::vector<std::string> extras;
    for (const CallArg& a : call.args) {
      std::string v = eval(a.value.get());
      if (a.name.empty()) {
        std::size_t p = pos + offset;
        if (variadicSlot >= 0 && (int)p >= variadicSlot) extras.push_back(v);
        else if (p < slots.size()) slots[p] = v;
        ++pos;
      } else {
        for (std::size_t i = 0; i < paramNames.size(); ++i) {
          if (paramNames[i] == a.name) {
            std::size_t p = i + offset;
            if (p < slots.size()) slots[p] = v;
          }
        }
      }
    }
    if (variadicSlot >= 0 && (std::size_t)variadicSlot < slots.size() &&
        slots[variadicSlot].empty()) {
      if (extras.empty()) {
        slots[variadicSlot] = callRtA("a_list_new", "ip", {"0", "null"});
      } else {
        std::string av = argvArray((int)extras.size());
        for (std::size_t i = 0; i < extras.size(); ++i)
          argvStore(av, (int)i, extras[i]);
        slots[variadicSlot] = callRtA("a_list_new", "ip",
                                     {std::to_string(extras.size()), av});
      }
    }
  }

  void emitCallExpr(const CallExpr& call) {
    callResult_ = evalCall(call);
  }
  std::string callResult_;

  std::string evalCall(const CallExpr& call) {
    // Sema-resolved builtin.
    if (call.builtinId != BuiltinId::None) {
      int id = builtinIdForBuiltin(call.builtinId);
      if (id >= 0) return evalBiCall(id, call);
    }
    const Expr* callee = call.callee.get();
    if (callee->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*callee);
      if (call.calleeFunction) {
        auto fit = fns_.find(call.calleeFunction);
        if (fit != fns_.end() && !fit->second->isMethod) {
          FnInfo* fi = fit->second;
          std::vector<std::string> ps(fi->paramNames.size());
          placeArgs(call, fi->paramNames, ps, 0,
                    fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
          return callAFunction(fi->cname, ps);
        }
      }
      if (call.calleeIsBuiltin) {
        int bid = builtinRuntimeId(call.calleeBuiltin.empty() ? id.name
                                                               : call.calleeBuiltin);
        if (bid >= 0) return evalBiCall(bid, call);
      }
      LlBinding* b = lookup(id.name);
      if (b) {
        if (b->kind == BKind::PyFunc) {
          return emitPyCall(b->aux, b->pyMember, call);
        }
        if (b->kind == BKind::FnValue && b->fn) {
          FnInfo* fi = b->fn;
          std::vector<std::string> ps(fi->paramNames.size());
          placeArgs(call, fi->paramNames, ps, 0,
                    fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
          return callAFunction(fi->cname, ps);
        }
        if (b->kind == BKind::StructCtor && b->st) {
          std::vector<std::string> ps(b->st->fieldNames.size());
          placeArgs(call, b->st->fieldNames, ps, 0, -1);
          return callAFunction(b->st->ctorCname, ps);
        }
        if (b->kind == BKind::Builtin) return evalBiCall(b->builtinId, call);
        if (b->kind == BKind::EnumValue || b->kind == BKind::Module) {
          fail("value is not callable");
          return kNone;
        }
        return evalACall(callee, call);
      }
      int bid = builtinRuntimeId(id.name);
      if (bid >= 0) return evalBiCall(bid, call);
      fail("undefined function '" + id.name + "'");
      return kNone;
    }
    if (callee->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*callee);
      if (f.object->kind == ExprKind::Ident) {
        LlBinding* mb = lookup(static_cast<const IdentExpr*>(f.object.get())->name);
        if (mb && mb->kind == BKind::PyModule) {
          return emitPyCall(mb->aux, f.field, call);
        }
        if (mb && mb->kind == BKind::Module) {
          // Module.member(...) — resolve to fn/ctor/builtin/global.
          if (mb->moduleId >= 0) {
            // Try direct function resolution.
            FnInfo* target = nullptr;
            StructInfo* stTarget = nullptr;
            std::string glob;
            resolveModuleCall(*mb, f.field, target, stTarget, glob);
            if (target) {
              std::vector<std::string> ps(target->paramNames.size());
              placeArgs(call, target->paramNames, ps, 0,
                        target->variadic ? (int)target->paramNames.size() - 1 : -1);
              return callAFunction(target->cname, ps);
            }
            if (stTarget) {
              std::vector<std::string> ps(stTarget->fieldNames.size());
              placeArgs(call, stTarget->fieldNames, ps, 0, -1);
              return callAFunction(stTarget->ctorCname, ps);
            }
            if (!glob.empty()) {
              std::vector<std::string> args;
              for (const CallArg& a : call.args) args.push_back(eval(a.value.get()));
              std::string av = argvArray((int)args.size() + 1);
              argvStore(av, 0, kNone);
              for (std::size_t i = 0; i < args.size(); ++i)
                argvStore(av, (int)i + 1, args[i]);
              std::string fv = T();
              emit("  " + fv + " = load i64, ptr @" + glob);
              std::string cr = T();
              emit("  " + cr + " = call i64 @a_call(i64 " + fv + ", i64 " +
                   std::to_string(args.size()) + ", ptr " + av + ")");
              return cr;
            }
          } else {
            // Native module member call.
            int bid = -1;
            if (const std::string* bn = nativeFunction(mb->aux, f.field))
              bid = builtinRuntimeId(*bn);
            if (bid < 0) bid = builtinRuntimeId(f.field);
            if (bid >= 0) return evalBiCall(bid, call);
            fail("native module function not supported by llvm backend: " +
                 mb->aux + "." + f.field);
            return kNone;
          }
          // Member not resolved in the file module: try builtin fallback,
          // then fail (mirrors AsmEmitter).
          {
            int bid = builtinRuntimeId(f.field);
            if (bid >= 0) return evalBiCall(bid, call);
          }
          fail("unknown module member '" + f.field + "'");
          return kNone;
        }
      }
      // Algebraic enum variant construction: Enum.Variant(args...) or
      // mod.Enum.Variant(args...). Sema validated arity; build the
      // {__tag,__name} dict and store payload fields by name.
      {
        std::vector<std::string> parts;
        if (flattenFieldChain(callee, parts) && parts.size() >= 2) {
          const EnumVariantInfo* ev = nullptr;
          LlBinding* fb = lookup(parts.front());
          if (parts.size() >= 3 && fb && fb->kind == BKind::Module &&
              fb->moduleId >= 0) {
            ev = findEnumVariantLl(parts[parts.size() - 2], parts.back(),
                                   fb->moduleId);
          } else if (parts.size() == 2) {
            ev = findEnumVariantLl(parts[0], parts[1], -1);
          }
          if (ev && ev->algebraic) {
            std::string d = emitVariantDict(*ev, parts.back());
            std::size_t n = std::min(call.args.size(), ev->payloadFields.size());
            for (std::size_t i = 0; i < n; ++i) {
              callRt("a_dict_set",
                     {d, avalStrBytes(ev->payloadFields[i]),
                      eval(call.args[i].value.get())});
            }
            return d;
          }
        }
      }
      // Struct method with known receiver type / dynamic dispatch.
      return evalMethodDispatch(f.object.get(), f.field, call);
    }
    return evalACall(callee, call);
  }

  void resolveModuleCall(const LlBinding& mb, const std::string& field,
                         FnInfo*& fn, StructInfo*& st, std::string& glob) {
    fn = nullptr; st = nullptr; glob.clear();
    int mid = mb.moduleId;
    if (mid < 0 || mid >= (int)graph_.modules.size()) return;
    // Follow `from "x.a" import name` re-exports (package __init__ modules).
    auto rm = resolveModuleMember(graph_, mid, field);
    mid = rm.first;
    const std::string& name = rm.second;
    if (mid < 0 || mid >= (int)graph_.modules.size()) return;
    const Module& mod = graph_.at(mid);
    if (!mod.program) return;
    for (FnInfo* fi : fnList_) {
      if (!fi->isMethod && fi->moduleId == mid && fi->name == name) {
        fn = fi;
        return;
      }
    }
    for (StructInfo* si : structList_) {
      if (si->moduleId == mid && si->name == name) {
        st = si;
        return;
      }
    }
    auto git = topLevelGlobals_.find(std::to_string(mid) + ":" + name);
    if (git != topLevelGlobals_.end()) glob = git->second;
    (void)mod;
  }

  // callee(...) where callee is an arbitrary expr -> eval + a_call.
  std::string evalACall(const Expr* callee, const CallExpr& call) {
    std::string fv = eval(callee);
    std::vector<std::string> args;
    for (const CallArg& a : call.args) args.push_back(eval(a.value.get()));
    int n = (int)args.size();
    std::string av = argvArray(n > 0 ? n : 1);
    for (int i = 0; i < n; ++i) argvStore(av, i, args[i]);
    std::string r = T();
    emit("  " + r + " = call i64 @a_call(i64 " + fv + ", i64 " +
         std::to_string(n) + ", ptr " + (n == 0 ? "null" : av) + ")");
    return r;
  }

  // obj.name(args...) dispatch — mirrors AsmEmitter::emitCallExpr's Field
  // branch: statically-known struct receivers call the method directly
  // (self = argv[1]); untyped receivers get a runtime struct-name-checked
  // dispatch with a_method fallback; everything else goes to a_method.
  std::string evalMethodDispatch(const Expr* objExpr, const std::string& field,
                                 const CallExpr& call) {
    TypePtr ot = resolveType(objExpr->inferredType);
    FnInfo* method = nullptr;
    if (ot && ot->kind == TypeKind::Struct) {
      if (StructInfo* st = findStructByNameLl(shortName(ot->structName))) {
        auto mit = st->methods.find(field);
        if (mit != st->methods.end()) method = mit->second;
      }
    }
    if (method) {
      // Direct call: argv[0]=env, argv[1]=self, argv[2..]=params.
      std::string self = eval(objExpr);
      std::vector<std::string> ps(method->paramNames.size());
      ps[0] = self;
      if (ps.size() > 1) {
        std::vector<std::string> rest(method->paramNames.begin() + 1,
                                      method->paramNames.end());
        placeArgs(call, rest, ps, 1,
                  method->variadic ? (int)ps.size() - 1 : -1);
      }
      return callAFunction(method->cname, ps);
    }
    // Untyped/unknown receiver: collect every struct declaring this method,
    // keyed by the display name a_struct_new stores (module qualifier
    // stripped). Ambiguous display names are dropped -> a_method fallback.
    bool isRuntimeValue = ot && ot->kind != TypeKind::Struct &&
                          ot->kind != TypeKind::Error && ot->kind != TypeKind::Var;
    if (!isRuntimeValue) {
      auto dispName = [](const std::string& n) {
        std::size_t sep = n.rfind("::");
        return sep == std::string::npos ? n : n.substr(sep + 2);
      };
      std::unordered_map<std::string, int> nameCount;
      for (StructInfo* st : structList_)
        if (st->methods.count(field)) ++nameCount[dispName(st->name)];
      std::vector<std::pair<std::string, FnInfo*>> cands;
      for (StructInfo* st : structList_) {
        auto mit = st->methods.find(field);
        if (mit == st->methods.end()) continue;
        std::string dn = dispName(st->name);
        if (nameCount[dn] == 1) cands.push_back({dn, mit->second});
      }
      if (!cands.empty())
        return evalCheckedMethodCall(objExpr, field, call, cands);
    }
    return evalMethodCall(objExpr, field, call);
  }

  // Runtime-checked method dispatch for receivers whose static type cannot
  // name the struct. Mirrors AsmEmitter::emitCheckedMethodCall: the receiver
  // must be a T_STRUCT (tagged pointer, 16-aligned payload, AObj.type == 17)
  // whose AStruct.name aval (payload+40) equals the candidate's display
  // name; on match the method is called directly with self = receiver,
  // otherwise a_method handles builtin/container receivers.
  std::string evalCheckedMethodCall(
      const Expr* objExpr, const std::string& field, const CallExpr& call,
      const std::vector<std::pair<std::string, FnInfo*>>& cands) {
    std::string obj = eval(objExpr);
    std::string os = alloca("i64");
    emit("  store i64 " + obj + ", ptr " + os);
    std::string phi = alloca("i64");
    std::string lDone = newLabel("umdn");
    std::string lChk = newLabel("umchk"), lFb = newLabel("umfb");
    // T_STRUCT test: tagged pointer (top16 == 0xFFFF), 16-aligned payload,
    // nonzero low-48 payload pointer.
    std::string ov = T(), top = T(), cTop = T(), al = T(), cAl = T(),
                pay = T(), cPay = T(), c1 = T(), c2 = T();
    emit("  " + ov + " = load i64, ptr " + os);
    emit("  " + top + " = lshr i64 " + ov + ", 48");
    emit("  " + cTop + " = icmp eq i64 " + top + ", 65535");
    emit("  " + al + " = and i64 " + ov + ", 15");
    emit("  " + cAl + " = icmp eq i64 " + al + ", 0");
    emit("  " + pay + " = and i64 " + ov + ", 281474976710655");
    emit("  " + cPay + " = icmp ne i64 " + pay + ", 0");
    emit("  " + c1 + " = and i1 " + cTop + ", " + cAl);
    emit("  " + c2 + " = and i1 " + c1 + ", " + cPay);
    emit("  br i1 " + c2 + ", label %" + lChk + ", label %" + lFb);
    emit("  " + lChk + ":");
    // AObj.type == T_STRUCT (17) at payload+0.
    std::string objp = T(), ty = T(), cTy = T();
    emit("  " + objp + " = inttoptr i64 " + pay + " to ptr");
    emit("  " + ty + " = load i32, ptr " + objp);
    emit("  " + cTy + " = icmp eq i32 " + ty + ", 17");
    std::string lName = newLabel("umnm");
    emit("  br i1 " + cTy + ", label %" + lName + ", label %" + lFb);
    emit("  " + lName + ":");
    // AStruct.name aval at payload+40 (i64 index 5).
    std::string np = T(), nv = T();
    emit("  " + np + " = getelementptr i64, ptr " + objp + ", i64 5");
    emit("  " + nv + " = load i64, ptr " + np);
    for (std::size_t i = 0; i < cands.size(); ++i) {
      const auto& c = cands[i];
      bool last = i + 1 == cands.size();
      std::string lNext = last ? lFb : newLabel("umnx");
      std::string nameV = avalStrBytes(c.first);
      std::string eq = callRt("a.eq.fast", {nv, nameV});
      std::string tr = callRt("a_truthy_raw_fast", {eq});
      std::string ok = T();
      emit("  " + ok + " = icmp ne i64 " + tr + ", 0");
      std::string lCall = newLabel("umcl");
      emit("  br i1 " + ok + ", label %" + lCall + ", label %" + lNext);
      emit("  " + lCall + ":");
      FnInfo* m = c.second;
      std::string self = T();
      emit("  " + self + " = load i64, ptr " + os);
      std::vector<std::string> ps(m->paramNames.size());
      ps[0] = self;
      if (ps.size() > 1) {
        std::vector<std::string> rest(m->paramNames.begin() + 1,
                                      m->paramNames.end());
        placeArgs(call, rest, ps, 1,
                  m->variadic ? (int)ps.size() - 1 : -1);
      }
      std::string res = callAFunction(m->cname, ps);
      emit("  store i64 " + res + ", ptr " + phi);
      emit("  br label %" + lDone);
      if (!last) emit("  " + lNext + ":");
    }
    // Fallback: generic a_method dispatch (receiver already evaluated once).
    emit("  " + lFb + ":");
    std::string selfv = T();
    emit("  " + selfv + " = load i64, ptr " + os);
    std::string fb = evalMethodCallObj(selfv, field, call);
    emit("  store i64 " + fb + ", ptr " + phi);
    emit("  br label %" + lDone);
    emit("  " + lDone + ":");
    std::string out = T();
    emit("  " + out + " = load i64, ptr " + phi);
    return out;
  }

  // obj.name(args...) -> a_method(obj, "name", n, nkw, argv).
  std::string evalMethodCall(const Expr* objExpr, const std::string& field,
                             const CallExpr& call) {
    return evalMethodCallObj(eval(objExpr), field, call);
  }
  // Same, with the receiver already evaluated into an i64 value/alloca.
  std::string evalMethodCallObj(const std::string& obj, const std::string& field,
                                const CallExpr& call) {
    std::string nm = cstrGlobal(field, true);
    // Separate positional and named args; named become (name,value) pairs.
    std::vector<std::string> pos, named;
    for (const CallArg& a : call.args) {
      if (a.name.empty()) pos.push_back(eval(a.value.get()));
      else {
        named.push_back(avalStrBytes(a.name));
        named.push_back(eval(a.value.get()));
      }
    }
    int nkw = (int)named.size() / 2;
    int total = (int)pos.size() + (int)named.size();
    std::string av = argvArray(total > 0 ? total : 1);
    for (std::size_t i = 0; i < pos.size(); ++i) argvStore(av, (int)i, pos[i]);
    for (std::size_t i = 0; i < named.size(); ++i)
      argvStore(av, (int)pos.size() + (int)i, named[i]);
    std::string r = T();
    emit("  " + r + " = call i64 @a_method(i64 " + obj + ", ptr " +
         nm + ", i64 " + std::to_string(pos.size()) + ", i64 " +
         std::to_string(nkw) + ", ptr " + (total == 0 ? "null" : av) + ")");
    return r;
  }

  // py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
  // the module/member strings, argv[2] the kwargs count, then positional
  // args, then name/value pairs. Dispatched through the runtime "py_call"
  // builtin (id 162), which resolves libpython — mirrors
  // AsmEmitter::emitPyCall.
  std::string emitPyCall(const std::string& module, const std::string& member,
                         const CallExpr& call) {
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, std::string>> kw;
    for (const CallArg& a : call.args) {
      std::string v = eval(a.value.get());
      if (a.name.empty()) {
        pos.push_back(v);
      } else {
        kw.push_back({a.name, v});
      }
    }
    int n = 3 + (int)pos.size() + 2 * (int)kw.size();
    std::string av = argvArray(n);
    argvStore(av, 0, avalStrBytes(module));
    argvStore(av, 1, avalStrBytes(member));
    argvStore(av, 2, avalIntLit((std::int64_t)kw.size()));
    for (std::size_t i = 0; i < pos.size(); ++i)
      argvStore(av, 3 + (int)i, pos[i]);
    for (std::size_t i = 0; i < kw.size(); ++i) {
      argvStore(av, 3 + (int)pos.size() + 2 * (int)i,
                avalStrBytes(kw[i].first));
      argvStore(av, 3 + (int)pos.size() + 2 * (int)i + 1, kw[i].second);
    }
    return callRtA("a_bi_call", "iip",
                   {std::to_string(builtinRuntimeId("py_call")),
                    std::to_string(n), av});
  }

  // Builtin call: a_bi_call(id, n, argv) — argv holds just the args.
  std::string evalBiCall(int id, const CallExpr& call) {
    std::vector<std::string> args;
    for (const CallArg& a : call.args) args.push_back(eval(a.value.get()));
    int n = (int)args.size();
    std::string av = argvArray(n > 0 ? n : 1);
    for (int i = 0; i < n; ++i) argvStore(av, i, args[i]);
    std::string r = T();
    emit("  " + r + " = call i64 @a_bi_call(i64 " + std::to_string(id) +
         ", i64 " + std::to_string(n) + ", ptr " + (n == 0 ? "null" : av) + ")");
    return r;
  }

  // ---- struct ctor --------------------------------------------------------------
  void emitStructCtorBody(StructInfo* st) {
    // Mirrors AsmEmitter::emitStructCtor: build name/methods/fns, call
    // a_struct_new, set fields from argv, promote, return.
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    resetGlobalScope(st->moduleId);
    pushScope();
    // Display name: strip module qualifier.
    std::string disp = st->name;
    std::size_t sep = disp.rfind("::");
    if (sep != std::string::npos) disp = disp.substr(sep + 2);
    std::string nameV = avalStrBytes(disp);
    // Method-name list.
    std::vector<std::string> mnames;
    for (const auto& kv : st->methods) mnames.push_back(kv.first);
    std::sort(mnames.begin(), mnames.end());
    std::string methL = callRtA("a_list_new", "ip", {"0", "null"});
    for (const std::string& mn : mnames) {
      std::string ms = avalStrBytes(mn);
      callRt("a_list_append", {methL, ms});
    }
    // Method-name -> fn dict.
    std::string fnsD = callRt("a_dict_new", {});
    for (const std::string& mn : mnames) {
      FnInfo* mf = st->methods[mn];
      std::string ks = avalStrBytes(mn);
      std::string vs = fixFnonNew("", mf->cname, kNone, mn);
      callRt("a_dict_set", {fnsD, ks, vs});
    }
    std::string obj = callRt("a_struct_new", {nameV, methL, fnsD});
    // Fields: argv[i+1] into dict under field name.
    for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
      std::string g = T(), fv = T();
      emit("  " + g + " = getelementptr i64, ptr %argv, i64 " +
           std::to_string(i + 1));
      emit("  " + fv + " = load i64, ptr " + g);
      std::string ks = avalStrBytes(st->fieldNames[i]);
      callRt("a_dict_set", {obj, ks, fv});
    }
    callRt("a_promote", {obj});
    emit("  ret i64 " + obj);
    std::ostringstream head;
    for (auto& kv : pendingAllocas_)
      head << "  " << kv.first << " = alloca " << kv.second << "\n";
    body_ << "define i64 @" << st->ctorCname << "(ptr %argv) {\n"
          << head.str() << fn_.str() << "}\n\n";
    scopes_.clear();
  }

  // ---- module init / main ---------------------------------------------------------
  void emitModInit(int moduleId) {
    const Module& mod = graph_.at(moduleId);
    if (!mod.program) return;
    if (mod.id != graph_.entry && mod.name == "prelude") return;
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    int savedModule = curModuleId_;
    curModuleId_ = moduleId;
    resetGlobalScope(moduleId);
    pushScope();
    bindModuleImports(moduleId);
    inFunctionBody_ = false;
    deferStack_.emplace_back();
    for (const StmtPtr& s : mod.program->statements) emitStmt(*s);
    if (!term_) {
      emitDefers();
      emit("  ret i64 " + kNone);
    }
    term_ = false;
    clearDeferFrames();
    inFunctionBody_ = true;
    curModuleId_ = savedModule;
    scopes_.clear();
    std::ostringstream head;
    for (auto& kv : pendingAllocas_)
      head << "  " << kv.first << " = alloca " << kv.second << "\n";
    body_ << "define i64 @" << modInitSym(moduleId) << "(ptr %argv) {\n"
          << head.str() << fn_.str() << "}\n\n";
  }

  void emitMain() {
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    resetGlobalScope(graph_.entry);
    pushScope();
    deferStack_.emplace_back();
    emit("  call void @a_runtime_init()");
    // Struct ctor fnon values.
    for (StructInfo* st : structList_) {
      std::string sig = st->name + "(";
      for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
        if (i) sig += ", ";
        sig += st->fieldNames[i];
      }
      sig += ")";
      std::string v = fixFnonNewEx(st->ctorCname, kNone, st->name, sig,
                                   st->decl ? st->decl->doc : "");
      emit("  store i64 " + v + ", ptr @" + st->globalVar);
    }
    // Function fnon values.
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      std::string sig = fnSignatureText(f);
      std::string v = fixFnonNewEx(f->cname, kNone, f->name, sig, fnDocText(f));
      emit("  store i64 " + v + ", ptr @" + f->globalVar);
    }
    // Simple enum values.
    for (auto& kv : enums_) {
      if (kv.second.algebraic) continue;
      std::string v = callRt("a_i64n", {std::to_string(kv.second.value)});
      emit("  store i64 " + v + ", ptr @" + kv.second.globalVar);
    }
    // Module inits in graph order.
    for (int mid : moduleOrder()) {
      if (mid < 0 || mid >= (int)graph_.modules.size()) continue;
      const Module& mod = graph_.at(mid);
      if (!mod.program) continue;
      if (mod.id != graph_.entry && mod.name == "prelude") continue;
      callModInit(mid);
    }
    if (testMode_) {
      emitTestMain();
    } else {
      emitEntryCall();
      emit("  ret i32 0");
    }
    clearDeferFrames();
    std::ostringstream head;
    for (auto& kv : pendingAllocas_)
      head << "  " << kv.first << " = alloca " << kv.second << "\n";
    body_ << "define i32 @main(i32 %argc, ptr %argv0) {\n"
          << head.str() << fn_.str() << "}\n\n";
    scopes_.clear();
  }

  void callModInit(int moduleId) {
    std::string av = argvArray(1);
    argvStore(av, 0, kNone);
    emit("  " + T() + " = call i64 @" + modInitSym(moduleId) + "(ptr " + av + ")");
  }

  void emitEntryCall() {
    FnInfo* entryFn = nullptr;
    for (FnInfo* f : fnList_) {
      if (!f->isMethod && f->isEntry && f->moduleId == graph_.entry) {
        if (entryFn) {
          fail("multiple 'entry' functions");
          return;
        }
        entryFn = f;
      }
    }
    if (!entryFn) return;
    std::vector<std::string> ps(entryFn->paramNames.size());
    if (entryFn->variadic && !ps.empty())
      ps.back() = callRtA("a_list_new", "ip", {"0", "null"});
    (void)callAFunction(entryFn->cname, ps);
  }

  void emitTestMain() {
    std::vector<FnInfo*> tests;
    for (FnInfo* f : fnList_) {
      if (!f->isMethod && f->moduleId == graph_.entry &&
          f->name.rfind("test_", 0) == 0 &&
          (testFilter_.empty() || f->name.find(testFilter_) != std::string::npos))
        tests.push_back(f);
    }
    std::sort(tests.begin(), tests.end(),
              [](const FnInfo* a, const FnInfo* b) { return a->name < b->name; });
    if (tests.empty()) {
      emit("  ret i32 0");
      return;
    }
    // names/fns arrays as LLVM globals.
    std::string namesArr = "@__a_test_names";
    std::string fnsArr = "@__a_test_fns";
    std::vector<std::string> nameSyms;
    for (const FnInfo* t : tests) nameSyms.push_back(cstrGlobal(t->name, true));
    pre_ << "  " << namesArr << " = private global [" << tests.size()
         << " x ptr] [";
    for (std::size_t i = 0; i < tests.size(); ++i) {
      if (i) pre_ << ", ";
      pre_ << "ptr " << nameSyms[i];
    }
    pre_ << "]\n";
    pre_ << "  " << fnsArr << " = private global [" << tests.size()
         << " x ptr] [";
    for (std::size_t i = 0; i < tests.size(); ++i) {
      if (i) pre_ << ", ";
      pre_ << "ptr @" << tests[i]->cname;
    }
    pre_ << "]\n";
    std::string r = T();
    emit("  " + r + " = call i32 @a_test_main(i64 " +
         std::to_string(tests.size()) + ", ptr " + namesArr + ", ptr " +
         fnsArr + ")");
    emit("  ret i32 " + r);
  }

  // a_fnon_new_ex(entry, env, name, sig, doc) — all ptr/i64 args.
  std::string fixFnonNewEx(const std::string& cname, const std::string& env,
                           const std::string& name, const std::string& sig,
                           const std::string& doc) {
    std::string nn = cstrGlobal(name, true);
    std::string ss = cstrGlobal(sig, true);
    std::string dd = cstrGlobal(doc, true);
    std::string r = T();
    emit("  " + r + " = call i64 @a_fnon_new_ex(i64 ptrtoint (ptr @" + cname +
         " to i64), i64 " + env + ", ptr " + nn + ", ptr " +
         ss + ", ptr " + dd + ")");
    return r;
  }

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
  static std::string fnDocText(const FnInfo* f) {
    if (!f->decl) return "";
    if (!f->decl->doc.empty()) return f->decl->doc;
    if (!f->decl->body.empty()) {
      std::string out;
      if (docstringText(*f->decl->body.front(), out)) return out;
    }
    return "";
  }

  void emitGlobalDefs() {
    std::set<std::string> seen;
    auto def = [&](const std::string& sym) {
      if (seen.insert(sym).second)
        pre_ << "  @" << sym << " = global i64 0\n";
    };
    for (const std::string& s : globals_) def(s);
    for (FnInfo* f : fnList_) def(f->globalVar);
    for (StructInfo* st : structList_) def(st->globalVar);
    for (auto& kv : enums_) def(kv.second.globalVar);
  }

  void emitGcRoots() {
    std::set<std::string> roots;
    for (const std::string& s : globals_) roots.insert(s);
    for (FnInfo* f : fnList_)
      if (!f->isMethod) roots.insert(f->globalVar);
    for (StructInfo* st : structList_) roots.insert(st->globalVar);
    for (auto& kv : enums_)
      if (!kv.second.algebraic) roots.insert(kv.second.globalVar);
    if (roots.empty()) {
      pre_ << "  @__a_gc_roots = global [1 x i64] [i64 0]\n"
              "  @__a_gc_nroots = global i64 0\n";
      return;
    }
    pre_ << "  @__a_gc_roots = global [" << roots.size() << " x i64] [";
    bool first = true;
    for (const std::string& s : roots) {
      if (!first) pre_ << ", ";
      first = false;
      pre_ << "i64 ptrtoint (ptr @" << s << " to i64)";
    }
    pre_ << "]\n  @__a_gc_nroots = global i64 " << roots.size() << "\n";
  }
};

}  // namespace

bool emitLLVM(const ModuleGraph& graph, const SemaResult& sema,
              std::string& out, std::string& error, bool testMode,
              const std::string* testFilter) {
  LlvmEmitter emitter(graph, sema, testMode, testFilter);
  return emitter.run(out, error);
}

}  // namespace a
