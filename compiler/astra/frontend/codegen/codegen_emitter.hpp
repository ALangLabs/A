#pragma once

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
#include "codegen/codegen.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

namespace a::detail {


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
  ~AsmEmitter() {
    for (FnInfo* f : fnList_) delete f;
    for (StructInfo* s : structList_) delete s;
  }

    bool run(std::string& out, std::string& error);

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
  // Body of the function currently being emitted (null for entry-stmt
  // emission); strength reduction scans pre-loop writes through it.
  const std::vector<StmtPtr>* curFnBody_ = nullptr;
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
    bool rawRegOn() const;
    int rawRegCode(const char* reg) const;
  // Register r was overwritten: forget every slot->r claim.
    void rawDropReg(int r);
    void rawInvAll();
  // Slot's memory now holds the bits in register r; drop the slot's stale
  // claims in other registers (its old contents are gone).
    void rawLearn(int slot, int r);
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

    void loopXmmFlush();
  // Reload resident values after a call: slots were flushed before it, so
  // movq m64->xmm restores each register without needing a scratch GPR
  // (which could hold the call's result).
    void loopXmmReload();

  // Install every in-scope raw-int/spec binding (up to xmm15) into an xmm
  // register for the duration of a pure loop body. Returns a marker for
  // loopXmmExit. Bindings not touched by the body simply waste a register;
  // correctness never depends on use.
    int loopXmmEnter();

  // Flush resident values back to their slots and drop this loop level's
  // residency (outer loops' entries survive for nested loops).
    void loopXmmExit(int marker);

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

    void fail(const std::string& msg);
    void emit(const std::string& s);
    std::string u64s(uint64_t v);
    std::string slotRef(int i);
    int allocSlot();
    int frameBytes() const;
    std::string newLabel(const std::string& pfx);
    void emitLabel(const std::string& l);
    void jmp(const std::string& l);

  // ---- value materialization ----------------------------------------------
    uint64_t mkintA(int64_t v) const;
  // Integer literal -> aval, mirroring mkint_full: the inline 47-bit
  // encoding only covers [-2^46, 2^46); wider values must box as T_INT64
  // (mkintA alone would truncate the payload and miscompile the sign).
    void emitIntLitA(std::int64_t v);
    void movImm(uint64_t v, const char* reg = "%rax");
    void storeRax(int slot);
    void loadSlot(int slot, const char* reg = "%rax");
    void leaSlot(int slot, const char* reg);
  // mem -> mem is impossible on x86; route slot copies through %rcx.
    void copySlot(int from, int to);
    uint64_t dblBits(double d) const;

  // Byte string in .rodata (.byte list: safest for the astra assembler).
  // Returns the label; bytes are NOT escaped so any content works.
    std::string emitStrData(const std::string& bytes, bool nulTerm);

  // Panic-traceback frames: emit a_frame_push(name, file, line) / a_frame_pop
  // calls. Push goes in the prologue (after argv is saved to its slot);
  // pop goes before every `leave; ret` while the return value is still in a
  // stack slot, not %rax (a_frame_pop clobbers caller-saved registers).
    void emitFramePush(const std::string& name, int moduleId, int line);
    void emitFramePop();

  // aval string value -> fresh slot holding a_str_new(ptr, len).
    int strLitSlot(const std::string& text);

  // ---- frame argv arrays -----------------------------------------------------
  // An argv block is `count` consecutive slots [base, base+count). Because
  // slot addresses decrease with index, argv[0] (lowest address) is the
  // highest-index slot: argv[k] -> slot(base + count - 1 - k).
  struct Argv {
    int base = 0;
    int n = 0;
  };
    Argv allocArgv(int count);
    int argvSlot(const Argv& a, int k) const;
    int argvPtr(const Argv& a) const;
    void argvStore(const Argv& a, int k, int valSlot);
    void argvStoreImm(const Argv& a, int k, uint64_t v);

  // ---- slots / bindings ------------------------------------------------------
    void pushScope();
    void popScope();
    ABinding* lookup(const std::string& name);
    void bindLocal(const std::string& name, int slot);

