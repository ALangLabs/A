// x86-64 assembly backend (A -> .s -> astra). Values are aval words passed
// in %rax / held in rbp-relative frame slots; runtime calls use the *_slow
// externs from a_runtime.h (the header fast paths are C-only). Constructs
// outside the covered subset fail emission (no C fallback exists).
#include "codegen/codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

namespace a {
namespace {

struct ABinding {
  BKind kind = BKind::Local;
  int slot = -1;           // Local/Param: frame slot index
  std::string sym;         // Global/FnValue/StructCtor/EnumValue: symbol name
  std::string aux;         // EnumValue: variant short name (for __name)
  int builtinId = -1;
  FnInfo* fn = nullptr;
  StructInfo* st = nullptr;
  EnumVariantInfo* en = nullptr;
  int moduleId = -1;
  FnInfo* dropFn = nullptr;  // struct `drop` method, if the value owns one
  std::string pyMember;      // PyFunc: python member name (aux = module)
  // Phase 5 scalar promotion: when set on a Local binding, `slot` holds a
  // raw int64 (not an aval). The binding is only marked after proving every
  // write produces an int64 via rawSafeExpr; any later non-raw write demotes
  // it (flag cleared, aval stored). Reads always stay correct: generic paths
  // box via emitBoxInt64, raw paths copy the int64 directly.
  bool rawInt = false;
  // Speculative raw accumulator ("optimistic nsw + deopt"): when set,
  // `slot` holds a raw int64 ONLY while `specFlag` == 0. Overflowing +,-,*
  // writes are emitted as raw ops with `jo` to a deopt that computes the
  // result through the aval slow path, stores it in `specAval`, and sets
  // specFlag = 1 — a PERMANENT, function-local demotion: every later read
  // or write of this binding checks the flag and uses the aval slot.
  // specNames_ (function-level, name-based like promoteNames_) decides
  // which bindings get this treatment; all accesses MUST go through the
  // flag check (emitBindingValue, emitRawExprImpl, emitSpecAssign).
  bool specRaw = false;
  int specFlag = -1;   // frame slot: 0 = raw int64 live, 1 = demoted to aval
  int specAval = -1;   // frame slot: aval value once demoted
};

// ---- int64 range analysis domain -----------------------------------------
// Conservative interval used to prove that '+', '-', '*' on raw int64 slots
// cannot overflow (overflow yields a *double* aval, which a raw slot cannot
// represent — see arith_int_int in runtime/value.c). `known == false` is the
// top element ("any value, possibly non-int"); a known range always describes
// int64 values only, so proofs built on it are sound when ranges only ever
// over-approximate. Bounds are __int128 so interval arithmetic never wraps;
// the type is a GCC/Clang extension, allowed locally via pragma.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
struct ARange {
  bool known = false;
  __int128 lo = 0;
  __int128 hi = 0;
};

// Scope-stack environment for the range dataflow: mirrors the lexical scopes
// emission uses (one map per emitBody/block level). Lookups walk innermost to
// outermost; assignments update the innermost binding and ignore names that
// resolve outside the function (globals/outer bindings stay unknown).
struct ARangeEnv {
  std::vector<std::unordered_map<std::string, ARange>> scopes;

  ARange find(const std::string& n) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) return f->second;
    }
    return ARange{};
  }
  // Update the innermost scope binding `n`; false when unbound here (global).
  bool assign(const std::string& n, const ARange& r) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) {
        f->second = r;
        return true;
      }
    }
    return false;
  }
  void bindTop(const std::string& n, const ARange& r) {
    if (scopes.empty()) scopes.emplace_back();
    scopes.back()[n] = r;
  }
  void setUnknown(const std::string& n) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) {
        f->second = ARange{};
        return;
      }
    }
  }
};
#pragma GCC diagnostic pop

class AsmEmitter {
 public:
  AsmEmitter(const ModuleGraph& graph, const SemaResult& sema, bool testMode = false)
      : graph_(graph), sema_(sema), testMode_(testMode) {}

  bool run(std::string& out, std::string& error) {
    error_ = &error;
    collectGlobals();
    buildGlobalScope();
    for (StructInfo* st : structList_) emitStructCtor(st);
    for (FnInfo* f : fnList_) {
      if (!failed_) emitFunction(f);
    }
    emitMain();
    // Lambda bodies are emitted after everything else so nested lambdas
    // discovered during their own emission are drained too.
    while (!lambdas_.empty()) {
      PendingLambda pl = std::move(lambdas_.back());
      lambdas_.pop_back();
      if (!failed_) emitLambdaFn(pl);
    }
    if (failed_) return false;
    out = bss_.str() + rodata_.str() + text_.str();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::string* error_ = nullptr;
  bool failed_ = false;
  int counter_ = 0;
  int labelCounter_ = 0;
  int slotCounter_ = 0;
  std::ostringstream text_;
  std::ostringstream rodata_;
  std::ostringstream bss_;

  std::unordered_map<const FnDecl*, FnInfo*> fns_;
  std::vector<FnInfo*> fnList_;
  std::unordered_map<std::string, StructInfo*> structs_;
  std::vector<StructInfo*> structList_;
  std::unordered_map<std::string, EnumVariantInfo> enums_;  // "Enum.Variant"
  std::vector<std::string> globals_;
  std::unordered_map<std::string, std::string> topLevelGlobals_;

  // Lambda support: each LambdaExpr emits closure-creation code at its site
  // and queues a PendingLambda; emitLambdaFn emits the body later.
  struct PendingLambda {
    const LambdaExpr* expr = nullptr;
    int moduleId = -1;
    std::string cname;
    std::vector<std::pair<std::string, ABinding>> captures;
  };
  std::vector<PendingLambda> lambdas_;
  int lambdaCounter_ = 0;

  std::vector<std::unordered_map<std::string, ABinding>> scopes_;
  std::vector<std::string> loopBreak_;
  std::vector<std::string> loopCont_;
  // `defer`/cleanup support: each emitBody frame collects deferred statements
  // (run LIFO) and owned struct values (drop() called on scope exit).
  struct DropRec {
    int slot = -1;        // frame slot holding the value (locals)
    FnInfo* fn = nullptr; // struct `drop` method
    std::string sym;      // global symbol holding the value (top-level); empty = use slot
  };
  struct ScopeFrame {
    std::vector<const Stmt*> defers;
    std::vector<DropRec> drops;  // owned struct values dropped on scope exit
  };
  std::vector<ScopeFrame> deferStack_;
  std::vector<int> loopDeferDepth_;
  bool inFunction_ = false;
  int curModuleId_ = -1;
  bool testMode_ = false;

  // Small-function inlining state. Generic inlining reuses the normal aval
  // emitter with a fresh function-like scope; raw-int inlining is a separate
  // straight-line path for leaf int->int functions and never sets these.
  int inlineDepth_ = 0;
  std::string inlineReturnLabel_;
  int inlineResultSlot_ = -1;
  int inlineDeferBase_ = 0;
  std::unordered_map<std::string, int> rawVars_;

  // Phase 5 scalar promotion state. promoteNames_ holds the locals of the
  // function currently being emitted whose bindings may keep raw int64 values
  // (see ABinding::rawInt). rawDepth_/rawCallCount_ bound compile-time raw
  // inlining so mutually recursive raw-inline candidates still terminate.
  std::unordered_set<std::string> promoteNames_;
  // Subset of promoteNames_ whose bindings use the speculative raw
  // accumulator representation (see ABinding::specRaw). Computed by
  // computePromoteSet; saved/restored together with promoteNames_.
  std::unordered_set<std::string> specNames_;
  int rawDepth_ = 0;
  int rawCallCount_ = 0;
  // Coverage-walk state: true while inside a loop body/condition, where a
  // boxed read would execute per iteration. Reads outside loops box at most
  // once per call, which is still a win versus boxing every write.
  bool promoteHot_ = false;
  static constexpr int kMaxRawDepth = 4;
  static constexpr int kMaxRawCalls = 256;

  // Phase 5b range analysis: nameRanges_ maps each local name to the union
  // (over every program point of the function being analyzed) of its
  // conservative int64 interval — an over-approximation valid at *any* read,
  // which is what the raw '+', '-', '*' overflow proofs consume. It is
  // computed by computeNameRanges inside computePromoteSet and saved /
  // restored together with promoteNames_ (inlined callees get their own).
  std::unordered_map<std::string, ARange> nameRanges_;
  int rangeFreeze_ = 0;    // >0 suppresses acc recording (loop fixpoint)
  int rangeBudget_ = 0;    // stmt-processing budget; exhaustion -> prove nothing
  bool rangeAbort_ = false;
  struct RangeLoopCtx {
    ARangeEnv* head = nullptr;   // loop-head env (fixpoint state)
    ARangeEnv* cont = nullptr;   // accumulated continue-exit envs
    ARangeEnv* brk = nullptr;    // accumulated break-exit envs
    int headDepth = 0;           // scopes kept for continue states
    int postDepth = 0;           // scopes kept for post-loop / break states
    bool* hasCont = nullptr;
    bool* hasBrk = nullptr;
  };
  std::vector<RangeLoopCtx> rangeLoops_;

  // Raw-int register cache (mem2reg-style store forwarding, confined to
  // raw-expression regions): while rawRegDepth_ > 0 the slot helpers below
  // track which frame slots currently have their bits in a register
  // (rawRegOf_[slot] = register code), so a store immediately followed by
  // a load of the same slot forwards the register instead of reloading.
  // Facts are bit-level ("reg == slot contents"), maintained only by the
  // helpers; every emit site that can clobber registers outside them must
  // call rawInvAll()/rawDropReg() or run under RawRegSuspend. Outside raw
  // regions (rawRegDepth_ == 0) the helpers behave exactly as before.
  int rawRegDepth_ = 0;
  std::unordered_map<int, int> rawRegOf_;
  enum RawReg : int {
    kRRax, kRRcx, kRRdx, kRRsi, kRRdi, kRR8, kRR9, kRR10, kRR11
  };
  static constexpr const char* kRawRegNames[9] = {
      "%rax", "%rcx", "%rdx", "%rsi", "%rdi", "%r8", "%r9", "%r10", "%r11"};
  bool rawRegOn() const { return rawRegDepth_ > 0; }
  int rawRegCode(const char* reg) const {
    for (int i = 0; i < 9; ++i)
      if (std::strcmp(reg, kRawRegNames[i]) == 0) return i;
    return -1;
  }
  // Register r was overwritten: forget every slot->r claim.
  void rawDropReg(int r) {
    if (!rawRegOn()) return;
    for (auto it = rawRegOf_.begin(); it != rawRegOf_.end();) {
      if (it->second == r) it = rawRegOf_.erase(it);
      else ++it;
    }
  }
  void rawInvAll() { rawRegOf_.clear(); }
  // Slot's memory now holds the bits in register r; drop the slot's stale
  // claims in other registers (its old contents are gone).
  void rawLearn(int slot, int r) {
    if (!rawRegOn()) return;
    rawRegOf_.erase(slot);
    rawRegOf_[slot] = r;
  }
  // Suspend the reg cache while emitting code it cannot model: arbitrary
  // expr/call sequences contain raw emits that clobber registers in ways
  // the helpers do not track. Depth and cache are restored on scope exit.
  struct RawRegSuspend {
    AsmEmitter* e_;
    int saved_;
    explicit RawRegSuspend(AsmEmitter* e) : e_(e), saved_(e->rawRegDepth_) {
      e_->rawRegDepth_ = 0;
      e_->rawRegOf_.clear();
    }
    ~RawRegSuspend() {
      e_->rawRegDepth_ = saved_;
      e_->rawRegOf_.clear();
    }
  };

  // Loop register residency: inside pure for-loop bodies, hot raw-int
  // bindings live in xmm2..xmm15 (caller-saved, so no prologue/epilogue
  // is needed and `return` needs no cleanup) instead of round-tripping
  // through frame slots every access. loopXmm_ maps frame slot -> xmm
  // index. xmm regs are clobbered by any call, so emit() flushes every
  // resident slot back to its frame slot and clears the map the moment
  // a `call` instruction is emitted; later accesses then correctly use
  // the (now authoritative) slots. loopXmmAdded_ tracks which slots the
  // innermost registerized loop installed so nested loops restore
  // correctly on exit.
  std::unordered_map<int, int> loopXmm_;
  std::vector<int> loopXmmAdded_;

  void loopXmmFlush() {
    if (loopXmm_.empty()) return;
    for (const auto& kv : loopXmm_) {
      emit(std::string("    movq %xmm") + std::to_string(kv.second) + ", " +
           slotRef(kv.first));
    }
  }
  // Reload resident values after a call: slots were flushed before it, so
  // movq m64->xmm restores each register without needing a scratch GPR
  // (which could hold the call's result).
  void loopXmmReload() {
    if (loopXmm_.empty()) return;
    for (const auto& kv : loopXmm_) {
      emit("    movq " + slotRef(kv.first) + ", %xmm" +
           std::to_string(kv.second));
    }
  }

  // Install every in-scope raw-int/spec binding (up to xmm15) into an xmm
  // register for the duration of a pure loop body. Returns a marker for
  // loopXmmExit. Bindings not touched by the body simply waste a register;
  // correctness never depends on use.
  int loopXmmEnter() {
    const int marker = (int)loopXmmAdded_.size();
    int next = 2;  // xmm0/xmm1 stay reserved for double scratch
    auto xmmFree = [&](int x) {
      for (const auto& kv : loopXmm_)
        if (kv.second == x) return false;
      return true;
    };
    for (auto& scope : scopes_) {
      for (auto& kv : scope) {
        ABinding& b = kv.second;
        if (b.slot < 0 || loopXmm_.count(b.slot)) continue;
        if (b.kind != BKind::Local && b.kind != BKind::Param) continue;
        if (!b.rawInt && !b.specRaw) continue;
        while (next <= 15 && !xmmFree(next)) ++next;
        if (next > 15) return marker;
        loadSlot(b.slot);
        emit("    movq %rax, %xmm" + std::to_string(next));
        loopXmm_[b.slot] = next;
        loopXmmAdded_.push_back(b.slot);
        ++next;
      }
    }
    return marker;
  }

  // Flush resident values back to their slots and drop this loop level's
  // residency (outer loops' entries survive for nested loops).
  void loopXmmExit(int marker) {
    loopXmmFlush();
    while ((int)loopXmmAdded_.size() > marker) {
      loopXmm_.erase(loopXmmAdded_.back());
      loopXmmAdded_.pop_back();
    }
  }

  struct InlineInfo {
    bool safe = false;
    bool raw = false;
  };
  struct InlineScan {
    const FnDecl* fn = nullptr;
    bool unsafe = false;
    bool nonInt = false;
    bool unsupportedRaw = false;
    bool unsupportedRawStmt = false;
    int cost = 0;
  };
  std::unordered_map<const FnDecl*, InlineInfo> inlineInfo_;
  std::unordered_map<const FnDecl*, int> inlineSites_;
  static constexpr int kMaxInlineCost = 40;
  static constexpr int kMaxInlineSites = 16;

  static constexpr uint64_t kTag = 0xFFFF000000000000ULL;
  static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
  static constexpr uint64_t kNone = kTag | 0x2ULL;
  static constexpr uint64_t kFalse = kTag | 0x4ULL;
  static constexpr uint64_t kTrue = kTag | 0x6ULL;
  // "argument not supplied" sentinel for defaulted parameters: payload 0 is
  // unused by the runtime (not a pointer, not an int, not a small constant).
  // Callers pad missing argv slots with this; callees substitute defaults.
  static constexpr uint64_t kMissing = kTag;
  // Mask matching inline ints: top 16 bits all 1s AND payload bit0 set.
  // (v & kTagInt) == kTagInt iff v is an inline int aval.
  static constexpr uint64_t kTagInt = kTag | 1ULL;

  void fail(const std::string& msg) {
    if (!failed_) {
      failed_ = true;
      if (error_) *error_ = "asm backend: " + msg;
    }
  }
  void emit(const std::string& s) {
    if (s.empty() || failed_) return;
    // Any call may clobber xmm registers: spill resident slots to their
    // frame slots before the call and reload them right after, keeping
    // residency (and correctness on the taken path) with zero cost on
    // paths that never reach the call.
    if (!loopXmm_.empty() &&
        (s.find("call ") != std::string::npos ||
         s.find("call\t") != std::string::npos)) {
      loopXmmFlush();
      text_ << s;
      if (s.back() != '\n') text_ << '\n';
      loopXmmReload();
      return;
    }
    text_ << s;
    if (s.back() != '\n') text_ << '\n';
  }
  std::string u64s(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
  }
  std::string slotRef(int i) { return "-" + std::to_string(8 + 8 * i) + "(%rbp)"; }
  int allocSlot() { return slotCounter_++; }
  int frameBytes() const { return ((slotCounter_ + 1) * 8 + 15) & ~15; }
  std::string newLabel(const std::string& pfx) {
    return "__a_" + pfx + std::to_string(labelCounter_++);
  }
  void emitLabel(const std::string& l) { emit(l + ":"); }
  void jmp(const std::string& l) { emit("    jmp " + l); }

  // ---- value materialization ----------------------------------------------
  uint64_t mkintA(int64_t v) const {
    uint64_t p = (((uint64_t)v) << 1) | 1ULL;
    return kTag | (p & kPayloadMask);
  }
  // Integer literal -> aval, mirroring mkint_full: the inline 47-bit
  // encoding only covers [-2^46, 2^46); wider values must box as T_INT64
  // (mkintA alone would truncate the payload and miscompile the sign).
  void emitIntLitA(std::int64_t v) {
    const std::int64_t lim = std::int64_t(1) << 46;
    if (v >= -lim && v < lim) {
      movImm(mkintA(v));
      return;
    }
    movImm((uint64_t)v);
    emitBoxInt64();
  }
  void movImm(uint64_t v, const char* reg = "%rax") {
    emit(std::string("    mov $") + u64s(v) + ", " + reg);
    if (rawRegOn()) {
      int c = rawRegCode(reg);
      if (c >= 0) rawDropReg(c);
    }
  }
  void storeRax(int slot) {
    auto xi = loopXmm_.find(slot);
    if (xi != loopXmm_.end()) {
      emit(std::string("    movq %rax, %xmm") + std::to_string(xi->second));
      rawLearn(slot, kRRax);  // %rax still holds the stored bits
      return;
    }
    emit("    mov %rax, " + slotRef(slot));
    rawLearn(slot, kRRax);  // %rax still holds the stored bits
  }
  void loadSlot(int slot, const char* reg = "%rax") {
    auto xi = loopXmm_.find(slot);
    if (xi != loopXmm_.end()) {
      emit(std::string("    movq %xmm") + std::to_string(xi->second) + ", " +
           reg);
      if (rawRegOn()) {
        int c = rawRegCode(reg);
        if (c >= 0) {
          rawDropReg(c);
          rawLearn(slot, c);
        }
      }
      return;
    }
    if (rawRegOn()) {
      int want = rawRegCode(reg);
      if (want >= 0) {
        auto it = rawRegOf_.find(slot);
        if (it != rawRegOf_.end() && it->second == want) return;
        if (it != rawRegOf_.end()) {
          emit(std::string("    mov ") + kRawRegNames[it->second] + ", " + reg);
        } else {
          emit(std::string("    mov ") + slotRef(slot) + ", " + reg);
        }
        rawDropReg(want);
        rawLearn(slot, want);
        return;
      }
    }
    emit(std::string("    mov ") + slotRef(slot) + ", " + reg);
  }
  void leaSlot(int slot, const char* reg) {
    emit(std::string("    lea ") + slotRef(slot) + ", " + reg);
  }
  // mem -> mem is impossible on x86; route slot copies through %rcx.
  void copySlot(int from, int to) {
    auto xf = loopXmm_.find(from), xt = loopXmm_.find(to);
    if (xf != loopXmm_.end() && xt != loopXmm_.end()) {
      emit(std::string("    movapd %xmm") + std::to_string(xf->second) +
           ", %xmm" + std::to_string(xt->second));
      return;
    }
    if (xf != loopXmm_.end()) {
      emit(std::string("    movq %xmm") + std::to_string(xf->second) +
           ", %rcx\n    mov %rcx, " + slotRef(to));
      rawLearn(to, kRRcx);
      return;
    }
    if (xt != loopXmm_.end()) {
      emit("    mov " + slotRef(from) + ", %rcx\n    movq %rcx, %xmm" +
           std::to_string(xt->second));
      rawLearn(to, kRRcx);
      return;
    }
    if (!rawRegOn()) {
      emit("    mov " + slotRef(from) + ", %rcx\n    mov %rcx, " + slotRef(to));
      return;
    }
    auto it = rawRegOf_.find(from);
    if (it != rawRegOf_.end()) {
      // `from` is already in a register: store straight from it (no %rcx
      // round-trip, and %rcx's claims survive untouched).
      int r = it->second;
      emit(std::string("    mov ") + kRawRegNames[r] + ", " + slotRef(to));
      rawLearn(to, r);
      return;
    }
    rawDropReg(kRRcx);  // %rcx is rewritten below
    emit("    mov " + slotRef(from) + ", %rcx\n    mov %rcx, " + slotRef(to));
    rawRegOf_[from] = kRRcx;  // %rcx now holds `from`'s bits as well
    rawLearn(to, kRRcx);
  }
  uint64_t dblBits(double d) const {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
  }

  // Byte string in .rodata (.byte list: safest for the astra assembler).
  // Returns the label; bytes are NOT escaped so any content works.
  std::string emitStrData(const std::string& bytes, bool nulTerm) {
    std::string l = "__a_d" + std::to_string(counter_++);
    rodata_ << "    .section .rodata\n    .align 1\n" << l << ":\n    .byte ";
    bool first = true;
    auto put = [&](int b) {
      if (!first) rodata_ << ",";
      rodata_ << b;
      first = false;
    };
    for (unsigned char c : bytes) put(c);
    if (nulTerm) put(0);
    if (first) put(0);
    rodata_ << "\n";
    return l;
  }

  // aval string value -> fresh slot holding a_str_new(ptr, len).
  int strLitSlot(const std::string& text) {
    std::string l = emitStrData(text, false);
    int s = allocSlot();
    emit("    lea " + l + "(%rip), %rdi\n");
    movImm(text.size(), "%rsi");
    emit("    call a_str_new\n    mov %rax, " + slotRef(s));
    return s;
  }

  // ---- frame argv arrays -----------------------------------------------------
  // An argv block is `count` consecutive slots [base, base+count). Because
  // slot addresses decrease with index, argv[0] (lowest address) is the
  // highest-index slot: argv[k] -> slot(base + count - 1 - k).
  struct Argv {
    int base = 0;
    int n = 0;
  };
  Argv allocArgv(int count) {
    Argv a;
    a.base = slotCounter_;
    a.n = count;
    for (int i = 0; i < count; ++i) allocSlot();
    return a;
  }
  int argvSlot(const Argv& a, int k) const { return a.base + a.n - 1 - k; }
  int argvPtr(const Argv& a) const { return argvSlot(a, 0); }
  void argvStore(const Argv& a, int k, int valSlot) {
    copySlot(valSlot, argvSlot(a, k));
  }
  void argvStoreImm(const Argv& a, int k, uint64_t v) {
    movImm(v, "%rcx");
    emit("    mov %rcx, " + slotRef(argvSlot(a, k)));
  }

  // ---- slots / bindings ------------------------------------------------------
  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  ABinding* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void bindLocal(const std::string& name, int slot) {
    ABinding b;
    b.kind = BKind::Local;
    b.slot = slot;
    scopes_.back()[name] = b;
  }

  // Evaluate expr -> fresh slot holding the aval (null expr -> A_NONE slot).
  int evalSlot(const Expr* e) {
    if (!e) {
      int s = allocSlot();
      movImm(kNone, "%rcx");
      emit("    mov %rcx, " + slotRef(s));
      return s;
    }
    emitExpr(e);
    int s = allocSlot();
    storeRax(s);
    return s;
  }
  // Raw int64 of an aval slot -> fresh slot (inline unbox; a_int_full on
  // non-int, preserving its panic behaviour).
  int int64Slot(int avalS) {
    int s = allocSlot();
    loadSlot(avalS, "%rdi");
    emitAvalToI64();
    storeRax(s);
    return s;
  }

  void storeBinding(const ABinding& b, int vs) {
    if (b.kind == BKind::Local || b.kind == BKind::Param) {
      copySlot(vs, b.slot);
    } else if (b.kind == BKind::Global || b.kind == BKind::FnValue ||
               b.kind == BKind::StructCtor || b.kind == BKind::EnumValue) {
      loadSlot(vs);
      emit("    mov %rax, " + b.sym + "(%rip)");
    } else {
      fail("unsupported assignment target");
    }
  }
  void emitBindingValue(const ABinding& b) {
    if (b.specRaw) {
      // Speculative accumulator: flag==0 -> box the raw int64 slot;
      // flag==1 (permanently demoted) -> the aval slot is canonical.
      std::string setL = newLabel("spa"), doneL = newLabel("spd");
      loadSlot(b.specFlag);
      emit("    test %rax, %rax\n    jnz " + setL + "\n");
      loadSlot(b.slot);
      emitBoxInt64();
      emit("    jmp " + doneL + "\n");
      emitLabel(setL);
      loadSlot(b.specAval);
      emitLabel(doneL);
      return;
    }
    if (b.rawInt) {
      // Promoted local: slot holds raw int64; box to an aval on demand.
      loadSlot(b.slot);
      emitBoxInt64();
      return;
    }
    if (b.kind == BKind::Local || b.kind == BKind::Param) {
      loadSlot(b.slot);
    } else if (b.kind == BKind::Global || b.kind == BKind::FnValue ||
               b.kind == BKind::StructCtor) {
      emit("    mov " + b.sym + "(%rip), %rax");
    } else if (b.kind == BKind::EnumValue) {
      if (b.en && b.en->algebraic) {
        emitVariantDict(*b.en, b.aux);
      } else {
        emit("    mov " + b.sym + "(%rip), %rax");
      }
    } else if (b.kind == BKind::Capture) {
      // Closure env: slot holds the env aval, builtinId is the slot index.
      loadSlot(b.slot, "%rdi");
      movImm((uint64_t)b.builtinId, "%rsi");
      emit("    call a_env_get");
    } else if (b.kind == BKind::Builtin) {
      movImm((uint64_t)b.builtinId, "%rdi");
      emit("    call a_builtin");
    } else if (b.kind == BKind::NativeConst) {
      // aux holds the native constant name (PI, E, ...); same lowering as
      // `math.PI` in emitField.
      double v = b.aux == "PI"   ? 3.14159265358979323846
                : b.aux == "E"    ? 2.71828182845904523536
                                    : 0.0;
      movImm(dblBits(v));
    } else {
      fail("unsupported binding kind in expression");
      movImm(kNone);
    }
  }

  static const char* binopSlow(const std::string& op) {
    static const std::unordered_map<std::string, const char*> kMap = {
        {"+", "a_add_slow"}, {"-", "a_sub_slow"}, {"*", "a_mul_slow"},
        {"/", "a_div_slow"}, {"%", "a_mod_slow"}, {"**", "a_pow"},
        {"==", "a_eq_slow"}, {"!=", "a_ne_slow"}, {"<", "a_lt_slow"},
        {"<=", "a_le_slow"}, {">", "a_gt_slow"}, {">=", "a_ge_slow"},
        {"&", "a_band_slow"}, {"|", "a_bor_slow"}, {"^", "a_bxor_slow"},
        {"<<", "a_shl_slow"}, {">>", "a_shr_slow"},
    };
    auto it = kMap.find(op);
    return it == kMap.end() ? nullptr : it->second;
  }
  // f(lSlot, rSlot) -> rax, for runtime binary helpers taking (aval,aval).
  void call2slot(const char* sym, int a, int b) {
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }
  void call3slot(const char* sym, int a, int b, int c) {
    loadSlot(c, "%rdx");
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }
  void call4slot(const char* sym, int a, int b, int c, int d) {
    loadSlot(d, "%rcx");
    loadSlot(c, "%rdx");
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }
  // f(aval) -> rax from a slot.
  void call1slot(const char* sym, int a) {
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }

  // ---- inline numeric fast paths ------------------------------------------
  // Slots keep holding avals; arithmetic/comparison first tries to unbox
  // both operands to raw int64 (inline 47-bit int or boxed T_INT64 heap
  // object, mirroring is_int_val/int_val_full) and emits native x86 ops,
  // falling back to the *_slow runtime calls whenever the fast path does
  // not apply. %rdx must hold kTagInt on entry to emitUnboxInt/emitBoxInt64.

  // %rdi = aval -> int64 in `dst`; jumps to slowL unless the value is an
  // integer. Clobbers %rax, %rcx and `dst`; preserves %rdx, %rsi.
  void emitUnboxInt(const char* dst, const std::string& slowL) {
    std::string inlL = newLabel("ubi"), doneL = newLabel("ubd");
    emit("    mov %rdi, %rax\n    mov %rdi, %rcx\n    and %rdx, %rcx\n"
         "    cmp %rdx, %rcx\n    je " + inlL + "\n");
    // Boxed T_INT64: tagged pointer (top16 == 0xFFFF, payload 16-aligned and
    // nonzero) to an object whose AObj.type == T_INT64 (11); value at +16.
    emit("    mov %rdi, %rcx\n    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n"
         "    jne " + slowL + "\n");
    emit("    mov %rdi, %rcx\n    and $0xF, %ecx\n    jnz " + slowL + "\n");
    emit("    mov %rdi, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n"
         "    jz " + slowL + "\n");
    emit("    cmpl $11, (%rcx)\n    jne " + slowL + "\n");
    emit("    mov 16(%rcx), " + std::string(dst) + "\n    jmp " + doneL + "\n");
    emitLabel(inlL);
    emit(std::string("    mov %rdi, ") + dst + "\n    shr $1, " + dst +
         "\n    shl $17, " + dst + "\n    sar $17, " + dst + "\n");
    emitLabel(doneL);
  }

  // %rdi = aval -> %rax = int64, equivalent to a_int_full but with the
  // inline-int and boxed-T_INT64 cases inlined (mirroring emitUnboxInt).
  // Anything else (doubles, strings, ...) falls back to the runtime call,
  // preserving its panic behaviour exactly.
  void emitAvalToI64() {
    std::string slowL = newLabel("a2is"), doneL = newLabel("a2id");
    movImm(kTagInt, "%rdx");
    emitUnboxInt("%rax", slowL);
    jmp(doneL);
    emitLabel(slowL);
    emit("    mov %rdi, %rax\n    call a_int_full\n");
    emitLabel(doneL);
  }

  // %rax = int64 -> %rax = aval; equivalent to mkint_full (inline ints cover
  // [-2^46, 2^46), wider values box as T_INT64 via a runtime call).
  // Loads kTagInt into %rdx itself: callers must NOT rely on %rdx surviving
  // (idiv/cqto leave the remainder there). Clobbers %rcx, %rdx; %rdi only on
  // the call path.
  void emitBoxInt64() {
    std::string callL = newLabel("boxc"), skipL = newLabel("boxs");
    movImm(kTagInt, "%rdx");
    // v in [-2^46, 2^46) iff (v >> 46) is 0 or -1, i.e. (v>>46)+1 <=u 1.
    emit("    mov %rax, %rcx\n    sar $46, %rcx\n    inc %rcx\n"
         "    cmp $1, %rcx\n    ja " + callL + "\n");
    emit("    mov %rax, %rcx\n    add %rcx, %rcx\n    or $1, %rcx\n"
         "    shl $16, %rcx\n    shr $16, %rcx\n    or %rdx, %rcx\n"
         "    mov %rcx, %rax\n    jmp " + skipL + "\n");
    emitLabel(callL);
    emit("    mov %rax, %rdi\n    call mkint_full\n");
    emitLabel(skipL);
  }

  void emitPanicStr(const std::string& msg) {
    std::string l = emitStrData(msg, true);
    emit("    lea " + l + "(%rip), %rdi\n    call a_panic_msg\n");
  }

  // ---- double (SSE2) fast paths --------------------------------------------
  // Doubles are stored inline in the aval word as raw IEEE-754 bits
  // (a_mkdouble); any bit pattern whose top 16 bits are NOT 0xFFFF is an
  // untagged double (a_is_double == !A_IS_TAGGED). Arithmetic results are
  // canonicalized with a_double_canon/mkdouble: every NaN becomes the
  // positive quiet NaN 0x7FF8000000000000, which also guarantees the result
  // can never collide with the tagged aval space (a negative-NaN pattern
  // WOULD have top16 == 0xFFFF and be misread as a tagged value).

  static constexpr uint64_t kCanonNaN = 0x7FF8000000000000ULL;

  // Emits the a_is_double test for both operand slots: each aval must be an
  // untagged double (top16 != 0xFFFF). Jumps to slowL otherwise, so mixed
  // int/double and non-numeric operands keep the exact runtime semantics.
  void emitDoublePairCheck(int ls, int rs, const std::string& slowL) {
    for (int s : {ls, rs}) {
      loadSlot(s, "%rcx");
      emit("    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n    je " + slowL +
           "\n");
    }
  }

  // Stores the low 64 bits of %xmm0 into %rax, canonicalizing a NaN result
  // to kCanonNaN (ucomisd with itself sets PF iff the value is unordered,
  // i.e. NaN — 2 instructions to detect, matching a_double_canon exactly).
  void emitXmm0ToAval(int scratch, const std::string& doneL) {
    std::string nanL = newLabel("dnan");
    emit("    ucomisd %xmm0, %xmm0\n    jp " + nanL + "\n"
         "    movsd %xmm0, " + slotRef(scratch) + "\n"
         "    mov " + slotRef(scratch) + ", %rax\n"
         "    jmp " + doneL + "\n");
    emitLabel(nanL);
    movImm(kCanonNaN);
    emit("    jmp " + doneL + "\n");
  }

  // Double*double arithmetic with inline SSE2: both operands must be
  // untagged doubles (checked via emitDoublePairCheck), then addsd/subsd/
  // mulsd/divsd compute the IEEE result. Division by zero (either sign of
  // zero) jumps to the shared divzL panic block — identical to the
  // a_div/a_div_slow check `b == 0.0`. % / ** / mixed types stay on the
  // slow path (fmod/pow semantics and mixed-type promotion live there).
  void emitDoubleArith(const std::string& op, int ls, int rs,
                       const std::string& slowL, const std::string& doneL,
                       const std::string& divzL) {
    static const std::unordered_map<std::string, const char*> kSse = {
        {"+", "addsd"}, {"-", "subsd"}, {"*", "mulsd"}, {"/", "divsd"}};
    auto it = kSse.find(op);
    if (it == kSse.end()) {
      jmp(slowL);
      return;
    }
    rawInvAll();  // direct slotRef/slot traffic below bypasses the reg cache
    emitDoublePairCheck(ls, rs, slowL);
    if (op == "/") {
      // x + x == 0 (mod 2^64) iff x is +0.0 or -0.0; C `b == 0.0` is true
      // for both zero signs, matching the runtime panic condition.
      loadSlot(rs, "%rcx");
      emit("    add %rcx, %rcx\n    jz " + divzL + "\n");
    }
    int t = allocSlot();
    emit("    movsd " + slotRef(ls) + ", %xmm0\n"
         "    movsd " + slotRef(rs) + ", %xmm1\n"
         "    " + std::string(it->second) + " %xmm1, %xmm0\n");
    emitXmm0ToAval(t, doneL);
  }

  // Double*double comparisons with inline ucomisd. ucomisd sets
  // CF/ZF/PF with PF=1 on unordered (NaN) operands; the cc sequences below
  // reproduce the C semantics used by a_lt/a_le/a_gt/a_ge/a_eq/a_ne:
  // every ordered comparison involving NaN is false, != is true, and
  // -0.0 == +0.0. Operand order is chosen per op so a single setcc (plus a
  // parity fixup for ==/!=) excludes the unordered case. Non-double
  // operands (incl. mixed int/double, which the runtime promotes) jump to
  // slowL.
  void emitDoubleCmp(const std::string& op, int ls, int rs,
                     const std::string& slowL, const std::string& doneL) {
    static const std::set<std::string> kDCmp = {
        "==", "!=", "<", "<=", ">", ">="};
    if (!kDCmp.count(op)) {
      jmp(slowL);
      return;
    }
    rawInvAll();
    emitDoublePairCheck(ls, rs, slowL);
    emit("    movsd " + slotRef(ls) + ", %xmm0\n"
         "    movsd " + slotRef(rs) + ", %xmm1\n"
         "    xor %eax, %eax\n");
    if (op == "==" || op == "!=") {
      // ucomisd %xmm1,%xmm0 compares lhs(xmm0) vs rhs(xmm1): unordered
      // sets ZF=PF=1, so == needs setnp&sete and != needs setp|setne.
      emit("    ucomisd %xmm1, %xmm0\n");
      if (op == "==") {
        emit("    setnp %cl\n    sete %al\n    and %cl, %al\n");
      } else {
        emit("    setp %cl\n    setne %al\n    or %cl, %al\n");
      }
    } else {
      // For </<= compare rhs vs lhs (`ucomisd %xmm0,%xmm1`) so that
      // "above"/"above-or-equal" on the swapped operands yields the
      // desired ordered relation while staying false on NaN.
      const bool swap = op == "<" || op == "<=";
      emit(std::string("    ucomisd ") +
           (swap ? "%xmm0, %xmm1" : "%xmm1, %xmm0") + "\n");
      emit(std::string("    ") +
           (op == "<" || op == ">" ? "seta" : "setae") + " %al\n");
    }
    // rax is 0/1; rdx = kTagInt so lea 0x3(rdx,rax,2) yields A_FALSE/A_TRUE
    // (same materialization as the int compare path).
    movImm(kTagInt, "%rdx");
    emit("    lea 0x3(%rdx,%rax,2), %rax\n    jmp " + doneL + "\n");
  }

  // Binary arith/compare with inline int64 (and double*double) fast paths.
  // ls/rs are slots holding avals; result aval is left in %rax.
  void emitArith(const std::string& op, int ls, int rs) {
    const char* sym = binopSlow(op);
    if (!sym) {
      fail("unsupported operator '" + op + "'");
      movImm(kNone);
      return;
    }
    static const std::set<std::string> kFastInt = {
        "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
        "==", "!=", "<", "<=", ">", ">="};
    if (!kFastInt.count(op)) {
      call2slot(sym, ls, rs);
      return;
    }
    const bool isCmp = op == "==" || op == "!=" || op == "<" ||
                       op == "<=" || op == ">" || op == ">=";
    const bool isArith = op == "+" || op == "-" || op == "*" || op == "/";
    std::string slowL = newLabel("osl"), doneL = newLabel("odn"),
            otherL = newLabel("oot");
    std::string divzL, modzL;
    if (op == "/") divzL = newLabel("divz");
    if (op == "%") modzL = newLabel("modz");
    movImm(kTagInt, "%rdx");
    loadSlot(ls, "%rdi");
    emitUnboxInt("%r8", otherL);
    loadSlot(rs, "%rdi");
    emitUnboxInt("%r9", otherL);
    // r8 = lhs int64, r9 = rhs int64.
    if (isCmp) {
      static const std::unordered_map<std::string, const char*> kCc = {
          {"==", "sete"}, {"!=", "setne"}, {"<", "setl"},
          {"<=", "setle"}, {">", "setg"}, {">=", "setge"}};
      // cmp %r9,%r8 computes lhs - rhs; rdx = kTagInt so
      // lea 3(rdx, rax*2) yields A_FALSE (tag|4) or A_TRUE (tag|6).
      emit("    xor %eax, %eax\n    cmp %r9, %r8\n    " +
           std::string(kCc.at(op)) + " %al\n    lea 0x3(%rdx,%rax,2), %rax\n" +
           "    jmp " + doneL + "\n");
    } else {
      if (op == "+") {
        emit("    mov %r8, %rax\n    add %r9, %rax\n    jo " + slowL + "\n");
      } else if (op == "-") {
        emit("    mov %r8, %rax\n    sub %r9, %rax\n    jo " + slowL + "\n");
      } else if (op == "*") {
        emit("    mov %r8, %rax\n    imul %r9, %rax\n    jo " + slowL + "\n");
      } else if (op == "/") {
        emit("    test %r9, %r9\n    jz " + divzL + "\n");
        // rhs == -1 would trap on INT64_MIN/-1; the slow path returns
        // mkdouble(2^63) for that case and -a otherwise.
        emit("    cmp $-1, %r9\n    je " + slowL + "\n");
        emit("    mov %r8, %rax\n    cqto\n    idiv %r9\n");
      } else if (op == "%") {
        emit("    test %r9, %r9\n    jz " + modzL + "\n");
        emit("    cmp $-1, %r9\n    je " + slowL + "\n");
        emit("    mov %r8, %rax\n    cqto\n    idiv %r9\n    mov %rdx, %rax\n");
      } else if (op == "<<" || op == ">>") {
        // shift_amount(): amount must be an int in [0,63] else panic via
        // the slow path; << is a logical shift on the uint64 pattern, >>
        // is arithmetic on int64.
        emit("    test %r9, %r9\n    js " + slowL + "\n"
             "    cmp $63, %r9\n    ja " + slowL + "\n");
        emit("    mov %r8, %rax\n    mov %r9, %rcx\n");
        emit(std::string("    ") + (op == "<<" ? "shl" : "sar") + " %cl, %rax\n");
      } else {  // & | ^
        const char* ins = op == "&" ? "and" : op == "|" ? "or" : "xor";
        emit(std::string("    mov %r8, %rax\n    ") + ins + " %r9, %rax\n");
      }
      emitBoxInt64();
      emit("    jmp " + doneL + "\n");
    }
    emitLabel(otherL);
    if (isArith) {
      emitDoubleArith(op, ls, rs, slowL, doneL, divzL);
    } else {
      emitDoubleCmp(op, ls, rs, slowL, doneL);
    }
    if (!divzL.empty()) {
      emitLabel(divzL);
      emitPanicStr("Division by zero.\n  = help: Check the divisor before dividing.");
      emit("    jmp " + doneL + "\n");
    }
    if (!modzL.empty()) {
      emitLabel(modzL);
      emitPanicStr("Remainder by zero.\n  = help: Check the divisor before dividing.");
      emit("    jmp " + doneL + "\n");
    }
    emitLabel(slowL);
    call2slot(sym, ls, rs);
    emitLabel(doneL);
  }

  // `a <cmp> b` with int operands materialized as raw int64 and compared
  // directly: comparisons cannot trap or overflow, so both sides only need
  // the rawBinFastOk operand gate (safe-op restriction does not apply).
  bool rawCmpOk(const BinaryExpr& b) {
    static const std::set<std::string> kCmp = {"==", "!=", "<", "<=", ">", ">="};
    return kCmp.count(b.op) && exprIsInt(b.lhs.get()) &&
           exprIsInt(b.rhs.get()) && rawSafeExpr(b.lhs.get()) &&
           rawSafeExpr(b.rhs.get());
  }

  // Structural mirror of tryRawCondJump for the promotion coverage scan.
  bool rawCondOk(const Expr* e) {
    if (!e) return false;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
      return u.op == "not" && rawCondOk(u.operand.get());
    }
    if (e->kind == ExprKind::Binary &&
        rawCmpOk(static_cast<const BinaryExpr&>(*e)))
      return true;
    return exprIsInt(e) && rawSafeExpr(e);
  }

