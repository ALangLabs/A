// Tree-walking interpreter backend for A ("--vm"). Executes a loaded,
// sema-checked program in-process on top of the C runtime's `aval`
// NaN-boxed value model (compiler/astra/runtime/a_runtime.h), so every
// value — ints, floats, strings, lists, dicts, options, results, iterators,
// builtin functions — is the exact same representation the compiled backend
// uses. VM-defined functions are ordinary FNON values whose entry point is
// a C++ trampoline; this keeps builtins that call back into A code (map,
// filter, sorted key=, ...) working unchanged through a_call.
//
// Known v1 limitations (all report "error: ... is not supported in --vm
// mode" and exit 1, except where noted):
//   * struct/class, enum, trait, impl and type-alias declarations
//   * python: module interop is partially supported (member calls route
//     through the py_call builtin; bare `import python:x` binds a marker)
//   * `import prelude` sees an empty module (the prelude is skipped exactly
//     like the codegen backend skips it for non-entry programs, and its
//     class/enum declarations are unsupported anyway)
//   * match variant patterns resolve through Sema's enum tables on a
//     best-effort basis
//   * named arguments to builtins/native functions are ignored (sema
//     rejects them for known signatures anyway)

#include "vm/vm.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "ast/ast.hpp"
#include "ast/builtins.hpp"
#include "codegen/codegen_internal.hpp"  // builtinRuntimeId / builtinIdForBuiltin
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"

extern "C" {
#include "a_runtime.h"
// Internal runtime entry points (non-static in the runtime .c modules; the
// public a_runtime.h does not declare them). Declared here so vm.cpp does
// not have to include a_runtime_internal.h (POSIX-only).
aval fnon_new_raw(uint64_t entry, aval env, const char *name, uint32_t flags,
                  uint64_t nparams);
void a_gc_mark_aval(aval v);
void a_gc_add_root_cb(void (*fn)(void));
}

namespace a::vm {
namespace {

// ---------------------------------------------------------------------------
// Exceptions used for non-local control flow inside the interpreter.
// ---------------------------------------------------------------------------
struct VmReturn {
  aval v;
};
struct VmPropagate {
  aval v;  // `expr?` early-returns this value from the enclosing function
};
struct VmBreak {};
struct VmContinue {};
struct VmUnsupported {
  std::string msg;
};

// ---------------------------------------------------------------------------
// Scopes: Python-style lexical chain. `let` declares in the current scope;
// `name = v` writes through to the owning scope when the name already
// exists. Every scope ever created is retained in scopeStore so closure env
// keys stay valid for the whole session (sessions are short-lived).
// ---------------------------------------------------------------------------
struct Scope {
  std::vector<std::pair<std::string, aval>> vars;
  std::shared_ptr<Scope> parent;
  uint64_t key = 0;  // index into VmState::scopeStore
};
using ScopePtr = std::shared_ptr<Scope>;

// A VM-callable function value. `capture` is the scope the function was
// defined in (stored in the closure env so calls routed through a_call —
// e.g. from map/filter — rebuild the same parent chain).
struct VmFn {
  enum class Kind { Decl, Lambda, PyCall } kind = Kind::Decl;
  const FnDecl* decl = nullptr;
  const LambdaExpr* lambda = nullptr;
  ScopePtr capture;
  int moduleId = -1;
  // Kind::PyCall: `from python:<mod> import name` — calling it routes through
  // the runtime py_call builtin with argv = [module, member, args...].
  std::string pyModule;
  std::string pyMember;
};

struct VmState {
  std::vector<ScopePtr> scopeStore;   // scopeKey -> Scope (never shrinks)
  std::vector<VmFn> fns;              // fnKey -> VmFn (never shrinks)
  std::unordered_map<aval, std::pair<uint64_t, uint64_t>> fnInfo;
  //   fn aval -> (fnKey, scopeKey of the captured scope)
  std::unordered_map<aval, int> moduleOf;        // "<module>" aval -> moduleId
  std::unordered_map<aval, std::string> nativeOf;  // aval -> native module name
  std::unordered_map<aval, std::string> pyModOf;   // aval -> python module name
  std::unordered_map<const FnDecl*, uint64_t> declFnKey;
  std::unordered_map<int, ScopePtr> moduleScopes;
  std::vector<ScopePtr> allScopes;  // GC root walk (superset of scopeStore)

  const ModuleGraph* graph = nullptr;
  const SemaResult* sema = nullptr;
  int curModule = -1;
};

VmState g_vm;

// Runtime helpers not in the public header (see extern "C" block above).
constexpr uint64_t kVmMissing = A_TAG;  // codegen's kMissing sentinel

[[noreturn]] void vmUnsupported(const std::string& what) {
  throw VmUnsupported{what};
}

ScopePtr vmNewScope(ScopePtr parent) {
  auto s = std::make_shared<Scope>();
  s->parent = std::move(parent);
  s->key = g_vm.scopeStore.size();
  g_vm.scopeStore.push_back(s);
  g_vm.allScopes.push_back(s);
  return s;
}

// Deepest scope in the chain that binds `name`, or nullptr.
Scope* vmFindScope(ScopePtr s, const std::string& name) {
  for (Scope* cur = s.get(); cur; cur = cur->parent.get()) {
    for (const auto& kv : cur->vars)
      if (kv.first == name) return cur;
  }
  return nullptr;
}

// Read `name` through the chain; returns false when unbound.
bool vmScopeGet(ScopePtr s, const std::string& name, aval& out) {
  for (Scope* cur = s.get(); cur; cur = cur->parent.get()) {
    for (const auto& kv : cur->vars)
      if (kv.first == name) {
        out = kv.second;
        return true;
      }
  }
  return false;
}

// Write `name`: update the owning scope when bound, else declare locally.
void vmScopeSet(ScopePtr s, const std::string& name, aval v) {
  if (Scope* owner = vmFindScope(s, name)) {
    for (auto& kv : owner->vars)
      if (kv.first == name) {
        kv.second = v;
        return;
      }
  }
  for (auto& kv : s->vars)
    if (kv.first == name) {
      kv.second = v;
      return;
    }
  s->vars.emplace_back(name, v);
}

// Declare (or shadow) `name` in exactly this scope.
void vmScopeDeclare(ScopePtr s, const std::string& name, aval v) {
  for (auto& kv : s->vars)
    if (kv.first == name) {
      kv.second = v;
      return;
    }
  s->vars.emplace_back(name, v);
}

// ---------------------------------------------------------------------------
// Function values.
// ---------------------------------------------------------------------------
// Forward decl (mutual recursion between call paths and the trampoline).
aval vmTrampoline(aval* argv);

uint64_t vmAddFn(VmFn f) {
  g_vm.fns.push_back(std::move(f));
  return g_vm.fns.size() - 1;
}

// Create the FNON value for fnKey captured in `capture`. env layout:
//   slot 0 = fnKey (inline int)
//   slot 1 = scopeKey of the captured scope (inline int)
aval vmMakeCallable(uint64_t fnKey, const ScopePtr& capture, const char* name) {
  aval env = a_closure_env_new(2);
  a_env_set(env, 0, a_mkint((int64_t)fnKey));
  a_env_set(env, 1, a_mkint((int64_t)capture->key));
  // nparams = fixed call arity so a_call's arity check/padding matches the
  // emitted-code convention (callers always pass exactly arity args, with
  // kVmMissing in defaulted slots).
  uint64_t nparams = UINT64_MAX;
  const VmFn& f = g_vm.fns[fnKey];
  if (f.kind == VmFn::Kind::Decl && f.decl)
    nparams = (uint64_t)(f.decl->comptimeParams.size() + f.decl->params.size());
  else if (f.kind == VmFn::Kind::Lambda && f.lambda)
    nparams = (uint64_t)f.lambda->params.size();
  else if (f.kind == VmFn::Kind::PyCall)
    nparams = UINT64_MAX;  // py_call takes variadic argv
  aval v = fnon_new_raw((uint64_t)(uintptr_t)&vmTrampoline, env, name, 0,
                        nparams);
  g_vm.fnInfo[v] = {fnKey, capture->key};
  return v;
}

// Forward decls (mutual recursion between call paths and the trampoline).
aval vmTrampoline(aval* argv);
aval vmCall(uint64_t fnKey, ScopePtr capture, std::vector<aval>& args);

struct Vm {
  const ModuleGraph& graph;
  const SemaResult& sema;