  // Evaluate expr -> fresh slot holding the aval (null expr -> A_NONE slot).
    int evalSlot(const Expr* e);
  // Raw int64 of an aval slot -> fresh slot (inline unbox; a_int_full on
  // non-int, preserving its panic behaviour).
    int int64Slot(int avalS);

    void storeBinding(const ABinding& b, int vs);
    void emitBindingValue(const ABinding& b);

    static const char* binopSlow(const std::string& op);
  // f(lSlot, rSlot) -> rax, for runtime binary helpers taking (aval,aval).
    void call2slot(const char* sym, int a, int b);
    void call3slot(const char* sym, int a, int b, int c);
    void call4slot(const char* sym, int a, int b, int c, int d);
  // f(aval) -> rax from a slot.
    void call1slot(const char* sym, int a);

  // ---- inline numeric fast paths ------------------------------------------
  // Slots keep holding avals; arithmetic/comparison first tries to unbox
  // both operands to raw int64 (inline 47-bit int or boxed T_INT64 heap
  // object, mirroring is_int_val/int_val_full) and emits native x86 ops,
  // falling back to the *_slow runtime calls whenever the fast path does
  // not apply. %rdx must hold kTagInt on entry to emitUnboxInt/emitBoxInt64.

  // %rdi = aval -> int64 in `dst`; jumps to slowL unless the value is an
  // integer. Clobbers %rax, %rcx and `dst`; preserves %rdx, %rsi.
  
  void emitUnboxInt(const char* dst, const std::string& slowL);

  // %rdi = aval -> %rax = int64, equivalent to a_int_full but with the
  // inline-int and boxed-T_INT64 cases inlined (mirroring emitUnboxInt).
  // Anything else (doubles, strings, ...) falls back to the runtime call,
  // preserving its panic behaviour exactly.
    void emitAvalToI64();

  // %rax = int64 -> %rax = aval; equivalent to mkint_full (inline ints cover
  // [-2^46, 2^46), wider values box as T_INT64 via a runtime call).
  // Loads kTagInt into %rdx itself: callers must NOT rely on %rdx surviving
  // (idiv/cqto leave the remainder there). Clobbers %rcx, %rdx; %rdi only on
  // the call path.
    void emitBoxInt64();

    void emitPanicStr(const std::string& msg);

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
    void emitDoublePairCheck(int ls, int rs, const std::string& slowL);

  // Stores the low 64 bits of %xmm0 into %rax, canonicalizing a NaN result
  // to kCanonNaN (ucomisd with itself sets PF iff the value is unordered,
  // i.e. NaN — 2 instructions to detect, matching a_double_canon exactly).
    void emitXmm0ToAval(int scratch, const std::string& doneL);

  // Double*double arithmetic with inline SSE2: both operands must be
  // untagged doubles (checked via emitDoublePairCheck), then addsd/subsd/
  // mulsd/divsd compute the IEEE result. Division by zero (either sign of
  // zero) jumps to the shared divzL panic block — identical to the
  // a_div/a_div_slow check `b == 0.0`. % / ** / mixed types stay on the
  // slow path (fmod/pow semantics and mixed-type promotion live there).
    void emitDoubleArith(const std::string& op, int ls, int rs,
                       const std::string& slowL, const std::string& doneL,
                       const std::string& divzL);

  // Double*double comparisons with inline ucomisd. ucomisd sets
  // CF/ZF/PF with PF=1 on unordered (NaN) operands; the cc sequences below
  // reproduce the C semantics used by a_lt/a_le/a_gt/a_ge/a_eq/a_ne:
  // every ordered comparison involving NaN is false, != is true, and
  // -0.0 == +0.0. Operand order is chosen per op so a single setcc (plus a
  // parity fixup for ==/!=) excludes the unordered case. Non-double
  // operands (incl. mixed int/double, which the runtime promotes) jump to
  // slowL.
    void emitDoubleCmp(const std::string& op, int ls, int rs,
                     const std::string& slowL, const std::string& doneL);

