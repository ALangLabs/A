// `a`-dialect MLIR emitter (A -> .mlir -> a-lower-to-llvm -> LLVM IR).
// Sibling of mlir_emit.cpp: identical AST coverage and identical semantics,
// exposed under the emitMLIRDialect entry point for the `a`-dialect pipeline.
//
// DIALECT STATUS (verified against `a mlir-opt`, MLIR 18 + this tree's
// src/astra/mlir): this emitter produces the llvm-dialect op stream with the
// `a`-dialect ops that ARE text-expressible and semantics-preserving:
//   * every `a_*` runtime call (helpers callI64/callI64T/callI32T/
//     callVoidT, pin, frame push/pop, gc roots, a_runtime_init, a_fib_main,
//     a_test_main) emits `a.runtime.call "sym"(args) {signature = ...} :
//     (types) -> ret`. a-lower-to-llvm lowers it to `llvm.call @sym` with
//     the same signature inferred from lowered operand/result types.
//   * pointers that exclusively feed `a.runtime.call` pointer arguments
//     (cstrPtr/stringLiteralAval/char-literal globals, %gr0 in emitMain)
//     emit `a.addressof @g : !a.ptr` (lowers to llvm.mlir.addressof).
// Everything else stays llvm.* on purpose:
//   * a.func/a.return: the emitter's aval domain is raw i64 bit patterns;
//     a.func param unpack produces !a.aval/typed results and canonicalize
//     hoists llvm.mlir.constant out of a.func regions, breaking
//     a-lower-to-llvm (constant defined outside region).
//   * a.store/a.load/a.gep/a.alloca: verifiers require !a.ptr pointers and
//     a.store requires !a.aval/!a.ptr values; the emitter stores raw i64.
//   * a.const: result is always !a.aval; cannot feed i64 consumers.
//   * a.call: callee type must match operand/result types; A-fn ABI is the
//     packed argv pointer (callAFunction), which a.call cannot express.
//   * llvm.mlir.global, llvm.mlir.constant, control flow, llvm.return, and
//     addressof sites feeding llvm.load/store/ptrtoint stay llvm.*.
//
// Coverage (same as emitMLIR): entry points, plain `fn` declarations and
// direct calls, let/assign/expr/return/if/while/do-while/for/loop/
// break/continue, literals (int/float/bool/char/string/array/tuple/map/set/
// range), unary and binary operators through the boxed runtime slow paths,
// indexing, field access, builtins via a_bi_call, top-level lets as globals.
// Deferred (clear error): extern fns, generics/comptime params.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <cctype>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "fastpath_mlir.hpp"
#include "mlir/mlir_util.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace {

// Shared with mlir_emit.cpp via mlir_util.hpp.
using mlirutil::avalIntEncode;
using mlirutil::kAvalTag;
using mlirutil::kInlineLim;
using mlirutil::parforEnabled;
using mlirutil::pfDenyName;
using mlirutil::runtimeDeclText;
using mlirutil::stemOf;

struct MlBinding {
  std::string slot;   // alloca SSA name (a !llvm.ptr value)
  std::string dropFn; // cname of struct `drop` method, "" if none
  // "aval" (default): slot holds a raw i64 aval word. Otherwise the MLIR
  // scalar type of the unboxed value stored in the slot ("i64"/"f64"/"i1");
  // only produced when A_MLIR_TYPED=1 and Sema proves a scalar binding.
  std::string type = "aval";
};

// Typed scalar SSA value produced by evalExprT (A_MLIR_TYPED=1 only).
// kind == "aval": `name` is a raw i64 aval word (phase-1 representation).
// kind == "i64"/"f64"/"i1": `name` is an unboxed MLIR scalar of that type.
// isConst marks literal/constant-folded values so boxToAval can re-emit the
// cheap aval constant encoding instead of the range-checked box sequence.
struct TypedVal {
  std::string name;
  std::string kind = "aval";
  bool isConst = false;
  std::int64_t iConst = 0;
  double fConst = 0.0;
};

// ---- inlined a_*_fast runtime fast paths ------------------------------------
// The a_*_fast exports (src/astra/runtime/fastpath.c) are re-emitted here as
// llvm.func definitions so the LLVM inliner folds the NaN-box tag checks and
// the inline-int arithmetic into callers instead of paying a runtime call
// per operator. Semantics mirror fastpath.c exactly: every case the C fast
// path defers goes to the same *_slow export. linkonce_odr keeps the defs
// discardable and conflict-free if libaruntime.a's fastpath.o is ever pulled
// (its strong symbols simply win; behaviour is identical either way).

namespace {
// kFastPathMLIR packs all a_*_fast defs into one blob; split it into
// per-function fragments (lazily, once) so the emitter can append only the
// defs the module actually calls — the full blob is ~2.3k MLIR ops and
// parsing/optimizing unused defs dominated compile time.
struct FastPathFrag {
  std::string name;
  std::string text;
};
const std::vector<FastPathFrag>& fastPathFrags() {
  static const std::vector<FastPathFrag> v = [] {
    std::vector<FastPathFrag> out;
    const std::string s = fastpathmlir::kFastPathMLIR;
    std::vector<size_t> starts;
    for (size_t p = s.find("llvm.func @"); p != std::string::npos;
         p = s.find("llvm.func @", p + 1))
      starts.push_back(p);
    for (size_t i = 0; i < starts.size(); ++i) {
      const size_t b = starts[i];
      const size_t e = (i + 1 < starts.size()) ? starts[i + 1] : s.size();
      const size_t ns = b + 11;  // strlen("llvm.func @")
      const size_t ne = s.find('(', ns);
      if (ne == std::string::npos) continue;
      out.push_back({s.substr(ns, ne - ns), s.substr(b, e - b)});
    }
    return out;
  }();
  return v;
}
}  // namespace

class MlirDialectEmitter {
 public:
  MlirDialectEmitter(const ModuleGraph& graph, const SemaResult& sema,
              bool testMode = false, const std::string& testFilter = "")
      : graph_(graph), sema_(sema), testMode_(testMode),
        testFilter_(testFilter) {
    // A_MLIR_TYPED defaults ON (pipeline typedMlirEnabled agrees); only an
    // explicit off value disables unboxed scalar emission.
    const char* t = std::getenv("A_MLIR_TYPED");
    std::string ts = t ? t : "";
    for (char& c : ts) c = (char)std::tolower((unsigned char)c);
    typed_ = !(ts == "0" || ts == "off" || ts == "no" || ts == "false");
  }

  bool run(std::string& out, std::string& error) {
    (void)sema_;
    error_ = &error;
    collectTopLevel();
    emitModuleOpen();
    emitDecls();
    emitGlobals();
    resolveImpls();
    emitFunctions();
    emitStructCtors();
    emitMainBody();
    emitGcRoots();
    emitMain();
    emitLambdas();  // drain last: lambdas may be created anywhere
    // Inline-able a_*_fast fast-path definitions (fastpath.c semantics as
    // llvm.func bodies) so the LLVM optimizer folds tag checks into callers.
    // Emitted only for symbols the module actually calls: the full blob is
    // ~2.3k MLIR ops and parsing+optimizing unused defs dominated compile
    // time. Must run after emitLambdas (lambdas can emit _fast calls).
    emitUsedFastPaths();
    if (failed_) return false;
    out = pre_.str() + body_.str() + "}\n";
    return true;
  }

  // Per-module emission (Mojo-style .ambc prerequisite): emit ONLY the decls
  // owned by `moduleId` — its functions, struct ctors, globals, and a
  // `__a_m<id>_init` fn holding its global inits + top-level stmts. All
  // cross-module symbol tables are still collected (collectTopLevel scans the
  // whole graph), so calls/globals/methods in other modules resolve to the
  // same mangled symbols the whole-graph emitter produces. Program-global
  // pieces (runtime decls, __a_gc_root_mark, __a_main_body orchestration,
  // main/a_test_main) are NOT emitted; the consumer re-declares them.
  // Mirrors MlirEmitter::runModule in mlir_emit.cpp.
  bool runModule(int moduleId, std::string& out, std::string& error) {
    if (moduleId < 0 || moduleId >= (int)graph_.modules.size()) {
      error = "mlir-a backend: emitModuleMLIR: invalid module id " +
              std::to_string(moduleId);
      return false;
    }
    emitOnlyModule_ = moduleId;
    error_ = &error;
    collectTopLevel();
    emitModuleOpen();
    resolveImpls();
    emitGlobals();
    emitFunctions();
    emitStructCtors();
    emitModuleInit();
    emitLambdas();  // drain lambdas/parfors created by this module's code
    emitModuleDecls();  // last: scans emitted text for foreign @symbol refs
    if (failed_) return false;
    out = pre_.str() + body_.str() + "}\n";
    return true;
  }

  // Entry-body emission for per-module .ambc packages: `out` = the entry
  // fn's body statements (splice-ready for the consumer's synthesized
  // __a_main_body), `support` = a `module { ... }` document with the
  // module-level ops the body created (literal globals, auto-vivified
  // __a_g_ globals, lambda/parfor helpers) for the precompile step to merge
  // into the entry module's .ambc. Mirrors MlirEmitter::runEntryBody.
  bool runEntryBody(std::string& out, std::string& support,
                    std::string& error);

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::string* error_ = nullptr;
  bool failed_ = false;

  std::ostringstream pre_;   // module { ... decls + globals
  std::ostringstream body_;  // llvm.func definitions (outside module? no—inside)
  std::ostringstream fn_;    // current function body
  std::ostringstream fnAllocs_;  // hoisted entry-block allocas
  std::string curFnName_, curFnFile_;
  int curFnLine_ = 0;

  std::unordered_map<std::string, std::string> fnCnames_;
  std::unordered_map<std::string, std::string> globals_;
  // import aliases: local name -> native module name (for mod.member calls)
  std::unordered_map<std::string, std::string> nativeAliases_;
  // from-imported builtin members: local name -> builtin name
  std::unordered_map<std::string, std::string> builtinAliases_;
  // from-imported native constants: local name -> constant name (PI/E/...)
  std::unordered_map<std::string, std::string> constAliases_;
  // A-module support: (moduleId, fnName) -> FnDecl* ; local alias -> FnDecl*
  std::map<std::pair<int, std::string>, const FnDecl*> moduleFns_;
  std::unordered_map<std::string, const FnDecl*> aModuleFnAliases_;
  // import "foo.astra" [as x]: alias -> moduleId
  std::unordered_map<std::string, int> aModuleAliases_;
  // per-module top-level lets: (moduleId, name) -> global symbol, for
  // module-member value resolution (os.name, os.path, ...)
  std::map<std::pair<int, std::string>, std::string> moduleGlobals_;
  int curModuleId_ = -1;  // module owning the code currently being emitted
  // moduleId -> exported local name -> (source moduleId, original name)
  std::map<int, std::map<std::string, std::pair<int, std::string>>> moduleReexports_;
  // from-imported module-level values: local name -> (moduleId, member name);
  // resolved lazily to a global symbol at use sites (covers `let`/implicit
  // globals re-exported through __init__ chains)
  std::unordered_map<std::string, std::pair<int, std::string>> moduleGlobalAliases_;
  // (moduleId, structName) -> ctor cname
  std::map<std::pair<int, std::string>, std::string> moduleStructCtors_;
  // python module aliases: local name -> python module path
  std::unordered_map<std::string, std::string> pythonAliases_;
  // from python:x import f: local name -> (module, member)
  std::unordered_map<std::string, std::pair<std::string, std::string>> pyMemberAliases_;
  struct GlobalInit {
    int moduleId;
    const LetStmt* stmt;
  };
  std::vector<GlobalInit> globalInits_;
  struct TopStmt {
    int moduleId;
    const Stmt* stmt;
  };
  std::vector<TopStmt> topStmts_;
  const FnDecl* entry_ = nullptr;
  // fn decls in emission order
  std::vector<std::pair<const FnDecl*, int>> fnDecls_;
  std::unordered_map<const FnDecl*, std::string> fnDeclCnames_;
  std::unordered_map<std::string, const FnDecl*> fnByCname_;
  struct MlStruct {
    std::string displayName;
    std::string ctorCname;
    int moduleId = -1;
    std::vector<std::string> fieldNames;
    std::vector<std::pair<std::string, std::string>> methods;  // name -> cname
  };
  struct MlVariant {
    int tag = 0;
    bool algebraic = false;
    std::vector<std::string> payloadFields;
    std::int64_t value = 0;
  };
  std::unordered_map<std::string, MlVariant> enumVariants_;  // "Enum.Variant" -> info
  std::vector<MlStruct> structs_;
  std::unordered_map<std::string, std::string> structCtors_;
  // method FnDecls to emit: (decl, cname)
  std::vector<std::pair<const FnDecl*, std::string>> methodFns_;
  std::unordered_map<std::string, const FnDecl*> methodDecls_;  // cname -> decl
  std::vector<const ImplDecl*> pendingImpls_;
  struct PendingLambda {
    const LambdaExpr* expr;
    std::vector<std::string> captures;  // names, sorted
    std::string cname;
    int moduleId = -1;
  };
  std::vector<PendingLambda> pendingLambdas_;
  int lamCounter_ = 0;
  struct PendingParfor {
    const ForStmt* stmt;
    std::vector<std::string> capNames;
    std::string cname;
    int moduleId = -1;
  };
  std::vector<PendingParfor> pendingParfors_;
  int parforCounter_ = 0;
  // >=0 restricts emission to one module's owned decls (runModule path);
  // symbol tables still cover the whole graph so cross-module references
  // resolve to identical mangled names. -1 = whole-graph mode.
  int emitOnlyModule_ = -1;
  // ImplDecl -> owning module, and impl method FnDecl -> owning module
  // (Sema leaves impl methods at moduleId=-1; only consulted in split mode).
  std::unordered_map<const ImplDecl*, int> implModule_;
  std::unordered_map<const FnDecl*, int> implMethodModule_;
  // Entry-body mode (runEntryBody): emitter-private symbols get
  // "__a_e"-flavored names (__a_estr/__a_echr/__a_ecstr/__a_elam_/
  // __a_epf_) so they cannot collide with the per-module units'
  // __a_str/__a_chr/__a_cstr/__a_lam_/__a_pf_ counters (which restart per
  // module). privPrefix() yields "__a_e" in this mode, "__a_" otherwise.
  bool entryBodyMode_ = false;
  // The consumer's synthesized __a_main_body has no a_frame_push, so the
  // entry body's own `return` must not emit a_frame_pop (whole-graph
  // __a_main_body pushes one frame for itself). Only suppresses the pop
  // while emitting the entry body's statements — helper funcs keep
  // balanced push/pop.
  bool suppressFramePop_ = false;
  std::string privPrefix() const { return entryBodyMode_ ? "__a_e" : "__a_"; }
  bool inFnBody_ = false;
  bool inUserFn_ = false;  // inside a user fn/lambda/parfor body (not main_body)
  bool inEntryBody_ = false;  // inside the entry fn's body in __a_main_body
  // A_MLIR_TYPED=1: emit unboxed i64/f64/i1 scalars for Sema-proven Int/
  // Float/Bool exprs instead of always aval-encoding. Read once here; when
  // off every typed path below is skipped and output is unchanged.
  bool typed_ = false;
  // Per-function assign-kind safety scan for typed bindings: bitmask of the
  // scalar kinds (`scalarKindBit`) seen in AssignStmt values targeting each
  // name anywhere in the function body. A binding may only be typed when
  // every recorded assign kind equals the binding kind (unannotated A
  // bindings rebind freely, so `x = 5; x = "s"` must stay aval).
  std::unordered_map<std::string, int> assignKindMask_;
  bool assignScanPending_ = false;
  // Typed-call ABI: eligible fns emit a `__a_tc_<cname>` core taking/returning
  // raw scalars plus a boxed-ABI thunk under `<cname>`. inTypedCore_ marks
  // body emission inside a core (typed returns, no argv params).
  bool inTypedCore_ = false;
  // A typed core whose body cannot call anything, allocate, or trap needs no
  // frame hooks: there is no young object to track, no traceback to enrich,
  // and no stack overflow risk. Skipping a_frame_push/pop on these leaf cores
  // lets LLVM fully inline them (the calls are opaque side effects otherwise)
  // — without this, a call in a hot loop keeps two runtime calls per iteration
  // even after the callee body is inlined (100x+ slowdown on call-heavy code).
  bool curFnFrameFree_ = false;
  // Non-empty while emitting a kernel/device fn's typed core: the value is
  // the `a.offload` attribute string ("kernel"/"device") stamped on the
  // llvm.func so the MLIR backend can slice it out for device compilation.
  std::string curOffloadAttr_;
  std::unordered_map<const FnDecl*, bool> frameFreeMemo_;
  std::unordered_set<const FnDecl*> frameFreeVisiting_;
  std::string typedCoreRetKind_;
  std::unordered_map<const FnDecl*, int> typedCoreCache_;  // 1=yes, -1=no
  bool testMode_ = false;
  std::string testFilter_;

  // current function state
  std::vector<std::map<std::string, MlBinding>> scopes_;
  // RAII drops: per-scope list of (slot, dropFnCname), emitted LIFO at
  // scope exit — mirrors LlvmEmitter's DropRec frames.
  std::vector<std::vector<std::pair<std::string, std::string>>> dropStack_;
  // struct display/full name -> MlStruct index for drop lookup
  std::unordered_map<std::string, size_t> structByName_;
  std::vector<std::string> rootSyms_;  // global syms registered as GC roots
  std::unordered_map<std::int64_t, std::string> constCache_;
  int vCounter_ = 0;
  int labelCounter_ = 0;
  int strCounter_ = 0;
  std::vector<std::string> breakLabels_;
  std::vector<std::string> contLabels_;
  std::vector<const Stmt*> pendingDefers_;  // function-scope defer (MVP)
  bool terminated_ = false;  // current block already has a terminator

  void fail(const std::string& msg) {
    if (failed_) return;
    failed_ = true;
    *error_ = "mlir-a backend: " + msg;
  }
  void failAt(const std::string& msg, const Expr* e) {
    (void)e;
    fail(msg);
  }
  void failAt(const std::string& msg, const Stmt* s) {
    if (s) {
      std::ostringstream ss;
      ss << msg << " (stmt kind " << (int)s->kind << " @line " << s->span.line << ")";
      fail(ss.str());
      return;
    }
    fail(msg);
  }

  std::string nv() { return "%v" + std::to_string(vCounter_++); }
  std::string nl() { return "L" + std::to_string(labelCounter_++); }

  // ---- constants -------------------------------------------------------------
  std::string c64(std::int64_t v) {
    // No cross-block caching: a constant defined in one MLIR block does not
    // dominate sibling blocks. Fresh constant per use site; canonicalize
    // dedupes later. (constCache_ kept for potential dominator-aware reuse.)
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.constant(" << v << " : i64) : i64\n";
    return r;
  }
  std::string avalInt(std::int64_t v) {
    if (v >= -kInlineLim && v < kInlineLim) return c64(avalIntEncode(v));
    return callI64("a_i64n", {c64(v)});
  }
  std::string avalDbl(double d) {
    if (d != d) return c64(9221120237041090560LL);
    std::uint64_t u;
    std::memcpy(&u, &d, 8);
    return c64((std::int64_t)u);
  }
  std::string avalBool(bool b) { return c64(b ? (std::int64_t)kAvalTag + 6 : (std::int64_t)kAvalTag + 4); }

  // ---- memory ------------------------------------------------------------------
  std::string allocaI64() {
    std::string r = nv();
    std::string sz = nv();
    fnAllocs_ << "  " << sz << " = llvm.mlir.constant(1 : i64) : i64\n";
    fnAllocs_ << "  " << r << " = llvm.alloca " << sz
              << " x !llvm.array<1 x i64> : (i64) -> !llvm.ptr\n";
    return r;
  }
  std::string allocaArr(int n) {
    int m = n > 0 ? n : 1;
    std::string r = nv();
    std::string sz = nv();
    fnAllocs_ << "  " << sz << " = llvm.mlir.constant(" << m << " : i64) : i64\n";
    fnAllocs_ << "  " << r << " = llvm.alloca " << sz
              << " x !llvm.array<" << m << " x i64> : (i64) -> !llvm.ptr\n";
    return r;
  }
  std::string gepIdx(const std::string& p, const std::string& idx) {
    std::string r = nv();
    fn_ << "  " << r << " = llvm.getelementptr " << p << "[" << idx
        << "] : (!llvm.ptr, i64) -> !llvm.ptr, i64\n";
    return r;
  }
  void store(const std::string& p, const std::string& v) {
    fn_ << "  llvm.store " << v << ", " << p << " : i64, !llvm.ptr\n";
  }
  std::string load(const std::string& p) {
    std::string r = nv();
    fn_ << "  " << r << " = llvm.load " << p << " : !llvm.ptr -> i64\n";
    return r;
  }
  void pin(const std::string& p) {
    fn_ << "  a.runtime.call \"a_pin_slot\"(" << p
        << ") {signature = \"(!llvm.ptr) -> ()\"} : (!llvm.ptr) -> ()\n";
  }
  std::string nullPtr() {
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.zero : !llvm.ptr\n";
    return r;
  }

  // ---- calls -------------------------------------------------------------------
  // `a.runtime.call` form: callee is the C symbol name (StrAttr); the
  // `signature` attr documents the LLVM-level signature (lowering infers the
  // call type from the already-lowered operand/result types).
  std::string runtimeSig(const std::string& args, const std::string& ret) {
    return "(" + args + ") -> " + ret;
  }

