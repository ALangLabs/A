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
#include <deque>
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
  std::string slotF;  // numMode==1: alloca i1 holding the is-double flag
  int numMode = 0;    // 1 = unboxed numeric: slot=i64 payload, slotF=i1 flag
  int vecMode = 0;    // 1 = unboxed SIMD vector: slot=alloca <N x ty>
  std::string vecTy;  // vecMode: LLVM vector type, e.g. "<4 x double>"
  int vecN = 0;       // vecMode: lane count
  bool vecFloat = false;  // vecMode: lanes are float/double (else i64)
  std::string vecLane;  // vecMode: LLVM lane type ("i64"/"double"/"float")
  bool rawSafe = false;  // numMode: flag provably never 2 (boxed aval)
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
  // FnInfos for `extern fn` C declarations: no A body is emitted; calls go
  // straight to the C symbol (FnInfo::cname holds the extern name).
  std::unordered_set<const FnInfo*> externFns_;
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

  // ---- raw-numeric safety analysis --------------------------------------
  // Per-function pre-pass proving that a numMode binding can never hold
  // flag==2 (a boxed aval in the payload slot): every write to the binding
  // must produce a raw numeric value (literal, raw numMode ident, or raw
  // unary/binary op over proven-safe operands). Params and anything fed by
  // calls/builtins/collections stay unproven -> guards remain. See
  // analyzeRawSafety for details.
  struct RawBindInfo;
  struct RawWrite {
    const Expr* e = nullptr;                 // assigned value expression
    BinOp cop = BinOp::Invalid;              // Invalid = `=`, else compound op
    std::unordered_map<std::string, RawBindInfo*> env;  // visible bindings
  };
  struct RawBindInfo {
    enum Cls : std::uint8_t { kCand, kPlain, kGlobal, kShadow };
    enum State : std::uint8_t { kSafe, kUnsafe, kNotNum };
    Cls cls = kCand;
    State state = kSafe;
    bool isParam = false;
    const Expr* init = nullptr;              // let/assign-new initializer
    std::unordered_map<std::string, RawBindInfo*> initEnv;
    std::vector<RawWrite> writes;            // post-init assignments
  };
  struct RawFrame {
    std::map<std::string, RawBindInfo*> m;
    std::vector<const Stmt*> defers;         // deferred stmts (run at exit)
  };
  std::deque<RawFrame> rawFrameArena_;
  std::deque<RawBindInfo> rawInfoArena_;
  std::vector<RawFrame*> rawFrames_;
  std::vector<RawBindInfo*> rawCands_;
  std::vector<std::pair<const Stmt*, RawBindInfo*>> rawStmtBinds_;
  std::unordered_map<const Stmt*, bool> rawSafeStmts_;
  RawBindInfo rawPlainSentinel_, rawGlobalSentinel_, rawShadowSentinel_;
  bool rawInFnBody_ = true;
  int rawModuleId_ = -1;

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
          if (fn->isExtern) {
            // C symbol name; the mangled __a_f_* cname is unused.
            info->cname = fn->externName.empty() ? fn->name : fn->externName;
            externFns_.insert(info);
          }
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
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
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
    emitVecHelpers();
    emitExternDecls();
    emitExternHelpers();
  }

  // ---- C FFI (extern fn) ------------------------------------------------------
  // Map an A signature type to the LLVM type used in the extern `declare`.
  // Integer params always widen to i64 (the C callee reads the low bits of
  // the argument register); narrower returns keep their width so the ABI
  // sign/zero extension is correct. `what` is for diagnostics.
  std::string ffiLlType(const TypePtr& type, bool isReturn,
                        const std::string& what) {
    TypePtr t = resolveType(type);
    if (!t || t->kind == TypeKind::Error) {
      fail("extern '" + what + "' has an unresolved type");
      return isReturn ? "void" : "i64";
    }
    switch (t->kind) {
      case TypeKind::Int: {
        if (!isReturn) return "i64";
        if (t->bits == 8) return "i8";
        if (t->bits == 16) return "i16";
        if (t->bits == 32) return "i32";
        return "i64";
      }
      case TypeKind::Float:
        return t->bits <= 32 ? "float" : "double";
      case TypeKind::Bool:
        return isReturn ? "i1" : "i32";
      case TypeKind::String:
        return "ptr";
      case TypeKind::Char:
        return "i64";
      case TypeKind::Pointer:
        return "i64";  // raw address as an integer
      case TypeKind::Bytes:
        if (!isReturn) return "ptr";
        break;
      case TypeKind::Void:
        if (isReturn) return "void";
        break;
      case TypeKind::Var:
        return isReturn ? "void" : "i64";
      default:
        break;
    }
    fail("type '" + typeName(t) + "' cannot cross the C ABI in extern '" +
         what + "'");
    return isReturn ? "void" : "i64";
  }

  void emitExternDecls() {
    // strlen boxes `-> str` returns (C char* -> A string). Declared only
    // when needed and skipped when the user already declares `strlen`
    // themselves (their `declare` covers it).
    bool needStrlen = false, userDeclaresStrlen = false;
    for (FnInfo* f : fnList_) {
      if (!externFns_.count(f) || !f->decl) continue;
      auto rit = sema_.fnReturnTypes.find(f->decl);
      if (rit == sema_.fnReturnTypes.end()) continue;
      TypePtr rt = resolveType(rit->second);
      if (rt && rt->kind == TypeKind::String) needStrlen = true;
      if (f->cname == "strlen") userDeclaresStrlen = true;
    }
    if (needStrlen && !userDeclaresStrlen)
      pre_ << "declare i64 @strlen(ptr)\n";
    std::set<std::string> emitted;
    for (FnInfo* f : fnList_) {
      if (!externFns_.count(f) || !f->decl) continue;
      auto pit = sema_.fnParamTypes.find(f->decl);
      auto rit = sema_.fnReturnTypes.find(f->decl);
      if (pit == sema_.fnParamTypes.end() ||
          rit == sema_.fnReturnTypes.end()) {
        fail("extern '" + f->name + "' is missing sema signature types");
        continue;
      }
      TypePtr rt = resolveType(rit->second);
      std::string sig = "declare " + ffiLlType(rit->second, true,
                                             f->name + " return") + " @" +
                        f->cname + "(";
      for (std::size_t i = 0; i < pit->second.size(); ++i) {
        if (i) sig += ", ";
        sig += ffiLlType(pit->second[i], false,
                         "parameter '" +
                             (i < f->paramNames.size() ? f->paramNames[i]
                                                       : std::to_string(i)) +
                             "' of extern '" + f->name + "'");
      }
      sig += ")\n";
      // Two A decls may alias the same C symbol (`= "sym"`); emit the
      // declare once when the signatures are identical.
      if (emitted.insert(sig).second) pre_ << sig;
      (void)rt;
    }
  }

  // Marshall an aval argument to the C value for `type`. All helpers are
  // internal IR functions emitted by emitExternHelpers.
  std::string ffiArgValue(const std::string& aval, const TypePtr& type) {
    auto callI = [&](const char* sym, const char* retty) {
      std::string r = T();
      emit("  " + r + " = call " + retty + " @" + sym + "(i64 " + aval + ")");
      return r;
    };
    TypePtr t = resolveType(type);
    if (!t) return "0";
    switch (t->kind) {
      case TypeKind::Int:
      case TypeKind::Char:
      case TypeKind::Pointer:
        return callI("a.ffi.i64", "i64");
      case TypeKind::Float:
        if (t->bits <= 32) {
          std::string d = callI("a.ffi.f64", "double");
          std::string r = T();
          emit("  " + r + " = fptrunc double " + d + " to float");
          return r;
        }
        return callI("a.ffi.f64", "double");
      case TypeKind::Bool:
        return callI("a.ffi.bool", "i32");
      case TypeKind::String:
        return callI("a.ffi.str", "ptr");
      case TypeKind::Bytes:
        return callI("a.ffi.bytes", "ptr");
      default:
        fail("type '" + typeName(t) + "' cannot be passed to a C function");
        return "0";
    }
  }

  // Convert the raw C return value `r` (of LLVM type `retLl`) into an aval.
  std::string ffiReturnValue(const std::string& r, const std::string& retLl,
                             const TypePtr& type) {
    TypePtr t = resolveType(type);
    if (!t || retLl == "void") return kNone;
    switch (t->kind) {
      case TypeKind::Int: {
        std::string wide;
        if (retLl != "i64") {
          wide = T();
          // Unsigned returns zero-extend; signed sign-extend.
          emit("  " + wide + " = " + (t->isSigned ? "sext " : "zext ") +
               retLl + " " + r + " to i64");
        } else {
          wide = r;
        }
        std::string out = T();
        emit("  " + out + " = call i64 @a_i64n(i64 " + wide + ")");
        return out;
      }
      case TypeKind::Char:
      case TypeKind::Pointer: {
        std::string out = T();
        emit("  " + out + " = call i64 @a_i64n(i64 " + r + ")");
        return out;
      }
      case TypeKind::Float: {
        std::string d = r;
        if (retLl == "float") {
          d = T();
          emit("  " + d + " = fpext float " + r + " to double");
        }
        std::string out = T();
        emit("  " + out + " = call i64 @a.mdouble(double " + d + ")");
        return out;
      }
      case TypeKind::Bool: {
        std::string out = T();
        emit("  " + out + " = select i1 " + r + ", i64 " + kTrue +
             ", i64 " + kFalse);
        return out;
      }
      case TypeKind::String: {
        // char* -> A string via strlen + a_str_new.
        std::string len = T();
        emit("  " + len + " = call i64 @strlen(ptr " + r + ")");
        std::string out = T();
        emit("  " + out + " = call i64 @a_str_new(ptr " + r + ", i64 " +
             len + ")");
        return out;
      }
      default:
        fail("type '" + typeName(t) + "' cannot be returned from a C function");
        return kNone;
    }
  }

  // Call an extern C function: marshal aval args to C args, call the
  // symbol, box the C return into an aval.
  std::string emitExternCall(FnInfo* f, const CallExpr& call) {
    auto pit = sema_.fnParamTypes.find(f->decl);
    auto rit = sema_.fnReturnTypes.find(f->decl);
    if (pit == sema_.fnParamTypes.end() || rit == sema_.fnReturnTypes.end() ||
        !f->decl) {
      fail("extern '" + f->name + "' is missing its sema signature");
      return kNone;
    }
    std::vector<std::string> ps(f->paramNames.size());
    placeArgs(call, f->paramNames, ps, 0, -1);
    std::string retLl = ffiLlType(rit->second, true, f->name + " return");
    std::string args;
    for (std::size_t i = 0; i < ps.size(); ++i) {
      if (i) args += ", ";
      if (ps[i].empty()) {
        fail("missing argument " + std::to_string(i + 1) + " to extern '" +
             f->name + "'");
        return kNone;
      }
      TypePtr pt = i < pit->second.size() ? pit->second[i] : nullptr;
      std::string ty = ffiLlType(pt, false,
                                 "parameter '" +
                                     (i < f->paramNames.size()
                                          ? f->paramNames[i]
                                          : std::to_string(i)) +
                                     "' of extern '" + f->name + "'");
      args += ty + " " + ffiArgValue(ps[i], pt);
    }
    if (retLl == "void") {
      emit("  call void @" + f->cname + "(" + args + ")");
      return kNone;
    }
    std::string r = T();
    emit("  " + r + " = call " + retLl + " @" + f->cname + "(" + args + ")");
    return ffiReturnValue(r, retLl, rit->second);
  }

  // Marshalling helpers for extern calls. aval layout mirrors a_runtime.h:
  // tagged pointers have payload&0xF==0 and point at a 16-byte AObj header;
  // AStr/ABytes store their data pointer (or inline bytes) at offset 16.
  void emitExternHelpers() {
    std::string msgNum = cstrGlobal("extern: expected a number", true);
    std::string msgStr = cstrGlobal("extern: expected a string", true);
    std::string msgBytes = cstrGlobal("extern: expected bytes", true);
    helpers_ << "define internal i64 @a.ffi.i64(i64 %v) {\n"
                "entry:\n"
                "  %un = call { i64, i64 } @a.unboxnum(i64 %v)\n"
                "  %p = extractvalue { i64, i64 } %un, 0\n"
                "  %f = extractvalue { i64, i64 } %un, 1\n"
                "  %isD = icmp eq i64 %f, 1\n"
                "  %db = bitcast i64 %p to double\n"
                "  %di = fptosi double %db to i64\n"
                "  %isB = icmp eq i64 %f, 2\n"
                "  br i1 %isB, label %bad, label %ok\n"
                "bad:\n"
                "  call i64 @a_panic_msg(ptr " << msgNum << ")\n"
                "  unreachable\n"
                "ok:\n"
                "  %r = select i1 %isD, i64 %di, i64 %p\n"
                "  ret i64 %r\n"
                "}\n\n"
                "define internal double @a.ffi.f64(i64 %v) {\n"
                "entry:\n"
                "  %un = call { i64, i64 } @a.unboxnum(i64 %v)\n"
                "  %p = extractvalue { i64, i64 } %un, 0\n"
                "  %f = extractvalue { i64, i64 } %un, 1\n"
                "  %isI = icmp eq i64 %f, 0\n"
                "  %si = sitofp i64 %p to double\n"
                "  %db = bitcast i64 %p to double\n"
                "  %isB = icmp eq i64 %f, 2\n"
                "  br i1 %isB, label %bad, label %ok\n"
                "bad:\n"
                "  call i64 @a_panic_msg(ptr " << msgNum << ")\n"
                "  unreachable\n"
                "ok:\n"
                "  %r = select i1 %isI, double %si, double %db\n"
                "  ret double %r\n"
                "}\n\n"
                "define internal i32 @a.ffi.bool(i64 %v) {\n"
                "  %t = call i1 @a.truthy(i64 %v)\n"
                "  %r = zext i1 %t to i32\n"
                "  ret i32 %r\n"
                "}\n\n";
    // Shared tagged-pointer prologue for str/bytes: verify the aval is a
    // heap object of the expected AObj type before touching its payload.
    auto ptrObj = [&](const char* name, int wantType, const char* bodyGo,
                      const std::string& msg) {
      helpers_ << "define internal ptr @" << name << "(i64 %v) {\n"
                  "entry:\n"
                  "  %tag = icmp uge i64 %v, -281474976710656\n"
                  "  %pay = and i64 %v, 281474976710655\n"
                  "  %low = and i64 %pay, 15\n"
                  "  %isP = icmp eq i64 %low, 0\n"
                  "  %nz = icmp ne i64 %pay, 0\n"
                  "  %pshp = and i1 %isP, %nz\n"
                  "  %shape = and i1 %tag, %pshp\n"
                  "  br i1 %shape, label %chk, label %bad\n"
                  "chk:\n"
                  "  %op = inttoptr i64 %pay to ptr\n"
                  "  %ty = load i32, ptr %op\n"
                  "  %ok = icmp eq i32 %ty, " << wantType << "\n"
                  "  br i1 %ok, label %go, label %bad\n"
                  "go:\n" << bodyGo << "bad:\n"
                  "  call i64 @a_panic_msg(ptr " << msg << ")\n"
                  "  unreachable\n"
                  "}\n\n";
    };
    ptrObj("a.ffi.str", 1,
           "  %d = getelementptr i8, ptr %op, i64 16\n"
           "  ret ptr %d\n",
           msgStr);
    ptrObj("a.ffi.bytes", 18,
           "  %dp = getelementptr i8, ptr %op, i64 16\n"
           "  %p = load ptr, ptr %dp\n"
           "  ret ptr %p\n",
           msgBytes);
  }

  // ---- SIMD vector helpers ----------------------------------------------------
  // aval -> lane value for vector unboxing. a.unboxnum yields {payload, flag}
  // with flag 0 = int payload, 1 = double bits, 2 = boxed aval (non-numeric;
  // treated as its raw payload — vector lanes are always numeric in practice).
  void emitVecHelpers() {
    helpers_ << R"LLVM(define internal i64 @a.vlane.i64(i64 %v) {
entry:
  %un = call { i64, i64 } @a.unboxnum(i64 %v)
  %p = extractvalue { i64, i64 } %un, 0
  %f = extractvalue { i64, i64 } %un, 1
  %isD = icmp eq i64 %f, 1
  %db = bitcast i64 %p to double
  %di = fptosi double %db to i64
  %r = select i1 %isD, i64 %di, i64 %p
  ret i64 %r
}