  explicit Vm(const ModuleGraph& g, const SemaResult& s) : graph(g), sema(s) {}

  // ---- expressions -------------------------------------------------------
  aval eval(const Expr* e, ScopePtr scope);
  aval evalCall(const CallExpr& c, ScopePtr scope);
  aval callValue(aval fn, std::vector<aval>& args, ScopePtr scope);
  bool matchPattern(const Pattern& p, aval subj, ScopePtr scope);
  void execStmt(const Stmt* s, ScopePtr scope);
  void execStmts(const std::vector<StmtPtr>& stmts, ScopePtr scope);
  void execBlock(const std::vector<StmtPtr>& stmts, ScopePtr scope);
  void bindModuleImports(const Module& mod, ScopePtr scope);
  void execModule(int moduleId);
};

Vm* g_vmExec = nullptr;  // active interpreter (for the trampoline)

// Parameter names of a VmFn in binding order: comptime params first, then
// value params (mirrors codegen_decls.cpp FnInfo::paramNames).
std::vector<std::string> vmParamNames(const VmFn& f) {
  std::vector<std::string> names;
  if (f.kind == VmFn::Kind::Decl && f.decl) {
    for (const ComptimeParam& p : f.decl->comptimeParams) names.push_back(p.name);
    for (const ParamDecl& p : f.decl->params) names.push_back(p.name);
  } else if (f.kind == VmFn::Kind::Lambda && f.lambda) {
    for (const ParamDecl& p : f.lambda->params) names.push_back(p.name);
  }
  return names;
}

// Invoke a VM function value. `args` are the user arguments (no env slot);
// they are padded to the declared arity with kVmMissing so default
// expressions and A_NONE filling behave like the emitted backend.
aval Vm::callValue(aval fn, std::vector<aval>& args, ScopePtr scope) {
  (void)scope;
  auto it = g_vm.fnInfo.find(fn);
  if (it != g_vm.fnInfo.end()) {
    uint64_t key = it->second.first;
    ScopePtr capture = g_vm.scopeStore[it->second.second];
    return vmCall(key, capture, args);
  }
  // Builtin / native / struct-method FNON (or anything else a_call accepts).
  aval* argv = args.empty() ? nullptr : args.data();
  return a_call(fn, (uint64_t)args.size(), argv);
}

// Core VM call: bind params into a fresh scope chained to the captured
// (definition) scope, then run the body. Reached both directly from
// callValue and through vmTrampoline when native code calls back.
aval vmCall(uint64_t fnKey, ScopePtr capture, std::vector<aval>& args) {
  VmFn& f = g_vm.fns[fnKey];
  if (f.kind == VmFn::Kind::PyCall) {
    // from python:<mod> import name -> py_call(mod, name, args...)
    std::vector<aval> full;
    full.reserve(args.size() + 2);
    full.push_back(a_str_new(f.pyModule.data(), f.pyModule.size()));
    full.push_back(a_str_new(f.pyMember.data(), f.pyMember.size()));
    for (aval a : args) full.push_back(a);
    return a_bi_call(A_BI_PY_CALL, (uint64_t)full.size(), full.data());
  }

  ScopePtr fnScope = vmNewScope(capture);
  const FnDecl* decl = f.kind == VmFn::Kind::Decl ? f.decl : nullptr;
  const LambdaExpr* lam = f.kind == VmFn::Kind::Lambda ? f.lambda : nullptr;

  // Arity: comptime params then params. Variadic last param collects the
  // remaining positional args into a list (mirrors placeArgs).
  const std::vector<ParamDecl>* params =
      decl ? &decl->params : (lam ? &lam->params : nullptr);
  std::size_t nParams = params ? params->size() : 0;
  bool variadic = !params->empty() && params->back().variadic;
  std::size_t fixed = nParams - (variadic ? 1 : 0);

  std::size_t ai = 0;
  auto nextArg = [&]() -> aval {
    if (ai < args.size()) return args[ai++];
    return kVmMissing;
  };

  // Comptime params: plain bindings, no defaults.
  if (decl) {
    for (const ComptimeParam& p : decl->comptimeParams) {
      aval v = nextArg();
      vmScopeDeclare(fnScope, p.name, v == kVmMissing ? A_NONE : v);
    }
  }

  for (std::size_t i = 0; i < fixed; ++i) {
    const ParamDecl& p = (*params)[i];
    aval v = nextArg();
    if (v == kVmMissing) {
      if (p.defaultValue) v = g_vmExec->eval(p.defaultValue.get(), fnScope);
      else v = A_NONE;
    }
    vmScopeDeclare(fnScope, p.name, v);
  }
  if (variadic) {
    const ParamDecl& p = params->back();
    std::vector<aval> rest;
    while (ai < args.size()) rest.push_back(args[ai++]);
    aval list = a_list_new((uint64_t)rest.size(),
                           rest.empty() ? nullptr : rest.data());
    vmScopeDeclare(fnScope, p.name, list);
  }

  const char* fname = decl ? decl->name.c_str() : "<lambda>";
  int line = decl ? decl->span.line : (lam ? lam->span.line : 0);
  a_frame_push(fname, "", line);
  try {
    if (decl) {
      g_vmExec->execStmts(decl->body, vmNewScope(fnScope));
    } else if (lam && lam->body) {
      aval r = g_vmExec->eval(lam->body.get(), vmNewScope(fnScope));
      a_frame_pop();
      return r;
    }
    a_frame_pop();
    return A_NONE;
  } catch (VmReturn& r) {
    a_frame_pop();
    return r.v;
  } catch (VmPropagate& p) {
    a_frame_pop();
    return p.v;
  } catch (...) {
    a_frame_pop();
    throw;
  }
}

// Trampoline invoked through a_call: argv[0] is the closure env, argv[1..n]
// the arguments. The env carries (fnKey, scopeKey) so calls routed through
// native code rebuild the same captured scope.
aval vmTrampoline(aval* argv) {
  aval env = argv[0];
  uint64_t fnKey = (uint64_t)a_int_val(a_env_get(env, 0));
  uint64_t scopeKey = (uint64_t)a_int_val(a_env_get(env, 1));
  ScopePtr capture = g_vm.scopeStore[scopeKey];
  // The callee's declared arity is fixed (nparams on the FNON), so argv[1..]
  // holds exactly that many slots; variadic packing already happened at the
  // original call site.
  const VmFn& f = g_vm.fns[fnKey];
  uint64_t nparams = UINT64_MAX;
  if (f.kind == VmFn::Kind::Decl && f.decl)
    nparams = (uint64_t)(f.decl->comptimeParams.size() + f.decl->params.size());
  else if (f.kind == VmFn::Kind::Lambda && f.lambda)
    nparams = (uint64_t)f.lambda->params.size();
  std::vector<aval> args;
  if (nparams != UINT64_MAX) {
    args.reserve(nparams);
    for (uint64_t i = 0; i < nparams; ++i) args.push_back(argv[1 + i]);
  } else {
    // Unknown arity (PyCall): cannot recover n from argv alone; pass none.
    // py_call members are normally invoked through callValue directly.
  }
  return vmCall(fnKey, capture, args);
}

// ---------------------------------------------------------------------------
// GC roots: every Scope's bindings are reachable from allScopes; mark them
// all on each collection. C++ locals live on the machine stack which the
// conservative marker already scans; heap-side aval vectors (call args) are
// pushed via a_gc_push_root around calls that can allocate.
// ---------------------------------------------------------------------------
void vmGcMark(void) {
  for (const ScopePtr& s : g_vm.allScopes) {
    if (!s) continue;
    for (const auto& kv : s->vars) a_gc_mark_aval(kv.second);
  }
}

// a_gc_push_root/pop_root are internal too; declare locally.
extern "C" void a_gc_push_root(aval v);
extern "C" void a_gc_pop_root(void);

struct VmCallGuard {
  explicit VmCallGuard(const std::vector<aval>& args) : n(args.size()) {
    for (aval a : args) a_gc_push_root(a);
  }
  ~VmCallGuard() {
    for (std::size_t i = 0; i < n; ++i) a_gc_pop_root();
  }
  std::size_t n;
};

// ---------------------------------------------------------------------------
// Evaluator.
// ---------------------------------------------------------------------------
aval Vm::eval(const Expr* e, ScopePtr scope) {
  if (!e) return A_NONE;
  switch (e->kind) {
    case ExprKind::IntLit: {
      int64_t v = static_cast<const IntLitExpr*>(e)->value;
      return a_in_inline_range(v) ? a_mkint(v) : a_i64n((long long)v);
    }
    case ExprKind::FloatLit:
      return a_double_canon(static_cast<const FloatLitExpr*>(e)->value);
    case ExprKind::BoolLit:
      return static_cast<const BoolLitExpr*>(e)->value ? A_TRUE : A_FALSE;
    case ExprKind::CharLit: {
      char32_t cp = static_cast<const CharLitExpr*>(e)->value;
      char buf[4];
      int n = 0;
      if (cp < 0x80) {
        buf[n++] = (char)cp;
      } else if (cp < 0x800) {
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
      return a_char_new(buf, (uint64_t)n);
    }
    case ExprKind::StringLit: {
      const auto& x = static_cast<const StringLitExpr&>(*e);
      if (x.parts.empty()) return a_str_new("", 0);
      if (x.parts.size() == 1 && !x.parts[0].isExpr)
        return a_str_new(x.parts[0].text.data(), x.parts[0].text.size());
      std::vector<aval> segs;
      segs.reserve(x.parts.size());
      for (const StringPart& p : x.parts) {
        if (p.isExpr) {
          aval v = eval(p.expr.get(), scope);
          segs.push_back(a_str(v));
        } else {
          segs.push_back(a_str_new(p.text.data(), p.text.size()));
        }
      }
      return a_str_join((uint64_t)segs.size(), segs.data());
    }
    case ExprKind::Ident: {
      const auto& x = static_cast<const IdentExpr&>(*e);
      const std::string& nm = x.name;
      // Compile-time constants (mirrors codegen emitIdent).
      if (nm == "None") return A_NONE;
      if (nm == "PI") return a_double_canon(3.14159265358979323846);
      if (nm == "E") return a_double_canon(2.71828182845904523536);
      if (nm == "I8_MIN") return a_mkint(-128);
      if (nm == "I8_MAX") return a_mkint(127);
      if (nm == "I16_MIN") return a_mkint(-32768);
      if (nm == "I16_MAX") return a_mkint(32767);
      if (nm == "I32_MIN") return a_mkint(-2147483648LL);
      if (nm == "I32_MAX") return a_mkint(2147483647LL);
      if (nm == "I64_MIN") return a_i64n(INT64_MIN);
      if (nm == "I64_MAX") return a_i64n(INT64_MAX);
      if (nm == "U8_MAX") return a_mkint(255);
      if (nm == "U16_MAX") return a_mkint(65535);
      if (nm == "U32_MAX") return a_mkint(4294967295LL);
      if (nm == "F32_MIN") return a_double_canon(-3.4028234663852886e38);
      if (nm == "F32_MAX") return a_double_canon(3.4028234663852886e38);
      if (nm == "F64_MIN") return a_double_canon(-1.7976931348623157e308);
      if (nm == "F64_MAX") return a_double_canon(1.7976931348623157e308);
      aval v;
      if (vmScopeGet(scope, nm, v)) return v;
      int bid = builtinRuntimeId(nm);
      if (bid >= 0) return a_builtin((uint64_t)bid);
      vmUnsupported("undefined name '" + nm + "'");
    }
    case ExprKind::Unary: {
      const auto& x = static_cast<const UnaryExpr&>(*e);
      if (x.op == "++" || x.op == "--") {
        // Prefix/postfix increment on a variable: read-modify-write the
        // scope slot; prefix yields the new value, postfix the old.
        const Expr* tgt = x.operand.get();
        if (tgt->kind != ExprKind::Ident)
          vmUnsupported("operand of '" + x.op + "' must be a variable");
        const std::string& nm = static_cast<const IdentExpr*>(tgt)->name;
        aval cur;
        if (!vmScopeGet(scope, nm, cur))
          vmUnsupported("undefined variable '" + nm + "'");
        aval delta = a_mkint(x.op == "++" ? 1 : -1);
        aval next = a_add(cur, delta);
        vmScopeSet(scope, nm, next);
        return x.postfix ? cur : next;
      }
      aval v = eval(x.operand.get(), scope);
      if (x.op == "-") return a_neg(v);
      if (x.op == "~") return a_bnot(v);
      if (x.op == "not" || x.op == "!") {
        // '!' is the postfix unwrap operator (parser reuses UnaryExpr).
        if (x.op == "!") return a_unwrap(v);
        return a_not(v);
      }
      vmUnsupported("unsupported unary operator '" + x.op + "'");
    }
    case ExprKind::Binary: {
      const auto& x = static_cast<const BinaryExpr&>(*e);
      switch (x.binop) {
        case BinOp::And: {
          aval l = eval(x.lhs.get(), scope);
          if (a_truthy_raw(l) == 0) return l;
          return eval(x.rhs.get(), scope);
        }
        case BinOp::Or: {
          aval l = eval(x.lhs.get(), scope);
          if (a_truthy_raw(l) != 0) return l;
          return eval(x.rhs.get(), scope);
        }
        case BinOp::Coalesce: {
          aval l = eval(x.lhs.get(), scope);
          if (l == A_NONE) return eval(x.rhs.get(), scope);
          return a_unwrap(l);
        }
        case BinOp::In:
        case BinOp::NotIn: {
          aval item = eval(x.lhs.get(), scope);
          aval cont = eval(x.rhs.get(), scope);
          aval r = a_contains(cont, item);
          return x.binop == BinOp::NotIn ? a_not(r) : r;
        }
        case BinOp::Range: {
          aval lo = eval(x.lhs.get(), scope);
          aval hi = eval(x.rhs.get(), scope);
          return a_range_new(lo, hi, a_mkint(1));
        }
        default: break;
      }
      aval l = eval(x.lhs.get(), scope);
      aval r = eval(x.rhs.get(), scope);
      switch (x.binop) {
        case BinOp::Add: return a_add(l, r);
        case BinOp::Sub: return a_sub(l, r);
        case BinOp::Mul: return a_mul(l, r);
        case BinOp::Div: return a_div(l, r);
        case BinOp::FloorDiv: return a_floordiv_slow(l, r);
        case BinOp::Mod: return a_mod(l, r);
        case BinOp::Pow: return a_pow(l, r);
        case BinOp::Eq: return a_eq(l, r);
        case BinOp::Ne: return a_ne(l, r);
        case BinOp::Lt: return a_lt(l, r);
        case BinOp::Le: return a_le(l, r);
        case BinOp::Gt: return a_gt(l, r);
        case BinOp::Ge: return a_ge(l, r);
        case BinOp::BitAnd: return a_band(l, r);
        case BinOp::BitOr: return a_bor(l, r);
        case BinOp::BitXor: return a_bxor(l, r);
        case BinOp::Shl: return a_shl(l, r);
        case BinOp::Shr: return a_shr(l, r);
        default:
          // Fall back to the source spelling for anything the enum misses.
          if (x.op == "||") return a_bor(l, r);
          if (x.op == "&&") return a_band(l, r);
          vmUnsupported("unsupported binary operator '" + x.op + "'");
      }
    }
    case ExprKind::Index: {
      const auto& x = static_cast<const IndexExpr&>(*e);
      aval obj = eval(x.object.get(), scope);
      aval idx = eval(x.index.get(), scope);
      return a_getitem(obj, idx);
    }
    case ExprKind::Slice: {
      const auto& x = static_cast<const SliceExpr&>(*e);
      aval obj = eval(x.object.get(), scope);
      aval lo = x.start ? eval(x.start.get(), scope) : A_NONE;
      aval hi = x.end ? eval(x.end.get(), scope) : A_NONE;
      aval st = x.step ? eval(x.step.get(), scope) : A_NONE;
      return a_slice(obj, lo, hi, st);
    }
    case ExprKind::Field: {
      const auto& x = static_cast<const FieldExpr&>(*e);
      aval obj = eval(x.object.get(), scope);
      auto mi = g_vm.moduleOf.find(obj);
      if (mi != g_vm.moduleOf.end()) {
        ScopePtr ms = g_vm.moduleScopes[mi->second];
        aval v;
        if (vmScopeGet(ms, x.field, v)) return v;
        int bid = builtinRuntimeId(x.field);
        if (bid >= 0) return a_builtin((uint64_t)bid);
        vmUnsupported("unknown module member '" + x.field + "'");
      }
      auto ni = g_vm.nativeOf.find(obj);
      if (ni != g_vm.nativeOf.end()) {
        const std::string& mod = ni->second;
        if (const std::string* cn = nativeConstant(mod, x.field)) {
          double v = *cn == "PI"   ? 3.14159265358979323846
                    : *cn == "E"    ? 2.71828182845904523536
                                    : 0.0;
          return a_double_canon(v);
        }
        if (const std::string* fn = nativeFunction(mod, x.field)) {
          int bid = builtinRuntimeId(*fn);
          if (bid >= 0) return a_builtin((uint64_t)bid);
        }
        int bid = builtinRuntimeId(x.field);
        if (bid >= 0) return a_builtin((uint64_t)bid);
        vmUnsupported("unknown native module member '" + x.field + "'");
      }
      auto pi = g_vm.pyModOf.find(obj);
      if (pi != g_vm.pyModOf.end()) {
        // python:<mod>.member as a value: materialize a callable marker that
        // routes through py_call when invoked.
        uint64_t key = vmAddFn(VmFn{VmFn::Kind::PyCall, nullptr, nullptr, scope,
                                    -1, pi->second, x.field});
        return vmMakeCallable(key, scope, x.field.c_str());
      }
      return a_getattr(obj, x.field.c_str());
    }
    case ExprKind::SafeNav: {
      const auto& x = static_cast<const SafeNavExpr&>(*e);
      aval obj = eval(x.object.get(), scope);
      if (obj == A_NONE) return A_NONE;
      aval inner = a_unwrap(obj);
      aval v = a_getattr(inner, x.field.c_str());
      if (x.fieldOptional) return v;
      // Mirror emitSafeNav: None stays None, an already-Some value passes
      // through, anything else is wrapped in Some.
      if (v == A_NONE) return A_NONE;
      if (a_is_option(v)) return v;
      return a_some_new(v);
    }
    case ExprKind::ArrayLit: {
      const auto& x = static_cast<const ArrayLitExpr&>(*e);
      std::vector<aval> items;
      items.reserve(x.elements.size());
      for (const ExprPtr& el : x.elements) items.push_back(eval(el.get(), scope));
      return a_list_new((uint64_t)items.size(),
                        items.empty() ? nullptr : items.data());
    }
    case ExprKind::Tuple: {
      const auto& x = static_cast<const TupleExpr&>(*e);
      std::vector<aval> items;
      items.reserve(x.elements.size());
      for (const ExprPtr& el : x.elements) items.push_back(eval(el.get(), scope));
      return a_tuple_new((uint64_t)items.size(),
                         items.empty() ? nullptr : items.data());
    }
    case ExprKind::MapLit: {
      const auto& x = static_cast<const MapLitExpr&>(*e);
      aval d = a_dict_new();
      for (const auto& kv : x.entries) {
        aval k = eval(kv.first.get(), scope);
        aval v = eval(kv.second.get(), scope);
        a_dict_set(d, k, v);
      }
      return d;
    }
    case ExprKind::SetLit: {
      const auto& x = static_cast<const SetLitExpr&>(*e);
      aval s = a_set_new();
      for (const ExprPtr& el : x.elements) a_set_add(s, eval(el.get(), scope));
      return s;
    }
    case ExprKind::Range: {
      const auto& x = static_cast<const RangeExpr&>(*e);
      aval lo = eval(x.start.get(), scope);
      aval hi = eval(x.end.get(), scope);
      aval st = x.step ? eval(x.step.get(), scope) : a_mkint(1);
      return a_range_new(lo, hi, st);
    }
    case ExprKind::Ternary: {
      const auto& x = static_cast<const TernaryExpr&>(*e);
      return a_truthy_raw(eval(x.cond.get(), scope))
                 ? eval(x.thenValue.get(), scope)
                 : eval(x.elseValue.get(), scope);
    }
    case ExprKind::Lambda: {
      const auto& x = static_cast<const LambdaExpr&>(*e);
      uint64_t key = vmAddFn(VmFn{VmFn::Kind::Lambda, nullptr, &x, scope, -1, "", ""});
      return vmMakeCallable(key, scope, "<lambda>");
    }
    case ExprKind::Comprehension: {
      const auto& x = static_cast<const ComprehensionExpr&>(*e);
      aval it = a_iter_new(eval(x.iterable.get(), scope));
      aval acc = x.isSet ? a_set_new() : a_list_new(0, nullptr);
      ScopePtr cs = vmNewScope(scope);
      vmScopeDeclare(cs, x.var, A_NONE);
      aval item;
      while (a_iter_next(it, &item)) {
        vmScopeSet(cs, x.var, item);
        if (x.condition && a_truthy_raw(eval(x.condition.get(), cs)) == 0)
          continue;
        aval v = eval(x.element.get(), cs);
        if (x.isSet) {
          a_set_add(acc, v);
        } else {
          a_list_push(acc, v);
        }
      }
      return acc;
    }
    case ExprKind::Match: {
      const auto& m = static_cast<const MatchExpr&>(*e);
      aval subj = eval(m.subject.get(), scope);
      for (const MatchArm& arm : m.arms) {
        ScopePtr armScope = vmNewScope(scope);
        if (!matchPattern(arm.pattern, subj, armScope)) continue;
        if (arm.guard && a_truthy_raw(eval(arm.guard.get(), armScope)) == 0)
          continue;
        return eval(arm.result.get(), armScope);
      }
      a_match_fail(subj);
      return A_NONE;
    }
    case ExprKind::Try: {
      const auto& x = static_cast<const TryExpr&>(*e);
      aval v = a_try_result(eval(x.operand.get(), scope));
      if (a_is_propagate(v)) throw VmPropagate{v};
      return v;
    }
    case ExprKind::Call:
      return evalCall(*static_cast<const CallExpr*>(e), scope);
    default:
      vmUnsupported("unsupported expression kind");
  }
}

// ---- match patterns -------------------------------------------------------
// Returns true when `pat` matches `subj`, binding variables into `scope`.
bool Vm::matchPattern(const Pattern& p, aval subj, ScopePtr scope) {
  switch (p.kind) {
    case Pattern::Kind::Wildcard:
      return true;
    case Pattern::Kind::Binding: {
      vmScopeDeclare(scope, p.name, subj);
      return true;
    }
    case Pattern::Kind::Literal: {
      aval v = eval(p.value.get(), scope);
      return a_truthy_raw(a_eq(subj, v)) != 0;
    }
    case Pattern::Kind::Range: {
      aval lo = eval(p.rangeStart.get(), scope);
      aval hi = eval(p.rangeEnd.get(), scope);
      return a_truthy_raw(a_in_range_inclusive(subj, lo, hi)) != 0;
    }
    case Pattern::Kind::Tuple: {
      aval len = a_len(subj);
      aval exp = a_mkint((int64_t)p.subpatterns.size());
      if (a_truthy_raw(a_eq(len, exp)) == 0) return false;
      // Bind subpatterns against element avals; do it in two phases so a
      // failed arm does not leave partial bindings (the scope is per-arm
      // anyway, but keep it clean).
      std::vector<std::pair<std::string, aval>> binds;
      std::vector<ScopePtr> subScopes;
      for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
        aval el = a_getitem(subj, a_mkint((int64_t)i));
        ScopePtr ss = vmNewScope(scope);
        if (!matchPattern(p.subpatterns[i], el, ss)) return false;
        subScopes.push_back(ss);
      }
      // Flatten: copy bound names from sub-scopes into the arm scope.
      for (ScopePtr& ss : subScopes)
        for (auto& kv : ss->vars) vmScopeDeclare(scope, kv.first, kv.second);
      return true;
    }
    case Pattern::Kind::Or: {
      for (const Pattern& alt : p.subpatterns) {
        ScopePtr altScope = vmNewScope(scope);
        if (matchPattern(alt, subj, altScope)) {
          for (auto& kv : altScope->vars)
            vmScopeDeclare(scope, kv.first, kv.second);
          return true;
        }
      }
      return false;
    }
    case Pattern::Kind::Variant: {
      // Algebraic variant: subject is a dict with "__tag"/"__name". Resolve
      // the ctor's tag through the sema enum tables when possible.
      int64_t tag = -1;
      const FieldExpr* fe = dynamic_cast<const FieldExpr*>(p.ctor.get());
      if (fe) {
        const IdentExpr* ie = dynamic_cast<const IdentExpr*>(fe->object.get());
        if (ie) {
          const EnumInfo* ei = nullptr;
          auto it = sema.enums.find(ie->name);
          if (it != sema.enums.end()) {
            ei = &it->second;
          } else {
            // Try qualified "Enum.Variant" style keys too.
            for (const auto& kv : sema.enums) {
              if (kv.second.findVariant(fe->field) &&
                  (kv.first == ie->name ||
                   (kv.first.size() > ie->name.size() &&
                    kv.first.compare(kv.first.size() - ie->name.size(),
                                     ie->name.size(), ie->name) == 0))) {
                ei = &kv.second;
                break;
              }
            }
          }
          if (ei) {
            const VariantInfo* vi = ei->findVariant(fe->field);
            if (vi) {
              for (std::size_t i = 0; i < ei->variants.size(); ++i)
                if (&ei->variants[i] == vi) tag = (int64_t)i;
            }
          }
        }
      }
      if (tag < 0) {
        // Fall back to comparing the ctor value itself (simple variants).
        aval cv = eval(p.ctor.get(), scope);
        return a_truthy_raw(a_eq(subj, cv)) != 0;
      }
      aval tagKey = a_str_new("__tag", 5);
      aval subjTag = a_dict_get(subj, tagKey, A_NONE);
      if (a_truthy_raw(a_eq(subjTag, a_mkint(tag))) == 0) return false;
      // Payload subpatterns bind against the variant's payload fields.
      if (!p.subpatterns.empty() && !p.variantFields.empty()) {
        std::size_t n = p.subpatterns.size() < p.variantFields.size()
                            ? p.subpatterns.size()
                            : p.variantFields.size();
        for (std::size_t i = 0; i < n; ++i) {
          aval fk = a_str_new(p.variantFields[i].data(),
                              p.variantFields[i].size());
          aval fv = a_dict_get(subj, fk, A_NONE);
          ScopePtr ss = vmNewScope(scope);
          if (!matchPattern(p.subpatterns[i], fv, ss)) return false;
          for (auto& kv : ss->vars) vmScopeDeclare(scope, kv.first, kv.second);
        }
      }
      return true;
    }
  }
  return false;
}

// ---- calls ----------------------------------------------------------------
// Resolve positional+named args into a flat aval vector for a_bi_call /
// a_call (user args only). Named args are appended as (name, value) pairs
// for a_method receivers; for plain calls they are matched by vmCall's
// parameter binding when the callee is a VM function — handled there.
aval Vm::evalCall(const CallExpr& c, ScopePtr scope) {
  // 1. Sema-resolved builtin id.
  if (c.builtinId != BuiltinId::None) {
    int id = builtinIdForBuiltin(c.builtinId);
    if (id >= 0) {
      std::vector<aval> args;
      args.reserve(c.args.size());
      for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
      VmCallGuard g(args);
      return a_bi_call((uint64_t)id, (uint64_t)args.size(), args.data());
    }
  }
  // 2. Sema-resolved FnDecl: call it directly.
  if (c.calleeFunction) {
    auto it = g_vm.declFnKey.find(c.calleeFunction);
    uint64_t key;
    if (it != g_vm.declFnKey.end()) {
      key = it->second;
    } else {
      // Decl not yet executed (defensive): register it against its module
      // scope so the call still works.
      int mid = -1;
      auto mi = sema.fnModule.find(c.calleeFunction);
      if (mi != sema.fnModule.end()) mid = mi->second;
      ScopePtr ms = mid >= 0 ? g_vm.moduleScopes[mid]
                             : (scope ? scope : vmNewScope(nullptr));
      VmFn f{VmFn::Kind::Decl, c.calleeFunction, nullptr, ms, mid, "", ""};
      key = vmAddFn(std::move(f));
      g_vm.declFnKey[c.calleeFunction] = key;
    }
    std::vector<aval> args;
    args.reserve(c.args.size());
    for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
    VmCallGuard g(args);
    ScopePtr capture = g_vm.fns[key].capture;
    return vmCall(key, capture, args);
  }
  // 3. Sema-resolved runtime builtin (native-module from-import etc.).
  if (c.calleeIsBuiltin) {
    std::string nm = c.calleeBuiltin;
    if (nm.empty() && c.callee->kind == ExprKind::Ident)
      nm = static_cast<const IdentExpr*>(c.callee.get())->name;
    int bid = builtinRuntimeId(nm);
    if (bid >= 0) {
      std::vector<aval> args;
      args.reserve(c.args.size());
      for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
      VmCallGuard g(args);
      return a_bi_call((uint64_t)bid, (uint64_t)args.size(), args.data());
    }
  }
  if (c.calleeStruct)
    vmUnsupported("struct constructor calls are not supported in --vm mode");

  // 4. Field callee: module member call or generic a_method dispatch.
  if (c.callee->kind == ExprKind::Field) {
    const FieldExpr& f = static_cast<const FieldExpr&>(*c.callee);
    aval obj = eval(f.object.get(), scope);
    auto mi = g_vm.moduleOf.find(obj);
    if (mi != g_vm.moduleOf.end()) {
      ScopePtr ms = g_vm.moduleScopes[mi->second];
      aval v;
      bool found = vmScopeGet(ms, f.field, v);
      if (!found) {
        int bid = builtinRuntimeId(f.field);
        if (bid >= 0) {
          std::vector<aval> args;
          for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
          VmCallGuard g(args);
          return a_bi_call((uint64_t)bid, (uint64_t)args.size(), args.data());
        }
        vmUnsupported("unknown module member '" + f.field + "'");
      }
      std::vector<aval> args;
      for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
      VmCallGuard g(args);
      return callValue(v, args, scope);
    }
    auto ni = g_vm.nativeOf.find(obj);
    if (ni != g_vm.nativeOf.end()) {
      const std::string& mod = ni->second;
      int bid = -1;
      if (const std::string* fn = nativeFunction(mod, f.field))
        bid = builtinRuntimeId(*fn);
      if (bid < 0) bid = builtinRuntimeId(f.field);
      if (bid < 0)
        vmUnsupported("native module function not supported: " + mod + "." +
                      f.field);
      std::vector<aval> args;
      for (const CallArg& a : c.args) args.push_back(eval(a.value.get(), scope));
      VmCallGuard g(args);
      return a_bi_call((uint64_t)bid, (uint64_t)args.size(), args.data());
    }
    auto pi = g_vm.pyModOf.find(obj);
    if (pi != g_vm.pyModOf.end()) {
      std::vector<aval> full;
      full.push_back(a_str_new(pi->second.data(), pi->second.size()));
      full.push_back(a_str_new(f.field.data(), f.field.size()));
      for (const CallArg& a : c.args) full.push_back(eval(a.value.get(), scope));
      VmCallGuard g(full);
      return a_bi_call(A_BI_PY_CALL, (uint64_t)full.size(), full.data());
    }
    // Generic method call: a_method(obj, name, n, nkw, argv).
    std::vector<aval> argv;
    std::vector<std::pair<std::string, aval>> kwargs;
    for (const CallArg& a : c.args) {
      aval v = eval(a.value.get(), scope);
      if (a.name.empty()) {
        argv.push_back(v);
      } else {
        kwargs.emplace_back(a.name, v);
      }
    }
    uint64_t nkw = (uint64_t)kwargs.size();
    std::vector<aval> flat;
    flat.reserve(argv.size() + nkw * 2);
    for (aval v : argv) flat.push_back(v);
    for (auto& kv : kwargs) {
      flat.push_back(a_str_new(kv.first.data(), kv.first.size()));
      flat.push_back(kv.second);
    }
    VmCallGuard g(flat);
    return a_method(obj, f.field.c_str(), (uint64_t)argv.size(), nkw,
                    flat.empty() ? nullptr : flat.data());
  }

  // 5. Generic callee: evaluate to a value and call it.
  aval fn = eval(c.callee.get(), scope);
  // Named arguments for VM functions: match them to parameter names.
  std::vector<aval> args;
  std::vector<std::pair<std::string, aval>> kwargs;
  for (const CallArg& a : c.args) {
    aval v = eval(a.value.get(), scope);
    if (a.name.empty()) args.push_back(v);
    else kwargs.emplace_back(a.name, v);
  }
  if (!kwargs.empty()) {
    auto it = g_vm.fnInfo.find(fn);
    if (it != g_vm.fnInfo.end()) {
      const VmFn& vf = g_vm.fns[it->second.first];
      std::vector<std::string> names = vmParamNames(vf);
      // Expand kwargs into positional slots (named args fill their slot;
      // positional args already occupy the front).
      std::vector<aval> expanded(args.size() + kwargs.size(), kVmMissing);
      for (std::size_t i = 0; i < args.size(); ++i) expanded[i] = args[i];
      for (auto& kv : kwargs) {
        bool done = false;
        for (std::size_t i = 0; i < names.size(); ++i) {
          if (names[i] == kv.first) {
            if (i >= expanded.size()) expanded.resize(i + 1, kVmMissing);
            expanded[i] = kv.second;
            done = true;
            break;
          }
        }
        if (!done) vmUnsupported("no parameter named '" + kv.first + "'");
      }
      args = std::move(expanded);
    } else {
      vmUnsupported("named arguments are not supported in --vm mode");
    }
  }
  VmCallGuard g(args);
  return callValue(fn, args, scope);
}

// ---- statements ------------------------------------------------------------
void Vm::execStmts(const std::vector<StmtPtr>& stmts, ScopePtr scope) {
  // Defer bodies run when this statement list exits, for any reason
  // (mirrors emitDeferredDownTo in codegen).
  std::vector<const Stmt*> defers;
  try {
    for (const StmtPtr& s : stmts) {
      if (!s) continue;
      if (s->span.line > 0) a_stmt_line = s->span.line;
      if (s->kind == StmtKind::Defer) {
        defers.push_back(static_cast<const DeferStmt*>(s.get())->body.get());
        continue;
      }
      execStmt(s.get(), scope);
    }
  } catch (...) {
    for (auto it = defers.rbegin(); it != defers.rend(); ++it)
      execStmt(*it, scope);
    throw;
  }
  for (auto it = defers.rbegin(); it != defers.rend(); ++it)
    execStmt(*it, scope);
}

void Vm::execBlock(const std::vector<StmtPtr>& stmts, ScopePtr scope) {
  execStmts(stmts, vmNewScope(scope));
}

// Binary operator application shared by Binary exprs and compound assigns.
aval vmBinOp(const std::string& op, BinOp binop, aval l, aval r) {
  switch (binop) {
    case BinOp::Add: return a_add(l, r);
    case BinOp::Sub: return a_sub(l, r);
    case BinOp::Mul: return a_mul(l, r);
    case BinOp::Div: return a_div(l, r);
    case BinOp::FloorDiv: return a_floordiv_slow(l, r);
    case BinOp::Mod: return a_mod(l, r);
    case BinOp::Pow: return a_pow(l, r);
    case BinOp::Eq: return a_eq(l, r);
    case BinOp::Ne: return a_ne(l, r);
    case BinOp::Lt: return a_lt(l, r);
    case BinOp::Le: return a_le(l, r);
    case BinOp::Gt: return a_gt(l, r);
    case BinOp::Ge: return a_ge(l, r);
    case BinOp::BitAnd: return a_band(l, r);
    case BinOp::BitOr: return a_bor(l, r);
    case BinOp::BitXor: return a_bxor(l, r);
    case BinOp::Shl: return a_shl(l, r);
    case BinOp::Shr: return a_shr(l, r);
    default: break;
  }
  if (op == "+") return a_add(l, r);
  if (op == "-") return a_sub(l, r);
  if (op == "*") return a_mul(l, r);
  if (op == "/") return a_div(l, r);
  if (op == "//") return a_floordiv_slow(l, r);
  if (op == "%") return a_mod(l, r);
  if (op == "**") return a_pow(l, r);
  if (op == "==") return a_eq(l, r);
  if (op == "!=") return a_ne(l, r);
  if (op == "<") return a_lt(l, r);
  if (op == "<=") return a_le(l, r);
  if (op == ">") return a_gt(l, r);
  if (op == ">=") return a_ge(l, r);
  if (op == "&") return a_band(l, r);
  if (op == "|") return a_bor(l, r);
  if (op == "^") return a_bxor(l, r);
  if (op == "<<") return a_shl(l, r);
  if (op == ">>") return a_shr(l, r);
  vmUnsupported("unsupported assignment operator '" + op + "'");
}

// Read the current value of an assignment target.
aval vmReadTarget(const Expr* target, ScopePtr scope, Vm& vm) {
  if (target->kind == ExprKind::Ident) {
    aval v;
    if (vmScopeGet(scope, static_cast<const IdentExpr*>(target)->name, v))
      return v;
    return A_NONE;
  }
  if (target->kind == ExprKind::Index) {
    const auto& x = static_cast<const IndexExpr&>(*target);
    aval obj = vm.eval(x.object.get(), scope);
    aval idx = vm.eval(x.index.get(), scope);
    return a_getitem(obj, idx);
  }
  if (target->kind == ExprKind::Field) {
    const auto& x = static_cast<const FieldExpr&>(*target);
    aval obj = vm.eval(x.object.get(), scope);
    return a_getattr(obj, x.field.c_str());
  }
  vmUnsupported("unsupported assignment target");
}

// Write `v` to an assignment target.
void vmWriteTarget(const Expr* target, aval v, ScopePtr scope, Vm& vm) {
  if (target->kind == ExprKind::Ident) {
    vmScopeSet(scope, static_cast<const IdentExpr*>(target)->name, v);
    return;
  }
  if (target->kind == ExprKind::Index) {
    const auto& x = static_cast<const IndexExpr&>(*target);
    aval obj = vm.eval(x.object.get(), scope);
    aval idx = vm.eval(x.index.get(), scope);
    a_setitem(obj, idx, v);
    return;
  }
  if (target->kind == ExprKind::Field) {
    const auto& x = static_cast<const FieldExpr&>(*target);
    aval obj = vm.eval(x.object.get(), scope);
    a_setattr(obj, x.field.c_str(), v);
    return;
  }
  vmUnsupported("unsupported assignment target");
}

void Vm::execStmt(const Stmt* s, ScopePtr scope) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::Let: {
      const auto& st = static_cast<const LetStmt&>(*s);
      aval v = st.init ? eval(st.init.get(), scope) : A_NONE;
      vmScopeDeclare(scope, st.name, v);
      return;
    }
    case StmtKind::Assign: {
      const auto& st = static_cast<const AssignStmt&>(*s);
      if (st.op == "=") {
        aval v = eval(st.value.get(), scope);
        vmWriteTarget(st.target.get(), v, scope, *this);
      } else {
        std::string baseOp = st.op.substr(0, st.op.size() - 1);
        aval cur = vmReadTarget(st.target.get(), scope, *this);
        aval rhs = eval(st.value.get(), scope);
        aval v = vmBinOp(baseOp, st.binop, cur, rhs);
        vmWriteTarget(st.target.get(), v, scope, *this);
      }
      return;
    }
    case StmtKind::Expr: {
      const auto& st = static_cast<const ExprStmt&>(*s);
      std::string dummy;
      if (plainStringText(st.expr, dummy)) return;  // docstring
      eval(st.expr.get(), scope);
      return;
    }
    case StmtKind::Destructure: {
      const auto& st = static_cast<const DestructureStmt&>(*s);
      aval v = eval(st.value.get(), scope);
      for (std::size_t i = 0; i < st.names.size(); ++i) {
        if (st.names[i] == "_") continue;
        aval el = a_getitem(v, a_mkint((int64_t)i));
        vmScopeDeclare(scope, st.names[i], el);
      }
      return;
    }
    case StmtKind::Return: {
      const auto& st = static_cast<const ReturnStmt&>(*s);
      throw VmReturn{st.value ? eval(st.value.get(), scope) : A_NONE};
    }
    case StmtKind::Break:
      throw VmBreak{};
    case StmtKind::Continue:
      throw VmContinue{};
    case StmtKind::Defer:
      return;  // collected by execStmts
    case StmtKind::If: {
      const auto& st = static_cast<const IfStmt&>(*s);
      for (const auto& br : st.branches) {
        if (a_truthy_raw(eval(br.cond.get(), scope)) != 0) {
          execBlock(br.body, scope);
          return;
        }
      }
      if (!st.elseBody.empty()) execBlock(st.elseBody, scope);
      return;
    }
    case StmtKind::While: {
      const auto& st = static_cast<const WhileStmt&>(*s);
      while (a_truthy_raw(eval(st.cond.get(), scope)) != 0) {
        try {
          execBlock(st.body, scope);
        } catch (VmBreak&) {
          break;
        } catch (VmContinue&) {
          continue;
        }
      }
      return;
    }
    case StmtKind::DoWhile: {
      const auto& st = static_cast<const DoWhileStmt&>(*s);
      while (true) {
        try {
          execBlock(st.body, scope);
        } catch (VmBreak&) {
          break;
        } catch (VmContinue&) {
        }
        if (a_truthy_raw(eval(st.cond.get(), scope)) == 0) break;
      }
      return;
    }
    case StmtKind::For: {
      const auto& st = static_cast<const ForStmt&>(*s);
      aval it = a_iter_new(eval(st.iterable.get(), scope));
      aval item;
      while (a_iter_next(it, &item)) {
        ScopePtr iterScope = vmNewScope(scope);
        vmScopeDeclare(iterScope, st.var, item);
        try {
          execStmts(st.body, vmNewScope(iterScope));
        } catch (VmBreak&) {
          break;
        } catch (VmContinue&) {
          continue;
        }
      }
      return;
    }
    case StmtKind::Loop: {
      const auto& st = static_cast<const LoopStmt&>(*s);
      while (true) {
        try {
          execBlock(st.body, scope);
        } catch (VmBreak&) {
          break;
        } catch (VmContinue&) {
          continue;
        }
      }
      return;
    }
    case StmtKind::FnDecl: {
      const auto& st = static_cast<const FnDecl&>(*s);
      int mid = st.moduleId >= 0 ? st.moduleId : g_vm.curModule;
      VmFn f{VmFn::Kind::Decl, &st, nullptr, scope, mid, "", ""};
      uint64_t key = vmAddFn(std::move(f));
      g_vm.declFnKey[&st] = key;
      aval v = vmMakeCallable(key, scope, st.name.c_str());
      vmScopeDeclare(scope, st.name, v);
      return;
    }
    case StmtKind::Import:
    case StmtKind::FromImport:
      return;  // bound ahead of execution by bindModuleImports
    case StmtKind::StructDecl:
      vmUnsupported("struct declarations are not supported in --vm mode");
    case StmtKind::EnumDecl:
      vmUnsupported("enum declarations are not supported in --vm mode");
    case StmtKind::TraitDecl:
      vmUnsupported("trait declarations are not supported in --vm mode");
    case StmtKind::ImplDecl:
      vmUnsupported("impl declarations are not supported in --vm mode");
    case StmtKind::TypeAlias:
      vmUnsupported("type alias declarations are not supported in --vm mode");
  }
  vmUnsupported("unsupported statement kind");
}