  // Raw int64 condition jump: int comparisons, `not e`, and bare int exprs
  // (truthy iff != 0) branch without materializing the bool aval. Anything
  // else falls through to the generic truthiness path in condJump.
  bool tryRawCondJump(const Expr* cond, const std::string& label,
                      bool jumpIfTrue) {
    if (!cond || failed_) return false;
    if (cond->kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(*cond);
      if (u.op != "not") return false;
      return tryRawCondJump(u.operand.get(), label, !jumpIfTrue);
    }
    if (cond->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*cond);
      static const std::unordered_map<std::string, const char*> kJcc = {
          {"==", "je"}, {"!=", "jne"}, {"<", "jl"},
          {"<=", "jle"}, {">", "jg"}, {">=", "jge"}};
      auto it = kJcc.find(b.op);
      if (it != kJcc.end() && rawCmpOk(b)) {
        int ls = allocSlot(), rs = allocSlot();
        // One shared reg-cache region for both operands + the compare
        // loads, so rhs results forward straight into %rcx.
        ++rawRegDepth_;
        rawRegOf_.clear();
        emitRawExpr(b.lhs.get(), ls);
        emitRawExpr(b.rhs.get(), rs);
        loadSlot(ls);
        loadSlot(rs, "%rcx");
        --rawRegDepth_;
        rawRegOf_.clear();
        emit("    cmp %rcx, %rax\n");
        static const std::unordered_map<std::string, const char*> kInv = {
            {"je", "jne"}, {"jne", "je"}, {"jl", "jge"}, {"jle", "jg"},
            {"jg", "jle"}, {"jge", "jl"}};
        const char* cc = it->second;
        if (!jumpIfTrue) cc = kInv.at(cc);
        emit(std::string("    ") + cc + " " + label + "\n");
        return true;
      }
    }
    if (!exprIsInt(cond) || !rawSafeExpr(cond)) return false;
    int s = allocSlot();
    ++rawRegDepth_;
    rawRegOf_.clear();
    emitRawExpr(cond, s);
    loadSlot(s);  // usually forwards: %rax already holds the result
    --rawRegDepth_;
    rawRegOf_.clear();
    emit(std::string("    test %rax, %rax\n    ") +
         (jumpIfTrue ? "jnz " : "jz ") + label + "\n");
    return true;
  }

  // Jump helper: evaluate cond expr -> truthy test -> jcc.
  // Fast paths cover A_TRUE/A_FALSE/A_NONE, inline ints, and untagged
  // doubles inline; everything else (boxed ints, strings, objects) goes
  // through a_truthy_raw_slow.
  void condJump(const Expr* cond, const std::string& label, bool jumpIfTrue) {
    if (tryRawCondJump(cond, label, jumpIfTrue)) return;
    emitExpr(cond);
    std::string oneL = newLabel("tj1"), zeroL = newLabel("tj0"),
            intL = newLabel("tji"), dblL = newLabel("tjd"),
            slowL = newLabel("tjs"), doneL = newLabel("tjf");
    movImm(kTrue, "%rcx");
    emit("    cmp %rcx, %rax\n    je " + oneL + "\n");
    movImm(kFalse, "%rcx");
    emit("    cmp %rcx, %rax\n    je " + zeroL + "\n");
    movImm(kNone, "%rcx");
    emit("    cmp %rcx, %rax\n    je " + zeroL + "\n");
    movImm(kTagInt, "%rcx");
    emit("    mov %rax, %rdx\n    and %rcx, %rdx\n    cmp %rcx, %rdx\n"
         "    je " + intL + "\n");
    emit("    mov %rax, %rdx\n    shr $48, %rdx\n    cmp $0xFFFF, %edx\n"
         "    jne " + dblL + "\n    jmp " + slowL + "\n");
    emitLabel(intL);
    emit("    mov %rax, %rdx\n    shr $1, %rdx\n    shl $17, %rdx\n"
         "    sar $17, %rdx\n    test %rdx, %rdx\n    jnz " + oneL +
         "\n    jmp " + zeroL + "\n");
    emitLabel(dblL);
    // Double truthiness via integer bit tests (the tcc-fork assembler
    // rejects ucomisd/xorpd): only +0.0 (bits 0) and -0.0 (bits
    // 0x8000000000000000) are falsy; every other pattern, incl. NaN, is
    // truthy.
    emit("    test %rax, %rax\n    jz " + zeroL + "\n");
    movImm(0x8000000000000000ULL, "%rcx");
    emit("    cmp %rcx, %rax\n    je " + zeroL + "\n    jmp " + oneL + "\n");
    emitLabel(slowL);
    emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n"
         "    jnz " + oneL + "\n    jmp " + zeroL + "\n");
    emitLabel(oneL);
    emit("    mov $1, %eax\n    jmp " + doneL + "\n");
    emitLabel(zeroL);
    emit("    xor %eax, %eax\n");
    emitLabel(doneL);
    emit(std::string("    test %rax, %rax\n    ") +
         (jumpIfTrue ? "jnz " : "jz ") + label + "\n");
  }

  // Build the algebraic-enum variant dict ({__tag, __name}) into %rax.
  void emitVariantDict(const EnumVariantInfo& ev, const std::string& name) {
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

  // ---- registration (mirrors Emitter) --------------------------------------
  void collectGlobals() {
    int fnId = 0, stId = 0;
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          FnInfo* info = new FnInfo();
          info->decl = fn;
          info->name = fn->name;
          info->moduleId = mod.id;
          info->cname = "__a_f" + std::to_string(fnId++);
          info->globalVar = "__a_gf" + std::to_string(fnId);
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
          info->ctorCname = "__a_ctor" + std::to_string(stId);
          info->globalVar = "__a_gs" + std::to_string(stId++);
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
            mi->cname = "__a_m" + std::to_string(fnId++);
            mi->globalVar = "__a_gf" + std::to_string(fnId);
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
            ev.globalVar = "__a_ev_" + e->name + "_" + v.name + "_" +
                           (ev.value < 0 ? "m" : "") + std::to_string(ev.value < 0 ? -ev.value : ev.value);
            for (const FieldDecl& f : v.fields) ev.payloadFields.push_back(f.name);
            enums_[e->name + "." + v.name] = ev;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          std::string cname = "__a_top_" + std::to_string(counter_++);
          globals_.push_back(cname);
          topLevelGlobals_[std::to_string(mod.id) + ":" + let->name] = cname;
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            std::string key = std::to_string(mod.id) + ":" + nm;
            if (!topLevelGlobals_.count(key)) {
              std::string cname = "__a_top_" + std::to_string(counter_++);
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
        auto bit = structs_.find(base);
        if (bit == structs_.end() || !bit->second->decl) break;
        chain.push_back(bit->second->decl);
        base = bit->second->decl->baseClass;
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

  std::int64_t evalConstIntA(const Expr* e, std::int64_t fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::IntLit) return static_cast<const IntLitExpr*>(e)->value;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      if (u->op == "-") return -evalConstIntA(u->operand.get(), 0);
    }
    if (e->kind == ExprKind::Ident) {
      auto it = enums_.find(static_cast<const IdentExpr*>(e)->name);
      if (it != enums_.end()) return it->second.value;
    }
    return fallback;
  }

  void buildGlobalScope() {
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
      b.aux = kv.first.substr(kv.first.find('.') + 1);
      g[kv.first] = b;
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
  }

  // ---- functions / main -------------------------------------------------------
  // Bind `from <native-module> import member [as x]` items into the current
  // scope: functions become BKind::Builtin (dispatched via a_bi_call),
  // constants become BKind::NativeConst. Members missing from the
  // nativeModules() spec are skipped here; sema already reported them.
  void bindNativeFromImport(const FromImportStmt* im) {
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
  void bindModuleImports(int moduleId) {
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
          const Module& src = graph_.at(im->moduleId);
          if (src.program) {
            for (const StmtPtr& ss : src.program->statements) {
              if (ss->kind == StmtKind::FnDecl) {
                const FnDecl* fd = static_cast<const FnDecl*>(ss.get());
                for (FnInfo* fi : fnList_) {
                  if (fi->decl == fd) {
                    ABinding b;
                    b.kind = BKind::FnValue;
                    b.sym = fi->globalVar;
                    b.fn = fi;
                    scopes_.back()[fd->name] = b;
                  }
                }
              } else if (ss->kind == StmtKind::StructDecl) {
                const StructDecl* sd = static_cast<const StructDecl*>(ss.get());
                for (StructInfo* sti : structList_) {
                  if (sti->decl == sd) {
                    ABinding b;
                    b.kind = BKind::StructCtor;
                    b.sym = sti->globalVar;
                    b.st = sti;
                    scopes_.back()[sd->name] = b;
                  }
                }
              }
            }
          }
        } else if (im->nativeModule) {
          // from <native> import member: bind member -> native builtin name
          bindNativeFromImport(im);
        }
      }
    }
  }

  void emitStructCtor(StructInfo* st) {
    // Slots: 0=argv ptr, 1=dict, 2+2i=key str, 3+2i=field value.
    int need = 2 + 2 * (int)st->fieldNames.size();
    slotCounter_ = need;
    emit("    .text\n    .globl " + st->ctorCname + "\n" + st->ctorCname + ":\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frameBytes()) + ", %rsp\n");
    emit("    mov %rdi, " + slotRef(0) + "\n");
    emit("    call a_dict_new\n    mov %rax, " + slotRef(1) + "\n");
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
    loadSlot(1);
    emit("    leave\n    ret\n");
    slotCounter_ = 0;
  }

  void emitFunction(FnInfo* f) {
    // Two-pass: body first into a scratch stream so the frame size is known
    // before the prologue `sub $N, %rsp` is emitted.
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    bindModuleImports(f->moduleId);
    int argvSlot = allocSlot();
    emit("    mov %rdi, " + slotRef(argvSlot) + "\n");
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
    emitBody(f->decl->body);
    promoteNames_ = std::move(savedPromote);
    specNames_ = std::move(savedSpec);
    nameRanges_ = std::move(savedRanges);
    emit("    mov $" + u64s(kNone) + ", %rax\n    leave\n    ret\n");
    int frame = frameBytes();
    std::ostringstream emitted;
    emitted.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl " + f->cname + "\n" + f->cname + ":\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
    text_ << emitted.str();
    scopes_.clear();
    curModuleId_ = savedModule;
    inFunction_ = savedInFn;
  }

  void emitMain() {
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    allocSlot();  // slot 0 reserved (unused scratch)
    emit("    call a_runtime_init\n");
    for (StructInfo* st : structList_) {
      emit("    lea " + st->ctorCname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
      std::string lbl = emitStrData(st->name, true);
      emit("    lea " + lbl + "(%rip), %rdx\n    call a_fnon_new\n");
      emit("    mov %rax, " + st->globalVar + "(%rip)\n");
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      emit("    lea " + f->cname + "(%rip), %rdi\n    mov $" + u64s(kNone) + ", %rsi\n");
      std::string lbl = emitStrData(f->name, true);
      emit("    lea " + lbl + "(%rip), %rdx\n    call a_fnon_new\n");
      emit("    mov %rax, " + f->globalVar + "(%rip)\n");
    }
    for (auto& kv : enums_) {
      if (kv.second.algebraic) continue;
      movImm((uint64_t)kv.second.value, "%rdi");
      emit("    call a_i64n\n    mov %rax, " + kv.second.globalVar + "(%rip)\n");
    }
    if (testMode_) {
      // `a test`: emit a table of top-level test_* functions and let the
      // runtime harness (a_test_main) fork/pipe each one, matching the old
      // C-emitter protocol.
      std::vector<FnInfo*> tests;
      for (FnInfo* f : fnList_) {
        if (!f->isMethod && f->moduleId == graph_.entry &&
            f->name.rfind("test_", 0) == 0) {
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
      emit("    call a_test_main\n    leave\n    ret\n");
    } else {
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
            const Module& src = graph_.at(im->moduleId);
            if (src.program) {
              for (const StmtPtr& ss : src.program->statements) {
                if (ss->kind == StmtKind::FnDecl) {
                  const FnDecl* fd = static_cast<const FnDecl*>(ss.get());
                  for (FnInfo* fi : fnList_) {
                    if (fi->decl == fd) {
                      ABinding b;
                      b.kind = BKind::FnValue;
                      b.sym = fi->globalVar;
                      b.fn = fi;
                      scopes_.back()[fd->name] = b;
                    }
                  }
                } else if (ss->kind == StmtKind::StructDecl) {
                  const StructDecl* sd = static_cast<const StructDecl*>(ss.get());
                  for (StructInfo* sti : structList_) {
                    if (sti->decl == sd) {
                      ABinding b;
                      b.kind = BKind::StructCtor;
                      b.sym = sti->globalVar;
                      b.st = sti;
                      scopes_.back()[sd->name] = b;
                    }
                  }
                }
              }
            }
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
      emit("    xor %eax, %eax\n    leave\n    ret\n");
    }
    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl main\nmain:\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
    text_ << body.str();
    for (const std::string& g : globals_) bss_ << "    .section .bss\n    .globl " << g << "\n" << g << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (FnInfo* f : fnList_) if (!f->isMethod) bss_ << "    .section .bss\n    .globl " << f->globalVar << "\n" << f->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (StructInfo* st : structList_) bss_ << "    .section .bss\n    .globl " << st->globalVar << "\n" << st->globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    for (auto& kv : enums_) if (!kv.second.algebraic) bss_ << "    .section .bss\n    .globl " << kv.second.globalVar << "\n" << kv.second.globalVar << ":\n    .byte 0,0,0,0,0,0,0,0\n";
    scopes_.clear();
  }

  // ---- statements -------------------------------------------------------------
  // Emit deferred statements of frames [depth, size) innermost-first, LIFO
  // within each frame. Frames are moved out so a `defer` inside a defer body
  // registers on the parent frame instead of invalidating iteration.
  // If `type` is a struct with a `drop` method, return its FnInfo.
  FnInfo* dropFnFor(TypePtr type) {
    TypePtr t = resolveType(type);
    if (!t || t->kind != TypeKind::Struct) return nullptr;
    auto sit = structs_.find(shortName(t->structName));
    if (sit == structs_.end()) return nullptr;
    auto mit = sit->second->methods.find("drop");
    return mit == sit->second->methods.end() ? nullptr : mit->second;
  }

  void emitDropNow(int slot, FnInfo* fn) {
    emitDirectCall(fn->cname.c_str(), {slot});
  }
  void emitDropRec(const DropRec& d) {
    if (d.fn == nullptr) return;
    if (d.sym.empty()) {
      emitDropNow(d.slot, d.fn);
      return;
    }
    int s = allocSlot();
    emit("    mov " + d.sym + "(%rip), %rax\n    mov %rax, " + slotRef(s));
    emitDirectCall(d.fn->cname.c_str(), {s});
  }
  // Find the pending drop record for a binding across all live frames.
  DropRec* findDropRec(const ABinding& b) {
    for (auto fi = deferStack_.rbegin(); fi != deferStack_.rend(); ++fi) {
      for (auto it = fi->drops.rbegin(); it != fi->drops.rend(); ++it) {
        if (b.kind == BKind::Local || b.kind == BKind::Param) {
          if (it->sym.empty() && it->slot == b.slot) return &*it;
        } else if (it->sym == b.sym) {
          return &*it;
        }
      }
    }
    return nullptr;
  }

  void emitDeferredDownTo(int depth) {
    while ((int)deferStack_.size() > depth) {
      ScopeFrame frame = std::move(deferStack_.back());
      deferStack_.pop_back();
      for (auto it = frame.defers.rbegin(); it != frame.defers.rend(); ++it)
        emitStmt(*it, false);
      for (auto it = frame.drops.rbegin(); it != frame.drops.rend(); ++it)
        emitDropRec(*it);
    }
  }
  void emitDeferredAll() { emitDeferredDownTo(0); }

  // Return/try-propagate path: emit deferred statements of frames [depth,
  // size) innermost-first, LIFO within each frame, WITHOUT popping — later
  // code in the same function still needs its frames. Inlining uses `depth`
  // to run only the callee's frames and leave the caller's defers untouched.
  void emitDeferredPeekDownTo(int depth) {
    for (int fi = (int)deferStack_.size() - 1; fi >= depth; --fi) {
      ScopeFrame& f = deferStack_[fi];
      for (auto it = f.defers.rbegin(); it != f.defers.rend(); ++it)
        emitStmt(*it, false);
      for (auto it = f.drops.rbegin(); it != f.drops.rend(); ++it)
        emitDropRec(*it);
    }
  }
  void emitDeferredPeekAll() { emitDeferredPeekDownTo(0); }

  void emitBody(const std::vector<StmtPtr>& stmts, bool topLevel = false) {
    pushScope();
    int base = (int)deferStack_.size();
    deferStack_.emplace_back();
    for (const StmtPtr& s : stmts) {
      if (failed_) break;
      emitStmt(s.get(), topLevel);
    }
    // Emit this frame plus any frames leaked by early exits (return/break
    // already popped everything below `base`, so this is a no-op then).
    emitDeferredDownTo(base);
    popScope();
  }

  void emitStmt(const Stmt* s, bool topLevel = false) {
    if (failed_ || !s) return;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        // Scalar promotion: a promotable local whose initializer is a raw
        // int64 expression keeps the value untagged in its frame slot.
        const bool wantRaw =
            !topLevel && st.init && promoteNames_.count(st.name) &&
            rawSafeExpr(st.init.get()) && exprIsInt(st.init.get());
        int sl;
        if (wantRaw) {
          sl = allocSlot();
          emitRawExpr(st.init.get(), sl);
        } else {
          sl = evalSlot(st.init.get());
        }
        if (topLevel) {
          std::string key = std::to_string(curModuleId_) + ":" + st.name;
          auto it = topLevelGlobals_.find(key);
          if (it == topLevelGlobals_.end()) {
            fail("top-level variable '" + st.name + "' was not registered");
            return;
          }
          ABinding b;
          b.kind = BKind::Global;
          b.sym = it->second;
          storeBinding(b, sl);
          scopes_.back()[st.name] = b;
          FnInfo* dropFn = dropFnFor(st.init->inferredType);
          if (dropFn) {
            ABinding* lb = lookup(st.name);
            if (lb) lb->dropFn = dropFn;
            deferStack_.back().drops.push_back({-1, dropFn, it->second});
          }
        } else {
          bindLocal(st.name, sl);
          if (wantRaw) {
            ABinding* rb = lookup(st.name);
            if (rb) {
              rb->rawInt = true;
              if (specNames_.count(st.name)) initSpecSlots(rb);
            }
          }
          FnInfo* dropFn = dropFnFor(st.init->inferredType);
          if (dropFn) {
            ABinding* b = lookup(st.name);
            if (b) b->dropFn = dropFn;
            deferStack_.back().drops.push_back({sl, dropFn, ""});
          }
        }
        return;
      }
      case StmtKind::Assign:
        emitAssign(static_cast<const AssignStmt&>(*s));
        return;
      case StmtKind::Expr: {
        const ExprStmt& st = static_cast<const ExprStmt&>(*s);
        std::string dummy;
        if (plainStringText(st.expr, dummy)) return;
        emitExpr(st.expr.get());
        return;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        int tv = evalSlot(st.value.get());
        for (std::size_t i = 0; i < st.names.size(); ++i) {
          if (st.names[i] == "_") continue;
          int is = allocSlot();
          movImm(mkintA((int64_t)i), "%rcx");
          emit("    mov %rcx, " + slotRef(is));
          call2slot("a_getitem", tv, is);  // rax = element
          int ns = allocSlot();
          storeRax(ns);
          bindLocal(st.names[i], ns);
        }
        return;
      }
      case StmtKind::Return: {
        const ReturnStmt& st = static_cast<const ReturnStmt&>(*s);
        int rvS = allocSlot();
        if (st.value) {
          emitExpr(st.value.get());
        } else {
          movImm(kNone);
        }
        storeRax(rvS);
        if (inlineDepth_ > 0) {
          emitDeferredPeekDownTo(inlineDeferBase_);
          copySlot(rvS, inlineResultSlot_);
          jmp(inlineReturnLabel_);
          return;
        }
        emitDeferredPeekAll();
        loadSlot(rvS);
        if (inFunction_) {
          emit("    leave\n    ret");
        } else {
          emit("    xor %eax, %eax\n    leave\n    ret");
        }
        return;
      }
      case StmtKind::Break:
        if (loopBreak_.empty()) {
          fail("break outside loop");
        } else {
          emitDeferredDownTo(loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back());
          jmp(loopBreak_.back());
        }
        return;
      case StmtKind::Continue:
        if (loopCont_.empty()) {
          fail("continue outside loop");
        } else {
          emitDeferredDownTo(loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back());
          jmp(loopCont_.back());
        }
        return;
      case StmtKind::Defer:
        if (deferStack_.empty()) {
          fail("defer outside of a scope");
        } else {
          const DeferStmt& st = static_cast<const DeferStmt&>(*s);
          deferStack_.back().defers.push_back(st.body.get());
        }
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        std::string endL = newLabel("ifend");
        for (std::size_t i = 0; i < st.branches.size(); ++i) {
          std::string nextL = newLabel("ifnext");
          condJump(st.branches[i].cond.get(), nextL, false);
          emitBody(st.branches[i].body);
          jmp(endL);
          emitLabel(nextL);
        }
        if (!st.elseBody.empty()) emitBody(st.elseBody);
        emitLabel(endL);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        std::string loopL = newLabel("while");
        std::string endL = newLabel("wend");
        emitLabel(loopL);
        condJump(st.cond.get(), endL, false);
        loopBreak_.push_back(endL);
        loopCont_.push_back(loopL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        jmp(loopL);
        emitLabel(endL);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        std::string topL = newLabel("do");
        std::string contL = newLabel("docont");
        std::string endL = newLabel("dowend");
        emitLabel(topL);
        loopBreak_.push_back(endL);
        loopCont_.push_back(contL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        emitLabel(contL);
        condJump(st.cond.get(), topL, true);
        loopBreak_.pop_back();
        loopCont_.pop_back();
        emitLabel(endL);
        return;
      }
      case StmtKind::For:
        emitFor(static_cast<const ForStmt&>(*s));
        return;
      case StmtKind::Loop: {
        const LoopStmt& st = static_cast<const LoopStmt&>(*s);
        std::string topL = newLabel("loop");
        std::string endL = newLabel("loopen");
        emitLabel(topL);
        loopBreak_.push_back(endL);
        loopCont_.push_back(topL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        jmp(topL);
        emitLabel(endL);
        return;
      }
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
      default:
        fail("unsupported statement");
        return;
    }
  }

  // cur += step with INT64 saturation toward `end` (mirrors the C emitter's
  // range fast path). curS/endS/stepS hold raw int64 (not avals).
  void emitRangeIncr(int curS, int endS, int stepS) {
    std::string posL = newLabel("rincrp");
    std::string setEndL = newLabel("rincre");
    std::string nextL = newLabel("rincrn");
    emit("    mov " + slotRef(curS) + ", %rax\n    mov " + slotRef(stepS) + ", %rcx\n");
    emit("    test %rcx, %rcx\n    jns " + posL + "\n");
    // step < 0: if cur < INT64_MIN - step -> cur = end
    movImm((uint64_t)INT64_MIN, "%rdx");
    emit("    sub %rcx, %rdx\n");
    emit("    cmp %rdx, %rax\n    jl " + setEndL + "\n");
    emit("    add %rcx, %rax\n    mov %rax, " + slotRef(curS) + "\n    jmp " + nextL + "\n");
    emitLabel(setEndL);
    copySlot(endS, curS);
    jmp(nextL);
    emitLabel(posL);
    // step > 0: if cur > INT64_MAX - step -> cur = end
    movImm((uint64_t)INT64_MAX, "%rdx");
    emit("    sub %rcx, %rdx\n");
    emit("    cmp %rdx, %rax\n    jg " + setEndL + "\n");
    emit("    add %rcx, %rax\n    mov %rax, " + slotRef(curS) + "\n");
    emitLabel(nextL);
  }

  // ---- LICM / bounds-check-elimination support ------------------------------
  // Conservative purity scan shared by the array-iteration LICM (emitForArray)
  // and the counted-loop bounds-check elimination (emitFor range path). An
  // expression/statement is "pure" when evaluating it cannot mutate a heap
  // object or run unknown code: no calls (builtins included — they reach the
  // runtime), no lambdas (they capture values and may be invoked later), and
  // no non-Ident assignment targets (index/slice/field writes). Plain
  // `name = v` / `name op= v` only rebinds a name — it cannot mutate the
  // object a `for` loop snapshotted — so it stays pure; `writes` optionally
  // collects every bound name (lets, assigns, destructure/for vars, ++/--
  // targets) so callers can prove specific bindings are never reassigned.
  // Any doubt -> not pure.
  bool scanExprPure(const Expr* e, std::unordered_set<std::string>* writes) {
    if (!e) return true;
    switch (e->kind) {
      case ExprKind::Call:
      case ExprKind::Lambda:
        return false;
      case ExprKind::Ident:
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return true;
      case ExprKind::StringLit:
        for (const StringPart& p :
             static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr && !scanExprPure(p.expr.get(), writes)) return false;
        return true;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if ((u.op == "++" || u.op == "--") && u.operand &&
            u.operand->kind == ExprKind::Ident && writes)
          writes->insert(static_cast<const IdentExpr&>(*u.operand).name);
        return scanExprPure(u.operand.get(), writes);
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        return scanExprPure(b.lhs.get(), writes) &&
               scanExprPure(b.rhs.get(), writes);
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        return scanExprPure(x.object.get(), writes) &&
               scanExprPure(x.index.get(), writes);
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        return scanExprPure(x.object.get(), writes) &&
               scanExprPure(x.start.get(), writes) &&
               scanExprPure(x.end.get(), writes) &&
               scanExprPure(x.step.get(), writes);
      }
      case ExprKind::Field:
        return scanExprPure(static_cast<const FieldExpr&>(*e).object.get(),
                            writes);
      case ExprKind::SafeNav:
        return scanExprPure(static_cast<const SafeNavExpr&>(*e).object.get(),
                            writes);
      case ExprKind::ArrayLit:
      case ExprKind::Tuple:
      case ExprKind::SetLit: {
        const auto& els = e->kind == ExprKind::ArrayLit
            ? static_cast<const ArrayLitExpr&>(*e).elements
            : e->kind == ExprKind::SetLit
                ? static_cast<const SetLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els)
          if (!scanExprPure(el.get(), writes)) return false;
        return true;
      }
      case ExprKind::MapLit:
        for (const auto& kv :
             static_cast<const MapLitExpr&>(*e).entries)
          if (!scanExprPure(kv.first.get(), writes) ||
              !scanExprPure(kv.second.get(), writes))
            return false;
        return true;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        return scanExprPure(r.start.get(), writes) &&
               scanExprPure(r.end.get(), writes) &&
               scanExprPure(r.step.get(), writes);
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        return scanExprPure(t.cond.get(), writes) &&
               scanExprPure(t.thenValue.get(), writes) &&
               scanExprPure(t.elseValue.get(), writes);
      }
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        return scanExprPure(c.element.get(), writes) &&
               scanExprPure(c.iterable.get(), writes) &&
               scanExprPure(c.condition.get(), writes);
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        if (!scanExprPure(m.subject.get(), writes)) return false;
        for (const MatchArm& arm : m.arms) {
          if (!scanPatternPure(arm.pattern, writes)) return false;
          if (!scanExprPure(arm.guard.get(), writes) ||
              !scanExprPure(arm.result.get(), writes))
            return false;
        }
        return true;
      }
      case ExprKind::Try:
        return scanExprPure(static_cast<const TryExpr&>(*e).operand.get(),
                            writes);
    }
    return false;  // unknown kind -> doubt
  }

  bool scanPatternPure(const Pattern& p,
                       std::unordered_set<std::string>* writes) {
    if ((p.value && !scanExprPure(p.value.get(), writes)) ||
        (p.rangeStart && !scanExprPure(p.rangeStart.get(), writes)) ||
        (p.rangeEnd && !scanExprPure(p.rangeEnd.get(), writes)) ||
        (p.ctor && !scanExprPure(p.ctor.get(), writes)))
      return false;
    for (const Pattern& sub : p.subpatterns)
      if (!scanPatternPure(sub, writes)) return false;
    return true;
  }

  bool scanStmtsPure(const std::vector<StmtPtr>& stmts,
                     std::unordered_set<std::string>* writes) {
    for (const StmtPtr& s : stmts)
      if (!scanStmtPure(s.get(), writes)) return false;
    return true;
  }

  bool scanStmtPure(const Stmt* s,
                    std::unordered_set<std::string>* writes) {
    if (!s) return true;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        if (writes) writes->insert(st.name);
        return scanExprPure(st.init.get(), writes);
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        // Only a bare `name = v` / `name op= v` rebinds; index/slice/field
        // targets write through to a heap object -> not pure.
        if (st.target->kind != ExprKind::Ident) return false;
        if (writes)
          writes->insert(static_cast<const IdentExpr&>(*st.target).name);
        return scanExprPure(st.value.get(), writes);
      }
      case StmtKind::Expr:
        return scanExprPure(static_cast<const ExprStmt&>(*s).expr.get(),
                            writes);
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        if (writes)
          for (const std::string& nm : st.names)
            if (nm != "_") writes->insert(nm);
        return scanExprPure(st.value.get(), writes);
      }
      case StmtKind::Return:
        return scanExprPure(static_cast<const ReturnStmt&>(*s).value.get(),
                            writes);
      case StmtKind::Break:
      case StmtKind::Continue:
        return true;
      case StmtKind::Defer:
        return scanStmtPure(static_cast<const DeferStmt&>(*s).body.get(),
                            writes);
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches) {
          if (!scanExprPure(br.cond.get(), writes)) return false;
          if (!scanStmtsPure(br.body, writes)) return false;
        }
        return scanStmtsPure(st.elseBody, writes);
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        return scanExprPure(st.cond.get(), writes) &&
               scanStmtsPure(st.body, writes);
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        return scanStmtsPure(st.body, writes) &&
               scanExprPure(st.cond.get(), writes);
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        if (writes) writes->insert(st.var);
        return scanExprPure(st.iterable.get(), writes) &&
               scanStmtsPure(st.body, writes);
      }
      case StmtKind::Loop:
        return scanStmtsPure(static_cast<const LoopStmt&>(*s).body, writes);
      default:
        return true;  // decls/imports emit nothing inside a body
    }
  }

  // Bounds-check elimination context: one entry per enclosing
  // `for i in lo..len(arr)` loop (lo a non-negative literal, step 1, pure
  // body that never writes i or arr). The Index fast path skips its bounds
  // check when both the object and index bindings match — binding pointers
  // make shadowing/rebinding fall back to the checked path automatically.
  struct BceCtx {
    ABinding* varBind = nullptr;  // loop variable binding
    ABinding* arrBind = nullptr;  // array binding named in len(arr)
  };
  std::vector<BceCtx> bceLoops_;

  // Value whitelist for the fill-loop idiom: scalar, non-allocating,
  // non-aliasing expressions whose value cannot change across iterations
  // (no calls, no heap reads/writes, no object-building literals, no
  // ++/-- side effects). Anything else is re-evaluated per iteration and
  // must stay on the generic loop path.
  bool fillValueOk(const Expr* e) const {
    if (!e) return true;
    switch (e->kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
      case ExprKind::Ident:
        return true;
      case ExprKind::StringLit:
        for (const StringPart& p :
             static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) return false;
        return true;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if (u.op == "++" || u.op == "--") return false;
        return fillValueOk(u.operand.get());
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        return fillValueOk(b.lhs.get()) && fillValueOk(b.rhs.get());
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        return fillValueOk(t.cond.get()) && fillValueOk(t.thenValue.get()) &&
               fillValueOk(t.elseValue.get());
      }
      default:
        return false;
    }
  }

  // `for i in lo..hi[..step]` whose entire body is `xs[i] = v` with v a
  // loop-invariant scalar lowers to one a_list_fill_range call instead of
  // the per-element loop. curS/endS/stepS are the raw int64 range slots
  // already computed by emitFor; xs and v are evaluated once (both are
  // loop-invariant: xs is a plain ident, v passes fillValueOk and names
  // neither i nor xs). The runtime helper reproduces a_setitem's
  // normalize_index panics element-by-element.
  bool tryEmitForFill(const ForStmt& st, int curS, int endS, int stepS) {
    if (st.body.size() != 1) return false;
    const Stmt* bs = st.body[0].get();
    if (!bs || bs->kind != StmtKind::Assign) return false;
    const AssignStmt& as = static_cast<const AssignStmt&>(*bs);
    if (as.op != "=" || !as.target || !as.value ||
        as.target->kind != ExprKind::Index)
      return false;
    const IndexExpr& ix = static_cast<const IndexExpr&>(*as.target);
    if (ix.object->kind != ExprKind::Ident ||
        ix.index->kind != ExprKind::Ident)
      return false;
    const std::string& xsName =
        static_cast<const IdentExpr&>(*ix.object).name;
    if (xsName == st.var ||
        static_cast<const IdentExpr&>(*ix.index).name != st.var)
      return false;
    TypePtr xt = resolveType(ix.object->inferredType);
    if (!xt || xt->kind != TypeKind::Array) return false;
    if (!fillValueOk(as.value.get())) return false;
    std::set<std::string> ids;
    collectIdents(as.value.get(), ids);
    if (ids.count(st.var) || ids.count(xsName)) return false;
    int xsS = evalSlot(ix.object.get());
    int vS = evalSlot(as.value.get());
    loadSlot(xsS, "%rdi");
    loadSlot(curS, "%rsi");
    loadSlot(endS, "%rdx");
    loadSlot(stepS, "%rcx");
    loadSlot(vS, "%r8");
    emit("    call a_list_fill_range\n");
    return true;
  }

  void emitFor(const ForStmt& st) {
    // Fast path: `for x in lo..hi[..step]` as a native counted int64 loop.
    const Expr* loE = nullptr;
    const Expr* hiE = nullptr;
    const Expr* stE = nullptr;
    const Expr* iter = st.iterable.get();
    if (iter && iter->kind == ExprKind::Range) {
      const RangeExpr& re = static_cast<const RangeExpr&>(*iter);
      loE = re.start.get();
      hiE = re.end.get();
      stE = re.step.get();
    } else if (iter && iter->kind == ExprKind::Binary &&
               static_cast<const BinaryExpr&>(*iter).op == "..") {
      const BinaryExpr& be = static_cast<const BinaryExpr&>(*iter);
      loE = be.lhs.get();
      hiE = be.rhs.get();
    }
    if (loE && hiE) {
      int loV = evalSlot(loE);
      int hiV = evalSlot(hiE);
      int stV = stE ? evalSlot(stE) : -1;
      int curS = int64Slot(loV);
      int endS = int64Slot(hiV);
      int stepS;
      if (stV >= 0) {
        stepS = int64Slot(stV);
      } else {
        stepS = allocSlot();
        movImm(1, "%rcx");
        emit("    mov %rcx, " + slotRef(stepS));
      }
      // step == 0 -> panic (same message as the C fast path).
      {
        std::string okL = newLabel("rok");
        std::string msg = emitStrData("range() step must be positive, found 0.", true);
        loadSlot(stepS, "%rax");
        emit("    test %rax, %rax\n    jnz " + okL + "\n");
        emit("    lea " + msg + "(%rip), %rdi\n    call a_panic_msg\n");
        emitLabel(okL);
      }
      // Fill idiom: `for i in lo..hi[..step]: xs[i] = <invariant>` becomes
      // one runtime bulk-fill call (checked after the step==0 panic so
      // that behaviour is preserved).
      if (tryEmitForFill(st, curS, endS, stepS)) return;
      int varS = allocSlot();
      std::string loopL = newLabel("for");
      std::string contL = newLabel("forc");
      std::string endL = newLabel("fend");
      std::string bodyL = newLabel("forb");
      int xmmMark = loopXmmEnter();
      // Raw range-loop var: written (copySlot/storeRax) before any read
      // each pass, so it can join residency without a preheader load.
      if (promoteNames_.count(st.var)) {
        for (int x = 2; x <= 15; ++x) {
          bool used = false;
          for (const auto& kv : loopXmm_)
            if (kv.second == x) used = true;
          if (!used) {
            loopXmm_[varS] = x;
            loopXmmAdded_.push_back(varS);
            break;
          }
        }
      }
      emitLabel(loopL);
      // continue while (step>0 ? cur<end : cur>end)
      emit("    mov " + slotRef(curS) + ", %rax\n    mov " + slotRef(endS) + ", %rcx\n    mov " + slotRef(stepS) + ", %rdx\n");
      emit("    test %rdx, %rdx\n    jns " + bodyL + "_p\n");
      emit("    cmp %rcx, %rax\n    jg " + bodyL + "\n    jmp " + endL + "\n");
      emitLabel(bodyL + "_p");
      emit("    cmp %rcx, %rax\n    jl " + bodyL + "\n    jmp " + endL + "\n");
      // Promoted loop var: varS is a distinct slot holding the raw int64
      // counter value (copied, not boxed); reads box on demand and any
      // non-raw write demotes the binding. The promotion analysis only
      // admits the var when every write is raw-safe, so the slot content
      // always matches the flag across iterations.
      const bool rawVar = promoteNames_.count(st.var) != 0;
      emitLabel(bodyL);
      if (rawVar) {
        copySlot(curS, varS);
      } else {
        // var = a_i64n(cur)
        emit("    mov " + slotRef(curS) + ", %rdi\n    call a_i64n\n");
        storeRax(varS);
      }
      pushScope();
      bindLocal(st.var, varS);
      if (rawVar) {
        ABinding* vb = lookup(st.var);
        if (vb) vb->rawInt = true;
      }
      // Bounds-check elimination: `for i in <nonneg-const>..len(arr)` with
      // step 1 and a pure body that never writes i or arr proves every i in
      // [0, len(arr)), so xs[i] on that same binding can skip its bounds
      // check (see the Index fast path). Anything doubtful -> no entry.
      bool bcePush = false;
      {
        std::unordered_set<std::string> writes;
        const bool pure = scanStmtsPure(st.body, &writes);
        if (pure && !writes.count(st.var) &&
            (stE == nullptr ||
             (stE->kind == ExprKind::IntLit &&
              static_cast<const IntLitExpr*>(stE)->value == 1)) &&
            loE->kind == ExprKind::IntLit &&
            static_cast<const IntLitExpr*>(loE)->value >= 0 &&
            hiE->kind == ExprKind::Call) {
          const CallExpr& hc = static_cast<const CallExpr&>(*hiE);
          if (hc.builtinId == BuiltinId::Len && hc.args.size() == 1 &&
              hc.args[0].name.empty() &&
              hc.args[0].value->kind == ExprKind::Ident) {
            const std::string& an =
                static_cast<const IdentExpr&>(*hc.args[0].value).name;
            TypePtr at = resolveType(hc.args[0].value->inferredType);
            if (an != st.var && at && at->kind == TypeKind::Array &&
                !writes.count(an)) {
              ABinding* ab = lookup(an);
              ABinding* vb2 = lookup(st.var);
              if (ab && vb2) {
                bceLoops_.push_back({vb2, ab});
                bcePush = true;
              }
            }
          }
        }
      }
      loopBreak_.push_back(endL);
      loopCont_.push_back(contL);
      loopDeferDepth_.push_back((int)deferStack_.size());
      emitBody(st.body);
      if (bcePush) bceLoops_.pop_back();
      loopDeferDepth_.pop_back();
      loopBreak_.pop_back();
      loopCont_.pop_back();
      popScope();
      emitLabel(contL);
      emitRangeIncr(curS, endS, stepS);
      jmp(loopL);
      emitLabel(endL);
      loopXmmExit(xmmMark);
      return;
    }
    // Array/tuple-typed iterables get a direct indexed loop over the AArr
    // items buffer (with a runtime kind guard and a generic-iterator
    // fallback for views/other objects).
    TypePtr itT = resolveType(st.iterable->inferredType);
    if (itT && (itT->kind == TypeKind::Array || itT->kind == TypeKind::Tuple)) {
      emitForArray(st);
      return;
    }
    // General iterable: a_iter_new / a_iter_next.
    int itV = evalSlot(st.iterable.get());
    std::string doneL = newLabel("fend");
    emitForIterTail(itV, st, doneL);
    emitLabel(doneL);
  }

  // Generic `for x in <itV-slot>` loop via a_iter_new/a_iter_next; jumps to
  // mergedL when done so callers can share a continuation label.
  void emitForIterTail(int itV, const ForStmt& st, const std::string& mergedL) {
    int itS = allocSlot();
    loadSlot(itV, "%rdi");
    emit("    call a_iter_new\n    mov %rax, " + slotRef(itS));
    int varS = allocSlot();
    movImm(kNone, "%rcx");
    emit("    mov %rcx, " + slotRef(varS));
    std::string loopL = newLabel("for");
    std::string contL = newLabel("forc");
    std::string endL = newLabel("fend");
    emitLabel(loopL);
    loadSlot(itS, "%rdi");
    emit("    lea " + slotRef(varS) + ", %rsi\n    call a_iter_next\n    test %rax, %rax\n");
    emit("    jz " + endL + "\n");
    pushScope();
    bindLocal(st.var, varS);
    loopBreak_.push_back(endL);
    loopCont_.push_back(contL);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emitBody(st.body);
    loopDeferDepth_.pop_back();
    loopBreak_.pop_back();
    loopCont_.pop_back();
    popScope();
    emitLabel(contL);
    jmp(loopL);
    emitLabel(endL);
    jmp(mergedL);
  }

  // Shared tail of an emitForArray fast loop: binds the loop variable,
  // pushes break/continue labels, emits the body, bumps the index and
  // jumps back. `varRaw` marks the binding rawInt (slot holds int64).
  void emitForArrayLoopTail(const ForStmt& st, int iS, int varS,
                            const std::string& loopL,
                            const std::string& contL,
                            const std::string& endL, bool varRaw) {
    pushScope();
    bindLocal(st.var, varS);
    if (varRaw) {
      ABinding* vb = lookup(st.var);
      if (vb) vb->rawInt = true;
    }
    loopBreak_.push_back(endL);
    loopCont_.push_back(contL);
    loopDeferDepth_.push_back((int)deferStack_.size());
    emitBody(st.body);
    loopDeferDepth_.pop_back();
    loopBreak_.pop_back();
    loopCont_.pop_back();
    popScope();
    emitLabel(contL);
    emit("    addq $1, " + slotRef(iS) + "\n");
    jmp(loopL);
  }

  // `for x in <array-typed>`: emit a runtime kind check once; AArr-backed
  // objects (T_LIST/T_TUPLE/T_SET share {AObj; aval *items}) iterate with a
  // pointer-bump loop that reloads len/items each pass (matching
  // a_iter_next's live-view semantics under mutation). Anything else falls
  // back to the generic iterator loop.
  //
  // Packed-int lists (AArr.flags bit0, iitems @24) get a dedicated loop:
  // with a pure body the flag/items/len are loop-invariant, so the packed
  // check runs once and elements load as raw int64 — bound rawInt when the
  // promote scan proves every write to the var is raw-safe, boxed to aval
  // otherwise. Impure bodies use a per-pass flag check that tolerates
  // mid-iteration packed<->aval conversion (append of a non-int).
  void emitForArray(const ForStmt& st) {
    int objS = evalSlot(st.iterable.get());
    std::string fastL = newLabel("afast"), slowL = newLabel("aslow"),
            fastEndL = newLabel("afend"), mergedL = newLabel("aend");
    loadSlot(objS);
    emit("    mov %rax, %rcx\n    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n"
         "    jne " + slowL + "\n");
    emit("    mov %rax, %rcx\n    and $0xF, %rcx\n    jnz " + slowL + "\n");
    emit("    mov %rax, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n");
    emit("    mov (%rcx), %eax\n    cmp $2, %eax\n    je " + fastL + "\n"
         "    cmp $3, %eax\n    je " + fastL + "\n"
         "    cmp $5, %eax\n    je " + fastL + "\n"
         "    jmp " + slowL + "\n");
    emitLabel(fastL);
    // LICM: when the body cannot mutate any heap object (no calls, lambdas,
    // or index/slice/field writes — scanStmtPure), h.len and the items
    // pointer are loop-invariant, so load them once here. %rcx still holds
    // the untagged object pointer from the kind check above. The fallback
    // keeps reloading both every pass, which is what makes mutation during
    // iteration (e.g. append) observable — preserved exactly.
    const bool hoist = scanStmtsPure(st.body, nullptr);
    if (hoist) {
      // Packed check once: pure bodies cannot append/convert, so the flag
      // and the iitems pointer stay valid for the whole loop.
      std::string pkL = newLabel("apk"), avL = newLabel("aav");
      emit("    mov 32(%rcx), %eax\n    test $1, %eax\n"
           "    jnz " + pkL + "\n    jmp " + avL + "\n");
      emitLabel(pkL);
      // ---- packed-int loop (raw int64 elements) ----
      PromoteScan ps;
      promoteWriteStmts(st.body, ps);
      // Spec-write names have no plain-raw element binding: the packed
      // loop stores raw int64 every pass with no demotion check.
      const bool varRaw = !ps.bad.count(st.var) && !ps.spec.count(st.var);
      int pkLenS = allocSlot(), pkItS = allocSlot();
      emit("    mov 4(%rcx), %edx\n    mov %rdx, " + slotRef(pkLenS) + "\n"
           "    mov 24(%rcx), %rcx\n    mov %rcx, " + slotRef(pkItS) + "\n");
      int pkIS = allocSlot(), pkVarS = allocSlot();
      movImm(0, "%rcx");
      emit("    mov %rcx, " + slotRef(pkIS));
      std::string pkLoopL = newLabel("for"), pkContL = newLabel("forc"),
              pkEndL = newLabel("fend");
      int xmmMark = loopXmmEnter();
      // The packed loop var's slot is written before any read each pass,
      // so it can join residency without a preheader load.
      if (varRaw) {
        for (int x = 2; x <= 15; ++x) {
          bool used = false;
          for (const auto& kv : loopXmm_)
            if (kv.second == x) used = true;
          if (!used) {
            loopXmm_[pkVarS] = x;
            loopXmmAdded_.push_back(pkVarS);
            break;
          }
        }
      }
      emitLabel(pkLoopL);
      emit("    mov " + slotRef(pkIS) + ", %rax\n"
           "    mov " + slotRef(pkLenS) + ", %rdx\n    cmp %rdx, %rax\n    jae " +
           pkEndL + "\n"
           "    mov " + slotRef(pkItS) + ", %rcx\n    mov (%rcx,%rax,8), %rax\n");
      if (varRaw) {
        storeRax(pkVarS);  // raw int64 element
      } else {
        emitBoxInt64();    // box to aval element
        storeRax(pkVarS);
      }
      emitForArrayLoopTail(st, pkIS, pkVarS, pkLoopL, pkContL, pkEndL, varRaw);
      emitLabel(pkEndL);
      loopXmmExit(xmmMark);
      jmp(fastEndL);
      // ---- aval loop (existing hoisted path) ----
      emitLabel(avL);
      int lenS = allocSlot(), itemsS = allocSlot();
      emit("    mov 4(%rcx), %edx\n    mov %rdx, " + slotRef(lenS) + "\n"
           "    mov 16(%rcx), %rcx\n    mov %rcx, " + slotRef(itemsS) + "\n");
      int iS = allocSlot();
      movImm(0, "%rcx");
      emit("    mov %rcx, " + slotRef(iS));
      int varS = allocSlot();
      std::string loopL = newLabel("for");
      std::string contL = newLabel("forc");
      std::string avEndL = newLabel("avend");
      int xmmMarkAv = loopXmmEnter();
      emitLabel(loopL);
      // iS = index; lenS/itemsS hoisted. Same load as the reload path but
      // without re-untagging the object or re-reading h.len/items.
      emit("    mov " + slotRef(iS) + ", %rax\n"
           "    mov " + slotRef(lenS) + ", %rdx\n    cmp %rdx, %rax\n    jae " +
           avEndL + "\n"
           "    mov " + slotRef(itemsS) + ", %rcx\n    mov (%rcx,%rax,8), %rax\n"
           "    mov %rax, " + slotRef(varS) + "\n");
      emitForArrayLoopTail(st, iS, varS, loopL, contL, avEndL, false);
      emitLabel(avEndL);
      loopXmmExit(xmmMarkAv);
      jmp(fastEndL);
    } else {
      // Impure body: reload the object each pass and branch on the packed
      // flag per element, so mid-iteration packed<->aval conversion
      // (append of a non-int frees iitems) is observed correctly.
      int iS = allocSlot();
      movImm(0, "%rcx");
      emit("    mov %rcx, " + slotRef(iS));
      int varS = allocSlot();
      std::string loopL = newLabel("for");
      std::string contL = newLabel("forc");
      std::string pkL = newLabel("apkp"), avL = newLabel("apav"),
              bodyL = newLabel("apbd");
      emitLabel(loopL);
      // obj aval -> rcx = object pointer; rdx = len (u32); rax = index.
      emit("    mov " + slotRef(objS) + ", %rax\n"
           "    mov %rax, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n"
           "    mov 4(%rcx), %edx\n"
           "    mov " + slotRef(iS) + ", %rax\n    cmp %rdx, %rax\n    jae " +
           fastEndL + "\n"
           "    mov 32(%rcx), %r8d\n    test $1, %r8d\n    jnz " + pkL + "\n"
           "    jmp " + avL + "\n");
      emitLabel(avL);
      emit("    mov 16(%rcx), %rcx\n    mov (%rcx,%rax,8), %rax\n"
           "    mov %rax, " + slotRef(varS) + "\n    jmp " + bodyL + "\n");
      emitLabel(pkL);
      emit("    mov 24(%rcx), %rcx\n    mov (%rcx,%rax,8), %rax\n");
      emitBoxInt64();
      emit("    mov %rax, " + slotRef(varS) + "\n");
      emitLabel(bodyL);
      emitForArrayLoopTail(st, iS, varS, loopL, contL, fastEndL, false);
    }
    emitLabel(fastEndL);
    jmp(mergedL);
    emitLabel(slowL);
    emitForIterTail(objS, st, mergedL);
    emitLabel(mergedL);
  }

  // Allocate the demotion flag (0 = raw live) and aval slot for a
  // speculative accumulator binding. The aval slot stays uninitialized
  // until the first deopt or aval write — unreachable while flag == 0.
  void initSpecSlots(ABinding* b) {
    b->specRaw = true;
    b->specFlag = allocSlot();
    b->specAval = allocSlot();
    movImm(0, "%rcx");
    emit("    mov %rcx, " + slotRef(b->specFlag) + "\n");
  }

  // Speculative accumulator write (see ABinding::specRaw). Every access
  // checks specFlag: while 0 the raw slot is authoritative and +,-,* ops
  // run with `jo` into a deopt that recomputes through the aval slow path,
  // stores the aval result in specAval and latches flag = 1 forever; once
  // 1, writes go straight to the aval slot. Non-arith writes simply latch
  // the flag after storing the aval.
  void emitSpecAssign(ABinding* b, const AssignStmt& st) {
    const int rawS = b->slot, avalS = b->specAval, flagS = b->specFlag;
    // Classify (mirrors specWriteOk / promoteWriteStmt's rawWrite gate).
    const BinaryExpr* topArith = nullptr;
    bool opAssign = false;
    std::string baseOp;
    if (st.op != "=") {
      opAssign = true;
      baseOp = st.op.substr(0, st.op.size() - 1);
    } else if (st.value && st.value->kind == ExprKind::Binary) {
      const BinaryExpr& bb = static_cast<const BinaryExpr&>(*st.value);
      if ((bb.op == "+" || bb.op == "-" || bb.op == "*") &&
          exprIsInt(bb.lhs.get()) && exprIsInt(bb.rhs.get()) &&
          rawSafeExpr(bb.lhs.get()) && rawSafeExpr(bb.rhs.get()) &&
          !rawSafeExpr(st.value.get()))
        topArith = &bb;
    }
    const bool valRawInt =
        st.value && exprIsInt(st.value.get()) && rawSafeExpr(st.value.get());
    const bool arithOp = baseOp == "+" || baseOp == "-" || baseOp == "*";
    const bool safeOp = baseOp == "&" || baseOp == "|" || baseOp == "^" ||
                        baseOp == "%" || baseOp == "<<" || baseOp == ">>";

    if (topArith) {
      // `v = L <op> R`, unproven top-level arith: raw op + jo deopt.
      const char* slowSym = binopSlow(topArith->op);
      std::string setL = newLabel("spa"), deoptL = newLabel("spd"),
              doneL = newLabel("spx");
      loadSlot(flagS);
      emit("    test %rax, %rax\n    jnz " + setL + "\n");
      int ls = allocSlot(), rs = allocSlot();
      emitRawExpr(topArith->lhs.get(), ls);
      emitRawExpr(topArith->rhs.get(), rs);
      loadSlot(ls);
      loadSlot(rs, "%rcx");
      emit(std::string("    ") +
           (topArith->op == "+" ? "add" : topArith->op == "-" ? "sub" : "imul") +
           " %rcx, %rax\n    jo " + deoptL + "\n");
      storeRax(rawS);
      jmp(doneL);
      // Overflow: operands are still raw in ls/rs (the slot was not
      // written). Box both and recompute through the aval slow path —
      // identical to emitArith's slow branch, including overflow->double.
      emitLabel(deoptL);
      int ta = allocSlot(), tb = allocSlot();
      loadSlot(ls);
      emitBoxInt64();
      rawInvAll();
      storeRax(ta);
      loadSlot(rs);
      emitBoxInt64();
      rawInvAll();
      storeRax(tb);
      call2slot(slowSym, ta, tb);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      movImm(1, "%rcx");
      emit("    mov %rcx, " + slotRef(flagS) + "\n");
      jmp(doneL);
      // Demoted: evaluate the whole RHS as avals (spec reads see flag==1).
      emitLabel(setL);
      int la = evalSlot(topArith->lhs.get());
      int lb = evalSlot(topArith->rhs.get());
      emitArith(topArith->op, la, lb);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      emitLabel(doneL);
      return;
    }

    if (opAssign && arithOp && valRawInt) {
      // `v <op>= e`, op in {+,-,*}: raw update + jo deopt.
      const char* slowSym = binopSlow(baseOp);
      std::string setL = newLabel("spa"), deoptL = newLabel("spd"),
              doneL = newLabel("spx");
      loadSlot(flagS);
      emit("    test %rax, %rax\n    jnz " + setL + "\n");
      int vs = allocSlot();
      emitRawExpr(st.value.get(), vs);
      loadSlot(rawS);
      loadSlot(vs, "%rcx");
      emit(std::string("    ") +
           (baseOp == "+" ? "add" : baseOp == "-" ? "sub" : "imul") +
           " %rcx, %rax\n    jo " + deoptL + "\n");
      storeRax(rawS);
      jmp(doneL);
      // v_old is still in rawS (jo fired before the store); e is raw in vs.
      emitLabel(deoptL);
      int ta = allocSlot(), tb = allocSlot();
      loadSlot(rawS);
      emitBoxInt64();
      rawInvAll();
      storeRax(ta);
      loadSlot(vs);
      emitBoxInt64();
      rawInvAll();
      storeRax(tb);
      call2slot(slowSym, ta, tb);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      movImm(1, "%rcx");
      emit("    mov %rcx, " + slotRef(flagS) + "\n");
      jmp(doneL);
      emitLabel(setL);
      int cs = evalSlot(st.target.get());
      int rs = evalSlot(st.value.get());
      emitArith(baseOp, cs, rs);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      emitLabel(doneL);
      return;
    }

    if (opAssign && safeOp && valRawInt) {
      // Bitwise/%/shift op-assign: result is always int64-or-panic, so the
      // raw path never deopts — but the flag still selects the
      // representation once demoted.
      std::string setL = newLabel("spa"), doneL = newLabel("spx");
      loadSlot(flagS);
      emit("    test %rax, %rax\n    jnz " + setL + "\n");
      int vs = allocSlot();
      emitRawExpr(st.value.get(), vs);
      emitRawBin(baseOp, rawS, vs, rawS);
      jmp(doneL);
      emitLabel(setL);
      int cs = evalSlot(st.target.get());
      int rs = evalSlot(st.value.get());
      emitArith(baseOp, cs, rs);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      emitLabel(doneL);
      return;
    }

    if (opAssign) {
      // Non-raw-safe operand (or unknown op): identical to the generic
      // op-assign path, but the result lands in the aval slot and latches
      // demotion. evalSlot(target) yields the right aval whether the
      // binding is still raw (boxes it) or already demoted.
      int cs = evalSlot(st.target.get());
      int rs = evalSlot(st.value.get());
      emitArith(baseOp, cs, rs);
      emit("    mov %rax, " + slotRef(avalS) + "\n");
      movImm(1, "%rcx");
      emit("    mov %rcx, " + slotRef(flagS) + "\n");
      return;
    }

    if (valRawInt) {
      // `v = <raw-safe int>`: plain raw write while flag==0; once demoted
      // the value must be stored as an aval (boxed) instead.
      std::string setL = newLabel("spa"), doneL = newLabel("spx");
      loadSlot(flagS);
      emit("    test %rax, %rax\n    jnz " + setL + "\n");
      emitRawExpr(st.value.get(), rawS);
      jmp(doneL);
      emitLabel(setL);
      int t = allocSlot();
      emitRawExpr(st.value.get(), t);
      loadSlot(t);
      emitBoxInt64();
      rawInvAll();
      storeRax(avalS);
      emitLabel(doneL);
      return;
    }

    // `v = <non-raw-safe value>`: latch demotion and store the aval.
    int vs = evalSlot(st.value.get());
    copySlot(vs, avalS);
    movImm(1, "%rcx");
    emit("    mov %rcx, " + slotRef(flagS) + "\n");
  }

  void emitAssign(const AssignStmt& st) {
    if (st.target->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*st.target);
      ABinding* b = lookup(id.name);
      // A name that resolves to a module/builtin/capture is not a storable
      // target (e.g. a local named `words` in `words.a`, whose entry stem
      // is bound as a Module in the global scope): shadow it with a fresh
      // local instead of failing in storeBinding.
      const bool assignable = b && (b->kind == BKind::Local ||
                                    b->kind == BKind::Param ||
                                    b->kind == BKind::Global ||
                                    b->kind == BKind::FnValue ||
                                    b->kind == BKind::StructCtor ||
                                    b->kind == BKind::EnumValue);
      if (!assignable) {
        int sl = allocSlot();
        bindLocal(id.name, sl);
        b = lookup(id.name);
        // Fresh local from `x = <raw int expr>`: promote like a `let`.
        if (st.op == "=" && b && promoteNames_.count(id.name) &&
            rawSafeExpr(st.value.get()) && exprIsInt(st.value.get())) {
          emitRawExpr(st.value.get(), sl);
          b->rawInt = true;
          if (specNames_.count(id.name)) initSpecSlots(b);
          return;
        }
      }
      // Speculative accumulator: the slot/flag/aval representation is
      // selected per-write; intercept before the plain rawInt paths.
      if (b->specRaw) {
        emitSpecAssign(b, st);
        return;
      }
      // Promoted local + raw-safe int write: keep the slot as raw int64.
      // Ints never own drops, so the drop bookkeeping below is unnecessary.
      if (b->rawInt && st.op == "=" && !b->dropFn &&
          rawSafeExpr(st.value.get()) && exprIsInt(st.value.get())) {
        emitRawExpr(st.value.get(), b->slot);
        return;
      }
      // Promoted local + proven non-overflowing op-assign (x += v, x &= v,
      // ...): update the raw int64 slot in place. The proof mirrors
      // promoteWriteStmt's assignOpRawOk gate, so a promoted binding always
      // takes this path and keeps rawInt set.
      if (b->rawInt && !b->dropFn && st.op.size() > 1 && st.op.back() == '=' &&
          assignOpRawOk(id.name, st.op.substr(0, st.op.size() - 1),
                        st.value.get())) {
        int vs = allocSlot();
        emitRawExpr(st.value.get(), vs);
        emitRawBin(st.op.substr(0, st.op.size() - 1), b->slot, vs, b->slot);
        return;
      }
      int vs;
      if (st.op != "=") {
        std::string baseOp = st.op.substr(0, st.op.size() - 1);
        int cs = evalSlot(st.target.get());
        int rs = evalSlot(st.value.get());
        emitArith(baseOp, cs, rs);  // rax = cur <op> val
        vs = allocSlot();
        storeRax(vs);
      } else {
        vs = evalSlot(st.value.get());
      }
      // Rebinding an owned struct drops the previous value first.
      DropRec* oldRec = b->dropFn ? findDropRec(*b) : nullptr;
      if (b->dropFn && oldRec &&
          (b->kind == BKind::Local || b->kind == BKind::Param ||
           b->kind == BKind::Global)) {
        emitDropRec(*oldRec);
      }
      storeBinding(*b, vs);
      // Demotion: an aval was just stored into this slot, so it no longer
      // holds a raw int64 (covers op-assigns and non-raw-safe values).
      if (b->rawInt) b->rawInt = false;
      FnInfo* newDrop = dropFnFor(st.value->inferredType);
      b->dropFn = newDrop;
      // Update this binding's pending drop record in place, or register one.
      DropRec* rec = findDropRec(*b);
      if (rec) {
        rec->fn = newDrop;
      } else if (newDrop && !inFunction_ &&
                 (b->kind == BKind::Local || b->kind == BKind::Global)) {
        if (b->kind == BKind::Global) {
          deferStack_.back().drops.push_back({-1, newDrop, b->sym});
        } else {
          deferStack_.back().drops.push_back({b->slot, newDrop, ""});
        }
      } else if (newDrop && inFunction_ && b->kind == BKind::Local) {
        deferStack_.back().drops.push_back({b->slot, newDrop, ""});
      }
      return;
    }
    if (st.target->kind == ExprKind::Index) {
      const IndexExpr& ix = static_cast<const IndexExpr&>(*st.target);
      int os = evalSlot(ix.object.get());
      int is = evalSlot(ix.index.get());
      int vs = evalSlot(st.value.get());
      if (st.op != "=") {
        // obj[i] op= v  ->  obj[i] = obj[i] <op> v
        std::string baseOp = st.op.substr(0, st.op.size() - 1);
        int cs = allocSlot();
        call2slot("a_getitem", os, is);
        storeRax(cs);
        emitArith(baseOp, cs, vs);
        int ns = allocSlot();
        storeRax(ns);
        call3slot("a_setitem", os, is, ns);
        return;
      }
      call3slot("a_setitem", os, is, vs);
      return;
    }
    if (st.target->kind == ExprKind::Field) {
      const FieldExpr& fx = static_cast<const FieldExpr&>(*st.target);
      // Module attribute assignment: counter.value = 99
      if (fx.object->kind == ExprKind::Ident) {
        ABinding* mb = lookup(static_cast<const IdentExpr&>(*fx.object).name);
        if (mb && mb->kind == BKind::Module && mb->moduleId >= 0) {
          auto it = topLevelGlobals_.find(std::to_string(mb->moduleId) + ":" + fx.field);
          if (it != topLevelGlobals_.end()) {
            ABinding gb;
            gb.kind = BKind::Global;
            gb.sym = it->second;
            if (st.op != "=") {
              // mod.g op= v  ->  mod.g = mod.g <op> v
              std::string baseOp = st.op.substr(0, st.op.size() - 1);
              int vs = evalSlot(st.value.get());
              int cs = allocSlot();
              emit("    mov " + gb.sym + "(%rip), %rax\n");
              storeRax(cs);
              emitArith(baseOp, cs, vs);
              int ns = allocSlot();
              storeRax(ns);
              storeBinding(gb, ns);
              return;
            }
            int vs = evalSlot(st.value.get());
            storeBinding(gb, vs);
            return;
          }
        }
      }
      int os = evalSlot(fx.object.get());
      int vs = evalSlot(st.value.get());
      std::string lbl = emitStrData(fx.field, true);
      if (st.op != "=") {
        // obj.f op= v  ->  obj.f = obj.f <op> v
        std::string baseOp = st.op.substr(0, st.op.size() - 1);
        const char* sym = binopSlow(baseOp);
        if (!sym) {
          fail("unsupported operator '" + baseOp + "'");
          return;
        }
        loadSlot(os, "%rdi");
        emit("    lea " + lbl + "(%rip), %rsi\n    call a_getattr\n");
        int cs = allocSlot();
        storeRax(cs);
        call2slot(sym, cs, vs);
        int ns = allocSlot();
        storeRax(ns);
        loadSlot(os, "%rdi");
        emit("    lea " + lbl + "(%rip), %rsi\n");
        loadSlot(ns, "%rdx");
        emit("    call a_setattr");
        return;
      }
      loadSlot(os, "%rdi");
      emit("    lea " + lbl + "(%rip), %rsi\n");
      loadSlot(vs, "%rdx");
      emit("    call a_setattr");
      return;
    }
    fail("unsupported assignment target");
  }

  // ---- small-function inlining ---------------------------------------------
  // The scanner is deliberately conservative: it only proves that emitting the
  // body at a direct call site cannot change name resolution, control flow, or
  // ownership behaviour. Anything doubtful disables inlining for that fn.
  bool exprIsInt(const Expr* e) const {
    TypePtr t = e ? resolveType(e->inferredType) : nullptr;
    return t && t->kind == TypeKind::Int;
  }

  void scanInlineExpr(InlineScan& sc, const Expr* e) {
    if (!e || sc.unsafe) return;
    if (++sc.cost > kMaxInlineCost) {
      sc.unsafe = true;
      return;
    }
    TypePtr rt = resolveType(e->inferredType);
    if (rt && rt->kind == TypeKind::Struct && dropFnFor(rt)) sc.unsafe = true;
    if (!exprIsInt(e)) sc.nonInt = true;
    const bool rawSimple =
        e->kind == ExprKind::IntLit || e->kind == ExprKind::Ident ||
        (e->kind == ExprKind::Unary &&
         static_cast<const UnaryExpr&>(*e).op == "~") ||
        (e->kind == ExprKind::Binary &&
         (static_cast<const BinaryExpr&>(*e).op == "&" ||
          static_cast<const BinaryExpr&>(*e).op == "|" ||
          static_cast<const BinaryExpr&>(*e).op == "^" ||
          static_cast<const BinaryExpr&>(*e).op == "<<" ||
          static_cast<const BinaryExpr&>(*e).op == ">>" ||
          static_cast<const BinaryExpr&>(*e).op == "%"));
    if (!rawSimple) sc.unsupportedRaw = true;
    switch (e->kind) {
      case ExprKind::Ident:
        if (static_cast<const IdentExpr&>(*e).name == sc.fn->name) sc.unsafe = true;
        return;
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return;
      case ExprKind::StringLit:
        for (const StringPart& p : static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) scanInlineExpr(sc, p.expr.get());
        return;
      case ExprKind::Unary:
        scanInlineExpr(sc, static_cast<const UnaryExpr&>(*e).operand.get());
        return;
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        scanInlineExpr(sc, b.lhs.get());
        scanInlineExpr(sc, b.rhs.get());
        return;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        scanInlineExpr(sc, c.callee.get());
        for (const CallArg& a : c.args) scanInlineExpr(sc, a.value.get());
        return;
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        scanInlineExpr(sc, x.object.get());
        scanInlineExpr(sc, x.index.get());
        return;
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        scanInlineExpr(sc, x.object.get());
        scanInlineExpr(sc, x.start.get());
        scanInlineExpr(sc, x.end.get());
        scanInlineExpr(sc, x.step.get());
        return;
      }
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr&>(*e).entries) {
          scanInlineExpr(sc, kv.first.get());
          scanInlineExpr(sc, kv.second.get());
        }
        return;
      case ExprKind::SetLit:
      case ExprKind::ArrayLit:
      case ExprKind::Tuple: {
        const auto& els = e->kind == ExprKind::SetLit
            ? static_cast<const SetLitExpr&>(*e).elements
            : e->kind == ExprKind::ArrayLit
                ? static_cast<const ArrayLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els) scanInlineExpr(sc, el.get());
        return;
      }
      case ExprKind::Field:
        scanInlineExpr(sc, static_cast<const FieldExpr&>(*e).object.get());
        return;
      case ExprKind::SafeNav:
        scanInlineExpr(sc, static_cast<const SafeNavExpr&>(*e).object.get());
        return;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        scanInlineExpr(sc, r.start.get());
        scanInlineExpr(sc, r.end.get());
        scanInlineExpr(sc, r.step.get());
        return;
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        scanInlineExpr(sc, t.cond.get());
        scanInlineExpr(sc, t.thenValue.get());
        scanInlineExpr(sc, t.elseValue.get());
        return;
      }
      case ExprKind::Lambda:
        scanInlineExpr(sc, static_cast<const LambdaExpr&>(*e).body.get());
        return;
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        scanInlineExpr(sc, c.element.get());
        scanInlineExpr(sc, c.iterable.get());
        scanInlineExpr(sc, c.condition.get());
        return;
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        scanInlineExpr(sc, m.subject.get());
        for (const MatchArm& arm : m.arms) {
          scanInlinePattern(sc, arm.pattern);
          scanInlineExpr(sc, arm.guard.get());
          scanInlineExpr(sc, arm.result.get());
        }
        return;
      }
      case ExprKind::Try:
        scanInlineExpr(sc, static_cast<const TryExpr&>(*e).operand.get());
        return;
      default:
        return;
    }
  }

  void scanInlinePattern(InlineScan& sc, const Pattern& p) {
    if (sc.unsafe) return;
    if (++sc.cost > kMaxInlineCost) {
      sc.unsafe = true;
      return;
    }
    scanInlineExpr(sc, p.value.get());
    scanInlineExpr(sc, p.rangeStart.get());
    scanInlineExpr(sc, p.rangeEnd.get());
    scanInlineExpr(sc, p.ctor.get());
    for (const Pattern& sub : p.subpatterns) scanInlinePattern(sc, sub);
  }

  void scanInlineStmt(InlineScan& sc, const Stmt* s) {
    if (!s || sc.unsafe) return;
    if (++sc.cost > kMaxInlineCost) {
      sc.unsafe = true;
      return;
    }
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        scanInlineExpr(sc, st.init.get());
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind == ExprKind::Ident) {
          const std::string& nm = static_cast<const IdentExpr&>(*st.target).name;
          if (nm == sc.fn->name) sc.unsafe = true;
          if (st.op != "=") sc.unsupportedRawStmt = true;
        } else {
          sc.unsupportedRawStmt = true;
          scanInlineExpr(sc, st.target.get());
        }
        scanInlineExpr(sc, st.value.get());
        return;
      }
      case StmtKind::Expr:
        scanInlineExpr(sc, static_cast<const ExprStmt&>(*s).expr.get());
        return;
      case StmtKind::Destructure:
        sc.unsupportedRawStmt = true;
        scanInlineExpr(sc, static_cast<const DestructureStmt&>(*s).value.get());
        return;
      case StmtKind::Return:
        scanInlineExpr(sc, static_cast<const ReturnStmt&>(*s).value.get());
        return;
      case StmtKind::Break:
      case StmtKind::Continue:
        sc.unsupportedRawStmt = true;
        return;
      case StmtKind::If: {
        sc.unsupportedRawStmt = true;
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const auto& br : st.branches) {
          scanInlineExpr(sc, br.cond.get());
          for (const StmtPtr& b : br.body) scanInlineStmt(sc, b.get());
        }
        for (const StmtPtr& b : st.elseBody) scanInlineStmt(sc, b.get());
        return;
      }
      case StmtKind::While: {
        sc.unsupportedRawStmt = true;
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        scanInlineExpr(sc, st.cond.get());
        for (const StmtPtr& b : st.body) scanInlineStmt(sc, b.get());
        return;
      }
      case StmtKind::DoWhile: {
        sc.unsupportedRawStmt = true;
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        for (const StmtPtr& b : st.body) scanInlineStmt(sc, b.get());
        scanInlineExpr(sc, st.cond.get());
        return;
      }
      case StmtKind::For: {
        sc.unsupportedRawStmt = true;
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        scanInlineExpr(sc, st.iterable.get());
        for (const StmtPtr& b : st.body) scanInlineStmt(sc, b.get());
        return;
      }
      case StmtKind::Loop: {
        sc.unsupportedRawStmt = true;
        const LoopStmt& st = static_cast<const LoopStmt&>(*s);
        for (const StmtPtr& b : st.body) scanInlineStmt(sc, b.get());
        return;
      }
      default:
        sc.unsafe = true;
        return;
    }
  }

  // Conservative check for whether an otherwise-undeclared assignment target
  // could resolve to a global/function/struct/enum binding rather than a fresh
  // local. Used only by the raw-int path; false positives just disable it.
  bool rawNameMayBeGlobal(const std::string& nm) const {
    for (const auto& kv : topLevelGlobals_) {
      const std::string& key = kv.first;
      if (key.size() > nm.size() &&
          key.compare(key.size() - nm.size(), nm.size(), nm) == 0 &&
          key[key.size() - nm.size() - 1] == ':')
        return true;
    }
    for (FnInfo* f : fnList_)
      if (!f->isMethod && f->name == nm) return true;
    for (StructInfo* st : structList_)
      if (st->name == nm) return true;
    for (const auto& kv : enums_) {
      std::size_t dot = kv.first.find('.');
      if (dot != std::string::npos && kv.first.substr(dot + 1) == nm) return true;
    }
    return false;
  }

  bool rawInlineShapeOk(const FnDecl* fn) const {
    if (!fn || fn->body.empty()) return false;
    std::set<std::string> declared;
    for (const ParamDecl& p : fn->params) declared.insert(p.name);
    int returns = 0;
    for (std::size_t i = 0; i < fn->body.size(); ++i) {
      const Stmt* s = fn->body[i].get();
      if (!s) return false;
      switch (s->kind) {
        case StmtKind::Let: {
          const LetStmt& st = static_cast<const LetStmt&>(*s);
          if (!st.init) return false;
          declared.insert(st.name);
          break;
        }
        case StmtKind::Assign: {
          const AssignStmt& st = static_cast<const AssignStmt&>(*s);
          if (st.op != "=" || !st.target || st.target->kind != ExprKind::Ident ||
              !st.value) return false;
          const std::string& nm = static_cast<const IdentExpr&>(*st.target).name;
          if (!declared.count(nm)) {
            // `v = ...` with no prior binding creates a local in codegen unless
            // the name resolves to a global/function/struct/enum binding.
            if (rawNameMayBeGlobal(nm)) return false;
            declared.insert(nm);
          }
          break;
        }
        case StmtKind::Expr:
          break;
        case StmtKind::Return: {
          const ReturnStmt& st = static_cast<const ReturnStmt&>(*s);
          if (i + 1 != fn->body.size() || !st.value) return false;
          ++returns;
          break;
        }
        default:
          return false;
      }
    }
    return returns == 1;
  }

  bool allIntParamsAndReturn(const FnDecl* fn) const {
    auto pit = sema_.fnParamTypes.find(fn);
    if (pit == sema_.fnParamTypes.end() || pit->second.size() != fn->params.size())
      return false;
    for (const TypePtr& t : pit->second) {
      TypePtr r = resolveType(t);
      if (!r || r->kind != TypeKind::Int) return false;
    }
    auto rit = sema_.fnReturnTypes.find(fn);
    if (rit == sema_.fnReturnTypes.end()) return false;
    TypePtr r = resolveType(rit->second);
    return r && r->kind == TypeKind::Int;
  }

  const InlineInfo& inlineInfoFor(FnInfo* f) {
    auto it = inlineInfo_.find(f->decl);
    if (it != inlineInfo_.end()) return it->second;
    InlineInfo info;
    if (f->decl && !f->isMethod && !f->variadic && f->decl->comptimeParams.empty() &&
        f->decl->typeParams.empty() && f->paramNames.size() == f->decl->params.size()) {
      InlineScan sc;
      sc.fn = f->decl;
      for (const StmtPtr& s : f->decl->body) scanInlineStmt(sc, s.get());
      info.safe = !sc.unsafe && sc.cost <= kMaxInlineCost;
      if (info.safe) {
        info.raw = !sc.nonInt && !sc.unsupportedRaw && !sc.unsupportedRawStmt &&
                   rawInlineShapeOk(f->decl) && allIntParamsAndReturn(f->decl);
      }
    }
    auto res = inlineInfo_.emplace(f->decl, info);
    return res.first->second;
  }

  // Raw int64 helpers used only while emitting a raw-inline body. Slots in
  // rawVars_ hold untagged int64 values, not avals.
  bool rawIntLitFits(std::int64_t v) const {
    const std::int64_t lim = std::int64_t(1) << 46;
    return v >= -lim && v < lim;
  }

  // ---- Phase 5: raw int64 expressions & scalar promotion -------------------
  // Ops whose int64 result is always an integer (or a panic): bitwise, shifts
  // (panic on out-of-range amount), '~', and '%'. '+', '-', '*', '/' are
  // excluded on purpose: int64 overflow (and INT64_MIN / -1) produces a
  // *double* aval that a raw int64 slot cannot represent, so those stay on
  // the aval path where the slow helpers implement overflow->double.
  static bool rawSafeBinOp(const std::string& op) {
    return op == "&" || op == "|" || op == "^" || op == "<<" ||
           op == ">>" || op == "%";
  }

  // Direct-call target eligible for raw (int64) inlining, or nullptr.
  // Mirrors the gates in tryEmitInlineCall; used by both the emitter and the
  // promotion analysis so they always agree.
  FnInfo* rawCallTarget(const CallExpr& call) {
    if (call.callee->kind != ExprKind::Ident) return nullptr;
    ABinding* b = lookup(static_cast<const IdentExpr&>(*call.callee).name);
    if (!b || b->kind != BKind::FnValue || !b->fn) return nullptr;
    FnInfo* f = b->fn;
    if (f->isMethod || f->variadic || !f->decl || f->moduleId != curModuleId_ ||
        call.args.size() != f->paramNames.size())
      return nullptr;
    for (const CallArg& a : call.args)
      if (!a.name.empty()) return nullptr;
    if (!callArgsAllInt(call)) return nullptr;
    if (!inlineInfoFor(f).raw) return nullptr;
    return f;
  }

  // Structural check mirroring emitRawExpr's capabilities. Ident is always
  // accepted: bindings that are not raw-int fall back to aval evaluation +
  // a_int_full (the same unbox the Phase-4 raw path already used).
  bool rawSafeExpr(const Expr* e) {
    if (!e || failed_) return false;
    switch (e->kind) {
      case ExprKind::IntLit:
      case ExprKind::Ident:
        return true;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if (u.op == "~") return rawSafeExpr(u.operand.get());
        // Unary minus is int-safe only when the operand range proves the
        // result cannot be -INT64_MIN (which yields a double aval).
        if (u.op == "-" && !u.postfix && exprIsInt(e) &&
            rawSafeExpr(u.operand.get())) {
          ARange o = exprRangeFlat(u.operand.get());
          return o.known && o.lo > rngI64Min();
        }
        return false;
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        if (rawSafeBinOp(b.op))
          return rawSafeExpr(b.lhs.get()) && rawSafeExpr(b.rhs.get());
        // '+', '-', '*' are raw-safe only where range analysis proves the
        // int64 result cannot overflow (overflow produces a double aval).
        if ((b.op == "+" || b.op == "-" || b.op == "*") &&
            exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
            rawSafeExpr(b.lhs.get()) && rawSafeExpr(b.rhs.get()))
          return arithFits(exprRangeFlat(b.lhs.get()),
                           exprRangeFlat(b.rhs.get()), b.op);
        return false;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        if (!rawCallTarget(c)) return false;
        for (const CallArg& a : c.args)
          if (!rawSafeExpr(a.value.get())) return false;
        return true;
      }
      default:
        return false;
    }
  }

  // emitBinary fast-path gate: a safe-op binary whose operands are all raw
  // int64 materializable. Kept in sync with the emitBinary call site and the
  // promotion coverage scan.
  bool rawBinFastOk(const BinaryExpr& b) {
    return exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
           rawSafeExpr(&b);
  }

  // Raw-expression entry point. Each top-level call opens a fresh
  // register-cache region; nested calls (rawRegDepth_ > 0) share the
  // caller's cache so intermediates forward through the whole tree. The
  // cache is dropped on exit so no facts leak into unmodeled code.
  bool emitRawExpr(const Expr* e, int out) {
    if (rawRegDepth_ == 0) rawRegOf_.clear();
    ++rawRegDepth_;
    bool ok = emitRawExprImpl(e, out);
    --rawRegDepth_;
    if (rawRegDepth_ == 0) rawRegOf_.clear();
    return ok;
  }

  bool emitRawExprImpl(const Expr* e, int out) {
    if (!e || failed_) return false;
    if (e->kind == ExprKind::IntLit) {
      std::int64_t v = static_cast<const IntLitExpr&>(*e).value;
      if (rawIntLitFits(v)) {
        movImm((uint64_t)v);
        storeRax(out);
      } else {
        // Preserve the generic literal encoding exactly (including its
        // range behaviour) by materializing the aval and unboxing it.
        { RawRegSuspend sus(this); emitExpr(e); }
        emit("    mov %rax, %rdi\n");
        emitAvalToI64();
        rawInvAll();
        storeRax(out);
      }
      return true;
    }
    if (e->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr&>(*e).name;
      auto it = rawVars_.find(nm);
      if (it != rawVars_.end()) {
        copySlot(it->second, out);
        return true;
      }
      // Speculative accumulator: flag==0 -> copy the raw int64 slot;
      // flag==1 -> unbox the canonical aval slot back to int64.
      ABinding* b = lookup(nm);
      if (b && b->specRaw) {
        std::string setL = newLabel("spr"), doneL = newLabel("spd");
        loadSlot(b->specFlag);
        emit("    test %rax, %rax\n    jnz " + setL + "\n");
        copySlot(b->slot, out);
        emit("    jmp " + doneL + "\n");
        emitLabel(setL);
        loadSlot(b->specAval, "%rdi");
        emitAvalToI64();
        rawInvAll();  // the unbox clobbers registers the cache cannot track
        storeRax(out);
        emitLabel(doneL);
        return true;
      }
      // Promoted local: the slot already holds a raw int64.
      if (b && b->rawInt) {
        copySlot(b->slot, out);
        return true;
      }
      { RawRegSuspend sus(this); emitExpr(e); }
      emit("    mov %rax, %rdi\n");
      emitAvalToI64();
      rawInvAll();
      storeRax(out);
      return true;
    }
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
      if (u.op == "-" && !u.postfix) {
        // rawSafeExpr proved operand range excludes INT64_MIN, so neg cannot
        // produce the overflow double.
        int os = allocSlot();
        if (!emitRawExpr(u.operand.get(), os)) return false;
        loadSlot(os);
        emit("    neg %rax\n");
        rawDropReg(kRRax);
        storeRax(out);
        return true;
      }
      if (u.op != "~") {
        fail("unsupported raw int unary operator '" + u.op + "'");
        return false;
      }
      int os = allocSlot();
      if (!emitRawExpr(u.operand.get(), os)) return false;
      loadSlot(os);
      emit("    not %rax\n");
      rawDropReg(kRRax);
      storeRax(out);
      return true;
    }
    if (e->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
      int ls = allocSlot(), rs = allocSlot();
      if (!emitRawExpr(b.lhs.get(), ls) || !emitRawExpr(b.rhs.get(), rs))
        return false;
      emitRawBin(b.op, ls, rs, out);
      return true;
    }
    if (e->kind == ExprKind::Call) {
      const CallExpr& c = static_cast<const CallExpr&>(*e);
      FnInfo* f = rawCallTarget(c);
      if (!f) {
        fail("unsupported raw int call");
        return false;
      }
      if (rawDepth_ >= kMaxRawDepth || rawCallCount_ >= kMaxRawCalls) {
        // Inlining budget exhausted: emit an ordinary aval call and unbox.
        // Args still go through emitRawExpr + box so raw-int locals and
        // raw-inline params (rawVars_) resolve correctly in every context.
        std::vector<int> ps(f->paramNames.size(), -1);
        for (std::size_t i = 0; i < c.args.size(); ++i) {
          int rs = allocSlot();
          emitRawExpr(c.args[i].value.get(), rs);
          loadSlot(rs);
          emitBoxInt64();
          rawInvAll();
          int vs = allocSlot();
          storeRax(vs);
          ps[i] = vs;
        }
        { RawRegSuspend sus(this); emitDirectCall(f->cname.c_str(), ps); }
        emit("    mov %rax, %rdi\n");
        emitAvalToI64();
        rawInvAll();
        storeRax(out);
        return true;
      }
      ++rawCallCount_;
      ++rawDepth_;
      std::vector<int> rargs;
      rargs.reserve(c.args.size());
      for (const CallArg& a : c.args) {
        int rs = allocSlot();
        emitRawExpr(a.value.get(), rs);
        rargs.push_back(rs);
      }
      emitRawBody(f, rargs, true, true);
      --rawDepth_;
      storeRax(out);
      return true;
    }
    fail("unsupported raw int expression");
    return false;
  }

  void emitRawBin(const std::string& op, int ls, int rs, int out) {
    if (op == "+" || op == "-" || op == "*") {
      // Only reachable when rawSafeExpr's range proof showed the int64
      // result cannot overflow (overflow must yield a double aval, which a
      // raw slot cannot hold), so a plain add/sub/imul is exact — no jo.
      loadSlot(ls, "%rax");
      loadSlot(rs, "%rcx");
      emit(std::string("    ") + (op == "+" ? "add" : op == "-" ? "sub" : "imul") +
           " %rcx, %rax\n");
      rawDropReg(kRRax);  // only %rax is rewritten; %rcx and friends survive
      storeRax(out);
      return;
    }
    if (op == "&" || op == "|" || op == "^") {
      loadSlot(ls, "%rax");
      loadSlot(rs, "%rcx");
      emit(std::string("    ") + (op == "&" ? "and" : op == "|" ? "or" : "xor") +
           " %rcx, %rax\n");
      rawDropReg(kRRax);  // only %rax is rewritten; %rcx and friends survive
      storeRax(out);
      return;
    }
    if (op == "%") {
      // int64 remainder, matching arith_int_int('%'): divisor 0 panics with
      // the same message, divisor -1 yields 0 (also avoiding the
      // INT64_MIN % -1 hardware trap), otherwise idiv leaves the remainder
      // in %rdx. The result is always an integer, never a double.
      std::string modzL = newLabel("rmdz"), zeroL = newLabel("rmd0"),
              doneL = newLabel("rmdd");
      loadSlot(rs, "%rcx");
      emit("    test %rcx, %rcx\n    jz " + modzL + "\n"
           "    cmp $-1, %rcx\n    je " + zeroL + "\n");
      loadSlot(ls, "%rax");
      emit("    cqto\n    idiv %rcx\n    mov %rdx, %rax\n    jmp " + doneL +
           "\n");
      emitLabel(zeroL);
      emit("    xor %eax, %eax\n    jmp " + doneL + "\n");
      emitLabel(modzL);
      emitPanicStr("Remainder by zero.\n  = help: Check the divisor before dividing.");
      // Join point: register contents differ per incoming path (idiv
      // clobbers %rdx:%rax, the panic path clobbers everything), so only
      // the %rax result stored below is certain on all paths.
      emitLabel(doneL);
      rawInvAll();
      storeRax(out);
      return;
    }
    if (op != "<<" && op != ">>") {
      fail("unsupported raw int operator '" + op + "'");
      movImm(0);
      storeRax(out);
      return;
    }
    const char* slowSym = op == "<<" ? "a_shl_slow" : "a_shr_slow";
    std::string slowL = newLabel("rshs"), doneL = newLabel("rshd");
    loadSlot(rs, "%rcx");
    emit("    test %rcx, %rcx\n    js " + slowL + "\n"
         "    cmp $63, %rcx\n    ja " + slowL + "\n");
    loadSlot(ls, "%rax");
    emit(std::string("    ") + (op == "<<" ? "shl" : "sar") + " %cl, %rax\n");
    rawDropReg(kRRax);  // the shift rewrites %rax; %rcx keeps the count
    emit("    jmp " + doneL + "\n");
    emitLabel(slowL);
    // Invalid shift amount: reproduce the runtime path exactly (it panics
    // for out-of-range amounts). Box both operands and call the slow helper.
    loadSlot(ls);
    emitBoxInt64();
    rawInvAll();
    int lBox = allocSlot();
    storeRax(lBox);
    loadSlot(rs);
    emitBoxInt64();
    rawInvAll();
    int rBox = allocSlot();
    storeRax(rBox);
    loadSlot(rBox, "%rsi");
    loadSlot(lBox, "%rdi");
    emit(std::string("    call ") + slowSym + "\n"
         "    mov %rax, %rdi\n    call a_int_full\n");
    rawInvAll();
    // Join point: %rax holds the raw int64 result on both paths, nothing
    // else is certain (the slow path called out).
    emitLabel(doneL);
    storeRax(out);
  }

  // argsRaw: argSlots already hold raw int64 (skip the aval->int64 unbox).
  // keepRaw: leave the raw int64 result in %rax instead of boxing to aval.
  // rawVars_ is saved/restored so raw-inline calls can nest inside
  // emitRawExpr (e.g. `f(g(x))` or a promoted `x = f(y)`).
  void emitRawBody(FnInfo* f, const std::vector<int>& argSlots,
                   bool argsRaw = false, bool keepRaw = false) {
    auto savedRawVars = std::move(rawVars_);
    rawVars_.clear();
    const FnDecl* fn = f->decl;
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      int rs = allocSlot();
      if (argsRaw) {
        copySlot(argSlots[i], rs);
      } else {
        loadSlot(argSlots[i], "%rdi");
        emitAvalToI64();
        rawInvAll();
        storeRax(rs);
      }
      rawVars_[fn->params[i].name] = rs;
    }
    for (const StmtPtr& sp : fn->body) {
      const Stmt* s = sp.get();
      if (s->kind == StmtKind::Let) {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        int vs = allocSlot();
        emitRawExpr(st.init.get(), vs);
        rawVars_[st.name] = vs;
      } else if (s->kind == StmtKind::Assign) {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        const std::string& nm = static_cast<const IdentExpr&>(*st.target).name;
        auto it = rawVars_.find(nm);
        if (it == rawVars_.end()) {
          int vs = allocSlot();
          emitRawExpr(st.value.get(), vs);
          rawVars_[nm] = vs;
        } else {
          emitRawExpr(st.value.get(), it->second);
        }
      } else if (s->kind == StmtKind::Expr) {
        int tmp = allocSlot();
        emitRawExpr(static_cast<const ExprStmt&>(*s).expr.get(), tmp);
      } else if (s->kind == StmtKind::Return) {
        int rs = allocSlot();
        emitRawExpr(static_cast<const ReturnStmt&>(*s).value.get(), rs);
        loadSlot(rs);
        if (!keepRaw) {
          emitBoxInt64();
          rawInvAll();
        }
      }
    }
    rawVars_ = std::move(savedRawVars);
  }

  // Rebuild a function-like scope for the callee so caller locals/params can
  // never be captured by name lookup. Returns the saved scope stack in
  // `saved`; caller restores it after emission.
  void beginInlineScope(FnInfo* f,
                        std::vector<std::unordered_map<std::string, ABinding>>& saved) {
    saved = std::move(scopes_);
    scopes_.clear();
    if (!saved.empty()) scopes_.push_back(saved.front());
    else scopes_.emplace_back();
    pushScope();
    bindModuleImports(f->moduleId);
  }

  void endInlineScope(
      std::vector<std::unordered_map<std::string, ABinding>>& saved) {
    scopes_ = std::move(saved);
  }

  bool callArgsAllInt(const CallExpr& call) const {
    for (const CallArg& a : call.args) {
      TypePtr t = resolveType(a.value ? a.value->inferredType : nullptr);
      if (!t || t->kind != TypeKind::Int) return false;
    }
    return true;
  }

  // ---- Phase 5b: int64 range analysis --------------------------------------
  // Dataflow that computes nameRanges_ (union of each local's interval over
  // all program points). Straight-line code is precise; control-flow merges
  // union intervals; loops iterate to a fixpoint (monotone union, capped —
  // non-converging loop-carried variables are poisoned to unknown). Anything
  // the walk cannot model (++/--, defer bodies, lambda captures, destructure,
  // non-range for-vars, globals) yields unknown, so proofs only ever fire on
  // genuinely bounded values.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
  static __int128 rngI64Min() { return -(((__int128)1) << 63); }
  static __int128 rngI64Max() { return (((__int128)1) << 63) - 1; }

  static ARange rngSpan(__int128 lo, __int128 hi) {
    const __int128 kMin = rngI64Min(), kMax = rngI64Max();
    if (lo < kMin) lo = kMin;
    if (hi > kMax) hi = kMax;
    if (lo > kMax) lo = kMax;
    if (hi < kMin) hi = kMin;
    ARange r;
    r.known = true;
    if (lo > hi) {  // empty intersection: keep it sound with the full domain
      r.lo = kMin;
      r.hi = kMax;
    } else {
      r.lo = lo;
      r.hi = hi;
    }
    return r;
  }

  static void rngJoin(ARange& a, const ARange& b) {
    // Unknown is the top element ("could be anything"): joining any known
    // range with an unknown one must stay unknown. The previous order-
    // dependent behaviour could drop earlier known bounds and let overflow
    // proofs fire on incomplete intervals.
    if (!a.known || !b.known) {
      a.known = false;
      return;
    }
    if (b.lo < a.lo) a.lo = b.lo;
    if (b.hi > a.hi) a.hi = b.hi;
  }

  static bool rngIsConst(const ARange& r, __int128 v) {
    return r.known && r.lo == v && r.hi == v;
  }

  // Overflow proof shared by rawSafeExpr and rangeBinOp: the mathematical
  // result of a+b / a-b / a*b must fit int64 for EVERY value in the operand
  // intervals (interval arithmetic is exact for these bilinear ops, so the
  // corners decide).
  static bool arithFits(const ARange& a, const ARange& b,
                        const std::string& op) {
    if (!a.known || !b.known) return false;
    const __int128 kMin = rngI64Min(), kMax = rngI64Max();
    if (op == "+") return a.lo + b.lo >= kMin && a.hi + b.hi <= kMax;
    if (op == "-") return a.lo - b.hi >= kMin && a.hi - b.lo <= kMax;
    __int128 p1 = a.lo * b.lo, p2 = a.lo * b.hi;
    __int128 p3 = a.hi * b.lo, p4 = a.hi * b.hi;
    __int128 mn = p1 < p2 ? p1 : p2;
    if (p3 < mn) mn = p3;
    if (p4 < mn) mn = p4;
    __int128 mx = p1 > p2 ? p1 : p2;
    if (p3 > mx) mx = p3;
    if (p4 > mx) mx = p4;
    return mn >= kMin && mx <= kMax;
  }

  // Interval of a binary op's int64 result, or unknown. Only ops that always
  // yield int64-or-panic are modeled (mirrors rawSafeBinOp plus the proven
  // arith ops); anything else is unknown.
  ARange rangeBinOp(const std::string& op, const ARange& l,
                    const ARange& r) const {
    if (op == "+" || op == "-" || op == "*") {
      if (!arithFits(l, r, op)) return ARange{};
      if (op == "+") return rngSpan(l.lo + r.lo, l.hi + r.hi);
      if (op == "-") return rngSpan(l.lo - r.hi, l.hi - r.lo);
      __int128 ps[4] = {l.lo * r.lo, l.lo * r.hi, l.hi * r.lo, l.hi * r.hi};
      __int128 mn = ps[0], mx = ps[0];
      for (int i = 1; i < 4; ++i) {
        if (ps[i] < mn) mn = ps[i];
        if (ps[i] > mx) mx = ps[i];
      }
      return rngSpan(mn, mx);
    }
    if (op == "%") {
      // |x % d| < |d|; bounded whenever the divisor's magnitude is.
      if (!r.known) return ARange{};
      __int128 am = r.lo < 0 ? -r.lo : r.lo;
      __int128 ah = r.hi < 0 ? -r.hi : r.hi;
      if (ah > am) am = ah;
      if (am <= 0) return ARange{};  // always panics; no value produced
      return rngSpan(-(am - 1), am - 1);
    }
    if (op == "&") {
      // x & C with C >= 0 yields [0, C] regardless of x (bits subset of C).
      if (r.known && r.lo == r.hi && r.lo >= 0) return rngSpan(0, r.hi);
      if (l.known && l.lo == l.hi && l.lo >= 0) return rngSpan(0, l.hi);
      if (l.known && r.known && l.lo >= 0 && r.lo >= 0)
        return rngSpan(0, l.hi < r.hi ? l.hi : r.hi);
      return ARange{};
    }
    if (op == "|") {
      if (rngIsConst(r, 0)) return l;  // x | 0 == x
      if (rngIsConst(l, 0)) return r;
      if (l.known && r.known && l.lo >= 0 && r.lo >= 0)
        return rngSpan(l.lo > r.lo ? l.lo : r.lo, l.hi + r.hi);
      return ARange{};
    }
    if (op == "^") {
      if (l.known && r.known && l.lo == l.hi && r.lo == r.hi)
        return rngSpan(l.lo ^ r.lo, l.lo ^ r.lo);
      if (l.known && r.known && l.lo >= 0 && r.lo >= 0)
        return rngSpan(0, l.hi + r.hi);
      return ARange{};
    }
    if (op == "<<" || op == ">>") {
      // Both wrap/stay in int64 (shl is mod 2^64, sar stays int64; out-of-range
      // amounts panic), so a tight range only when no wrap/trap is possible.
      if (!l.known || !r.known) return ARange{};
      if (op == "<<") {
        if (l.lo >= 0 && r.lo >= 0 && r.hi <= 62) {
          __int128 mx = l.hi << (int)r.hi;
          if (mx <= rngI64Max())
            return rngSpan(l.lo << (int)r.lo, mx);
        }
        return ARange{};
      }
      // sar: monotone in operand; extremes at the shift-amount corners.
      __int128 slo = r.lo < 0 ? 0 : r.lo;
      __int128 shi = r.hi > 63 ? 63 : r.hi;
      if (slo > shi) return ARange{};  // every amount panics
      __int128 c[4] = {l.lo >> (int)shi, l.hi >> (int)shi,
                       l.lo >> (int)slo, l.hi >> (int)slo};
      __int128 mn = c[0], mx = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < mn) mn = c[i];
        if (c[i] > mx) mx = c[i];
      }
      return rngSpan(mn, mx);
    }
    return ARange{};
  }

  // Interval of an expression's int64 value under lookup `lk` (name -> range).
  // Only exprs whose evaluation provably yields int64 (or panics) get a known
  // range; everything else is unknown.
  template <typename Lookup>
  ARange exprRangeIn(const Expr* e, Lookup&& lk) const {
    if (!e) return ARange{};
    switch (e->kind) {
      case ExprKind::IntLit: {
        __int128 v = static_cast<const IntLitExpr&>(*e).value;
        return rngSpan(v, v);
      }
      case ExprKind::Ident:
        return lk(static_cast<const IdentExpr&>(*e).name);
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if (u.op != "~" && !(u.op == "-" && !u.postfix)) return ARange{};
        ARange o = exprRangeIn(u.operand.get(), lk);
        if (!o.known) return ARange{};
        if (u.op == "~") return rngSpan(-o.hi - 1, -o.lo - 1);
        if (o.lo > rngI64Min()) return rngSpan(-o.hi, -o.lo);
        return ARange{};  // -INT64_MIN overflows to a double
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        return rangeBinOp(b.op, exprRangeIn(b.lhs.get(), lk),
                          exprRangeIn(b.rhs.get(), lk));
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        ARange a = exprRangeIn(t.thenValue.get(), lk);
        ARange b = exprRangeIn(t.elseValue.get(), lk);
        rngJoin(a, b);
        return a;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        // len(x) is always a non-negative int.
        if (c.builtinId == BuiltinId::Len && c.args.size() == 1)
          return rngSpan(0, rngI64Max());
        return ARange{};
      }
      default:
        return ARange{};
    }
  }

  // Emit-time/proof-time lookup: the union range recorded for a name.
  ARange exprRangeFlat(const Expr* e) const {
    return exprRangeIn(e, [this](const std::string& n) -> ARange {
      auto it = nameRanges_.find(n);
      return it == nameRanges_.end() ? ARange{} : it->second;
    });
  }

  // Overflow proofs for in-place updates (`x++`, `x--`, `x += v`, ...).
  // nameRanges_[nm] is the union of every interval nm holds at any program
  // point, so "union range <op> value range fits int64" proves the update
  // can never produce the double aval that overflow yields — the same
  // contract rawSafeExpr's arithFits gate relies on.
  bool nameRangeKnown(const std::string& nm, ARange& out) const {
    auto it = nameRanges_.find(nm);
    if (it == nameRanges_.end() || !it->second.known) return false;
    out = it->second;
    return true;
  }

  bool incDecProven(const std::string& nm, bool isInc) const {
    ARange r;
    if (!nameRangeKnown(nm, r)) return false;
    return arithFits(r, rngSpan(1, 1), isInc ? "+" : "-");
  }

  // `nm <op>= v` stays int64: structural for ops that cannot overflow
  // (& | ^ % << >> on int64 always yield int64-or-panic, and emitRawBin
  // reproduces the panic paths), range-proven for + - *.
  bool assignOpRawOk(const std::string& nm, const std::string& base,
                     const Expr* v) {
    if (!v || !exprIsInt(v) || !rawSafeExpr(v)) return false;
    if (base == "&" || base == "|" || base == "^" || base == "%" ||
        base == "<<" || base == ">>")
      return true;
    if (base != "+" && base != "-" && base != "*") return false;
    ARange r;
    if (!nameRangeKnown(nm, r)) return false;
    return arithFits(r, exprRangeFlat(v), base);
  }

  // Write-side gate for the speculative accumulator (mirrors
  // emitSpecAssign's classification): an int-typed write whose int64
  // result MAY overflow is still speculatable — emission keeps the slot
  // raw and deopts to the aval path on `jo`. Covers `v <op>= e` and
  // `v = L <op> R` for op in {+,-,*} with raw-safe int operands; the
  // `=` form only counts when the top-level arith is unproven (proven
  // writes already took the rawWrite branch above).
  bool specWriteOk(const AssignStmt& st) {
    if (!st.value) return false;
    if (st.op.size() > 1 && st.op.back() == '=') {
      const std::string base = st.op.substr(0, st.op.size() - 1);
      return (base == "+" || base == "-" || base == "*") &&
             exprIsInt(st.value.get()) && rawSafeExpr(st.value.get());
    }
    if (st.op == "=" && st.value->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*st.value);
      return (b.op == "+" || b.op == "-" || b.op == "*") &&
             exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
             rawSafeExpr(b.lhs.get()) && rawSafeExpr(b.rhs.get());
    }
    return false;
  }

  // ---- range dataflow -------------------------------------------------------
  void rangeJoinInto(ARangeEnv& dst, const ARangeEnv& src) {
    if (dst.scopes.size() != src.scopes.size()) return;  // caller bug; stay safe
    for (std::size_t i = 0; i < src.scopes.size(); ++i) {
      for (const auto& kv : src.scopes[i]) {
        auto f = dst.scopes[i].find(kv.first);
        if (f == dst.scopes[i].end())
          dst.scopes[i][kv.first] = kv.second;
        else
          rngJoin(f->second, kv.second);
      }
    }
  }

  ARangeEnv rangeTruncate(const ARangeEnv& env, int depth) const {
    ARangeEnv t;
    t.scopes.assign(env.scopes.begin(),
                    env.scopes.begin() + std::min<int>(depth, env.scopes.size()));
    return t;
  }

  bool rangeEnvEq(const ARangeEnv& a, const ARangeEnv& b) const {
    if (a.scopes.size() != b.scopes.size()) return false;
    for (std::size_t i = 0; i < a.scopes.size(); ++i) {
      if (a.scopes[i].size() != b.scopes[i].size()) return false;
      for (const auto& kv : a.scopes[i]) {
        auto f = b.scopes[i].find(kv.first);
        if (f == b.scopes[i].end()) return false;
        const ARange& x = kv.second;
        const ARange& y = f->second;
        if (x.known != y.known || x.lo != y.lo || x.hi != y.hi) return false;
      }
    }
    return true;
  }

  void rangeRecord(const ARangeEnv& env) {
    if (rangeFreeze_ > 0 || rangeAbort_) return;
    for (const auto& sc : env.scopes)
      for (const auto& kv : sc) rngJoin(nameRanges_[kv.first], kv.second);
  }

  // Expr side effects that can change a local's value outside the stmt-level
  // writes: ++/-- targets and names captured by lambdas (a stored lambda may
  // run later). Everything else in an expr is range-neutral.
  void rangePoisonExpr(const Expr* e, ARangeEnv& env) {
    if (!e || rangeAbort_) return;
    switch (e->kind) {
      case ExprKind::Ident:
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return;
      case ExprKind::StringLit:
        for (const StringPart& p : static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) rangePoisonExpr(p.expr.get(), env);
        return;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if ((u.op == "++" || u.op == "--") &&
            u.operand->kind == ExprKind::Ident)
          env.setUnknown(static_cast<const IdentExpr&>(*u.operand).name);
        rangePoisonExpr(u.operand.get(), env);
        return;
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        rangePoisonExpr(b.lhs.get(), env);
        rangePoisonExpr(b.rhs.get(), env);
        return;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        rangePoisonExpr(c.callee.get(), env);
        for (const CallArg& a : c.args) rangePoisonExpr(a.value.get(), env);
        return;
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        rangePoisonExpr(x.object.get(), env);
        rangePoisonExpr(x.index.get(), env);
        return;
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        rangePoisonExpr(x.object.get(), env);
        rangePoisonExpr(x.start.get(), env);
        rangePoisonExpr(x.end.get(), env);
        rangePoisonExpr(x.step.get(), env);
        return;
      }
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr&>(*e).entries) {
          rangePoisonExpr(kv.first.get(), env);
          rangePoisonExpr(kv.second.get(), env);
        }
        return;
      case ExprKind::SetLit:
      case ExprKind::ArrayLit:
      case ExprKind::Tuple: {
        const auto& els = e->kind == ExprKind::SetLit
            ? static_cast<const SetLitExpr&>(*e).elements
            : e->kind == ExprKind::ArrayLit
                ? static_cast<const ArrayLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els) rangePoisonExpr(el.get(), env);
        return;
      }
      case ExprKind::Field:
        rangePoisonExpr(static_cast<const FieldExpr&>(*e).object.get(), env);
        return;
      case ExprKind::SafeNav:
        rangePoisonExpr(static_cast<const SafeNavExpr&>(*e).object.get(), env);
        return;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        rangePoisonExpr(r.start.get(), env);
        rangePoisonExpr(r.end.get(), env);
        rangePoisonExpr(r.step.get(), env);
        return;
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        rangePoisonExpr(t.cond.get(), env);
        rangePoisonExpr(t.thenValue.get(), env);
        rangePoisonExpr(t.elseValue.get(), env);
        return;
      }
      case ExprKind::Lambda: {
        const LambdaExpr& l = static_cast<const LambdaExpr&>(*e);
        for (const ParamDecl& p : l.params) env.setUnknown(p.name);
        std::set<std::string> ids;
        collectIdents(l.body.get(), ids);
        for (const std::string& nm : ids) env.setUnknown(nm);
        return;
      }
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        rangePoisonExpr(c.element.get(), env);
        rangePoisonExpr(c.iterable.get(), env);
        rangePoisonExpr(c.condition.get(), env);
        return;
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        rangePoisonExpr(m.subject.get(), env);
        for (const MatchArm& arm : m.arms) {
          rangePoisonPattern(arm.pattern, env);
          rangePoisonExpr(arm.guard.get(), env);
          rangePoisonExpr(arm.result.get(), env);
        }
        return;
      }
      case ExprKind::Try:
        rangePoisonExpr(static_cast<const TryExpr&>(*e).operand.get(), env);
        return;
      default:
        return;
    }
  }

  void rangePoisonPattern(const Pattern& p, ARangeEnv& env) {
    if (p.value) rangePoisonExpr(p.value.get(), env);
    if (p.rangeStart) rangePoisonExpr(p.rangeStart.get(), env);
    if (p.rangeEnd) rangePoisonExpr(p.rangeEnd.get(), env);
    if (p.ctor) rangePoisonExpr(p.ctor.get(), env);
    for (const Pattern& sub : p.subpatterns) rangePoisonPattern(sub, env);
  }

  // A `defer` body runs at scope exit, not at its lexical position: any name
  // it assigns must be unknown for everything after the defer statement.
  void rangePoisonDefer(const Stmt* s, ARangeEnv& env) {
    if (!s || rangeAbort_) return;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        rangePoisonExpr(st.init.get(), env);  // let is defer-local: no write
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind == ExprKind::Ident)
          env.setUnknown(static_cast<const IdentExpr&>(*st.target).name);
        else
          rangePoisonExpr(st.target.get(), env);
        rangePoisonExpr(st.value.get(), env);
        return;
      }
      case StmtKind::Expr:
        rangePoisonExpr(static_cast<const ExprStmt&>(*s).expr.get(), env);
        return;
      case StmtKind::Destructure:
        rangePoisonExpr(static_cast<const DestructureStmt&>(*s).value.get(), env);
        return;
      case StmtKind::Return:
        rangePoisonExpr(static_cast<const ReturnStmt&>(*s).value.get(), env);
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches) {
          rangePoisonExpr(br.cond.get(), env);
          for (const StmtPtr& b : br.body) rangePoisonDefer(b.get(), env);
        }
        for (const StmtPtr& b : st.elseBody) rangePoisonDefer(b.get(), env);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        rangePoisonExpr(st.cond.get(), env);
        for (const StmtPtr& b : st.body) rangePoisonDefer(b.get(), env);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        for (const StmtPtr& b : st.body) rangePoisonDefer(b.get(), env);
        rangePoisonExpr(st.cond.get(), env);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        rangePoisonExpr(st.iterable.get(), env);
        for (const StmtPtr& b : st.body) rangePoisonDefer(b.get(), env);
        return;
      }
      case StmtKind::Loop:
        for (const StmtPtr& b : static_cast<const LoopStmt&>(*s).body)
          rangePoisonDefer(b.get(), env);
        return;
      case StmtKind::Defer:
        rangePoisonDefer(static_cast<const DeferStmt&>(*s).body.get(), env);
        return;
      default:
        return;
      }
    }

  // Names assigned anywhere inside a stmt list (recursive) — used to poison
  // non-converged loop-carried variables.
  void rangeCollectAssigns(const Stmt* s, std::unordered_set<std::string>& out) {
    if (!s) return;
    switch (s->kind) {
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind == ExprKind::Ident)
          out.insert(static_cast<const IdentExpr&>(*st.target).name);
        return;
      }
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        out.insert(st.name);  // conservative: may shadow an outer binding
        return;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        for (const std::string& n : st.names)
          if (n != "_") out.insert(n);
        return;
      }
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches)
          for (const StmtPtr& b : br.body) rangeCollectAssigns(b.get(), out);
        for (const StmtPtr& b : st.elseBody) rangeCollectAssigns(b.get(), out);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        for (const StmtPtr& b : st.body) rangeCollectAssigns(b.get(), out);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        for (const StmtPtr& b : st.body) rangeCollectAssigns(b.get(), out);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        out.insert(st.var);
        for (const StmtPtr& b : st.body) rangeCollectAssigns(b.get(), out);
        return;
      }
      case StmtKind::Loop:
        for (const StmtPtr& b : static_cast<const LoopStmt&>(*s).body)
          rangeCollectAssigns(b.get(), out);
        return;
      case StmtKind::Defer:
        rangeCollectAssigns(static_cast<const DeferStmt&>(*s).body.get(), out);
        return;
      default:
        return;
      }
    }

  void rangeBlock(const std::vector<StmtPtr>& stmts, ARangeEnv& env) {
    env.scopes.emplace_back();
    for (const StmtPtr& s : stmts) {
      if (failed_ || rangeAbort_) break;
      if (--rangeBudget_ < 0) {
        rangeAbort_ = true;
        break;
      }
      rangeStmt(s.get(), env);
      rangeRecord(env);
    }
    env.scopes.pop_back();
  }

  // Loop fixpoint: `head` is the env at the loop head (includes the for-var
  // scope for counted/iterable fors). Iterates the body until ranges
  // stabilize; continue/break exits are accumulated into cont/brk and joined
  // into the head/post envs so mid-body states are never lost. Non-converged
  // variables are poisoned to unknown before the recording pass.
  void rangeLoop(const std::vector<StmtPtr>& body, const std::string* varName,
                 const ARange* varRange, ARangeEnv& env) {
    static constexpr int kMaxRangeIter = 8;
    const int postDepth = (int)env.scopes.size();
    if (varName) {
      env.scopes.emplace_back();
      env.scopes.back()[*varName] = varRange ? *varRange : ARange{};
    }
    const int headDepth = (int)env.scopes.size();
    ARangeEnv head = env;
    ARangeEnv cont, brk;
    bool hasCont = false, hasBrk = false;
    rangeLoops_.push_back({&head, &cont, &brk, headDepth, postDepth,
                           &hasCont, &hasBrk});
    // Prescan: poison ++/-- targets and lambda captures once up front so the
    // fixpoint starts from the same env every iteration would produce.
    for (const StmtPtr& s : body) rangePrescanStmt(s.get(), head);
    // Fixpoint with widening: head grows monotonically (joins only union), so
    // it either stabilizes or some variable's interval keeps expanding. On
    // non-convergence, poison the variables that moved in the last step and
    // re-run — unknown is the top element, so each round strictly reduces the
    // known set and the cascade of dependent unknowns propagates. A final
    // fallback poisons every assigned name.
    bool converged = false;
    ++rangeFreeze_;
    for (int round = 0; round < 6 && !converged && !rangeAbort_; ++round) {
      ARangeEnv prev;
      for (int it = 0; it < kMaxRangeIter && !rangeAbort_; ++it) {
        prev = head;
        ARangeEnv bodyEnv = head;
        rangeBlock(body, bodyEnv);
        rangeJoinInto(head, bodyEnv);
        if (hasCont) rangeJoinInto(head, cont);
        if (varName && varRange)
          head.scopes[postDepth][*varName] = *varRange;  // rebound each iter
        if (rangeEnvEq(head, prev)) {
          converged = true;
          break;
        }
      }
      if (converged || rangeAbort_) break;
      // Widen: poison the names whose interval moved in the last step, then
      // re-run so dependents collapse to unknown too. Unknown is top, so each
      // round strictly shrinks the known set and this terminates.
      bool any = false;
      for (std::size_t i = 0; i < head.scopes.size() && i < prev.scopes.size();
           ++i) {
        for (const auto& kv : head.scopes[i]) {
          auto f = prev.scopes[i].find(kv.first);
          const bool moved =
              f == prev.scopes[i].end() || f->second.known != kv.second.known ||
              f->second.lo != kv.second.lo || f->second.hi != kv.second.hi;
          if (moved && kv.second.known) {
            head.scopes[i][kv.first] = ARange{};
            any = true;
          }
        }
      }
      if (varName && varRange)
        head.scopes[postDepth][*varName] = *varRange;
      if (!any) converged = true;  // nothing left that can still move
    }
    if (!converged && !rangeAbort_) {
      // Last resort: poison every name assigned in the body, then propagate
      // once more so dependents collapse. Reads of unassigned names keep
      // their (input) ranges, which are unaffected by the loop.
      std::unordered_set<std::string> writes;
      for (const StmtPtr& s : body) rangeCollectAssigns(s.get(), writes);
      for (const std::string& nm : writes) head.setUnknown(nm);
      if (varName && varRange)
        head.scopes[postDepth][*varName] = *varRange;
      for (int it = 0; it < kMaxRangeIter && !rangeAbort_; ++it) {
        ARangeEnv prev = head;
        ARangeEnv bodyEnv = head;
        rangeBlock(body, bodyEnv);
        rangeJoinInto(head, bodyEnv);
        if (hasCont) rangeJoinInto(head, cont);
        if (varName && varRange)
          head.scopes[postDepth][*varName] = *varRange;
        if (rangeEnvEq(head, prev)) break;
      }
    }
    --rangeFreeze_;
    rangeLoops_.pop_back();
    if (rangeAbort_) {
      env.scopes.clear();
      return;
    }
    rangeRecord(head);
    // Recording pass: process the body once more with the converged head so
    // acc sees the states reads inside the loop actually observe.
    {
      ARangeEnv fin = head;
      rangeBlock(body, fin);
    }
    // Post-loop env: head states (truncated to pre-loop depth) joined with
    // break exits. The for-var scope is dropped.
    ARangeEnv post = rangeTruncate(head, postDepth);
    if (hasBrk) rangeJoinInto(post, brk);
    env.scopes = std::move(post.scopes);
  }

  // Prescan equivalent of rangePoisonExpr over a stmt list (no env writes,
  // only poison side effects on `env`).
  void rangePrescanStmt(const Stmt* s, ARangeEnv& env) {
    if (!s || rangeAbort_) return;
    switch (s->kind) {
      case StmtKind::Let:
        rangePoisonExpr(static_cast<const LetStmt&>(*s).init.get(), env);
        return;
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        rangePoisonExpr(st.target.get(), env);
        rangePoisonExpr(st.value.get(), env);
        return;
      }
      case StmtKind::Expr:
        rangePoisonExpr(static_cast<const ExprStmt&>(*s).expr.get(), env);
        return;
      case StmtKind::Destructure:
        rangePoisonExpr(static_cast<const DestructureStmt&>(*s).value.get(), env);
        return;
      case StmtKind::Return:
        rangePoisonExpr(static_cast<const ReturnStmt&>(*s).value.get(), env);
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches) {
          rangePoisonExpr(br.cond.get(), env);
          for (const StmtPtr& b : br.body) rangePrescanStmt(b.get(), env);
        }
        for (const StmtPtr& b : st.elseBody) rangePrescanStmt(b.get(), env);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        rangePoisonExpr(st.cond.get(), env);
        for (const StmtPtr& b : st.body) rangePrescanStmt(b.get(), env);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        for (const StmtPtr& b : st.body) rangePrescanStmt(b.get(), env);
        rangePoisonExpr(st.cond.get(), env);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        rangePoisonExpr(st.iterable.get(), env);
        for (const StmtPtr& b : st.body) rangePrescanStmt(b.get(), env);
        return;
      }
      case StmtKind::Loop:
        for (const StmtPtr& b : static_cast<const LoopStmt&>(*s).body)
          rangePrescanStmt(b.get(), env);
        return;
      case StmtKind::Defer:
        rangePoisonDefer(static_cast<const DeferStmt&>(*s).body.get(), env);
        return;
      default:
        return;
      }
    }

  // `x++`/`x--`/`++x`/`--x` as a whole statement expr: model as x = x +/- 1.
  // rangeBinOp yields unknown whenever the increment can overflow int64
  // (the runtime would then produce a double aval), so the recorded range
  // stays sound and incDecProven only fires on genuinely bounded values.
  // Nested uses (f(x++), y = x++ + 1) still go through rangePoisonExpr.
  bool rangeIncDec(const Expr* e, ARangeEnv& env) {
    if (!e || e->kind != ExprKind::Unary) return false;
    const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
    if ((u.op != "++" && u.op != "--") || !u.operand ||
        u.operand->kind != ExprKind::Ident)
      return false;
    const std::string& nm = static_cast<const IdentExpr&>(*u.operand).name;
    env.assign(nm, rangeBinOp(u.op == "++" ? "+" : "-",
                              env.find(nm), rngSpan(1, 1)));
    return true;
  }

  void rangeStmt(const Stmt* s, ARangeEnv& env) {
    if (!s || rangeAbort_) return;
    auto lk = [&env](const std::string& n) -> ARange { return env.find(n); };
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        rangePoisonExpr(st.init.get(), env);
        ARange r = st.init ? exprRangeIn(st.init.get(), lk) : ARange{};
        env.bindTop(st.name, r);
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        rangePoisonExpr(st.value.get(), env);
        if (st.target->kind == ExprKind::Ident) {
          const std::string& nm =
              static_cast<const IdentExpr&>(*st.target).name;
          ARange r;
          if (st.op == "=") {
            r = exprRangeIn(st.value.get(), lk);
          } else {
            // x <op>= v  ==  x = x <op> v; unknown when unprovable (the
            // write may produce a double aval).
            std::string base = st.op.substr(0, st.op.size() - 1);
            r = rangeBinOp(base, env.find(nm), exprRangeIn(st.value.get(), lk));
          }
          env.assign(nm, r);
        } else {
          rangePoisonExpr(st.target.get(), env);
        }
        return;
      }
      case StmtKind::Expr: {
        const Expr* ex = static_cast<const ExprStmt&>(*s).expr.get();
        if (!rangeIncDec(ex, env)) rangePoisonExpr(ex, env);
        return;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        rangePoisonExpr(st.value.get(), env);
        for (const std::string& n : st.names)
          if (n != "_") env.bindTop(n, ARange{});
        return;
      }
      case StmtKind::Return:
        rangePoisonExpr(static_cast<const ReturnStmt&>(*s).value.get(), env);
        return;
      case StmtKind::Break: {
        if (!rangeLoops_.empty()) {
          RangeLoopCtx& c = rangeLoops_.back();
          ARangeEnv t = rangeTruncate(env, c.postDepth);
          if (!*c.hasBrk) {
            *c.brk = t;
            *c.hasBrk = true;
          } else {
            rangeJoinInto(*c.brk, t);
          }
        }
        return;
      }
      case StmtKind::Continue: {
        if (!rangeLoops_.empty()) {
          RangeLoopCtx& c = rangeLoops_.back();
          ARangeEnv t = rangeTruncate(env, c.headDepth);
          if (!*c.hasCont) {
            *c.cont = t;
            *c.hasCont = true;
          } else {
            rangeJoinInto(*c.cont, t);
          }
        }
        return;
      }
      case StmtKind::Defer:
        rangePoisonDefer(static_cast<const DeferStmt&>(*s).body.get(), env);
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches)
          rangePoisonExpr(br.cond.get(), env);
        ARangeEnv join = env;
        for (const IfStmt::Branch& br : st.branches) {
          ARangeEnv eb = env;
          rangeBlock(br.body, eb);
          rangeJoinInto(join, eb);
        }
        if (!st.elseBody.empty()) {
          ARangeEnv ee = env;
          rangeBlock(st.elseBody, ee);
          rangeJoinInto(join, ee);
        }
        env.scopes = std::move(join.scopes);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        rangePoisonExpr(st.cond.get(), env);
        rangeLoop(st.body, nullptr, nullptr, env);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        rangePoisonExpr(st.cond.get(), env);
        rangeLoop(st.body, nullptr, nullptr, env);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        rangePoisonExpr(st.iterable.get(), env);
        // Counted range `lo..hi[..step]` (RangeExpr or `..` binary): the var
        // takes values in [lo, hi) for positive step, (hi, lo] for negative.
        const Expr* loE = nullptr;
        const Expr* hiE = nullptr;
        const Expr* stE = nullptr;
        const Expr* it = st.iterable.get();
        if (it && it->kind == ExprKind::Range) {
          const RangeExpr& re = static_cast<const RangeExpr&>(*it);
          loE = re.start.get();
          hiE = re.end.get();
          stE = re.step.get();
        } else if (it && it->kind == ExprKind::Binary &&
                   static_cast<const BinaryExpr&>(*it).op == "..") {
          const BinaryExpr& be = static_cast<const BinaryExpr&>(*it);
          loE = be.lhs.get();
          hiE = be.rhs.get();
        }
        ARange vr;
        bool haveVar = false;
        if (loE && hiE) {
          ARange lo = exprRangeIn(loE, lk);
          ARange hi = exprRangeIn(hiE, lk);
          ARange stp = stE ? exprRangeIn(stE, lk) : rngSpan(1, 1);
          if (lo.known && hi.known && stp.known) {
            if (stp.lo > 0) {
              __int128 h = hi.hi - 1;
              if (h < lo.lo) h = lo.lo;  // empty loop: any range is sound
              vr = rngSpan(lo.lo, h);
              haveVar = true;
            } else if (stp.hi < 0) {
              __int128 l = hi.lo + 1;
              if (l > lo.hi) l = lo.hi;
              vr = rngSpan(l, lo.hi);
              haveVar = true;
            }
          }
        }
        rangeLoop(st.body, &st.var, haveVar ? &vr : nullptr, env);
        return;
      }
      case StmtKind::Loop: {
        const LoopStmt& st = static_cast<const LoopStmt&>(*s);
        rangeLoop(st.body, nullptr, nullptr, env);
        return;
      }
      default:
        return;  // decls/imports are not executed as body statements
      }
    }

  void computeNameRanges(const FnDecl* fn) {
    nameRanges_.clear();
    rangeAbort_ = false;
    rangeFreeze_ = 0;
    rangeLoops_.clear();
    rangeBudget_ = 60000;
    if (!fn) return;
    ARangeEnv env;
    env.scopes.emplace_back();
    for (const ParamDecl& p : fn->params) env.scopes.back()[p.name] = ARange{};
    rangeBlock(fn->body, env);
    if (rangeAbort_) nameRanges_.clear();  // incomplete walk: prove nothing
  }
