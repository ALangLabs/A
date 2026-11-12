// Bytecode interpreter core for the slot-based VM (see bytecode.hpp).

#include "vm/bytecode.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "ast/ast.hpp"
#include "ast/builtins.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"

extern "C" {
#include "a_runtime.h"
aval fnon_new_raw(uint64_t entry, aval env, const char *name, uint32_t flags,
                  uint64_t nparams);
void a_gc_mark_aval(aval v);
void a_gc_add_root_cb(void (*fn)(void));
void a_gc_push_root(aval v);
void a_gc_pop_root(void);
}

namespace a::vm::bc {

extern State g_bc;  // defined in bc_compile.cpp

namespace {

constexpr aval kMissing = (aval)A_TAG;

struct LiveFrame {
  const Code* code;
  std::uint32_t base;      // frame slots offset in g_arena
  std::uint32_t argsBase;  // copied call args region
  std::uint32_t argsN;
};

std::vector<aval> g_arena;
std::vector<aval> g_argbuf;  // staging for call args (may point into arena)
std::vector<LiveFrame> g_live;

aval bcInvoke(std::uint32_t codeIdx, aval env, std::uint32_t nargs, const aval* args);
aval bcCallValue(aval fn, std::uint32_t nargs, const aval* args);
aval bcMakeCallable(std::uint32_t codeIdx, int moduleId, const char* name,
                    const aval* parentSlots);
aval bcCallMethod(aval obj, const std::string& field, std::uint32_t nargs,
                  std::uint32_t nkw, aval* argv);
aval bcGetField(aval obj, const std::string& field);
aval bcTrampoline(aval* argv);

struct FramePop {
  std::size_t arenaSize;
  bool hasFrame;
  FramePop(std::size_t s, bool f) : arenaSize(s), hasFrame(f) {}
  ~FramePop() {
    g_arena.resize(arenaSize);
    g_live.pop_back();
    if (hasFrame) a_frame_pop();
  }
};

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------
// Defined below; trampoline entry for a_call.
aval bcTrampolineFwd(aval* argv) { return bcTrampoline(argv); }

aval bcMakeCallable(std::uint32_t codeIdx, int moduleId, const char* name,
                    const aval* parentSlots) {
  Code& c = g_bc.codes[codeIdx];
  aval env = a_closure_env_new(2 + c.upvals.size());
  a_env_set(env, 0, a_mkint((int64_t)codeIdx));
  a_env_set(env, 1, a_mkint((int64_t)moduleId));
  for (std::size_t i = 0; i < c.upvals.size(); ++i)
    a_env_set(env, 2 + i, parentSlots[c.upvals[i]]);
  uint64_t nparams = (uint64_t)c.paramNames.size();
  aval v = fnon_new_raw((uint64_t)(uintptr_t)&bcTrampolineFwd, env, name, 0,
                        nparams);
  g_bc.fnInfo[v] = codeIdx;
  g_bc.fnEnv[v] = env;
  return v;
}

// Run a defer list slot LIFO and reset it to None. May call back into the
// VM (arena can move — callers refresh their slot pointer afterwards).
void runDeferSlot(aval* sl, int slot) {
  aval l = sl[slot];
  sl[slot] = A_NONE;
  if (l == A_NONE) return;
  int64_t n = a_int_val(a_len(l));
  for (int64_t i = n - 1; i >= 0; --i) {
    aval fn = a_getitem(l, a_mkint(i));
    a_call(fn, 0, nullptr);
  }
}

aval bcInvoke(std::uint32_t codeIdx, aval env, std::uint32_t nargs, const aval* args) {
  Code& c = g_bc.codes[codeIdx];
  // `args` may point into g_arena, which resize() can relocate — stage
  // through g_argbuf first (no nested calls happen before the copy below).
  if (nargs) {
    g_argbuf.resize(nargs);
    std::memcpy(g_argbuf.data(), args, nargs * sizeof(aval));
  }
  std::size_t argBase = g_arena.size();
  g_arena.resize(argBase + nargs);
  if (nargs)
    std::memcpy(g_arena.data() + argBase, g_argbuf.data(),
                nargs * sizeof(aval));
  std::size_t base = g_arena.size();
  g_arena.resize(base + (std::size_t)c.frameSize);
  for (int i = 0; i < c.frameSize; ++i) g_arena[base + i] = A_NONE;
  g_live.push_back({&c, (std::uint32_t)base, (std::uint32_t)argBase,
                    nargs});
  bool hasFrame = !c.isMain;
  if (hasFrame) a_frame_push(c.name.c_str(), "", c.line);
  FramePop guard(argBase, hasFrame);

  // Bind params: comptime+value params in order; last may be variadic.
  int P = (int)c.paramNames.size();
  int fixed = c.variadic ? P - 1 : P;
  for (int i = 0; i < fixed; ++i) {
    aval v = A_NONE;
    if ((std::uint32_t)i < nargs) {
      v = g_arena[argBase + i];
      if (v == kMissing) v = A_NONE;
    }
    if (v == A_NONE && i < (int)c.paramDefaults.size() &&
        c.paramDefaults[i] >= 0) {
      v = c.consts[c.paramDefaults[i]];
    }
    g_arena[base + i] = v;
  }
  if (c.variadic && P > 0) {
    std::uint32_t restN = nargs > (std::uint32_t)fixed ? nargs - fixed : 0;
    aval list = a_list_new(restN, restN ? &g_arena[argBase + fixed] : nullptr);
    g_arena[base + P - 1] = list;
  }

  const Instr* ins = c.ins.data();
  std::size_t nins = c.ins.size();
  std::size_t ip = 0;
  aval ret = A_NONE;
  bool done = false;
  aval* sl = g_arena.data() + base;
  try {
  while (!done && ip < nins) {
    const Instr in = ins[ip++];
    switch (in.op) {
      case Op::LoadConst: sl[in.a] = c.consts[in.b]; break;
      case Op::LoadNone: sl[in.a] = A_NONE; break;
      case Op::LoadTrue: sl[in.a] = A_TRUE; break;
      case Op::LoadFalse: sl[in.a] = A_FALSE; break;
      case Op::LoadGlobal:
        sl[in.a] = g_bc.modules[c.moduleId].vars[in.b];
        break;
      case Op::StoreGlobal:
        g_bc.modules[c.moduleId].vars[in.a] = sl[in.b];
        break;
      case Op::LoadBuiltin: sl[in.a] = a_builtin((uint64_t)in.b); break;
      case Op::Move: sl[in.a] = sl[in.b]; break;

      case Op::Add: sl[in.a] = a_add(sl[in.b], sl[in.c]); break;
      case Op::Sub: sl[in.a] = a_sub(sl[in.b], sl[in.c]); break;
      case Op::Mul: sl[in.a] = a_mul(sl[in.b], sl[in.c]); break;
      case Op::Div: sl[in.a] = a_div(sl[in.b], sl[in.c]); break;
      case Op::FloorDiv: sl[in.a] = a_floordiv_slow(sl[in.b], sl[in.c]); break;
      case Op::Mod: sl[in.a] = a_mod(sl[in.b], sl[in.c]); break;
      case Op::Pow: sl[in.a] = a_pow(sl[in.b], sl[in.c]); break;
      case Op::Eq: sl[in.a] = a_eq(sl[in.b], sl[in.c]); break;
      case Op::Ne: sl[in.a] = a_ne(sl[in.b], sl[in.c]); break;
      case Op::Lt: sl[in.a] = a_lt(sl[in.b], sl[in.c]); break;
      case Op::Le: sl[in.a] = a_le(sl[in.b], sl[in.c]); break;
      case Op::Gt: sl[in.a] = a_gt(sl[in.b], sl[in.c]); break;
      case Op::Ge: sl[in.a] = a_ge(sl[in.b], sl[in.c]); break;
      case Op::BitAnd: sl[in.a] = a_band(sl[in.b], sl[in.c]); break;
      case Op::BitOr: sl[in.a] = a_bor(sl[in.b], sl[in.c]); break;
      case Op::BitXor: sl[in.a] = a_bxor(sl[in.b], sl[in.c]); break;
      case Op::Shl: sl[in.a] = a_shl(sl[in.b], sl[in.c]); break;
      case Op::Shr: sl[in.a] = a_shr(sl[in.b], sl[in.c]); break;
      case Op::In: sl[in.a] = a_contains(sl[in.c], sl[in.b]); break;
      case Op::Neg: sl[in.a] = a_neg(sl[in.b]); break;
      case Op::Not: sl[in.a] = a_not(sl[in.b]); break;
      case Op::BNot: sl[in.a] = a_bnot(sl[in.b]); break;
      case Op::Unwrap: sl[in.a] = a_unwrap(sl[in.b]); break;

      case Op::Jump: ip = (std::size_t)in.a; break;
      case Op::JumpIfFalse:
        if (a_truthy_raw(sl[in.a]) == 0) ip = (std::size_t)in.b;
        break;
      case Op::JumpIfTrue:
        if (a_truthy_raw(sl[in.a]) != 0) ip = (std::size_t)in.b;
        break;

      case Op::Call: {
        aval fn = sl[in.b];
        aval r = bcCallValue(fn, (std::uint32_t)in.d, &sl[in.c]);
        sl = g_arena.data() + base;
        sl[in.a] = r;
        break;
      }
      case Op::CallKnown: {
        aval r = bcInvoke((std::uint32_t)in.b, A_NONE, (std::uint32_t)in.d,
                          &sl[in.c]);
        sl = g_arena.data() + base;
        sl[in.a] = r;
        break;
      }
      case Op::CallBuiltin: {
        aval r = a_bi_call((std::uint64_t)in.b, (std::uint64_t)in.d,
                           &sl[in.c]);
        sl = g_arena.data() + base;
        sl[in.a] = r;
        break;
      }
      case Op::MethodCall: {
        aval obj = sl[in.b];
        const std::string& nm = c.names[in.c];
        std::uint32_t np = (std::uint32_t)in.d, nkw = (std::uint32_t)in.e;
        std::vector<aval> argv;
        argv.reserve(np + nkw * 2);
        for (std::uint32_t i = 0; i < np; ++i) argv.push_back(sl[in.b + 1 + i]);
        for (std::uint32_t i = 0; i < nkw; ++i) {
          argv.push_back(sl[in.b + 1 + np + i * 2]);
          argv.push_back(sl[in.b + 1 + np + i * 2 + 1]);
        }
        for (aval a : argv) a_gc_push_root(a);
        aval r = bcCallMethod(obj, nm, np, nkw, argv.empty() ? nullptr : argv.data());
        for (std::size_t i = 0; i < argv.size(); ++i) a_gc_pop_root();
        sl = g_arena.data() + base;
        sl[in.a] = r;
        break;
      }

      case Op::MakeCell: {
        aval cell = a_closure_env_new(1);
        a_env_set(cell, 0, sl[in.a]);
        sl[in.a] = cell;
        break;
      }
      case Op::CellGet: sl[in.a] = a_env_get(sl[in.b], 0); break;
      case Op::CellSet: a_env_set(sl[in.a], 0, sl[in.b]); break;
      case Op::GetUpval: {
        aval cell = a_env_get(env, (std::uint64_t)(2 + in.b));
        sl[in.a] = a_env_get(cell, 0);
        break;
      }
      case Op::TryQ: {
        aval v = a_try_result(sl[in.b]);
        if (a_is_propagate(v)) throw VmPropagate{v};
        sl[in.a] = v;
        break;
      }
      case Op::SafeNav: {
        aval obj = sl[in.b];
        aval res = A_NONE;
        if (obj != A_NONE) {
          aval v = a_getattr(a_unwrap(obj), c.names[in.c].c_str());
          if (in.d || v == A_NONE || a_is_option(v)) res = v;
          else res = a_some_new(v);
        }
        sl[in.a] = res;
        break;
      }
      case Op::SetUpval:
        a_env_set(a_env_get(env, (std::uint64_t)(2 + in.a)), 0, sl[in.b]);
        break;
      case Op::PushDefer: {
        if (sl[in.a] == A_NONE) sl[in.a] = a_list_new(0, nullptr);
        a_list_push(sl[in.a], sl[in.b]);
        break;
      }
      case Op::RunDefers: {
        runDeferSlot(sl, in.a);
        sl = g_arena.data() + base;  // defer calls may resize the arena
        break;
      }
      case Op::MatchFail:
        a_match_fail(sl[in.a]);
        break;

      case Op::Return:
        ret = in.a < 0 ? A_NONE : sl[in.a];
        done = true;
        break;

      case Op::NewList:
        sl[in.a] = a_list_new((std::uint64_t)in.c,
                              in.c ? &sl[in.b] : nullptr);
        break;
      case Op::NewTuple:
        sl[in.a] = a_tuple_new((std::uint64_t)in.c,
                               in.c ? &sl[in.b] : nullptr);
        break;
      case Op::NewSet: {
        aval st = a_set_new();
        for (int i = 0; i < in.c; ++i) a_set_add(st, sl[in.b + i]);
        sl[in.a] = st;
        break;
      }
      case Op::NewDict: {
        aval d = a_dict_new();
        for (int i = 0; i + 1 < in.c; i += 2)
          a_dict_set(d, sl[in.b + i], sl[in.b + i + 1]);
        sl[in.a] = d;
        break;
      }
      case Op::GetItem: sl[in.a] = a_getitem(sl[in.b], sl[in.c]); break;
      case Op::SetItem: a_setitem(sl[in.a], sl[in.b], sl[in.c]); break;
      case Op::GetField:
        sl[in.a] = bcGetField(sl[in.b], c.names[in.c]);
        break;
      case Op::SetField: {
        aval obj = sl[in.a];
        auto mi = g_bc.moduleOf.find(obj);
        if (mi != g_bc.moduleOf.end()) {
          BcModule& m = g_bc.modules[mi->second];
          auto f = m.slotOf.find(c.names[in.b]);
          if (f == m.slotOf.end())
            throw VmUnsupported{"module has no member '" + c.names[in.b] +
                                "'"};
          m.vars[f->second] = sl[in.c];
          break;
        }
        if (g_bc.nativeOf.count(obj) || g_bc.pyModOf.count(obj))
          throw VmUnsupported{
              "module attribute assignment is not supported in --vm mode"};
        a_setattr(obj, c.names[in.b].c_str(), sl[in.c]);
        break;
      }
      case Op::Slice:
        sl[in.a] = a_slice(sl[in.b], sl[in.b + 1], sl[in.b + 2], sl[in.b + 3]);
        break;
      case Op::StrJoin:
        sl[in.a] = a_str_join((std::uint64_t)in.c, &sl[in.b]);
        break;
      case Op::MakeRange:
        sl[in.a] = a_range_new(sl[in.b], sl[in.c], sl[in.d]);
        break;

      case Op::IterInit: sl[in.a] = a_iter_new(sl[in.b]); break;
      case Op::ListAppend: a_list_push(sl[in.a], sl[in.b]); break;
      case Op::SetAdd: a_set_add(sl[in.a], sl[in.b]); break;
      case Op::IterNext: {
        aval item;
        if (a_iter_next(sl[in.b], &item)) {
          sl[in.a] = item;
        } else {
          ip = (std::size_t)in.c;
        }
        break;
      }

      case Op::Closure: {
        aval fv = bcMakeCallable((std::uint32_t)in.b, c.moduleId,
                                 g_bc.codes[in.b].name.c_str(), sl);
        if (in.c == 1) g_bc.modules[c.moduleId].vars[in.a] = fv;
        else sl[in.a] = fv;
        break;
      }
      case Op::BindModule: {
        std::string msg = "<module '" + c.names[in.b] + "'>";
        aval v = a_str_new(msg.data(), msg.size());
        if (in.c >= 0) g_bc.moduleOf[v] = in.c;
        else if (in.d >= 0) g_bc.nativeOf[v] = c.names[in.d];
        g_bc.modules[c.moduleId].vars[in.a] = v;
        break;
      }
      case Op::ImportMember: {
        BcModule& src = g_bc.modules[in.b];
        auto f = src.slotOf.find(c.names[in.c]);
        aval v = A_NONE;
        if (f != src.slotOf.end()) v = src.vars[f->second];
        else {
          int bid = builtinRuntimeId(c.names[in.c]);
          if (bid >= 0) v = a_builtin((std::uint64_t)bid);
          else {
            throw VmUnsupported{"unresolved from-import '" +
                                c.names[in.c] + "'"};
          }
        }
        g_bc.modules[c.moduleId].vars[in.a] = v;
        break;
      }
    }
  }
  } catch (VmPropagate& p) {
    // `expr?` early-returns the propagated value from this function.
    ret = p.v;
  } catch (...) {
    // Unwind pending defers (deepest scope first) before propagating.
    if (!c.deferSlots.empty()) {
      auto ds = c.deferSlots;
      std::sort(ds.begin(), ds.end(),
                [](const auto& x, const auto& y) { return x.second > y.second; });
      for (const auto& [slot, depth] : ds) {
        sl = g_arena.data() + base;
        runDeferSlot(sl, slot);
      }
    }
    throw;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Field access / method calls on module markers (mirrors Vm::eval Field and
// evalCall's field-callee path).
// ---------------------------------------------------------------------------
aval bcGetField(aval obj, const std::string& field) {
  auto mi = g_bc.moduleOf.find(obj);
  if (mi != g_bc.moduleOf.end()) {
    BcModule& m = g_bc.modules[mi->second];
    auto f = m.slotOf.find(field);
    if (f != m.slotOf.end()) return m.vars[f->second];
    int bid = builtinRuntimeId(field);
    if (bid >= 0) return a_builtin((std::uint64_t)bid);
    throw VmUnsupported{"unknown module member '" + field + "'"};
  }
  auto ni = g_bc.nativeOf.find(obj);
  if (ni != g_bc.nativeOf.end()) {
    const std::string& mod = ni->second;
    if (const std::string* cn = nativeConstant(mod, field)) {
      double v = *cn == "PI"   ? 3.14159265358979323846
                : *cn == "E"    ? 2.71828182845904523536
                                : 0.0;
      return a_double_canon(v);
    }
    if (const std::string* fn = nativeFunction(mod, field)) {
      int bid = builtinRuntimeId(*fn);
      if (bid >= 0) return a_builtin((std::uint64_t)bid);
    }
    int bid = builtinRuntimeId(field);
    if (bid >= 0) return a_builtin((std::uint64_t)bid);
    throw VmUnsupported{"unknown native module member '" + field + "'"};
  }
  auto pi = g_bc.pyModOf.find(obj);
  if (pi != g_bc.pyModOf.end()) {
    throw VmUnsupported{"python module member access unsupported"};
  }
  return a_getattr(obj, field.c_str());
}

aval bcMakeCallable(std::uint32_t codeIdx, int moduleId, const char* name,
                    const aval* parentSlots);
aval bcCallMethod(aval obj, const std::string& field, std::uint32_t nargs,
                  std::uint32_t nkw, aval* argv) {
  auto mi = g_bc.moduleOf.find(obj);
  if (mi != g_bc.moduleOf.end()) {
    BcModule& m = g_bc.modules[mi->second];
    auto f = m.slotOf.find(field);
    aval v = A_NONE;
    if (f != m.slotOf.end()) v = m.vars[f->second];
    else {
      int bid = builtinRuntimeId(field);
      if (bid >= 0) return a_bi_call((std::uint64_t)bid, (std::uint64_t)nargs,
                                     argv);
      throw VmUnsupported{"unknown module member '" + field + "'"};
    }
    return bcCallValue(v, nargs, argv);
  }
  auto ni = g_bc.nativeOf.find(obj);
  if (ni != g_bc.nativeOf.end()) {
    const std::string& mod = ni->second;
    int bid = -1;
    if (const std::string* fn = nativeFunction(mod, field))
      bid = builtinRuntimeId(*fn);
    if (bid < 0) bid = builtinRuntimeId(field);
    if (bid < 0)
      throw VmUnsupported{"native module function not supported: " + mod +
                           "." + field};
    return a_bi_call((std::uint64_t)bid, (std::uint64_t)nargs, argv);
  }
  if (g_bc.pyModOf.count(obj))
    throw VmUnsupported{"python module call unsupported"};
  return a_method(obj, field.c_str(), (std::uint64_t)nargs,
                  (std::uint64_t)nkw, argv);
}

aval bcCallValue(aval fn, std::uint32_t nargs, const aval* args) {
  auto it = g_bc.fnInfo.find(fn);
  if (it != g_bc.fnInfo.end()) {
    auto ei = g_bc.fnEnv.find(fn);
    return bcInvoke(it->second, ei == g_bc.fnEnv.end() ? A_NONE : ei->second,
                    nargs, args);
  }
  return a_call(fn, (std::uint64_t)nargs, const_cast<aval*>(args));
}

// Trampoline for a_call (native -> bc callbacks like map/filter key=).
aval bcTrampoline(aval* argv) {
  aval env = argv[0];
  std::uint32_t codeIdx =
      (std::uint32_t)a_int_val(a_env_get(env, 0));
  Code& c = g_bc.codes[codeIdx];
  std::uint32_t n = (std::uint32_t)c.paramNames.size();
  return bcInvoke(codeIdx, env, n, argv + 1);
}

// ---------------------------------------------------------------------------
// GC roots
// ---------------------------------------------------------------------------
void gcMark() {
  for (const LiveFrame& f : g_live) {
    const Code& c = *f.code;
    for (int i = 0; i < c.frameSize; ++i)
      a_gc_mark_aval(g_arena[f.base + i]);
    for (std::uint32_t i = 0; i < f.argsN; ++i)
      a_gc_mark_aval(g_arena[f.argsBase + i]);
  }
  for (const BcModule& m : g_bc.modules)
    for (aval v : m.vars) a_gc_mark_aval(v);
  for (const Code& c : g_bc.codes)
    for (aval v : c.consts) a_gc_mark_aval(v);
  for (const auto& kv : g_bc.fnEnv) a_gc_mark_aval(kv.second);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
int runTests(std::vector<std::pair<std::string, std::uint32_t>>& tests) {
  int passed = 0, failed = 0;
  for (auto& [name, ci] : tests) {
    try {
      bcInvoke(ci, A_NONE, 0, nullptr);
      std::printf("ok %s\n", name.c_str());
      ++passed;
    } catch (const std::exception& e) {
      std::printf("FAILED %s: %s\n", name.c_str(), e.what());
      ++failed;
    }
  }
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

}  // namespace

int execute(const ModuleGraph& graph, bool testMode,
            const std::string& testFilter) {
  a_gc_add_root_cb(gcMark);
  try {
    std::vector<int> order = graph.order;
    if (order.empty() && graph.entry >= 0) order.push_back(graph.entry);
    for (int mid : order) {
      if (mid < 0 || mid >= (int)graph.modules.size()) continue;
      int ci = mid < (int)g_bc.moduleMain.size() ? g_bc.moduleMain[mid] : -1;
      if (ci < 0) continue;
      bcInvoke((std::uint32_t)ci, A_NONE, 0, nullptr);
    }

    const Module* entryMod =
        (graph.entry >= 0 && graph.entry < (int)graph.modules.size())
            ? &graph.at(graph.entry)
            : nullptr;

    if (!testMode) {
      const FnDecl* entryFn = nullptr;
      if (entryMod && entryMod->program) {
        for (const StmtPtr& s : entryMod->program->statements) {
          if (s->kind == StmtKind::FnDecl) {
            const FnDecl* f = static_cast<const FnDecl*>(s.get());
            if (f->isEntry) { entryFn = f; break; }
          }
        }
      }
      if (!entryFn) return 0;
      auto it = g_bc.declCode.find(entryFn);
      if (it == g_bc.declCode.end()) {
        std::fprintf(stderr, "error: entry function was not executed\n");
        return 1;
      }
      bcInvoke(it->second, A_NONE, 0, nullptr);
      return 0;
    }

    std::vector<std::pair<std::string, std::uint32_t>> tests;
    if (entryMod && entryMod->program) {
      for (const StmtPtr& s : entryMod->program->statements) {
        if (s->kind != StmtKind::FnDecl) continue;
        const FnDecl* f = static_cast<const FnDecl*>(s.get());
        if (f->name.rfind("test_", 0) != 0) continue;
        if (!f->params.empty() || !f->comptimeParams.empty()) continue;
        if (!testFilter.empty() &&
            f->name.find(testFilter) == std::string::npos)
          continue;
        auto it = g_bc.declCode.find(f);
        if (it != g_bc.declCode.end()) tests.push_back({f->name, it->second});
      }
    }
    std::sort(tests.begin(), tests.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (tests.empty()) {
      if (!testFilter.empty()) {
        std::fprintf(stderr, "a test: no tests matched --filter '%s'\n",
                     testFilter.c_str());
        return 1;
      }
      std::printf("\n0 passed, 0 failed\n");
      return 0;
    }
    return runTests(tests);
  } catch (VmUnsupported&) {
    // Capability gap discovered at run time: let the caller fall back to
    // the native backend (mirrors the tree-walker's VmUnsupported path).
    return kVmFallbackToNative;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

}  // namespace a::vm::bc