  // all-i64 args -> i64 result
  std::string callI64(const std::string& sym, const std::vector<std::string>& args) {
    std::string r = nv();
    std::string s = "  " + r + " = a.runtime.call \"" + sym + "\"(";
    std::string tys;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) { s += ", "; tys += ", "; }
      s += args[i];
      tys += "i64";
    }
    s += ") {signature = \"" + runtimeSig(tys, "i64")
         + "\"} : (" + tys + ") -> i64\n";
    fn_ << s;
    return r;
  }
  // typed args -> i64 result
  std::string callI64T(const std::string& sym,
                       const std::vector<std::pair<std::string, std::string>>& args) {
    std::string r = nv();
    std::string s = "  " + r + " = a.runtime.call \"" + sym + "\"(";
    std::string tys;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) { s += ", "; tys += ", "; }
      s += args[i].second;
      tys += args[i].first;
    }
    s += ") {signature = \"" + runtimeSig(tys, "i64")
         + "\"} : (" + tys + ") -> i64\n";
    fn_ << s;
    return r;
  }
  // typed args -> i32 result
  std::string callI32T(const std::string& sym,
                       const std::vector<std::pair<std::string, std::string>>& args) {
    std::string r = nv();
    std::string s = "  " + r + " = a.runtime.call \"" + sym + "\"(";
    std::string tys;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) { s += ", "; tys += ", "; }
      s += args[i].second;
      tys += args[i].first;
    }
    s += ") {signature = \"" + runtimeSig(tys, "i32")
         + "\"} : (" + tys + ") -> i32\n";
    fn_ << s;
    return r;
  }
  // pack aval args into [n x i64] argv array, return ptr
  std::string argvPack(const std::vector<std::string>& args) {
    int n = (int)args.size();
    std::string av = allocaArr(n > 0 ? n : 1);
    for (int i = 0; i < n; ++i) store(gepIdx(av, c64(i)), args[i]);
    return av;
  }
  // A-function call: argv[0]=env(kNone), argv[1..]=args
  std::string callAFunction(const std::string& cname,
                            const std::vector<std::string>& args) {
    int n = (int)args.size() + 1;
    std::string av = allocaArr(n);
    store(gepIdx(av, c64(0)), c64((std::int64_t)kAvalTag | 0x2));  // A_NONE env
    for (int i = 0; i < (int)args.size(); ++i)
      store(gepIdx(av, c64(i + 1)), args[i]);
    std::string r = nv();
    fn_ << "  " << r << " = llvm.call @" << cname << "(" << av
        << ") : (!llvm.ptr) -> i64\n";
    return r;
  }
  // builtin call: a_bi_call(id, argc, argv)
  std::string callBuiltin(int id, const std::vector<std::string>& args) {
    std::string av = argvPack(args);
    return callI64T("a_bi_call",
                    {{"i64", c64(id)}, {"i64", c64((std::int64_t)args.size())},
                     {"!llvm.ptr", av}});
  }
  // generic value call: a_call(fn, argc, argv)
  std::string callValue(const std::string& fnval, const std::vector<std::string>& args) {
    std::string av = argvPack(args);
    return callI64T("a_call",
                    {{"i64", fnval}, {"i64", c64((std::int64_t)args.size())},
                     {"!llvm.ptr", av}});
  }

  // ---- scope --------------------------------------------------------------------
  void pushScope() { scopes_.emplace_back(); dropStack_.emplace_back(); }
  void emitDropAt(const std::string& slot, const std::string& fn) {
    if (fn.empty() || failed_) return;
    std::string self;
    if (slot.rfind("g:", 0) == 0) {
      std::string r = nv();
      fn_ << "  " << r << " = llvm.mlir.addressof @" << slot.substr(2) << " : !llvm.ptr\n";
      self = load(r);
    } else self = load(slot);
    callAFunction(fn, {self});
  }
  void removeDrop(const std::string& slot) {
    for (auto& f : dropStack_)
      f.erase(std::remove_if(f.begin(), f.end(),
          [&](const std::pair<std::string,std::string>& r){ return r.first == slot; }), f.end());
  }
  void popScope() {
    if (!dropStack_.empty()) {
      auto& dr = dropStack_.back();
      for (auto it = dr.rbegin(); it != dr.rend(); ++it)
        emitDropAt(it->first, it->second);
      dr.clear();
    }
    dropStack_.pop_back();
    scopes_.pop_back();
  }
  // drops for all live frames (return paths)
  void emitAllDrops() {
    for (auto fi = dropStack_.rbegin(); fi != dropStack_.rend(); ++fi)
      for (auto it = fi->rbegin(); it != fi->rend(); ++it)
        emitDropAt(it->first, it->second);
    for (auto& f : dropStack_) f.clear();
  }
  MlBinding* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void bindLocal(const std::string& name, const std::string& aval) {
    std::string slot = allocaI64();
    store(slot, aval);
    MlBinding b;
    b.slot = slot;
    scopes_.back()[name] = b;
  }
  // global symbol for `name` visible from the current module, "" if none.
  // Globals are module-scoped: inside module M only M's top-level lets are
  // visible by bare name (cross-module access goes through mod.member).
  std::string globalSymFor(const std::string& name) {
    if (curModuleId_ >= 0) {
      auto mg = moduleGlobals_.find({curModuleId_, name});
      if (mg != moduleGlobals_.end()) return mg->second;
      return "";
    }
    auto g = globals_.find(name);
    return g != globals_.end() ? g->second : "";
  }
  // load a binding's current value as an aval i64 word. Typed bindings are
  // loaded unboxed then re-boxed (boxToAval), so every existing aval
  // consumer stays correct.
  std::string bindingValue(const std::string& name, bool& isGlobal) {
    TypedVal tv = bindingValueT(name, isGlobal);
    return tv.kind == "aval" ? tv.name : boxToAval(tv);
  }
  // store an aval i64 word into a binding; typed bindings decode it back to
  // their scalar kind first (unchecked: Sema + the assign-kind scan prove
  // the dynamic type).
  void storeBinding(const std::string& name, const std::string& val) {
    if (MlBinding* b = lookup(name)) {
      if (b->type != "aval") { storeTypedFromAval(*b, val); return; }
      store(b->slot, val);
      return;
    }
    std::string sym = globalSymFor(name);
    if (sym.empty()) {
      // auto-vivify: treat first write to an unbound name as a new global
      // (covers from-imported/module-level bindings the MVP doesn't model).
      sym = curModuleId_ >= 0
                ? "__a_g_" + std::to_string(curModuleId_) + "_" + name
                : "__a_g_" + name;
      if (curModuleId_ >= 0) moduleGlobals_[{curModuleId_, name}] = sym;
      globals_[name] = sym;
      rootSyms_.push_back(sym);
      pre_ << "  llvm.mlir.global @" << sym << "(0 : i64) : i64\n";
    }
    {
      std::string r = nv();
      fn_ << "  " << r << " = llvm.mlir.addressof @" << sym << " : !llvm.ptr\n";
      store(r, val);
    }
  }

  // ---- module/function collection ----------------------------------------------
  // integer literal (possibly unary-negated) for enum variant values
  bool evalEnumLit(const Expr& e, std::int64_t& out) {
    if (e.kind == ExprKind::IntLit) {
      out = static_cast<const IntLitExpr&>(e).value;
      return true;
    }
    if (e.kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(e);
      if (u.op == "-" && evalEnumLit(*u.operand, out)) { out = -out; return true; }
      if (u.op == "+" && evalEnumLit(*u.operand, out)) return true;
    }
    return false;
  }

  // modules in dependency order (imports before importers), matching
  // LlvmEmitter — top-level stmts/global inits must run in this order
  std::vector<const Module*> orderedModules() {
    std::vector<const Module*> out;
    std::vector<char> seen(graph_.modules.size(), 0);
    for (int id : graph_.order) {
      if (id >= 0 && id < (int)graph_.modules.size() && !seen[id]) {
        seen[id] = 1;
        out.push_back(&graph_.at(id));
      }
    }
    for (const Module& m : graph_.modules)
      if (m.id >= 0 && m.id < (int)seen.size() && !seen[m.id]) {
        seen[m.id] = 1;
        out.push_back(&m);
      }
    return out;
  }

  void collectTopLevel() {
    // Pre-pass: register every module's top-level fns so from-imports can
    // resolve members regardless of module visitation order.
    for (const Module* mp : orderedModules()) {
      const Module& mod = *mp;
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements)
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          if (!fn->isExtern) moduleFns_[{mod.id, fn->name}] = fn;
        }
    }
    for (const Module* mp : orderedModules()) {
      const Module& mod = *mp;
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          if (fn->isEntry) {
            if (entry_) { fail("multiple entry points"); return; }
            entry_ = fn;
          } else if (!fn->isExtern && fn->comptimeParams.empty()) {
            std::string cn = "__a_f_m" + std::to_string(mod.id) + "_" +
                             fn->name + "_" + std::to_string(fnDecls_.size());
            fnCnames_[fn->name] = cn;
            fnDeclCnames_[fn] = cn;
            fnByCname_[cn] = fn;
            fnDecls_.push_back({fn, mod.id});
            moduleFns_[{mod.id, fn->name}] = fn;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* ls = static_cast<const LetStmt*>(s.get());
          std::string gsym = "__a_g_" + std::to_string(mod.id) + "_" + ls->name;
          moduleGlobals_[{mod.id, ls->name}] = gsym;
          globals_[ls->name] = gsym;
          globalInits_.push_back({mod.id, ls});
        } else if (s->kind == StmtKind::Import) {
          const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
          if (im->pythonModuleImport || !im->pythonModule.empty()) {
            std::string alias = !im->alias.empty() ? im->alias
                             : (!im->bareName.empty() ? im->bareName : im->pythonModule);
            pythonAliases_[alias] = im->pythonModule;
            continue;
          }
          std::string modName = im->nativeModule ? im->bareName
                              : (!im->stdModule.empty() ? im->stdModule : "");
          // native module only when the spec actually exists; std:/bare
          // imports that resolve to A-source modules fall through to
          // aModuleAliases_ via moduleId.
          if (!modName.empty() && nativeModuleSpec(modName)) {
            std::string alias = !im->alias.empty() ? im->alias
                             : (!im->bareName.empty() ? im->bareName
                                                      : stemOf(im->path));
            nativeAliases_[alias] = modName;
            continue;
          }
          if (im->moduleId >= 0) {
            std::string alias = !im->alias.empty() ? im->alias
                             : (!im->bareName.empty() ? im->bareName
                                                      : stemOf(im->path));
            if (!alias.empty()) aModuleAliases_[alias] = im->moduleId;
          }
        } else if (s->kind == StmtKind::EnumDecl) {
          const EnumDecl* ed = static_cast<const EnumDecl*>(s.get());
          int tag = 0;
          std::int64_t next = 0;
          for (const EnumVariant& v : ed->variants) {
            MlVariant mv;
            mv.tag = tag++;
            mv.algebraic = ed->isAlgebraic();
            if (v.value) {
              std::int64_t ev = 0;
              if (!evalEnumLit(*v.value, ev)) {
                fail("enum variant value must be an integer literal");
                return;
              }
              mv.value = ev;
            } else {
              mv.value = next;
            }
            next = mv.value + 1;
            for (const FieldDecl& f : v.fields) mv.payloadFields.push_back(f.name);
            enumVariants_[ed->name + "." + v.name] = mv;
          }
        } else if (s->kind == StmtKind::StructDecl) {
          const StructDecl* sd = static_cast<const StructDecl*>(s.get());
          {
            MlStruct ms;
            std::string disp = sd->name;
            std::size_t sep = disp.rfind("::");
            if (sep != std::string::npos) disp = disp.substr(sep + 2);
            ms.displayName = disp;
            ms.ctorCname = "__a_ctor_" + std::to_string(structs_.size());
            for (const FieldDecl& f : sd->fields) ms.fieldNames.push_back(f.name);
            for (const auto& m : sd->methods) {
              std::string mc = "__a_m" + std::to_string(methodFns_.size());
              ms.methods.push_back({m->name, mc});
              methodFns_.push_back({m.get(), mc});
              methodDecls_[mc] = m.get();
            }
            ms.moduleId = mod.id;
            structByName_[sd->name] = structs_.size();
            structByName_[disp] = structs_.size();
            structByName_[shortName(sd->name)] = structs_.size();
            structByName_[shortName(disp)] = structs_.size();
            structCtors_[sd->name] = ms.ctorCname;
            structCtors_[disp] = ms.ctorCname;
            moduleStructCtors_[{mod.id, sd->name}] = ms.ctorCname;
            moduleStructCtors_[{mod.id, disp}] = ms.ctorCname;
            structs_.push_back(ms);
          }
        } else if (s->kind == StmtKind::ImplDecl) {
          const ImplDecl* impl = static_cast<const ImplDecl*>(s.get());
          pendingImpls_.push_back(impl);
          implModule_[impl] = mod.id;
        } else if (s->kind == StmtKind::FromImport) {
          const FromImportStmt* fi = static_cast<const FromImportStmt*>(s.get());
          if (fi->pythonModuleImport || !fi->pythonModule.empty()) {
            for (const auto& item : fi->items) {
              std::string local = !item.alias.empty() ? item.alias : item.name;
              pyMemberAliases_[local] = {fi->pythonModule, item.name};
            }
            continue;
          }
          std::string modName = fi->nativeModule ? fi->bareName
                              : (!fi->stdModule.empty() ? fi->stdModule : "");
          // native spec wins when it exists; std:/bare A-module imports fall
          // through to moduleFns_/re-export resolution below.
          if (!modName.empty() && nativeModuleSpec(modName)) {
            const NativeModuleSpec* spec = nativeModuleSpec(modName);
            for (const auto& item : fi->items) {
              std::string local = !item.alias.empty() ? item.alias : item.name;
              auto f = spec->functions.find(item.name);
              if (f != spec->functions.end()) builtinAliases_[local] = f->second;
              auto cn = spec->constants.find(item.name);
              if (cn != spec->constants.end()) constAliases_[local] = cn->second;
            }
            continue;
          }
          if (fi->moduleId >= 0 && !fi->nativeModule && fi->pythonModule.empty() &&
              !fi->pythonModuleImport) {
            for (const auto& item : fi->items) {
              std::string local = !item.alias.empty() ? item.alias : item.name;
              auto it = moduleFns_.find({fi->moduleId, item.name});
              if (getenv("A_DEBUG_MOD")) fprintf(stderr, "[mod] from-import mod=%d item=%s local=%s hit=%d\n", fi->moduleId, item.name.c_str(), local.c_str(), it!=moduleFns_.end()?1:0);
              if (it != moduleFns_.end()) aModuleFnAliases_[local] = it->second;
              moduleGlobalAliases_[local] = {fi->moduleId, item.name};
              // re-export: this module exposes `local` -> (fi->moduleId, item.name)
              moduleReexports_[mod.id][local] = {fi->moduleId, item.name};
            }
            continue;
          }
        } else {
          // Top-level `name = ...` assigns to an implicit global; pre-register
          // so functions emitted earlier can resolve it.
          if (s->kind == StmtKind::Assign) {
            const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
            if (as->target->kind == ExprKind::Ident && as->binop == BinOp::Invalid) {
              const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
              std::string gsym = "__a_g_" + std::to_string(mod.id) + "_" + nm;
              moduleGlobals_[{mod.id, nm}] = gsym;
              if (globals_.find(nm) == globals_.end()) globals_[nm] = gsym;
            }
          }
          if (s->kind != StmtKind::StructDecl && s->kind != StmtKind::EnumDecl &&
              s->kind != StmtKind::TraitDecl && s->kind != StmtKind::ImplDecl &&
              s->kind != StmtKind::TypeAlias) {
            topStmts_.push_back({mod.id, s.get()});
          }
        }
      }
    }
  }

  void emitModuleOpen() { pre_ << "module {\n"; }

  void emitDecls() {
    // (sig parsed below into llvm.func declarations)
    // The a_*_fast exports are DEFINED (not declared) via
    // fastpathmlir::kFastPathMLIR appended to body_ in run(); only their
    // *_slow fallbacks and helpers are declared here.
    static const std::pair<const char*, const char*> sigs[] = {
        {"a_add_slow", "(i64, i64) -> i64"}, {"a_sub_slow", "(i64, i64) -> i64"},
        {"a_mul_slow", "(i64, i64) -> i64"}, {"a_div_slow", "(i64, i64) -> i64"},
        {"a_floordiv_slow", "(i64, i64) -> i64"},
        {"a_mod_slow", "(i64, i64) -> i64"}, {"a_pow", "(i64, i64) -> i64"},
        {"a_eq_slow", "(i64, i64) -> i64"}, {"a_ne_slow", "(i64, i64) -> i64"},
        {"a_lt_slow", "(i64, i64) -> i64"}, {"a_le_slow", "(i64, i64) -> i64"},
        {"a_gt_slow", "(i64, i64) -> i64"}, {"a_ge_slow", "(i64, i64) -> i64"},
        {"a_band_slow", "(i64, i64) -> i64"}, {"a_bor_slow", "(i64, i64) -> i64"},
        {"a_bxor_slow", "(i64, i64) -> i64"}, {"a_shl_slow", "(i64, i64) -> i64"},
        {"a_shr_slow", "(i64, i64) -> i64"}, {"a_neg_slow", "(i64) -> i64"},
        {"a_bnot_slow", "(i64) -> i64"}, {"a_not", "(i64) -> i64"},
        {"a_truthy_raw_slow", "(i64) -> i64"},
        {"floor", "(f64) -> f64"}, {"fmod", "(f64, f64) -> f64"},
        {"llvm.sadd.with.overflow.i64",
         "(i64, i64) -> !llvm.struct<(i64, i1)>"},
        {"llvm.ssub.with.overflow.i64",
         "(i64, i64) -> !llvm.struct<(i64, i1)>"},
        {"llvm.smul.with.overflow.i64",
         "(i64, i64) -> !llvm.struct<(i64, i1)>"},
        {"a_unwrap", "(i64) -> i64"}, {"a_some_new", "(i64) -> i64"},
        {"a_try_result", "(i64) -> i64"}, {"a_is_option", "(i64) -> i32"},
        {"a_is_propagate", "(i64) -> i32"}, {"a_str", "(i64) -> i64"},
        {"a_repr", "(i64) -> i64"}, {"a_len", "(i64) -> i64"},
        {"a_int", "(i64) -> i64"}, {"a_float", "(i64) -> i64"},
        {"a_bool", "(i64) -> i64"}, {"a_type_name", "(i64) -> i64"},
        {"a_int_full", "(i64) -> i64"}, {"a_i64n", "(i64) -> i64"},
        {"a_str_new", "(!llvm.ptr, i64) -> i64"},
        {"a_char_new", "(!llvm.ptr, i64) -> i64"},
        {"a_str_concat", "(i64, i64) -> i64"},
        {"a_str_join", "(i64, !llvm.ptr) -> i64"},
        {"a_str_joinsep", "(i64, i64, !llvm.ptr) -> i64"},
        {"a_list_new", "(i64, !llvm.ptr) -> i64"},
        {"a_list_append", "(i64, i64) -> i64"},
        {"a_list_append_int", "(i64, i64) -> i64"},
        {"a_list_from_iter", "(i64) -> i64"},
        {"a_tuple_new", "(i64, !llvm.ptr) -> i64"},
        {"a_dict_new", "() -> i64"}, {"a_dict_set", "(i64, i64, i64) -> i64"},
        {"a_dict_get", "(i64, i64, i64) -> i64"},
        {"a_dict_has", "(i64, i64) -> i64"},
        {"a_set_new", "() -> i64"}, {"a_set_add", "(i64, i64) -> i64"},
        {"a_range_new", "(i64, i64, i64) -> i64"},
        {"a_range_bound", "(i64, i64) -> i64"},
        {"a_getitem", "(i64, i64) -> i64"},
        {"a_setitem", "(i64, i64, i64) -> i64"},
        {"a_slice", "(i64, i64, i64, i64) -> i64"},
        {"a_iter_new", "(i64) -> i64"},
        {"a_iter_next", "(i64, !llvm.ptr) -> i64"},
        {"a_contains", "(i64, i64) -> i64"},
        {"a_fnon_new", "(i64, i64, !llvm.ptr) -> i64"},
        {"a_closure_env_new", "(i64) -> i64"},
        {"a_env_new", "(i64) -> i64"}, {"a_env_get", "(i64, i64) -> i64"},
        {"a_env_set", "(i64, i64, i64) -> i64"},
        {"a_struct_new", "(i64, i64, i64) -> i64"},
        {"a_builtin", "(i64) -> i64"},
        {"a_bi_call", "(i64, i64, !llvm.ptr) -> i64"},
        {"a_call", "(i64, i64, !llvm.ptr) -> i64"},
        {"a_method", "(i64, !llvm.ptr, i64, i64, !llvm.ptr) -> i64"},
        {"a_getattr", "(i64, !llvm.ptr) -> i64"},
        {"a_setattr", "(i64, !llvm.ptr, i64) -> i64"},
        {"a_promote", "(i64) -> i64"}, {"a_panic_msg", "(!llvm.ptr) -> i64"},
        {"a_join_len", "(i64) -> i64"}, {"a_match_fail", "(i64) -> i64"},
        {"a_frame_push", "(!llvm.ptr, !llvm.ptr, i32) -> ()"}, {"a_frame_pop", "() -> ()"},
        {"a_pin_slot", "(!llvm.ptr) -> ()"}, {"a_runtime_init", "() -> ()"},
        {"a_fib_main", "(i64, i64, i64, !llvm.ptr) -> i64"},
        {"a_fib_spawn_handle", "(i64, i64, !llvm.ptr) -> i64"},
        {"a_fib_join_handle", "(i64) -> i64"},
        {"a_test_main", "(i64, !llvm.ptr, !llvm.ptr) -> i32"},
        {"a_printv", "(i64, !llvm.ptr) -> ()"},
        {"a_eprintv", "(i64, !llvm.ptr) -> ()"},
        {"a_println", "(i64) -> ()"}, {"a_sleep", "(i64) -> ()"},
        {"a_now", "() -> i64"}, {"a_gc_collect", "() -> ()"},
        {"a_gc_mark_aval", "(i64) -> ()"}, {"a_gc_add_root_cb", "(!llvm.ptr) -> ()"},
    };
    for (const auto& kv : sigs)
      pre_ << runtimeDeclText(kv.first, kv.second) << "\n";
    if (typed_) {
      // aval -> f64 decode for typed float slots (core_util.c num_to_double:
      // int avals convert, double avals bitcast, else panic).
      pre_ << "  llvm.func @num_to_double(%a0: i64) -> f64\n";
    }
  }

  void emitGlobals() {
    rootSyms_.clear();
    std::vector<std::string> syms;
    if (emitOnlyModule_ < 0) {
      // whole-graph: globals_ + moduleGlobals_ (dedup below)
      for (const auto& kv : globals_) syms.push_back(kv.second);
    }
    for (const auto& kv : moduleGlobals_) {
      // per-module mode: only this module's globals are defined here; other
      // modules' globals are declared by their own .ambc units (and by the
      // consumer's synthesized tail). globals_ is keyed by bare name across
      // the whole graph, so it is whole-graph-only.
      if (emitOnlyModule_ >= 0 && kv.first.first != emitOnlyModule_) continue;
      syms.push_back(kv.second);
    }
    std::sort(syms.begin(), syms.end());
    syms.erase(std::unique(syms.begin(), syms.end()), syms.end());
    rootSyms_ = syms;
    for (const std::string& sym : syms)
      pre_ << "  llvm.mlir.global @" << sym << "(0 : i64) : i64\n";
  }

  // GC roots: dedup and emit the marker fn (defined before main so the
  // registration call in main can reference it).
  void emitGcRoots() {
    std::sort(rootSyms_.begin(), rootSyms_.end());
    rootSyms_.erase(std::unique(rootSyms_.begin(), rootSyms_.end()),
                    rootSyms_.end());
    if (rootSyms_.empty()) return;
    body_ << "llvm.func @__a_gc_root_mark() -> () {\n";
    for (const std::string& sym : rootSyms_) {
      body_ << "  %r_" << sym << " = llvm.mlir.addressof @" << sym << " : !llvm.ptr\n";
      body_ << "  %v_" << sym << " = llvm.load %r_" << sym << " : !llvm.ptr -> i64\n";
      body_ << "  a.runtime.call \"a_gc_mark_aval\"(" << "%v_" << sym
             << ") {signature = \"(i64) -> ()\"} : (i64) -> ()\n";
    }
    body_ << "  llvm.return\n}\n\n";
  }

  // ---- functions ------------------------------------------------------------------
  void beginFunction(const std::string& sym, const FnDecl* fn) {
    curModuleId_ = fn->moduleId;
    curFnName_ = (fn && !fn->name.empty()) ? fn->name : sym;
    curFnFile_ = (fn && fn->moduleId >= 0 &&
                  fn->moduleId < (int)graph_.modules.size())
                     ? graph_.at(fn->moduleId).path : "";
    curFnLine_ = (fn && fn->span.line > 0) ? fn->span.line : 0;
    fn_.str("");
    fnAllocs_.str("");
    constCache_.clear();
    vCounter_ = 0;
    labelCounter_ = 0;
    scopes_.clear();
    breakLabels_.clear();
    contLabels_.clear();
    pendingDefers_.clear();
    terminated_ = false;
    inTypedCore_ = false;
    curFnFrameFree_ = false;
    // typed-binding safety scan runs on the first emitStatements of this fn
    assignScanPending_ = true;
    assignKindMask_.clear();
    // nullPtr uses a function-local cache; reset by scoping it per call site
    pushScope();
    fn_ << "llvm.func @" << sym << "(%argv: !llvm.ptr) -> i64 {\n";
    // Run the assign-kind scan before binding params so canTypeBind sees
    // reassignments of annotated params (scan is idempotent; emitStatements
    // re-runs it on the same body).
    if (typed_ && fn && assignScanPending_) scanAssignKinds(fn->body);
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const ParamDecl& pr = fn->params[i];
      std::string aval = load(gepIdx("%argv", c64((std::int64_t)i + 1)));
      // Explicitly annotated params with no default may bind a typed slot
      // (unannotated params stay dynamically rebindable aval; defaults need
      // the A_MISSING sentinel which typed slots cannot represent).
      std::string pkind;
      if (typed_ && pr.type && !pr.defaultValue && !pr.variadic && fn) {
        auto it = sema_.fnParamTypes.find(fn);
        if (it != sema_.fnParamTypes.end() && i < it->second.size())
          pkind = scalarKindOfT(it->second[i]);
      }
      if (!pkind.empty() && canTypeBind(pr.name, pkind)) {
        MlBinding b;
        b.type = pkind;
        if (pkind == "f64") b.slot = allocaF64();
        else b.slot = allocaI64();
        storeTypedVal(b, unboxFromAval(aval, pkind));
        scopes_.back()[pr.name] = b;
        continue;
      }
      std::string slot = allocaI64();
      store(slot, aval);
      MlBinding b;
      b.slot = slot;
      scopes_.back()[pr.name] = b;
    }
    // defaults: param == A_MISSING -> evaluate default in callee scope
    // (defaults may reference earlier params; processed in order).
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const ParamDecl& pr = fn->params[i];
      if (!pr.defaultValue) continue;
      auto it = scopes_.back().find(pr.name);
      if (it == scopes_.back().end()) continue;
      std::string cur = load(it->second.slot);
      std::string km = c64(kAvalTag);
      std::string isMiss = nv();
      fn_ << "  " << isMiss << " = llvm.icmp \"eq\" " << cur << ", " << km << " : i64\n";
      std::string ldef = nl(), lskip = nl(), ldm = nl();
      fn_ << "  llvm.cond_br " << isMiss << ", ^" << ldef << ", ^" << lskip << "\n";
      fn_ << "^" << ldef << ":\n";
      terminated_ = false;
      pushScope();
      {
        std::string dv = evalExpr(*pr.defaultValue);
        store(it->second.slot, dv);
      }
      popScope();
      if (!terminated_) fn_ << "  llvm.br ^" << ldm << "\n";
      fn_ << "^" << lskip << ":\n";
      terminated_ = false;
      fn_ << "  llvm.br ^" << ldm << "\n";
      fn_ << "^" << ldm << ":\n";
      terminated_ = false;
    }
    emitFramePush();
  }
  // Emits a copy of the deferred stmts (LIFO). Does NOT clear: every return
  // site needs its own copy; the registry lives for the whole function.
  void emitPendingDefers() {
    for (auto it = pendingDefers_.rbegin(); it != pendingDefers_.rend(); ++it)
      emitStmt(**it);
  }
  // call-frame tracking for panic tracebacks + stack-overflow detection
  // (mirrors LlvmEmitter::emitFramePush/emitFramePop).
  void emitFramePush() {
    if (curFnFrameFree_) return;
    std::string np = cstrPtr(curFnName_);
    std::string fp = cstrPtr(curFnFile_);
    std::string lp = nv();
    fn_ << "  " << lp << " = llvm.mlir.constant(" << curFnLine_
        << " : i32) : i32\n";
    fn_ << "  a.runtime.call \"a_frame_push\"(" << np << ", " << fp << ", "
        << lp << ") {signature = \"(!llvm.ptr, !llvm.ptr, i32) -> ()\"}"
        << " : (!llvm.ptr, !llvm.ptr, i32) -> ()\n";
  }
  void emitFramePop() {
    if (suppressFramePop_ || curFnFrameFree_) return;
    fn_ << "  a.runtime.call \"a_frame_pop\"() {signature = \"() -> ()\"}"
        << " : () -> ()\n";
  }
  void emitReturn(const std::string& v) {
    emitFramePop();
    fn_ << "  llvm.return " << v << " : i64\n";
  }

  void endFunction() {
    if (!terminated_) {
      emitPendingDefers();
      emitAllDrops();
      std::string k = c64(kAvalTag | 0x2);
      emitReturn(k);
    }
    fn_ << "}\n\n";
    flushFunction();
  }

  // ---- typed-call ABI ---------------------------------------------------------
  // Fns whose params and return are all Sema-proven scalars get a second
  // entry point `__a_tc_<cname>` with a raw-scalar signature; direct calls
  // with matching arity emit `llvm.call` straight to it, skipping the argv
  // pack/unpack and per-arg box/unbox round-trips. The original `<cname>`
  // symbol remains as a thin boxed-ABI thunk so fn values, indirect calls,
  // and non-typed call sites keep working unchanged.

  static const char* kindLLVMType(const std::string& k) {
    return k == "f64" ? "f64" : k == "i1" ? "i1" : "i64";
  }

  // Recursive bare-`return` scan: a typed core returns a raw scalar and
  // cannot produce the A_NONE aval a bare `return` yields, so such fns
  // must keep the boxed ABI. Nested FnDecls have their own returns.
  bool stmtsHaveBareReturn(const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& s : stmts)
      if (s && stmtHasBareReturn(*s)) return true;
    return false;
  }
  bool stmtHasBareReturn(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Return:
        return !static_cast<const ReturnStmt&>(s).value;
      case StmtKind::FnDecl:
        return false;
      case StmtKind::If: {
        const IfStmt& is = static_cast<const IfStmt&>(s);
        for (const auto& br : is.branches)
          if (stmtsHaveBareReturn(br.body)) return true;
        return stmtsHaveBareReturn(is.elseBody);
      }
      case StmtKind::While:
        return stmtsHaveBareReturn(static_cast<const WhileStmt&>(s).body);
      case StmtKind::DoWhile:
        return stmtsHaveBareReturn(static_cast<const DoWhileStmt&>(s).body);
      case StmtKind::For:
        return stmtsHaveBareReturn(static_cast<const ForStmt&>(s).body);
      case StmtKind::Loop:
        return stmtsHaveBareReturn(static_cast<const LoopStmt&>(s).body);
      case StmtKind::Defer: {
        const DeferStmt& ds = static_cast<const DeferStmt&>(s);
        return ds.body && stmtHasBareReturn(*ds.body);
      }
      default:
        return false;
    }
  }

  bool typedCoreEligible(const FnDecl* fn) {
    if (!typed_ || !fn || fn->isExtern || fn->isEntry) return false;
    if (!fn->typeParams.empty() || !fn->comptimeParams.empty() ||
        !fn->whereClause.empty())
      return false;
    auto pit = sema_.fnParamTypes.find(fn);
    auto rit = sema_.fnReturnTypes.find(fn);
    if (pit == sema_.fnParamTypes.end() || rit == sema_.fnReturnTypes.end())
      return false;
    if (pit->second.size() != fn->params.size()) return false;
    if (scalarKindOfT(rit->second).empty()) return false;
    if (stmtsHaveBareReturn(fn->body)) return false;
    // Params must be explicitly annotated: unannotated params accept any
    // runtime type (Sema's inferred scalar kind is not a guarantee — e.g.
    // `fn id(x): return x` called with a string). This mirrors the boxed
    // prologue's pr.type requirement. canTypeBind additionally requires
    // every reassign in the body to keep the same scalar kind.
    // The mask scan runs on a saved/restored assignKindMask_ because this
    // check can fire mid-emission of an unrelated function (tryTypedCall).
    auto savedMask = std::move(assignKindMask_);
    assignKindMask_.clear();
    scanAssignKinds(fn->body);
    bool ok = true;
    for (size_t i = 0; i < fn->params.size() && ok; ++i) {
      const ParamDecl& pr = fn->params[i];
      std::string pk = scalarKindOfT(pit->second[i]);
      if (!pr.type || pr.defaultValue || pr.variadic || pk.empty() ||
          !canTypeBind(pr.name, pk))
        ok = false;
    }
    assignKindMask_ = std::move(savedMask);
    return ok;
  }
  int typedCoreStatus(const FnDecl* fn) {
    auto it = typedCoreCache_.find(fn);
    if (it != typedCoreCache_.end()) return it->second;
    int v = typedCoreEligible(fn) ? 1 : -1;
    typedCoreCache_[fn] = v;
    return v;
  }
  static std::string typedCoreSym(const std::string& cname) {
    return "__a_tc_" + cname;
  }

  // Resolve a syntactically direct call to its FnDecl (mirrors tryTypedCall).
  // Indirect calls (function values, lambdas, builtins) never qualify.
  const FnDecl* resolveCallFn(const CallExpr& c) const {
    if (c.typeArgs.size() || c.calleeIsBuiltin ||
        c.builtinId != BuiltinId::None)
      return nullptr;
    if (c.calleeFunction) return c.calleeFunction;
    if (!c.callee || c.callee->kind != ExprKind::Ident) return nullptr;
    const std::string& name = static_cast<const IdentExpr&>(*c.callee).name;
    if (name == "cast") return nullptr;
    auto fn = fnCnames_.find(name);
    if (fn == fnCnames_.end()) return nullptr;
    auto fit = fnByCname_.find(fn->second);
    return fit == fnByCname_.end() ? nullptr : fit->second;
  }

  // Conservative "no side effects that need a frame" scan. Only expressions
  // built from literals, locals, non-trapping scalar operators, and calls to
  // other frame-free user functions qualify; anything that can call out to
  // the runtime, allocate, or trap keeps its frame hooks. Calls to unknown /
  // indirect targets conservatively keep frames too.
  bool exprFrameFree(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
      case ExprKind::Ident:
        return true;
      case ExprKind::StringLit:
        return false;  // interpolation may call repr/format helpers
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        return exprFrameFree(*u.operand);
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        // Div/Mod/Pow/FloorDiv route through *_slow and can panic; string
        // '+' can allocate. Only the plain arithmetic/bitwise/comparison
        // ops stay allocation- and trap-free in the typed path.
        switch (b.binop) {
          case BinOp::Div: case BinOp::FloorDiv: case BinOp::Mod:
          case BinOp::Pow: case BinOp::Coalesce:
            return false;
          default:
            break;
        }
        return exprFrameFree(*b.lhs) && exprFrameFree(*b.rhs);
      }
      case ExprKind::Ternary: {
        const auto& t = static_cast<const TernaryExpr&>(e);
        return exprFrameFree(*t.cond) && exprFrameFree(*t.thenValue) &&
               exprFrameFree(*t.elseValue);
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        const FnDecl* callee = resolveCallFn(c);
        if (!callee || !fnFrameFree(callee)) return false;
        for (const CallArg& a : c.args)
          if (!a.value || !exprFrameFree(*a.value)) return false;
        return true;
      }
      default:
        // Index, Slice, Field, Range, ArrayLit, Tuple, MapLit, SetLit,
        // Lambda, Comprehension, Match, Try, SafeNav can call/allocate/trap.
        return false;
    }
  }
  bool stmtsFrameFree(const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& sp : stmts) {
      if (!sp) continue;
      const Stmt& s = *sp;
      switch (s.kind) {
        case StmtKind::Let: {
          const auto& l = static_cast<const LetStmt&>(s);
          if (l.init && !exprFrameFree(*l.init)) return false;
          break;
        }
        case StmtKind::Assign: {
          const auto& a = static_cast<const AssignStmt&>(s);
          if (a.value && !exprFrameFree(*a.value)) return false;
          break;
        }
        case StmtKind::Expr: {
          const auto& x = static_cast<const ExprStmt&>(s);
          if (x.expr && !exprFrameFree(*x.expr)) return false;
          break;
        }
        case StmtKind::Return: {
          const auto& r = static_cast<const ReturnStmt&>(s);
          if (r.value && !exprFrameFree(*r.value)) return false;
          break;
        }
        case StmtKind::If: {
          const auto& i = static_cast<const IfStmt&>(s);
          for (const auto& br : i.branches) {
            if (br.cond && !exprFrameFree(*br.cond)) return false;
            if (!stmtsFrameFree(br.body)) return false;
          }
          if (!stmtsFrameFree(i.elseBody)) return false;
          break;
        }
        case StmtKind::While: {
          const auto& w = static_cast<const WhileStmt&>(s);
          if (w.cond && !exprFrameFree(*w.cond)) return false;
          if (!stmtsFrameFree(w.body)) return false;
          break;
        }
        case StmtKind::DoWhile: {
          const auto& w = static_cast<const DoWhileStmt&>(s);
          if (w.cond && !exprFrameFree(*w.cond)) return false;
          if (!stmtsFrameFree(w.body)) return false;
          break;
        }
        case StmtKind::For: {
          // for-loops over a scalar range emit a_range_bound (no allocation,
          // no trap) plus the body. Any other iterable may call/allocate.
          const auto& f = static_cast<const ForStmt&>(s);
          if (f.iterable) {
            if (f.iterable->kind == ExprKind::Range) {
              const auto& rg = static_cast<const RangeExpr&>(*f.iterable);
              if ((rg.start && !exprFrameFree(*rg.start)) ||
                  (rg.end && !exprFrameFree(*rg.end)) ||
                  (rg.step && !exprFrameFree(*rg.step)))
                return false;
            } else if (!exprFrameFree(*f.iterable)) {
              return false;
            }
          }
          if (!stmtsFrameFree(f.body)) return false;
          break;
        }
        case StmtKind::Loop: {
          const auto& l = static_cast<const LoopStmt&>(s);
          if (!stmtsFrameFree(l.body)) return false;
          break;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
          break;
        default:
          // Defer, nested fn/struct decls, imports, Destructure: keep frames.
          return false;
      }
    }
    return true;
  }
  // Memoized fixpoint over the direct-call graph. Functions in the current
  // walk are assumed frame-free so direct/mutual recursion terminates; a
  // cycle can only contain functions that otherwise qualify, and none of
  // them allocates or calls out to a frame-requiring target.
  bool fnFrameFree(const FnDecl* fn) {
    if (!fn) return false;
    auto it = frameFreeMemo_.find(fn);
    if (it != frameFreeMemo_.end()) return it->second;
    if (frameFreeVisiting_.count(fn)) return true;
    frameFreeVisiting_.insert(fn);
    bool ok = stmtsFrameFree(fn->body);
    frameFreeVisiting_.erase(fn);
    frameFreeMemo_[fn] = ok;
    return ok;
  }

  // beginFunction variant for the typed core: params arrive as unboxed
  // scalar SSA args (bound directly into typed slots), returns are raw.
  void beginFunctionTyped(const std::string& sym, const FnDecl* fn,
                          const std::vector<std::string>& pkinds,
                          const std::string& retkind) {
    curModuleId_ = fn->moduleId;
    curFnName_ = (!fn->name.empty()) ? fn->name : sym;
    curFnFile_ = (fn->moduleId >= 0 && fn->moduleId < (int)graph_.modules.size())
                     ? graph_.at(fn->moduleId).path
                     : "";
    curFnLine_ = fn->span.line > 0 ? fn->span.line : 0;
    fn_.str("");
    fnAllocs_.str("");
    constCache_.clear();
    vCounter_ = 0;
    labelCounter_ = 0;
    scopes_.clear();
    breakLabels_.clear();
    contLabels_.clear();
    pendingDefers_.clear();
    terminated_ = false;
    assignScanPending_ = true;
    assignKindMask_.clear();
    inTypedCore_ = true;
    // Offload cores never touch the host runtime: no frame hooks regardless
    // of the body scan (the scan only gates the host fast path).
    curFnFrameFree_ =
        typed_ && (fnFrameFree(fn) || !curOffloadAttr_.empty());
    typedCoreRetKind_ = retkind;
    pushScope();
    fn_ << "llvm.func @" << sym << "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
      if (i) fn_ << ", ";
      fn_ << "%p" << i << ": " << kindLLVMType(pkinds[i]);
    }
    fn_ << ") -> " << kindLLVMType(retkind);
    if (!curOffloadAttr_.empty())
      fn_ << " attributes {a.offload = \"" << curOffloadAttr_ << "\"}";
    fn_ << " {\n";
    if (fn) scanAssignKinds(fn->body);
    assignScanPending_ = false;
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const ParamDecl& pr = fn->params[i];
      const std::string& pk = pkinds[i];
      MlBinding b;
      b.type = pk;
      b.slot = (pk == "f64") ? allocaF64() : allocaI64();
      storeTypedVal(b, TypedVal{"%p" + std::to_string(i), pk});
      scopes_.back()[pr.name] = b;
    }
    emitFramePush();
  }

  // Return inside a typed core: coerce the expr's TypedVal to the declared
  // scalar return kind (aval/other-kind values round-trip through the boxed
  // representation so the callee-side unbox sees the same value the boxed
  // ABI would have carried).
  void emitReturnTyped(const TypedVal& tv) {
    emitFramePop();
    const std::string& rk = typedCoreRetKind_;
    std::string val;
    if (tv.isConst && tv.kind == rk) {
      val = tvalSSA(tv);
    } else if (tv.kind == rk && !tv.name.empty()) {
      val = tvalSSA(tv);
    } else if (tv.kind == "aval" && !tv.name.empty()) {
      val = tvalSSA(unboxFromAval(tv.name, rk));
    } else {
      // unreachable in practice (eligibility rejects bare returns); emit a
      // zero rather than malformed IR if it ever slips through.
      TypedVal z{"", rk, true, 0, 0.0};
      val = tvalSSA(z);
    }
    fn_ << "  llvm.return " << val << " : " << kindLLVMType(rk) << "\n";
  }

  void endFunctionTyped() {
    if (!terminated_) {
      emitPendingDefers();
      emitAllDrops();
      TypedVal z{"", typedCoreRetKind_, true, 0, 0.0};
      emitReturnTyped(z);
    }
    fn_ << "}\n\n";
    flushFunction();
    inTypedCore_ = false;
    curFnFrameFree_ = false;
  }

  // Boxed-ABI thunk for a typed core: unboxes each argv param, calls the
  // core, boxes the scalar result. No frame of its own — the core owns the
  // call frame, so traceback depth matches the old single-function shape.
  void emitTypedThunk(const std::string& cname, const FnDecl* fn,
                      const std::vector<std::string>& pkinds,
                      const std::string& retkind,
                      const std::string& core) {
    fn_.str("");
    fnAllocs_.str("");
    constCache_.clear();
    vCounter_ = 0;
    labelCounter_ = 0;
    fn_ << "llvm.func @" << cname << "(%argv: !llvm.ptr) -> i64 {\n";
    std::vector<std::string> argssa;
    for (size_t i = 0; i < fn->params.size(); ++i) {
      std::string av = load(gepIdx("%argv", c64((std::int64_t)i + 1)));
      TypedVal tv = unboxFromAval(av, pkinds[i]);
      argssa.push_back(tvalSSA(tv));
    }
    std::string csig;
    for (size_t i = 0; i < pkinds.size(); ++i) {
      if (i) csig += ", ";
      csig += kindLLVMType(pkinds[i]);
    }
    std::string r = nv();
    fn_ << "  " << r << " = llvm.call @" << core << "(";
    for (size_t i = 0; i < argssa.size(); ++i) {
      if (i) fn_ << ", ";
      fn_ << argssa[i];
    }
    fn_ << ") : (" << csig << ") -> " << kindLLVMType(retkind) << "\n";
    std::string boxed = boxToAval(TypedVal{r, retkind});
    fn_ << "  llvm.return " << boxed << " : i64\n";
    fn_ << "}\n\n";
    body_ << fn_.str();
  }

  void emitTypedCoreAndThunk(const FnDecl* fn) {
    const std::string& cname = fnDeclCnames_[fn];
    auto pit = sema_.fnParamTypes.find(fn);
    std::vector<std::string> pkinds;
    for (size_t i = 0; i < fn->params.size(); ++i)
      pkinds.push_back(scalarKindOfT(pit->second[i]));
    const std::string retkind = scalarKindOfT(sema_.fnReturnTypes.at(fn));
    const std::string core = typedCoreSym(cname);
    beginFunctionTyped(core, fn, pkinds, retkind);
    bool sv = inFnBody_; inFnBody_ = true;
    bool suv = inUserFn_; inUserFn_ = true;
    emitStatements(fn->body);
    inUserFn_ = suv; inFnBody_ = sv;
    endFunctionTyped();
    if (failed_) return;
    emitTypedThunk(cname, fn, pkinds, retkind, core);
  }

  // kernel/device fn: emit ONLY the typed core (unboxed scalar signature)
  // stamped with `a.offload` — no boxed thunk (host code cannot call it) and
  // no frame hooks. The MLIR backend slices these funcs into a device module
  // compiled for the GPU target (Mojo-style compile_offload).
  void emitOffloadCore(const FnDecl* fn) {
    if (typedCoreStatus(fn) != 1) {
      failAt("'" + fn->name +
                 "' cannot be a kernel/device function: every parameter "
                 "and the return type must be explicitly annotated scalar "
                 "types (int/float/bool).",
             fn);
      return;
    }
    if (!fnFrameFree(fn)) {
      failAt("'" + fn->name +
                 "' cannot be a kernel/device function: its body uses "
                 "operations that require the host runtime.",
             fn);
      return;
    }
    const std::string& cname = fnDeclCnames_[fn];
    auto pit = sema_.fnParamTypes.find(fn);
    std::vector<std::string> pkinds;
    for (size_t i = 0; i < fn->params.size(); ++i)
      pkinds.push_back(scalarKindOfT(pit->second[i]));
    const std::string retkind = scalarKindOfT(sema_.fnReturnTypes.at(fn));
    const std::string core = typedCoreSym(cname);
    curOffloadAttr_ = fn->offload == FnDecl::OffloadKind::Kernel ? "kernel"
                                                                 : "device";
    beginFunctionTyped(core, fn, pkinds, retkind);
    bool sv = inFnBody_; inFnBody_ = true;
    bool suv = inUserFn_; inUserFn_ = true;
    emitStatements(fn->body);
    inUserFn_ = suv; inFnBody_ = sv;
    endFunctionTyped();
    curOffloadAttr_.clear();
  }

  // splice hoisted allocas into the entry block (right after the func
  // header) — allocas emitted mid-loop would allocate fresh stack every
  // iteration since their address escapes into runtime calls.
  void flushFunction() {
    std::string body = fn_.str();
    std::string allocs = fnAllocs_.str();
    if (!allocs.empty()) {
      auto nl = body.find('\n');
      if (nl != std::string::npos)
        body.insert(nl + 1, allocs);
      else
        body += allocs;
    }
    body_ << body;
  }

  void emitFunctions() {
    if (getenv("A_DEBUG_MOD")) {
      for (auto& kv : moduleFns_)
        if (kv.first.second.find("prod") != std::string::npos)
          fprintf(stderr, "[modfns] mod=%d fn=%s\n", kv.first.first, kv.first.second.c_str());
    }
    for (auto& kv : fnDecls_) {
      if (emitOnlyModule_ >= 0 && kv.second != emitOnlyModule_) continue;
      const FnDecl* fn = kv.first;
      if (fn->offload != FnDecl::OffloadKind::None) {
        emitOffloadCore(fn);
        if (failed_) return;
        continue;
      }
      if (typedCoreStatus(fn) == 1) {
        emitTypedCoreAndThunk(fn);
        if (failed_) return;
        continue;
      }
      beginFunction(fnDeclCnames_[fn], fn);
      bool sv = inFnBody_; inFnBody_ = true;
      bool suv = inUserFn_; inUserFn_ = true;
      emitStatements(fn->body);
      inUserFn_ = suv; inFnBody_ = sv;
      endFunction();
      if (failed_) return;
    }
    for (auto& mf : methodFns_) {
      // Ownership: struct methods carry moduleId from Sema; impl methods keep
      // -1 and are attributed via implMethodModule_ (the module holding the
      // `impl` block). Unattributed methods fall back to the requested module
      // so they are never silently dropped in split mode.
      int owner = mf.first->moduleId;
      if (owner < 0) {
        auto im = implMethodModule_.find(mf.first);
        owner = im != implMethodModule_.end() ? im->second : emitOnlyModule_;
      }
      if (emitOnlyModule_ >= 0 && owner != emitOnlyModule_) continue;
      beginFunction(mf.second, mf.first);
      bool sv = inFnBody_; inFnBody_ = true;
      bool suv = inUserFn_; inUserFn_ = true;
      emitStatements(mf.first->body);
      inUserFn_ = suv; inFnBody_ = sv;
      endFunction();
      if (failed_) return;
    }
  }

  void resolveImpls() {
    for (const ImplDecl* im : pendingImpls_) {
      int implMod = implModule_.count(im) ? implModule_[im] : -1;
      if (!im->traitName.empty() && im->traitName != im->targetName) continue;
      for (auto& ms : structs_) {
        bool match = ms.displayName == im->targetName ||
            (im->targetName.size() >= ms.displayName.size() &&
             im->targetName.compare(im->targetName.size() - ms.displayName.size(),
                                    ms.displayName.size(), ms.displayName) == 0);
        if (!match) continue;
        for (const auto& m : im->methods) {
          std::string mc = "__a_m" + std::to_string(methodFns_.size());
          ms.methods.push_back({m->name, mc});
          methodFns_.push_back({m.get(), mc});
          methodDecls_[mc] = m.get();
          if (implMod >= 0) implMethodModule_[m.get()] = implMod;
        }
        break;
      }
    }
  }

  // struct ctor: name/methods/fns dict -> a_struct_new -> fields from argv.
  // Mirrors LlvmEmitter::emitStructCtorBody (methods dict empty in v1).
  void emitStructCtors() {
    for (const MlStruct& ms : structs_) {
      if (emitOnlyModule_ >= 0 && ms.moduleId != emitOnlyModule_) continue;
      FnDecl dummy;
      beginFunction(ms.ctorCname, &dummy);
      std::string nameV = stringLiteralAval(ms.displayName);
      std::string methL = callI64T("a_list_new", {{"i64", c64(0)}, {"!llvm.ptr", nullPtr()}});
      std::string fnsD = callI64("a_dict_new", {});
      for (const auto& m : ms.methods) {
        // method name str -> fnon(fnptr, env=None, name)
        std::string mn = stringLiteralAval(m.first);
        callI64("a_list_append", {methL, mn});
        std::string fnptr = nv();
        { std::string r = nv();
          fn_ << "  " << r << " = llvm.mlir.addressof @" << m.second << " : !llvm.ptr\n";
          fnptr = nv();
          fn_ << "  " << fnptr << " = llvm.ptrtoint " << r << " : !llvm.ptr to i64\n"; }
        std::string envN = c64(kAvalTag | 0x2);
        std::string nmC = cstrPtr(m.first);
        std::string fv = callI64T("a_fnon_new",
            {{"i64", fnptr}, {"i64", envN}, {"!llvm.ptr", nmC}});
        callI64("a_dict_set", {fnsD, mn, fv});
      }
      std::string obj = callI64("a_struct_new", {nameV, methL, fnsD});
      for (size_t i = 0; i < ms.fieldNames.size(); ++i) {
        std::string fv = load(gepIdx("%argv", c64((std::int64_t)i + 1)));
        std::string ks = stringLiteralAval(ms.fieldNames[i]);
        callI64("a_dict_set", {obj, ks, fv});
      }
      callI64("a_promote", {obj});
      emitReturn(obj);
      fn_ << "}\n\n";
      flushFunction();
      if (failed_) return;
    }
  }

  void emitMainBody() {
    // __a_main_body: global inits + top-level stmts + entry body, in module
    // order. Uses a dummy FnDecl (main_body takes only the argv pointer).
    FnDecl dummy;
    beginFunction("__a_main_body", &dummy);
    // __a_main_body spans every module's top-level stmts plus the entry
    // body; the dummy FnDecl's empty body means beginFunction's assign-kind
    // scan saw nothing, and a nested emitStatements would otherwise consume
    // the pending scan on a partial body. Scan everything up front.
    assignScanPending_ = false;
    for (const auto& ts : topStmts_) scanStmtAssigns(*ts.stmt);
    if (entry_) scanAssignKinds(entry_->body);
    // global init stores + top-level stmts interleaved per module order:
    // emit inits first (globals must exist before use), then top stmts.
    for (const auto& gi : globalInits_) {
      int savedMod = curModuleId_;
      curModuleId_ = gi.moduleId;
      if (gi.stmt->init) {
        std::string v = evalExpr(*gi.stmt->init);
        std::string g = globalSymFor(gi.stmt->name);
        if (g.empty()) g = globals_[gi.stmt->name];
        std::string r = nv();
        fn_ << "  " << r << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
        store(r, v);
      }
      curModuleId_ = savedMod;
      if (failed_) return;
    }
    for (const auto& ts : topStmts_) {
      int savedMod = curModuleId_;
      curModuleId_ = ts.moduleId;
      emitStmt(*ts.stmt);
      curModuleId_ = savedMod;
      if (failed_) return;
    }
    // test mode runs module inits only; entry body is skipped (mirrors
    // LlvmEmitter, where emitMainBody is not emitted under a_test_main).
    if (entry_ && !testMode_) {
      bool svf = inFnBody_; inFnBody_ = true;
      bool sve = inEntryBody_; inEntryBody_ = true;
      int svm = curModuleId_; curModuleId_ = entry_->moduleId;
      emitStatements(entry_->body);
      curModuleId_ = svm; inFnBody_ = svf; inEntryBody_ = sve;
    }
    endFunction();
  }

  void emitMain() {
    if (testMode_) { emitTestMain(); return; }
    body_ << "llvm.func @main(%argc: i32, %argv0: !llvm.ptr) -> i32 {\n";
    // a_runtime_init()
    body_ << "  a.runtime.call \"a_runtime_init\"() {signature = \"() -> ()\"}"
          << " : () -> ()\n";
    if (!rootSyms_.empty()) {
      // register GC root callback marking all module globals
      body_ << "  %gr0 = llvm.mlir.addressof @__a_gc_root_mark : !llvm.ptr\n";
      body_ << "  a.runtime.call \"a_gc_add_root_cb\"(%gr0)"
            << " {signature = \"(!llvm.ptr) -> ()\"} : (!llvm.ptr) -> ()\n";
    }
    // %fnp = ptrtoint addressof __a_main_body
    body_ << "  %fnp0 = llvm.mlir.addressof @__a_main_body : !llvm.ptr\n";
    body_ << "  %fnp1 = llvm.ptrtoint %fnp0 : !llvm.ptr to i64\n";
    body_ << "  %cnone = llvm.mlir.constant(-281474976710654 : i64) : i64\n";
    body_ << "  %czero = llvm.mlir.constant(0 : i64) : i64\n";
    body_ << "  %nullp = llvm.mlir.zero : !llvm.ptr\n";
    body_ << "  %r = a.runtime.call \"a_fib_main\"(%fnp1, %cnone, %czero, %nullp)"
             " {signature = \"(i64, i64, i64, !llvm.ptr) -> i64\"}"
             " : (i64, i64, i64, !llvm.ptr) -> i64\n";
    body_ << "  %c0 = llvm.mlir.constant(0 : i32) : i32\n";
    body_ << "  llvm.return %c0 : i32\n";
    body_ << "}\n\n";
  }

  // Append only the a_*_fast fastpath defs this module references. Calls
  // reach them as a.runtime.call "sym" / llvm.call @sym, so a substring
  // scan of the emitted text finds every needed def; fragments are
  // independent (each only calls *_slow/floor/fmod/a_pow, all declared).
  void emitUsedFastPaths() {
    const std::string body = body_.str();
    for (const FastPathFrag& f : fastPathFrags()) {
      if (body.find(f.name) != std::string::npos) body_ << f.text;
    }
  }

  // Test-mode main: run module inits (via __a_main_body on a fiber, same as
  // normal mode), then hand the entry module's `test_*` fns to the runtime
  // harness a_test_main, which forks a child per test. Mirrors
  // LlvmEmitter::emitTestMain.
  void emitTestMain() {
    body_ << "llvm.func @main(%argc: i32, %argv0: !llvm.ptr) -> i32 {\n";
    body_ << "  a.runtime.call \"a_runtime_init\"() {signature = \"() -> ()\"}"
          << " : () -> ()\n";
    body_ << "  %fnp0 = llvm.mlir.addressof @__a_main_body : !llvm.ptr\n";
    body_ << "  %fnp1 = llvm.ptrtoint %fnp0 : !llvm.ptr to i64\n";
    body_ << "  %cnone = llvm.mlir.constant(-281474976710654 : i64) : i64\n";
    body_ << "  %czero = llvm.mlir.constant(0 : i64) : i64\n";
    body_ << "  %nullp = llvm.mlir.zero : !llvm.ptr\n";
    body_ << "  %rinit = a.runtime.call \"a_fib_main\"(%fnp1, %cnone, %czero, %nullp)"
             " {signature = \"(i64, i64, i64, !llvm.ptr) -> i64\"}"
             " : (i64, i64, i64, !llvm.ptr) -> i64\n";
    std::vector<std::pair<std::string, std::string>> tests;  // (name, cname)
    for (auto& kv : fnDecls_) {
      const FnDecl* fn = kv.first;
      if (fn->moduleId != graph_.entry) continue;
      if (fn->name.rfind("test_", 0) != 0) continue;
      if (!testFilter_.empty() &&
          fn->name.find(testFilter_) == std::string::npos)
        continue;
      auto cit = fnDeclCnames_.find(fn);
      if (cit != fnDeclCnames_.end())
        tests.push_back({fn->name, cit->second});
    }
    std::sort(tests.begin(), tests.end());
    if (tests.empty()) {
      body_ << "  %c0 = llvm.mlir.constant(0 : i32) : i32\n";
      body_ << "  llvm.return %c0 : i32\n}\n\n";
      return;
    }
    int n = (int)tests.size();
    // names/fns as stack arrays of !llvm.ptr (char* / fn-ptr elements).
    body_ << "  %tone = llvm.mlir.constant(1 : i64) : i64\n";
    body_ << "  %names = llvm.alloca %tone x !llvm.array<" << n
          << " x !llvm.ptr> : (i64) -> !llvm.ptr\n";
    body_ << "  %fns = llvm.alloca %tone x !llvm.array<" << n
          << " x !llvm.ptr> : (i64) -> !llvm.ptr\n";
    for (int i = 0; i < n; ++i) {
      std::string idx = "%ti" + std::to_string(i);
      body_ << "  " << idx << " = llvm.mlir.constant(" << i << " : i64) : i64\n";
      std::string g = cstrGlobalSym(tests[i].first);
      body_ << "  %tn" << i << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
      body_ << "  %tnp" << i << " = llvm.getelementptr %names[" << idx
            << "] : (!llvm.ptr, i64) -> !llvm.ptr, i64\n";
      body_ << "  llvm.store %tn" << i << ", %tnp" << i
            << " : !llvm.ptr, !llvm.ptr\n";
      body_ << "  %tf" << i << " = llvm.mlir.addressof @" << tests[i].second
            << " : !llvm.ptr\n";
      body_ << "  %tfp" << i << " = llvm.getelementptr %fns[" << idx
            << "] : (!llvm.ptr, i64) -> !llvm.ptr, i64\n";
      body_ << "  llvm.store %tf" << i << ", %tfp" << i
            << " : !llvm.ptr, !llvm.ptr\n";
    }
    body_ << "  %tn = llvm.mlir.constant(" << n << " : i64) : i64\n";
    body_ << "  %rc = a.runtime.call \"a_test_main\"(%tn, %names, %fns)"
             " {signature = \"(i64, !llvm.ptr, !llvm.ptr) -> i32\"}"
             " : (i64, !llvm.ptr, !llvm.ptr) -> i32\n";
    body_ << "  llvm.return %rc : i32\n}\n\n";
  }

  // ---- statements ------------------------------------------------------------------
  void emitStatements(const std::vector<StmtPtr>& stmts) {
    // First call after beginFunction sees the whole body: record every
    // AssignStmt value kind per name so bindLocalT can prove a typed slot
    // is never overwritten by an incompatible (rebound) value.
    if (assignScanPending_) {
      assignScanPending_ = false;
      scanAssignKinds(stmts);
    }
    for (const StmtPtr& s : stmts) {
      if (terminated_) return;
      emitStmt(*s);
      if (failed_) return;
    }
  }

  void emitStmt(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Let: {
        const LetStmt& ls = static_cast<const LetStmt&>(s);
        if (ls.init) bindLocalT(ls.name, evalExprT(*ls.init));
        else bindLocal(ls.name, c64(kAvalTag | 0x2));
        break;
      }
      case StmtKind::Assign: {
        const AssignStmt& as = static_cast<const AssignStmt&>(s);
        emitAssign(as);
        break;
      }
      case StmtKind::Expr: {
        const ExprStmt& es = static_cast<const ExprStmt&>(s);
        (void)evalExpr(*es.expr);
        break;
      }
      case StmtKind::Return: {
        const ReturnStmt& rs = static_cast<const ReturnStmt&>(s);
        if (inTypedCore_) {
          TypedVal tv = rs.value ? evalExprT(*rs.value) : TypedVal{"", "aval"};
          emitPendingDefers();
          emitAllDrops();
          emitReturnTyped(tv);
        } else {
          std::string v = rs.value ? evalExpr(*rs.value) : c64(kAvalTag | 0x2);
          emitPendingDefers();
          emitAllDrops();
          emitReturn(v);
        }
        terminated_ = true;
        break;
      }
      case StmtKind::If: emitIf(static_cast<const IfStmt&>(s)); break;
      case StmtKind::While: emitWhile(static_cast<const WhileStmt&>(s)); break;
      case StmtKind::DoWhile: emitDoWhile(static_cast<const DoWhileStmt&>(s)); break;
      case StmtKind::For: emitFor(static_cast<const ForStmt&>(s)); break;
      case StmtKind::Loop: emitLoop(static_cast<const LoopStmt&>(s)); break;
      case StmtKind::Break: {
        if (breakLabels_.empty()) { fail("break outside loop"); return; }
        fn_ << "  llvm.br ^" << breakLabels_.back() << "\n";
        terminated_ = true;
        break;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& d = static_cast<const DestructureStmt&>(s);
        std::string v = evalExpr(*d.value);
        for (size_t i = 0; i < d.names.size(); ++i) {
          if (d.names[i] == "_") continue;
          bindLocal(d.names[i], callI64("a_getitem", {v, avalInt((std::int64_t)i)}));
        }
        break;
      }
      case StmtKind::Defer: {
        const DeferStmt& ds = static_cast<const DeferStmt&>(s);
        pendingDefers_.push_back(ds.body.get());
        break;
      }
      case StmtKind::Continue: {
        if (contLabels_.empty()) { fail("continue outside loop"); return; }
        fn_ << "  llvm.br ^" << contLabels_.back() << "\n";
        terminated_ = true;
        break;
      }
      case StmtKind::Import:
      case StmtKind::FromImport:
        break;  // bindings resolve at use sites; unsupported uses error there
      case StmtKind::FnDecl:
        fail("nested function declarations are not supported by the mlir backend yet");
        break;
      default:
        fail("statement kind not supported by the mlir backend yet");
        break;
    }
  }

  // struct `drop` method cname for an expr's inferred struct type, "" none.
  std::string dropFnForExpr(const Expr* e) {
    if (!e || !e->inferredType) return "";
    TypePtr t = resolveType(e->inferredType);
    if (!t || t->kind != TypeKind::Struct) return "";
    std::string sn = shortName(t->structName);
    auto it = structByName_.find(sn);
    if (it == structByName_.end()) it = structByName_.find(t->structName);
    if (it == structByName_.end()) return "";
    for (const auto& m : structs_[it->second].methods)
      if (m.first == "drop") return m.second;
    return "";
  }
  // record a drop for a freshly-bound droppable value
  void recordDrop(const std::string& slot, const std::string& fn) {
    if (fn.empty() || dropStack_.empty()) return;
    dropStack_.back().push_back({slot, fn});
  }

  void emitAssign(const AssignStmt& as) {
    const Expr& t = *as.target;
    if (as.binop != BinOp::Invalid) {
      // compound assign: target = target <op> value
      // typed fast path: unboxed scalar binding op unboxed scalar value
      if (t.kind == ExprKind::Ident && typed_) {
        const std::string& an = static_cast<const IdentExpr&>(t).name;
        if (MlBinding* tb = lookup(an); tb && tb->type != "aval") {
          bool isG = false;
          TypedVal curT = bindingValueT(an, isG);
          TypedVal rhsT = evalExprT(*as.value);
          if (curT.kind != "aval" && curT.kind == rhsT.kind) {
            TypedVal res = typedBinOp(as.binop, curT, rhsT, curT.kind);
            if (!res.kind.empty()) {
              MlBinding* old = lookup(an);
              if (old && !old->dropFn.empty()) emitDropAt(old->slot, old->dropFn);
              storeBindingT(an, res);
              std::string df = dropFnForExpr(as.value.get());
              if (df.empty() && old) old->dropFn = "";
              if (!df.empty() && old) { old->dropFn = df; recordDrop(old->slot, df); }
              return;
            }
          }
        }
      }
      std::string cur;
      if (t.kind == ExprKind::Ident) {
        bool isG = false;
        cur = bindingValue(static_cast<const IdentExpr&>(t).name, isG);
        if (cur.empty()) {
          // unbound compound-assign target: read as zero-init global then write back
          cur = c64(kAvalTag | 0x2);
        }
      } else {
        cur = evalExpr(t);
      }
      std::string rhs = evalExpr(*as.value);
      std::string v = emitBinOp(as.binop, cur, rhs, as.opSpan);
      if (t.kind == ExprKind::Ident) {
        const std::string& an = static_cast<const IdentExpr&>(t).name;
        if (!lookup(an) && globalSymFor(an).empty()) {
          failAt("cannot assign to unbound name '" + an + "'", &as);
          return;
        }
        MlBinding* old = lookup(an);
        if (old && !old->dropFn.empty()) emitDropAt(old->slot, old->dropFn);
        storeBinding(an, v);
        // new value may itself be droppable
        std::string df = dropFnForExpr(as.value.get());
        if (df.empty() && old) old->dropFn = "";
        if (!df.empty() && old) { old->dropFn = df; recordDrop(old->slot, df); }
      } else if (t.kind == ExprKind::Index) {
        const IndexExpr& ix = static_cast<const IndexExpr&>(t);
        std::string obj = evalExpr(*ix.object);
        std::string idx = evalExpr(*ix.index);
        callI64("a_setitem", {obj, idx, v});
      } else if (t.kind == ExprKind::Field) {
        const FieldExpr& fx = static_cast<const FieldExpr&>(t);
        std::string obj = evalExpr(*fx.object);
        callI64T("a_setattr", {{"i64", obj}, {"!llvm.ptr", cstrPtr(fx.field)}, {"i64", v}});
      } else {
        fail("compound assignment target not supported");
      }
      return;
    }
    // plain "="
    TypedVal tv = evalExprT(*as.value);
    if (t.kind == ExprKind::Ident) {
      const std::string& an = static_cast<const IdentExpr&>(t).name;
      // bare `x = ...` inside a user fn or the entry body binds a local
      // (entry-body names are invisible to other functions — verified by
      // Sema — so a stack slot is semantics-preserving and lets typed
      // scalars stay unboxed; conservative-stack-scan GC covers the slots)
      bool firstBind = (inUserFn_ || inEntryBody_) && !lookup(an) &&
                       globalSymFor(an).empty();
      if (firstBind) bindLocalT(an, tv);
      MlBinding* old = lookup(an);
      bool hadDrop = old && !old->dropFn.empty();
      if (hadDrop) emitDropAt(old->slot, old->dropFn);
      // firstBind already stored the value in bindLocalT; storing again
      // would re-emit the box sequence for typed values. When typed_ is off
      // keep the original double-store so output stays byte-identical.
      if (!firstBind || !typed_) storeBindingT(an, tv);
      std::string df = dropFnForExpr(as.value.get());
      if (MlBinding* nb = lookup(an)) {
        nb->dropFn = df;
        if (df.empty()) { if (hadDrop) removeDrop(nb->slot); }
        else if (!hadDrop) recordDrop(nb->slot, df);
        // hadDrop && df: existing record already tracks the slot — no re-add
      } else {
        // global binding: record "g:<sym>" drop
        std::string gsym = globalSymFor(an);
        if (!gsym.empty() && !df.empty())
          recordDrop("g:" + gsym, df);
      }
    } else if (t.kind == ExprKind::Index) {
      const IndexExpr& ix = static_cast<const IndexExpr&>(t);
      callI64("a_setitem", {evalExpr(*ix.object), evalExpr(*ix.index),
                            boxToAval(tv)});
    } else if (t.kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(t);
      // module-global store: mod.name = v (mod is an A-module alias)
      if (fx.object->kind == ExprKind::Ident) {
        const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
        auto ma = aModuleAliases_.find(oid.name);
        if (ma != aModuleAliases_.end()) {
          std::string gsym = moduleGlobalSym(ma->second, fx.field);
          if (!gsym.empty()) {
            std::string r = nv();
            fn_ << "  " << r << " = llvm.mlir.addressof @" << gsym << " : !llvm.ptr\n";
            store(r, boxToAval(tv));
            return;
          }
        }
      }
      std::string obj = evalExpr(*fx.object);
      std::string namev = cstrPtr(fx.field);
      callI64T("a_setattr", {{"i64", obj}, {"!llvm.ptr", namev}, {"i64", boxToAval(tv)}});
    } else {
      fail("assignment target not supported");
    }
  }

  // ---- control flow -----------------------------------------------------------------
  void emitIf(const IfStmt& is) {
    // linear chain: each branch gets body^N / next^N blocks; else runs in
    // the final next block; everything merges at ^end.
    std::string lend = nl();
    for (size_t i = 0; i < is.branches.size(); ++i) {
      const auto& br = is.branches[i];
      bool last = (i + 1 == is.branches.size());
      std::string lbody = nl(), lnext = nl();
      TypedVal ctv = evalExprT(*br.cond);
      std::string b = truthyT(ctv);
      fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lnext << "\n";
      fn_ << "^" << lbody << ":\n";
      terminated_ = false;
      pushScope();
      emitStatements(br.body);
      popScope();
      if (!terminated_) fn_ << "  llvm.br ^" << lend << "\n";
      fn_ << "^" << lnext << ":\n";
      terminated_ = false;
      if (last) {
        pushScope();
        emitStatements(is.elseBody);
        popScope();
        if (!terminated_) fn_ << "  llvm.br ^" << lend << "\n";
      }
    }
    if (is.branches.empty()) {
      pushScope();
      emitStatements(is.elseBody);
      popScope();
      if (!terminated_) fn_ << "  llvm.br ^" << lend << "\n";
    }
    fn_ << "^" << lend << ":\n";
    terminated_ = false;
  }

  void emitWhile(const WhileStmt& ws) {
    std::string lcond = nl(), lbody = nl(), lafter = nl();
    breakLabels_.push_back(lafter);
    contLabels_.push_back(lcond);
    fn_ << "  llvm.br ^" << lcond << "\n";
    fn_ << "^" << lcond << ":\n";
    terminated_ = false;
    TypedVal wtv = evalExprT(*ws.cond);
    std::string b = truthyT(wtv);
    fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lafter << "\n";
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    emitStatements(ws.body);
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lcond << "\n";
    fn_ << "^" << lafter << ":\n";
    terminated_ = false;
    breakLabels_.pop_back();
    contLabels_.pop_back();
  }

  void emitDoWhile(const DoWhileStmt& dw) {
    std::string lbody = nl(), lcond = nl(), lafter = nl();
    breakLabels_.push_back(lafter);
    contLabels_.push_back(lcond);
    fn_ << "  llvm.br ^" << lbody << "\n";
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    emitStatements(dw.body);
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lcond << "\n";
    fn_ << "^" << lcond << ":\n";
    terminated_ = false;
    TypedVal dtv = evalExprT(*dw.cond);
    std::string b = truthyT(dtv);
    fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lafter << "\n";
    fn_ << "^" << lafter << ":\n";
    terminated_ = false;
    breakLabels_.pop_back();
    contLabels_.pop_back();
  }

  // `for i in a..b[..s]:` counted loop (port of LlvmEmitter's
  // emitCountedLoop): bounds evaluated once via a_range_bound (same
  // expect_int64 checks/panics as a_range_new), step 0 panics up front,
  // cur advances with overflow saturation to hi. The loop variable binds
  // as a typed i64 local when the assign-kind scan proves it is never
  // rebound to a non-i64 scalar, so body arithmetic stays unboxed.
  void emitCountedLoop(const ForStmt& s, const std::string& lo,
                       const std::string& hi, const std::string& st) {
    // step == 0 -> panic before the first iteration (a_range_new parity)
    std::string zc = nl(), zo = nl();
    {
      std::string z = nv();
      std::string zero = c64(0);  // emits its own line; hoist before use
      fn_ << "  " << z << " = llvm.icmp \"eq\" " << st << ", " << zero
          << " : i64\n";
      fn_ << "  llvm.cond_br " << z << ", ^" << zc << ", ^" << zo << "\n";
    }
    fn_ << "^" << zc << ":\n";
    callI64T("a_panic_msg",
             {{"!llvm.ptr",
               cstrPtr("range() step must be positive, found 0.")}});
    fn_ << "  llvm.unreachable\n";
    fn_ << "^" << zo << ":\n";

    std::string cur = allocaI64();
    store(cur, lo);
    // The assign-kind scan is per-function and may not have covered this
    // body (top-level stmts bypass it), so scan here: `var` gets a typed
    // i64 slot only when no AssignStmt in the body rebinds it to another
    // scalar kind.
    if (typed_) scanAssignKinds(s.body);
    const bool typedVar = typed_ && canTypeBind(s.var, "i64");
    std::string vslot = allocaI64();
    if (!typedVar) pin(vslot);  // aval fallback may hold heap values
    std::string sp = nv();
    {
      std::string zero = c64(0);
      fn_ << "  " << sp << " = llvm.icmp \"sgt\" " << st << ", " << zero
          << " : i64\n";
    }
    std::string lhead = nl(), lbody = nl(), lincr = nl(), lend = nl();
    breakLabels_.push_back(lend);
    contLabels_.push_back(lincr);  // continue advances cur, like a_iter_next
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lhead << ":\n";
    terminated_ = false;
    std::string c = load(cur);
    std::string lt = nv(), gt = nv(), more = nv();
    fn_ << "  " << lt << " = llvm.icmp \"slt\" " << c << ", " << hi
        << " : i64\n";
    fn_ << "  " << gt << " = llvm.icmp \"sgt\" " << c << ", " << hi
        << " : i64\n";
    fn_ << "  " << more << " = llvm.select " << sp << ", " << lt << ", "
        << gt << " : i1, i1\n";
    fn_ << "  llvm.cond_br " << more << ", ^" << lbody << ", ^" << lend
        << "\n";
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    {
      MlBinding mb;
      mb.slot = vslot;
      if (typedVar) {
        mb.type = "i64";
        storeTy(vslot, c, "i64");
      } else {
        store(vslot, boxToAval({c, "i64"}));
      }
      scopes_.back()[s.var] = mb;
    }
    emitStatements(s.body);
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lincr << "\n";
    fn_ << "^" << lincr << ":\n";
    // Advance with the same saturation as ITAG_RANGE: on overflow cur is
    // set to hi so the next check terminates.
    std::string limP = nv(), ovP = nv(), sum = nv(), nP = nv();
    std::string imax = c64(INT64_MAX);
    fn_ << "  " << limP << " = llvm.sub " << imax << ", " << st << " : i64\n";
    fn_ << "  " << ovP << " = llvm.icmp \"sgt\" " << c << ", " << limP
        << " : i64\n";
    fn_ << "  " << sum << " = llvm.add " << c << ", " << st << " : i64\n";
    fn_ << "  " << nP << " = llvm.select " << ovP << ", " << hi << ", "
        << sum << " : i1, i64\n";
    std::string limN = nv(), ovN = nv(), nN = nv(), nxt = nv();
    // INT64_MIN as -INT64_MAX-1: the bare -9223372036854775808 literal does
    // not round-trip through MLIR's integer-attr parser.
    std::string iMin = nv();
    std::string nmax = c64(-INT64_MAX), one = c64(1);
    fn_ << "  " << iMin << " = llvm.sub " << nmax << ", " << one << " : i64\n";
    fn_ << "  " << limN << " = llvm.sub " << iMin << ", " << st << " : i64\n";
    fn_ << "  " << ovN << " = llvm.icmp \"slt\" " << c << ", " << limN
        << " : i64\n";
    fn_ << "  " << nN << " = llvm.select " << ovN << ", " << hi << ", "
        << sum << " : i1, i64\n";
    fn_ << "  " << nxt << " = llvm.select " << sp << ", " << nP << ", "
        << nN << " : i1, i64\n";
    store(cur, nxt);
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lend << ":\n";
    terminated_ = false;
    breakLabels_.pop_back();
    contLabels_.pop_back();
  }

  void emitForRange(const ForStmt& s) {
    const auto& r = *static_cast<const RangeExpr*>(s.iterable.get());
    // a_range_bound returns the raw i64 bound (expect_int64 checks, same
    // panics as a_range_new) — not an aval word.
    std::string lo = callI64("a_range_bound", {evalExpr(*r.start), c64(0)});
    std::string hi = callI64("a_range_bound", {evalExpr(*r.end), c64(0)});
    std::string st = r.step
                         ? callI64("a_range_bound", {evalExpr(*r.step), c64(1)})
                         : c64(1);
    emitCountedLoop(s, lo, hi, st);
  }

  // True when `range(...)` resolved to the builtin (Sema-set flags, so a
  // user shadow is already excluded) with 1..3 positional args — the same
  // shape bi_range accepts. lo/hi/st get the raw i64 bounds in bi_range's
  // argument order.
  bool rangeCallBounds(const CallExpr& c, std::string& lo, std::string& hi,
                       std::string& st) {
    if (!c.calleeIsBuiltin || c.calleeBuiltin != "range") return false;
    const std::size_t na = c.args.size();
    if (na < 1 || na > 3) return false;
    for (const CallArg& a : c.args)
      if (!a.name.empty()) return false;
    // Evaluation order matches bi_range: lo, hi, step (left to right).
    lo = na >= 2 ? callI64("a_range_bound", {evalExpr(*c.args[0].value), c64(0)})
                 : c64(0);
    hi = callI64("a_range_bound",
                 {evalExpr(*c.args[na >= 2 ? 1 : 0].value), c64(0)});
    st = na == 3
             ? callI64("a_range_bound", {evalExpr(*c.args[2].value), c64(1)})
             : c64(1);
    return true;
  }

  void emitFor(const ForStmt& fs) {
    if (parforEligible(fs)) { emitForParfor(fs); return; }
    if (fs.iterable && fs.iterable->kind == ExprKind::Range) {
      emitForRange(fs);
      return;
    }
    if (fs.iterable && fs.iterable->kind == ExprKind::Call) {
      const auto& c = *static_cast<const CallExpr*>(fs.iterable.get());
      std::string lo, hi, st;
      if (rangeCallBounds(c, lo, hi, st)) {
        emitCountedLoop(fs, lo, hi, st);
        return;
      }
    }
    std::string coll = evalExpr(*fs.iterable);
    std::string it = callI64("a_iter_new", {coll});
    std::string itemSlot = allocaI64();
    pin(itemSlot);  // item aval lives across a_iter_next calls -> GC-visible
    std::string lhead = nl(), lbody = nl(), lend = nl();
    breakLabels_.push_back(lend);
    contLabels_.push_back(lhead);
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lhead << ":\n";
    terminated_ = false;
    std::string nx = callI64T("a_iter_next",
                              {{"i64", it}, {"!llvm.ptr", gepIdx(itemSlot, c64(0))}});
    std::string b = nv();
    { std::string z = c64(0);
      fn_ << "  " << b << " = llvm.icmp \"ne\" " << nx << ", " << z << " : i64\n"; }
    fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lend << "\n";
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    {
      MlBinding mb;
      mb.slot = itemSlot;
      scopes_.back()[fs.var] = mb;
    }
    emitStatements(fs.body);
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lend << ":\n";
    terminated_ = false;
    breakLabels_.pop_back();
    contLabels_.pop_back();
  }

  void emitLoop(const LoopStmt& ls) {
    std::string lhead = nl(), lend = nl();
    breakLabels_.push_back(lend);
    contLabels_.push_back(lhead);
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lhead << ":\n";
    terminated_ = false;
    pushScope();
    emitStatements(ls.body);
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lend << ":\n";
    terminated_ = false;
    breakLabels_.pop_back();
    contLabels_.pop_back();
  }

  // ---- expressions ------------------------------------------------------------------
  // string literal -> aval (a_str_new on a byte global; f-string parts concat)
  std::string stringLiteralAval(const std::string& text) {
    // emit byte global with insertvalue initializer
    std::string bytes = text;
    bytes.push_back('\0');
    std::string g = privPrefix() + "str" + std::to_string(strCounter_++);
    pre_ << "  llvm.mlir.global private constant @" << g << "() : !llvm.array<"
         << bytes.size() << " x i8> {\n";
    // initializer region: build array via insertvalue chain
    pre_ << "    %u = llvm.mlir.undef : !llvm.array<" << bytes.size() << " x i8>\n";
    std::string cur = "%u";
    for (size_t i = 0; i < bytes.size(); ++i) {
      std::string cv = "%cb" + std::to_string(i);
      pre_ << "    " << cv << " = llvm.mlir.constant(" << (int)(unsigned char)bytes[i]
           << " : i8) : i8\n";
      std::string nx = "%r" + std::to_string(i);
      pre_ << "    " << nx << " = llvm.insertvalue " << cv << ", " << cur << "["
           << i << "] : !llvm.array<" << bytes.size() << " x i8>\n";
      cur = nx;
    }
    pre_ << "    llvm.return " << cur << " : !llvm.array<" << bytes.size() << " x i8>\n";
    pre_ << "  }\n";
    // in function: addressof + a_str_new(ptr, len)
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
    return callI64T("a_str_new", {{"!llvm.ptr", r},
                                  {"i64", c64((std::int64_t)text.size())}});
  }

  // aval-producing expression evaluator (phase-1 representation). All
  // recursive uses go through the evalExpr wrapper, which routes typed
  // subexpressions through evalExprT and re-boxes them at this boundary.
  std::string evalExprImpl(const Expr& e) {
    if (failed_) return "";
    switch (e.kind) {
      case ExprKind::IntLit:
        return avalInt(static_cast<const IntLitExpr&>(e).value);
      case ExprKind::FloatLit:
        return avalDbl(static_cast<const FloatLitExpr&>(e).value);
      case ExprKind::BoolLit:
        return avalBool(static_cast<const BoolLitExpr&>(e).value);
      case ExprKind::CharLit: {
        // encode codepoint as UTF-8
        char32_t cp = static_cast<const CharLitExpr&>(e).value;
        std::string u8;
        if (cp < 0x80) u8 += (char)cp;
        else if (cp < 0x800) {
          u8 += (char)(0xC0 | (cp >> 6));
          u8 += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          u8 += (char)(0xE0 | (cp >> 12));
          u8 += (char)(0x80 | ((cp >> 6) & 0x3F));
          u8 += (char)(0x80 | (cp & 0x3F));
        } else {
          u8 += (char)(0xF0 | (cp >> 18));
          u8 += (char)(0x80 | ((cp >> 12) & 0x3F));
          u8 += (char)(0x80 | ((cp >> 6) & 0x3F));
          u8 += (char)(0x80 | (cp & 0x3F));
        }
        // reuse stringLiteralAval machinery via a_char_new
        std::string g = privPrefix() + "chr" + std::to_string(strCounter_++);
        std::string bytes = u8;
        bytes.push_back('\0');
        pre_ << "  llvm.mlir.global private constant @" << g << "() : !llvm.array<"
             << bytes.size() << " x i8> {\n";
        pre_ << "    %u = llvm.mlir.undef : !llvm.array<" << bytes.size() << " x i8>\n";
        std::string cur = "%u";
        for (size_t i = 0; i < bytes.size(); ++i) {
          std::string cv = "%cb" + std::to_string(i);
          pre_ << "    " << cv << " = llvm.mlir.constant(" << (int)(unsigned char)bytes[i]
               << " : i8) : i8\n";
          std::string nx = "%r" + std::to_string(i);
          pre_ << "    " << nx << " = llvm.insertvalue " << cv << ", " << cur << "["
               << i << "] : !llvm.array<" << bytes.size() << " x i8>\n";
          cur = nx;
        }
        pre_ << "    llvm.return " << cur << " : !llvm.array<" << bytes.size() << " x i8>\n";
        pre_ << "  }\n";
        std::string r = nv();
        fn_ << "  " << r << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
        return callI64T("a_char_new", {{"!llvm.ptr", r},
                                       {"i64", c64((std::int64_t)u8.size())}});
      }
      case ExprKind::StringLit: return evalStringLit(static_cast<const StringLitExpr&>(e));
      case ExprKind::Ident: {
        const IdentExpr& id = static_cast<const IdentExpr&>(e);
        if (id.name == "None") return c64(kAvalTag | 0x2);
        if (id.name == "True") return c64(kAvalTag | 0x6);
        if (id.name == "False") return c64(kAvalTag | 0x4);
        // builtin constants (mirrors LlvmEmitter::evalIdent kConsts)
        const auto& kDConsts = mlirutil::kBuiltinDConsts;
        const auto& kIConsts = mlirutil::kBuiltinIConsts;
        if (id.name == "I64_MIN" || id.name == "I64_MAX")
          return callI64("a_i64n", {c64(id.name == "I64_MIN" ? INT64_MIN : INT64_MAX)});
        if (id.name == "stdout") return c64(kAvalTag | 0x8);
        if (id.name == "stderr") return c64(kAvalTag | 0xA);
        auto dc = kDConsts.find(id.name);
        if (dc != kDConsts.end()) return avalDbl(dc->second);
        auto ic = kIConsts.find(id.name);
        if (ic != kIConsts.end()) return avalInt(ic->second);
        // locals/globals shadow same-named functions in other modules
        // (fnCnames_ is keyed by bare name across the whole module graph)
        bool isG = false;
        std::string bv = bindingValue(id.name, isG);
        if (!bv.empty()) return bv;
        // named function used as a value -> fnon(ptrtoint(@fn), env=None, name)
        auto fc = fnCnames_.find(id.name);
        if (fc != fnCnames_.end()) {
          std::string r = nv();
          fn_ << "  " << r << " = llvm.mlir.addressof @" << fc->second << " : !llvm.ptr\n";
          std::string fp = nv();
          fn_ << "  " << fp << " = llvm.ptrtoint " << r << " : !llvm.ptr to i64\n";
          std::string envN = c64(kAvalTag | 0x2);
          std::string nm = cstrPtr(id.name);
          return callI64T("a_fnon_new", {{"i64", fp}, {"i64", envN}, {"!llvm.ptr", nm}});
        }
        // from-imported native constants
        auto ca = constAliases_.find(id.name);
        if (ca != constAliases_.end()) {
          if (ca->second == "PI") return avalDbl(3.14159265358979323846);
          if (ca->second == "E") return avalDbl(2.71828182845904523536);
          failAt("unknown native constant '" + ca->second + "'", &e);
          return "";
        }
        // python module name as a value -> py_import_module(module)
        auto pa = pythonAliases_.find(id.name);
        if (pa != pythonAliases_.end()) {
          int iid = builtinRuntimeId("py_import_module");
          if (iid >= 0)
            return callBuiltin(iid, {stringLiteralAval(pa->second)});
        }
        if (bv.empty()) {
          failAt("unbound name '" + id.name + "'", &e);
          return "";
        }
        return bv;
      }
      case ExprKind::Unary: return evalUnary(static_cast<const UnaryExpr&>(e));
      case ExprKind::Binary: return evalBinary(static_cast<const BinaryExpr&>(e));
      case ExprKind::Call: return evalCall(static_cast<const CallExpr&>(e));
      case ExprKind::Index: {
        const IndexExpr& ix = static_cast<const IndexExpr&>(e);
        return callI64("a_getitem", {evalExpr(*ix.object), evalExpr(*ix.index)});
      }
      case ExprKind::Field: {
        const FieldExpr& fx = static_cast<const FieldExpr&>(e);
        // native module constant: math.PI, math.E, ...
        if (fx.object->kind == ExprKind::Ident) {
          const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
          auto na = nativeAliases_.find(oid.name);
          if (na != nativeAliases_.end()) {
            const NativeModuleSpec* spec = nativeModuleSpec(na->second);
            if (spec) {
              auto cn = spec->constants.find(fx.field);
              if (cn != spec->constants.end()) {
                const std::string& cv = cn->second;
                if (cv == "PI") return avalDbl(3.14159265358979323846);
                if (cv == "E") return avalDbl(2.71828182845904523536);
                failAt("unknown native constant '" + na->second + "." + fx.field + "'", &e);
                return "";
              }
            }
          }
        }
        // A-module member value: os.name, os.path, ...
        if (fx.object->kind == ExprKind::Ident) {
          const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
          auto ma = aModuleAliases_.find(oid.name);
          if (ma != aModuleAliases_.end())
            return moduleMemberValue(ma->second, fx.field, &e);
        }
        // enum variant value: Enum.Variant (no payload)
        {
          std::vector<std::string> parts = flattenField(&e);
          if (const MlVariant* mv = findVariant(parts)) {
            // variant name is last part
            return variantValue(*mv, parts.back(), nullptr);
          }
        }
        std::string obj = evalExpr(*fx.object);
        std::string namev = cstrPtr(fx.field);
        return callI64T("a_getattr", {{"i64", obj}, {"!llvm.ptr", namev}});
      }
      case ExprKind::ArrayLit: {
        const ArrayLitExpr& al = static_cast<const ArrayLitExpr&>(e);
        std::string list = callI64T("a_list_new", {{"i64", c64(0)}, {"!llvm.ptr", nullPtr()}});
        for (const ExprPtr& el : al.elements) {
          std::string ev = evalExpr(*el);
          callI64("a_list_append", {list, ev});  // in-place; returns A_NONE
        }
        return list;
      }
      case ExprKind::Tuple: {
        const TupleExpr& te = static_cast<const TupleExpr&>(e);
        std::vector<std::string> items;
        for (const ExprPtr& el : te.elements) items.push_back(evalExpr(*el));
        std::string av = argvPack(items);
        return callI64T("a_tuple_new",
                        {{"i64", c64((std::int64_t)items.size())}, {"!llvm.ptr", av}});
      }
      case ExprKind::MapLit: {
        const MapLitExpr& me = static_cast<const MapLitExpr&>(e);
        std::string d = callI64("a_dict_new", {});
        for (const auto& pr : me.entries) {
          callI64("a_dict_set", {d, evalExpr(*pr.first), evalExpr(*pr.second)});
        }
        return d;
      }
      case ExprKind::SetLit: {
        const SetLitExpr& se = static_cast<const SetLitExpr&>(e);
        std::string st = callI64("a_set_new", {});
        for (const ExprPtr& el : se.elements)
          callI64("a_set_add", {st, evalExpr(*el)});
        return st;
      }
      case ExprKind::Range: {
        const RangeExpr& re = static_cast<const RangeExpr&>(e);
        std::string a = re.start ? evalExpr(*re.start) : avalInt(0);
        std::string b = evalExpr(*re.end);
        std::string st = re.step ? evalExpr(*re.step) : avalInt(1);
        return callI64("a_range_new", {a, b, st});
      }
      case ExprKind::SliceSpec: {
        // multi-index slice component -> 3-list [start,end,step], A_NONE open
        const SliceSpecExpr& sx = static_cast<const SliceSpecExpr&>(e);
        std::string kn = c64(kAvalTag | 0x2);
        std::string lo = sx.start ? evalExpr(*sx.start) : kn;
        std::string hi = sx.end ? evalExpr(*sx.end) : kn;
        std::string st = sx.step ? evalExpr(*sx.step) : kn;
        std::string av = allocaArr(3);
        store(gepIdx(av, c64(0)), lo);
        store(gepIdx(av, c64(1)), hi);
        store(gepIdx(av, c64(2)), st);
        return callI64T("a_list_new", {{"i64", c64(3)}, {"!llvm.ptr", av}});
      }
      case ExprKind::Slice: {
        const SliceExpr& sl = static_cast<const SliceExpr&>(e);
        std::string kn = c64(kAvalTag | 0x2);
        std::string lo = sl.start ? evalExpr(*sl.start) : kn;
        std::string hi = sl.end ? evalExpr(*sl.end) : kn;
        std::string st = sl.step ? evalExpr(*sl.step) : kn;
        return callI64("a_slice", {evalExpr(*sl.object), lo, hi, st});
      }
      case ExprKind::Try: return evalTry(static_cast<const TryExpr&>(e));
      case ExprKind::Comprehension:
        return evalComprehension(static_cast<const ComprehensionExpr&>(e));
      case ExprKind::Lambda:
        return evalLambda(static_cast<const LambdaExpr&>(e));
      case ExprKind::SafeNav:
        return evalSafeNav(static_cast<const SafeNavExpr&>(e));
      case ExprKind::Match:
        return evalMatch(static_cast<const MatchExpr&>(e));
      case ExprKind::Ternary: {
        const TernaryExpr& te = static_cast<const TernaryExpr&>(e);
        return ifExprResult(*te.cond, *te.thenValue, *te.elseValue);
      }
      default:
        failAt("expression kind " + std::to_string((int)e.kind) + " not supported", &e);
        return "";
    }
  }

  // ---- typed expression evaluation (A_MLIR_TYPED) ------------------------------
  // evalExprT returns the expression's value either as a raw i64 aval word
  // (kind "aval") or as an unboxed i64/f64/i1 scalar. evalExpr is the
  // compatibility wrapper every pre-existing call site uses: it boxes any
  // typed result back to an aval word, so all legacy consumers (runtime
  // calls, argv packing, returns, stores into aval slots) keep working.
  std::string evalExpr(const Expr& e) {
    TypedVal tv = evalExprT(e);
    return boxToAval(tv);
  }

  TypedVal evalExprT(const Expr& e) {
    if (failed_) return {"", "aval"};
    if (!typed_) return {evalExprImpl(e), "aval"};
    switch (e.kind) {
      case ExprKind::IntLit:
        return {"", "i64", true,
                static_cast<const IntLitExpr&>(e).value, 0.0};
      case ExprKind::FloatLit:
        return {"", "f64", true, 0,
                static_cast<const FloatLitExpr&>(e).value};
      case ExprKind::BoolLit:
        return {"", "i1", true,
                static_cast<const BoolLitExpr&>(e).value ? 1 : 0, 0.0};
      case ExprKind::Ident: {
        const IdentExpr& id = static_cast<const IdentExpr&>(e);
        if (MlBinding* b = lookup(id.name)) {
          if (b->type != "aval") return loadTypedVal(*b);
        }
        return {evalExprImpl(e), "aval"};
      }
      case ExprKind::Unary:
        return evalUnaryT(static_cast<const UnaryExpr&>(e));
      case ExprKind::Binary:
        return evalBinaryT(static_cast<const BinaryExpr&>(e));
      case ExprKind::Call: {
        TypedVal tv = tryTypedCall(static_cast<const CallExpr&>(e));
        if (!tv.kind.empty()) return tv;
        return {evalExprImpl(e), "aval"};
      }
      default:
        return {evalExprImpl(e), "aval"};
    }
  }

  // Attempt a typed-ABI direct call to a `__a_tc_<cname>` core. Every
  // bail-out check that doesn't need arg values runs BEFORE any arg is
  // evaluated, so a rejected call leaves no partially-emitted IR for the
  // boxed fallback to duplicate. Returns kind=="" on bail.
  TypedVal tryTypedCall(const CallExpr& c) {
    if (c.typeArgs.size() || c.calleeIsBuiltin ||
        c.builtinId != BuiltinId::None)
      return {"", ""};
    const FnDecl* fd = nullptr;
    if (c.calleeFunction) {
      fd = c.calleeFunction;
    } else if (c.callee->kind == ExprKind::Ident) {
      const IdentExpr& cid = static_cast<const IdentExpr&>(*c.callee);
      // `cast` is a parser-desugared pseudo-call handled specially by
      // evalCall; never route it through the typed ABI.
      if (cid.name == "cast") return {"", ""};
      auto fn = fnCnames_.find(cid.name);
      if (fn == fnCnames_.end()) return {"", ""};
      auto fit = fnByCname_.find(fn->second);
      if (fit == fnByCname_.end()) return {"", ""};
      fd = fit->second;
    } else {
      return {"", ""};
    }
    // Defensive: sema rejects host->offload calls, but never emit a call
    // into an offload core from host code even if it slips through.
    if (!curOffloadAttr_.empty() &&
        fd->offload == FnDecl::OffloadKind::None) {
      failAt("offload function calls host function '" + fd->name + "'", &c);
      return {"", "aval"};
    }
    if (typedCoreStatus(fd) != 1) return {"", ""};
    auto cit = fnDeclCnames_.find(fd);
    if (cit == fnDeclCnames_.end()) return {"", ""};
    if (c.args.size() != fd->params.size()) return {"", ""};
    for (const CallArg& a : c.args)
      if (!a.name.empty() || !a.value) return {"", ""};
    auto pit = sema_.fnParamTypes.find(fd);
    auto rit = sema_.fnReturnTypes.find(fd);
    if (pit == sema_.fnParamTypes.end() || rit == sema_.fnReturnTypes.end())
      return {"", ""};
    const std::string retkind = scalarKindOfT(rit->second);
    if (retkind.empty()) return {"", ""};
    std::vector<std::string> argssa;
    argssa.reserve(c.args.size());
    for (size_t i = 0; i < c.args.size(); ++i) {
      const std::string want = scalarKindOfT(pit->second[i]);
      TypedVal tv = evalExprT(*c.args[i].value);
      if (failed_) return {"", "aval"};
      if (tv.kind == want) {
        argssa.push_back(tvalSSA(tv));
      } else if (tv.kind == "aval" && !tv.name.empty()) {
        argssa.push_back(tvalSSA(unboxFromAval(tv.name, want)));
      } else if (!tv.name.empty() || tv.isConst) {
        // Different scalar kind: round-trip through aval so the core sees
        // exactly what the boxed ABI would have delivered (and the same
        // unbox panic on true type mismatches).
        argssa.push_back(tvalSSA(unboxFromAval(boxToAval(tv), want)));
      } else {
        return {"", "aval"};
      }
    }
    const std::string core = typedCoreSym(cit->second);
    std::string csig;
    for (size_t i = 0; i < c.args.size(); ++i) {
      if (i) csig += ", ";
      csig += kindLLVMType(scalarKindOfT(pit->second[i]));
    }
    std::string r = nv();
    fn_ << "  " << r << " = llvm.call @" << core << "(";
    for (size_t i = 0; i < argssa.size(); ++i) {
      if (i) fn_ << ", ";
      fn_ << argssa[i];
    }
    fn_ << ") : (" << csig << ") -> " << kindLLVMType(retkind) << "\n";
    return {r, retkind};
  }

  TypedVal evalUnaryT(const UnaryExpr& u) {
    // postfix ++/--, prefix ++/--, and `!` (Option unwrap) stay on the aval
    // path; `not`/`-`/`~` on proven scalars get native ops.
    if (u.postfix || u.op == "++" || u.op == "--" || u.op == "!")
      return {evalExprImpl(u), "aval"};
    TypedVal tv = evalExprT(*u.operand);
    if (u.op == "not" && tv.kind == "i1") {
      if (tv.isConst) return {"", "i1", true, tv.iConst ? 0 : 1, 0.0};
      std::string f = ci1(false);
      return {icmp("eq", tvalSSA(tv), f, "i1"), "i1"};
    }
    if (u.op == "-" && tv.kind == "i64") {
      if (tv.isConst) {
        // -INT64_MIN promotes to double at runtime; keep the aval path.
        if (tv.iConst == INT64_MIN) return {evalExprImpl(u), "aval"};
        return {"", "i64", true, -tv.iConst, 0.0};
      }
      // Non-constant i64 negation can hit INT64_MIN, which the runtime
      // promotes to f64 — guard it and fall back to the aval path there.
      {
        std::string x = tvalSSA(tv);
        std::string ls = nl(), lf = nl(), lm = nl(), pv = nv();
        std::string bad = icmp("eq", x, c64(INT64_MIN), "i64");
        fn_ << "  llvm.cond_br " << bad << ", ^" << ls << ", ^" << lf << "\n";
        fn_ << "^" << lf << ":\n";
        std::string res = llvm2("sub", c64(0), x, "i64");
        fn_ << "  llvm.br ^" << lm << "(" << res << " : i64)\n";
        fn_ << "^" << ls << ":\n";
        // Same fallback as the untyped path: boxed negate, then a_int_full
        // (panics on the f64 result INT64_MIN produces).
        std::string av = callI64("a_neg_fast", {boxToAval(tv)});
        std::string ri = callI64("a_int_full", {av});
        fn_ << "  llvm.br ^" << lm << "(" << ri << " : i64)\n";
        fn_ << "^" << lm << "(" << pv << " : i64):\n";
        return {pv, "i64"};
      }
    }
    if (u.op == "-" && tv.kind == "f64") {
      if (tv.isConst) return {"", "f64", true, 0, -tv.fConst};
      return {llvm1("fneg", tvalSSA(tv), "f64"), "f64"};
    }
    if (u.op == "~" && tv.kind == "i64") {
      if (tv.isConst) return {"", "i64", true, ~tv.iConst, 0.0};
      return {llvm2("xor", tvalSSA(tv), c64(-1), "i64"), "i64"};
    }
    return {evalExprImpl(u), "aval"};
  }

  TypedVal evalBinaryT(const BinaryExpr& b) {
    // Short-circuit logic, `??`, container membership/range, and the ops
    // with non-trivial runtime semantics (/, //, %, **) stay on the aval
    // path; everything else tries typed scalar ops on same-kind operands.
    if (b.op == "and" || b.op == "&&" || b.binop == BinOp::And ||
        b.op == "or" || b.op == "||" || b.binop == BinOp::Or ||
        b.binop == BinOp::Coalesce || b.binop == BinOp::In ||
        b.binop == BinOp::NotIn || b.binop == BinOp::Range ||
        b.binop == BinOp::Div || b.binop == BinOp::Pow)
      return {evalBinary(b), "aval"};
    TypedVal lt = evalExprT(*b.lhs);
    TypedVal rt = evalExprT(*b.rhs);
    if (lt.kind != "aval" && lt.kind == rt.kind) {
      TypedVal r = typedBinOp(b.binop, lt, rt, lt.kind, b.opSpan);
      if (!r.kind.empty()) return r;
    }
    // mixed/aval operands: box and use the runtime slow op (identical to
    // the untyped path).
    return {emitBinOp(b.binop, boxToAval(lt), boxToAval(rt), b.opSpan), "aval"};
  }

  // `expr?`: a_try_result normalizes; a_is_propagate detects Err/None that
  // must early-return from the enclosing function (after defers), mirroring
  // LlvmEmitter::evalTry.
  std::string evalTry(const TryExpr& te) {
    std::string t = callI64("a_try_result", {evalExpr(*te.operand)});
    std::string isP = callI32T("a_is_propagate", {{"i64", t}});
    // a_is_propagate returns i32; compare against an i32 zero constant
    std::string z32 = nv();
    fn_ << "  " << z32 << " = llvm.mlir.constant(0 : i32) : i32\n";
    std::string r = nv();
    fn_ << "  " << r << " = llvm.icmp \"ne\" " << isP << ", " << z32 << " : i32\n";
    std::string lprop = nl(), lok = nl();
    fn_ << "  llvm.cond_br " << r << ", ^" << lprop << ", ^" << lok << "\n";
    fn_ << "^" << lprop << ":\n";
    terminated_ = false;
    emitPendingDefers();
    callI64("a_promote", {t});
    emitReturn(t);
    fn_ << "^" << lok << ":\n";
    terminated_ = false;
    return t;
  }

  // Emits pattern test; returns i1 SSA name. Binds capture names on success
  // is NOT done here (done in arm body scope). Returns "" on unsupported.
  std::string patternCond(const Pattern& pat, const std::string& subj) {
    switch (pat.kind) {
      case Pattern::Kind::Wildcard: {
        std::string one = nv();
        fn_ << "  " << one << " = llvm.mlir.constant(true) : i1\n";
        return one;
      }
      case Pattern::Kind::Binding: {
        std::string one = nv();
        fn_ << "  " << one << " = llvm.mlir.constant(true) : i1\n";
        bindLocal(pat.name, subj);
        return one;
      }
      case Pattern::Kind::Literal: {
        if (!pat.value) return "";
        std::string lv = evalExpr(*pat.value);
        std::string eq = callI64("a_eq_fast", {subj, lv});
        // a_eq_slow returns aval bool (A_TRUE/A_FALSE) — compare to A_TRUE
        std::string at = c64(kAvalTag | 0x6);
        std::string r = nv();
        fn_ << "  " << r << " = llvm.icmp \"eq\" " << eq << ", " << at << " : i64\n";
        return r;
      }
      case Pattern::Kind::Range: {
        if (!pat.rangeStart || !pat.rangeEnd) return "";
        std::string lo = evalExpr(*pat.rangeStart);
        std::string hi = evalExpr(*pat.rangeEnd);
        std::string ge = callI64("a_ge_fast", {subj, lo});
        std::string le = callI64("a_le_fast", {subj, hi});
        std::string at = c64(kAvalTag | 0x6);
        std::string g = nv(), l = nv(), r = nv();
        fn_ << "  " << g << " = llvm.icmp \"eq\" " << ge << ", " << at << " : i64\n";
        fn_ << "  " << l << " = llvm.icmp \"eq\" " << le << ", " << at << " : i64\n";
        fn_ << "  " << r << " = llvm.mlir.constant(true) : i1\n";
        // and
        std::string rr = nv();
        fn_ << "  " << rr << " = llvm.and " << g << ", " << l << " : i1\n";
        (void)r;
        return rr;
      }
      case Pattern::Kind::Variant: {
        // resolve ctor chain -> variant info
        std::vector<std::string> parts = flattenField(pat.ctor.get());
        const MlVariant* mv = findVariant(parts);
        if (!mv) {
          // unresolvable: whole-value equality against ctor expr
          if (!pat.ctor) return "";
          std::string cv = evalExpr(*pat.ctor);
          std::string eq = callI64("a_eq_fast", {subj, cv});
          std::string at = c64(kAvalTag | 0x6);
          std::string r = nv();
          fn_ << "  " << r << " = llvm.icmp \"eq\" " << eq << ", " << at << " : i64\n";
          return r;
        }
        if (!mv->algebraic) {
          // simple variant: compare whole values
          std::vector<std::string> dummy;
          std::string cv = variantValue(*mv, parts.back(), nullptr);
          std::string eq = callI64("a_eq_fast", {subj, cv});
          std::string at = c64(kAvalTag | 0x6);
          std::string r = nv();
          fn_ << "  " << r << " = llvm.icmp \"eq\" " << eq << ", " << at << " : i64\n";
          return r;
        }
        // algebraic: subj.__tag == tag AND payload subpatterns
        std::vector<std::string> conds;
        {
          std::string tagV = callI64("a_dict_get",
              {subj, stringLiteralAval("__tag"), c64(kAvalTag | 0x2)});
          std::string eq = callI64("a_eq_fast", {tagV, avalInt(mv->tag)});
          std::string at = c64(kAvalTag | 0x6);
          std::string r = nv();
          fn_ << "  " << r << " = llvm.icmp \"eq\" " << eq << ", " << at << " : i64\n";
          conds.push_back(r);
        }
        for (size_t i = 0; i < pat.subpatterns.size() && i < pat.variantFields.size(); ++i) {
          std::string fv = callI64("a_dict_get",
              {subj, stringLiteralAval(pat.variantFields[i]), c64(kAvalTag | 0x2)});
          std::string sc = patternCond(pat.subpatterns[i], fv);
          if (sc.empty()) return "";
          conds.push_back(sc);
        }
        // AND all conds
        std::string acc = conds[0];
        for (size_t i = 1; i < conds.size(); ++i) {
          std::string r = nv();
          fn_ << "  " << r << " = llvm.and " << acc << ", " << conds[i] << " : i1\n";
          acc = r;
        }
        return acc;
      }
      case Pattern::Kind::Tuple: {
        std::vector<std::string> conds;
        for (size_t i = 0; i < pat.subpatterns.size(); ++i) {
          std::string el = callI64("a_getitem", {subj, avalInt((std::int64_t)i)});
          std::string sc = patternCond(pat.subpatterns[i], el);
          if (sc.empty()) return "";
          conds.push_back(sc);
        }
        if (conds.empty()) {
          std::string one = nv();
          fn_ << "  " << one << " = llvm.mlir.constant(true) : i1\n";
          return one;
        }
        std::string acc = conds[0];
        for (size_t i = 1; i < conds.size(); ++i) {
          std::string r = nv();
          fn_ << "  " << r << " = llvm.and " << acc << ", " << conds[i] << " : i1\n";
          acc = r;
        }
        return acc;
      }
      default:
        fail("match pattern kind not supported by the mlir backend yet");
        return "";
    }
  }

  std::string evalMatch(const MatchExpr& m) {
    std::string slot = allocaI64();
    std::string subj = evalExpr(*m.subject);
    std::string lmerge = nl();
    for (size_t ai = 0; ai < m.arms.size(); ++ai) {
      const MatchArm& arm = m.arms[ai];
      std::string lbody = nl(), lnext = nl();
      // test in current block
      std::string cond = patternCond(arm.pattern, subj);
      if (cond.empty()) return "";
      fn_ << "  llvm.cond_br " << cond << ", ^" << lbody << ", ^" << lnext << "\n";
      // body block
      fn_ << "^" << lbody << ":\n";
      terminated_ = false;
      pushScope();
      if (arm.guard) {
        TypedVal gtv = evalExprT(*arm.guard);
        std::string gb = truthyT(gtv);
        std::string ldo = nl(), lskip = nl();
        fn_ << "  llvm.cond_br " << gb << ", ^" << ldo << ", ^" << lskip << "\n";
        fn_ << "^" << ldo << ":\n";
        terminated_ = false;
        {
          std::string rv = evalExpr(*arm.result);
          store(slot, rv);
        }
        if (!terminated_) fn_ << "  llvm.br ^" << lmerge << "\n";
        fn_ << "^" << lskip << ":\n";
        terminated_ = false;
        fn_ << "  llvm.br ^" << lnext << "\n";
      } else {
        {
          std::string rv = evalExpr(*arm.result);
          store(slot, rv);
        }
        if (!terminated_) fn_ << "  llvm.br ^" << lmerge << "\n";
      }
      popScope();
      // next-arm test continues here
      fn_ << "^" << lnext << ":\n";
      terminated_ = false;
    }
    // no arm matched -> None (approximation)
    { std::string kn = c64(kAvalTag | 0x2); store(slot, kn); }
    if (!terminated_) fn_ << "  llvm.br ^" << lmerge << "\n";
    fn_ << "^" << lmerge << ":\n";
    terminated_ = false;
    return load(slot);
  }

  // Build algebraic variant dict {__tag,__name[,payload fields]} or
  // integer value for simple enums. Returns "" if not a variant ref.
  std::string variantValue(const MlVariant& mv, const std::string& vname,
                           const std::vector<std::string>* payload) {
    if (!mv.algebraic) return avalInt(mv.value);
    std::string d = callI64("a_dict_new", {});
    callI64("a_dict_set", {d, stringLiteralAval("__tag"), avalInt(mv.tag)});
    callI64("a_dict_set", {d, stringLiteralAval("__name"), stringLiteralAval(vname)});
    if (payload) {
      size_t n = std::min(payload->size(), mv.payloadFields.size());
      for (size_t i = 0; i < n; ++i)
        callI64("a_dict_set", {d, stringLiteralAval(mv.payloadFields[i]), (*payload)[i]});
    }
    return d;
  }
  // Resolve "Enum.Variant" (2-part) or "mod.Enum.Variant" field chains.
  const MlVariant* findVariant(const std::vector<std::string>& parts) {
    if (parts.size() == 2) {
      auto it = enumVariants_.find(parts[0] + "." + parts[1]);
      return it == enumVariants_.end() ? nullptr : &it->second;
    }
    if (parts.size() >= 3) {
      // last two parts are Enum.Variant
      std::string key = parts[parts.size() - 2] + "." + parts.back();
      auto it = enumVariants_.find(key);
      return it == enumVariants_.end() ? nullptr : &it->second;
    }
    return nullptr;
  }
  std::vector<std::string> flattenField(const Expr* e) {
    std::vector<std::string> parts;
    while (e && e->kind == ExprKind::Field) {
      const FieldExpr& f = *static_cast<const FieldExpr*>(e);
      parts.push_back(f.field);
      e = f.object.get();
    }
    if (e && e->kind == ExprKind::Ident)
      parts.push_back(static_cast<const IdentExpr*>(e)->name);
    std::reverse(parts.begin(), parts.end());
    return parts;
  }

  // obj?.field: None -> None; else unwrap Some, getattr, re-wrap Some
  // unless field is itself optional. Mirrors LlvmEmitter::evalSafeNav.
  std::string evalSafeNav(const SafeNavExpr& x) {
    std::string slot = allocaI64();
    std::string kn = c64(kAvalTag | 0x2);
    std::string obj = evalExpr(*x.object);
    std::string isNone = nv();
    fn_ << "  " << isNone << " = llvm.icmp \"eq\" " << obj << ", " << kn << " : i64\n";
    std::string lnone = nl(), lget = nl(), lmerge = nl();
    fn_ << "  llvm.cond_br " << isNone << ", ^" << lnone << ", ^" << lget << "\n";
    fn_ << "^" << lnone << ":\n";
    terminated_ = false;
    store(slot, kn);
    fn_ << "  llvm.br ^" << lmerge << "\n";
    fn_ << "^" << lget << ":\n";
    terminated_ = false;
    pushScope();
    {
      std::string inner = callI64("a_unwrap", {obj});
      std::string fv = callI64T("a_getattr",
          {{"i64", inner}, {"!llvm.ptr", cstrPtr(x.field)}});
      if (x.fieldOptional) {
        store(slot, fv);
        fn_ << "  llvm.br ^" << lmerge << "\n";
      } else {
        // fv==None -> None; is_option(fv) -> fv; else Some(fv)
        std::string fn2 = nv();
        fn_ << "  " << fn2 << " = llvm.icmp \"eq\" " << fv << ", " << kn << " : i64\n";
        std::string lno2 = nl(), lopt = nl(), lwrap = nl();
        fn_ << "  llvm.cond_br " << fn2 << ", ^" << lno2 << ", ^" << lopt << "\n";
        fn_ << "^" << lno2 << ":\n";
        terminated_ = false;
        store(slot, kn);
        fn_ << "  llvm.br ^" << lmerge << "\n";
        fn_ << "^" << lopt << ":\n";
        terminated_ = false;
        std::string isOpt = callI32T("a_is_option", {{"i64", fv}});
        std::string z32 = nv();
        fn_ << "  " << z32 << " = llvm.mlir.constant(0 : i32) : i32\n";
        std::string isPc = nv();
        fn_ << "  " << isPc << " = llvm.icmp \"ne\" " << isOpt << ", " << z32 << " : i32\n";
        std::string lpassthrough = nl(), lmk = nl();
        fn_ << "  llvm.cond_br " << isPc << ", ^" << lpassthrough << ", ^" << lmk << "\n";
        fn_ << "^" << lpassthrough << ":\n";
        terminated_ = false;
        store(slot, fv);
        fn_ << "  llvm.br ^" << lmerge << "\n";
        fn_ << "^" << lmk << ":\n";
        terminated_ = false;
        std::string sv = callI64("a_some_new", {fv});
        store(slot, sv);
        fn_ << "  llvm.br ^" << lmerge << "\n";
      }
    }
    popScope();
    fn_ << "^" << lmerge << ":\n";
    terminated_ = false;
    return load(slot);
  }

  // fn(args) => body: snapshot-capture visible locals into a closure env,
  // return a fnon value. Mirrors LlvmEmitter::evalLambda/emitLambdaFn.
  std::string evalLambda(const LambdaExpr& x) {
    std::map<std::string, std::string> caps;  // name -> alloca slot
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      for (auto& kv : *it)
        if (!caps.count(kv.first)) caps[kv.first] = kv.second.slot;
    PendingLambda pl;
    pl.expr = &x;
    pl.cname = privPrefix() + "lam_" + std::to_string(lamCounter_++);
    for (auto& kv : caps) pl.captures.push_back(kv.first);
    pl.moduleId = curModuleId_;
    pendingLambdas_.push_back(pl);
    std::string env = callI64("a_closure_env_new", {c64((std::int64_t)pl.captures.size())});
    for (size_t i = 0; i < pl.captures.size(); ++i) {
      std::string cv = load(caps[pl.captures[i]]);
      callI64("a_env_set", {env, c64((std::int64_t)i), cv});
    }
    // fnon(ptrtoint(@lam), env, "<lambda>")
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.addressof @" << pl.cname << " : !llvm.ptr\n";
    std::string fp = nv();
    fn_ << "  " << fp << " = llvm.ptrtoint " << r << " : !llvm.ptr to i64\n";
    std::string nm = cstrPtr("<lambda>");
    return callI64T("a_fnon_new", {{"i64", fp}, {"i64", env}, {"!llvm.ptr", nm}});
  }

  // ---- auto-parfor (port of LlvmEmitter's emitForParfor/emitParforFn) ----
  // pfDenyName/parforEnabled live in mlir_util.hpp (shared with the
  // boxed llvm-dialect emitter).
  // name resolves to a binding that would be shared under capture
  bool pfOuterBinding(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      if (it->count(name)) return true;
    return !globalSymFor(name).empty() || fnCnames_.count(name);
  }
  bool pfExprOk(const Expr* e) {
    if (!e) return true;
    switch (e->kind) {
      case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::BoolLit:
      case ExprKind::CharLit: case ExprKind::StringLit: case ExprKind::Ident:
        return true;
      case ExprKind::Unary: return pfExprOk(static_cast<const UnaryExpr*>(e)->operand.get());
      case ExprKind::Binary: {
        const auto& b = *static_cast<const BinaryExpr*>(e);
        return pfExprOk(b.lhs.get()) && pfExprOk(b.rhs.get());
      }
      case ExprKind::Call: {
        const auto& c = *static_cast<const CallExpr*>(e);
        if (c.callee->kind == ExprKind::Ident) {
          if (pfDenyName(static_cast<const IdentExpr*>(c.callee.get())->name)) return false;
        } else if (c.callee->kind == ExprKind::Field) {
          const FieldExpr& f = *static_cast<const FieldExpr*>(c.callee.get());
          if (pfDenyName(f.field)) return false;
          if (!pfExprOk(f.object.get())) return false;
        } else if (!pfExprOk(c.callee.get())) return false;
        for (const CallArg& a : c.args) if (!pfExprOk(a.value.get())) return false;
        return true;
      }
      case ExprKind::Index: {
        const auto& x = *static_cast<const IndexExpr*>(e);
        return pfExprOk(x.object.get()) && pfExprOk(x.index.get());
      }
      case ExprKind::Slice: {
        const auto& x = *static_cast<const SliceExpr*>(e);
        return pfExprOk(x.object.get()) && pfExprOk(x.start.get()) &&
               pfExprOk(x.end.get()) && pfExprOk(x.step.get());
      }
      case ExprKind::Field: return pfExprOk(static_cast<const FieldExpr*>(e)->object.get());
      case ExprKind::SafeNav: return pfExprOk(static_cast<const SafeNavExpr*>(e)->object.get());
      case ExprKind::ArrayLit:
        for (const ExprPtr& el : static_cast<const ArrayLitExpr*>(e)->elements)
          if (!pfExprOk(el.get())) return false;
        return true;
      case ExprKind::Tuple:
        for (const ExprPtr& el : static_cast<const TupleExpr*>(e)->elements)
          if (!pfExprOk(el.get())) return false;
        return true;
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr*>(e)->entries)
          if (!pfExprOk(kv.first.get()) || !pfExprOk(kv.second.get())) return false;
        return true;
      case ExprKind::SetLit:
        for (const ExprPtr& el : static_cast<const SetLitExpr*>(e)->elements)
          if (!pfExprOk(el.get())) return false;
        return true;
      case ExprKind::Range: {
        const auto& x = *static_cast<const RangeExpr*>(e);
        return pfExprOk(x.start.get()) && pfExprOk(x.end.get()) && pfExprOk(x.step.get());
      }
      case ExprKind::Ternary: {
        const auto& x = *static_cast<const TernaryExpr*>(e);
        return pfExprOk(x.cond.get()) && pfExprOk(x.thenValue.get()) && pfExprOk(x.elseValue.get());
      }
      case ExprKind::Comprehension: {
        const auto& x = *static_cast<const ComprehensionExpr*>(e);
        return pfExprOk(x.iterable.get()) && pfExprOk(x.element.get()) &&
               (!x.condition || pfExprOk(x.condition.get()));
      }
      default: return false;
    }
  }
  bool pfStmtOk(const Stmt* st, const std::string& loopVar) {
    if (!st) return true;
    switch (st->kind) {
      case StmtKind::Let: {
        const auto& l = *static_cast<const LetStmt*>(st);
        return !l.init || pfExprOk(l.init.get());
      }
      case StmtKind::Destructure: {
        const auto& d = *static_cast<const DestructureStmt*>(st);
        return !d.value || pfExprOk(d.value.get());
      }
      case StmtKind::Assign: {
        const auto& a = *static_cast<const AssignStmt*>(st);
        const Expr* t = a.target.get();
        if (t->kind == ExprKind::Ident) {
          if (pfOuterBinding(static_cast<const IdentExpr*>(t)->name)) return false;
        } else if (t->kind == ExprKind::Index) {
          const auto& ix = *static_cast<const IndexExpr*>(t);
          bool disjoint = ix.index->kind == ExprKind::Ident &&
              static_cast<const IdentExpr*>(ix.index.get())->name == loopVar;
          bool localBase = ix.object->kind == ExprKind::Ident &&
              !pfOuterBinding(static_cast<const IdentExpr*>(ix.object.get())->name);
          if (!disjoint && !localBase) return false;
          if (!pfExprOk(ix.object.get()) || !pfExprOk(ix.index.get())) return false;
        } else return false;
        return pfExprOk(a.value.get());
      }
      case StmtKind::Expr:
        return pfExprOk(static_cast<const ExprStmt*>(st)->expr.get());
      case StmtKind::If: {
        const auto& f = *static_cast<const IfStmt*>(st);
        for (const auto& br : f.branches) {
          if (!pfExprOk(br.cond.get())) return false;
          for (const StmtPtr& b : br.body) if (!pfStmtOk(b.get(), loopVar)) return false;
        }
        for (const StmtPtr& b : f.elseBody) if (!pfStmtOk(b.get(), loopVar)) return false;
        return true;
      }
      default: return false;
    }
  }
  bool parforEligible(const ForStmt& s) {
    if (!parforEnabled() || !inFnBody_ || s.body.empty()) return false;
    const Expr* it = s.iterable.get();
    if (!it || it->kind != ExprKind::Call) return false;
    const auto& call = *static_cast<const CallExpr*>(it);
    if (call.callee->kind != ExprKind::Ident ||
        static_cast<const IdentExpr*>(call.callee.get())->name != "range") return false;
    for (const StmtPtr& b : s.body) if (!pfStmtOk(b.get(), s.var)) return false;
    return true;
  }

  // spawn loop: for each item, a_fib_spawn_handle(fnval,1,&arg) -> hs list
  void emitForParfor(const ForStmt& s) {
    // capture visible locals (same rule as evalLambda)
    std::map<std::string, std::string> caps;
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      for (auto& kv : *it)
        if (!caps.count(kv.first)) caps[kv.first] = kv.second.slot;
    PendingParfor pf;
    pf.stmt = &s;
    pf.cname = privPrefix() + "pf_" + std::to_string(parforCounter_++);
    for (auto& kv : caps) pf.capNames.push_back(kv.first);
    pf.moduleId = curModuleId_;
    pendingParfors_.push_back(pf);

    std::string env = callI64("a_closure_env_new", {c64((std::int64_t)caps.size())});
    { size_t i = 0;
      for (auto& kv : caps) {
        callI64("a_env_set", {env, c64((std::int64_t)i++), load(kv.second)});
      } }
    // fnon(ptrtoint(@pf), env, "<parfor>")
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.addressof @" << pf.cname << " : !llvm.ptr\n";
    std::string fp = nv();
    fn_ << "  " << fp << " = llvm.ptrtoint " << r << " : !llvm.ptr to i64\n";
    std::string nm = cstrPtr("<parfor>");
    std::string fnval = callI64T("a_fnon_new",
        {{"i64", fp}, {"i64", env}, {"!llvm.ptr", nm}});
    std::string fnSlot = allocaI64();
    pin(fnSlot);
    store(fnSlot, fnval);

    // it = a_iter_new(range(...)); hs = []
    std::string it = callI64("a_iter_new", {evalExpr(*s.iterable)});
    std::string itSlot = allocaI64(); pin(itSlot); store(itSlot, it);
    std::string hs = callI64T("a_list_new", {{"i64", c64(0)}, {"!llvm.ptr", nullPtr()}});
    std::string hsSlot = allocaI64(); pin(hsSlot); store(hsSlot, hs);
    std::string argArr = allocaArr(1); pin(argArr);

    std::string lhead = nl(), lbody = nl(), lend = nl();
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lhead << ":\n";
    terminated_ = false;
    {
      std::string itv = load(itSlot);
      std::string nx = callI64T("a_iter_next",
          {{"i64", itv}, {"!llvm.ptr", gepIdx(itemSlotTmp(), c64(0))}});
      std::string zero = c64(0);
      std::string b = nv();
      fn_ << "  " << b << " = llvm.icmp \"ne\" " << nx << ", " << zero << " : i64\n";
      fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lend << "\n";
    }
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    {
      // bind loop var from item slot
      std::string iv = load(itemSlotTmp());
      bindLocal(s.var, iv);
      // args[0] = iv; spawn
      storeIdx(argArr, c64(0), iv);
      std::string fnv = load(fnSlot);
      std::string h = callI64T("a_fib_spawn_handle",
          {{"i64", fnv}, {"i64", c64(1)}, {"!llvm.ptr", argArr}});
      std::string hsr = load(hsSlot);
      callI64("a_list_append", {hsr, h});
    }
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lend << ":\n";
    terminated_ = false;

    // join loop: for h in hs: a_fib_join_handle(h)
    std::string it2 = callI64("a_iter_new", {load(hsSlot)});
    std::string it2Slot = allocaI64(); pin(it2Slot); store(it2Slot, it2);
    std::string h2 = nl(), b2 = nl(), e2 = nl();
    fn_ << "  llvm.br ^" << h2 << "\n";
    fn_ << "^" << h2 << ":\n";
    terminated_ = false;
    {
      std::string itv = load(it2Slot);
      std::string is2 = itemSlotTmp2();
      std::string nx = callI64T("a_iter_next",
          {{"i64", itv}, {"!llvm.ptr", gepIdx(is2, c64(0))}});
      std::string zero = c64(0);
      std::string bb = nv();
      fn_ << "  " << bb << " = llvm.icmp \"ne\" " << nx << ", " << zero << " : i64\n";
      fn_ << "  llvm.cond_br " << bb << ", ^" << b2 << ", ^" << e2 << "\n";
    }
    fn_ << "^" << b2 << ":\n";
    terminated_ = false;
    {
      std::string hv = load(itemSlotTmp2());
      callI64("a_fib_join_handle", {hv});
    }
    if (!terminated_) fn_ << "  llvm.br ^" << h2 << "\n";
    fn_ << "^" << e2 << ":\n";
    terminated_ = false;
  }

  std::string itemSlotTmp() {
    if (itemSlot_ == "") itemSlot_ = allocaI64();
    return itemSlot_;
  }
  std::string itemSlotTmp2() {
    if (itemSlot2_ == "") itemSlot2_ = allocaI64();
    return itemSlot2_;
  }
  void storeIdx(const std::string& arr, const std::string& idx, const std::string& v) {
    std::string p = gepIdx(arr, idx);
    store(p, v);
  }
  std::string itemSlot_, itemSlot2_;

  void emitParforFn(const PendingParfor& pf) {
    const ForStmt& x = *pf.stmt;
    FnDecl dummy;
    dummy.moduleId = pf.moduleId;
    beginFunction(pf.cname, &dummy);
    // env = argv[0]
    std::string env = load(gepIdx("%argv", c64(0)));
    // captures -> locals
    for (size_t i = 0; i < pf.capNames.size(); ++i) {
      std::string cv = callI64("a_env_get", {env, c64((std::int64_t)i)});
      bindLocal(pf.capNames[i], cv);
    }
    // loop var = argv[1]
    bindLocal(x.var, load(gepIdx("%argv", c64(1))));
    inFnBody_ = true; inUserFn_ = true;
    emitStatements(x.body);
    inFnBody_ = false; inUserFn_ = false;
    emitPendingDefers();
    std::string kn = c64(kAvalTag | 0x2);
    emitReturn(kn);
    fn_ << "}\n\n";
    flushFunction();
  }

  void emitLambdas() {
    while (!failed_ && !pendingLambdas_.empty()) {
      PendingLambda pl = pendingLambdas_.back();
      pendingLambdas_.pop_back();
      if (emitOnlyModule_ >= 0 && pl.moduleId != emitOnlyModule_) continue;
      const LambdaExpr& x = *pl.expr;
      FnDecl dummy;
      dummy.moduleId = pl.moduleId;
      beginFunction(pl.cname, &dummy);
      // env = argv[0]
      std::string env = load(gepIdx("%argv", c64(0)));
      // captures -> locals
      for (size_t i = 0; i < pl.captures.size(); ++i) {
        std::string cv = callI64("a_env_get", {env, c64((std::int64_t)i)});
        bindLocal(pl.captures[i], cv);
      }
      // params -> argv[i+1]
      for (size_t i = 0; i < x.params.size(); ++i) {
        std::string v = load(gepIdx("%argv", c64((std::int64_t)i + 1)));
        bindLocal(x.params[i].name, v);
      }
      std::string rv = evalExpr(*x.body);
      emitReturn(rv);
      fn_ << "}\n\n";
      flushFunction();
    }
    while (!failed_ && !pendingParfors_.empty()) {
      PendingParfor pf = pendingParfors_.back();
      pendingParfors_.pop_back();
      if (emitOnlyModule_ >= 0 && pf.moduleId != emitOnlyModule_) continue;
      emitParforFn(pf);
    }
  }

  // [expr for x in iter if cond] / {expr for ...}: desugar to iterator
  // loop + collection append. Mirrors the for-statement lowering.
  std::string evalComprehension(const ComprehensionExpr& cp) {
    std::string coll = evalExpr(*cp.iterable);
    if (coll.empty()) return "";
    std::string acc;
    if (cp.isSet) acc = callI64("a_set_new", {});
    else acc = callI64T("a_list_new", {{"i64", c64(0)}, {"!llvm.ptr", nullPtr()}});
    std::string it = callI64("a_iter_new", {coll});
    std::string itemSlot = allocaI64();
    pin(itemSlot);
    std::string lhead = nl(), lbody = nl(), lend = nl();
    fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lhead << ":\n";
    terminated_ = false;
    std::string nx = callI64T("a_iter_next",
                              {{"i64", it}, {"!llvm.ptr", gepIdx(itemSlot, c64(0))}});
    std::string b = nv();
    { std::string z = c64(0);
      fn_ << "  " << b << " = llvm.icmp \"ne\" " << nx << ", " << z << " : i64\n"; }
    fn_ << "  llvm.cond_br " << b << ", ^" << lbody << ", ^" << lend << "\n";
    fn_ << "^" << lbody << ":\n";
    terminated_ = false;
    pushScope();
    {
      MlBinding mb;
      mb.slot = itemSlot;
      scopes_.back()[cp.var] = mb;
    }
    // optional filter
    bool doAppend = true;
    if (cp.condition) {
      TypedVal ctv = evalExprT(*cp.condition);
      std::string cb = truthyT(ctv);
      std::string lskip = nl(), lapp = nl();
      fn_ << "  llvm.cond_br " << cb << ", ^" << lapp << ", ^" << lskip << "\n";
      fn_ << "^" << lskip << ":\n";
      terminated_ = false;
      fn_ << "  llvm.br ^" << lhead << "\n";
      fn_ << "^" << lapp << ":\n";
      terminated_ = false;
    }
    {
      std::string ev = evalExpr(*cp.element);
      if (cp.isSet) callI64("a_set_add", {acc, ev});
      else callI64("a_list_append", {acc, ev});
    }
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lhead << "\n";
    fn_ << "^" << lend << ":\n";
    terminated_ = false;
    (void)doAppend;
    return acc;
  }

  // NUL-terminated C string global for a_method names etc. Returns the
  // global symbol; caller emits a.addressof in the function.
  std::string cstrGlobalSym(const std::string& text) {
    std::string bytes = text;
    bytes.push_back('\0');
    std::string g = privPrefix() + "cstr" + std::to_string(strCounter_++);
    pre_ << "  llvm.mlir.global private constant @" << g << "() : !llvm.array<"
         << bytes.size() << " x i8> {\n";
    pre_ << "    %u = llvm.mlir.undef : !llvm.array<" << bytes.size() << " x i8>\n";
    std::string cur = "%u";
    for (size_t i = 0; i < bytes.size(); ++i) {
      std::string cv = "%cb" + std::to_string(i);
      pre_ << "    " << cv << " = llvm.mlir.constant(" << (int)(unsigned char)bytes[i]
           << " : i8) : i8\n";
      std::string nx = "%r" + std::to_string(i);
      pre_ << "    " << nx << " = llvm.insertvalue " << cv << ", " << cur << "["
           << i << "] : !llvm.array<" << bytes.size() << " x i8>\n";
      cur = nx;
    }
    pre_ << "    llvm.return " << cur << " : !llvm.array<" << bytes.size() << " x i8>\n";
    pre_ << "  }\n";
    return g;
  }
  std::string cstrPtr(const std::string& text) {
    std::string g = cstrGlobalSym(text);
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
    return r;
  }

  // python:module.member(args...) -> py_call builtin (id 162).
  // argv = [module_str, member_str, nkw, pos..., (kname, kval)*]
  std::string evalPyCall(const std::string& module, const std::string& member,
                         const CallExpr& call) {
    std::vector<std::string> pos, named;
    for (const CallArg& a : call.args) {
      if (a.name.empty()) pos.push_back(evalExpr(*a.value));
      else { named.push_back(a.name); named.push_back(evalExpr(*a.value)); }
    }
    int nkw = (int)named.size() / 2;
    std::vector<std::string> payload;
    payload.push_back(stringLiteralAval(module));
    payload.push_back(stringLiteralAval(member));
    payload.push_back(avalInt(nkw));
    for (auto& v : pos) payload.push_back(v);
    for (size_t i = 0; i < named.size(); i += 2) {
      payload.push_back(stringLiteralAval(named[i]));
      payload.push_back(named[i + 1]);
    }
    std::string av = argvPack(payload);
    return callI64T("a_bi_call",
        {{"i64", c64(162)}, {"i64", c64((std::int64_t)payload.size())},
         {"!llvm.ptr", av}});
  }

  // obj.name(args...) -> a_method(obj, "name", npos, nkw, argv)
  // named args become (name,value) aval pairs appended after positionals.
  std::string evalMethodCall(const Expr& objExpr, const std::string& field,
                             const CallExpr& call) {
    std::string obj = evalExpr(objExpr);
    if (obj.empty()) return "";
    std::string nm = cstrPtr(field);
    std::vector<std::string> pos, named;
    for (const CallArg& a : call.args) {
      if (a.name.empty()) pos.push_back(evalExpr(*a.value));
      else {
        named.push_back(stringLiteralAval(a.name));
        named.push_back(evalExpr(*a.value));
      }
    }
    // variadic method: pack args beyond fixed params into a list.
    // Resolve obj's struct type -> method cname -> FnDecl.
    int varFixed = -1;  // -1 = not variadic; else # fixed params (excl self)
    int methFixed = -1;   // non-variadic fixed params (excl self), -1 unknown
    bool methHasDefault = false;
    {
      TypePtr ot = objExpr.inferredType ? resolveType(objExpr.inferredType) : nullptr;
      if (ot && ot->kind == TypeKind::Struct) {
        std::string sn = shortName(ot->structName);
        auto si = structByName_.find(sn);
        if (si == structByName_.end()) si = structByName_.find(ot->structName);
        if (si != structByName_.end()) {
          for (const auto& mp : structs_[si->second].methods)
            if (mp.first == field) {
              auto di = methodDecls_.find(mp.second);
              if (di != methodDecls_.end()) {
                const FnDecl* md = di->second;
                if (!md->params.empty() && md->params.back().variadic) {
                  // fixed = params minus self minus variadic
                  varFixed = (int)md->params.size() - 2;
                  if (varFixed < 0) varFixed = 0;
                } else {
                  methFixed = (int)md->params.size() - 1;  // excl self
                  for (size_t pi = 1; pi < md->params.size(); ++pi)
                    if (md->params[pi].defaultValue) methHasDefault = true;
                }
              }
              break;
            }
        }
      }
    }
    if (varFixed >= 0) {
      // keep first varFixed positional args; pack the rest (maybe empty) into a list
      int kp = varFixed < (int)pos.size() ? varFixed : (int)pos.size();
      std::vector<std::string> keep(pos.begin(), pos.begin() + kp);
      std::vector<std::string> extras(pos.begin() + kp, pos.end());
      std::string exav = argvPack(extras);
      std::string exlist = callI64T("a_list_new",
          {{"i64", c64((std::int64_t)extras.size())}, {"!llvm.ptr", exav}});
      keep.push_back(exlist);
      pos = keep;
    }
    // pad missing args for methods with default params -> A_MISSING
    if (varFixed < 0 && methHasDefault && methFixed > 0 &&
        (int)pos.size() < methFixed) {
      std::string km = c64(kAvalTag);
      while ((int)pos.size() < methFixed) pos.push_back(km);
    }
    int nkw = (int)named.size() / 2;
    int total = (int)pos.size() + (int)named.size();
    std::string av = argvPack(total > 0 ? [&]{
      std::vector<std::string> all = pos; all.insert(all.end(), named.begin(), named.end()); return all;
    }() : std::vector<std::string>{});
    if (total == 0) { av = nullPtr(); }
    return callI64T("a_method",
        {{"i64", obj}, {"!llvm.ptr", nm}, {"i64", c64((std::int64_t)pos.size())},
         {"i64", c64((std::int64_t)nkw)}, {"!llvm.ptr", av}});
  }

  std::string evalStringLit(const StringLitExpr& se) {
    // concat parts: text -> a_str_new, expr -> a_str(eval)
    std::string acc;
    bool first = true;
    for (const StringPart& p : se.parts) {
      std::string pv;
      if (p.isExpr) {
        pv = callI64("a_str", {evalExpr(*p.expr)});
      } else {
        pv = stringLiteralAval(p.text);
      }
      if (first) { acc = pv; first = false; }
      else acc = callI64("a_str_concat", {acc, pv});
    }
    if (first) return stringLiteralAval("");
    return acc;
  }

  // ++/-- on ident/index/field targets. Returns old (postfix) or new (prefix).
  std::string emitIncDec(const Expr& target, bool inc, bool prefix,
                         const UnaryExpr& u) {
    auto bump = [&](const std::string& cur) {
      return callI64(inc ? "a_add_fast" : "a_sub_fast", {cur, avalInt(1)});
    };
    if (target.kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr&>(target).name;
      bool isG = false;
      std::string cur = bindingValue(nm, isG);
      if (cur.empty()) { failAt("++/-- on unbound name '" + nm + "'", &u); return ""; }
      std::string nv2 = bump(cur);
      storeBinding(nm, nv2);
      return prefix ? nv2 : cur;
    }
    if (target.kind == ExprKind::Index) {
      const IndexExpr& ix = static_cast<const IndexExpr&>(target);
      std::string obj = evalExpr(*ix.object);
      std::string idx = evalExpr(*ix.index);
      std::string cur = callI64("a_getitem", {obj, idx});
      std::string nv2 = bump(cur);
      callI64("a_setitem", {obj, idx, nv2});
      return prefix ? nv2 : cur;
    }
    if (target.kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(target);
      std::string obj = evalExpr(*fx.object);
      std::string namev = cstrPtr(fx.field);
      std::string cur = callI64T("a_getattr", {{"i64", obj}, {"!llvm.ptr", namev}});
      std::string nv2 = bump(cur);
      callI64T("a_setattr", {{"i64", obj}, {"!llvm.ptr", namev}, {"i64", nv2}});
      return prefix ? nv2 : cur;
    }
    failAt("++/-- target not supported", &u);
    return "";
  }

  std::string evalUnary(const UnaryExpr& u) {
    if (u.postfix && (u.op == "++" || u.op == "--")) {
      return emitIncDec(*u.operand, u.op == "++", /*prefix=*/false, u);
    }
    if (u.postfix) {
      if (u.op == "!") return callI64("a_unwrap", {evalExpr(*u.operand)});
      failAt("postfix ++/-- not supported", &u); return "";
    }
    std::string v = evalExpr(*u.operand);
    if (u.op == "-") return callI64("a_neg_fast", {v});
    if (u.op == "~") return callI64("a_bnot_fast", {v});
    if (u.op == "!") return callI64("a_unwrap", {v});
    if (u.op == "not") return callI64("a_not_fast", {v});
    if (u.op == "++" || u.op == "--") {
      return emitIncDec(*u.operand, u.op == "++", /*prefix=*/true, u);
    }
    failAt("unary op '" + u.op + "' not supported", &u);
    return "";
  }

  std::string emitBinOp(BinOp op, const std::string& l, const std::string& r,
                        const Span& span) {
    const char* sym = nullptr;
    switch (op) {
      case BinOp::Add: sym = "a_add_fast"; break;
      case BinOp::Sub: sym = "a_sub_fast"; break;
      case BinOp::Mul: sym = "a_mul_fast"; break;
      case BinOp::Div: sym = "a_div_fast"; break;
      case BinOp::FloorDiv: sym = "a_floordiv_fast"; break;
      case BinOp::Mod: sym = "a_mod_fast"; break;
      case BinOp::Pow: sym = "a_pow_fast"; break;
      case BinOp::Eq: sym = "a_eq_fast"; break;
      case BinOp::Ne: sym = "a_ne_fast"; break;
      case BinOp::Lt: sym = "a_lt_fast"; break;
      case BinOp::Le: sym = "a_le_fast"; break;
      case BinOp::Gt: sym = "a_gt_fast"; break;
      case BinOp::Ge: sym = "a_ge_fast"; break;
      case BinOp::BitAnd: sym = "a_band_fast"; break;
      case BinOp::BitOr: sym = "a_bor_fast"; break;
      case BinOp::BitXor: sym = "a_bxor_fast"; break;
      case BinOp::Shl: sym = "a_shl_fast"; break;
      case BinOp::Shr: sym = "a_shr_fast"; break;
      case BinOp::In: {
        // a_contains(container, item) — rhs is container
        return callI64("a_contains", {r, l});
      }
      case BinOp::NotIn: {
        std::string c = callI64("a_contains", {r, l});
        return callI64("a_not_fast", {c});
      }
      case BinOp::Range:
        return callI64("a_range_new", {l, r, avalInt(1)});
      default: break;
    }
    if (sym) return callI64(sym, {l, r});
    (void)span;
    fail("binary operator not supported by the mlir backend yet");
    return "";
  }

  std::string evalBinary(const BinaryExpr& b) {
    // and/or short-circuit (word and symbol forms)
    if (b.op == "and" || b.op == "&&" || b.binop == BinOp::And)
      return logicExpr(*b.lhs, *b.rhs, true);
    if (b.op == "or" || b.op == "||" || b.binop == BinOp::Or)
      return logicExpr(*b.lhs, *b.rhs, false);
    if (b.binop == BinOp::Coalesce) return coalesceExpr(*b.lhs, *b.rhs);
    return emitBinOp(b.binop, evalExpr(*b.lhs), evalExpr(*b.rhs), b.opSpan);
  }

  // `l ?? r`: l is None -> r, else a_unwrap(l) (Some/Ok payload or l itself)
  std::string coalesceExpr(const Expr& lhs, const Expr& rhs) {
    std::string slot = allocaI64();
    std::string lv = evalExpr(lhs);
    std::string isNone = nv();
    { std::string kn = c64(kAvalTag | 0x2);
      fn_ << "  " << isNone << " = llvm.icmp \"eq\" " << lv << ", " << kn << " : i64\n"; }
    std::string lr = nl(), le = nl(), lm = nl();
    fn_ << "  llvm.cond_br " << isNone << ", ^" << lr << ", ^" << le << "\n";
    fn_ << "^" << lr << ":\n";
    terminated_ = false;
    pushScope();
    store(slot, evalExpr(rhs));
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lm << "\n";
    fn_ << "^" << le << ":\n";
    terminated_ = false;
    pushScope();
    store(slot, callI64("a_unwrap", {lv}));
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lm << "\n";
    fn_ << "^" << lm << ":\n";
    terminated_ = false;
    return load(slot);
  }

  // `and`: truthy(lhs) ? rhs : lhs ; `or`: truthy(lhs) ? lhs : rhs
  std::string logicExpr(const Expr& lhs, const Expr& rhs, bool isAnd) {
    std::string slot = allocaI64();
    std::string lv = evalExpr(lhs);
    store(slot, lv);
    std::string lb = truthyI1(lv);
    std::string lr = nl(), le = nl();
    if (isAnd) {
      fn_ << "  llvm.cond_br " << lb << ", ^" << lr << ", ^" << le << "\n";
      fn_ << "^" << lr << ":\n";
      terminated_ = false;
      pushScope();
      store(slot, evalExpr(rhs));
      popScope();
      if (!terminated_) fn_ << "  llvm.br ^" << le << "\n";
      fn_ << "^" << le << ":\n";
    } else {
      fn_ << "  llvm.cond_br " << lb << ", ^" << le << ", ^" << lr << "\n";
      fn_ << "^" << lr << ":\n";
      terminated_ = false;
      pushScope();
      store(slot, evalExpr(rhs));
      popScope();
      if (!terminated_) fn_ << "  llvm.br ^" << le << "\n";
      fn_ << "^" << le << ":\n";
    }
    terminated_ = false;
    return load(slot);
  }

  // if-expression helper
  std::string ifExprResult(const Expr& cond, const Expr& thenE, const Expr& elseE) {
    std::string slot = allocaI64();
    std::string c = evalExpr(cond);
    std::string b = truthyI1(c);
    std::string lt = nl(), le = nl(), lm = nl();
    fn_ << "  llvm.cond_br " << b << ", ^" << lt << ", ^" << le << "\n";
    fn_ << "^" << lt << ":\n";
    terminated_ = false;
    pushScope();
    store(slot, evalExpr(thenE));
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lm << "\n";
    fn_ << "^" << le << ":\n";
    terminated_ = false;
    pushScope();
    store(slot, evalExpr(elseE));
    popScope();
    if (!terminated_) fn_ << "  llvm.br ^" << lm << "\n";
    fn_ << "^" << lm << ":\n";
    terminated_ = false;
    return load(slot);
  }

  std::string truthyI1(const std::string& aval) {
    std::string t = callI64("a_truthy_raw_fast", {aval});
    std::string r = nv();
    { std::string z = c64(0);
      fn_ << "  " << r << " = llvm.icmp \"ne\" " << t << ", " << z << " : i64\n"; }
    return r;
  }

  // ---- typed scalars (A_MLIR_TYPED=1) -----------------------------------------
  // Unboxed i64/f64/i1 values flow only inside evalExprT-produced expression
  // trees and typed local slots. Every legacy aval consumer goes through
  // boxToAval (typed -> raw i64 aval word); every aval producer feeding a
  // typed slot goes through unboxFromAval. NOTE: `a.to_aval`/`a.from_aval`
  // are deliberately NOT emitted: their !a.aval result/operand type cannot
  // feed the i64-pinned llvm.store/load/return/argv consumers of this
  // emitter at MLIR text level, and `a.from_aval` inside SingleBlock regions
  // (llvm.func bodies) requires an already-i64 operand (ALowerTyped.cpp).
  // The inline sequences below replicate ALowerTyped's emitBoxI64/
  // emitBoxFloat/emitBoxBool1 bit-for-bit.

  // Sema scalar type -> MLIR scalar kind ("" = not an M1 scalar). All int
  // widths map to i64: Sema's i32/uN annotations are not range-enforced at
  // runtime (the aval payload is a full i64), so narrowing would corrupt.
  static std::string scalarKindOfT(TypePtr t) {
    if (!t) return "";
    t = resolveType(t);
    if (!t) return "";
    switch (t->kind) {
      case TypeKind::Int: return "i64";
      case TypeKind::Float: return "f64";
      case TypeKind::Bool: return "i1";
      default: return "";
    }
  }
  static int scalarKindBit(const std::string& kind) {
    if (kind == "i64") return 1;
    if (kind == "f64") return 2;
    if (kind == "i1") return 4;
    return 8;  // aval/unknown/other
  }

  std::string ci1(bool b) {
    std::string r = nv();
    fn_ << "  " << r << " = llvm.mlir.constant(" << (b ? 1 : 0) << " : i1) : i1\n";
    return r;
  }
  // f64 constant via i64 bitcast: exact for nan/inf/-0.0, no float-parser
  // edge cases.
  std::string emitF64Const(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, 8);
    std::string i = c64((std::int64_t)u);
    std::string r = nv();
    fn_ << "  " << r << " = llvm.bitcast " << i << " : i64 to f64\n";
    return r;
  }
  std::string f64Zero() { return emitF64Const(0.0); }

  // Materialize a TypedVal as an SSA name of its kind (consts emitted lazily).
  std::string tvalSSA(const TypedVal& v) {
    if (!v.isConst) return v.name;
    if (v.kind == "i64") return c64(v.iConst);
    if (v.kind == "f64") return emitF64Const(v.fConst);
    if (v.kind == "i1") return ci1(v.iConst != 0);
    return v.name;
  }

  // typed scalar -> raw i64 aval word (the phase-1 boxed representation).
  std::string boxToAval(const TypedVal& v) {
    if (v.kind == "aval" || (v.name.empty() && !v.isConst)) return v.name;
    if (v.isConst) {
      if (v.kind == "i64") return avalInt(v.iConst);
      if (v.kind == "f64") return avalDbl(v.fConst);
      if (v.kind == "i1") return avalBool(v.iConst != 0);
      return v.name;
    }
    if (v.kind == "i64") {
      // emitBoxI64: inline tag encoding in [-2^46, 2^46), else @a_i64n.
      std::string nlim = c64(-kInlineLim), plim = c64(kInlineLim);
      std::string lo = nv(), hi = nv(), inR = nv();
      fn_ << "  " << lo << " = llvm.icmp \"sge\" " << v.name << ", " << nlim << " : i64\n";
      fn_ << "  " << hi << " = llvm.icmp \"slt\" " << v.name << ", " << plim << " : i64\n";
      fn_ << "  " << inR << " = llvm.and " << lo << ", " << hi << " : i1\n";
      std::string one = c64(1), shl = nv(), pay = nv(), mask = nv(), tag = nv(), enc = nv();
      fn_ << "  " << shl << " = llvm.shl " << v.name << ", " << one << " : i64\n";
      fn_ << "  " << pay << " = llvm.or " << shl << ", " << one << " : i64\n";
      fn_ << "  " << mask << " = llvm.mlir.constant(281474976710655 : i64) : i64\n";
      // `pay` (the or-result) is the and's LEFT operand; the result needs a
      // fresh name — reusing `pay` here emitted `llvm.and %vN, ...` with
      // %vN as its own operand (use-before-def).
      std::string masked = nv();
      fn_ << "  " << masked << " = llvm.and " << pay << ", " << mask << " : i64\n";
      fn_ << "  " << tag << " = llvm.mlir.constant(" << kAvalTag << " : i64) : i64\n";
      enc = nv();
      fn_ << "  " << enc << " = llvm.or " << tag << ", " << masked << " : i64\n";
      // NOTE: keep this straight-line — boxToAval runs in contexts where a
      // cond_br's successor labels are emitted later, so it must not open
      // blocks of its own.
      std::string boxed = callI64("a_i64n", {v.name});
      std::string r = nv();
      fn_ << "  " << r << " = llvm.select " << inR << ", " << enc << ", " << boxed << " : i1, i64\n";
      return r;
    }
    if (v.kind == "f64") {
      // NaN canonicalization (runtime mkdouble) then bitcast to i64.
      std::string isNaN = nv();
      fn_ << "  " << isNaN << " = llvm.fcmp \"uno\" " << v.name << ", " << v.name << " : f64\n";
      std::string nanBits = c64((std::int64_t)0x7FF8000000000000ULL);
      std::string nanI = nv(), canon = nv(), sel = nv(), r = nv();
      fn_ << "  " << nanI << " = llvm.bitcast " << nanBits << " : i64 to f64\n";
      fn_ << "  " << sel << " = llvm.select " << isNaN << ", " << nanI << ", " << v.name << " : i1, f64\n";
      fn_ << "  " << r << " = llvm.bitcast " << sel << " : f64 to i64\n";
      return r;
    }
    if (v.kind == "i1") {
      // select(A_TRUE, A_FALSE) on (v == true)
      std::string t = ci1(true);
      std::string isT = nv();
      fn_ << "  " << isT << " = llvm.icmp \"eq\" " << v.name << ", " << t << " : i1\n";
      std::string tv = c64(kAvalTag + 6), fv = c64(kAvalTag + 4), r = nv();
      fn_ << "  " << r << " = llvm.select " << isT << ", " << tv << ", " << fv << " : i1, i64\n";
      return r;
    }
    return v.name;
  }

  // aval i64 word -> typed scalar (unchecked ABI unpack; only emitted where
  // Sema proved the dynamic type, mirroring a.from_aval's contract).
  TypedVal unboxFromAval(const std::string& aval, const std::string& kind) {
    if (kind == "i64") return {callI64("a_int_full", {aval}), "i64"};
    if (kind == "f64") {
      std::string r = nv();
      fn_ << "  " << r << " = a.runtime.call \"num_to_double\"(" << aval
          << ") {signature = \"(i64) -> f64\"} : (i64) -> f64\n";
      return {r, "f64"};
    }
    if (kind == "i1") {
      // bool aval is exactly A_TRUE/A_FALSE.
      std::string at = c64(kAvalTag + 6), r = nv();
      fn_ << "  " << r << " = llvm.icmp \"eq\" " << aval << ", " << at << " : i64\n";
      return {r, "i1"};
    }
    return {aval, "aval"};
  }

  // ---- typed local slots -------------------------------------------------------
  std::string allocaF64() {
    std::string r = nv();
    std::string sz = nv();
    fnAllocs_ << "  " << sz << " = llvm.mlir.constant(1 : i64) : i64\n";
    fnAllocs_ << "  " << r << " = llvm.alloca " << sz
              << " x !llvm.array<1 x f64> : (i64) -> !llvm.ptr\n";
    return r;
  }
  std::string loadTy(const std::string& p, const std::string& ty) {
    std::string r = nv();
    fn_ << "  " << r << " = llvm.load " << p << " : !llvm.ptr -> " << ty << "\n";
    return r;
  }
  void storeTy(const std::string& p, const std::string& v, const std::string& ty) {
    fn_ << "  llvm.store " << v << ", " << p << " : " << ty << ", !llvm.ptr\n";
  }
  // load a typed binding's slot as a TypedVal ("i1" slots store i64 0/1).
  TypedVal loadTypedVal(const MlBinding& b) {
    if (b.type == "f64") return {loadTy(b.slot, "f64"), "f64"};
    if (b.type == "i1") {
      std::string w = loadTy(b.slot, "i64"), r = nv();
      fn_ << "  " << r << " = llvm.trunc " << w << " : i64 to i1\n";
      return {r, "i1"};
    }
    return {loadTy(b.slot, "i64"), "i64"};
  }
  void storeTypedVal(const MlBinding& b, const TypedVal& v) {
    // Kind mismatches must round-trip through the aval representation:
    // an "aval"-kind value is a raw i64 word and a different scalar kind
    // needs conversion — storing either directly into the typed slot
    // would emit an ill-typed llvm.store (and wrong bits).
    if (v.kind == "aval" && !v.name.empty()) {
      storeTypedFromAval(b, v.name);
      return;
    }
    if (v.kind != b.type) {
      storeTypedFromAval(b, boxToAval(v));
      return;
    }
    if (b.type == "f64") { storeTy(b.slot, tvalSSA(v), "f64"); return; }
    if (b.type == "i1") {
      // i1 -> i64 slot (zext; consts store 0/1 directly)
      if (v.isConst) { storeTy(b.slot, c64(v.iConst != 0 ? 1 : 0), "i64"); return; }
      std::string w = nv();
      fn_ << "  " << w << " = llvm.zext " << v.name << " : i1 to i64\n";
      storeTy(b.slot, w, "i64");
      return;
    }
    storeTy(b.slot, tvalSSA(v), "i64");
  }
  // store an aval i64 word into a typed slot (decode; unchecked per Sema).
  void storeTypedFromAval(const MlBinding& b, const std::string& aval) {
    storeTypedVal(b, unboxFromAval(aval, b.type));
  }

  // may `name` get a typed binding of `kind`? Requires every AssignStmt
  // value targeting `name` in this function to have the same scalar kind.
  bool canTypeBind(const std::string& name, const std::string& kind) {
    auto it = assignKindMask_.find(name);
    if (it == assignKindMask_.end()) return true;
    return it->second == scalarKindBit(kind);
  }
  void noteAssignKind(const std::string& name, TypePtr t) {
    assignKindMask_[name] |= scalarKindBit(scalarKindOfT(t));
  }
  void scanStmtAssigns(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Assign: {
        const AssignStmt& as = static_cast<const AssignStmt&>(s);
        if (as.target->kind == ExprKind::Ident && as.value)
          noteAssignKind(static_cast<const IdentExpr*>(as.target.get())->name,
                         as.value->inferredType);
        break;
      }
      case StmtKind::If: {
        const IfStmt& is = static_cast<const IfStmt&>(s);
        for (const auto& br : is.branches) scanAssignKinds(br.body);
        scanAssignKinds(is.elseBody);
        break;
      }
      case StmtKind::While:
        scanAssignKinds(static_cast<const WhileStmt&>(s).body); break;
      case StmtKind::DoWhile:
        scanAssignKinds(static_cast<const DoWhileStmt&>(s).body); break;
      case StmtKind::For:
        scanAssignKinds(static_cast<const ForStmt&>(s).body); break;
      case StmtKind::Loop:
        scanAssignKinds(static_cast<const LoopStmt&>(s).body); break;
      case StmtKind::Defer: {
        const DeferStmt& ds = static_cast<const DeferStmt&>(s);
        if (ds.body) scanStmtAssigns(*ds.body);
        break;
      }
      default: break;
    }
  }
  void scanAssignKinds(const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& s : stmts) if (s) scanStmtAssigns(*s);
  }

  // typed-aware binding producers/consumers
  void bindLocalT(const std::string& name, const TypedVal& tv) {
    if (typed_ && tv.kind != "aval" && canTypeBind(name, tv.kind)) {
      MlBinding b;
      b.type = tv.kind;
      if (tv.kind == "f64") b.slot = allocaF64();
      else b.slot = allocaI64();  // i64 and i1 (i1 stored as i64 0/1)
      storeTypedVal(b, tv);
      scopes_.back()[name] = b;
      return;
    }
    bindLocal(name, boxToAval(tv));
  }
  // TypedVal view of a binding (typed slot -> typed load; everything else
  // aval). Globals/module members are always aval.
  TypedVal bindingValueT(const std::string& name, bool& isGlobal) {
    if (MlBinding* b = lookup(name)) {
      isGlobal = false;
      if (b->type == "aval") return {load(b->slot), "aval"};
      return loadTypedVal(*b);
    }
    // reuse the aval global-resolution tail of bindingValue
    auto mga = moduleGlobalAliases_.find(name);
    if (mga != moduleGlobalAliases_.end()) {
      auto rm = resolveModuleMember(graph_, mga->second.first, mga->second.second);
      auto mg = moduleGlobals_.find({rm.first, rm.second});
      if (mg != moduleGlobals_.end()) {
        isGlobal = true;
        std::string r = nv();
        fn_ << "  " << r << " = llvm.mlir.addressof @" << mg->second << " : !llvm.ptr\n";
        return {load(r), "aval"};
      }
    }
    std::string sym = globalSymFor(name);
    if (!sym.empty()) {
      isGlobal = true;
      std::string r = nv();
      fn_ << "  " << r << " = llvm.mlir.addressof @" << sym << " : !llvm.ptr\n";
      return {load(r), "aval"};
    }
    isGlobal = false;
    return {"", "aval"};
  }
  void storeBindingT(const std::string& name, const TypedVal& tv) {
    if (MlBinding* b = lookup(name)) {
      if (b->type == "aval") { store(b->slot, boxToAval(tv)); return; }
      storeTypedVal(*b, tv);
      return;
    }
    storeBinding(name, boxToAval(tv));
  }

  // truthiness of a TypedVal -> i1 (for if/while/&& conditions)
  std::string truthyT(const TypedVal& v) {
    if (v.kind == "i1") return tvalSSA(v);
    if (v.kind == "i64") {
      std::string s = tvalSSA(v), z = c64(0), r = nv();
      fn_ << "  " << r << " = llvm.icmp \"ne\" " << s << ", " << z << " : i64\n";
      return r;
    }
    if (v.kind == "f64") {
      std::string s = tvalSSA(v), z = f64Zero(), r = nv();
      fn_ << "  " << r << " = llvm.fcmp \"une\" " << s << ", " << z << " : f64\n";
      return r;
    }
    return truthyI1(v.name);
  }

  // ---- typed constant folding --------------------------------------------------
  // Signed-overflow-checked i64 arithmetic. GCC/Clang have
  // __builtin_*_overflow; MSVC does not, so fall back to explicit range
  // checks there (and anywhere else the builtins are unavailable).
  static bool i64AddOverflow(std::int64_t a, std::int64_t b,
                             std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
      return true;
    out = a + b;
    return false;
#endif
  }
  static bool i64SubOverflow(std::int64_t a, std::int64_t b,
                             std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, &out);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
      return true;
    out = a - b;
    return false;