#pragma GCC diagnostic pop

  // ---- Phase 5: scalar-promotion analysis ----------------------------------
  // A local may keep a raw int64 in its frame slot only when every write
  // produces an int64 via rawSafeExpr and every read is consumed inside a
  // raw region (safe-op binary, raw-inlined call arg, or promoted
  // let/assign). Reads outside those regions are still *correct* — they box
  // on demand — but each box of a large int is a heap allocation, so the
  // analysis disqualifies names with uncovered reads to avoid regressions.
  // Emission re-checks every condition, so a wrong inclusion is only ever a
  // missed optimization or a demotion, never a miscompile.
  struct PromoteScan {
    std::unordered_set<std::string> safe;       // all writes raw-safe + int
    std::unordered_set<std::string> bad;        // disqualified names
    std::unordered_set<std::string> pending;    // safe|spec - bad
    std::unordered_set<std::string> uncovered;  // reads outside raw regions
    std::unordered_set<std::string> spec;       // >=1 overflow-unproven
                                                 // +,-,* write (deopt-able)
    std::unordered_set<std::string> noSpec;     // ++/-- targets, for-vars:
                                                 // speculation excluded
  };

  // Walk every expr child (mirrors collectIdents) marking names that must
  // not be promoted: ++/-- targets (they store avals) and anything captured
  // by a lambda (captures snapshot avals).
  void promoteMarkExpr(const Expr* e, PromoteScan& ps) {
    if (!e) return;
    switch (e->kind) {
      case ExprKind::Ident:
        return;
      case ExprKind::StringLit:
        for (const StringPart& p : static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) promoteMarkExpr(p.expr.get(), ps);
        return;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        if ((u.op == "++" || u.op == "--") && u.operand &&
            u.operand->kind == ExprKind::Ident) {
          // ++/-- stores through the raw int64 path only when the range
          // proof shows the increment cannot overflow (emitExpr mirrors
          // this check); otherwise the target is disqualified as before.
          const std::string& nm =
              static_cast<const IdentExpr&>(*u.operand).name;
          if (!incDecProven(nm, u.op == "++")) ps.bad.insert(nm);
          // ++/-- updates the raw slot in place with no deopt point, so a
          // name touched by one can never use the speculative accumulator.
          ps.noSpec.insert(nm);
        }
        promoteMarkExpr(u.operand.get(), ps);
        return;
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        promoteMarkExpr(b.lhs.get(), ps);
        promoteMarkExpr(b.rhs.get(), ps);
        return;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        promoteMarkExpr(c.callee.get(), ps);
        for (const CallArg& a : c.args) promoteMarkExpr(a.value.get(), ps);
        return;
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        promoteMarkExpr(x.object.get(), ps);
        promoteMarkExpr(x.index.get(), ps);
        return;
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        promoteMarkExpr(x.object.get(), ps);
        promoteMarkExpr(x.start.get(), ps);
        promoteMarkExpr(x.end.get(), ps);
        promoteMarkExpr(x.step.get(), ps);
        return;
      }
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr&>(*e).entries) {
          promoteMarkExpr(kv.first.get(), ps);
          promoteMarkExpr(kv.second.get(), ps);
        }
        return;
      case ExprKind::SetLit:
      case ExprKind::ArrayLit:
      case ExprKind::Tuple: {
        const auto& els = e->kind == ExprKind::SetLit
            ? static_cast<const SetLitExpr&>(*e).elements
            : e->kind == ExprKind::ArrayLit
                ? static_cast<const ArrayLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els) promoteMarkExpr(el.get(), ps);
        return;
      }
      case ExprKind::Field:
        promoteMarkExpr(static_cast<const FieldExpr&>(*e).object.get(), ps);
        return;
      case ExprKind::SafeNav:
        promoteMarkExpr(static_cast<const SafeNavExpr&>(*e).object.get(), ps);
        return;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        promoteMarkExpr(r.start.get(), ps);
        promoteMarkExpr(r.end.get(), ps);
        promoteMarkExpr(r.step.get(), ps);
        return;
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        promoteMarkExpr(t.cond.get(), ps);
        promoteMarkExpr(t.thenValue.get(), ps);
        promoteMarkExpr(t.elseValue.get(), ps);
        return;
      }
      case ExprKind::Lambda: {
        std::set<std::string> ids;
        collectIdents(static_cast<const LambdaExpr&>(*e).body.get(), ids);
        for (const std::string& nm : ids) ps.bad.insert(nm);
        return;
      }
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        promoteMarkExpr(c.element.get(), ps);
        promoteMarkExpr(c.iterable.get(), ps);
        promoteMarkExpr(c.condition.get(), ps);
        return;
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        promoteMarkExpr(m.subject.get(), ps);
        for (const MatchArm& arm : m.arms) {
          promoteMarkPattern(arm.pattern, ps);
          promoteMarkExpr(arm.guard.get(), ps);
          promoteMarkExpr(arm.result.get(), ps);
        }
        return;
      }
      case ExprKind::Try:
        promoteMarkExpr(static_cast<const TryExpr&>(*e).operand.get(), ps);
        return;
      default:
        return;
    }
  }

  void promoteMarkPattern(const Pattern& p, PromoteScan& ps) {
    if (p.value) promoteMarkExpr(p.value.get(), ps);
    if (p.rangeStart) promoteMarkExpr(p.rangeStart.get(), ps);
    if (p.rangeEnd) promoteMarkExpr(p.rangeEnd.get(), ps);
    if (p.ctor) promoteMarkExpr(p.ctor.get(), ps);
    for (const Pattern& sub : p.subpatterns) promoteMarkPattern(sub, ps);
  }

  // Write-side scan: record which names have only raw-safe int writes and
  // which are disqualified (++/--, lambda captures, compound assigns,
  // destructure/for bindings, non-int or non-raw initializers).
  void promoteWriteStmt(const Stmt* s, PromoteScan& ps) {
    if (!s) return;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        if (st.init && rawSafeExpr(st.init.get()) && exprIsInt(st.init.get()))
          ps.safe.insert(st.name);
        else
          ps.bad.insert(st.name);
        promoteMarkExpr(st.init.get(), ps);
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind == ExprKind::Ident) {
          const std::string& nm =
              static_cast<const IdentExpr&>(*st.target).name;
          bool rawWrite = false;
          if (st.op == "=" && st.value &&
              rawSafeExpr(st.value.get()) && exprIsInt(st.value.get())) {
            rawWrite = true;
          } else if (st.op.size() > 1 && st.op.back() == '=' && st.value) {
            // x <op>= v keeps a raw int64 slot when the op cannot overflow
            // (structural for bitwise/%/shifts, range-proven for + - *);
            // emitAssign mirrors this via assignOpRawOk.
            rawWrite = assignOpRawOk(nm, st.op.substr(0, st.op.size() - 1),
                                     st.value.get());
          }
          if (rawWrite) {
            ps.safe.insert(nm);
          } else if (specWriteOk(st)) {
            // Overflow-unproven +,-,* write: still int64-or-double, handled
            // at emission by the speculative raw accumulator + jo deopt.
            ps.spec.insert(nm);
          } else {
            ps.bad.insert(nm);
          }
        } else {
          promoteMarkExpr(st.target.get(), ps);
        }
        promoteMarkExpr(st.value.get(), ps);
        return;
      }
      case StmtKind::Expr:
        promoteMarkExpr(static_cast<const ExprStmt&>(*s).expr.get(), ps);
        return;
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        for (const std::string& nm : st.names)
          if (nm != "_") ps.bad.insert(nm);
        promoteMarkExpr(st.value.get(), ps);
        return;
      }
      case StmtKind::Return:
        promoteMarkExpr(static_cast<const ReturnStmt&>(*s).value.get(), ps);
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches) {
          promoteMarkExpr(br.cond.get(), ps);
          for (const StmtPtr& b : br.body) promoteWriteStmt(b.get(), ps);
        }
        for (const StmtPtr& b : st.elseBody) promoteWriteStmt(b.get(), ps);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        promoteMarkExpr(st.cond.get(), ps);
        for (const StmtPtr& b : st.body) promoteWriteStmt(b.get(), ps);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        for (const StmtPtr& b : st.body) promoteWriteStmt(b.get(), ps);
        promoteMarkExpr(st.cond.get(), ps);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        // Range loops (`for i in lo..hi[..step]`) bind the var as a raw
        // int64 each iteration (see emitFor), so the implicit write is a
        // safe int write like a promoted `let`; body writes are scanned
        // normally and any non-raw one disqualifies the name. Other
        // iterables still bind an aval per iteration.
        const Expr* it = st.iterable.get();
        const bool rawRange =
            it && (it->kind == ExprKind::Range ||
                   (it->kind == ExprKind::Binary &&
                    static_cast<const BinaryExpr&>(*it).op == ".."));
        if (rawRange) ps.safe.insert(st.var);
        else ps.bad.insert(st.var);
        // The per-iteration implicit write cannot check a demotion flag,
        // so for-vars never use the speculative accumulator.
        ps.noSpec.insert(st.var);
        promoteMarkExpr(st.iterable.get(), ps);
        for (const StmtPtr& b : st.body) promoteWriteStmt(b.get(), ps);
        return;
      }
      case StmtKind::Loop:
        for (const StmtPtr& b : static_cast<const LoopStmt&>(*s).body)
          promoteWriteStmt(b.get(), ps);
        return;
      case StmtKind::Defer:
        promoteWriteStmt(static_cast<const DeferStmt&>(*s).body.get(), ps);
        return;
      default:
        return;  // decls/imports are not emitted as body statements
    }
  }

  void promoteWriteStmts(const std::vector<StmtPtr>& stmts, PromoteScan& ps) {
    for (const StmtPtr& s : stmts) promoteWriteStmt(s.get(), ps);
  }

  // Condition positions: tryRawCondJump emits raw int comparisons, `not`,
  // and bare int exprs without boxing, so reads inside them stay raw.
  void promoteCoverCond(const Expr* e, PromoteScan& ps) {
    if (rawCondOk(e)) return;
    promoteCoverExpr(e, ps);
  }

  // Read-side scan: mark pending names whose reads are NOT inside a raw
  // region. Mirrors the emission rules: safe-op binaries with raw-safe int
  // operands emit fully raw (emitBinary fast path); calls to raw-inline
  // fns with raw-safe int args materialize args raw; everything else is an
  // aval position where a bare promoted ident would have to box.
  void promoteCoverExpr(const Expr* e, PromoteScan& ps) {
    if (!e) return;
    switch (e->kind) {
      case ExprKind::Ident: {
        const std::string& nm = static_cast<const IdentExpr&>(*e).name;
        if (promoteHot_ && ps.pending.count(nm)) ps.uncovered.insert(nm);
        return;
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        if (rawBinFastOk(b)) return;  // whole subtree stays raw
        promoteCoverExpr(b.lhs.get(), ps);
        promoteCoverExpr(b.rhs.get(), ps);
        return;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        promoteCoverExpr(c.callee.get(), ps);
        FnInfo* f = rawCallTarget(c);
        bool argsRaw = f != nullptr;
        if (argsRaw) {
          for (const CallArg& a : c.args)
            if (!rawSafeExpr(a.value.get())) {
              argsRaw = false;
              break;
            }
        }
        if (argsRaw) return;  // args materialize raw (or box on fallback)
        for (const CallArg& a : c.args) promoteCoverExpr(a.value.get(), ps);
        return;
      }
      case ExprKind::Lambda: {
        std::set<std::string> ids;
        collectIdents(static_cast<const LambdaExpr&>(*e).body.get(), ids);
        for (const std::string& nm : ids)
          if (promoteHot_ && ps.pending.count(nm)) ps.uncovered.insert(nm);
        return;
      }
      case ExprKind::StringLit:
        for (const StringPart& p : static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) promoteCoverExpr(p.expr.get(), ps);
        return;
      case ExprKind::Unary: {
        const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
        // A proven ++/-- reads its operand as raw int64 and boxes only the
        // yielded value, so the operand read stays inside a raw region.
        if ((u.op == "++" || u.op == "--") && u.operand &&
            u.operand->kind == ExprKind::Ident &&
            incDecProven(static_cast<const IdentExpr&>(*u.operand).name,
                         u.op == "++"))
          return;
        promoteCoverExpr(u.operand.get(), ps);
        return;
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        promoteCoverExpr(x.object.get(), ps);
        promoteCoverExpr(x.index.get(), ps);
        return;
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        promoteCoverExpr(x.object.get(), ps);
        promoteCoverExpr(x.start.get(), ps);
        promoteCoverExpr(x.end.get(), ps);
        promoteCoverExpr(x.step.get(), ps);
        return;
      }
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr&>(*e).entries) {
          promoteCoverExpr(kv.first.get(), ps);
          promoteCoverExpr(kv.second.get(), ps);
        }
        return;
      case ExprKind::SetLit:
      case ExprKind::ArrayLit:
      case ExprKind::Tuple: {
        const auto& els = e->kind == ExprKind::SetLit
            ? static_cast<const SetLitExpr&>(*e).elements
            : e->kind == ExprKind::ArrayLit
                ? static_cast<const ArrayLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els) promoteCoverExpr(el.get(), ps);
        return;
      }
      case ExprKind::Field:
        promoteCoverExpr(static_cast<const FieldExpr&>(*e).object.get(), ps);
        return;
      case ExprKind::SafeNav:
        promoteCoverExpr(static_cast<const SafeNavExpr&>(*e).object.get(), ps);
        return;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        promoteCoverExpr(r.start.get(), ps);
        promoteCoverExpr(r.end.get(), ps);
        promoteCoverExpr(r.step.get(), ps);
        return;
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        promoteCoverCond(t.cond.get(), ps);
        promoteCoverExpr(t.thenValue.get(), ps);
        promoteCoverExpr(t.elseValue.get(), ps);
        return;
      }
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        promoteCoverExpr(c.element.get(), ps);
        promoteCoverExpr(c.iterable.get(), ps);
        promoteCoverCond(c.condition.get(), ps);
        return;
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        promoteCoverExpr(m.subject.get(), ps);
        for (const MatchArm& arm : m.arms) {
          promoteCoverPattern(arm.pattern, ps);
          promoteCoverCond(arm.guard.get(), ps);
          promoteCoverExpr(arm.result.get(), ps);
        }
        return;
      }
      case ExprKind::Try:
        promoteCoverExpr(static_cast<const TryExpr&>(*e).operand.get(), ps);
        return;
      default:
        return;
    }
  }

  void promoteCoverPattern(const Pattern& p, PromoteScan& ps) {
    if (p.value) promoteCoverExpr(p.value.get(), ps);
    if (p.rangeStart) promoteCoverExpr(p.rangeStart.get(), ps);
    if (p.rangeEnd) promoteCoverExpr(p.rangeEnd.get(), ps);
    if (p.ctor) promoteCoverExpr(p.ctor.get(), ps);
    for (const Pattern& sub : p.subpatterns) promoteCoverPattern(sub, ps);
  }

  void promoteCoverStmt(const Stmt* s, PromoteScan& ps) {
    if (!s) return;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        // If this let promotes, its whole initializer is emitted raw.
        if (!(ps.pending.count(st.name) && st.init &&
              rawSafeExpr(st.init.get()) && exprIsInt(st.init.get())))
          promoteCoverExpr(st.init.get(), ps);
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind != ExprKind::Ident)
          promoteCoverExpr(st.target.get(), ps);
        promoteCoverExpr(st.value.get(), ps);
        return;
      }
      case StmtKind::Expr:
        promoteCoverExpr(static_cast<const ExprStmt&>(*s).expr.get(), ps);
        return;
      case StmtKind::Destructure:
        promoteCoverExpr(static_cast<const DestructureStmt&>(*s).value.get(), ps);
        return;
      case StmtKind::Return:
        promoteCoverExpr(static_cast<const ReturnStmt&>(*s).value.get(), ps);
        return;
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        for (const IfStmt::Branch& br : st.branches) {
          promoteCoverCond(br.cond.get(), ps);
          for (const StmtPtr& b : br.body) promoteCoverStmt(b.get(), ps);
        }
        for (const StmtPtr& b : st.elseBody) promoteCoverStmt(b.get(), ps);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        bool savedHot = promoteHot_;
        promoteHot_ = true;  // cond + body run per iteration
        promoteCoverCond(st.cond.get(), ps);
        for (const StmtPtr& b : st.body) promoteCoverStmt(b.get(), ps);
        promoteHot_ = savedHot;
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        bool savedHot = promoteHot_;
        promoteHot_ = true;
        for (const StmtPtr& b : st.body) promoteCoverStmt(b.get(), ps);
        promoteCoverCond(st.cond.get(), ps);
        promoteHot_ = savedHot;
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        promoteCoverExpr(st.iterable.get(), ps);  // evaluated once
        bool savedHot = promoteHot_;
        promoteHot_ = true;
        for (const StmtPtr& b : st.body) promoteCoverStmt(b.get(), ps);
        promoteHot_ = savedHot;
        return;
      }
      case StmtKind::Loop: {
        bool savedHot = promoteHot_;
        promoteHot_ = true;
        for (const StmtPtr& b : static_cast<const LoopStmt&>(*s).body)
          promoteCoverStmt(b.get(), ps);
        promoteHot_ = savedHot;
        return;
      }
      case StmtKind::Defer:
        promoteCoverStmt(static_cast<const DeferStmt&>(*s).body.get(), ps);
        return;
      default:
        return;
    }
  }

  void computePromoteSet(const FnDecl* fn) {
    promoteNames_.clear();
    nameRanges_.clear();
    if (!fn) return;
    computeNameRanges(fn);  // range proofs feed rawSafeExpr's + - * gate
    PromoteScan ps;
    promoteWriteStmts(fn->body, ps);
    for (const std::string& nm : ps.safe)
      if (!ps.bad.count(nm)) ps.pending.insert(nm);
    for (const std::string& nm : ps.spec)
      if (!ps.bad.count(nm)) ps.pending.insert(nm);
    promoteHot_ = false;
    for (const StmtPtr& s : fn->body) promoteCoverStmt(s.get(), ps);
    for (const std::string& nm : ps.pending)
      if (!ps.uncovered.count(nm)) promoteNames_.insert(nm);
    // Speculative accumulators: promoted names with at least one
    // overflow-unproven +,-,* write, excluding ++/-- targets and for-vars
    // (noSpec — their raw update paths have no deopt point). A spec-write
    // name that fails the gate is dropped from promotion entirely: its
    // writes have no plain-raw fallback, so leaving it promoted would hit
    // the compile-time demotion path mid-loop (unsound across the back
    // edge). Dropping it restores the all-aval status quo.
    specNames_.clear();
    for (const std::string& nm : ps.spec)
      if (promoteNames_.count(nm) && !ps.noSpec.count(nm))
        specNames_.insert(nm);
    for (const std::string& nm : ps.spec)
      if (!specNames_.count(nm)) promoteNames_.erase(nm);
  }

  bool tryEmitInlineCall(FnInfo* f, const CallExpr& call) {
    if (!f || inlineDepth_ != 0 || f->isMethod || f->variadic || !f->decl ||
        f->moduleId != curModuleId_ || call.args.size() != f->paramNames.size())
      return false;
    for (const CallArg& a : call.args)
      if (!a.name.empty()) return false;
    const InlineInfo& info = inlineInfoFor(f);
    if (!info.safe) return false;
    int& sites = inlineSites_[f->decl];
    if (sites >= kMaxInlineSites) return false;

    // Raw-int inlining prefers args already materialized as int64 (raw-int
    // locals copy for free, literals become immediates, safe-op exprs stay
    // raw) and falls back to aval slots + per-param unbox otherwise. Args
    // must be evaluated exactly once, so the strategy is chosen before any
    // arg code is emitted.
    const bool rawOk = info.raw && callArgsAllInt(call);
    bool argsRaw = false;
    if (rawOk) {
      argsRaw = true;
      for (const CallArg& a : call.args)
        if (!rawSafeExpr(a.value.get())) {
          argsRaw = false;
          break;
        }
    }
    std::vector<int> argSlots;
    argSlots.reserve(call.args.size());
    if (argsRaw) {
      for (const CallArg& a : call.args) {
        int s = allocSlot();
        emitRawExpr(a.value.get(), s);
        argSlots.push_back(s);
      }
    } else {
      for (const CallArg& a : call.args)
        argSlots.push_back(evalSlot(a.value.get()));
    }
    ++sites;

    std::vector<std::unordered_map<std::string, ABinding>> savedScopes;
    beginInlineScope(f, savedScopes);
    int savedModule = curModuleId_;
    bool savedInFn = inFunction_;
    curModuleId_ = f->moduleId;
    inFunction_ = true;
    // The inlined body is a different function: swap in its own promotion
    // set (computed below for the generic path; raw bodies don't need it).
    auto savedPromote = std::move(promoteNames_);
    auto savedSpec = std::move(specNames_);
    auto savedRanges = std::move(nameRanges_);
    promoteNames_.clear();
    specNames_.clear();
    nameRanges_.clear();

    if (rawOk) {
      emitRawBody(f, argSlots, argsRaw, false);
    } else {
      for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
        ABinding b;
        b.kind = BKind::Param;
        b.slot = argSlots[i];
        scopes_.back()[f->paramNames[i]] = b;
      }
      computePromoteSet(f->decl);  // promote the callee's own locals
      int resultSlot = allocSlot();
      int savedDepth = inlineDepth_;
      std::string savedLabel = inlineReturnLabel_;
      int savedResult = inlineResultSlot_;
      int savedDeferBase = inlineDeferBase_;
      inlineDepth_ = 1;
      inlineDeferBase_ = (int)deferStack_.size();
      inlineReturnLabel_ = newLabel("inlret");
      inlineResultSlot_ = resultSlot;
      emitBody(f->decl->body);
      movImm(kNone);
      storeRax(resultSlot);
      emitLabel(inlineReturnLabel_);
      loadSlot(resultSlot);
      inlineDepth_ = savedDepth;
      inlineReturnLabel_ = savedLabel;
      inlineResultSlot_ = savedResult;
      inlineDeferBase_ = savedDeferBase;
    }

    promoteNames_ = std::move(savedPromote);
    specNames_ = std::move(savedSpec);
    nameRanges_ = std::move(savedRanges);
    curModuleId_ = savedModule;
    inFunction_ = savedInFn;
    endInlineScope(savedScopes);
    return true;
  }

  // ---- calls -------------------------------------------------------------------
  std::vector<const Expr*> positionalArgs(const CallExpr& call) {
    std::vector<const Expr*> out;
    for (auto& a : call.args) out.push_back(a.value.get());
    return out;
  }

  // a_bi_call(id, n, argv): argv holds the n user args (no env slot).
  void emitBiCallSlots(int id, const std::vector<int>& slots) {
    Argv av = allocArgv((int)slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) argvStore(av, (int)i, slots[i]);
    movImm((uint64_t)id, "%rdi");
    movImm(slots.size(), "%rsi");
    if (slots.empty()) {
      emit("    xor %edx, %edx");
    } else {
      leaSlot(argvPtr(av), "%rdx");
    }
    emit("    call a_bi_call");
  }

  // len(x) with an array-typed argument: read AObj.len (offset 4) inline.
  // Whitelisted runtime types all keep the element count in h.len
  // (T_LIST/T_TUPLE/T_DICT/T_SET/T_VIEW); T_STR stores bytes and T_RANGE
  // stores no count, so those — and anything unexpected — fall back to
  // the a_len runtime call. Result aval left in %rax.
  bool tryEmitFastLen(const Expr* arg) {
    TypePtr at = resolveType(arg->inferredType);
    if (!at || at->kind != TypeKind::Array) return false;
    int s = evalSlot(arg);
    std::string fastL = newLabel("lenf"), slowL = newLabel("lens"),
            doneL = newLabel("lend");
    loadSlot(s);
    emit("    mov %rax, %rcx\n    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n"
         "    jne " + slowL + "\n");
    emit("    mov %rax, %rcx\n    and $0xF, %rcx\n    jnz " + slowL + "\n");
    emit("    mov %rax, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n");
    emit("    mov (%rcx), %eax\n    cmp $2, %eax\n    je " + fastL + "\n"
         "    cmp $3, %eax\n    je " + fastL + "\n"
         "    cmp $4, %eax\n    je " + fastL + "\n"
         "    cmp $5, %eax\n    je " + fastL + "\n"
         "    cmp $13, %eax\n    je " + fastL + "\n"
         "    jmp " + slowL + "\n");
    emitLabel(fastL);
    emit("    mov 4(%rcx), %eax\n");
    emitBoxInt64();
    emit("    jmp " + doneL + "\n");
    emitLabel(slowL);
    call1slot("a_len", s);
    emitLabel(doneL);
    return true;
  }

  // sqrt(x) fast path for an already-evaluated argument slot: untagged
  // doubles compute via sqrtsd instead of the a_bi_call dispatch. The
  // domain check `x < 0.0` is done with ucomisd so NaN behaves exactly like
  // the runtime (C `x < 0.0` is false for NaN, and sqrt(NaN) canonicalizes
  // via mkdouble). Negative inputs panic with the same "math domain error"
  // message; non-double avals (ints, wrong types) fall back to a_bi_call.
  void tryEmitSqrtSlot(int id, int s) {
    std::string slowL = newLabel("sqs"), domL = newLabel("sqd"),
            doneL = newLabel("sqf");
    rawInvAll();  // direct slotRef traffic below bypasses the reg cache
    loadSlot(s, "%rcx");
    emit("    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n    je " + slowL +
         "\n");
    int t = allocSlot();
    emit("    movsd " + slotRef(s) + ", %xmm0\n"
         "    xorpd %xmm1, %xmm1\n"
         // ucomisd %xmm0,%xmm1 compares 0.0(xmm1) vs x(xmm0): `ja` (above,
         // ordered) means 0.0 > x i.e. x < 0.0. NaN leaves CF=ZF=1 so the
         // jump is not taken — matching C semantics for the domain check.
         "    ucomisd %xmm0, %xmm1\n    ja " + domL + "\n"
         "    sqrtsd %xmm0, %xmm0\n");
    emitXmm0ToAval(t, doneL);
    emitLabel(domL);
    emitPanicStr("math domain error");
    emitLabel(slowL);
    emitBiCallSlots(id, {s});
    emitLabel(doneL);
  }

  void emitBiCall(int id, const CallExpr& call) {
    if (id == 2 && call.args.size() == 1 && call.args[0].name.empty() &&
        tryEmitFastLen(call.args[0].value.get())) {
      return;
    }
    if (id == 12 && call.args.size() == 1 && call.args[0].name.empty()) {
      int s = evalSlot(call.args[0].value.get());
      tryEmitSqrtSlot(id, s);
      return;
    }
    std::vector<const Expr*> args = positionalArgs(call);
    std::vector<int> slots;
    for (const Expr* a : args) slots.push_back(evalSlot(a));
    emitBiCallSlots(id, slots);
  }

  // py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
  // the module/member strings, argv[2] the kwargs count, then positional
  // args, then name/value pairs. The runtime resolves libpython.
  void emitPyCall(const std::string& module, const std::string& member,
                  const CallExpr& call) {
    int ms = strLitSlot(module);
    int fs = strLitSlot(member);
    std::vector<int> posSlots;
    std::vector<std::pair<std::string, int>> kwSlots;
    for (const CallArg& a : call.args) {
      int s = evalSlot(a.value.get());
      if (a.name.empty()) {
        posSlots.push_back(s);
      } else {
        kwSlots.push_back({a.name, s});
      }
    }
    Argv av = allocArgv((int)posSlots.size() + 2 * (int)kwSlots.size() + 3);
    argvStore(av, 0, ms);
    argvStore(av, 1, fs);
    argvStoreImm(av, 2, mkintA((int64_t)kwSlots.size()));
    for (std::size_t i = 0; i < posSlots.size(); ++i)
      argvStore(av, (int)i + 3, posSlots[i]);
    for (std::size_t i = 0; i < kwSlots.size(); ++i) {
      int ks = strLitSlot(kwSlots[i].first);
      argvStore(av, (int)(3 + posSlots.size() + 2 * i), ks);
      argvStore(av, (int)(3 + posSlots.size() + 2 * i + 1), kwSlots[i].second);
    }
    movImm((uint64_t)builtinRuntimeId("py_call"), "%rdi");
    movImm(av.n, "%rsi");
    leaSlot(argvPtr(av), "%rdx");
    emit("    call a_bi_call");
  }

  // a_call(fn, n, argv): indirect call through an FNON value.
  void emitACall(const Expr* callee, const CallExpr& call) {
    int fnS = evalSlot(callee);
    std::vector<const Expr*> args = positionalArgs(call);
    std::vector<int> slots;
    for (const Expr* a : args) slots.push_back(evalSlot(a));
    Argv av = allocArgv((int)args.size());
    for (std::size_t i = 0; i < args.size(); ++i) argvStore(av, (int)i, slots[i]);
    loadSlot(fnS, "%rdi");
    movImm(args.size(), "%rsi");
    if (args.empty()) {
      emit("    xor %edx, %edx");
    } else {
      leaSlot(argvPtr(av), "%rdx");
    }
    emit("    call a_call");
  }

  // a_method(obj, name, n, nkw, argv): generic method dispatch. argv holds
  // the n positional args followed by nkw (name, value) pairs; only
  // T_PYOBJ receivers consume the pairs (as Python kwargs) — every other
  // receiver kind panics on nkw > 0.
  void emitMethodCall(const Expr* objExpr, const std::string& field, const CallExpr& call) {
    // `xs.append(v)` on an array-typed receiver: direct runtime push that
    // skips the generic a_method name-dispatch chain. a_list_append keeps
    // the same semantics for non-T_LIST objects (incl. the view panic).
    if (field == "append" && call.args.size() == 1 && call.args[0].name.empty()) {
      TypePtr ot = resolveType(objExpr->inferredType);
      if (ot && ot->kind == TypeKind::Array) {
        int os = evalSlot(objExpr);
        int vs = evalSlot(call.args[0].value.get());
        loadSlot(os, "%rdi");
        loadSlot(vs, "%rsi");
        // Int-typed values go through the packed-int-aware append: int
        // elements keep (or promote to) int64 storage; anything else
        // falls back to the generic append semantics inside the runtime.
        emit("    call a_list_append\n");
        return;
      }
    }
    int os = evalSlot(objExpr);
    std::vector<int> posSlots;
    std::vector<std::pair<std::string, int>> kwSlots;
    for (const CallArg& a : call.args) {
      int s = evalSlot(a.value.get());
      if (a.name.empty()) {
        posSlots.push_back(s);
      } else {
        kwSlots.push_back({a.name, s});
      }
    }
    Argv av = allocArgv((int)posSlots.size() + 2 * (int)kwSlots.size());
    for (std::size_t i = 0; i < posSlots.size(); ++i)
      argvStore(av, (int)i, posSlots[i]);
    for (std::size_t i = 0; i < kwSlots.size(); ++i) {
      int ks = strLitSlot(kwSlots[i].first);
      argvStore(av, (int)(posSlots.size() + 2 * i), ks);
      argvStore(av, (int)(posSlots.size() + 2 * i + 1), kwSlots[i].second);
    }
    std::string lbl = emitStrData(field, true);
    loadSlot(os, "%rdi");
    emit("    lea " + lbl + "(%rip), %rsi\n");
    movImm(posSlots.size(), "%rdx");
    movImm(kwSlots.size(), "%rcx");
    if (av.n == 0) {
      emit("    xor %r8d, %r8d");
    } else {
      leaSlot(argvPtr(av), "%r8");
    }
    emit("    call a_method");
  }

  // Place evaluated call args into paramSlots (indexed by paramNames position,
  // -1 = fill A_NONE). `offset` shifts where positional args land (1 for
  // methods whose paramNames[0] is `self`, already filled by the caller).
  // When `variadicSlot` >= 0, positional args landing at or past that index
  // are packed into a list stored in paramSlots[variadicSlot]; an empty list
  // is stored when no extras were passed (unless a named arg supplied it).
  void placeArgs(const CallExpr& call, const std::vector<std::string>& paramNames,
                 std::vector<int>& paramSlots, std::size_t offset = 0,
                 int variadicSlot = -1) {
    std::size_t pos = 0;
    std::vector<int> extras;
    for (auto& a : call.args) {
      int s = evalSlot(a.value.get());
      if (a.name.empty()) {
        std::size_t p = pos + offset;
        if (variadicSlot >= 0 && (int)p >= variadicSlot) {
          extras.push_back(s);
        } else if (p < paramSlots.size()) {
          paramSlots[p] = s;
        }
        ++pos;
      } else {
        for (std::size_t i = 0; i < paramNames.size(); ++i) {
          if (paramNames[i] == a.name) {
            std::size_t p = i + offset;
            if (p < paramSlots.size()) paramSlots[p] = s;
          }
        }
      }
    }
    if (variadicSlot >= 0 && (std::size_t)variadicSlot < paramSlots.size() &&
        paramSlots[variadicSlot] < 0) {
      int ls;
      if (extras.empty()) {
        movImm(0, "%rdi");
        emit("    xor %esi, %esi\n    call a_list_new\n");
        ls = allocSlot();
        storeRax(ls);
      } else {
        Argv av = allocArgv((int)extras.size());
        for (std::size_t i = 0; i < extras.size(); ++i) argvStore(av, (int)i, extras[i]);
        movImm(extras.size(), "%rdi");
        leaSlot(argvPtr(av), "%rsi");
        emit("    call a_list_new\n");
        ls = allocSlot();
        storeRax(ls);
      }
      paramSlots[variadicSlot] = ls;
    }
  }

  // Direct call to an A function/ctor: argv[0]=A_NONE env, argv[1..]=params.
  void emitDirectCall(const char* cname, const std::vector<int>& paramSlots) {
    Argv av = allocArgv((int)paramSlots.size() + 1);
    argvStoreImm(av, 0, kNone);
    for (std::size_t i = 0; i < paramSlots.size(); ++i) {
      if (paramSlots[i] >= 0) {
        argvStore(av, (int)i + 1, paramSlots[i]);
      } else {
        argvStoreImm(av, (int)i + 1, kMissing);
      }
    }
    leaSlot(argvPtr(av), "%rdi");
    emit(std::string("    call ") + cname);
  }

  // paramSlots sized paramNames.size(); fills from call args. For methods the
  // caller presets paramSlots[0] = self and passes paramNames = full list with
  // offset 1 / rest names.
  void emitCallExpr(const CallExpr& call) {
    if (call.builtinId != BuiltinId::None) {
      int id = builtinIdForBuiltin(call.builtinId);
      if (id >= 0) {
        emitBiCall(id, call);
        return;
      }
    }
    const Expr* callee = call.callee.get();
    if (callee->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*callee);
      ABinding* b = lookup(id.name);
      if (b) {
        if (b->kind == BKind::PyFunc) {
          emitPyCall(b->aux, b->pyMember, call);
          return;
        }
        if (b->kind == BKind::FnValue && b->fn) {
          if (tryEmitInlineCall(b->fn, call)) return;
          std::vector<int> ps(b->fn->paramNames.size(), -1);
          placeArgs(call, b->fn->paramNames, ps, 0,
                    b->fn->variadic ? (int)b->fn->paramNames.size() - 1 : -1);
          emitDirectCall(b->fn->cname.c_str(), ps);
          return;
        }
        if (b->kind == BKind::StructCtor && b->st) {
          std::vector<int> ps(b->st->fieldNames.size(), -1);
          placeArgs(call, b->st->fieldNames, ps);
          emitDirectCall(b->st->ctorCname.c_str(), ps);
          return;
        }
        if (b->kind == BKind::Builtin) {
          emitBiCall(b->builtinId, call);
          return;
        }
        if (b->kind == BKind::EnumValue || b->kind == BKind::Module) {
          fail("value is not callable");
          return;
        }
        emitACall(callee, call);
        return;
      }
      int bid = builtinRuntimeId(id.name);
      if (bid >= 0) {
        emitBiCall(bid, call);
        return;
      }
      fail("undefined function '" + id.name + "'");
      return;
    }
    if (callee->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*callee);
      // Module-qualified: math.sqrt, mod.fn, mod.Struct
      if (f.object->kind == ExprKind::Ident) {
        const IdentExpr& mid = static_cast<const IdentExpr&>(*f.object);
        ABinding* mb = lookup(mid.name);
        if (mb && mb->kind == BKind::PyModule) {
          emitPyCall(mb->aux, f.field, call);
          return;
        }
        if (mb && mb->kind == BKind::Module) {
          // Native modules resolve through the nativeModules() spec first:
          // member names may shadow builtin names (unsafe.set vs set).
          if (mb->moduleId >= 0) {
            int bid = builtinRuntimeId(f.field);
            if (bid >= 0) {
              emitBiCall(bid, call);
              return;
            }
          }
          if (mb->moduleId < 0) {
            // Native module member: resolve module.member -> builtin name
            // via the nativeModules() spec, then dispatch a_bi_call.
            int bid = -1;
            auto mit = nativeModules().find(mb->aux);
            if (mit != nativeModules().end()) {
              auto fit = mit->second.functions.find(f.field);
              if (fit != mit->second.functions.end())
                bid = builtinRuntimeId(fit->second);
            }
            if (bid < 0) bid = builtinRuntimeId(f.field);
            if (bid >= 0) {
              std::vector<int> ps;
              for (const CallArg& a : call.args) ps.push_back(evalSlot(a.value.get()));
              emitBiCallSlots(bid, ps);
              return;
            }
            fail("native module function not supported by asm backend: " +
                 mb->aux + "." + f.field);
            return;
          }
          for (FnInfo* fi : fnList_) {
            if (!fi->isMethod && fi->moduleId == mb->moduleId && fi->name == f.field) {
              if (tryEmitInlineCall(fi, call)) return;
              std::vector<int> ps(fi->paramNames.size(), -1);
              placeArgs(call, fi->paramNames, ps, 0,
                        fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
              emitDirectCall(fi->cname.c_str(), ps);
              return;
            }
          }
          for (StructInfo* st : structList_) {
            if (st->moduleId == mb->moduleId && st->name == f.field) {
              std::vector<int> ps(st->fieldNames.size(), -1);
              placeArgs(call, st->fieldNames, ps);
              emitDirectCall(st->ctorCname.c_str(), ps);
              return;
            }
          }
          fail("unknown module member '" + f.field + "'");
          return;
        }
      }
      // Struct method with known receiver type.
      TypePtr ot = resolveType(f.object->inferredType);
      FnInfo* method = nullptr;
      std::vector<std::string> restParams;
      if (ot && ot->kind == TypeKind::Struct) {
        auto sit = structs_.find(shortName(ot->structName));
        if (sit != structs_.end()) {
          auto mit = sit->second->methods.find(f.field);
          if (mit != sit->second->methods.end()) method = mit->second;
        }
      }
      if (!method) {
        // Fallback: method name provided by exactly one struct (untyped
        // receiver). Skip when the static type is a known runtime value.
        bool isRuntimeValue = ot && ot->kind != TypeKind::Struct &&
                              ot->kind != TypeKind::Error && ot->kind != TypeKind::Var;
        int candidates = 0;
        if (!isRuntimeValue) {
          for (StructInfo* st : structList_) {
            if (st->methods.count(f.field)) {
              method = st->methods[f.field];
              ++candidates;
            }
          }
        }
        if (candidates != 1) method = nullptr;
      }
      if (method) {
        int selfS = evalSlot(f.object.get());
        std::vector<int> ps(method->paramNames.size(), -1);
        ps[0] = selfS;  // self
        if (method->paramNames.size() > 1) {
          std::vector<std::string> rest(method->paramNames.begin() + 1, method->paramNames.end());
          placeArgs(call, rest, ps, 1,
                    method->variadic ? (int)method->paramNames.size() - 1 : -1);
        }
        emitDirectCall(method->cname.c_str(), ps);
        return;
      }
      emitMethodCall(f.object.get(), f.field, call);
      return;
    }
    emitACall(callee, call);
  }

  // ---- expressions ---------------------------------------------------------------
  void emitExpr(const Expr* e) {
    if (failed_) return;
    if (!e) {
      movImm(kNone);
      return;
    }
    switch (e->kind) {
      case ExprKind::IntLit:
        emitIntLitA(static_cast<const IntLitExpr*>(e)->value);
        return;
      case ExprKind::FloatLit:
        movImm(dblBits(static_cast<const FloatLitExpr*>(e)->value));
        return;
      case ExprKind::BoolLit:
        movImm(static_cast<const BoolLitExpr*>(e)->value ? kTrue : kFalse);
        return;
      case ExprKind::CharLit: {
        auto& x = static_cast<const CharLitExpr&>(*e);
        std::string u = utf8Encode(x.value);
        std::string lbl = emitStrData(u, false);
        emit("    lea " + lbl + "(%rip), %rdi\n");
        movImm(u.size(), "%rsi");
        emit("    call a_char_new");
        return;
      }
      case ExprKind::StringLit: {
        auto& x = static_cast<const StringLitExpr&>(*e);
        if (x.parts.size() == 1 && !x.parts[0].isExpr) {
          int s = strLitSlot(x.parts[0].text);
          loadSlot(s);
          return;
        }
        if (x.parts.empty()) {
          int s = strLitSlot("");
          loadSlot(s);
          return;
        }
        // Interpolation: str() each expr part, join all with a_str_join.
        std::vector<int> segs;
        for (auto& p : x.parts) {
          int s;
          if (p.isExpr) {
            emitExpr(p.expr.get());
            emit("    mov %rax, %rdi\n    call a_str\n");
            s = allocSlot();
            storeRax(s);
          } else {
            s = strLitSlot(p.text);
          }
          segs.push_back(s);
        }
        Argv av = allocArgv((int)segs.size());
        for (std::size_t i = 0; i < segs.size(); ++i) argvStore(av, (int)i, segs[i]);
        movImm(segs.size(), "%rdi");
        leaSlot(argvPtr(av), "%rsi");
        emit("    call a_str_join");
        return;
      }
      case ExprKind::Ident: {
        auto& x = static_cast<const IdentExpr&>(*e);
        if (x.name == "None") {
          movImm(kNone);
          return;
        }
        if (x.name == "PI") {
          movImm(dblBits(3.141592653589793));
          return;
        }
        if (x.name == "E") {
          movImm(dblBits(2.718281828459045));
          return;
        }
        if (x.name == "I8_MIN") { movImm(mkintA(-128)); return; }
        if (x.name == "I8_MAX") { movImm(mkintA(127)); return; }
        if (x.name == "I16_MIN") { movImm(mkintA(-32768)); return; }
        if (x.name == "I16_MAX") { movImm(mkintA(32767)); return; }
        if (x.name == "I32_MIN") { movImm(mkintA(-2147483648LL)); return; }
        if (x.name == "I32_MAX") { movImm(mkintA(2147483647LL)); return; }
        if (x.name == "I64_MIN") { emitIntLitA(INT64_MIN); return; }
        if (x.name == "I64_MAX") { emitIntLitA(INT64_MAX); return; }
        if (x.name == "U8_MAX") { movImm(mkintA(255)); return; }
        if (x.name == "U16_MAX") { movImm(mkintA(65535)); return; }
        if (x.name == "U32_MAX") { movImm(mkintA(4294967295LL)); return; }
        if (x.name == "F32_MIN") { movImm(dblBits(-3.4028234663852886e38)); return; }
        if (x.name == "F32_MAX") { movImm(dblBits(3.4028234663852886e38)); return; }
        if (x.name == "F64_MIN") { movImm(dblBits(-1.7976931348623157e308)); return; }
        if (x.name == "F64_MAX") { movImm(dblBits(1.7976931348623157e308)); return; }
        ABinding* b = lookup(x.name);
        if (b) {
          emitBindingValue(*b);
          return;
        }
        int id = builtinRuntimeId(x.name);
        if (id >= 0) {
          movImm((uint64_t)id, "%rdi");
          emit("    call a_builtin");
          return;
        }
        fail("undefined name '" + x.name + "'");
        movImm(kNone);
        return;
      }
      case ExprKind::Unary: {
        auto& x = static_cast<const UnaryExpr&>(*e);
        if (x.op == "++" || x.op == "--") {
          // ++/-- on a variable: updated = old +/- 1; prefix yields updated,
          // postfix yields old (mirrors the IR lowering).
          if (x.operand->kind != ExprKind::Ident) {
            fail("operand of '" + x.op + "' must be a variable");
            movImm(kNone);
            return;
          }
          const IdentExpr& id = static_cast<const IdentExpr&>(*x.operand);
          ABinding* b = lookup(id.name);
          if (!b) {
            fail("undefined variable '" + id.name + "'");
            movImm(kNone);
            return;
          }
          const bool isInc = x.op == "++";
          // Promoted raw-int binding + proven non-overflowing increment:
          // update the int64 slot in place and box only the yielded value.
          // The slot stays raw (ints own nothing, so no drop bookkeeping
          // and no demotion). incDecProven mirrors promoteMarkExpr /
          // promoteCoverExpr, so a promoted binding always qualifies.
          if (b->rawInt && !b->dropFn && incDecProven(id.name, isInc)) {
            int oldS = allocSlot();
            copySlot(b->slot, oldS);
            loadSlot(b->slot);
            emit(std::string("    ") + (isInc ? "addq $1, %rax\n"
                                                 : "subq $1, %rax\n"));
            storeRax(b->slot);
            loadSlot(x.postfix ? oldS : b->slot);
            emitBoxInt64();
            return;
          }
          emitBindingValue(*b);
          int oldS = allocSlot();
          storeRax(oldS);
          int oneS = allocSlot();
          movImm(mkintA(1), "%rcx");
          emit("    mov %rcx, " + slotRef(oneS));
          // emitArith inlines the int64 add/sub with jo -> a_add_slow /
          // a_sub_slow on overflow (yielding the double aval), so the slow
          // call is only emitted for non-int operands — identical
          // semantics to the previous unconditional call2slot.
          emitArith(isInc ? "+" : "-", oldS, oneS);
          int newS = allocSlot();
          storeRax(newS);
          // Rebinding drops the previous owned value first (same as assign).
          DropRec* oldRec = b->dropFn ? findDropRec(*b) : nullptr;
          if (b->dropFn && oldRec &&
              (b->kind == BKind::Local || b->kind == BKind::Param ||
               b->kind == BKind::Global)) {
            emitDropRec(*oldRec);
          }
          storeBinding(*b, newS);
          if (b->rawInt) b->rawInt = false;  // demote: slot now holds aval
          FnInfo* newDrop = dropFnFor(x.operand->inferredType);
          b->dropFn = newDrop;
          DropRec* rec = findDropRec(*b);
          if (rec) {
            rec->fn = newDrop;
          } else if (newDrop && !inFunction_ &&
                     (b->kind == BKind::Local || b->kind == BKind::Global)) {
            if (b->kind == BKind::Global) {
              deferStack_.back().drops.push_back({-1, newDrop, b->sym});
            } else {
              deferStack_.back().drops.push_back({b->slot, newDrop, ""});
            }
          } else if (newDrop && inFunction_ && b->kind == BKind::Local) {
            deferStack_.back().drops.push_back({b->slot, newDrop, ""});
          }
          loadSlot(x.postfix ? oldS : newS);
          return;
        }
        emitExpr(x.operand.get());
        if (x.op == "-") {
          // Untagged-double aval: negate by flipping the sign bit inline,
          // then canonicalize NaN exactly like mkdouble(-x) (a_neg_slow
          // routes doubles through mkdouble, which maps every NaN to the
          // canonical positive QNaN). Inline ints, boxed ints, and bad
          // operand types keep the a_neg_slow call (incl. its panic).
          int s = allocSlot();
          storeRax(s);
          std::string slowL = newLabel("ngs"), finL = newLabel("ngf"),
                  endL = newLabel("nge");
          rawInvAll();
          loadSlot(s, "%rcx");
          emit("    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n    je " + slowL +
               "\n");
          loadSlot(s);  // %rax = aval bits
          emit("    btc $63, %rax\n");
          // NaN iff exponent is all ones and the mantissa is nonzero.
          emit("    mov %rax, %rcx\n    shr $52, %rcx\n    and $0x7FF, %ecx\n"
               "    cmp $0x7FF, %ecx\n    jne " + finL + "\n"
               "    mov %rax, %rcx\n    shl $12, %rcx\n    jz " + finL +
               "\n");
          movImm(kCanonNaN);
          emitLabel(finL);
          emit("    jmp " + endL + "\n");
          emitLabel(slowL);
          loadSlot(s, "%rdi");
          emit("    call a_neg_slow\n");
          emitLabel(endL);
          return;
        }
        emit("    mov %rax, %rdi\n");
        if (x.op == "~") {
          emit("    call a_bnot_slow");
        } else if (x.op == "not") {
          emit("    call a_not");
        } else if (x.op == "!") {
          emit("    call a_unwrap");
        } else {
          fail("unsupported unary operator '" + x.op + "'");
          movImm(kNone);
        }
        return;
      }
      case ExprKind::Binary: {
        auto& x = static_cast<const BinaryExpr&>(*e);
        const std::string& op = x.op;
        if (op == "and" || op == "or") {
          // Short-circuit: and -> truthy(lhs) ? rhs : lhs ; or -> lhs : rhs.
          int ls = evalSlot(x.lhs.get());
          std::string lhsL = newLabel("sc_lhs");
          std::string endL = newLabel("sc_end");
          loadSlot(ls, "%rdi");
          emit("    call a_truthy_raw_slow\n    test %rax, %rax\n");
          emit(std::string(op == "and" ? "    jz " : "    jnz ") + lhsL + "\n");
          emitExpr(x.rhs.get());  // rax = rhs
          jmp(endL);
          emitLabel(lhsL);
          loadSlot(ls);  // rax = lhs
          emitLabel(endL);
          return;
        }
        emitBinary(x);
        return;
      }
      case ExprKind::Range: {
        auto& x = static_cast<const RangeExpr&>(*e);
        int lo = evalSlot(x.start.get());
        int hi = evalSlot(x.end.get());
        int st = x.step ? evalSlot(x.step.get()) : -1;
        if (st < 0) {
          st = allocSlot();
          movImm(mkintA(1), "%rcx");
          emit("    mov %rcx, " + slotRef(st));
        }
        call3slot("a_range_new", lo, hi, st);
        return;
      }
      case ExprKind::Ternary: {
        auto& x = static_cast<const TernaryExpr&>(*e);
        int rs = allocSlot();
        std::string elseL = newLabel("tern");
        std::string endL = newLabel("tend");
        condJump(x.cond.get(), elseL, false);
        emitExpr(x.thenValue.get());
        storeRax(rs);
        jmp(endL);
        emitLabel(elseL);
        emitExpr(x.elseValue.get());
        storeRax(rs);
        emitLabel(endL);
        loadSlot(rs);
        return;
      }
      case ExprKind::Index: {
        auto& x = static_cast<const IndexExpr&>(*e);
        int os = evalSlot(x.object.get());
        // Raw int64 index: a promoted (rawInt) ident already holds int64 in
        // its slot; any other raw-safe int expr with a proven range can be
        // materialized raw. Both feed %r8 directly, skipping the box+unbox
        // round-trip the aval path would pay per access. Anything else
        // keeps the generic evalSlot + emitUnboxInt path.
        int isRaw = -1;
        int is = -1;
        if (x.index && x.index->kind == ExprKind::Ident) {
          ABinding* ib =
              lookup(static_cast<const IdentExpr&>(*x.index).name);
          // specRaw bindings have no single raw slot (demotion swaps the
          // representation), so they take the generic evalSlot path.
          if (ib && ib->rawInt && !ib->specRaw) isRaw = ib->slot;
        }
        if (isRaw < 0 && x.index && exprIsInt(x.index.get()) &&
            rawSafeExpr(x.index.get()) &&
            exprRangeFlat(x.index.get()).known) {
          isRaw = allocSlot();
          ++rawRegDepth_;
          rawRegOf_.clear();
          emitRawExpr(x.index.get(), isRaw);
          --rawRegDepth_;
          rawRegOf_.clear();
        }
        if (isRaw < 0) is = evalSlot(x.index.get());
        // Fallback paths need the index as an aval slot; box the raw int64
        // on demand (only emitted where the slow path actually runs).
        auto avalIndex = [&]() -> int {
          if (isRaw < 0) return is;
          int bs = allocSlot();
          loadSlot(isRaw);
          emitBoxInt64();
          storeRax(bs);
          return bs;
        };
        // xs[i] on an array-typed object: unbox the index inline, verify
        // the receiver is an AArr-backed object (T_LIST/T_TUPLE), bounds
        // check against h.len (with negative-index wraparound, matching
        // normalize_index), and load items[i] directly. Anything else —
        // non-int index, dict/string/view/other receiver — falls back to
        // a_getitem.
        TypePtr ot = resolveType(x.object->inferredType);
        if (ot && ot->kind == TypeKind::Array) {
          // BCE: an enclosing `for i in lo..len(arr)` (lo >= 0 literal,
          // step 1, pure body that never writes i or arr — see emitFor)
          // proves i is in [0, len(arr)) here, so the negative-wrap and
          // bounds check can be skipped. Matching is by binding identity,
          // so shadowing or rebinding either name falls back to the check.
          bool bceHit = false;
          if (!bceLoops_.empty() && x.object->kind == ExprKind::Ident &&
              x.index->kind == ExprKind::Ident) {
            ABinding* ob =
                lookup(static_cast<const IdentExpr&>(*x.object).name);
            ABinding* ib =
                lookup(static_cast<const IdentExpr&>(*x.index).name);
            if (ob && ib) {
              for (auto it = bceLoops_.rbegin(); it != bceLoops_.rend(); ++it) {
                if (it->varBind == ib && it->arrBind == ob) {
                  bceHit = true;
                  break;
                }
              }
            }
          }
          std::string slowL = newLabel("gis"), fastL = newLabel("gif"),
                  doneL = newLabel("gid");
          std::string panicL, negL;
          if (!bceHit) {
            panicL = newLabel("gip");
            negL = newLabel("gin");
          }
          movImm(kTagInt, "%rdx");
          if (isRaw >= 0) {
            loadSlot(isRaw, "%r8");  // already int64; no unbox needed
          } else {
            loadSlot(is, "%rdi");
            emitUnboxInt("%r8", slowL);  // r8 = index int64
          }
          loadSlot(os, "%rdi");
          emit("    mov %rdi, %rcx\n    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n"
               "    jne " + slowL + "\n");
          emit("    mov %rdi, %rcx\n    and $0xF, %rcx\n    jnz " + slowL + "\n");
          emit("    mov %rdi, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n");
          emit("    mov (%rcx), %eax\n    cmp $2, %eax\n    je " + fastL + "\n"
               "    cmp $3, %eax\n    je " + fastL + "\n"
               "    jmp " + slowL + "\n");
          emitLabel(fastL);
          // Packed-int storage: flag bit0 of AArr.flags (@32) selects the
          // int64 element buffer (@24) instead of the aval items (@16);
          // the loaded int64 is boxed back to an aval for the result.
          {
            std::string pkL = newLabel("gipk"), avL = newLabel("giav");
            auto emitIdxLoad = [&](const std::string& doneL) {
              emit("    mov 32(%rcx), %r9d\n    test $1, %r9d\n    jnz " + pkL +
                   "\n    jmp " + avL + "\n");
              emitLabel(avL);
              emit("    mov 16(%rcx), %rcx\n    mov (%rcx,%r8,8), %rax\n"
                   "    jmp " + doneL + "\n");
              emitLabel(pkL);
              emit("    mov 24(%rcx), %rcx\n    mov (%rcx,%r8,8), %rax\n");
              emitBoxInt64();
              emit("    jmp " + doneL + "\n");
            };
            if (bceHit) {
              // i >= 0 and i < len(arr): load element directly.
              emitIdxLoad(doneL);
            } else {
              emit("    mov 4(%rcx), %edx\n");  // rdx = len (u32, zero-extended)
              emit("    test %r8, %r8\n    jns " + negL + "_p\n"
                   "    add %rdx, %r8\n");
              emitLabel(negL + "_p");
              emit("    cmp %rdx, %r8\n    jae " + panicL + "\n");
              emitIdxLoad(doneL);
              emitLabel(panicL);
              emitPanicStr("index out of range");
              emit("    jmp " + doneL + "\n");
            }
          }
          emitLabel(slowL);
          call2slot("a_getitem", os, avalIndex());
          emitLabel(doneL);
          return;
        }
        call2slot("a_getitem", os, avalIndex());
        return;
      }
      case ExprKind::Slice: {
        auto& x = static_cast<const SliceExpr&>(*e);
        int os = evalSlot(x.object.get());
        int lo = x.start ? evalSlot(x.start.get()) : evalSlot(nullptr);
        int hi = x.end ? evalSlot(x.end.get()) : evalSlot(nullptr);
        int st = x.step ? evalSlot(x.step.get()) : evalSlot(nullptr);
        call4slot("a_slice", os, lo, hi, st);
        return;
      }
      case ExprKind::Field:
        emitField(e);
        return;
      case ExprKind::ArrayLit: {
        auto& x = static_cast<const ArrayLitExpr&>(*e);
        if (x.elements.empty()) {
          movImm(0, "%rdi");
          emit("    xor %esi, %esi\n    call a_list_new");
          return;
        }
        std::vector<int> es;
        for (auto& el : x.elements) es.push_back(evalSlot(el.get()));
        Argv av = allocArgv((int)es.size());
        for (std::size_t i = 0; i < es.size(); ++i) argvStore(av, (int)i, es[i]);
        movImm(es.size(), "%rdi");
        leaSlot(argvPtr(av), "%rsi");
        emit("    call a_list_new");
        // All-int literals pack into int64 element storage (the runtime
        // re-verifies every element is an int before converting).
        bool allInt = true;
        for (auto& el : x.elements)
          if (!exprIsInt(el.get())) { allInt = false; break; }
        if (allInt) {
          emit("\n    mov %rax, %rdi\n    call a_list_pack");
        }
        return;
      }
      case ExprKind::Tuple: {
        auto& x = static_cast<const TupleExpr&>(*e);
        if (x.elements.empty()) {
          movImm(0, "%rdi");
          emit("    xor %esi, %esi\n    call a_tuple_new");
          return;
        }
        std::vector<int> es;
        for (auto& el : x.elements) es.push_back(evalSlot(el.get()));
        Argv av = allocArgv((int)es.size());
        for (std::size_t i = 0; i < es.size(); ++i) argvStore(av, (int)i, es[i]);
        movImm(es.size(), "%rdi");
        leaSlot(argvPtr(av), "%rsi");
        emit("    call a_tuple_new");
        return;
      }
      case ExprKind::SetLit: {
        auto& x = static_cast<const SetLitExpr&>(*e);
        emit("    call a_set_new\n");
        int dS = allocSlot();
        storeRax(dS);
        for (auto& el : x.elements) {
          int es = evalSlot(el.get());
          call2slot("a_set_add", dS, es);
        }
        loadSlot(dS);
        return;
      }
      case ExprKind::MapLit: {
        auto& x = static_cast<const MapLitExpr&>(*e);
        emit("    call a_dict_new\n");
        int dS = allocSlot();
        storeRax(dS);
        for (auto& kv : x.entries) {
          int ks = evalSlot(kv.first.get());
          int vs = evalSlot(kv.second.get());
          call3slot("a_dict_set", dS, ks, vs);
        }
        loadSlot(dS);
        return;
      }
      case ExprKind::Call:
        emitCallExpr(static_cast<const CallExpr&>(*e));
        return;
      case ExprKind::SafeNav:
        emitSafeNav(static_cast<const SafeNavExpr&>(*e), e);
        return;
      case ExprKind::Try:
        emitTry(static_cast<const TryExpr&>(*e));
        return;
      case ExprKind::Comprehension:
        emitComprehension(static_cast<const ComprehensionExpr&>(*e));
        return;
      case ExprKind::Match:
        emitMatch(static_cast<const MatchExpr&>(*e));
        return;
      case ExprKind::Lambda:
        emitLambda(static_cast<const LambdaExpr&>(*e));
        return;
      default:
        fail("unsupported expression");
        movImm(kNone);
        return;
    }
  }

  // `obj?.field`: None propagates; Some(v) -> Some(v.field) (or v.field when
  // the field itself is optional). Mirrors the old C emitter.
  void emitSafeNav(const SafeNavExpr& x, const Expr* e) {
    int os = evalSlot(x.object.get());
    int otS = allocSlot(), resS = allocSlot();
    loadSlot(os);
    storeRax(otS);
    std::string noneL = newLabel("safen"), doneL = newLabel("safed");
    loadSlot(otS);
    movImm(kNone, "%rcx");
    emit("    cmp %rcx, %rax\n    je " + noneL + "\n");
    loadSlot(otS, "%rdi");
    emit("    call a_unwrap\n");
    int innerS = allocSlot();
    storeRax(innerS);
    loadSlot(innerS, "%rdi");
    std::string fl = emitStrData(x.field, true);
    emit("    lea " + fl + "(%rip), %rsi\n    call a_getattr\n");
    TypePtr t = resolveType(e->inferredType);
    TypePtr elem = t && t->kind == TypeKind::Optional ? resolveType(t->elem) : nullptr;
    bool fieldOptional = elem && elem->kind == TypeKind::Optional;
    if (!fieldOptional) {
      emit("    mov %rax, %rdi\n    call a_some_new\n");
    }
    storeRax(resS);
    emit("    jmp " + doneL + "\n");
    emitLabel(noneL);
    movImm(kNone);
    storeRax(resS);
    emitLabel(doneL);
    loadSlot(resS);
  }

  // Match expression: subject matches one of the arms; unguarded wildcard is default.
  void emitPatternTest(const Pattern& p, int subjS, const std::string& failL) {
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
      case Pattern::Kind::Binding:
        // Always matches.
        return;
      case Pattern::Kind::Literal: {
        int valS = evalSlot(p.value.get());
        call2slot("a_eq_slow", subjS, valS);
        emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n");
        emit("    jz " + failL + "\n");
        return;
      }
      case Pattern::Kind::Range: {
        int loS = evalSlot(p.rangeStart.get());
        int hiS = evalSlot(p.rangeEnd.get());
        call3slot("a_in_range_inclusive", subjS, loS, hiS);
        emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n");
        emit("    jz " + failL + "\n");
        return;
      }
      case Pattern::Kind::Variant: {
        int vS = evalSlot(p.ctor.get());
        if (p.subpatterns.empty()) {
          call2slot("a_eq_slow", subjS, vS);
          emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n");
          emit("    jz " + failL + "\n");
          return;
        }
        // Algebraic variant: dict with __tag.
        int tagKeyS = strLitSlot("__tag");
        int noneS = allocSlot();
        movImm(kNone, "%rcx");
        emit("    mov %rcx, " + slotRef(noneS));
        call3slot("a_dict_get", subjS, tagKeyS, noneS);
        int tagS = allocSlot();
        storeRax(tagS);
        call2slot("a_eq_slow", tagS, vS);
        emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n");
        emit("    jz " + failL + "\n");
        return;
      }
      case Pattern::Kind::Tuple: {
        call1slot("a_len", subjS);
        int lenS = allocSlot();
        storeRax(lenS);
        int expLenS = allocSlot();
        movImm(mkintA((int64_t)p.subpatterns.size()), "%rcx");
        emit("    mov %rcx, " + slotRef(expLenS));
        call2slot("a_eq_slow", lenS, expLenS);
        emit("    mov %rax, %rdi\n    call a_truthy_raw_slow\n    test %rax, %rax\n");
        emit("    jz " + failL + "\n");
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          int idxS = allocSlot();
          movImm(mkintA((int64_t)i), "%rcx");
          emit("    mov %rcx, " + slotRef(idxS));
          call2slot("a_getitem", subjS, idxS);
          int elemS = allocSlot();
          storeRax(elemS);
          emitPatternTest(p.subpatterns[i], elemS, failL);
        }
        return;
      }
      case Pattern::Kind::Or: {
        std::string orMatchL = newLabel("ormat");
        std::string orEndL = newLabel("orend");
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string altFailL = newLabel("altfail");
          emitPatternTest(p.subpatterns[i], subjS, altFailL);
          // If this alt matched, jump to orMatch
          jmp(orMatchL);
          emitLabel(altFailL);
        }
        // All alts failed -> failL
        jmp(failL);
        emitLabel(orMatchL);
        return;
      }
    }
  }

  void bindPatternVariables(const Pattern& p, int subjS) {
    switch (p.kind) {
      case Pattern::Kind::Binding: {
        int varS = allocSlot();
        loadSlot(subjS);
        storeRax(varS);
        bindLocal(p.name, varS);
        return;
      }
      case Pattern::Kind::Tuple: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          int idxS = allocSlot();
          movImm(mkintA((int64_t)i), "%rcx");
          emit("    mov %rcx, " + slotRef(idxS));
          call2slot("a_getitem", subjS, idxS);
          int elemS = allocSlot();
          storeRax(elemS);
          bindPatternVariables(p.subpatterns[i], elemS);
        }
        return;
      }
      case Pattern::Kind::Variant: {
        for (auto& sub : p.subpatterns) {
          if (sub.kind == Pattern::Kind::Binding) {
            int keyS = strLitSlot(sub.name);
            int noneS = allocSlot();
            movImm(kNone, "%rcx");
            emit("    mov %rcx, " + slotRef(noneS));
            call3slot("a_dict_get", subjS, keyS, noneS);
            int valS = allocSlot();
            storeRax(valS);
            bindLocal(sub.name, valS);
          }
        }
        return;
      }
      case Pattern::Kind::Or: {
        for (auto& sub : p.subpatterns) bindPatternVariables(sub, subjS);
        return;
      }
      default:
        return;
    }
  }

  // ---- lambdas / closures ---------------------------------------------------
  // Collect every identifier referenced anywhere inside `e` (over-collection
  // is safe: shadowed names simply produce unused env slots).
  void collectIdents(const Expr* e, std::set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
      case ExprKind::Ident:
        out.insert(static_cast<const IdentExpr&>(*e).name);
        return;
      case ExprKind::StringLit:
        for (const StringPart& p : static_cast<const StringLitExpr&>(*e).parts)
          if (p.isExpr) collectIdents(p.expr.get(), out);
        return;
      case ExprKind::Unary:
        collectIdents(static_cast<const UnaryExpr&>(*e).operand.get(), out);
        return;
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        collectIdents(b.lhs.get(), out);
        collectIdents(b.rhs.get(), out);
        return;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        collectIdents(c.callee.get(), out);
        for (const CallArg& a : c.args) collectIdents(a.value.get(), out);
        return;
      }
      case ExprKind::Index: {
        const IndexExpr& x = static_cast<const IndexExpr&>(*e);
        collectIdents(x.object.get(), out);
        collectIdents(x.index.get(), out);
        return;
      }
      case ExprKind::Slice: {
        const SliceExpr& x = static_cast<const SliceExpr&>(*e);
        collectIdents(x.object.get(), out);
        collectIdents(x.start.get(), out);
        collectIdents(x.end.get(), out);
        collectIdents(x.step.get(), out);
        return;
      }
      case ExprKind::MapLit:
        for (const auto& kv : static_cast<const MapLitExpr&>(*e).entries) {
          collectIdents(kv.first.get(), out);
          collectIdents(kv.second.get(), out);
        }
        return;
      case ExprKind::SetLit:
      case ExprKind::ArrayLit:
      case ExprKind::Tuple: {
        const auto& els = e->kind == ExprKind::SetLit
            ? static_cast<const SetLitExpr&>(*e).elements
            : e->kind == ExprKind::ArrayLit
                ? static_cast<const ArrayLitExpr&>(*e).elements
                : static_cast<const TupleExpr&>(*e).elements;
        for (const ExprPtr& el : els) collectIdents(el.get(), out);
        return;
      }
      case ExprKind::Field:
        collectIdents(static_cast<const FieldExpr&>(*e).object.get(), out);
        return;
      case ExprKind::SafeNav:
        collectIdents(static_cast<const SafeNavExpr&>(*e).object.get(), out);
        return;
      case ExprKind::Range: {
        const RangeExpr& r = static_cast<const RangeExpr&>(*e);
        collectIdents(r.start.get(), out);
        collectIdents(r.end.get(), out);
        collectIdents(r.step.get(), out);
        return;
      }
      case ExprKind::Ternary: {
        const TernaryExpr& t = static_cast<const TernaryExpr&>(*e);
        collectIdents(t.cond.get(), out);
        collectIdents(t.thenValue.get(), out);
        collectIdents(t.elseValue.get(), out);
        return;
      }
      case ExprKind::Lambda:
        collectIdents(static_cast<const LambdaExpr&>(*e).body.get(), out);
        return;
      case ExprKind::Comprehension: {
        const ComprehensionExpr& c = static_cast<const ComprehensionExpr&>(*e);
        collectIdents(c.element.get(), out);
        collectIdents(c.iterable.get(), out);
        collectIdents(c.condition.get(), out);
        return;
      }
      case ExprKind::Match: {
        const MatchExpr& m = static_cast<const MatchExpr&>(*e);
        collectIdents(m.subject.get(), out);
        for (const MatchArm& arm : m.arms) {
          collectPatternIdents(arm.pattern, out);
          collectIdents(arm.guard.get(), out);
          collectIdents(arm.result.get(), out);
        }
        return;
      }
      case ExprKind::Try:
        collectIdents(static_cast<const TryExpr&>(*e).operand.get(), out);
        return;
      default:
        return;
    }
  }

  void collectPatternIdents(const Pattern& p, std::set<std::string>& out) {
    if (p.value) collectIdents(p.value.get(), out);
    if (p.rangeStart) collectIdents(p.rangeStart.get(), out);
    if (p.rangeEnd) collectIdents(p.rangeEnd.get(), out);
    if (p.ctor) collectIdents(p.ctor.get(), out);
    for (const Pattern& sub : p.subpatterns) collectPatternIdents(sub, out);
  }

  // Creation site: build the closure env, fill captures, wrap in a FNON.
  void emitLambda(const LambdaExpr& x) {
    PendingLambda pl;
    pl.expr = &x;
    pl.moduleId = curModuleId_;
    pl.cname = "__lambda_" + std::to_string(lambdaCounter_++);
    std::set<std::string> names;
    collectIdents(x.body.get(), names);
    for (const std::string& nm : names) {
      if (nm == "None" || nm == "PI" || nm == "E") continue;
      ABinding* b = lookup(nm);
      if (!b) continue;
      if (b->kind == BKind::Local || b->kind == BKind::Param ||
          b->kind == BKind::Capture) {
        pl.captures.push_back({nm, *b});
      }
    }
    int n = (int)pl.captures.size();
    if (n > 0) {
      movImm((uint64_t)n, "%rdi");
      emit("    call a_closure_env_new\n");
      int envS = allocSlot();
      storeRax(envS);
      for (int i = 0; i < n; ++i) {
        loadSlot(envS, "%rdi");
        movImm((uint64_t)i, "%rsi");
        emitBindingValue(pl.captures[i].second);  // rax = captured value
        emit("    mov %rax, %rdx\n    call a_env_set\n");
      }
      loadSlot(envS);  // rax = env aval
    } else {
      movImm(kNone);   // rax = A_NONE env
    }
    // a_fnon_new(entry, env, name): rdi=code, rsi=env, rdx=name.
    emit("    mov %rax, %rsi\n");
    emit("    lea " + pl.cname + "(%rip), %rdi\n");
    std::string nl = emitStrData(pl.cname, true);
    emit("    lea " + nl + "(%rip), %rdx\n    call a_fnon_new\n");
    lambdas_.push_back(std::move(pl));
  }

  // Definition site: emit __lambda_N like a normal function whose body is
  // the lambda expression; argv[0] is the closure env, argv[1..] the params.
  void emitLambdaFn(const PendingLambda& pl) {
    const LambdaExpr& x = *pl.expr;
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    bindModuleImports(pl.moduleId);
    int argvSlot = allocSlot();
    emit("    mov %rdi, " + slotRef(argvSlot) + "\n");
    int envS = allocSlot();
    loadSlot(argvSlot, "%rax");
    emit("    mov (%rax), %rax\n    mov %rax, " + slotRef(envS) + "\n");
    for (std::size_t i = 0; i < pl.captures.size(); ++i) {
      ABinding cb;
      cb.kind = BKind::Capture;
      cb.slot = envS;
      cb.builtinId = (int)i;
      scopes_.back()[pl.captures[i].first] = cb;
    }
    for (std::size_t i = 0; i < x.params.size(); ++i) {
      int ps = allocSlot();
      loadSlot(argvSlot, "%rax");
      emit("    mov " + std::to_string(8 * (i + 1)) + "(%rax), %rax\n    mov %rax, " + slotRef(ps) + "\n");
      ABinding b;
      b.kind = BKind::Param;
      b.slot = ps;
      scopes_.back()[x.params[i].name] = b;
    }
    int savedModule = curModuleId_;
    curModuleId_ = pl.moduleId;
    bool savedInFn = inFunction_;
    inFunction_ = true;
    promoteNames_.clear();  // lambda bodies are expressions; no raw locals
    specNames_.clear();
    nameRanges_.clear();
    emitExpr(x.body.get());
    emit("    leave\n    ret\n");
    int frame = frameBytes();
    std::ostringstream emitted;
    emitted.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl " + pl.cname + "\n" + pl.cname + ":\n");
    emit("    push %rbp\n    mov %rsp, %rbp\n    sub $" + std::to_string(frame) + ", %rsp\n");
    text_ << emitted.str();
    scopes_.clear();
    curModuleId_ = savedModule;
    inFunction_ = savedInFn;
  }

  void emitMatch(const MatchExpr& m) {
    int subjS = evalSlot(m.subject.get());
    int resS = allocSlot();
    std::string endL = newLabel("matchend");
    bool hasCatchAll = false;

    for (const MatchArm& arm : m.arms) {
      std::string nextArmL = newLabel("armfail");
      pushScope();
      emitPatternTest(arm.pattern, subjS, nextArmL);
      bindPatternVariables(arm.pattern, subjS);
      if (arm.guard) {
        condJump(arm.guard.get(), nextArmL, false);
      }
      int rvS = evalSlot(arm.result.get());
      loadSlot(rvS);
      storeRax(resS);
      popScope();
      jmp(endL);
      emitLabel(nextArmL);
      if (arm.pattern.kind == Pattern::Kind::Wildcard && !arm.guard) {
        hasCatchAll = true;
      }
    }

    if (!hasCatchAll) {
      loadSlot(subjS, "%rdi");
      emit("    call a_match_fail\n");
      movImm(kNone);
      storeRax(resS);
    }

    emitLabel(endL);
    loadSlot(resS);
  }

  // `[f(x) for x in it if c]` / `{...}`: build via a_iter_new/a_iter_next and
  // a_list_push/a_set_add, mirroring the C emitter's loop. Array/tuple-typed
  // iterables get the same indexed fast loop as `for` (with generic
  // fallback); everything else uses the iterator tail directly.
  void emitComprehension(const ComprehensionExpr& x) {
    int iv = evalSlot(x.iterable.get());
    TypePtr itT = resolveType(x.iterable->inferredType);
    if (itT && (itT->kind == TypeKind::Array || itT->kind == TypeKind::Tuple)) {
      std::string fastL = newLabel("cfast"), slowL = newLabel("cslow"),
              fastEndL = newLabel("cfend"), mergedL = newLabel("cend");
      loadSlot(iv);
      emit("    mov %rax, %rcx\n    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n"
           "    jne " + slowL + "\n");
      emit("    mov %rax, %rcx\n    and $0xF, %rcx\n    jnz " + slowL + "\n");
      emit("    mov %rax, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n");
      emit("    mov (%rcx), %eax\n    cmp $2, %eax\n    je " + fastL + "\n"
           "    cmp $3, %eax\n    je " + fastL + "\n"
           "    cmp $5, %eax\n    je " + fastL + "\n"
           "    jmp " + slowL + "\n");
      emitLabel(fastL);
      int iS = allocSlot(), accS = allocSlot(), varS = allocSlot();
      movImm(0, "%rcx");
      emit("    mov %rcx, " + slotRef(iS));
      if (x.isSet) {
        emit("    call a_set_new\n");
      } else {
        movImm(0, "%rdi");
        movImm(0, "%rsi");
        emit("    call a_list_new\n");
      }
      storeRax(accS);
      pushScope();
      movImm(kNone, "%rcx");
      emit("    mov %rcx, " + slotRef(varS));
      bindLocal(x.var, varS);
      std::string loopL = newLabel("comp"), contL = newLabel("compc");
      emitLabel(loopL);
      emit("    mov " + slotRef(iv) + ", %rax\n"
           "    mov %rax, %rcx\n    shl $16, %rcx\n    shr $16, %rcx\n"
           "    mov 4(%rcx), %edx\n"
           "    mov " + slotRef(iS) + ", %rax\n    cmp %rdx, %rax\n    jae " +
           fastEndL + "\n");
      // Packed-int source: flag bit0 of AArr.flags (@32) selects the
      // int64 element buffer (@24); the value is boxed for the var slot.
      // Checked per pass so mid-iteration packed<->aval conversion is
      // observed (element exprs may call back into the array).
      {
        std::string pkL = newLabel("cpkp"), avL = newLabel("cpav"),
                bodyL = newLabel("cpbd");
        emit("    mov 32(%rcx), %r8d\n    test $1, %r8d\n    jnz " + pkL +
             "\n    jmp " + avL + "\n");
        emitLabel(avL);
        emit("    mov 16(%rcx), %rcx\n    mov (%rcx,%rax,8), %rax\n"
             "    mov %rax, " + slotRef(varS) + "\n    jmp " + bodyL + "\n");
        emitLabel(pkL);
        emit("    mov 24(%rcx), %rcx\n    mov (%rcx,%rax,8), %rax\n");
        emitBoxInt64();
        emit("    mov %rax, " + slotRef(varS) + "\n");
        emitLabel(bodyL);
      }
      if (x.condition) {
        condJump(x.condition.get(), contL, false);
      }
      {
        int es = evalSlot(x.element.get());
        loadSlot(accS, "%rdi");
        loadSlot(es, "%rsi");
        emit(std::string("    call ") + (x.isSet ? "a_set_add" : "a_list_push") + "\n");
      }
      emitLabel(contL);
      emit("    addq $1, " + slotRef(iS) + "\n");
      jmp(loopL);
      emitLabel(fastEndL);
      loadSlot(accS);
      jmp(mergedL);
      emitLabel(slowL);
      emitComprehensionIterTail(iv, x, mergedL);
      emitLabel(mergedL);
      return;
    }
    std::string doneL = newLabel("compd");
    emitComprehensionIterTail(iv, x, doneL);
    emitLabel(doneL);
  }

  // Generic comprehension loop over the iterable held in slot `iv`;
  // jumps to mergedL when exhausted.
  void emitComprehensionIterTail(int iv, const ComprehensionExpr& x,
                                 const std::string& mergedL) {
    int itS = allocSlot(), accS = allocSlot(), varS = allocSlot();
    loadSlot(iv, "%rdi");
    emit("    call a_iter_new\n");
    storeRax(itS);
    if (x.isSet) {
      emit("    call a_set_new\n");
    } else {
      movImm(0, "%rdi");
      movImm(0, "%rsi");
      emit("    call a_list_new\n");
    }
    storeRax(accS);
    pushScope();
    movImm(kNone, "%rcx");
    emit("    mov %rcx, " + slotRef(varS));
    bindLocal(x.var, varS);
    std::string loopL = newLabel("comp"), contL = newLabel("compc"),
            endL = newLabel("compd");
    emitLabel(loopL);
    loadSlot(itS, "%rdi");
    emit("    lea " + slotRef(varS) + ", %rsi\n    call a_iter_next\n    test %rax, %rax\n");
    emit("    jz " + endL + "\n");
    if (x.condition) {
      condJump(x.condition.get(), contL, false);
    }
    {
      int es = evalSlot(x.element.get());
      loadSlot(accS, "%rdi");
      loadSlot(es, "%rsi");
      emit(std::string("    call ") + (x.isSet ? "a_set_add" : "a_list_push") + "\n");
    }
    emitLabel(contL);
    jmp(loopL);
    emitLabel(endL);
    popScope();
    loadSlot(accS);
    jmp(mergedL);
  }

  // `expr?`: unwrap ok/err; a propagating value returns early from the
  // enclosing function (defers run when Defer lands).
  void emitTry(const TryExpr& x) {
    int vs = evalSlot(x.operand.get());
    int tS = allocSlot();
    loadSlot(vs, "%rdi");
    emit("    call a_try_result\n");
    storeRax(tS);
    if (inFunction_) {
      std::string doneL = newLabel("tryok");
      loadSlot(tS, "%rdi");
      emit("    call a_is_propagate\n    test %eax, %eax\n    jz " + doneL + "\n");
      if (inlineDepth_ > 0) {
        emitDeferredPeekDownTo(inlineDeferBase_);
        copySlot(tS, inlineResultSlot_);
        jmp(inlineReturnLabel_);
      } else {
        emitDeferredPeekAll();
        loadSlot(tS);
        emit("    leave\n    ret\n");
      }
      emitLabel(doneL);
    }
    loadSlot(tS);
  }

  // Binary exprs that are not plain (l,r) runtime calls.
  void emitBinary(const BinaryExpr& x) {
    const std::string& op = x.op;
    if (op == "??") {
      int ls = evalSlot(x.lhs.get());
      std::string rhsL = newLabel("qqrhs");
      std::string endL = newLabel("qqend");
      loadSlot(ls);
      movImm(kNone, "%rcx");
      emit("    cmp %rcx, %rax\n    jne " + rhsL + "\n");
      emitExpr(x.rhs.get());
      jmp(endL);
      emitLabel(rhsL);
      loadSlot(ls, "%rdi");
      emit("    call a_unwrap");
      emitLabel(endL);
      return;
    }
    if (op == "in" || op == "not in") {
      int ls = evalSlot(x.lhs.get());
      int rs = evalSlot(x.rhs.get());
      call2slot("a_contains", rs, ls);  // a_contains(cont=r, item=l)
      if (op == "not in") {
        emit("    mov %rax, %rdi\n    call a_not");
      }
      return;
    }
    if (op == "..") {
      int ls = evalSlot(x.lhs.get());
      int rs = evalSlot(x.rhs.get());
      int one = allocSlot();
      movImm(mkintA(1), "%rcx");
      emit("    mov %rcx, " + slotRef(one));
      call3slot("a_range_new", ls, rs, one);
      return;
    }
    // Raw int64 fast path: safe-op binaries (& | ^ << >> %) whose operands
    // are all raw-materializable compute without ever boxing to avals
    // (promoted locals copy directly; other ints unbox inline). The result
    // is boxed once here. Ops that can yield a double (+ - * /) never take
    // this path.
    if (rawBinFastOk(x)) {
      int ls = allocSlot(), rs = allocSlot(), out = allocSlot();
      ++rawRegDepth_;
      rawRegOf_.clear();
      emitRawExpr(x.lhs.get(), ls);
      emitRawExpr(x.rhs.get(), rs);
      emitRawBin(op, ls, rs, out);
      loadSlot(out);  // usually forwards: %rax already holds the result
      --rawRegDepth_;
      rawRegOf_.clear();
      emitBoxInt64();
      return;
    }
    int ls = evalSlot(x.lhs.get());
    int rs = evalSlot(x.rhs.get());
    emitArith(op, ls, rs);
  }

  // Field access expr: module-qualified names, enum variants, getattr.
  void emitField(const Expr* e) {
    const FieldExpr& x = static_cast<const FieldExpr&>(*e);
    // Module-qualified chain: c.Color.Red, c.LIMIT, mod.fn (as value)
    {
      std::vector<std::string> chain;
      const Expr* cur = e;
      while (cur && cur->kind == ExprKind::Field) {
        chain.push_back(static_cast<const FieldExpr*>(cur)->field);
        cur = static_cast<const FieldExpr*>(cur)->object.get();
      }
      if (cur && cur->kind == ExprKind::Ident && !chain.empty()) {
        std::reverse(chain.begin(), chain.end());
        const std::string& first = static_cast<const IdentExpr*>(cur)->name;
        ABinding* mb = lookup(first);
        if (mb && mb->kind == BKind::Module && mb->moduleId >= 0) {
          if (chain.size() >= 2) {
            auto it = enums_.find(chain[chain.size() - 2] + "." + chain.back());
            if (it != enums_.end()) {
              EnumVariantInfo ev = it->second;
              if (ev.algebraic) {
                emitVariantDict(ev, chain.back());
              } else {
                emit("    mov " + ev.globalVar + "(%rip), %rax");
              }
              return;
            }
          }
          if (chain.size() == 1) {
            auto it = topLevelGlobals_.find(std::to_string(mb->moduleId) + ":" + chain[0]);
            if (it != topLevelGlobals_.end()) {
              emit("    mov " + it->second + "(%rip), %rax");
              return;
            }
            for (FnInfo* fi : fnList_) {
              if (!fi->isMethod && fi->moduleId == mb->moduleId && fi->name == chain[0]) {
                emit("    mov " + fi->globalVar + "(%rip), %rax");
                return;
              }
            }
            for (StructInfo* st : structList_) {
              if (st->moduleId == mb->moduleId && st->name == chain[0]) {
                emit("    mov " + st->globalVar + "(%rip), %rax");
                return;
              }
            }
          }
          fail("unknown module member '" + chain.back() + "'");
          movImm(kNone);
          return;
        }
        if (mb && mb->kind == BKind::Module && mb->moduleId < 0) {
          // Native module constant: math.PI, math.E, ...
          if (chain.size() == 1) {
            if (const std::string* cn = nativeConstant(mb->aux, chain[0])) {
              double v = *cn == "PI"   ? 3.14159265358979323846
                        : *cn == "E"    ? 2.71828182845904523536
                                        : 0.0;
              movImm(dblBits(v));
              return;
            }
          }
          fail("unknown native module member '" + chain.back() + "'");
          movImm(kNone);
          return;
        }
      }
    }
    // Enum variant: Color.Red
    if (x.object->kind == ExprKind::Ident) {
      const std::string& oname = static_cast<const IdentExpr&>(*x.object).name;
      auto it = enums_.find(oname + "." + x.field);
      if (it != enums_.end()) {
        if (it->second.algebraic) {
          emitVariantDict(it->second, x.field);
        } else {
          emit("    mov " + it->second.globalVar + "(%rip), %rax");
        }
        return;
      }
      ABinding* mb = lookup(oname);
      if (mb && mb->kind == BKind::Module) {
        auto it2 = enums_.find(x.field);
        if (it2 != enums_.end()) {
          if (it2->second.algebraic) {
            emitVariantDict(it2->second, x.field);
          } else {
            emit("    mov " + it2->second.globalVar + "(%rip), %rax");
          }
          return;
        }
      }
    }
    int os = evalSlot(x.object.get());
    std::string lbl = emitStrData(x.field, true);
    loadSlot(os, "%rdi");
    emit("    lea " + lbl + "(%rip), %rsi\n    call a_getattr");
  }
};

}  // namespace

bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode) {
  AsmEmitter emitter(graph, sema, testMode);
  return emitter.run(out, error);
}

}  // namespace a