// ---- module loading ---------------------------------------------------------
// Materialize the namespace marker aval for `import x`: a "<module 'name'>"
// string (same repr the codegen backend produces when a module is used as a
// value) plus a side-table entry so field access / calls resolve members.
aval vmModuleMarker(const std::string& display, int moduleId,
                    const std::string& nativeName, const std::string& pyName,
                    ScopePtr scope) {
  std::string msg = "<module '" + display + "'>";
  aval v = a_str_new(msg.data(), msg.size());
  if (moduleId >= 0) {
    g_vm.moduleOf[v] = moduleId;
  } else if (!nativeName.empty()) {
    g_vm.nativeOf[v] = nativeName;
  } else if (!pyName.empty()) {
    g_vm.pyModOf[v] = pyName;
  }
  (void)scope;
  return v;
}

void Vm::bindModuleImports(const Module& mod, ScopePtr scope) {
  if (!mod.program) return;
  for (const StmtPtr& s : mod.program->statements) {
    if (s->kind == StmtKind::Import) {
      const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
      if (im->pythonModuleImport) {
        std::string alias = !im->alias.empty() ? im->alias
                            : stemOf(im->pythonModule);
        aval v = vmModuleMarker(alias, -1, "", im->pythonModule, scope);
        vmScopeDeclare(scope, alias, v);
        continue;
      }
      std::string alias = !im->alias.empty() ? im->alias
                          : !im->bareName.empty() ? im->bareName
                          : stemOf(im->path);
      int mid = im->moduleId;
      if (im->nativeModule) {
        // An A package with the same name shadows the native namespace
        // (mirrors bindModuleImports in codegen_decls.cpp).
        for (const Module& cand : graph.modules) {
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
        aval v = vmModuleMarker(alias, mid, "", "", scope);
        vmScopeDeclare(scope, alias, v);
      } else if (im->nativeModule) {
        aval v = vmModuleMarker(alias, -1, im->bareName, "", scope);
        vmScopeDeclare(scope, alias, v);
      } else {
        vmUnsupported("cannot resolve import '" + alias + "'");
      }
    } else if (s->kind == StmtKind::FromImport) {
      const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
      if (im->pythonModuleImport) {
        for (const auto& item : im->items) {
          uint64_t key = vmAddFn(
              VmFn{VmFn::Kind::PyCall, nullptr, nullptr, scope, -1,
                   im->pythonModule, item.name});
          aval v = vmMakeCallable(key, scope, item.name.c_str());
          vmScopeDeclare(scope, item.alias, v);
        }
        continue;
      }
      if (im->moduleId >= 0) {
        // File module: resolve each item in the source module's scope
        // (re-exports are already bound there by its own import pass).
        ScopePtr src = g_vm.moduleScopes[im->moduleId];
        for (const auto& item : im->items) {
          aval v = A_NONE;
          bool found = vmScopeGet(src, item.name, v);
          if (!found) {
            // Fall back to a runtime builtin of the same name (nativeModules
            // style member access through a file module).
            int bid = builtinRuntimeId(item.name);
            if (bid >= 0) {
              v = a_builtin((uint64_t)bid);
              found = true;
            }
          }
          if (!found) {
            std::fprintf(stderr,
                         "error: module '%s' has no member '%s'\n",
                         mod.name.c_str(), item.name.c_str());
            vmUnsupported("unresolved from-import '" + item.name + "'");
          }
          vmScopeDeclare(scope, item.alias, v);
        }
      } else if (im->nativeModule) {
        for (const auto& item : im->items) {
          if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
            int bid = builtinRuntimeId(*bn);
            if (bid < 0) continue;
            vmScopeDeclare(scope, item.alias, a_builtin((uint64_t)bid));
          } else if (const std::string* cn = nativeConstant(im->bareName, item.name)) {
            double v = *cn == "PI"   ? 3.14159265358979323846
                      : *cn == "E"    ? 2.71828182845904523536
                                      : 0.0;
            vmScopeDeclare(scope, item.alias, a_double_canon(v));
          } else {
            int bid = builtinRuntimeId(item.name);
            if (bid >= 0)
              vmScopeDeclare(scope, item.alias, a_builtin((uint64_t)bid));
          }
        }
      }
    }
  }
}