#endif
  }
  static bool i64MulOverflow(std::int64_t a, std::int64_t b,
                             std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    if (a > 0) {
      if (b > 0) {
        if (a > INT64_MAX / b) return true;
      } else if (b < 0) {
        if (b < INT64_MIN / a) return true;
      } else {
        out = 0;
        return false;
      }
    } else if (a < 0) {
      if (b > 0) {
        if (a < INT64_MIN / b) return true;
      } else if (b < 0) {
        if (a != 0 && b < INT64_MAX / a) return true;
      } else {
        out = 0;
        return false;
      }
    } else {
      out = 0;
      return false;
    }
    out = a * b;
    return false;
#endif
  }

  static bool foldI64BinOp(BinOp op, std::int64_t a, std::int64_t b,
                           std::int64_t& out, bool& isBool) {
    isBool = false;
    switch (op) {
      case BinOp::Add:
        if (i64AddOverflow(a, b, out)) return false;
        return true;
      case BinOp::Sub:
        if (i64SubOverflow(a, b, out)) return false;
        return true;
      case BinOp::Mul:
        if (i64MulOverflow(a, b, out)) return false;
        return true;
      case BinOp::BitAnd: out = a & b; return true;
      case BinOp::BitOr: out = a | b; return true;
      case BinOp::BitXor: out = a ^ b; return true;
      case BinOp::Shl:
        if (b < 0 || b > 63) return false;
        out = (std::int64_t)((std::uint64_t)a << b); return true;
      case BinOp::Shr:
        if (b < 0 || b > 63) return false;
        out = a >> b; return true;  // arithmetic (matches a_shr_slow)
      case BinOp::Eq: out = a == b; isBool = true; return true;
      case BinOp::Ne: out = a != b; isBool = true; return true;
      case BinOp::Lt: out = a < b; isBool = true; return true;
      case BinOp::Le: out = a <= b; isBool = true; return true;
      case BinOp::Gt: out = a > b; isBool = true; return true;
      case BinOp::Ge: out = a >= b; isBool = true; return true;
      default: return false;
    }
  }
  static bool foldF64BinOp(BinOp op, double a, double b, double& out, bool& isBool) {
    isBool = false;
    switch (op) {
      case BinOp::Add: out = a + b; return true;
      case BinOp::Sub: out = a - b; return true;
      case BinOp::Mul: out = a * b; return true;
      case BinOp::Eq: out = a == b; isBool = true; return true;
      case BinOp::Ne: out = a != b; isBool = true; return true;
      case BinOp::Lt: out = a < b; isBool = true; return true;
      case BinOp::Le: out = a <= b; isBool = true; return true;
      case BinOp::Gt: out = a > b; isBool = true; return true;
      case BinOp::Ge: out = a >= b; isBool = true; return true;
      default: return false;
    }
  }
  TypedVal foldI64Cmp(BinOp op, std::int64_t a, std::int64_t b) {
    bool isB; std::int64_t o;
    foldI64BinOp(op, a, b, o, isB);
    return {"", "i1", true, o != 0, 0.0};
  }
  TypedVal foldF64Cmp(BinOp op, double a, double b) {
    bool isB; double o;
    foldF64BinOp(op, a, b, o, isB);
    return {"", "i1", true, o != 0.0 ? 1 : 0, 0.0};
  }

  // emit `r = llvm.<op> l, r2 : ty` helpers
  std::string llvm2(const char* op, const std::string& l, const std::string& r,
                    const std::string& ty) {
    std::string res = nv();
    fn_ << "  " << res << " = llvm." << op << " " << l << ", " << r << " : " << ty << "\n";
    return res;
  }
  std::string llvm1(const char* op, const std::string& a, const std::string& ty) {
    std::string res = nv();
    fn_ << "  " << res << " = llvm." << op << " " << a << " : " << ty << "\n";
    return res;
  }
  std::string icmp(const char* pred, const std::string& l, const std::string& r,
                   const std::string& ty) {
    std::string res = nv();
    fn_ << "  " << res << " = llvm.icmp \"" << pred << "\" " << l << ", " << r
        << " : " << ty << "\n";
    return res;
  }
  std::string fcmp(const char* pred, const std::string& l, const std::string& r) {
    std::string res = nv();
    fn_ << "  " << res << " = llvm.fcmp \"" << pred << "\" " << l << ", " << r << " : f64\n";
    return res;
  }

  // i64 binop with runtime-exact edge handling: the raw op runs in a fast
  // block guarded by an i1 `bad` condition; on `bad` (int64 overflow -> the
  // runtime promotes to f64, div-by-zero / INT64_MIN/-1 / out-of-range shift
  // -> *_slow panic) control merges through the same aval fallback the
  // untyped path uses, then a_int_full unboxes (panicking on f64->i64
  // exactly like storeTypedFromAval does today). Merge is a block arg, so
  // callers see a plain i64 SSA value.
  TypedVal checkedI64BinOp(BinOp op, const TypedVal& lt, const TypedVal& rt,
                           const Span& span) {
    std::string l = tvalSSA(lt), r = tvalSSA(rt);
    std::string ls = nl(), lf = nl(), lm = nl(), pv = nv();
    std::string bad, res;
    bool preGuard = true;  // raw op may be poison: emit inside ^lf
    switch (op) {
      case BinOp::Add:
      case BinOp::Sub:
      case BinOp::Mul: {
        // Canonical with.overflow intrinsics: LLVM lowers these to the
        // hardware op + overflow-flag branch, cheaper than hand-rolled
        // sign checks and recognized by IndVarSimplify for elimination.
        const char* intr = op == BinOp::Add ? "llvm.sadd.with.overflow.i64"
                            : op == BinOp::Sub ? "llvm.ssub.with.overflow.i64"
                                               : "llvm.smul.with.overflow.i64";
        std::string p = nv();
        fn_ << "  " << p << " = llvm.call @" << intr << "("
            << l << ", " << r
            << ") : (i64, i64) -> !llvm.struct<(i64, i1)>\n";
        res = nv();
        fn_ << "  " << res << " = llvm.extractvalue " << p
            << "[0] : !llvm.struct<(i64, i1)>\n";
        bad = nv();
        fn_ << "  " << bad << " = llvm.extractvalue " << p
            << "[1] : !llvm.struct<(i64, i1)>\n";
        preGuard = false;
        break;
      }
      case BinOp::FloorDiv: {
        // bad: b==0 (panic) or INT64_MIN/-1 (promotes to f64 -> i64 panic)
        std::string bz = icmp("eq", r, c64(0), "i64");
        std::string m1 = icmp("eq", l, c64(INT64_MIN), "i64");
        std::string m2 = icmp("eq", r, c64(-1), "i64");
        bad = llvm2("or", bz, llvm2("and", m1, m2, "i1"), "i1");
        break;
      }
      case BinOp::Mod: {
        std::string bz = icmp("eq", r, c64(0), "i64");
        std::string m1 = icmp("eq", l, c64(INT64_MIN), "i64");
        std::string m2 = icmp("eq", r, c64(-1), "i64");
        bad = llvm2("or", bz, llvm2("and", m1, m2, "i1"), "i1");
        break;
      }
      case BinOp::Shl: case BinOp::Shr: {
        // bad: shift amount outside [0,63] -> *_slow panics
        std::string lo = icmp("slt", r, c64(0), "i64");
        std::string hi = icmp("sgt", r, c64(63), "i64");
        bad = llvm2("or", lo, hi, "i1");
        break;
      }
      default:
        return {"", "", false, 0, 0.0};
    }
    fn_ << "  llvm.cond_br " << bad << ", ^" << ls << ", ^" << lf << "\n";
    fn_ << "^" << lf << ":\n";
    if (preGuard) {
      // Raw op here is safe: all poison cases were excluded by `bad`.
      switch (op) {
        case BinOp::FloorDiv: {
          std::string q = llvm2("sdiv", l, r, "i64");
          std::string rem = llvm2("srem", l, r, "i64");
          // --q iff a%b!=0 && (a<0)!=(b<0)  (value_arith.c a_floordiv_slow)
          std::string rz = icmp("ne", rem, c64(0), "i64");
          std::string s1 = icmp("slt", l, c64(0), "i64");
          std::string s2 = icmp("slt", r, c64(0), "i64");
          std::string fix = llvm2("and", rz, llvm2("xor", s1, s2, "i1"), "i1");
          std::string qm1 = llvm2("sub", q, c64(1), "i64");
          res = nv();
          fn_ << "  " << res << " = llvm.select " << fix << ", " << qm1
              << ", " << q << " : i1, i64\n";
          break;
        }
        case BinOp::Mod: {
          // r += b iff r!=0 && (r<0)!=(b<0)  (value_arith.c '%' case)
          std::string rem = llvm2("srem", l, r, "i64");
          std::string rz = icmp("ne", rem, c64(0), "i64");
          std::string s1 = icmp("slt", rem, c64(0), "i64");
          std::string s2 = icmp("slt", r, c64(0), "i64");
          std::string fix = llvm2("and", rz, llvm2("xor", s1, s2, "i1"), "i1");
          std::string adj = llvm2("add", rem, r, "i64");
          res = nv();
          fn_ << "  " << res << " = llvm.select " << fix << ", " << adj
              << ", " << rem << " : i1, i64\n";
          break;
        }
        case BinOp::Shl:
          res = llvm2("shl", l, r, "i64");
          break;
        case BinOp::Shr:
          res = llvm2("ashr", l, r, "i64");
          break;
        default: break;
      }
    }
    fn_ << "  llvm.br ^" << lm << "(" << res << " : i64)\n";
    fn_ << "^" << ls << ":\n";
    // Identical to the untyped fallback: boxed aval op, then a_int_full
    // (panics on non-int results, matching storeTypedFromAval).
    std::string av = emitBinOp(op, boxToAval(lt), boxToAval(rt), span);
    std::string ri = callI64("a_int_full", {av});
    fn_ << "  llvm.br ^" << lm << "(" << ri << " : i64)\n";
    fn_ << "^" << lm << "(" << pv << " : i64):\n";
    return {pv, "i64"};
  }

  // typed binary operator on two same-kind scalar TypedVals; returns "" when
  // the op must stay on the aval runtime path (caller falls back).
  // `lt`/`rt` are the operand TypedVals; `kind` is their shared kind.
  TypedVal typedBinOp(BinOp op, const TypedVal& lt, const TypedVal& rt,
                      const std::string& kind, const Span& span = Span()) {
    // constant folding first (overflow/out-of-range -> aval path, which keeps
    // the runtime's promote-to-double / panic semantics for those cases)
    if (lt.isConst && rt.isConst) {
      if (kind == "i64") {
        if (op == BinOp::Eq || op == BinOp::Ne || op == BinOp::Lt ||
            op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge)
          return foldI64Cmp(op, lt.iConst, rt.iConst);
        std::int64_t o; bool isB;
        if (foldI64BinOp(op, lt.iConst, rt.iConst, o, isB))
          return {"", kind, true, o, 0.0};
        return {"", "", false, 0, 0.0};  // signal: fall back
      }
      if (kind == "f64") {
        if (op == BinOp::Eq || op == BinOp::Ne || op == BinOp::Lt ||
            op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge)
          return foldF64Cmp(op, lt.fConst, rt.fConst);
        double o; bool isB;
        if (foldF64BinOp(op, lt.fConst, rt.fConst, o, isB))
          return {"", "f64", true, 0, o};
        return {"", "", false, 0, 0.0};
      }
      if (kind == "i1") {
        bool a = lt.iConst != 0, b = rt.iConst != 0, o = false;
        if (op == BinOp::BitAnd) o = a && b;
        else if (op == BinOp::BitOr) o = a || b;
        else if (op == BinOp::BitXor) o = a != b;
        else if (op == BinOp::Eq) o = a == b;
        else if (op == BinOp::Ne) o = a != b;
        else return {"", "", false, 0, 0.0};
        return {"", "i1", true, o ? 1 : 0, 0.0};
      }
    }

    std::string l = tvalSSA(lt), r = tvalSSA(rt);
    // kind == "i1": only ==/!= are meaningful on Bool (the runtime bitwise
    // ops panic on non-int operands, and ordering comparisons are rejected
    // by Sema); anything else falls back to the aval path.
    if (kind == "i1") {
      if (op == BinOp::Eq) return {icmp("eq", l, r, "i1"), "i1"};
      if (op == BinOp::Ne) return {icmp("ne", l, r, "i1"), "i1"};
      return {"", "", false, 0, 0.0};
    }
    switch (op) {
      // i64 + - * promote to f64 on int64 overflow and //,%,<<,>> have
      // div-by-zero / INT64_MIN/-1 / shift-range edge cases — all handled
      // by checkedI64BinOp's guarded fast block + aval fallback.
      case BinOp::FloorDiv: case BinOp::Mod:
      case BinOp::Shl: case BinOp::Shr:
        if (kind == "i64") return checkedI64BinOp(op, lt, rt, span);
        break;
      case BinOp::Add:
        if (kind == "i64") return checkedI64BinOp(op, lt, rt, span);
        if (kind == "f64") return {llvm2("fadd", l, r, "f64"), "f64"};
        break;
      case BinOp::Sub:
        if (kind == "i64") return checkedI64BinOp(op, lt, rt, span);
        if (kind == "f64") return {llvm2("fsub", l, r, "f64"), "f64"};
        break;
      case BinOp::Mul:
        if (kind == "i64") return checkedI64BinOp(op, lt, rt, span);
        if (kind == "f64") return {llvm2("fmul", l, r, "f64"), "f64"};
        break;
      case BinOp::BitAnd:
        if (kind == "i64") return {llvm2("and", l, r, kind), kind};
        break;
      case BinOp::BitOr:
        if (kind == "i64") return {llvm2("or", l, r, kind), kind};
        break;
      case BinOp::BitXor:
        if (kind == "i64") return {llvm2("xor", l, r, kind), kind};
        break;
      case BinOp::Eq: case BinOp::Ne: case BinOp::Lt:
      case BinOp::Le: case BinOp::Gt: case BinOp::Ge: {
        const char* ip = nullptr, *fp = nullptr;
        switch (op) {
          case BinOp::Eq: ip = "eq"; fp = "oeq"; break;
          case BinOp::Ne: ip = "ne"; fp = "une"; break;
          case BinOp::Lt: ip = "slt"; fp = "olt"; break;
          case BinOp::Le: ip = "sle"; fp = "ole"; break;
          case BinOp::Gt: ip = "sgt"; fp = "ogt"; break;
          case BinOp::Ge: ip = "sge"; fp = "oge"; break;
          default: break;
        }
        if (kind == "i64") return {icmp(ip, l, r, kind), "i1"};
        if (kind == "f64") return {fcmp(fp, l, r), "i1"};
        break;
      }
      default: break;
    }
    return {"", "", false, 0, 0.0};  // fall back to aval runtime path
  }

  // Eval call args packed for a specific FnDecl: positional order, kwargs
  // mapped to param names, variadic extras collected into a list.
  std::vector<std::string> evalFnArgs(const FnDecl* fn, const CallExpr& c) {
    int nparams = (int)fn->params.size();
    bool variadic = nparams > 0 && fn->params.back().variadic;
    int fixed = variadic ? nparams - 1 : nparams;
    std::vector<std::string> vals((size_t)std::max(nparams, 0), "");
    std::vector<std::string> extras;
    for (const CallArg& a : c.args) {
      std::string v = evalExpr(*a.value);
      if (!a.name.empty()) {
        for (int i = 0; i < nparams; ++i)
          if (fn->params[i].name == a.name) { vals[i] = v; break; }
        continue;
      }
      int slot = -1;
      for (int i = 0; i < fixed; ++i) if (vals[i].empty()) { slot = i; break; }
      if (slot < 0) { if (variadic) extras.push_back(v); }
      else vals[slot] = v;
    }
    if (variadic) {
      std::string lst = callI64T("a_list_new", {{"i64", c64(0)}, {"!llvm.ptr", nullPtr()}});
      for (auto& e : extras) callI64("a_list_append", {lst, e});
      vals[fixed] = lst;
    }
    // absent args -> A_MISSING sentinel; callee fills defaults in-scope
    for (int i = 0; i < nparams; ++i)
      if (vals[i].empty()) vals[i] = c64(kAvalTag);
    return vals;
  }

  // resolve mod.StructName -> ctor cname through re-export chains
  std::string resolveModuleStruct(int moduleId, const std::string& name, int depth = 0) {
    if (depth > 8) return "";
    auto it = moduleStructCtors_.find({moduleId, name});
    if (it != moduleStructCtors_.end()) return it->second;
    auto ri = moduleReexports_.find(moduleId);
    if (ri != moduleReexports_.end()) {
      auto ni = ri->second.find(name);
      if (ni != ri->second.end())
        return resolveModuleStruct(ni->second.first, ni->second.second, depth + 1);
    }
    return "";
  }

  // resolve mod.name through re-export chains (depth-limited)
  const FnDecl* resolveModuleFn(int moduleId, const std::string& name, int depth = 0) {
    if (depth > 8) return nullptr;
    auto it = moduleFns_.find({moduleId, name});
    if (it != moduleFns_.end()) return it->second;
    auto ri = moduleReexports_.find(moduleId);
    if (ri != moduleReexports_.end()) {
      auto ni = ri->second.find(name);
      if (ni != ri->second.end())
        return resolveModuleFn(ni->second.first, ni->second.second, depth + 1);
    }
    return nullptr;
  }

  // Resolve `mod.name` used as a value (module-level let, fn value, or a
  // native member re-exported through an A module). Follows re-export chains
  // via resolveModuleMember. Returns "" after failAt on failure.
  std::string moduleMemberValue(int moduleId, const std::string& name,
                                const Expr* e) {
    auto rm = resolveModuleMember(graph_, moduleId, name);
    int mid = rm.first;
    const std::string& nm = rm.second;
    auto g = moduleGlobals_.find({mid, nm});
    if (g != moduleGlobals_.end()) {
      std::string r = nv();
      fn_ << "  " << r << " = llvm.mlir.addressof @" << g->second << " : !llvm.ptr\n";
      return load(r);
    }
    const FnDecl* fd = resolveModuleFn(mid, nm);
    if (fd) {
      auto cit = fnDeclCnames_.find(fd);
      if (cit != fnDeclCnames_.end()) {
        std::string r = nv();
        fn_ << "  " << r << " = llvm.mlir.addressof @" << cit->second << " : !llvm.ptr\n";
        std::string fp = nv();
        fn_ << "  " << fp << " = llvm.ptrtoint " << r << " : !llvm.ptr to i64\n";
        std::string envN = c64(kAvalTag | 0x2);
        std::string nmc = cstrPtr(nm);
        return callI64T("a_fnon_new",
                        {{"i64", fp}, {"i64", envN}, {"!llvm.ptr", nmc}});
      }
    }
    // native member re-exported by the resolved A module
    // (`from math import PI` inside an __init__.astra)
    if (mid >= 0 && mid < (int)graph_.modules.size()) {
      const Module& mod = graph_.at(mid);
      if (mod.program) {
        for (const StmtPtr& s : mod.program->statements) {
          if (s->kind != StmtKind::FromImport) continue;
          const FromImportStmt* fi = static_cast<const FromImportStmt*>(s.get());
          if (!fi->nativeModule || fi->bareName.empty()) continue;
          const NativeModuleSpec* spec = nativeModuleSpec(fi->bareName);
          if (!spec) continue;
          for (const auto& item : fi->items) {
            if (item.name != nm && (!item.alias.empty() && item.alias != nm))
              continue;
            auto cn = spec->constants.find(item.name);
            if (cn != spec->constants.end()) {
              if (cn->second == "PI") return avalDbl(3.14159265358979323846);
              if (cn->second == "E") return avalDbl(2.71828182845904523536);
              return avalInt(0);
            }
            auto fn = spec->functions.find(item.name);
            if (fn != spec->functions.end()) {
              int bid = builtinRuntimeId(fn->second);
              if (bid >= 0)
                return callI64T("a_builtin", {{"i64", c64(bid)}});
            }
          }
        }
      }
    }
    failAt("module member '" + name + "' is not a value", e);
    return "";
  }

  // `mod.name` -> global symbol when the member is a module-level let/assign
  // global (possibly re-exported); "" otherwise.
  std::string moduleGlobalSym(int moduleId, const std::string& name) {
    auto rm = resolveModuleMember(graph_, moduleId, name);
    auto g = moduleGlobals_.find({rm.first, rm.second});
    return g != moduleGlobals_.end() ? g->second : "";
  }

  std::string evalCall(const CallExpr& c) {
    // cast<T>(x) / x as T: compile-time annotation, runtime identity
    if (c.callee->kind == ExprKind::Ident &&
        static_cast<const IdentExpr&>(*c.callee).name == "cast" && !c.args.empty())
      return evalExpr(*c.args[0].value);
    // kwargs: only method calls support them in the MVP; reject early for
    // other callee shapes below. Args are evaluated lazily per path so the
    // method-call path doesn't double-evaluate them.
    bool hasKw = false;
    for (const CallArg& a : c.args) if (!a.name.empty()) hasKw = true;
    auto evalArgs = [&]() {
      std::vector<std::string> v;
      for (const CallArg& a : c.args) v.push_back(evalExpr(*a.value));
      return v;
    };

    // sema-resolved direct function (covers ic/prelude/lambda-resolved calls)
    if (c.calleeFunction) {
      auto it = fnDeclCnames_.find(c.calleeFunction);
      if (it != fnDeclCnames_.end())
        return callAFunction(it->second, evalFnArgs(c.calleeFunction, c));
    }
    // builtin?
    if (c.calleeIsBuiltin) {
      if (hasKw) { failAt("keyword arguments not supported by the mlir backend yet", &c); return ""; }
      int id = builtinRuntimeId(c.calleeBuiltin);
      if (id < 0) id = builtinIdForBuiltin(c.builtinId);
      if (id < 0) {
        // ic()/__ic_format__ resolve to a prelude function, not a runtime id
        auto icf = fnCnames_.find("ic_impl");
        if (icf != fnCnames_.end()) return callAFunction(icf->second, evalArgs());
        failAt("unknown builtin", &c); return "";
      }
      return callBuiltin(id, evalArgs());
    }
    // ident callee -> fn decl, local/global callable, or builtin by name
    if (c.callee->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*c.callee);
      if (c.builtinId != BuiltinId::None) {
        int bid = builtinIdForBuiltin(c.builtinId);
        if (bid >= 0) return callBuiltin(bid, evalArgs());
      }
      auto ama = aModuleFnAliases_.find(id.name);
      if (ama != aModuleFnAliases_.end()) {
        auto cit = fnDeclCnames_.find(ama->second);
        if (cit != fnDeclCnames_.end())
          return callAFunction(cit->second, evalFnArgs(ama->second, c));
      }
      auto pma = pyMemberAliases_.find(id.name);
      if (pma != pyMemberAliases_.end())
        return evalPyCall(pma->second.first, pma->second.second, c);
      auto ba = builtinAliases_.find(id.name);
      if (ba != builtinAliases_.end()) {
        int bid = builtinRuntimeId(ba->second);
        if (bid >= 0) return callBuiltin(bid, evalArgs());
      }
      // locals/globals shadow same-named fns from other modules
      bool isG = false;
      std::string fv = bindingValue(id.name, isG);
      if (!fv.empty()) return callValue(fv, evalArgs());
      auto fn = fnCnames_.find(id.name);
      if (fn != fnCnames_.end()) {
        auto fit = fnByCname_.find(fn->second);
        return callAFunction(fn->second,
            fit != fnByCname_.end() ? evalFnArgs(fit->second, c) : evalArgs());
      }
      auto sc = structCtors_.find(id.name);
      if (sc != structCtors_.end()) {
        if (!hasKw) return callAFunction(sc->second, evalArgs());
        // kwargs: map names to field positions
        for (auto& ms : structs_) {
          if (ms.ctorCname != sc->second) continue;
          std::vector<std::string> pos(ms.fieldNames.size());
          std::vector<char> seen(ms.fieldNames.size(), 0);
          for (const CallArg& a : c.args) {
            if (a.name.empty()) continue;
            for (size_t i = 0; i < ms.fieldNames.size(); ++i) {
              if (ms.fieldNames[i] == a.name) {
                pos[i] = evalExpr(*a.value); seen[i] = 1; break;
              }
            }
          }
          for (size_t i = 0; i < pos.size(); ++i)
            if (!seen[i]) { failAt("missing struct field '" + ms.fieldNames[i] + "'", &c); return ""; }
          return callAFunction(sc->second, pos);
        }
        return callAFunction(sc->second, evalArgs());
      }
      if (hasKw) { failAt("keyword arguments not supported by the mlir backend yet", &c); return ""; }
      int bid = builtinRuntimeId(id.name);
      if (bid >= 0) return callBuiltin(bid, evalArgs());
      // capitalized Result/Option constructors map to the same runtime
      // builtins as their lowercase spellings (ok=37, err=38, some=36)
      if (id.name == "Ok" || id.name == "Result") return callBuiltin(37, evalArgs());
      if (id.name == "Err") return callBuiltin(38, evalArgs());
      if (id.name == "Some") return callBuiltin(36, evalArgs());
      failAt("call to unbound name '" + id.name + "'", &c);
      return "";
    }
    // python module member call: pymod.member(...)
    if (c.callee->kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(*c.callee);
      if (fx.object->kind == ExprKind::Ident) {
        const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
        auto pa = pythonAliases_.find(oid.name);
        if (pa != pythonAliases_.end())
          return evalPyCall(pa->second, fx.field, c);
      }
    }
    // enum variant ctor: Enum.Variant(payload...) or mod.Enum.Variant(...)
    if (c.callee->kind == ExprKind::Field) {
      std::vector<std::string> parts = flattenField(c.callee.get());
      if (parts.size() >= 2) {
        if (const MlVariant* mv = findVariant(parts)) {
          std::vector<std::string> payload;
          for (const CallArg& a : c.args) payload.push_back(evalExpr(*a.value));
          return variantValue(*mv, parts.back(), &payload);
        }
      }
    }
    // A-module member call: modalias.fn(...)
    if (c.callee->kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(*c.callee);
      if (fx.object->kind == ExprKind::Ident) {
        const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
        auto ma = aModuleAliases_.find(oid.name);
        if (ma != aModuleAliases_.end()) {
          const FnDecl* fd = resolveModuleFn(ma->second, fx.field);
          if (fd) {
            auto cit = fnDeclCnames_.find(fd);
            if (cit != fnDeclCnames_.end())
              return callAFunction(cit->second, evalFnArgs(fd, c));
          }
          // struct ctor: mod.StructName(args...)
          std::string sctor = resolveModuleStruct(ma->second, fx.field);
          if (!sctor.empty()) {
            std::vector<std::string> pos;
            for (const CallArg& a : c.args) pos.push_back(evalExpr(*a.value));
            return callAFunction(sctor, pos);
          }
          failAt("module '" + oid.name + "' has no member '" + fx.field + "'", &c);
          return "";
        }
      }
    }
    // native module member call: alias.member(...)
    if (c.callee->kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(*c.callee);
      if (fx.object->kind == ExprKind::Ident) {
        const IdentExpr& oid = static_cast<const IdentExpr&>(*fx.object);
        auto na = nativeAliases_.find(oid.name);
        if (na != nativeAliases_.end()) {
          const NativeModuleSpec* spec = nativeModuleSpec(na->second);
          if (spec) {
            auto f = spec->functions.find(fx.field);
            if (f != spec->functions.end()) {
              int bid = builtinRuntimeId(f->second);
              if (bid >= 0) return callBuiltin(bid, evalArgs());
            }
            auto cn = spec->constants.find(fx.field);
            if (cn != spec->constants.end()) {
              // constant member used as call -> error unless callable
              failAt("native constant '" + na->second + "." + fx.field + "' is not callable", &c);
              return "";
            }
          }
        }
      }
    }
    // method call: obj.field(args...) -> a_method dispatch
    if (c.callee->kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(*c.callee);
      // native module member handled above; everything else is a_method
      return evalMethodCall(*fx.object, fx.field, c);
    }
    // generic callee expr -> a_call
    if (hasKw) { failAt("keyword arguments not supported by the mlir backend yet", &c); return ""; }
    std::string fv = evalExpr(*c.callee);
    if (fv.empty()) return "";
    return callValue(fv, evalArgs());
  }

  // Per-module emission helpers (runModule path; emitOnlyModule_ >= 0).
  // emitModuleDecls: scans the emitted text for `@symbol` references and
  // inserts declarations (no body) for every referenced symbol this module
  // does not itself define — the consumer links per-module objects, so calls
  // to `__a_f_m<id>_*`/`__a_m*`/`__a_ctor_*`/`__a_lam_*`/`__a_pf_*` owned by
  // other modules must be declared, and foreign `__a_g_<id>_*` globals need
  // `llvm.mlir.global external` decls so `llvm.mlir.addressof` resolves.
  // Runs AFTER all bodies are emitted so locally-defined symbols are known
  // and never redeclared. `a_*` runtime symbols need no decls here: they
  // are invoked via `a.runtime.call "sym"` (string callee, not an @ref) and
  // a-lower-to-llvm materializes the llvm.func decl itself.
  void emitModuleDecls() {
    const std::string text = pre_.str() + body_.str();
    std::set<std::string> defined, referenced;
    {
      // function + global definitions: `llvm.func @sym(` /
      // `llvm.mlir.global ... @sym(` — match the op name first so
      // calls/addressofs of the same symbol don't count as definitions.
      static const std::regex defRe(
          "llvm\\.(?:func|mlir\\.global)[^@\\n]*@([A-Za-z_][A-Za-z0-9_]*)");
      static const std::regex refRe("@([A-Za-z_][A-Za-z0-9_]*)");
      for (std::sregex_iterator it(text.begin(), text.end(), defRe), end;
           it != end; ++it)
        defined.insert((*it)[1].str());
      for (std::sregex_iterator it(text.begin(), text.end(), refRe), end;
           it != end; ++it)
        referenced.insert((*it)[1].str());
    }
    std::vector<std::string> funcDecls, globalDecls;
    for (const std::string& sym : referenced) {
      if (defined.count(sym)) continue;
      if (sym.rfind("__a_tc_", 0) == 0) {
        // Typed-call core owned by another module: redeclare with its real
        // scalar signature — the generic boxed decl below would mismatch.
        auto fit = fnByCname_.find(sym.substr(6));
        if (fit == fnByCname_.end() || typedCoreStatus(fit->second) != 1)
          continue;  // leave undeclared: a loud link error beats a wrong sig
        const FnDecl* tf = fit->second;
        auto pit = sema_.fnParamTypes.find(tf);
        auto rit = sema_.fnReturnTypes.find(tf);
        std::string tsig;
        for (size_t i = 0; i < tf->params.size(); ++i) {
          if (i) tsig += ", ";
          tsig += "%p" + std::to_string(i) + ": " +
                  kindLLVMType(scalarKindOfT(pit->second[i]));
        }
        funcDecls.push_back("  llvm.func @" + sym + "(" + tsig + ") -> " +
                            kindLLVMType(scalarKindOfT(rit->second)) + "\n");
        continue;
      }
      if (sym.rfind("__a_g_", 0) == 0) {
        // Foreign module global: declare it exactly like the owning unit's
        // definition (external linkage + zero initializer — the real init
        // runs in that module's __a_m<id>_init). addressof only needs the
        // symbol to resolve; the i64 element type matches every __a_g_*.
        globalDecls.push_back(
            "  llvm.mlir.global external @" + sym +
            "(0 : i64) {addr_space = 0 : i32} : i64\n");
        continue;
      }
      // Uniform A-function ABI: every user fn/method/ctor/lambda/parfor
      // takes the packed argv pointer and returns an aval i64.
      funcDecls.push_back("  llvm.func @" + sym +
                          "(%argv: !llvm.ptr) -> i64\n");
    }
    if (funcDecls.empty() && globalDecls.empty()) return;
    std::sort(funcDecls.begin(), funcDecls.end());
    std::sort(globalDecls.begin(), globalDecls.end());
    std::ostringstream ins;
    for (const std::string& d : globalDecls) ins << d;
    for (const std::string& d : funcDecls) ins << d;
    // insert right after `module {\n` at the top of pre_
    std::string p = pre_.str();
    auto nl = p.find('\n');
    if (nl == std::string::npos) {
      p += ins.str();
    } else {
      p.insert(nl + 1, ins.str());
    }
    pre_.str("");
    pre_ << p;
  }

  // emitModuleInit: `__a_m<id>_init` — this module's global initializers and
  // top-level statements, in module order. This is the per-module unit the
  // program-global `__a_main_body` calls; see emitModuleMLIRDialect docs.
  // Mirrors MlirEmitter::emitModuleInit / the emitMainBody preamble.
  void emitModuleInit() {
    FnDecl dummy;
    dummy.moduleId = emitOnlyModule_;
    const std::string sym =
        "__a_m" + std::to_string(emitOnlyModule_) + "_init";
    beginFunction(sym, &dummy);
    // Same fix as emitMainBody: the dummy's empty body leaves the assign-kind
    // scan unarmed for this module's top-level stmts; scan them up front so
    // a nested emitStatements cannot consume it on a partial body.
    assignScanPending_ = false;
    for (const auto& ts : topStmts_)
      if (ts.moduleId == emitOnlyModule_) scanStmtAssigns(*ts.stmt);
    for (const auto& gi : globalInits_) {
      if (gi.moduleId != emitOnlyModule_) continue;
      curModuleId_ = gi.moduleId;
      if (gi.stmt->init) {
        std::string v = evalExpr(*gi.stmt->init);
        std::string g = globalSymFor(gi.stmt->name);
        if (g.empty()) g = globals_[gi.stmt->name];
        std::string r = nv();
        fn_ << "  " << r << " = llvm.mlir.addressof @" << g << " : !llvm.ptr\n";
        store(r, v);
      }
      if (failed_) return;
    }
    for (const auto& ts : topStmts_) {
      if (ts.moduleId != emitOnlyModule_) continue;
      curModuleId_ = ts.moduleId;
      emitStmt(*ts.stmt);
      if (failed_) return;
    }
    endFunction();
  }
};