define internal double @a.vlane.f64(i64 %v) {
entry:
  %un = call { i64, i64 } @a.unboxnum(i64 %v)
  %p = extractvalue { i64, i64 } %un, 0
  %f = extractvalue { i64, i64 } %un, 1
  %isI = icmp eq i64 %f, 0
  %db = bitcast i64 %p to double
  %si = sitofp i64 %p to double
  %r = select i1 %isI, double %si, double %db
  ret double %r
}

)LLVM";
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
    // div: true division (Python parity) — int/int yields double, so both
    // paths reduce to nval/nval/fdiv with a zero check routed to slow.
    helpers_ << head("a.div.fast") << R"LLVM(intp:
  %a = call i64 @a.ival(i64 %l)
  %b = call i64 @a.ival(i64 %r)
  %bz = icmp eq i64 %b, 0
  br i1 %bz, label %slow, label %go
go:
  %da0 = sitofp i64 %a to double
  %db0 = sitofp i64 %b to double
  %dr0 = fdiv double %da0, %db0
  %bx0 = call i64 @a.mdouble(double %dr0)
  ret i64 %bx0
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

; Box an unboxed numeric {payload, isDouble} into an aval. Payload is the
; int64 value when %d is false, the raw double bits when true.
define internal i64 @a.boxnum(i64 %p, i1 %d) {
  br i1 %d, label %dd, label %ii
dd:
  %db = bitcast i64 %p to double
  %r = call i64 @a.mdouble(double %db)
  ret i64 %r
ii:
  %ok = call i1 @a.inrange(i64 %p)
  br i1 %ok, label %m, label %big
m:
  %r2 = call i64 @a.mint(i64 %p)
  ret i64 %r2
big:
  %r3 = call i64 @a_i64n(i64 %p)
  ret i64 %r3
}