  // Binary arith/compare with inline int64 (and double*double) fast paths.
  // ls/rs are slots holding avals; result aval is left in %rax.
    void emitArith(const std::string& op, int ls, int rs);

  // `a <cmp> b` with int operands materialized as raw int64 and compared
  // directly: comparisons cannot trap or overflow, so both sides only need
  // the rawBinFastOk operand gate (safe-op restriction does not apply).
    bool rawCmpOk(const BinaryExpr& b);

  // Structural mirror of tryRawCondJump for the promotion coverage scan.
    bool rawCondOk(const Expr* e);

  // Raw int64 condition jump: int comparisons, `not e`, and bare int exprs
  // (truthy iff != 0) branch without materializing the bool aval. Anything
  // else falls through to the generic truthiness path in condJump.
    bool tryRawCondJump(const Expr* cond, const std::string& label,
                      bool jumpIfTrue);

  // Jump helper: evaluate cond expr -> truthy test -> jcc.
  // Fast paths cover A_TRUE/A_FALSE/A_NONE, inline ints, and untagged
  // doubles inline; everything else (boxed ints, strings, objects) goes
  // through a_truthy_raw_slow.
    void condJump(const Expr* cond, const std::string& label, bool jumpIfTrue);

  // Build the algebraic-enum variant dict ({__tag, __name}) into %rax.
    void emitVariantDict(const EnumVariantInfo& ev, const std::string& name);

  // ---- registration (mirrors Emitter) --------------------------------------
    void collectGlobals();

    std::int64_t evalConstIntA(const Expr* e, std::int64_t fallback);

    void buildGlobalScope();

  // ---- functions / main -------------------------------------------------------
  // Bind `from <native-module> import member [as x]` items into the current
  // scope: functions become BKind::Builtin (dispatched via a_bi_call),
  // constants become BKind::NativeConst. Members missing from the
  // nativeModules() spec are skipped here; sema already reported them.
    void bindNativeFromImport(const FromImportStmt* im);

  // Register a module's import aliases (import x / from x import y) into the
  // current scope so function bodies can resolve module.member calls.
    void bindModuleImports(int moduleId);

    void emitStructCtor(StructInfo* st);

    void emitFunction(FnInfo* f);

    void emitMain();

  // ---- statements -------------------------------------------------------------
  // Emit deferred statements of frames [depth, size) innermost-first, LIFO
  // within each frame. Frames are moved out so a `defer` inside a defer body
  // registers on the parent frame instead of invalidating iteration.
  // If `type` is a struct with a `drop` method, return its FnInfo.
    FnInfo* dropFnFor(TypePtr type);

    void emitDropNow(int slot, FnInfo* fn);
    void emitDropRec(const DropRec& d);
  // Find the pending drop record for a binding across all live frames.
    DropRec* findDropRec(const ABinding& b);

    void emitDeferredDownTo(int depth);
    void emitDeferredAll();

  // Return/try-propagate path: emit deferred statements of frames [depth,
  // size) innermost-first, LIFO within each frame, WITHOUT popping — later
  // code in the same function still needs its frames. Inlining uses `depth`
  // to run only the callee's frames and leave the caller's defers untouched.
    void emitDeferredPeekDownTo(int depth);
    void emitDeferredPeekAll();

    void emitBody(const std::vector<StmtPtr>& stmts, bool topLevel = false);

    void emitStmt(const Stmt* s, bool topLevel = false);

  // cur += step with INT64 saturation toward `end` (mirrors the C emitter's
  // range fast path). curS/endS/stepS hold raw int64 (not avals).
    void emitRangeIncr(int curS, int endS, int stepS);

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
    bool scanExprPure(const Expr* e, std::unordered_set<std::string>* writes);

    bool scanPatternPure(const Pattern& p,
                       std::unordered_set<std::string>* writes);

    bool scanStmtsPure(const std::vector<StmtPtr>& stmts,
                     std::unordered_set<std::string>* writes);

    bool scanStmtPure(const Stmt* s,
                    std::unordered_set<std::string>* writes);

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
    bool fillValueOk(const Expr* e) const;