// Defined below with the public entry points; declared here so
// runEntryBody can use it.
std::string moduleTextWithDeclsAD(const std::string& text);

// Emits the entry fn's body ops — the same statements emitMainBody inlines
// into __a_main_body for the entry portion — WITHOUT a function wrapper:
// `out` is splice-ready text for the consumer's synthesized __a_main_body
// (hoisted allocas first, then the statements). `support` receives a
// `module { ... }` document holding the module-level ops the body created
// (literal globals, auto-vivified __a_g_ globals, lambda/parfor helpers)
// for the precompile step to merge into the entry module's .ambc.
// Mirrors MlirEmitter::runEntryBody.
bool MlirDialectEmitter::runEntryBody(std::string& out, std::string& support,
                                      std::string& error) {
  error_ = &error;
  entryBodyMode_ = true;
  collectTopLevel();
  resolveImpls();
  if (failed_) return false;
  out.clear();
  support.clear();
  if (!entry_) return true;  // no entry fn: empty body, no support ops

  // Mirror emitMainBody's entry portion exactly: inFnBody_=true while
  // inUserFn_ stays false (bare `x = ...` binds a global, same as
  // whole-graph), module context = the entry's module so __a_g_<id>_*
  // names match the per-module units. No beginFunction: the consumer's
  // __a_main_body supplies the func frame (and deliberately no
  // a_frame_push, hence suppressFramePop_ for the body's own return).
  // assignScanPending_ is set manually because there is no beginFunction
  // call to arm the typed-binding assign-kind scan.
  curModuleId_ = entry_->moduleId;
  curFnName_ = entry_->name;
  curFnFile_ = (entry_->moduleId >= 0 &&
                entry_->moduleId < (int)graph_.modules.size())
                   ? graph_.at(entry_->moduleId).path
                   : "";
  curFnLine_ = entry_->span.line > 0 ? entry_->span.line : 0;
  pushScope();
  assignScanPending_ = true;
  assignKindMask_.clear();
  inFnBody_ = true;
  inEntryBody_ = true;
  suppressFramePop_ = true;
  emitStatements(entry_->body);
  if (!terminated_) {
    // endFunction's tail for the non-returning case: deferred stmts and
    // live-binding drops run before the consumer's appended none-return.
    emitPendingDefers();
    emitAllDrops();
  }
  suppressFramePop_ = false;
  inFnBody_ = false;
  inEntryBody_ = false;
  if (failed_) return false;
  if (terminated_) {
    // The entry body emitted `return` (llvm.return is a terminator). The
    // consumer always appends its own none-constant + llvm.return after
    // the splice, so open an unreachable block to keep __a_main_body
    // well-formed (ops after a terminator in one block are invalid).
    fn_ << "^a_entry_tail:\n";
    terminated_ = false;
  }
  // Snapshot the statement stream BEFORE draining helpers: emitLambdas
  // reuses fn_/fnAllocs_ for the helper function bodies.
  out = fnAllocs_.str() + fn_.str();
  emitLambdas();  // lambda/parfor helpers created by the entry body
  if (failed_) return false;
  // The support document is parsed standalone by the precompile step, so
  // it must declare every foreign/runtime symbol its helper funcs use —
  // the same job emitModuleDecls does for per-module units.
  support = moduleTextWithDeclsAD("module {\n" + pre_.str() + body_.str() +
                                  "}\n");
  return true;
}