void Vm::execModule(int moduleId) {
  const Module& mod = graph.at(moduleId);
  if (!mod.program) return;
  ScopePtr ms = g_vm.moduleScopes[moduleId];
  int saved = g_vm.curModule;
  g_vm.curModule = moduleId;
  bindModuleImports(mod, ms);
  // Top-level statements execute directly in the module scope so `let`
  // bindings, FnDecl closures and import aliases are visible to member
  // lookup and to functions defined in this module.
  execStmts(mod.program->statements, ms);
  g_vm.curModule = saved;
}

// ---------------------------------------------------------------------------
// Test harness: fork-per-test on POSIX (mirrors a_test_main's isolation so a
// panicking test does not kill the suite), serial in-process elsewhere.
// ---------------------------------------------------------------------------
struct VmTest {
  std::string name;
  uint64_t fnKey = 0;
  ScopePtr capture;
};

int vmRunTests(std::vector<VmTest>& tests) {
  int passed = 0, failed = 0;
#ifdef _WIN32
  for (VmTest& t : tests) {
    std::vector<aval> noargs;
    try {
      vmCall(t.fnKey, t.capture, noargs);
      std::printf("ok %s\n", t.name.c_str());
      ++passed;
    } catch (VmUnsupported& u) {
      std::printf("FAILED %s: %s\n", t.name.c_str(), u.msg.c_str());
      ++failed;
    }
  }
#else
  for (VmTest& t : tests) {
    int pfd[2];
    if (pipe(pfd) != 0) {
      // Fall back to in-process run.
      std::vector<aval> noargs;
      try {
        vmCall(t.fnKey, t.capture, noargs);
        std::printf("ok %s\n", t.name.c_str());
        ++passed;
      } catch (VmUnsupported& u) {
        std::printf("FAILED %s: %s\n", t.name.c_str(), u.msg.c_str());
        ++failed;
      }
      continue;
    }
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      close(pfd[0]);
      dup2(pfd[1], 2);
      close(pfd[1]);
      int dn = open("/dev/null", 1);
      if (dn >= 0) {
        dup2(dn, 1);
        close(dn);
      }
      std::vector<aval> noargs;
      vmCall(t.fnKey, t.capture, noargs);
      fflush(stdout);
      fflush(stderr);
      _exit(0);
    }
    close(pfd[1]);
    if (pid < 0) {
      close(pfd[0]);
      continue;
    }
    // Drain the child's stderr.
    std::string buf;
    buf.reserve(4096);
    while (true) {
      char tmp[4096];
      ssize_t n = read(pfd[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, (size_t)n);
        if (buf.size() > 65536) break;
      } else if (n == 0) {
        break;
      } else {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          int st;
          pid_t w = waitpid(pid, &st, WNOHANG);
          if (w == pid) {
            // Child exited; drain once more then break.
            n = read(pfd[0], tmp, sizeof(tmp));
            if (n > 0) buf.append(tmp, (size_t)n);
            break;
          }
          struct timespec ts{0, 1000000};
          nanosleep(&ts, nullptr);
          continue;
        }
        break;
      }
    }
    int status = 0;
    pid_t w;
    do {
      w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    close(pfd[0]);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) {
      std::printf("ok %s\n", t.name.c_str());
      ++passed;
    } else {
      // Mirror test_report: keep the message to the first meaningful line,
      // dropping "error: " prefixes, tracebacks and source locations.
      std::string m = buf;
      std::size_t cut = m.find("\nTraceback");
      if (cut != std::string::npos) m.resize(cut);
      cut = m.find("\n  --> ");
      if (cut != std::string::npos) m.resize(cut);
      while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
      if (m.rfind("error: ", 0) == 0) m.erase(0, 7);
      if (m.empty()) m = "test failed";
      std::printf("FAILED %s: %s\n", t.name.c_str(), m.c_str());
      ++failed;
    }
  }