  // `for i in lo..hi[..step]` whose entire body is `xs[i] = v` with v a
  // loop-invariant scalar lowers to one a_list_fill_range call instead of
  // the per-element loop. curS/endS/stepS are the raw int64 range slots
  // already computed by emitFor; xs and v are evaluated once (both are
  // loop-invariant: xs is a plain ident, v passes fillValueOk and names
  // neither i nor xs). The runtime helper reproduces a_setitem's
  // normalize_index panics element-by-element.
    bool tryEmitForFill(const ForStmt& st, int curS, int endS, int stepS);

  // Strength reduction: `for i in lo..hi: acc += i` (or `acc = acc + i`,
  // optionally scaled by a non-negative constant and summed with += forms
  // on both sides of the assignment) folds into the closed form
  // `acc += c * N * (lo + hi - 1) / 2` with N = max(0, hi - lo), for
  // step-1 range loops. curS/endS/stepS are the evaluated int64 range
  // slots from emitFor. The fold is only emitted when range analysis
  // proves the whole accumulation stays inside int64 (A promotes an
  // overflowing int + to a double, which would hide behind a closed
  // form), so iteration-by-iteration semantics equal the closed form
  // bit-for-bit. The accumulator may be a plain promoted raw local or a
  // speculative one (specRaw); the demoted branch goes through
  // a_add_slow so a live aval accumulator sees the same value either way.
    bool tryEmitForAccum(const ForStmt& st, int curS, int endS, int stepS);

  // Upper bound on the accumulator's value at the start of the loop:
  // union of the ranges of every `=`-style write to it that textually
  // precedes the loop in the current function body. Returns false when
  // the value cannot be bounded that way (compound writes to the name,
  // destructuring, or no writes at all). Used by tryEmitForAccum, where
  // fn-wide nameRanges_ (union over the whole function) is useless for
  // accumulators: the fixpoint there widens them to unknown.
    bool accEntryRange(const std::string& accName, const ForStmt& st,
                       ARange& out) const;

    void emitFor(const ForStmt& st);

  // Generic `for x in <itV-slot>` loop via a_iter_new/a_iter_next; jumps to
  // mergedL when done so callers can share a continuation label.
    void emitForIterTail(int itV, const ForStmt& st, const std::string& mergedL);

  // Shared tail of an emitForArray fast loop: binds the loop variable,
  // pushes break/continue labels, emits the body, bumps the index and
  // jumps back. `varRaw` marks the binding rawInt (slot holds int64).
    void emitForArrayLoopTail(const ForStmt& st, int iS, int varS,
                            const std::string& loopL,
                            const std::string& contL,
                            const std::string& endL, bool varRaw);

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
    void emitForArray(const ForStmt& st);

  // Allocate the demotion flag (0 = raw live) and aval slot for a
  // speculative accumulator binding. The aval slot stays uninitialized
  // until the first deopt or aval write — unreachable while flag == 0.
    void initSpecSlots(ABinding* b);

  // Speculative accumulator write (see ABinding::specRaw). Every access
  // checks specFlag: while 0 the raw slot is authoritative and +,-,* ops
  // run with `jo` into a deopt that recomputes through the aval slow path,
  // stores the aval result in specAval and latches flag = 1 forever; once
  // 1, writes go straight to the aval slot. Non-arith writes simply latch
  // the flag after storing the aval.
    void emitSpecAssign(ABinding* b, const AssignStmt& st);

    void emitAssign(const AssignStmt& st);

  // ---- small-function inlining ---------------------------------------------
  // The scanner is deliberately conservative: it only proves that emitting the
  // body at a direct call site cannot change name resolution, control flow, or
  // ownership behaviour. Anything doubtful disables inlining for that fn.
    bool exprIsInt(const Expr* e) const;

    bool exprIsFloat(const Expr* e) const;

    void scanInlineExpr(InlineScan& sc, const Expr* e);

    void scanInlinePattern(InlineScan& sc, const Pattern& p);

    void scanInlineStmt(InlineScan& sc, const Stmt* s);