// Inserts the declarations a standalone `module { ... }` parse needs:
// every @symbol referenced but not defined locally gets an llvm.func
// declaration (the uniform (!llvm.ptr) -> i64 A-function signature for
// user functions/methods/ctors/lambda+parfor helpers) or an
// llvm.mlir.global external declaration for global-shaped symbols.
// Mirrors MlirEmitter::moduleTextWithDecls, but for `a`-dialect emitter
// text: `a.runtime.call "sym"` ops carry their callee as a string attr
// (no @ref), and a-lower-to-llvm materializes their llvm.func decls, so
// only llvm.* @refs need declarations here.
std::string moduleTextWithDeclsAD(const std::string& text) {
  std::set<std::string> defined, referenced;
  static const std::regex defRe(
      "llvm\\.(?:func|mlir\\.global)[^@\\n]*@([A-Za-z_][A-Za-z0-9_]*)");
  static const std::regex refRe("@([A-Za-z_][A-Za-z0-9_]*)");
  for (std::sregex_iterator it(text.begin(), text.end(), defRe), end;
       it != end; ++it)
    defined.insert((*it)[1].str());
  for (std::sregex_iterator it(text.begin(), text.end(), refRe), end;
       it != end; ++it)
    referenced.insert((*it)[1].str());
  std::vector<std::string> funcDecls, globalDecls;
  for (const std::string& sym : referenced) {
    if (defined.count(sym)) continue;
    if (sym.rfind("__a_g_", 0) == 0) {
      globalDecls.push_back("llvm.mlir.global external @" + sym +
                            "(0 : i64) {addr_space = 0 : i32} : i64\n");
      continue;
    }
    funcDecls.push_back("llvm.func @" + sym +
                        "(%argv: !llvm.ptr) -> i64\n");
  }
  if (funcDecls.empty() && globalDecls.empty()) return text;
  std::sort(funcDecls.begin(), funcDecls.end());
  std::sort(globalDecls.begin(), globalDecls.end());
  std::ostringstream ins;
  for (const std::string& d : globalDecls) ins << d;
  for (const std::string& d : funcDecls) ins << d;
  const auto nl = text.find('\n');
  if (nl == std::string::npos) return ins.str() + text;
  return text.substr(0, nl + 1) + ins.str() + text.substr(nl + 1);
}

}  // namespace

bool emitMLIRDialect(const ModuleGraph& graph, const SemaResult& sema,
              std::string& out, std::string& error, bool testMode,
              const std::string* testFilter) {
  MlirDialectEmitter emitter(graph, sema, testMode, testFilter ? *testFilter : "");
  return emitter.run(out, error);
}

bool emitModuleMLIRDialect(const ModuleGraph& graph, const SemaResult& sema,
                           int moduleId, std::string& out,
                           std::string& error) {
  MlirDialectEmitter emitter(graph, sema);
  return emitter.runModule(moduleId, out, error);
}

bool emitEntryBodyMLIRDialect(const ModuleGraph& graph,
                              const SemaResult& sema, std::string& out,
                              std::string& error) {
  MlirDialectEmitter emitter(graph, sema);
  std::string support;
  return emitter.runEntryBody(out, support, error);
}

bool emitEntrySupportMLIRDialect(const ModuleGraph& graph,
                                 const SemaResult& sema, std::string& out,
                                 std::string& error) {
  MlirDialectEmitter emitter(graph, sema);
  std::string stmts;
  return emitter.runEntryBody(stmts, out, error);
}

}  // namespace a