#endif
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

}  // namespace

int runProgram(const ModuleGraph& graph, const SemaResult& sema,
               bool testMode, const std::string& testFilter) {
  a_runtime_init();
  a_gc_add_root_cb(vmGcMark);

  // Fresh state per run (the process executes one program per invocation,
  // but keep runProgram re-entrant for tooling/tests).
  g_vm = VmState{};
  g_vm.graph = &graph;
  g_vm.sema = &sema;

  Vm vm(graph, sema);
  g_vmExec = &vm;

  try {
    // Module scopes for every module in the graph.
    for (const Module& mod : graph.modules) {
      g_vm.moduleScopes[mod.id] = vmNewScope(nullptr);
    }

    // Execute modules in dependency order (imports before importers).
    std::vector<int> order = graph.order;
    if (order.empty() && graph.entry >= 0) order.push_back(graph.entry);
    for (int mid : order) {
      if (mid < 0 || mid >= (int)graph.modules.size()) continue;
      const Module& mod = graph.at(mid);
      if (!mod.program) continue;
      // Mirror codegen: skip the auto-loaded prelude for non-entry programs.
      if (mod.id != graph.entry && mod.name == "prelude") continue;
      vm.execModule(mid);
    }

    const Module* entryMod =
        (graph.entry >= 0 && graph.entry < (int)graph.modules.size())
            ? &graph.at(graph.entry)
            : nullptr;

    if (!testMode) {
      // Find the `entry` function and call it.
      const FnDecl* entryFn = nullptr;
      if (entryMod && entryMod->program) {
        for (const StmtPtr& s : entryMod->program->statements) {
          if (s->kind == StmtKind::FnDecl) {
            const FnDecl* f = static_cast<const FnDecl*>(s.get());
            if (f->isEntry) {
              entryFn = f;
              break;
            }
          }
        }
      }
      if (!entryFn) {
        // No explicit entry: top-level statements already ran; nothing else
        // to do (mirrors a script without `entry`).
        g_vmExec = nullptr;
        return 0;
      }
      auto it = g_vm.declFnKey.find(entryFn);
      if (it == g_vm.declFnKey.end()) {
        std::fprintf(stderr, "error: entry function was not executed\n");
        g_vmExec = nullptr;
        return 1;
      }
      uint64_t key = it->second;
      ScopePtr capture = g_vm.fns[key].capture;
      std::vector<aval> noargs;
      vmCall(key, capture, noargs);
      g_vmExec = nullptr;
      return 0;
    }

    // Test mode: collect top-level `fn test_*()` in the entry module.
    std::vector<VmTest> tests;
    if (entryMod && entryMod->program) {
      for (const StmtPtr& s : entryMod->program->statements) {
        if (s->kind != StmtKind::FnDecl) continue;
        const FnDecl* f = static_cast<const FnDecl*>(s.get());
        if (f->name.rfind("test_", 0) != 0) continue;
        if (!f->params.empty() || !f->comptimeParams.empty()) continue;
        if (!testFilter.empty() &&
            f->name.find(testFilter) == std::string::npos)
          continue;
        auto it = g_vm.declFnKey.find(f);
        if (it == g_vm.declFnKey.end()) continue;
        tests.push_back(VmTest{f->name, it->second, g_vm.fns[it->second].capture});
      }
    }
    // Deterministic order (mirrors the emitter's sort by name).
    std::sort(tests.begin(), tests.end(),
              [](const VmTest& a, const VmTest& b) { return a.name < b.name; });
    if (tests.empty()) {
      if (!testFilter.empty()) {
        std::fprintf(stderr, "a test: no tests matched --filter '%s'\n",
                     testFilter.c_str());
        g_vmExec = nullptr;
        return 1;
      }
      std::printf("\n0 passed, 0 failed\n");
      g_vmExec = nullptr;
      return 0;
    }
    int rc = vmRunTests(tests);
    g_vmExec = nullptr;
    return rc;
  } catch (VmUnsupported& u) {
    std::fprintf(stderr, "error: %s\n", u.msg.c_str());
    g_vmExec = nullptr;
    return 1;
  } catch (VmReturn&) {
    // `return` at module top level: treat as end of program.
    g_vmExec = nullptr;
    return 0;
  } catch (VmPropagate& p) {
    // `expr?` at top level: the propagated value is discarded (the codegen
    // backend leaves it as the expression value; ExprStmt drops it anyway).
    (void)p;
    g_vmExec = nullptr;
    return 0;
  } catch (VmBreak&) {
    std::fprintf(stderr, "error: 'break' outside of a loop\n");
    g_vmExec = nullptr;
    return 1;
  } catch (VmContinue&) {
    std::fprintf(stderr, "error: 'continue' outside of a loop\n");
    g_vmExec = nullptr;
    return 1;
  }
}

}  // namespace a::vm