  // Conservative check for whether an otherwise-undeclared assignment target
  // could resolve to a global/function/struct/enum binding rather than a fresh
  // local. Used only by the raw-int path; false positives just disable it.
    bool rawNameMayBeGlobal(const std::string& nm) const;

    bool rawInlineShapeOk(const FnDecl* fn) const;

    bool allIntParamsAndReturn(const FnDecl* fn) const;

    const InlineInfo& inlineInfoFor(FnInfo* f);

  // Raw int64 helpers used only while emitting a raw-inline body. Slots in
  // rawVars_ hold untagged int64 values, not avals.
    bool rawIntLitFits(std::int64_t v) const;

  // ---- Phase 5: raw int64 expressions & scalar promotion -------------------
  // Ops whose int64 result is always an integer (or a panic): bitwise, shifts
  // (panic on out-of-range amount), '~', and '%'. '+', '-', '*', '/' are
  // excluded on purpose: int64 overflow (and INT64_MIN / -1) produces a
  // *double* aval that a raw int64 slot cannot represent, so those stay on
  // the aval path where the slow helpers implement overflow->double.
    static bool rawSafeBinOp(const std::string& op);

  // Direct-call target eligible for raw (int64) inlining, or nullptr.
  // Mirrors the gates in tryEmitInlineCall; used by both the emitter and the
  // promotion analysis so they always agree.
    FnInfo* rawCallTarget(const CallExpr& call);

  // Structural check mirroring emitRawExpr's capabilities. Ident is always
  // accepted: bindings that are not raw-int fall back to aval evaluation +
  // a_int_full (the same unbox the Phase-4 raw path already used).
    bool rawSafeExpr(const Expr* e);

  // emitBinary fast-path gate: a safe-op binary whose operands are all raw
  // int64 materializable. Kept in sync with the emitBinary call site and the
  // promotion coverage scan.
    bool rawBinFastOk(const BinaryExpr& b);

  // Raw-expression entry point. Each top-level call opens a fresh
  // register-cache region; nested calls (rawRegDepth_ > 0) share the
  // caller's cache so intermediates forward through the whole tree. The
  // cache is dropped on exit so no facts leak into unmodeled code.
    bool emitRawExpr(const Expr* e, int out);

    bool emitRawExprImpl(const Expr* e, int out);

    void emitRawBin(const std::string& op, int ls, int rs, int out);

  // x % C with C a positive int32 constant and x provably non-negative:
  // unsigned magic-multiply division (M = floor(2^64/C)) replaces idiv.
  // q = mulhi(x, M) is floor(x/C) or one less; r = x - q*C in [0, 2C) is
  // fixed by a single conditional subtract.
    bool emitRawModConst(const BinaryExpr& b, int ls, int rs, int out);

  // argsRaw: argSlots already hold raw int64 (skip the aval->int64 unbox).
  // keepRaw: leave the raw int64 result in %rax instead of boxing to aval.
  // rawVars_ is saved/restored so raw-inline calls can nest inside
  // emitRawExpr (e.g. `f(g(x))` or a promoted `x = f(y)`).
    void emitRawBody(FnInfo* f, const std::vector<int>& argSlots,
                   bool argsRaw = false, bool keepRaw = false);

  // Rebuild a function-like scope for the callee so caller locals/params can
  // never be captured by name lookup. Returns the saved scope stack in
  // `saved`; caller restores it after emission.
    void beginInlineScope(FnInfo* f,
                        std::vector<std::unordered_map<std::string, ABinding>>& saved);

    void endInlineScope(
      std::vector<std::unordered_map<std::string, ABinding>>& saved);

    bool callArgsAllInt(const CallExpr& call) const;

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
    static __int128 rngI64Min();
    static __int128 rngI64Max();

    static ARange rngSpan(__int128 lo, __int128 hi);

    static void rngJoin(ARange& a, const ARange& b);

    static bool rngIsConst(const ARange& r, __int128 v);

  // Overflow proof shared by rawSafeExpr and rangeBinOp: the mathematical
  // result of a+b / a-b / a*b must fit int64 for EVERY value in the operand
  // intervals (interval arithmetic is exact for these bilinear ops, so the
  // corners decide).
    static bool arithFits(const ARange& a, const ARange& b,
                        const std::string& op);