; Inverse of a.boxnum for aval inputs: returns {payload, flag} where flag is
; 1 for doubles. Handles inline ints, boxed T_INT64 (AInt64.v at payload+16),
; plain doubles, and falls back to a_float for anything else (bool, numeric
; strings); non-numeric values panic inside a_float like the boxed paths.
define internal { i64, i64 } @a.unboxnum(i64 %v) {
entry:
  %isi = call i1 @a.isintv(i64 %v)
  br i1 %isi, label %ii, label %nc
ii:
  %iv = call i64 @a.ival(i64 %v)
  %r0 = insertvalue { i64, i64 } undef, i64 %iv, 0
  %r1 = insertvalue { i64, i64 } %r0, i64 0, 1
  ret { i64, i64 } %r1
nc:
  %notag = icmp ult i64 %v, -281474976710656
  br i1 %notag, label %dd, label %tg
dd:
  %r2 = insertvalue { i64, i64 } undef, i64 %v, 0
  %r3 = insertvalue { i64, i64 } %r2, i64 1, 1
  ret { i64, i64 } %r3
tg:
  %pay = and i64 %v, 281474976710655
  %al = and i64 %pay, 15
  %nz = icmp ne i64 %pay, 0
  %aln = icmp eq i64 %al, 0
  %isp = and i1 %aln, %nz
  br i1 %isp, label %obj, label %oth
obj:
  %op = inttoptr i64 %pay to ptr
  %ty = load i32, ptr %op
  %is64 = icmp eq i32 %ty, 11
  br i1 %is64, label %i64, label %oth
i64:
  %vp = getelementptr i64, ptr %op, i64 2
  %vv = load i64, ptr %vp
  %r4 = insertvalue { i64, i64 } undef, i64 %vv, 0
  %r5 = insertvalue { i64, i64 } %r4, i64 0, 1
  ret { i64, i64 } %r5
oth:
  ; Non-numeric aval (string, bool, None, ...): keep it boxed — flag 2
  ; means "payload slot holds an aval", which every numMode consumer
  ; already handles via the boxed slow paths. Never panics, so generic or
  ; dynamically-dispatched numeric params degrade to boxed semantics.
  %r6 = insertvalue { i64, i64 } undef, i64 %v, 0
  %r7 = insertvalue { i64, i64 } %r6, i64 2, 1
  ret { i64, i64 } %r7
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
    if (externFns_.count(f)) return;  // C declaration: no A body to emit
    allocaCounter_ = 0;
    pendingAllocas_.clear();
    fn_ = std::ostringstream();
    tmpCounter_ = 0;
    resetGlobalScope(f->moduleId);
    pushScope();
    bindModuleImports(f->moduleId);
    analyzeFnRawSafety(f);
    int savedModule = curModuleId_;
    curModuleId_ = f->moduleId;
    inFunctionBody_ = true;
    // Params: load argv[i+1] into a fresh alloca each. Sema-typed numeric
    // params (Int/Float, no default) are unboxed once into {i64,i1} slots.
    std::vector<std::string> paramSlots;
    std::size_t paramOffset =
        f->paramNames.size() - (f->decl ? f->decl->params.size() : 0);
    for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
      const ParamDecl* pd =
          (f->decl && i >= paramOffset)
              ? &f->decl->params[i - paramOffset]
              : nullptr;
      bool hasDefault = pd && pd->defaultValue;
      TypePtr ptype =
          (!hasDefault && pd) ? numericParamType(f, i - paramOffset)
                              : nullptr;
      if (!ptype && !hasDefault && pd) {
        // Sema-typed vector parameter: unbox the boxed list argument into a
        // native <N x ty> alloca.
        TypePtr vt = vectorParamType(f, i - paramOffset);
        if (vt) {
          VecMeta m;
          if (vecMeta(vt, m)) {
            std::string slot = alloca(m.vecTy);
            std::string g = T(), v = T();
            emit("  " + g + " = getelementptr i64, ptr %argv, i64 " +
                 std::to_string(i + 1));
            emit("  " + v + " = load i64, ptr " + g);
            RVal u = vecUnboxRVal(v, m);
            std::string uv = vecLoad(u);
            vecStore(slot, uv, m.vecTy);
            paramSlots.push_back(slot);
            LlBinding b;
            b.kind = BKind::Param;
            b.slot = slot;
            b.vecMode = 1;
            b.vecTy = m.vecTy;
            b.vecN = m.n;
            b.vecFloat = m.isFloat;
            b.vecLane = m.laneTy;
            scopes_.back()[f->paramNames[i]] = b;
            continue;
          }
        }
      }
      if (ptype) {
        std::string slot = alloca("i64");
        std::string slotF = alloca("i64");
        std::string g = T(), v = T();
        emit("  " + g + " = getelementptr i64, ptr %argv, i64 " +
             std::to_string(i + 1));
        emit("  " + v + " = load i64, ptr " + g);
        std::string un = T(), up = T(), uf = T();
        emit("  " + un + " = call { i64, i64 } @a.unboxnum(i64 " + v + ")");
        emit("  " + up + " = extractvalue { i64, i64 } " + un + ", 0");
        emit("  " + uf + " = extractvalue { i64, i64 } " + un + ", 1");
        emit("  store i64 " + up + ", ptr " + slot);
        emit("  store i64 " + uf + ", ptr " + slotF);
        paramSlots.push_back(slot);
        LlBinding b;
        b.kind = BKind::Param;
        b.slot = slot;
        b.slotF = slotF;
        b.numMode = 1;
        scopes_.back()[f->paramNames[i]] = b;
        continue;
      }
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

  // ---- raw-numeric safety analysis (implementation) ------------------------
  // Mirrors the emitter's scope/binding structure closely enough that an
  // Ident in a write expression resolves to the same binding at analysis and
  // emission time. Only stmt-level constructs that create or shadow bindings
  // are modeled; expr-level scopes (match arms, comprehensions, lambda
  // bodies) can never contain writes and are never entered by rawKind/rawNum,
  // so they are skipped entirely. `++`/`--` writes are neutral: on a numMode
  // binding they always store a kind-1 numArith result whose flag stays in
  // {0,1} whenever the binding is already safe.
  using RawEnv = std::unordered_map<std::string, RawBindInfo*>;

  RawBindInfo* rawNewInfo(RawBindInfo::Cls cls) {
    RawBindInfo& b = rawInfoArena_.emplace_back();
    b.cls = cls;
    return &b;
  }
  RawBindInfo* rawInfoForBinding(const LlBinding& b) {
    switch (b.kind) {
      case BKind::Global:
      case BKind::FnValue:
      case BKind::StructCtor:
      case BKind::EnumValue:
        return &rawGlobalSentinel_;
      case BKind::Module:
      case BKind::Builtin:
      case BKind::NativeConst:
      case BKind::PyFunc:
      case BKind::PyModule:
        return &rawShadowSentinel_;
      default:
        return &rawPlainSentinel_;
    }
  }
  RawBindInfo* rawLookup(const std::string& name) {
    for (auto it = rawFrames_.rbegin(); it != rawFrames_.rend(); ++it) {
      auto f = (*it)->m.find(name);
      if (f != (*it)->m.end()) return f->second;
    }
    return nullptr;
  }
  RawEnv rawMergeEnv() const {
    RawEnv env;
    for (auto it = rawFrames_.rbegin(); it != rawFrames_.rend(); ++it)
      for (const auto& kv : (*it)->m) env.emplace(kv.first, kv.second);
    return env;
  }
  void rawPush() {
    rawFrameArena_.emplace_back();
    rawFrames_.push_back(&rawFrameArena_.back());
  }
  void rawPop() {
    RawFrame* f = rawFrames_.back();
    if (!f->defers.empty()) {
      // Deferred bodies run at scope exit while the exiting frame is still
      // visible; their own bindings are dead afterwards.
      rawPush();
      for (auto it = f->defers.rbegin(); it != f->defers.rend(); ++it)
        if (*it) rawWalkStmt(**it);
      rawPop();
    }
    rawFrames_.pop_back();
  }

  void beginRawAnalysis(bool inFnBody, int moduleId) {
    rawFrameArena_.clear();
    rawInfoArena_.clear();
    rawFrames_.clear();
    rawCands_.clear();
    rawStmtBinds_.clear();
    rawSafeStmts_.clear();
    rawPlainSentinel_ = RawBindInfo();
    rawPlainSentinel_.cls = RawBindInfo::kPlain;
    rawGlobalSentinel_ = RawBindInfo();
    rawGlobalSentinel_.cls = RawBindInfo::kGlobal;
    rawShadowSentinel_ = RawBindInfo();
    rawShadowSentinel_.cls = RawBindInfo::kShadow;
    rawInFnBody_ = inFnBody;
    rawModuleId_ = moduleId;
    // Snapshot the emitter's current scopes (global scope + function frame
    // with module imports) so non-local names resolve identically.
    for (const auto& frame : scopes_) {
      rawFrameArena_.emplace_back();
      RawFrame& rf = rawFrameArena_.back();
      for (const auto& kv : frame) rf.m[kv.first] = rawInfoForBinding(kv.second);
      rawFrames_.push_back(&rf);
    }
  }
  void finishRawAnalysis() {
    // Function/module-level defers run at the end of the body.
    if (!rawFrames_.empty()) {
      RawFrame* top = rawFrames_.back();
      if (!top->defers.empty()) {
        rawPush();
        for (auto it = top->defers.rbegin(); it != top->defers.rend(); ++it)
          if (*it) rawWalkStmt(**it);
        rawPop();
      }
    }
    // Greatest-fixpoint: start optimistic (kSafe) and downgrade only.
    auto rank = [](RawBindInfo::State s) {
      return s == RawBindInfo::kSafe    ? 2
             : s == RawBindInfo::kUnsafe ? 1
                                         : 0;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (RawBindInfo* c : rawCands_) {
        RawBindInfo::State ns = rawClassify(*c);
        if (rank(ns) < rank(c->state)) {
          c->state = ns;
          changed = true;
        }
      }
    }
    for (const auto& pr : rawStmtBinds_)
      rawSafeStmts_[pr.first] = pr.second->state == RawBindInfo::kSafe;
  }
  void analyzeFnRawSafety(FnInfo* f) {
    beginRawAnalysis(true, f->moduleId);
    rawSeedParams(f);
    if (f->decl) rawWalkStmts(f->decl->body);
    finishRawAnalysis();
  }
  void analyzeModRawSafety(int moduleId, const std::vector<StmtPtr>& body) {
    beginRawAnalysis(false, moduleId);
    rawWalkStmts(body);
    finishRawAnalysis();
  }
  bool rawStmtSafe(const Stmt* s) const {
    auto it = rawSafeStmts_.find(s);
    return it != rawSafeStmts_.end() && it->second;
  }

  // Seed parameter bindings exactly like emitFunction's param loop:
  // sema-typed numeric params without defaults become unboxed numMode
  // bindings (always unsafe — @a.unboxnum may yield flag 2); everything
  // else is a plain aval slot.
  void rawSeedParams(FnInfo* f) {
    if (rawFrames_.empty()) return;
    std::size_t poff =
        f->paramNames.size() - (f->decl ? f->decl->params.size() : 0);
    for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
      const ParamDecl* pd =
          (f->decl && i >= poff) ? &f->decl->params[i - poff] : nullptr;
      bool hasDefault = pd && pd->defaultValue;
      TypePtr ptype =
          (!hasDefault && pd) ? numericParamType(f, i - poff) : nullptr;
      if (ptype) {
        RawBindInfo* b = rawNewInfo(RawBindInfo::kCand);
        b->isParam = true;
        b->state = RawBindInfo::kUnsafe;
        rawCands_.push_back(b);
        rawFrames_.back()->m[f->paramNames[i]] = b;
      } else {
        rawFrames_.back()->m[f->paramNames[i]] = &rawPlainSentinel_;
      }
    }
  }

  void rawWalkStmts(const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& s : stmts)
      if (s) rawWalkStmt(*s);
  }
  void rawWalkStmt(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Let: rawWalkLet(static_cast<const LetStmt&>(s)); break;
      case StmtKind::Assign:
        rawWalkAssign(static_cast<const AssignStmt&>(s));
        break;
      case StmtKind::Destructure: {
        const auto& d = static_cast<const DestructureStmt&>(s);
        for (const std::string& nm : d.names)
          if (nm != "_") rawFrames_.back()->m[nm] = &rawPlainSentinel_;
        break;
      }
      case StmtKind::Defer:
        rawFrames_.back()->defers.push_back(
            static_cast<const DeferStmt&>(s).body.get());
        break;
      case StmtKind::If: {
        const auto& x = static_cast<const IfStmt&>(s);
        for (const auto& br : x.branches) {
          rawPush();
          rawWalkStmts(br.body);
          rawPop();
        }
        if (!x.elseBody.empty()) {
          rawPush();
          rawWalkStmts(x.elseBody);
          rawPop();
        }
        break;
      }
      case StmtKind::While:
      case StmtKind::DoWhile:
      case StmtKind::Loop: {
        rawPush();
        if (s.kind == StmtKind::While)
          rawWalkStmts(static_cast<const WhileStmt&>(s).body);
        else if (s.kind == StmtKind::DoWhile)
          rawWalkStmts(static_cast<const DoWhileStmt&>(s).body);
        else
          rawWalkStmts(static_cast<const LoopStmt&>(s).body);
        rawPop();
        break;
      }
      case StmtKind::For: {
        const auto& x = static_cast<const ForStmt&>(s);
        rawPush();
        rawFrames_.back()->m[x.var] = &rawPlainSentinel_;
        rawWalkStmts(x.body);
        rawPop();
        break;
      }
      default:
        break;  // exprs/declarations/imports: no binding writes
    }
  }
  void rawWalkLet(const LetStmt& let) {
    // Module-init lets matching a top-level global store into the global
    // slot, not a local (mirrors emitLet).
    if (!rawInFnBody_) {
      std::string key = std::to_string(rawModuleId_) + ":" + let.name;
      if (topLevelGlobals_.find(key) != topLevelGlobals_.end()) {
        rawFrames_.back()->m[let.name] = &rawGlobalSentinel_;
        return;
      }
    }
    RawBindInfo* b = rawNewInfo(RawBindInfo::kCand);
    b->init = let.init ? let.init.get() : nullptr;
    b->initEnv = rawMergeEnv();
    rawFrames_.back()->m[let.name] = b;
    rawCands_.push_back(b);
    rawStmtBinds_.emplace_back(static_cast<const Stmt*>(&let), b);
  }
  void rawWalkAssign(const AssignStmt& as) {
    if (as.target->kind != ExprKind::Ident) return;
    const std::string& nm =
        static_cast<const IdentExpr*>(as.target.get())->name;
    RawBindInfo* b = rawLookup(nm);
    if (as.op == "=") {
      if (b && b->cls == RawBindInfo::kCand) {
        RawWrite w;
        w.e = as.value ? as.value.get() : nullptr;
        w.env = rawMergeEnv();
        b->writes.push_back(std::move(w));
        return;
      }
      if (b && (b->cls == RawBindInfo::kPlain ||
                b->cls == RawBindInfo::kGlobal))
        return;  // plain local / global value: not a numMode write
      // kShadow (module/builtin/...) or unresolved: the emitter drops the
      // binding and declares a fresh local shadowing it.
      if (!rawInFnBody_) return;  // module level: global store or error
      // Bare `name = v` in a function body declares a new local.
      RawBindInfo* nb = rawNewInfo(RawBindInfo::kCand);
      nb->init = as.value ? as.value.get() : nullptr;
      nb->initEnv = rawMergeEnv();
      rawFrames_.back()->m[nm] = nb;
      rawCands_.push_back(nb);
      rawStmtBinds_.emplace_back(static_cast<const Stmt*>(&as), nb);
      return;
    }
    // Compound assignment: a write only when the target is a candidate.
    if (b && b->cls == RawBindInfo::kCand) {
      RawWrite w;
      w.e = as.value ? as.value.get() : nullptr;
      w.cop = as.binop;
      w.env = rawMergeEnv();
      b->writes.push_back(std::move(w));
    }
  }

  // evalRaw-kind mirror: 1 iff evalRaw(e) yields kind 1 (unboxed numeric),
  // 0 otherwise. Ident resolution uses the frozen per-write env.
  int rawKind(const Expr* e, const RawEnv& env) {
    if (!e) return 0;
    switch (e->kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
        return 1;
      case ExprKind::Ident:
        return rawIdentNum(e, env) ? 1 : 0;
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(*e);
        if (u.op == "++" || u.op == "--") {
          if (u.operand->kind != ExprKind::Ident) return 0;
          return rawIdentNum(u.operand.get(), env) ? 1 : 0;
        }
        if (u.op == "-" || u.op == "~") return rawKind(u.operand.get(), env);
        return 0;  // "not" -> kind 2, "!"/others -> boxed
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(*e);
        if (!rawArithOp(b.binop)) return 0;
        return rawKind(b.lhs.get(), env) && rawKind(b.rhs.get(), env) ? 1 : 0;
      }
      default:
        return 0;
    }
  }
  bool rawIdentNum(const Expr* e, const RawEnv& env) {
    auto it = env.find(static_cast<const IdentExpr*>(e)->name);
    if (it == env.end() || !it->second) return false;
    const RawBindInfo* b = it->second;
    return b->cls == RawBindInfo::kCand && b->state != RawBindInfo::kNotNum;
  }
  // evalRaw-kind-1 AND flag provably in {0,1}: safe to feed raw ops and to
  // store into a numMode binding without ever producing flag==2.
  bool rawNum(const Expr* e, const RawEnv& env) {
    if (!e) return false;
    switch (e->kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
        return true;
      case ExprKind::Ident: {
        auto it = env.find(static_cast<const IdentExpr*>(e)->name);
        if (it == env.end() || !it->second) return false;
        const RawBindInfo* b = it->second;
        return b->cls == RawBindInfo::kCand && b->state == RawBindInfo::kSafe;
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(*e);
        if (u.op == "++" || u.op == "--") {
          if (u.operand->kind != ExprKind::Ident) return false;
          auto it = env.find(
              static_cast<const IdentExpr*>(u.operand.get())->name);
          if (it == env.end() || !it->second) return false;
          const RawBindInfo* b = it->second;
          return b->cls == RawBindInfo::kCand &&
                 b->state == RawBindInfo::kSafe;
        }
        if (u.op == "-" || u.op == "~") return rawNum(u.operand.get(), env);
        return false;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(*e);
        if (!rawArithOp(b.binop)) return false;
        return rawNum(b.lhs.get(), env) && rawNum(b.rhs.get(), env);
      }
      default:
        return false;
    }
  }
  RawBindInfo::State rawClassify(RawBindInfo& b) {
    if (b.isParam) return RawBindInfo::kUnsafe;
    if (rawKind(b.init, b.initEnv) == 0) return RawBindInfo::kNotNum;
    if (!rawNum(b.init, b.initEnv)) return RawBindInfo::kUnsafe;
    for (const RawWrite& w : b.writes) {
      bool ok = w.cop == BinOp::Invalid
                    ? rawNum(w.e, w.env)
                    : (rawArithOp(w.cop) && rawNum(w.e, w.env));
      if (!ok) return RawBindInfo::kUnsafe;
    }
    return RawBindInfo::kSafe;
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
    RVal rv = evalRaw(let.init.get());
    FnInfo* dropFn = dropFnFor(let.init.get());
    // Top-level lets in a module init store into their global slot.
    if (curModuleId_ >= 0 && !inFunctionBody_) {
      auto it = topLevelGlobals_.find(std::to_string(curModuleId_) + ":" + let.name);
      if (it != topLevelGlobals_.end()) {
        emit("  store i64 " + boxRVal(rv) + ", ptr @" + it->second);
        LlBinding b;
        b.kind = BKind::Global;
        b.sym = it->second;
        b.dropFn = dropFn;
        bind(let.name, b);
        recordDrop(b, dropFn);
        return;
      }
    }
    if (rv.kind == 1) {
      // Unboxed numeric local: {i64 payload, i64 tri-state flag} allocas.
      std::string slot = alloca("i64");
      std::string slotF = alloca("i64");
      emit("  store i64 " + rv.v + ", ptr " + slot);
      emit("  store i64 " + rv.f + ", ptr " + slotF);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = slot;
      b.slotF = slotF;
      b.numMode = 1;
      b.rawSafe = rawStmtSafe(&let);
      b.dropFn = dropFn;
      bind(let.name, b);
      recordDrop(b, dropFn);
      return;
    }
    if (rv.kind == 3) {
      // Unboxed SIMD vector local: slot is an alloca of <N x ty>. Copy into
      // a fresh alloca so `let w = v` does not alias v's slot.
      std::string slot = alloca(rv.vt);
      std::string cur = T();
      emit("  " + cur + " = load " + rv.vt + ", ptr " + rv.v);
      vecStore(slot, cur, rv.vt);
      LlBinding b;
      b.kind = BKind::Local;
      b.slot = slot;
      b.vecMode = 1;
      b.vecTy = rv.vt;
      b.vecN = rv.vn;
      b.vecFloat = rv.vf;
      b.vecLane = rv.vf ? (rv.vt.find(" x float>") != std::string::npos
                              ? "float"
                              : "double")
                        : "i64";
      b.dropFn = dropFn;
      bind(let.name, b);
      recordDrop(b, dropFn);
      return;
    }
    std::string v = boxRVal(rv);
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
  // Scope depth at which a module's top-level statements run (emitModInit
  // pushes one body scope over the global frame). Deeper scopes are block
  // bodies (if/while/for/do/loop/match arms) whose bindings are
  // block-scoped, matching sema and the VM.
  int moduleBodyScopeDepth_ = -1;
  bool inModuleBlockScope() const {
    return !inFunctionBody_ && moduleBodyScopeDepth_ >= 0 &&
           (int)scopes_.size() > moduleBodyScopeDepth_;
  }

  void emitAssign(const AssignStmt& as) {
    const Expr* target = as.target.get();
    auto dropOld = [&](LlBinding& b) {
      if (b.dropFn &&
          (b.kind == BKind::Local || b.kind == BKind::Param ||
           b.kind == BKind::Global)) {
        if (DropRec* old = findDropRec(b)) emitDropRec(*old);
      }
    };
    auto updateDrop = [&](LlBinding& b) {
      FnInfo* newDrop = dropFnFor(as.value.get());
      b.dropFn = newDrop;
      if (DropRec* rec = findDropRec(b)) {
        rec->fn = newDrop;
      } else {
        recordDrop(b, newDrop);
      }
    };
    if (as.op == "=") {
      RVal rv = evalRaw(as.value.get());
      if (target->kind == ExprKind::Ident) {
        const std::string& nm =
            static_cast<const IdentExpr*>(target)->name;
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
          // (Python-style). The same holds inside a block nested under a
          // module's top-level statements (if/while/for bodies): sema scopes
          // those bindings to the block, so they are locals too. Only at the
          // module top level itself does `name = v` target the global slot.
          if (inFunctionBody_ || inModuleBlockScope()) {
            if (rv.kind == 1) {
              std::string slot = alloca("i64");
              std::string slotF = alloca("i64");
              emit("  store i64 " + rv.v + ", ptr " + slot);
              emit("  store i64 " + rv.f + ", ptr " + slotF);
              LlBinding nb;
              nb.kind = BKind::Local;
              nb.slot = slot;
              nb.slotF = slotF;
              nb.numMode = 1;
              nb.rawSafe = rawStmtSafe(&as);
              nb.dropFn = dropFnFor(as.value.get());
              bind(nm, nb);
              recordDrop(nb, nb.dropFn);
              return;
            }
            std::string slot = alloca("i64");
            emit("  store i64 " + boxRVal(rv) + ", ptr " + slot);
            LlBinding nb;
            nb.kind = BKind::Local;
            nb.slot = slot;
            nb.dropFn = dropFnFor(as.value.get());
            bind(nm, nb);
            recordDrop(nb, nb.dropFn);
            return;
          }
          if (curModuleId_ >= 0) {
            auto it = topLevelGlobals_.find(
                std::to_string(curModuleId_) + ":" + nm);
            if (it != topLevelGlobals_.end()) {
              emit("  store i64 " + boxRVal(rv) + ", ptr @" + it->second);
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
        dropOld(*b);
        if (isVecBinding(*b)) {
          TypePtr vt = vecExprType(as.value.get());
          if (rv.kind == 3 || vt) {
            // Vector (or vector-typed boxed) value: store unboxed.
            vecStoreBinding(*b, rv);
          } else {
            // Rebinding to a non-vector (Python-style): demote to a plain
            // boxed i64 binding.
            demoteVecBinding(*b, boxRVal(rv));
          }
        } else if (isNumBinding(*b)) {
          if (rv.kind == 1) {
            storeNumBinding(*b, rv);
          } else {
            // Non-numeric (or boxed) value: keep numMode, store aval+flag2.
            storeNumBoxed(*b, boxRVal(rv));
          }
        } else if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
                   b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
          emit("  store i64 " + boxRVal(rv) + ", ptr @" + b->sym);
        } else {
          emit("  store i64 " + boxRVal(rv) + ", ptr " + b->slot);
        }
        updateDrop(*b);
        return;
      }
      std::string v = boxRVal(rv);
      if (target->kind == ExprKind::Index) {
        const IndexExpr& ix = static_cast<const IndexExpr&>(*target);
        if (vecExprType(ix.object.get())) {
          emitVecIndexAssign(ix, as.value.get());
          return;
        }
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
      return;
    }
    // Compound assignment: target op= value.
    if (target->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr*>(target)->name;
      LlBinding* b = lookup(nm);
      if (!b) { fail("undefined variable '" + nm + "'"); return; }
      if (isNumBinding(*b)) {
        RVal cur = loadNumBinding(*b);
        RVal rhs = evalRaw(as.value.get());
        RVal res = numArith(as.binop, cur, rhs);
        if (res.kind == 1) {
          storeNumBinding(*b, res);
        } else {
          std::string bv;
          if (res.kind == 0) {
            bv = res.v;
          } else if (res.kind == -1 || res.kind == 2) {
            // No raw path (In/NotIn/Range or non-fast op): boxed stmt-level
            // semantics, then demote the binding.
            std::string cb = boxRVal(cur), rb2 = boxRVal(rhs);
            if (const char* sym = binopSym(as.binop)) {
              bv = callRt(sym, {cb, rb2});
            } else if (as.binop == BinOp::In) {
              bv = callRt("a_contains", {rb2, cb});
            } else if (as.binop == BinOp::NotIn) {
              bv = callRt("a.not.fast",
                          {callRt("a_contains", {rb2, cb})});
            } else if (as.binop == BinOp::Range) {
              bv = callRt("a_range_new", {cb, rb2, avalIntLit(1)});
            } else {
              fail("unsupported binary operator in assignment");
              return;
            }
          } else {
            bv = boxRVal(res);
          }
          storeNumBoxed(*b, bv);
        }
        updateDrop(*b);
        return;
      }
      if (isVecBinding(*b)) {
        // v op= x on a vector binding: element-wise vector op.
        VecMeta m = bindingVecMeta(*b);
        RVal cur = rvVec(b->slot, b->vecTy, b->vecN, b->vecFloat);
        RVal rhs = evalRaw(as.value.get());
        RVal res = vecArithOp(as.binop, nullptr, cur, as.value.get(), rhs,
                              &m);
        if (res.kind == 3) {
          std::string vv = vecLoad(res);
          VecMeta rm;
          rm.vecTy = res.vt; rm.n = res.vn; rm.isFloat = res.vf;
          rm.isF32 = res.vt.find(" x float>") != std::string::npos;
          rm.laneTy = !res.vf ? "i64" : (rm.isF32 ? "float" : "double");
          vecStore(b->slot, vecCast(vv, rm, m), m.vecTy);
        } else {
          demoteVecBinding(*b, boxRVal(res));
        }
        updateDrop(*b);
        return;
      }
      // Boxed binding: existing path.
      std::string cur = bindingValue(*b);
      std::string rv = eval(as.value.get());
      std::string v = binopValue(as.binop, cur, rv, as.opSpan);
      if (v.empty()) return;
      dropOld(*b);
      if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
          b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
        emit("  store i64 " + v + ", ptr @" + b->sym);
      } else {
        emit("  store i64 " + v + ", ptr " + b->slot);
      }
      updateDrop(*b);
      return;
    }
    std::string cur = eval(target);
    std::string rv = eval(as.value.get());
    std::string v = binopValue(as.binop, cur, rv, as.opSpan);
    if (v.empty()) return;
    if (target->kind == ExprKind::Index) {
      const IndexExpr& ix = static_cast<const IndexExpr&>(*target);
      std::string obj = eval(ix.object.get());
      std::string idx = eval(ix.index.get());
      callRt("a_setitem", {obj, idx, v});
    } else if (target->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*target);
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
      std::string t = condI1(br.cond.get());
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
    std::string t = condI1(s.cond.get());
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
    std::string t = condI1(s.cond.get());
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
  // eval returns an LLVM value name ("%tN") or an integer constant: always a
  // boxed aval (i64). evalRaw is the numeric fast path — see below.
  std::string eval(const Expr* e) { return boxRVal(evalRaw(e)); }
  // Former boxed-only evaluator; subexpression eval() calls still route
  // through evalRaw so nested numerics stay fast up to the boxing boundary.
  std::string evalFallback(const Expr* e) {
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

  // Load the aval value a binding refers to (boxes numMode bindings).
  std::string bindingValue(const LlBinding& b) {
    switch (b.kind) {
      case BKind::Local:
      case BKind::Param:
      case BKind::Capture: {
        if (isVecBinding(b)) {
          return boxRVal(rvVec(b.slot, b.vecTy, b.vecN, b.vecFloat));
        }
        if (isNumBinding(b)) {
          RVal r = loadNumBinding(b);
          return boxRVal(r);
        }
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

  // ---- unboxed numeric values -------------------------------------------------
  // RVal is the raw counterpart of eval(): kind 0 = boxed aval (i64, `v`),
  // kind 1 = unboxed number (`v` = i64 payload, `f` = i1 is-double flag; an
  // int payload is the int64 value, a double payload is the raw double bits),
  // kind 2 = i1 boolean (`v`), kind 3 = unboxed SIMD vector (`v` = alloca name
  // holding `<N x ty>`; `vt` = LLVM vector type, `vn` = lanes, `vf` = float
  // lanes). kind -1 = "unsupported, use the boxed path".
  struct RVal {
    int kind = 0;
    std::string v;
    std::string f;
    bool safe = false;  // kind 1: flag provably never 2 (boxed aval in slot)
    std::string vt;     // kind 3: LLVM vector type "<N x ty>"
    int vn = 0;         // kind 3: lane count
    bool vf = false;    // kind 3: lanes are floating point
  };
  static RVal rvBoxed(const std::string& v) {
    RVal r;
    r.v = v;
    return r;
  }
  static RVal rvNum(const std::string& v, const std::string& f) {
    RVal r;
    r.kind = 1;
    r.v = v;
    r.f = f;
    r.safe = true;
    return r;
  }
  static RVal rvBool(const std::string& v) {
    RVal r;
    r.kind = 2;
    r.v = v;
    return r;
  }
  static RVal rvBad() {
    RVal r;
    r.kind = -1;
    return r;
  }
  RVal rvVec(const std::string& slot, const std::string& vt, int n,
             bool isFloat) {
    RVal r;
    r.kind = 3;
    r.v = slot;
    r.vt = vt;
    r.vn = n;
    r.vf = isFloat;
    return r;
  }

  // ---- SIMD vectors -----------------------------------------------------------
  // `vector[T, N]` values are native LLVM `<N x ty>` vectors inside
  // expressions (RVal kind 3, held in an alloca so they survive branches and
  // get mem2reg-promoted to SSA). At ABI boundaries they box to a plain A
  // list of N avals — the runtime needs no new type.
  struct VecMeta {
    std::string laneTy;  // "i64" | "double" | "float"
    std::string vecTy;   // "<N x laneTy>"
    int n = 0;
    bool isFloat = false;
    bool isF32 = false;
  };
  // Lane LLVM type for a vector element type (Int -> i64, Float -> f64/f32).
  static bool vecLaneInfo(const TypePtr& elem, std::string& laneTy,
                          bool& isFloat, bool& isF32) {
    TypePtr e = resolveType(elem);
    if (!e) return false;
    if (e->kind == TypeKind::Int) {
      laneTy = "i64";
      isFloat = false;
      isF32 = false;
      return true;
    }
    if (e->kind == TypeKind::Float) {
      isFloat = true;
      isF32 = e->bits == 32;
      laneTy = isF32 ? "float" : "double";
      return true;
    }
    return false;
  }
  bool vecMeta(const TypePtr& vecType, VecMeta& m) {
    TypePtr t = resolveType(vecType);
    if (!t || t->kind != TypeKind::Vector || t->length < 1) return false;
    m.n = (int)t->length;
    if (!vecLaneInfo(t->elem, m.laneTy, m.isFloat, m.isF32)) return false;
    m.vecTy = "<" + std::to_string(m.n) + " x " + m.laneTy + ">";
    return true;
  }
  // The vector type of an expression, or nullptr. Reads the type Sema
  // recorded on the AST node (Expr::inferredType).
  TypePtr vecExprType(const Expr* e) {
    if (!e || !e->inferredType) return nullptr;
    TypePtr t = resolveType(e->inferredType);
    if (t && t->kind == TypeKind::Vector) return t;
    return nullptr;
  }
  bool isVecBinding(const LlBinding& b) const {
    return b.vecMode == 1 &&
           (b.kind == BKind::Local || b.kind == BKind::Param ||
            b.kind == BKind::Capture);
  }
  // Sema-typed vector parameter type, nullptr otherwise.
  TypePtr vectorParamType(const FnInfo* f, std::size_t declIdx) {
    if (!f->decl) return nullptr;
    auto it = sema_.fnParamTypes.find(f->decl);
    if (it == sema_.fnParamTypes.end() || declIdx >= it->second.size())
      return nullptr;
    TypePtr t = resolveType(it->second[declIdx]);
    if (t && t->kind == TypeKind::Vector) return t;
    return nullptr;
  }

  // Box any RVal into an aval i64 (emits code for non-trivial kinds).
  std::string boxRVal(const RVal& r) {
    if (r.kind == 0) return r.v;
    if (r.kind == 3) return vecBox(r);
    if (r.kind < 0) {
      fail("internal error: unsupported raw value in boxRVal");
      return kNone;
    }
    if (r.kind == 2) {
      std::string t = T();
      emit("  " + t + " = select i1 " + r.v + ", i64 " + kTrue +
           ", i64 " + kFalse);
      return t;
    }
    return numToAval(r.v, r.f, r.safe);
  }
  // Load a numMode binding as a raw RVal. flag==2 means the payload slot
  // currently holds a boxed aval; consumers must check for it unless the
  // binding was proven raw-safe by the per-function pre-pass.
  RVal loadNumBinding(const LlBinding& b) {
    RVal r;
    r.kind = 1;
    r.v = T();
    emit("  " + r.v + " = load i64, ptr " + b.slot);
    r.f = T();
    emit("  " + r.f + " = load i64, ptr " + b.slotF);
    r.safe = b.rawSafe;
    return r;
  }
  void storeNumBinding(const LlBinding& b, const RVal& r) {
    emit("  store i64 " + r.v + ", ptr " + b.slot);
    emit("  store i64 " + r.f + ", ptr " + b.slotF);
  }
  // Store a boxed aval into a numMode binding (flag = 2).
  void storeNumBoxed(const LlBinding& b, const std::string& v) {
    emit("  store i64 " + v + ", ptr " + b.slot);
    emit("  store i64 2, ptr " + b.slotF);
  }
  // aval for a possibly-boxed-in-slot numeric payload: flag==2 -> payload is
  // already an aval; otherwise boxnum(payload, flag==1). When `safe` the flag
  // is provably never 2, so the boxed branch is skipped entirely.
  std::string numToAval(const std::string& p, const std::string& f,
                        bool safe) {
    if (safe) {
      std::string fi = T(), bn = T();
      emit("  " + fi + " = icmp eq i64 " + f + ", 1");
      emit("  " + bn + " = call i64 @a.boxnum(i64 " + p + ", i1 " + fi + ")");
      return bn;
    }
    std::string isB = T(), lB = newLabel("ntb"), lN = newLabel("ntn"),
                lEnd = newLabel("nte");
    emit("  " + isB + " = icmp eq i64 " + f + ", 2");
    emit("  br i1 " + isB + ", label %" + lB + ", label %" + lN);
    std::string phi = alloca("i64");
    emit("  " + lB + ":");
    emit("  store i64 " + p + ", ptr " + phi);
    emit("  br label %" + lEnd);
    emit("  " + lN + ":");
    std::string fi = T(), bn = T();
    emit("  " + fi + " = icmp eq i64 " + f + ", 1");
    emit("  " + bn + " = call i64 @a.boxnum(i64 " + p + ", i1 " + fi + ")");
    emit("  store i64 " + bn + ", ptr " + phi);
    emit("  br label %" + lEnd);
    emit("  " + lEnd + ":");
    std::string r = T();
    emit("  " + r + " = load i64, ptr " + phi);
    return r;
  }
  bool isNumBinding(const LlBinding& b) const {
    return b.numMode == 1 &&
           (b.kind == BKind::Local || b.kind == BKind::Param ||
            b.kind == BKind::Capture);
  }
  // Sema-resolved numeric parameter type (Int or Float), nullptr otherwise.
  TypePtr numericParamType(const FnInfo* f, std::size_t declIdx) {
    if (!f->decl) return nullptr;
    auto it = sema_.fnParamTypes.find(f->decl);
    if (it == sema_.fnParamTypes.end() || declIdx >= it->second.size())
      return nullptr;
    TypePtr t = resolveType(it->second[declIdx]);
    if (!t) return nullptr;
    if (t->kind == TypeKind::Int || t->kind == TypeKind::Float) return t;
    return nullptr;
  }

  // ---- SIMD vector emission ---------------------------------------------------
  // Value-level helpers: all take/return LLVM SSA value names of the lane or
  // vector type (never avals). RVal kind 3 carries an alloca pointer.

  // Load the <N x ty> value held by a kind-3 RVal's alloca.
  std::string vecLoad(const RVal& r) {
    std::string v = T();
    emit("  " + v + " = load " + r.vt + ", ptr " + r.v);
    return v;
  }
  void vecStore(const std::string& alloca, const std::string& val,
                const std::string& vt) {
    emit("  store " + vt + " " + val + ", ptr " + alloca);
  }
  // <N x ty> splat of a lane-typed value.
  std::string vecSplatValue(const VecMeta& m, const std::string& laneVal) {
    std::string cur = "zeroinitializer";
    for (int i = 0; i < m.n; ++i) {
      std::string t = T();
      emit("  " + t + " = insertelement " + m.vecTy + " " + cur + ", " +
           m.laneTy + " " + laneVal + ", i32 " + std::to_string(i));
      cur = t;
    }
    return cur;
  }
  // aval -> lane-typed value via the @a.vlane.* IR helpers (handles int,
  // double, and boxed-aval payloads inside a.unboxnum).
  std::string avalToLane(const std::string& aval, const VecMeta& m) {
    if (!m.isFloat) {
      std::string r = T();
      emit("  " + r + " = call i64 @a.vlane.i64(i64 " + aval + ")");
      return r;
    }
    std::string r = T();
    emit("  " + r + " = call double @a.vlane.f64(i64 " + aval + ")");
    if (m.isF32) {
      std::string t = T();
      emit("  " + t + " = fptrunc double " + r + " to float");
      return t;
    }
    return r;
  }
  // lane-typed value -> aval (a_i64n for ints, a.mdouble for floats).
  std::string laneToAval(const std::string& lane, const VecMeta& m) {
    if (!m.isFloat) {
      std::string r = T();
      emit("  " + r + " = call i64 @a_i64n(i64 " + lane + ")");
      return r;
    }
    std::string d = lane;
    if (m.isF32) {
      d = T();
      emit("  " + d + " = fpext float " + lane + " to double");
    }
    std::string r = T();
    emit("  " + r + " = call i64 @a.mdouble(double " + d + ")");
    return r;
  }
  // Scalar RVal -> lane-typed value name. Handles raw nums (flag tri-state,
  // including flag==2 boxed-aval payloads), boxed avals, and bools.
  std::string scalarToLane(const RVal& r, const VecMeta& m) {
    if (r.kind == 3) {
      // Defensive: a vector where a scalar was expected — take lane 0.
      std::string v = vecLoad(r);
      std::string t = T();
      emit("  " + t + " = extractelement " + r.vt + " " + v +
           ", i32 0");
      return t;
    }
    if (r.kind == 0) return avalToLane(r.v, m);
    if (r.kind == 2) {
      std::string i = T();
      emit("  " + i + " = select i1 " + r.v + ", i64 1, i64 0");
      if (!m.isFloat) return i;
      std::string d = T();
      emit("  " + d + " = sitofp i64 " + i + " to double");
      if (m.isF32) {
        std::string t = T();
        emit("  " + t + " = fptrunc double " + d + " to float");
        return t;
      }
      return d;
    }
    // kind 1 raw num (or kind -1 boxed fallback already aval): payload+flag.
    if (r.kind == 1) {
      // flag==2 means the payload slot holds a boxed aval.
      std::string isB = T(), lB = newLabel("vbx"), lR = newLabel("vrl"),
                  lE = newLabel("vbe");
      emit("  " + isB + " = icmp eq i64 " + r.f + ", 2");
      emit("  br i1 " + isB + ", label %" + lB + ", label %" + lR);
      std::string phi = alloca(m.laneTy);
      emit("  " + lB + ":");
      {
        std::string lv = avalToLane(r.v, m);
        emit("  store " + m.laneTy + " " + lv + ", ptr " + phi);
        emit("  br label %" + lE);
      }
      emit("  " + lR + ":");
      {
        std::string lv;
        if (!m.isFloat) {
          // int lane: flag==1 -> fptosi(double bits) else payload
          std::string isD = T(), db = T(), di = T(), sel = T();
          emit("  " + isD + " = icmp eq i64 " + r.f + ", 1");
          emit("  " + db + " = bitcast i64 " + r.v + " to double");
          emit("  " + di + " = fptosi double " + db + " to i64");
          emit("  " + sel + " = select i1 " + isD + ", i64 " + di +
               ", i64 " + r.v);
          lv = sel;
        } else {
          std::string d = numAsDouble(r.v, r.f);
          if (m.isF32) {
            lv = T();
            emit("  " + lv + " = fptrunc double " + d + " to float");
          } else {
            lv = d;
          }
        }
        emit("  store " + m.laneTy + " " + lv + ", ptr " + phi);
        emit("  br label %" + lE);
      }
      emit("  " + lE + ":");
      std::string out = T();
      emit("  " + out + " = load " + m.laneTy + ", ptr " + phi);
      return out;
    }
    // Fallback: box whatever we have and treat as aval.
    return avalToLane(boxRVal(r), m);
  }
  // Splat a scalar RVal into a fresh <N x ty> alloca -> kind-3 RVal.
  RVal vecBroadcast(const VecMeta& m, const RVal& scalar) {
    std::string lane = scalarToLane(scalar, m);
    std::string val = vecSplatValue(m, lane);
    std::string slot = alloca(m.vecTy);
    vecStore(slot, val, m.vecTy);
    return rvVec(slot, m.vecTy, m.n, m.isFloat);
  }
  // Cast a <N x ty> value between lane representations (i64 <-> double/float).
  std::string vecCast(const std::string& val, const VecMeta& from,
                      const VecMeta& to) {
    if (from.laneTy == to.laneTy && from.n == to.n) return val;
    if (from.n != to.n) return val;  // sema prevents this; keep value as-is
    std::string r = T();
    if (!from.isFloat && to.isFloat) {
      emit("  " + r + " = sitofp " + from.vecTy + " " + val + " to " +
           to.vecTy);
    } else if (from.isFloat && !to.isFloat) {
      emit("  " + r + " = fptosi " + from.vecTy + " " + val + " to " +
           to.vecTy);
    } else if (from.isF32 && !to.isF32) {
      emit("  " + r + " = fpext " + from.vecTy + " " + val + " to " +
           to.vecTy);
    } else {
      emit("  " + r + " = fptrunc " + from.vecTy + " " + val + " to " +
           to.vecTy);
    }
    return r;
  }
  // Boxed aval (a list of N avals) -> fresh kind-3 RVal.
  RVal vecUnboxRVal(const std::string& aval, const VecMeta& m) {
    std::string slot = alloca(m.vecTy);
    std::string cur = "zeroinitializer";
    for (int i = 0; i < m.n; ++i) {
      std::string iv = avalIntLit(i);
      std::string el = callRt("a_getitem", {aval, iv});
      std::string lane = avalToLane(el, m);
      std::string t = T();
      emit("  " + t + " = insertelement " + m.vecTy + " " + cur + ", " +
           m.laneTy + " " + lane + ", i32 " + std::to_string(i));
      cur = t;
    }
    vecStore(slot, cur, m.vecTy);
    return rvVec(slot, m.vecTy, m.n, m.isFloat);
  }
  // kind-3 RVal -> aval: build a list of N boxed lanes.
  std::string vecBox(const RVal& r) {
    VecMeta m;
    m.vecTy = r.vt;
    m.n = r.vn;
    m.isFloat = r.vf;
    m.isF32 = false;  // f32 lanes are widened to f64 avals anyway
    // Recover lane type: vf + vt string contains "float" vs "double".
    m.laneTy = r.vt.find("float") != std::string::npos ? "float" : "";
    if (!r.vf) m.laneTy = "i64";
    else m.laneTy = (r.vt.find(" x float>") != std::string::npos) ? "float"
                                                                 : "double";
    m.isF32 = m.laneTy == "float";
    std::string v = vecLoad(r);
    std::string av = argvArray(r.vn);
    for (int i = 0; i < r.vn; ++i) {
      std::string t = T();
      emit("  " + t + " = extractelement " + r.vt + " " + v + ", i32 " +
           std::to_string(i));
      argvStore(av, i, laneToAval(t, m));
    }
    return callRtA("a_list_new", "ip",
                   {std::to_string(r.vn), av});
  }
  // Element access bounds check shared by v[i] reads/writes: idx is i64,
  // negative wraps by +N; out-of-range panics like list indexing.
  std::string vecIndexI32(const std::string& idxI64, int n,
                          const std::string& /*what*/) {
    std::string neg = T(), adj = T(), sel = T(), k = T();
    emit("  " + neg + " = icmp slt i64 " + idxI64 + ", 0");
    emit("  " + adj + " = add i64 " + idxI64 + ", " + std::to_string(n));
    emit("  " + sel + " = select i1 " + neg + ", i64 " + adj + ", i64 " +
         idxI64);
    // Unsigned compare catches negatives (huge) and >= n.
    std::string ok = T(), lOk = newLabel("vidx"), lBad = newLabel("vidxb");
    emit("  " + ok + " = icmp ult i64 " + sel + ", " + std::to_string(n));
    emit("  br i1 " + ok + ", label %" + lOk + ", label %" + lBad);
    emit("  " + lBad + ":");
    {
      std::string msg = cstrGlobal("vector index out of bounds", true);
      emit("  call i64 @a_panic_msg(ptr " + msg + ")");
      emit("  unreachable");
    }
    emit("  " + lOk + ":");
    emit("  " + k + " = trunc i64 " + sel + " to i32");
    return k;
  }
  // i64 index value from a scalar RVal (kind 1 num / kind 0 aval / kind 2).
  std::string indexToI64(const RVal& r) {
    VecMeta im;
    im.laneTy = "i64";
    im.isFloat = false;
    im.isF32 = false;
    im.n = 1;
    im.vecTy = "<1 x i64>";
    return scalarToLane(r, im);
  }
  // Horizontal reduction over a <N x ty> value: op 0 = add, 1 = min, 2 = max.
  // Uses a shufflevector halving tree for power-of-two N, scalar chain else.
  std::string vecReduce(int op, const std::string& vecVal,
                        const std::string& vecTy, const VecMeta& m) {
    int n = m.n;
    std::string cur = vecVal;
    std::string curTy = vecTy;
    if ((n & (n - 1)) == 0 && n > 1) {
      int width = n;
      while (width > 1) {
        int half = width / 2;
        std::string loMask = "<" + std::to_string(half) + " x i32> <";
        std::string hiMask = "<" + std::to_string(half) + " x i32> <";
        for (int i = 0; i < half; ++i) {
          if (i) { loMask += ", "; hiMask += ", "; }
          loMask += "i32 " + std::to_string(2 * i);
          hiMask += "i32 " + std::to_string(2 * i + 1);
        }
        loMask += ">";
        hiMask += ">";
        std::string lo = T(), hi = T();
        emit("  " + lo + " = shufflevector " + curTy + " " + cur + ", " +
             curTy + " poison, " + loMask);
        emit("  " + hi + " = shufflevector " + curTy + " " + cur + ", " +
             curTy + " poison, " + hiMask);
        std::string nextTy = "<" + std::to_string(half) + " x " + m.laneTy +
                             ">";
        std::string t;
        if (op == 0) {
          t = T();
          emit("  " + t + " = " + (m.isFloat ? "fadd" : "add") + " " +
               nextTy + " " + lo + ", " + hi);
        } else {
          // min/max via compare + select (works for int and float lanes).
          std::string cmp = T();
          std::string pred = m.isFloat ? (op == 1 ? "olt" : "ogt")
                                       : (op == 1 ? "slt" : "sgt");
          std::string cc = m.isFloat ? "fcmp" : "icmp";
          emit("  " + cmp + " = " + cc + " " + pred + " " + nextTy + " " + lo +
               ", " + hi);
          t = vecReduceSelect(lo, hi, cmp, nextTy, half);
        }
        cur = t;
        curTy = nextTy;
        width = half;
      }
      std::string t = T();
      emit("  " + t + " = extractelement " + curTy + " " + cur + ", i32 0");
      return t;
    }
    // Non-power-of-two: extract + scalar chain.
    std::string acc;
    for (int i = 0; i < n; ++i) {
      std::string t = T();
      emit("  " + t + " = extractelement " + vecTy + " " + vecVal +
           ", i32 " + std::to_string(i));
      if (i == 0) {
        acc = t;
        continue;
      }
      std::string r = T();
      if (op == 0) {
        emit("  " + r + " = " + (m.isFloat ? "fadd" : "add") + " " +
             m.laneTy + " " + acc + ", " + t);
      } else if (op == 1) {
        std::string c = T(), s = T();
        emit("  " + c + " = " + (m.isFloat ? "fcmp olt" : "icmp slt") +
             " " + m.laneTy + " " + t + ", " + acc);
        emit("  " + s + " = select i1 " + c + ", " + m.laneTy + " " + t +
             ", " + m.laneTy + " " + acc);
        r = s;
      } else {
        std::string c = T(), s = T();
        emit("  " + c + " = " + (m.isFloat ? "fcmp ogt" : "icmp sgt") +
             " " + m.laneTy + " " + t + ", " + acc);
        emit("  " + s + " = select i1 " + c + ", " + m.laneTy + " " + t +
             ", " + m.laneTy + " " + acc);
        r = s;
      }
      acc = r;
    }
    return acc;
  }
  // select for vector min/max: <N x i1> cond picks lo else hi.
  std::string vecReduceSelect(const std::string& lo, const std::string& hi,
                              const std::string& cmp,
                              const std::string& nextTy, int lanes) {
    std::string selTy = "<" + std::to_string(lanes) + " x i1>";
    std::string t = T();
    emit("  " + t + " = select " + selTy + " " + cmp + ", " + nextTy + " " +
         lo + ", " + nextTy + " " + hi);
    return t;
  }
  // Binary arithmetic on two kind-3-ish operands -> kind-3 RVal.
  // lexpr/rexpr are the AST operands (for vecExprType); l/r their RVals.
  // Exactly one of (expr vector-typed) determines broadcast vs elementwise.
  RVal vecArithOp(BinOp op, const Expr* lexpr, const RVal& l,
                  const Expr* rexpr, const RVal& r,
                  const VecMeta* outOverride = nullptr) {
    VecMeta out;
    if (outOverride) {
      out = *outOverride;
    } else {
      TypePtr outT = vecExprType(lexpr) ? vecExprType(lexpr)
                                        : vecExprType(rexpr);
      if (!outT || !vecMeta(outT, out)) return rvBad();
    }
    // Load/cast each operand to the output vector type. A kind-3 RVal is a
    // vector operand (element-wise); anything else is a broadcast scalar.
    auto operandVec = [&](const Expr* /*e*/, const RVal& rv,
                          const VecMeta& /*unused*/) -> std::string {
      if (rv.kind == 3) {
        VecMeta rm;
        rm.vecTy = rv.vt;
        rm.n = rv.vn;
        rm.isFloat = rv.vf;
        rm.isF32 = rv.vt.find(" x float>") != std::string::npos;
        rm.laneTy = !rv.vf ? "i64" : (rm.isF32 ? "float" : "double");
        return vecCast(vecLoad(rv), rm, out);
      }
      // Scalar -> splat.
      std::string lane = scalarToLane(rv, out);
      return vecSplatValue(out, lane);
    };
    std::string lv = operandVec(lexpr, l, out);
    std::string rv = operandVec(rexpr, r, out);
    std::string res;
    bool fl = out.isFloat;
    switch (op) {
      case BinOp::Add: res = fl ? "fadd" : "add"; break;
      case BinOp::Sub: res = fl ? "fsub" : "sub"; break;
      case BinOp::Mul: res = fl ? "fmul" : "mul"; break;
      case BinOp::Div: res = fl ? "fdiv" : "sdiv"; break;
      case BinOp::FloorDiv: res = fl ? "fdiv" : "sdiv"; break;
      case BinOp::Mod: res = fl ? "" : "srem"; break;
      default: return rvBad();
    }
    std::string t = T();
    if (!res.empty()) {
      emit("  " + t + " = " + res + " " + out.vecTy + " " + lv + ", " + rv);
    } else {
      // float % : per-lane fmod(double, double).
      int n = out.n;
      std::string slot = alloca(out.vecTy);
      for (int i = 0; i < n; ++i) {
        std::string a = T(), b = T();
        emit("  " + a + " = extractelement " + out.vecTy + " " + lv +
             ", i32 " + std::to_string(i));
        emit("  " + b + " = extractelement " + out.vecTy + " " + rv +
             ", i32 " + std::to_string(i));
        std::string da = a, db = b;
        if (out.isF32) {
          da = T(); db = T();
          emit("  " + da + " = fpext float " + a + " to double");
          emit("  " + db + " = fpext float " + b + " to double");
        }
        std::string fm = T();
        emit("  " + fm + " = call double @fmod(double " + da + ", double " +
             db + ")");
        std::string lane = fm;
        if (out.isF32) {
          lane = T();
          emit("  " + lane + " = fptrunc double " + fm + " to float");
        }
        if (i == 0) {
          std::string z = T();
          emit("  " + z + " = insertelement " + out.vecTy +
               " zeroinitializer, " + out.laneTy + " " + lane +
               ", i32 0");
          vecStore(slot, z, out.vecTy);
        } else {
          std::string prev = T(), cur = T();
          emit("  " + prev + " = load " + out.vecTy + ", ptr " + slot);
          emit("  " + cur + " = insertelement " + out.vecTy + " " + prev +
               ", " + out.laneTy + " " + lane + ", i32 " +
               std::to_string(i));
          vecStore(slot, cur, out.vecTy);
        }
      }
      std::string ld = T();
      emit("  " + ld + " = load " + out.vecTy + ", ptr " + slot);
      return rvVec(slot, out.vecTy, out.n, out.isFloat);
    }
    std::string slot = alloca(out.vecTy);
    vecStore(slot, t, out.vecTy);
    return rvVec(slot, out.vecTy, out.n, out.isFloat);
  }
  // evalRawVecBinary: element-wise vector op, or scalar-binary broadcast.
  RVal evalRawVecBinary(const BinaryExpr& x) {
    TypePtr outT = vecExprType(&x);
    bool lVec = vecExprType(x.lhs.get()) != nullptr;
    bool rVec = vecExprType(x.rhs.get()) != nullptr;
    if (!lVec && !rVec) {
      // Scalar arithmetic coerced to a vector (splat broadcast).
      RVal s = evalRawNoVec(&x);
      VecMeta m;
      if (!vecMeta(outT, m)) return rvBad();
      if (s.kind == 1 || s.kind == 2) return vecBroadcast(m, s);
      if (s.kind == 0) return vecUnboxRVal(s.v, m);
      return rvBad();
    }
    RVal l = evalRaw(x.lhs.get());
    RVal r = evalRaw(x.rhs.get());
    // Scalar side may arrive boxed (kind 0) — vecArithOp handles via
    // scalarToLane. kind 3 only when the operand is itself vector-typed.
    return vecArithOp(x.binop, x.lhs.get(), l, x.rhs.get(), r);
  }
  RVal evalRawVecUnary(const UnaryExpr& x, const Expr* e) {
    if (x.op == "-" && vecExprType(x.operand.get())) {
      RVal v = evalRaw(x.operand.get());
      if (v.kind != 3) return rvBoxed(evalFallback(e));
      VecMeta m;
      vecMeta(vecExprType(e), m);
      std::string cur = vecLoad(v);
      // Cast operand to result meta if needed.
      VecMeta om;
      om.vecTy = v.vt; om.n = v.vn; om.isFloat = v.vf;
      om.isF32 = v.vt.find(" x float>") != std::string::npos;
      om.laneTy = !v.vf ? "i64" : (om.isF32 ? "float" : "double");
      cur = vecCast(cur, om, m);
      std::string t = T();
      if (m.isFloat) {
        emit("  " + t + " = fneg " + m.vecTy + " " + cur);
      } else {
        emit("  " + t + " = sub " + m.vecTy + " zeroinitializer, " + cur);
      }
      std::string slot = alloca(m.vecTy);
      vecStore(slot, t, m.vecTy);
      return rvVec(slot, m.vecTy, m.n, m.isFloat);
    }
    // Scalar unary coerced to vector (e.g. `-x` splat): broadcast.
    RVal s = evalRawNoVec(e);
    VecMeta m;
    vecMeta(vecExprType(e), m);
    if (s.kind == 1 || s.kind == 2) return vecBroadcast(m, s);
    if (s.kind == 0) return vecUnboxRVal(s.v, m);
    return rvBad();
  }
  RVal evalRawVecIdent(const IdentExpr& x) {
    LlBinding* b = lookup(x.name);
    VecMeta m;
    vecMeta(vecExprType(&x), m);
    if (b && isVecBinding(*b)) {
      // Hand out the binding's own alloca so v[i] = x mutates in place.
      return rvVec(b->slot, b->vecTy, b->vecN, b->vecFloat);
    }
    if (b && isNumBinding(*b)) {
      RVal n = loadNumBinding(*b);
      return vecBroadcast(m, n);
    }
    // Globals, boxed locals, module values: boxed list -> unbox.
    std::string av = evalIdent(x);
    return vecUnboxRVal(av, m);
  }
  // VecMeta view of a vecMode binding.
  VecMeta bindingVecMeta(const LlBinding& b) {
    VecMeta m;
    m.vecTy = b.vecTy;
    m.n = b.vecN;
    m.isFloat = b.vecFloat;
    m.isF32 = b.vecLane == "float";
    m.laneTy = b.vecLane.empty() ? (b.vecFloat ? "double" : "i64")
                                 : b.vecLane;
    return m;
  }
  // Store an RVal into a vecMode binding's <N x ty> alloca. Accepts kind-3
  // vectors (cast if lane types differ), kind-1/2 scalars (broadcast), and
  // kind-0 boxed avals (unbox as a list of lanes).
  void vecStoreBinding(LlBinding& b, const RVal& rv) {
    VecMeta m = bindingVecMeta(b);
    if (rv.kind == 3) {
      std::string v = vecLoad(rv);
      VecMeta rm;
      rm.vecTy = rv.vt;
      rm.n = rv.vn;
      rm.isFloat = rv.vf;
      rm.isF32 = rv.vt.find(" x float>") != std::string::npos;
      rm.laneTy = !rv.vf ? "i64" : (rm.isF32 ? "float" : "double");
      v = vecCast(v, rm, m);
      vecStore(b.slot, v, m.vecTy);
      return;
    }
    if (rv.kind == 1 || rv.kind == 2) {
      // Scalar broadcast (annotated `v = 2.0`-style coercions reach here
      // only when sema kept the vector type).
      RVal splat = vecBroadcast(m, rv);
      std::string v = vecLoad(splat);
      vecStore(b.slot, v, m.vecTy);
      return;
    }
    if (rv.kind == 0) {
      // Boxed aval: a list of N lanes -> unbox into the slot.
      RVal u = vecUnboxRVal(rv.v, m);
      std::string v = vecLoad(u);
      vecStore(b.slot, v, m.vecTy);
      return;
    }
    fail("internal error: cannot store raw value into vector binding");
  }
  // Rebind a vecMode binding to a plain boxed i64 value (Python-style type
  // change). The old <N x ty> alloca is simply abandoned.
  void demoteVecBinding(LlBinding& b, const std::string& aval) {
    std::string slot = alloca("i64");
    emit("  store i64 " + aval + ", ptr " + slot);
    b.vecMode = 0;
    b.vecTy.clear();
    b.vecN = 0;
    b.vecFloat = false;
    b.vecLane.clear();
    b.slot = slot;
    b.slotF.clear();
    b.numMode = 0;
    b.rawSafe = false;
  }
  // evalRawVec: produce a kind-3 RVal for an expression Sema typed as
  // vector[T, N].
  RVal evalRawVec(const Expr* e) {
    VecMeta m;
    vecMeta(vecExprType(e), m);
    switch (e->kind) {
      case ExprKind::ArrayLit:
        return vecFromList(*static_cast<const ArrayLitExpr*>(e), m);
      case ExprKind::Ident:
        return evalRawVecIdent(*static_cast<const IdentExpr*>(e));
      case ExprKind::Unary:
        return evalRawVecUnary(*static_cast<const UnaryExpr*>(e), e);
      case ExprKind::Binary:
        return evalRawVecBinary(*static_cast<const BinaryExpr*>(e));
      default:
        break;
    }
    // Everything else (calls returning vectors, ternaries, index-into-map
    // of vectors, coerced scalars): boxed aval -> unbox, or splat.
    RVal r = evalRawNoVec(e);
    if (r.kind == 1 || r.kind == 2) return vecBroadcast(m, r);
    if (r.kind == 0) return vecUnboxRVal(r.v, m);
    return rvBad();
  }
  // [e0, ..., eN-1] literal coerced to vector[T, N]: insertelement chain.
  RVal vecFromList(const ArrayLitExpr& x, const VecMeta& m) {
    std::string slot = alloca(m.vecTy);
    std::string cur = "zeroinitializer";
    for (int i = 0; i < m.n; ++i) {
      RVal er = evalRaw(x.elements[static_cast<std::size_t>(i)].get());
      std::string lane = scalarToLane(er, m);
      std::string t = T();
      emit("  " + t + " = insertelement " + m.vecTy + " " + cur + ", " +
           m.laneTy + " " + lane + ", i32 " + std::to_string(i));
      cur = t;
    }
    vecStore(slot, cur, m.vecTy);
    return rvVec(slot, m.vecTy, m.n, m.isFloat);
  }
  // v[i] read on a vector-typed object -> scalar num RVal (kind 1).
  RVal evalRawVecIndex(const IndexExpr& x) {
    TypePtr ot = vecExprType(x.object.get());
    VecMeta m;
    vecMeta(ot, m);
    RVal obj = evalRaw(x.object.get());
    if (obj.kind != 3) return rvBoxed(evalFallback(&x));
    std::string vv = vecLoad(obj);
    RVal idx = evalRaw(x.index.get());
    std::string i64 = indexToI64(idx);
    std::string i32 = vecIndexI32(i64, m.n, "index");
    std::string lane = T();
    emit("  " + lane + " = extractelement " + m.vecTy + " " + vv +
         ", i32 " + i32);
    // lane value -> raw num RVal: int stays i64; float bitcasts to i64 bits.
    if (!m.isFloat) return rvNum(lane, "0");
    std::string d = lane;
    if (m.isF32) {
      d = T();
      emit("  " + d + " = fpext float " + lane + " to double");
    }
    std::string bits = T();
    emit("  " + bits + " = bitcast double " + d + " to i64");
    return rvNum(bits, "1");
  }
  // v[i] = x on a vector-typed object: insertelement into the vector alloca.
  // Only vecMode bindings own an unboxed slot; every other vector value is a
  // boxed list, where a_setitem mutates the shared list object in place.
  void emitVecIndexAssign(const IndexExpr& ix, const Expr* value) {
    LlBinding* vb = nullptr;
    if (ix.object->kind == ExprKind::Ident) {
      vb = lookup(static_cast<const IdentExpr*>(ix.object.get())->name);
    }
    if (!vb || !isVecBinding(*vb)) {
      // Boxed path: evalFallback yields the stored aval (the shared list
      // object) without the unbox/rebox roundtrip evalRaw would do.
      std::string o = evalFallback(ix.object.get());
      std::string iv = eval(ix.index.get());
      std::string vv = eval(value);
      callRt("a_setitem", {o, iv, vv});
      return;
    }
    VecMeta m = bindingVecMeta(*vb);
    RVal obj = rvVec(vb->slot, vb->vecTy, vb->vecN, vb->vecFloat);
    std::string vv = vecLoad(obj);
    RVal idx = evalRaw(ix.index.get());
    std::string i64 = indexToI64(idx);
    std::string i32 = vecIndexI32(i64, m.n, "index");
    // Element type meta (single lane) for the stored value.
    VecMeta em = m;
    em.vecTy = "<1 x " + m.laneTy + ">";
    RVal vr = evalRaw(value);
    std::string lane = scalarToLane(vr, em);
    std::string t = T();
    emit("  " + t + " = insertelement " + m.vecTy + " " + vv + ", " +
         m.laneTy + " " + lane + ", i32 " + i32);
    vecStore(obj.v, t, m.vecTy);
  }
  // Vector intrinsic methods: sum/min/max/dot/to_list. Returns kind-1 num
  // RVal for reductions, kind-0 boxed for to_list; handled=false otherwise.
  RVal evalVecIntrinsic(const Expr* objExpr, const std::string& field,
                        const CallExpr& call, bool& handled) {
    handled = false;
    TypePtr ot = vecExprType(objExpr);
    if (!ot) return rvBad();
    VecMeta m;
    if (!vecMeta(ot, m)) return rvBad();
    if (field == "to_list") {
      handled = true;
      RVal obj = evalRaw(objExpr);
      return rvBoxed(vecBox(obj));
    }
    int redOp = -1;
    if (field == "sum") redOp = 0;
    else if (field == "min") redOp = 1;
    else if (field == "max") redOp = 2;
    else if (field == "dot") {
      if (call.args.size() != 1) return rvBad();
      handled = true;
      RVal l = evalRaw(objExpr);
      RVal r = evalRaw(call.args[0].value.get());
      // product vector -> horizontal add (manual: mul then reduce).
      TypePtr outT = ot;
      VecMeta out;
      vecMeta(outT, out);
      auto operandVec = [&](const Expr* /*e*/, const RVal& rv,
                            const VecMeta& /*u*/) -> std::string {
        if (rv.kind == 3) {
          VecMeta rm;
          rm.vecTy = rv.vt; rm.n = rv.vn; rm.isFloat = rv.vf;
          rm.isF32 = rv.vt.find(" x float>") != std::string::npos;
          rm.laneTy = !rv.vf ? "i64" : (rm.isF32 ? "float" : "double");
          return vecCast(vecLoad(rv), rm, out);
        }
        return vecSplatValue(out, scalarToLane(rv, out));
      };
      std::string lv = operandVec(objExpr, l, out);
      std::string rv2 = operandVec(call.args[0].value.get(), r, out);
      std::string t = T();
      emit("  " + t + " = " + (out.isFloat ? "fmul" : "mul") + " " +
           out.vecTy + " " + lv + ", " + rv2);
      std::string lane = vecReduce(0, t, out.vecTy, out);
      if (!out.isFloat) return rvNum(lane, "0");
      std::string d = lane;
      if (out.isF32) {
        d = T();
        emit("  " + d + " = fpext float " + lane + " to double");
      }
      std::string bits = T();
      emit("  " + bits + " = bitcast double " + d + " to i64");
      return rvNum(bits, "1");
    }
    if (redOp < 0) return rvBad();
    handled = true;
    RVal obj = evalRaw(objExpr);
    if (obj.kind != 3) return rvBad();
    std::string vv = vecLoad(obj);
    VecMeta rm;
    rm.vecTy = obj.vt; rm.n = obj.vn; rm.isFloat = obj.vf;
    rm.isF32 = obj.vt.find(" x float>") != std::string::npos;
    rm.laneTy = !obj.vf ? "i64" : (rm.isF32 ? "float" : "double");
    std::string lane = vecReduce(redOp, vv, obj.vt, rm);
    if (!rm.isFloat) return rvNum(lane, "0");
    std::string d = lane;
    if (rm.isF32) {
      d = T();
      emit("  " + d + " = fpext float " + lane + " to double");
    }
    std::string bits = T();
    emit("  " + bits + " = bitcast double " + d + " to i64");
    return rvNum(bits, "1");
  }
  // evalRaw entry for Call exprs: vector intrinsic methods stay unboxed.
  RVal evalRawCall(const CallExpr& x, const Expr* e) {
    if (auto* f = dynamic_cast<FieldExpr*>(x.callee.get())) {
      bool handled = false;
      RVal r = evalVecIntrinsic(f->object.get(), f->field, x, handled);
      if (handled) return r;
    }
    return rvBoxed(evalFallback(e));
  }

  // evalRaw: boxed eval() plus a raw numeric fast path for literals,
  // identifiers bound to numMode slots, unary/binary numeric ops.
  RVal evalRaw(const Expr* e) {
    if (!e) return rvBoxed(kNone);
    if (TypePtr vt = vecExprType(e)) {
      (void)vt;
      return evalRawVec(e);
    }
    return evalRawNoVec(e);
  }
  RVal evalRawNoVec(const Expr* e) {
    if (!e) return rvBoxed(kNone);
    switch (e->kind) {
      case ExprKind::IntLit:
        return rvNum(std::to_string(
                         static_cast<const IntLitExpr*>(e)->value),
                     "0");
      case ExprKind::FloatLit:
        return rvNum(avalDoubleLit(static_cast<const FloatLitExpr*>(e)->value),
                     "1");
      case ExprKind::BoolLit:
        return rvBool(static_cast<const BoolLitExpr*>(e)->value ? "true"
                                                                : "false");
      case ExprKind::Ident:
        return evalRawIdent(*static_cast<const IdentExpr*>(e));
      case ExprKind::Unary:
        return evalRawUnary(*static_cast<const UnaryExpr*>(e), e);
      case ExprKind::Binary:
        return evalRawBinary(*static_cast<const BinaryExpr*>(e), e);
      case ExprKind::Index: {
        const auto& x = *static_cast<const IndexExpr*>(e);
        if (vecExprType(x.object.get())) return evalRawVecIndex(x);
        break;
      }
      case ExprKind::Call:
        return evalRawCall(*static_cast<const CallExpr*>(e), e);
      default:
        break;
    }
    return rvBoxed(evalFallback(e));
  }

  RVal evalRawIdent(const IdentExpr& x) {
    LlBinding* b = lookup(x.name);
    if (b && isNumBinding(*b)) return loadNumBinding(*b);
    return rvBoxed(evalIdent(x));
  }

  RVal evalRawUnary(const UnaryExpr& x, const Expr* e) {
    if (x.op == "++" || x.op == "--") {
      if (x.operand->kind != ExprKind::Ident) return rvBoxed(evalFallback(e));
      const std::string& nm =
          static_cast<const IdentExpr*>(x.operand.get())->name;
      LlBinding* b = lookup(nm);
      if (!b || !isNumBinding(*b)) return rvBoxed(evalFallback(e));
      RVal cur = loadNumBinding(*b);
      RVal one = rvNum("1", "0");
      RVal res = numArith(x.op == "++" ? BinOp::Add : BinOp::Sub, cur, one);
      if (res.kind == 1) {
        storeNumBinding(*b, res);
        return x.postfix ? cur : res;
      }
      // Defensive: boxed results (flag==2 aval) are stored back raw-safe.
      std::string bv = res.kind == 0 ? res.v : boxRVal(res);
      storeNumBoxed(*b, bv);
      return x.postfix ? rvBoxed(boxRVal(cur)) : rvBoxed(bv);
    }
    if (x.op == "-" || x.op == "~" || x.op == "not") {
      RVal v = evalRaw(x.operand.get());
      if (v.kind != 1) return rvBoxed(evalFallback(e));
      if (x.op == "-") return numNeg(v);
      if (x.op == "~") return numBNot(v);
      return numNot(v);
    }
    return rvBoxed(evalFallback(e));
  }

  RVal evalRawBinary(const BinaryExpr& x, const Expr* e) {
    switch (x.binop) {
      case BinOp::Add:
      case BinOp::Sub:
      case BinOp::Mul:
      case BinOp::Div:
      case BinOp::FloorDiv:
      case BinOp::Mod:
      case BinOp::BitAnd:
      case BinOp::BitOr:
      case BinOp::BitXor:
      case BinOp::Shl:
      case BinOp::Shr:
        break;
      case BinOp::Eq:
      case BinOp::Ne:
      case BinOp::Lt:
      case BinOp::Le:
      case BinOp::Gt:
      case BinOp::Ge: {
        RVal l = evalRaw(x.lhs.get());
        RVal r = evalRaw(x.rhs.get());
        return numCmp(x.binop, l, r);
      }
      default:
        return rvBoxed(evalFallback(e));
    }
    RVal l = evalRaw(x.lhs.get());
    RVal r = evalRaw(x.rhs.get());
    return numArith(x.binop, l, r);
  }

  // ---- raw numeric ops ---------------------------------------------------------
  // Phi-alloca pair used to merge {payload, flag} across branches.
  struct NumPhi {
    std::string p, f;
  };
  // flag is i64: 0 = int payload, 1 = double bits, 2 = payload holds a boxed
  // aval (set when a numMode binding is assigned a non-raw value; keeps the
  // binding loop-invariant so emitted loop bodies stay correct).
  NumPhi numPhi() { return {alloca("i64"), alloca("i64")}; }
  void numPhiStore(const NumPhi& ph, const std::string& p,
                   const std::string& f) {
    emit("  store i64 " + p + ", ptr " + ph.p);
    emit("  store i64 " + f + ", ptr " + ph.f);
  }
  RVal numPhiLoad(const NumPhi& ph) {
    RVal r;
    r.kind = 1;
    r.v = T();
    emit("  " + r.v + " = load i64, ptr " + ph.p);
    r.f = T();
    emit("  " + r.f + " = load i64, ptr " + ph.f);
    return r;
  }
  // i1 phi for boolean merges.
  std::string boolPhi() { return alloca("i1"); }
  void boolPhiStore(const std::string& ph, const std::string& c) {
    emit("  store i1 " + c + ", ptr " + ph);
  }
  std::string boolPhiLoad(const std::string& ph) {
    std::string r = T();
    emit("  " + r + " = load i1, ptr " + ph);
    return r;
  }

  // "both operands are ints" predicate (i1); flag is i64 tri-state.
  std::string bothIntPred(const std::string& lf, const std::string& rf) {
    std::string a = T(), b = T(), r = T();
    emit("  " + a + " = icmp eq i64 " + lf + ", 0");
    emit("  " + b + " = icmp eq i64 " + rf + ", 0");
    emit("  " + r + " = and i1 " + a + ", " + b);
    return r;
  }
  // i1 "payload is double bits" for a tri-state flag (flag==1).
  std::string flagIsDouble(const std::string& f) {
    std::string r = T();
    emit("  " + r + " = icmp eq i64 " + f + ", 1");
    return r;
  }
  // Numeric value of a raw num as double: flag==1 ? bitcast(payload) :
  // sitofp(payload). Only valid when flag != 2 (guarded by callers).
  std::string numAsDouble(const std::string& p, const std::string& f) {
    std::string pb = T(), di = T(), fi = T(), d = T();
    emit("  " + pb + " = bitcast i64 " + p + " to double");
    emit("  " + di + " = sitofp i64 " + p + " to double");
    emit("  " + fi + " = icmp eq i64 " + f + ", 1");
    emit("  " + d + " = select i1 " + fi + ", double " + pb + ", double " + di);
    return d;
  }
  // Boxed fallback for a binary op: box both operands, call the @a.*.fast
  // helper (or the boxed stmt-level path when there is no helper).
  RVal boxedBinop(BinOp op, const RVal& l, const RVal& r) {
    if (const char* sym = binopSym(op))
      return rvBoxed(callRt(sym, {boxRVal(l), boxRVal(r)}));
    return rvBad();
  }
  // aval -> Bool RVal (comparison helpers always return A_TRUE/A_FALSE).
  RVal avalToBool(const std::string& aval) {
    std::string c = T();
    emit("  " + c + " = icmp eq i64 " + aval + ", " + kTrue);
    return rvBool(c);
  }

  // Ops with a raw (flag!=2-only) implementation in numArithRaw; other ops
  // routed through numArith can still produce flag==2 results.
  static bool rawArithOp(BinOp op) {
    switch (op) {
      case BinOp::Add:
      case BinOp::Sub:
      case BinOp::Mul:
      case BinOp::Div:
      case BinOp::FloorDiv:
      case BinOp::Mod:
      case BinOp::BitAnd:
      case BinOp::BitOr:
      case BinOp::BitXor:
      case BinOp::Shl:
      case BinOp::Shr:
        return true;
      default:
        return false;
    }
  }
  // Binary arithmetic/bitwise on possibly-raw operands -> RVal.
  // Returns kind -1 when the op has no boxed fast path (caller falls back).
  // flag==2 operands (boxed aval in the payload slot) take the boxed path.
  RVal numArith(BinOp op, const RVal& l, const RVal& r) {
    if (l.kind != 1 || r.kind != 1) return boxedBinop(op, l, r);
    const char* sym = binopSym(op);
    if (!sym) return rvBad();
    // Both operands proven flag!=2 and the op has a raw impl: skip the
    // boxed-guard branches entirely. The raw impls only reach their boxed
    // slow paths on inputs that panic (div-by-zero, bad shift, ~double), so
    // the result flag stays in {0,1}.
    if (l.safe && r.safe && rawArithOp(op)) {
      RVal res = numArithRaw(op, l, r);
      if (res.kind == 1) res.safe = true;
      return res;
    }
    // Runtime guard: either operand boxed-in-slot -> boxed helper.
    std::string lb = T(), rb = T(), anyB = T();
    emit("  " + lb + " = icmp eq i64 " + l.f + ", 2");
    emit("  " + rb + " = icmp eq i64 " + r.f + ", 2");
    emit("  " + anyB + " = or i1 " + lb + ", " + rb);
    NumPhi ph = numPhi();
    std::string lBox = newLabel("nab"), lRaw = newLabel("nar"),
                lEnd = newLabel("nae");
    emit("  br i1 " + anyB + ", label %" + lBox + ", label %" + lRaw);
    emit("  " + lBox + ":");
    {
      std::string aL = numToAval(l.v, l.f, l.safe);
      std::string aR = numToAval(r.v, r.f, r.safe);
      std::string rr = callRt(sym, {aL, aR});
      numPhiStore(ph, rr, "2");
      emit("  br label %" + lEnd);
    }
    emit("  " + lRaw + ":");
    {
      RVal res = numArithRaw(op, l, r);
      if (res.kind == 1) {
        numPhiStore(ph, res.v, res.f);
      } else {
        // Should not happen for the routed ops; box defensively.
        numPhiStore(ph, boxRVal(res), "2");
      }
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }
  RVal numArithRaw(BinOp op, const RVal& l, const RVal& r) {
    switch (op) {
      case BinOp::Add:
      case BinOp::Sub:
      case BinOp::Mul:
        return numArithOVF(op, l, r);
      case BinOp::Div:
      case BinOp::FloorDiv:
      case BinOp::Mod:
        return numArithDiv(op, l, r);
      case BinOp::BitAnd:
      case BinOp::BitOr:
      case BinOp::BitXor:
        return numBitwise(op, l, r);
      case BinOp::Shl:
      case BinOp::Shr:
        return numShift(op, l, r);
      default:
        return boxedBinop(op, l, r);
    }
  }

  // add/sub/mul: int path uses the with.overflow intrinsics; overflow (and
  // the imul_of INT64_MIN edge) promotes to double exactly like
  // arith_int_int. Double path mirrors the a.*.fast dblp branches.
  RVal numArithOVF(BinOp op, const RVal& l, const RVal& r) {
    const std::string& lp = l.v;
    const std::string& lf = l.f;
    const std::string& rp = r.v;
    const std::string& rf = r.f;
    std::string bothI = bothIntPred(lf, rf);
    NumPhi ph = numPhi();
    std::string lInt = newLabel("nint"), lDbl = newLabel("ndbl"),
                lEnd = newLabel("nend");
    emit("  br i1 " + bothI + ", label %" + lInt + ", label %" + lDbl);
    emit("  " + lInt + ":");
    std::string intr = op == BinOp::Add   ? "llvm.sadd.with.overflow.i64"
                       : op == BinOp::Sub ? "llvm.ssub.with.overflow.i64"
                                          : "llvm.smul.with.overflow.i64";
    std::string ao = T();
    emit("  " + ao + " = call { i64, i1 } @" + intr + "(i64 " + lp +
         ", i64 " + rp + ")");
    std::string res = T(), ovf = T();
    emit("  " + res + " = extractvalue { i64, i1 } " + ao + ", 0");
    emit("  " + ovf + " = extractvalue { i64, i1 } " + ao + ", 1");
    if (op == BinOp::Mul) {
      // imul_of treats any INT64_MIN operand (with a nonzero partner) as
      // overflow -> double, even when the i64 product is representable.
      std::string z1 = T(), z2 = T(), zz = T(), zzN = T();
      std::string e1 = T(), e2 = T(), ee = T(), edge = T(), ovfE = T();
      emit("  " + z1 + " = icmp eq i64 " + lp + ", 0");
      emit("  " + z2 + " = icmp eq i64 " + rp + ", 0");
      emit("  " + zz + " = or i1 " + z1 + ", " + z2);
      emit("  " + zzN + " = icmp eq i1 " + zz + ", false");
      emit("  " + e1 + " = icmp eq i64 " + lp + ", -9223372036854775808");
      emit("  " + e2 + " = icmp eq i64 " + rp + ", -9223372036854775808");
      emit("  " + ee + " = or i1 " + e1 + ", " + e2);
      emit("  " + edge + " = and i1 " + ee + ", " + zzN);
      emit("  " + ovfE + " = or i1 " + ovf + ", " + edge);
      ovf = ovfE;
    }
    std::string lOv = newLabel("novf"), lOk = newLabel("nok");
    emit("  br i1 " + ovf + ", label %" + lOv + ", label %" + lOk);
    emit("  " + lOk + ":");
    numPhiStore(ph, res, "0");
    emit("  br label %" + lEnd);
    emit("  " + lOv + ":");
    {
      // int overflow -> mkdouble((double)a op (double)b)
      std::string di = T(), dj = T(), fd = T(), fb = T();
      emit("  " + di + " = sitofp i64 " + lp + " to double");
      emit("  " + dj + " = sitofp i64 " + rp + " to double");
      emit("  " + fd + " = " + (op == BinOp::Add   ? "fadd"
                            : op == BinOp::Sub ? "fsub"
                                               : "fmul") +
           " double " + di + ", " + dj);
      emit("  " + fb + " = bitcast double " + fd + " to i64");
      numPhiStore(ph, fb, "1");
      emit("  br label %" + lEnd);
    }
    emit("  " + lDbl + ":");
    {
      std::string ld = numAsDouble(lp, lf);
      std::string rd = numAsDouble(rp, rf);
      std::string fd = T(), fb = T();
      emit("  " + fd + " = " + (op == BinOp::Add   ? "fadd"
                            : op == BinOp::Sub ? "fsub"
                                               : "fmul") +
           " double " + ld + ", " + rd);
      emit("  " + fb + " = bitcast double " + fd + " to i64");
      numPhiStore(ph, fb, "1");
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  // Slow-path block shared by div/mod/floordiv/bitwise/shift: box both
  // operands, call the @a.*.fast helper, unbox the result back to raw num.
  // (The helpers panic on the guarded cases, so this is effectively dead —
  // but it keeps the IR total for every input.)
  void emitNumSlow(const std::string& sym, const std::string& lp,
                   const std::string& lf, const std::string& rp,
                   const std::string& rf, const NumPhi& ph,
                   const std::string& lEnd) {
    std::string lb = numToAval(lp, lf, false);
    std::string rb = numToAval(rp, rf, false);
    std::string rr = T();
    emit("  " + rr + " = call i64 @" + sym + "(i64 " + lb + ", i64 " + rb + ")");
    std::string un = T(), up = T(), uf = T();
    emit("  " + un + " = call { i64, i64 } @a.unboxnum(i64 " + rr + ")");
    emit("  " + up + " = extractvalue { i64, i64 } " + un + ", 0");
    emit("  " + uf + " = extractvalue { i64, i64 } " + un + ", 1");
    numPhiStore(ph, up, uf);
    emit("  br label %" + lEnd);
  }

  // div / floordiv / mod: divisor-as-double == 0.0 panics through the boxed
  // slow path (covers int 0 and +/-0.0 alike, matching the C checks).
  RVal numArithDiv(BinOp op, const RVal& l, const RVal& r) {
    const std::string& lp = l.v;
    const std::string& lf = l.f;
    const std::string& rp = r.v;
    const std::string& rf = r.f;
    std::string bothI = bothIntPred(lf, rf);
    std::string rd = numAsDouble(rp, rf);
    NumPhi ph = numPhi();
    const char* sym = op == BinOp::Div       ? "a.div.fast"
                      : op == BinOp::FloorDiv ? "a.floordiv.fast"
                                             : "a.mod.fast";
    std::string lSlow = newLabel("ndz"), lChk = newLabel("ndc"),
                lEnd = newLabel("nde");
    std::string dz = T();
    emit("  " + dz + " = fcmp oeq double " + rd + ", 0.000000e+00");
    emit("  br i1 " + dz + ", label %" + lSlow + ", label %" + lChk);
    emit("  " + lSlow + ":");
    emitNumSlow(sym, lp, lf, rp, rf, ph, lEnd);
    emit("  " + lChk + ":");
    if (op == BinOp::Mod) {
      // int % -1 == 0 (also covers INT64_MIN % -1 which would trap srem).
      std::string bm1 = T(), lZero = newLabel("nzero"), lKind = newLabel("nkd");
      std::string m1 = T();
      emit("  " + m1 + " = icmp eq i64 " + rp + ", -1");
      emit("  " + bm1 + " = and i1 " + bothI + ", " + m1);
      emit("  br i1 " + bm1 + ", label %" + lZero + ", label %" + lKind);
      emit("  " + lZero + ":");
      numPhiStore(ph, "0", "0");
      emit("  br label %" + lEnd);
      emit("  " + lKind + ":");
    }
    // INT64_MIN / -1 promotes to double(2^63); excluded from the int path.
    std::string useI = bothI;
    if (op != BinOp::Mod) {
      std::string e1 = T(), e2 = T(), ee = T(), eeN = T(), edge = T(), u = T();
      emit("  " + e1 + " = icmp eq i64 " + lp + ", -9223372036854775808");
      emit("  " + e2 + " = icmp eq i64 " + rp + ", -1");
      emit("  " + ee + " = and i1 " + e1 + ", " + e2);
      emit("  " + eeN + " = icmp eq i1 " + ee + ", false");
      emit("  " + edge + " = and i1 " + bothI + ", " + eeN);
      useI = edge;
      // True division: int/int yields double — never take the int path.
      if (op == BinOp::Div) useI = "false";
    }
    std::string lInt = newLabel("ndi"), lDbl = newLabel("ndf");
    emit("  br i1 " + useI + ", label %" + lInt + ", label %" + lDbl);
    emit("  " + lInt + ":");
    if (op == BinOp::Div) {
      std::string q = T();
      emit("  " + q + " = sdiv i64 " + lp + ", " + rp);
      numPhiStore(ph, q, "0");
    } else if (op == BinOp::FloorDiv) {
      std::string q = T(), m = T(), rnz = T(), an = T(), bn = T(), dif = T(),
              adj = T(), q1 = T(), res = T();
      emit("  " + q + " = sdiv i64 " + lp + ", " + rp);
      emit("  " + m + " = srem i64 " + lp + ", " + rp);
      emit("  " + rnz + " = icmp ne i64 " + m + ", 0");
      emit("  " + an + " = icmp slt i64 " + lp + ", 0");
      emit("  " + bn + " = icmp slt i64 " + rp + ", 0");
      emit("  " + dif + " = icmp ne i1 " + an + ", " + bn);
      emit("  " + adj + " = and i1 " + rnz + ", " + dif);
      emit("  " + q1 + " = sub i64 " + q + ", 1");
      emit("  " + res + " = select i1 " + adj + ", i64 " + q1 + ", i64 " + q);
      numPhiStore(ph, res, "0");
    } else {  // Mod
      std::string res = T();
      emit("  " + res + " = srem i64 " + lp + ", " + rp);
      numPhiStore(ph, res, "0");
    }
    emit("  br label %" + lEnd);
    emit("  " + lDbl + ":");
    {
      std::string ld = numAsDouble(lp, lf);
      std::string fd = T(), fb = T();
      if (op == BinOp::Mod) {
        // fmod with divisor-sign adjustment (mirrors a.mod.fast dblp).
        std::string fm = T(), mz = T(), mg = T(), bg = T(), dif = T(),
                cond = T(), sum = T(), rs = T();
        emit("  " + fm + " = call double @fmod(double " + ld + ", double " +
             rd + ")");
        emit("  " + mz + " = fcmp une double " + fm + ", 0.000000e+00");
        emit("  " + mg + " = fcmp ogt double " + fm + ", 0.000000e+00");
        emit("  " + bg + " = fcmp ogt double " + rd + ", 0.000000e+00");
        emit("  " + dif + " = icmp ne i1 " + mg + ", " + bg);
        emit("  " + cond + " = and i1 " + mz + ", " + dif);
        emit("  " + sum + " = fadd double " + fm + ", " + rd);
        emit("  " + rs + " = select i1 " + cond + ", double " + sum +
             ", double " + fm);
        emit("  " + fb + " = bitcast double " + rs + " to i64");
      } else {
        emit("  " + fd + " = fdiv double " + ld + ", " + rd);
        if (op == BinOp::FloorDiv) {
          std::string fl = T();
          emit("  " + fl + " = call double @llvm.floor.f64(double " + fd +
               ")");
          fd = fl;
        }
        emit("  " + fb + " = bitcast double " + fd + " to i64");
      }
      numPhiStore(ph, fb, "1");
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  // &, |, ^: int-only op; anything else goes through the boxed helper
  // (which panics on non-ints, like a_band_slow & friends).
  RVal numBitwise(BinOp op, const RVal& l, const RVal& r) {
    const std::string& lp = l.v;
    const std::string& lf = l.f;
    const std::string& rp = r.v;
    const std::string& rf = r.f;
    std::string bothI = bothIntPred(lf, rf);
    const char* sym = op == BinOp::BitAnd ? "a.band.fast"
                    : op == BinOp::BitOr  ? "a.bor.fast"
                                          : "a.bxor.fast";
    const char* iop = op == BinOp::BitAnd ? "and"
                     : op == BinOp::BitOr  ? "or"
                                           : "xor";
    NumPhi ph = numPhi();
    std::string lInt = newLabel("nbi"), lEnd = newLabel("nbe");
    emit("  br i1 " + bothI + ", label %" + lInt + ", label %" + lEnd +
         ".s");
    emit("  " + lInt + ":");
    {
      std::string res = T();
      emit("  " + res + " = " + iop + " i64 " + lp + ", " + rp);
      numPhiStore(ph, res, "0");
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ".s:");
    emitNumSlow(sym, lp, lf, rp, rf, ph, lEnd);
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  // <<, >>: shift amount must be an int in [0,63]; violations take the boxed
  // slow path (panic), matching a_shl_slow/a_shr_slow.
  RVal numShift(BinOp op, const RVal& l, const RVal& r) {
    const std::string& lp = l.v;
    const std::string& lf = l.f;
    const std::string& rp = r.v;
    const std::string& rf = r.f;
    std::string bothI = bothIntPred(lf, rf);
    const char* sym = op == BinOp::Shl ? "a.shl.fast" : "a.shr.fast";
    const char* iop = op == BinOp::Shl ? "shl" : "ashr";
    NumPhi ph = numPhi();
    std::string lInt = newLabel("nsi"), lGo = newLabel("nsg"),
                lBad = newLabel("nsb"), lEnd = newLabel("nse");
    emit("  br i1 " + bothI + ", label %" + lInt + ", label %" + lBad);
    emit("  " + lInt + ":");
    std::string s0 = T(), s1 = T(), rng = T();
    emit("  " + s0 + " = icmp sge i64 " + rp + ", 0");
    emit("  " + s1 + " = icmp sle i64 " + rp + ", 63");
    emit("  " + rng + " = and i1 " + s0 + ", " + s1);
    emit("  br i1 " + rng + ", label %" + lGo + ", label %" + lBad);
    emit("  " + lGo + ":");
    {
      std::string res = T();
      emit("  " + res + " = " + iop + " i64 " + lp + ", " + rp);
      numPhiStore(ph, res, "0");
      emit("  br label %" + lEnd);
    }
    emit("  " + lBad + ":");
    emitNumSlow(sym, lp, lf, rp, rf, ph, lEnd);
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  // ---- raw comparisons ----------------------------------------------------------
  // -> Bool RVal. flag==2 operands (boxed aval in payload) take the boxed
  // comparison helper.
  RVal numCmp(BinOp op, const RVal& l, const RVal& r) {
    if (l.kind != 1 || r.kind != 1) {
      RVal b = boxedBinop(op, l, r);
      return b.kind == 0 ? avalToBool(b.v) : b;
    }
    const char* sym = binopSym(op);
    if (!sym) return rvBad();
    // flag!=2 proven on both sides: the boxed branch is dead.
    if (l.safe && r.safe) {
      RVal res = numCmpRaw(op, l, r);
      return res.kind == 2 ? res : rvBool("false");
    }
    std::string lb = T(), rb = T(), anyB = T();
    emit("  " + lb + " = icmp eq i64 " + l.f + ", 2");
    emit("  " + rb + " = icmp eq i64 " + r.f + ", 2");
    emit("  " + anyB + " = or i1 " + lb + ", " + rb);
    std::string ph = boolPhi();
    std::string lBox = newLabel("nqb"), lRaw = newLabel("nqr"),
                lEnd = newLabel("nqe");
    emit("  br i1 " + anyB + ", label %" + lBox + ", label %" + lRaw);
    emit("  " + lBox + ":");
    {
      std::string aL = numToAval(l.v, l.f, l.safe);
      std::string aR = numToAval(r.v, r.f, r.safe);
      std::string rr = callRt(sym, {aL, aR});
      std::string c = T();
      emit("  " + c + " = icmp eq i64 " + rr + ", " + kTrue);
      boolPhiStore(ph, c);
      emit("  br label %" + lEnd);
    }
    emit("  " + lRaw + ":");
    {
      RVal res = numCmpRaw(op, l, r);
      if (res.kind == 2) {
        boolPhiStore(ph, res.v);
      } else {
        boolPhiStore(ph, "false");
      }
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return rvBool(boolPhiLoad(ph));
  }
  RVal numCmpRaw(BinOp op, const RVal& l, const RVal& r) {
    const std::string& lp = l.v;
    const std::string& lf = l.f;
    const std::string& rp = r.v;
    const std::string& rf = r.f;
    switch (op) {
      case BinOp::Lt:
      case BinOp::Le:
      case BinOp::Gt:
      case BinOp::Ge: {
        // int-int -> icmp (exact), otherwise fcmp on double values —
        // matches the a.*.fast dblp path which converts ints via nval.
        const char* ip = op == BinOp::Lt ? "slt"
                         : op == BinOp::Le ? "sle"
                         : op == BinOp::Gt ? "sgt"
                                           : "sge";
        const char* fp = op == BinOp::Lt ? "olt"
                         : op == BinOp::Le ? "ole"
                         : op == BinOp::Gt ? "ogt"
                                           : "oge";
        std::string bothI = bothIntPred(lf, rf);
        std::string ld = numAsDouble(lp, lf);
        std::string rd = numAsDouble(rp, rf);
        std::string ic = T(), fc = T(), c = T();
        emit("  " + ic + " = icmp " + ip + " i64 " + lp + ", " + rp);
        emit("  " + fc + " = fcmp " + fp + " double " + ld + ", " + rd);
        emit("  " + c + " = select i1 " + bothI + ", i1 " + ic + ", i1 " + fc);
        return rvBool(c);
      }
      case BinOp::Eq:
      case BinOp::Ne: {
        // The fast helpers only compare same-kind numerics inline; mixed
        // int/double goes slow (values_equal). Mirror that with a
        // same-kind runtime dispatch.
        const char* sym = op == BinOp::Eq ? "a.eq.fast" : "a.ne.fast";
        const char* ip = op == BinOp::Eq ? "eq" : "ne";
        const char* fp = op == BinOp::Eq ? "oeq" : "une";
        std::string ph = boolPhi();
        std::string lSame = newLabel("neq"), lKind = newLabel("nkd"),
                    lInt = newLabel("nci"), lDbl = newLabel("ncd"),
                    lMix = newLabel("ncm"), lEnd = newLabel("nce");
        std::string same = T();
        emit("  " + same + " = icmp eq i64 " + lf + ", " + rf);
        emit("  br i1 " + same + ", label %" + lKind + ", label %" + lMix);
        emit("  " + lKind + ":");
        std::string lfi = flagIsDouble(lf);
        emit("  br i1 " + lfi + ", label %" + lDbl + ", label %" + lInt);
        emit("  " + lInt + ":");
        {
          std::string c = T();
          emit("  " + c + " = icmp " + ip + " i64 " + lp + ", " + rp);
          boolPhiStore(ph, c);
          emit("  br label %" + lEnd);
        }
        emit("  " + lDbl + ":");
        {
          std::string lb = T(), rb = T(), c = T();
          emit("  " + lb + " = bitcast i64 " + lp + " to double");
          emit("  " + rb + " = bitcast i64 " + rp + " to double");
          emit("  " + c + " = fcmp " + fp + " double " + lb + ", " + rb);
          boolPhiStore(ph, c);
          emit("  br label %" + lEnd);
        }
        emit("  " + lMix + ":");
        {
          std::string bv = callRt(sym, {boxRVal(l), boxRVal(r)});
          std::string c = T();
          emit("  " + c + " = icmp eq i64 " + bv + ", " + kTrue);
          boolPhiStore(ph, c);
          emit("  br label %" + lEnd);
        }
        emit("  " + lEnd + ":");
        return rvBool(boolPhiLoad(ph));
      }
      default:
        return rvBad();
    }
  }

  // ---- raw unary ops --------------------------------------------------------------
  // Unary numeric op guard: flag==2 operand -> boxed @a.*.fast helper, result
  // stays boxed-in-slot (flag 2). Otherwise the raw impl runs.
  RVal numUnaryGuard(const char* sym, const RVal& x,
                     RVal (LlvmEmitter::*raw)(const RVal&)) {
    const std::string& xp = x.v;
    const std::string& xf = x.f;
    // flag!=2 proven: the boxed branch is dead; run the raw impl directly.
    if (x.safe) {
      RVal res = (this->*raw)(x);
      if (res.kind == 1) res.safe = true;
      return res;
    }
    std::string isB = T(), lB = newLabel("nub"), lN = newLabel("nun"),
                lEnd = newLabel("nue");
    emit("  " + isB + " = icmp eq i64 " + xf + ", 2");
    emit("  br i1 " + isB + ", label %" + lB + ", label %" + lN);
    NumPhi ph = numPhi();
    emit("  " + lB + ":");
    {
      std::string rr = callRt(sym, {xp});
      numPhiStore(ph, rr, "2");
      emit("  br label %" + lEnd);
    }
    emit("  " + lN + ":");
    {
      RVal res = (this->*raw)(x);
      numPhiStore(ph, res.v, res.f);
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  RVal numNeg(const RVal& x) {
    return numUnaryGuard("a.neg.fast", x, &LlvmEmitter::numNegRaw);
  }
  RVal numBNot(const RVal& x) {
    return numUnaryGuard("a.bnot.fast", x, &LlvmEmitter::numBNotRaw);
  }

  RVal numNegRaw(const RVal& x) {
    // -i: INT64_MIN promotes to double(-(double)INT64_MIN) like a_neg_slow;
    // -d is fneg. Merged through a phi.
    const std::string& xp = x.v;
    const std::string& xf = x.f;
    NumPhi ph = numPhi();
    std::string lDbl = newLabel("nnd"), lInt = newLabel("nni"),
                lEnd = newLabel("nne");
    std::string fdi = flagIsDouble(xf);
    emit("  br i1 " + fdi + ", label %" + lDbl + ", label %" + lInt);
    emit("  " + lInt + ":");
    std::string isMin = T(), lMin = newLabel("nnm"), lGo = newLabel("nng");
    emit("  " + isMin + " = icmp eq i64 " + xp + ", -9223372036854775808");
    emit("  br i1 " + isMin + ", label %" + lMin + ", label %" + lGo);
    emit("  " + lGo + ":");
    {
      std::string n = T();
      emit("  " + n + " = sub i64 0, " + xp);
      numPhiStore(ph, n, "0");
      emit("  br label %" + lEnd);
    }
    emit("  " + lMin + ":");
    // INT64_MIN -> double path (same code as lDbl).
    {
      std::string di = T(), nd = T(), nb = T();
      emit("  " + di + " = sitofp i64 " + xp + " to double");
      emit("  " + nd + " = fneg double " + di);
      emit("  " + nb + " = bitcast double " + nd + " to i64");
      numPhiStore(ph, nb, "1");
      emit("  br label %" + lEnd);
    }
    emit("  " + lDbl + ":");
    {
      std::string db = T(), nd = T(), nb = T();
      emit("  " + db + " = bitcast i64 " + xp + " to double");
      emit("  " + nd + " = fneg double " + db);
      emit("  " + nb + " = bitcast double " + nd + " to i64");
      numPhiStore(ph, nb, "1");
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  RVal numBNotRaw(const RVal& x) {
    // ~i = xor -1; ~d panics through the boxed slow path.
    const std::string& xp = x.v;
    const std::string& xf = x.f;
    NumPhi ph = numPhi();
    std::string lInt = newLabel("nbi"), lEnd = newLabel("nbe");
    std::string fdi = flagIsDouble(xf);
    emit("  br i1 " + fdi + ", label %" + lEnd + ".s, label %" + lInt);
    emit("  " + lInt + ":");
    {
      std::string res = T();
      emit("  " + res + " = xor i64 " + xp + ", -1");
      numPhiStore(ph, res, "0");
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ".s:");
    // Unary slow path: box the operand, call the unary helper, unbox.
    {
      std::string lb = numToAval(xp, xf, x.safe), rr = T();
      emit("  " + rr + " = call i64 @a.bnot.fast(i64 " + lb + ")");
      std::string un = T(), up = T(), uf = T();
      emit("  " + un + " = call { i64, i64 } @a.unboxnum(i64 " + rr + ")");
      emit("  " + up + " = extractvalue { i64, i64 } " + un + ", 0");
      emit("  " + uf + " = extractvalue { i64, i64 } " + un + ", 1");
      numPhiStore(ph, up, uf);
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return numPhiLoad(ph);
  }

  RVal numNot(const RVal& x) {
    // `not` -> Bool: int -> (p == 0), double -> (d == 0.0) — matches
    // a.not.fast's intp/dd paths including NaN -> false.
    const std::string& xp = x.v;
    const std::string& xf = x.f;
    // flag==2 -> boxed aval: logical not via a.not.fast -> Bool. When the
    // operand is proven flag!=2 that branch is dead.
    if (x.safe) {
      std::string fi = flagIsDouble(xf);
      std::string db = T(), dz = T(), iz = T(), c = T();
      emit("  " + db + " = bitcast i64 " + xp + " to double");
      emit("  " + dz + " = fcmp oeq double " + db + ", 0.000000e+00");
      emit("  " + iz + " = icmp eq i64 " + xp + ", 0");
      emit("  " + c + " = select i1 " + fi + ", i1 " + dz + ", i1 " + iz);
      return rvBool(c);
    }
    std::string isB = T(), lB = newLabel("nnb"), lN = newLabel("nnn"),
                lEnd = newLabel("nne");
    emit("  " + isB + " = icmp eq i64 " + xf + ", 2");
    emit("  br i1 " + isB + ", label %" + lB + ", label %" + lN);
    std::string ph = boolPhi();
    emit("  " + lB + ":");
    {
      std::string rr = T(), c = T();
      emit("  " + rr + " = call i64 @a.not.fast(i64 " + xp + ")");
      emit("  " + c + " = icmp eq i64 " + rr + ", " + kTrue);
      boolPhiStore(ph, c);
      emit("  br label %" + lEnd);
    }
    emit("  " + lN + ":");
    {
      std::string fi = flagIsDouble(xf);
      std::string db = T(), dz = T(), iz = T(), c = T();
      emit("  " + db + " = bitcast i64 " + xp + " to double");
      emit("  " + dz + " = fcmp oeq double " + db + ", 0.000000e+00");
      emit("  " + iz + " = icmp eq i64 " + xp + ", 0");
      emit("  " + c + " = select i1 " + fi + ", i1 " + dz + ", i1 " + iz);
      boolPhiStore(ph, c);
      emit("  br label %" + lEnd);
    }
    emit("  " + lEnd + ":");
    return rvBool(boolPhiLoad(ph));
  }

  // Condition -> i1 for if/while/do-while/ternary/match guards.
  std::string condI1(const Expr* e) {
    RVal rv = evalRaw(e);
    if (rv.kind == 2) return rv.v;
    if (rv.kind == 1) {
      // flag==2 -> payload is a boxed aval: a.truthy. When the operand is
      // proven flag!=2 that branch is dead. Otherwise numeric truthiness:
      // int -> p != 0, double -> d != 0.0 (une: NaN is truthy, matching
      // a_truthy/a.not.fast).
      if (rv.safe) {
        std::string fi = flagIsDouble(rv.f);
        std::string db = T(), dz = T(), iz = T(), t = T();
        emit("  " + db + " = bitcast i64 " + rv.v + " to double");
        emit("  " + dz + " = fcmp une double " + db + ", 0.000000e+00");
        emit("  " + iz + " = icmp ne i64 " + rv.v + ", 0");
        emit("  " + t + " = select i1 " + fi + ", i1 " + dz + ", i1 " + iz);
        return t;
      }
      std::string isB = T(), lB = newLabel("ncb"), lN = newLabel("ncn"),
                  lEnd = newLabel("nce");
      emit("  " + isB + " = icmp eq i64 " + rv.f + ", 2");
      emit("  br i1 " + isB + ", label %" + lB + ", label %" + lN);
      std::string ph = alloca("i1");
      emit("  " + lB + ":");
      {
        std::string t = T();
        emit("  " + t + " = call i1 @a.truthy(i64 " + rv.v + ")");
        emit("  store i1 " + t + ", ptr " + ph);
        emit("  br label %" + lEnd);
      }
      emit("  " + lN + ":");
      {
        std::string fi = flagIsDouble(rv.f);
        std::string db = T(), dz = T(), iz = T(), t = T();
        emit("  " + db + " = bitcast i64 " + rv.v + " to double");
        emit("  " + dz + " = fcmp une double " + db + ", 0.000000e+00");
        emit("  " + iz + " = icmp ne i64 " + rv.v + ", 0");
        emit("  " + t + " = select i1 " + fi + ", i1 " + dz + ", i1 " + iz);
        emit("  store i1 " + t + ", ptr " + ph);
        emit("  br label %" + lEnd);
      }
      emit("  " + lEnd + ":");
      std::string r = T();
      emit("  " + r + " = load i1, ptr " + ph);
      return r;
    }
    std::string t = T();
    emit("  " + t + " = call i1 @a.truthy(i64 " + rv.v + ")");
    return t;
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
        std::string gt = condI1(arm.guard.get());
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
    std::string t = condI1(x.cond.get());
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
    std::map<std::string, LlBinding> caps;  // name -> binding (boxed on capture)
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      for (auto& kv : *it)
        if ((kv.second.kind == BKind::Local || kv.second.kind == BKind::Param) &&
            !caps.count(kv.first))
          caps[kv.first] = kv.second;
    PendingLambda pl;
    pl.expr = &x;
    pl.moduleId = curModuleId_;
    pl.cname = "__a_lam_" + moduleTag(curModuleId_ < 0 ? 0 : curModuleId_) + "_" +
               std::to_string(lamCounter_++);
    for (auto& kv : caps) pl.captures.push_back({kv.first, ""});
    lambdas_.push_back(pl);
    // env = a_closure_env_new(n); a_env_set(env, i, boxed cap)
    std::string env = callRt("a_closure_env_new", {std::to_string(pl.captures.size())});
    for (std::size_t i = 0; i < pl.captures.size(); ++i) {
      std::string v = bindingValue(caps[pl.captures[i].first]);
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
          if (externFns_.count(fi)) return emitExternCall(fi, call);
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
          if (externFns_.count(fi)) return emitExternCall(fi, call);
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
              if (externFns_.count(target)) return emitExternCall(target, call);
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
    // Vector intrinsics (sum/min/max/dot/to_list) — safety net for any path
    // that bypasses evalRaw's Call handling.
    if (vecExprType(objExpr)) {
      bool handled = false;
      RVal r = evalVecIntrinsic(objExpr, field, call, handled);
      if (handled) return boxRVal(r);
    }
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
    moduleBodyScopeDepth_ = (int)scopes_.size();
    bindModuleImports(moduleId);
    analyzeModRawSafety(moduleId, mod.program->statements);
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
    moduleBodyScopeDepth_ = -1;
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
      if (f->isMethod || externFns_.count(f)) continue;
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
      if (!f->isMethod && !externFns_.count(f) &&
          f->moduleId == graph_.entry &&
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
