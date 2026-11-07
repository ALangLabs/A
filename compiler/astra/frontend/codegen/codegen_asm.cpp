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
};

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
  void movImm(uint64_t v, const char* reg = "%rax") {
    emit(std::string("    mov $") + u64s(v) + ", " + reg);
  }
  void storeRax(int slot) { emit("    mov %rax, " + slotRef(slot)); }
  void loadSlot(int slot, const char* reg = "%rax") {
    emit(std::string("    mov ") + slotRef(slot) + ", " + reg);
  }
  void leaSlot(int slot, const char* reg) {
    emit(std::string("    lea ") + slotRef(slot) + ", " + reg);
  }
  // mem -> mem is impossible on x86; route slot copies through %rcx.
  void copySlot(int from, int to) {
    emit("    mov " + slotRef(from) + ", %rcx\n    mov %rcx, " + slotRef(to));
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
  // Raw int64 of an aval slot -> fresh slot via a_int_full.
  int int64Slot(int avalS) {
    int s = allocSlot();
    loadSlot(avalS, "%rdi");
    emit("    call a_int_full\n    mov %rax, " + slotRef(s));
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

  // %rax = int64 -> %rax = aval; equivalent to mkint_full (inline ints cover
  // [-2^46, 2^46), wider values box as T_INT64 via a runtime call).
  // Requires %rdx = kTagInt. Clobbers %rcx; %rdi only on the call path.
  void emitBoxInt64() {
    std::string callL = newLabel("boxc"), skipL = newLabel("boxs");
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

  // Double*double fast path for + - * / (arith_int_double semantics): both
  // operands must be untagged doubles; result canonicalizes NaN via
  // mkdouble. Falls through to slowL for anything else.
  void emitDoubleArith(const std::string& op, int ls, int rs,
                       const std::string& slowL, const std::string& doneL,
                       const std::string& divzL) {
    auto untagged = [&](int s, const std::string& failL) {
      emit("    mov " + slotRef(s) + ", %rax\n    mov %rax, %rdx\n"
           "    shr $48, %rdx\n    cmp $0xFFFF, %edx\n    je " + failL + "\n");
    };
    untagged(ls, slowL);
    untagged(rs, slowL);
    emit("    mov " + slotRef(ls) + ", %rax\n    movq %rax, %xmm0\n");
    emit("    mov " + slotRef(rs) + ", %rax\n    movq %rax, %xmm1\n");
    if (op == "/") {
      // b == 0.0 (incl. -0.0) panics; NaN divisor proceeds to NaN result.
      std::string okL = newLabel("dvok");
      emit("    xorpd %xmm2, %xmm2\n    ucomisd %xmm2, %xmm1\n"
           "    jp " + okL + "\n    jne " + okL + "\n    jmp " + divzL + "\n");
      emitLabel(okL);
      emit("    divsd %xmm1, %xmm0\n");
    } else {
      const char* ins = op == "+" ? "addsd" : op == "-" ? "subsd" : "mulsd";
      emit(std::string("    ") + ins + " %xmm1, %xmm0\n");
    }
    std::string nanL = newLabel("dnan");
    emit("    ucomisd %xmm0, %xmm0\n    jp " + nanL + "\n"
         "    movq %xmm0, %rax\n    jmp " + doneL + "\n");
    emitLabel(nanL);
    emit("    call mkdouble\n    jmp " + doneL + "\n");
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
        emit("    mov %r8, %rax\n    cqo\n    idiv %r9\n");
      } else if (op == "%") {
        emit("    test %r9, %r9\n    jz " + modzL + "\n");
        emit("    cmp $-1, %r9\n    je " + slowL + "\n");
        emit("    mov %r8, %rax\n    cqo\n    idiv %r9\n    mov %rdx, %rax\n");
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
      emit("    jmp " + slowL + "\n");
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

  // Jump helper: evaluate cond expr -> truthy test -> jcc.
  // Fast paths cover A_TRUE/A_FALSE/A_NONE, inline ints, and untagged
  // doubles inline; everything else (boxed ints, strings, objects) goes
  // through a_truthy_raw_slow.
  void condJump(const Expr* cond, const std::string& label, bool jumpIfTrue) {
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
    // ucomisd vs 0.0: unordered (NaN) or not-equal -> truthy; equal
    // (covers -0.0) -> falsy.
    emit("    movq %rax, %xmm0\n    xorpd %xmm1, %xmm1\n"
         "    ucomisd %xmm1, %xmm0\n    jp " + oneL + "\n    jne " + oneL +
         "\n    jmp " + zeroL + "\n");
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
    emitBody(f->decl->body);
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

  // Return/try-propagate path: emit every active frame's defers and drops
  // WITHOUT popping — later code in the same function still needs its frames.
  void emitDeferredPeekAll() {
    for (int fi = (int)deferStack_.size() - 1; fi >= 0; --fi) {
      ScopeFrame& f = deferStack_[fi];
      for (auto it = f.defers.rbegin(); it != f.defers.rend(); ++it)
        emitStmt(*it, false);
      for (auto it = f.drops.rbegin(); it != f.drops.rend(); ++it)
        emitDropRec(*it);
    }
  }

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
        int sl = evalSlot(st.init.get());
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
      int varS = allocSlot();
      std::string loopL = newLabel("for");
      std::string contL = newLabel("forc");
      std::string endL = newLabel("fend");
      std::string bodyL = newLabel("forb");
      emitLabel(loopL);
      // continue while (step>0 ? cur<end : cur>end)
      emit("    mov " + slotRef(curS) + ", %rax\n    mov " + slotRef(endS) + ", %rcx\n    mov " + slotRef(stepS) + ", %rdx\n");
      emit("    test %rdx, %rdx\n    jns " + bodyL + "_p\n");
      emit("    cmp %rcx, %rax\n    jg " + bodyL + "\n    jmp " + endL + "\n");
      emitLabel(bodyL + "_p");
      emit("    cmp %rcx, %rax\n    jl " + bodyL + "\n    jmp " + endL + "\n");
      emitLabel(bodyL);
      // var = a_i64n(cur)
      emit("    mov " + slotRef(curS) + ", %rdi\n    call a_i64n\n");
      storeRax(varS);
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
      emitRangeIncr(curS, endS, stepS);
      jmp(loopL);
      emitLabel(endL);
      return;
    }
    // General iterable: a_iter_new / a_iter_next.
    int itV = evalSlot(st.iterable.get());
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
  }

  void emitAssign(const AssignStmt& st) {
    if (st.target->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*st.target);
      ABinding* b = lookup(id.name);
      if (!b) {
        int sl = allocSlot();
        bindLocal(id.name, sl);
        b = lookup(id.name);
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

  void emitBiCall(int id, const CallExpr& call) {
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
        movImm(mkintA(static_cast<const IntLitExpr*>(e)->value));
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
        if (x.name == "I64_MIN") { movImm(mkintA(INT64_MIN)); return; }
        if (x.name == "I64_MAX") { movImm(mkintA(INT64_MAX)); return; }
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
          emitBindingValue(*b);
          int oldS = allocSlot();
          storeRax(oldS);
          int oneS = allocSlot();
          movImm(mkintA(1), "%rcx");
          emit("    mov %rcx, " + slotRef(oneS));
          call2slot(x.op == "++" ? "a_add_slow" : "a_sub_slow", oldS, oneS);
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
        emit("    mov %rax, %rdi\n");
        if (x.op == "-") {
          emit("    call a_neg_slow");
        } else if (x.op == "~") {
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
        int is = evalSlot(x.index.get());
        call2slot("a_getitem", os, is);
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
  // a_list_push/a_set_add, mirroring the C emitter's loop.
  void emitComprehension(const ComprehensionExpr& x) {
    int iv = evalSlot(x.iterable.get());
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
      emitDeferredPeekAll();
      loadSlot(tS);
      emit("    leave\n    ret\n");
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