  // Interval of a binary op's int64 result, or unknown. Only ops that always
  // yield int64-or-panic are modeled (mirrors rawSafeBinOp plus the proven
  // arith ops); anything else is unknown.
    ARange rangeBinOp(const std::string& op, const ARange& l,
                    const ARange& r) const;

  // Interval of an expression's int64 value under lookup `lk` (name -> range).
  // Only exprs whose evaluation provably yields int64 (or panics) get a known
  // range; everything else is unknown.
  template <typename Lookup>
    ARange exprRangeIn(const Expr* e, Lookup&& lk) const;

  // Emit-time/proof-time lookup: the union range recorded for a name.
    ARange exprRangeFlat(const Expr* e) const;

  // Overflow proofs for in-place updates (`x++`, `x--`, `x += v`, ...).
  // nameRanges_[nm] is the union of every interval nm holds at any program
  // point, so "union range <op> value range fits int64" proves the update
  // can never produce the double aval that overflow yields — the same
  // contract rawSafeExpr's arithFits gate relies on.
    bool nameRangeKnown(const std::string& nm, ARange& out) const;

    bool incDecProven(const std::string& nm, bool isInc) const;

  // `nm <op>= v` stays int64: structural for ops that cannot overflow
  // (& | ^ % << >> on int64 always yield int64-or-panic, and emitRawBin
  // reproduces the panic paths), range-proven for + - *.
    bool assignOpRawOk(const std::string& nm, const std::string& base,
                     const Expr* v);

  // Write-side gate for the speculative accumulator (mirrors
  // emitSpecAssign's classification): an int-typed write whose int64
  // result MAY overflow is still speculatable — emission keeps the slot
  // raw and deopts to the aval path on `jo`. Covers `v <op>= e` and
  // `v = L <op> R` for op in {+,-,*} with raw-safe int operands; the
  // `=` form only counts when the top-level arith is unproven (proven
  // writes already took the rawWrite branch above).
    bool specWriteOk(const AssignStmt& st);

  // ---- range dataflow -------------------------------------------------------
    void rangeJoinInto(ARangeEnv& dst, const ARangeEnv& src);

    ARangeEnv rangeTruncate(const ARangeEnv& env, int depth) const;

    bool rangeEnvEq(const ARangeEnv& a, const ARangeEnv& b) const;

    void rangeRecord(const ARangeEnv& env);

  // Expr side effects that can change a local's value outside the stmt-level
  // writes: ++/-- targets and names captured by lambdas (a stored lambda may
  // run later). Everything else in an expr is range-neutral.
    void rangePoisonExpr(const Expr* e, ARangeEnv& env);

    void rangePoisonPattern(const Pattern& p, ARangeEnv& env);

  // A `defer` body runs at scope exit, not at its lexical position: any name
  // it assigns must be unknown for everything after the defer statement.
    void rangePoisonDefer(const Stmt* s, ARangeEnv& env);

  // Names assigned anywhere inside a stmt list (recursive) — used to poison
  // non-converged loop-carried variables.
    void rangeCollectAssigns(const Stmt* s, std::unordered_set<std::string>& out);

    void rangeBlock(const std::vector<StmtPtr>& stmts, ARangeEnv& env);

  // Loop fixpoint: `head` is the env at the loop head (includes the for-var
  // scope for counted/iterable fors). Iterates the body until ranges
  // stabilize; continue/break exits are accumulated into cont/brk and joined
  // into the head/post envs so mid-body states are never lost. Non-converged
  // variables are poisoned to unknown before the recording pass.
    void rangeLoop(const std::vector<StmtPtr>& body, const std::string* varName,
                 const ARange* varRange, ARangeEnv& env);

  // Prescan equivalent of rangePoisonExpr over a stmt list (no env writes,
  // only poison side effects on `env`).
    void rangePrescanStmt(const Stmt* s, ARangeEnv& env);

  // `x++`/`x--`/`++x`/`--x` as a whole statement expr: model as x = x +/- 1.
  // rangeBinOp yields unknown whenever the increment can overflow int64
  // (the runtime would then produce a double aval), so the recorded range
  // stays sound and incDecProven only fires on genuinely bounded values.
  // Nested uses (f(x++), y = x++ + 1) still go through rangePoisonExpr.
    bool rangeIncDec(const Expr* e, ARangeEnv& env);

    void rangeStmt(const Stmt* s, ARangeEnv& env);

    void computeNameRanges(const FnDecl* fn);
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
    void promoteMarkExpr(const Expr* e, PromoteScan& ps);

    void promoteMarkPattern(const Pattern& p, PromoteScan& ps);

  // Write-side scan: record which names have only raw-safe int writes and
  // which are disqualified (++/--, lambda captures, compound assigns,
  // destructure/for bindings, non-int or non-raw initializers).
    void promoteWriteStmt(const Stmt* s, PromoteScan& ps);

    void promoteWriteStmts(const std::vector<StmtPtr>& stmts, PromoteScan& ps);

  // Condition positions: tryRawCondJump emits raw int comparisons, `not`,
  // and bare int exprs without boxing, so reads inside them stay raw.
    void promoteCoverCond(const Expr* e, PromoteScan& ps);

  // Read-side scan: mark pending names whose reads are NOT inside a raw
  // region. Mirrors the emission rules: safe-op binaries with raw-safe int
  // operands emit fully raw (emitBinary fast path); calls to raw-inline
  // fns with raw-safe int args materialize args raw; everything else is an
  // aval position where a bare promoted ident would have to box.
    void promoteCoverExpr(const Expr* e, PromoteScan& ps);

    void promoteCoverPattern(const Pattern& p, PromoteScan& ps);

    void promoteCoverStmt(const Stmt* s, PromoteScan& ps);

    void computePromoteSet(const FnDecl* fn);

    bool tryEmitInlineCall(FnInfo* f, const CallExpr& call);

  // ---- calls -------------------------------------------------------------------
    std::vector<const Expr*> positionalArgs(const CallExpr& call);

  // a_bi_call(id, n, argv): argv holds the n user args (no env slot).
    void emitBiCallSlots(int id, const std::vector<int>& slots);

  // len(x) with an array-typed argument: read AObj.len (offset 4) inline.
  // Whitelisted runtime types all keep the element count in h.len
  // (T_LIST/T_TUPLE/T_DICT/T_SET/T_VIEW); T_STR stores bytes and T_RANGE
  // stores no count, so those — and anything unexpected — fall back to
  // the a_len runtime call. Result aval left in %rax.
    bool tryEmitFastLen(const Expr* arg);

  // sqrt(x) fast path for an already-evaluated argument slot: untagged
  // doubles compute via sqrtsd instead of the a_bi_call dispatch. The
  // domain check `x < 0.0` is done with ucomisd so NaN behaves exactly like
  // the runtime (C `x < 0.0` is false for NaN, and sqrt(NaN) canonicalizes
  // via mkdouble). Negative inputs panic with the same "math domain error"
  // message; non-double avals (ints, wrong types) fall back to a_bi_call.
    void tryEmitSqrtSlot(int id, int s);

  // float(i)/f64(i) with a raw-safe int operand: cvtsi2sd straight from the
  // int64 slot instead of boxing the aval and calling a_float.
    bool tryEmitFloatInt(const Expr* arg);

  // Unary conversions that map 1:1 onto runtime helpers: call the helper
  // directly instead of staging an argv array for a_bi_call's want_args +
  // indirect dispatch. All helpers panic/convert exactly like bi_<name>.
    static const char* unaryBuiltinSym(int id);

    void emitBiCall(int id, const CallExpr& call);

  // py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
  // the module/member strings, argv[2] the kwargs count, then positional
  // args, then name/value pairs. The runtime resolves libpython.
    void emitPyCall(const std::string& module, const std::string& member,
                  const CallExpr& call);

  // a_call(fn, n, argv): indirect call through an FNON value.
    void emitACall(const Expr* callee, const CallExpr& call);

  // a_method(obj, name, n, nkw, argv): generic method dispatch. argv holds
  // the n positional args followed by nkw (name, value) pairs; only
  // T_PYOBJ receivers consume the pairs (as Python kwargs) — every other
  // receiver kind panics on nkw > 0.
    void emitMethodCall(const Expr* objExpr, const std::string& field, const CallExpr& call);

  // Place evaluated call args into paramSlots (indexed by paramNames position,
  // -1 = fill A_NONE). `offset` shifts where positional args land (1 for
  // methods whose paramNames[0] is `self`, already filled by the caller).
  // When `variadicSlot` >= 0, positional args landing at or past that index
  // are packed into a list stored in paramSlots[variadicSlot]; an empty list
  // is stored when no extras were passed (unless a named arg supplied it).
    void placeArgs(const CallExpr& call, const std::vector<std::string>& paramNames,
                 std::vector<int>& paramSlots, std::size_t offset = 0,
                 int variadicSlot = -1);

  // Direct call to an A function/ctor: argv[0]=A_NONE env, argv[1..]=params.
    void emitDirectCall(const char* cname, const std::vector<int>& paramSlots);

  // paramSlots sized paramNames.size(); fills from call args. For methods the
  // caller presets paramSlots[0] = self and passes paramNames = full list with
  // offset 1 / rest names.
    void emitCallExpr(const CallExpr& call);

  // ---- expressions ---------------------------------------------------------------
    void emitExpr(const Expr* e);

  // `obj?.field`: None propagates; Some(v) -> Some(v.field) (or v.field when
  // the field itself is optional). Mirrors the old C emitter.
    void emitSafeNav(const SafeNavExpr& x, const Expr* e);

  // Match expression: subject matches one of the arms; unguarded wildcard is default.
    void emitPatternTest(const Pattern& p, int subjS, const std::string& failL);

    void bindPatternVariables(const Pattern& p, int subjS);

  // ---- lambdas / closures ---------------------------------------------------
  // Collect every identifier referenced anywhere inside `e` (over-collection
  // is safe: shadowed names simply produce unused env slots).
    void collectIdents(const Expr* e, std::set<std::string>& out);

    void collectPatternIdents(const Pattern& p, std::set<std::string>& out);

  // Creation site: build the closure env, fill captures, wrap in a FNON.
    void emitLambda(const LambdaExpr& x);

  // Definition site: emit __lambda_N like a normal function whose body is
  // the lambda expression; argv[0] is the closure env, argv[1..] the params.
    void emitLambdaFn(const PendingLambda& pl);

    void emitMatch(const MatchExpr& m);

  // `[f(x) for x in lo..hi[..step] if c]`: counted int64 loop equivalent to
  // the generic iterator tail, but without a_range_new/a_iter_new/a_iter_next.
  // The loop variable is materialized as an aval each pass (inline int for
  // small values) so element/condition expressions see the same type as the
  // iterator path.
    void emitComprehensionRange(const ComprehensionExpr& x, const Expr* loE,
                              const Expr* hiE, const Expr* stE);

  // `[f(x) for x in it if c]` / `{...}`: build via a_iter_new/a_iter_next and
  // a_list_push/a_set_add, mirroring the C emitter's loop. Array/tuple-typed
  // iterables get the same indexed fast loop as `for` (with generic
  // fallback); everything else uses the iterator tail directly.
    void emitComprehension(const ComprehensionExpr& x);

  // Generic comprehension loop over the iterable held in slot `iv`;
  // jumps to mergedL when exhausted.
    void emitComprehensionIterTail(int iv, const ComprehensionExpr& x,
                                 const std::string& mergedL);

  // `expr?`: unwrap ok/err; a propagating value returns early from the
  // enclosing function (defers run when Defer lands).
    void emitTry(const TryExpr& x);

  // Binary exprs that are not plain (l,r) runtime calls.
    void emitBinary(const BinaryExpr& x);

  // Field access expr: module-qualified names, enum variants, getattr.
    void emitField(const Expr* e);
};

}  // namespace a::detail
