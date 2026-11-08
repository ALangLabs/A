#include "codegen/codegen_emitter.hpp"

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

namespace a::detail {

void AsmEmitter::fail(const std::string& msg) {
    if (!failed_) {
      failed_ = true;
      if (error_) *error_ = "asm backend: " + msg;
    }
  }

void AsmEmitter::emit(const std::string& s) {
    if (s.empty() || failed_) return;
    // Any call may clobber xmm registers: spill resident slots to their
    // frame slots before the call and reload them right after, keeping
    // residency (and correctness on the taken path) with zero cost on
    // paths that never reach the call.
    if (((!loopXmm_.empty() || endHoistSlot_ >= 0 || curGprSlot_ >= 0)) &&
        (s.find("call ") != std::string::npos ||
         s.find("call\t") != std::string::npos)) {
      // Flush the GPR counter to its slot first (it may hold the freshest
      // value; the slot is what the callee must see if it reads it).
      if (curGprSlot_ >= 0)
        text_ << "    mov %r10, " << slotRef(curGprSlot_) << "\n";
      loopXmmFlush();
      text_ << s;
      if (s.back() != '\n') text_ << '\n';
      loopXmmReload();
      // A call also clobbers the hoisted loop bound (%r11): reload it.
      if (endHoistSlot_ >= 0)
        text_ << "    mov " << slotRef(endHoistSlot_) << ", %r11\n";
      if (curGprSlot_ >= 0)
        text_ << "    mov " << slotRef(curGprSlot_) << ", %r10\n";
      return;
    }
    text_ << s;
    if (s.back() != '\n') text_ << '\n';
  }

std::string AsmEmitter::u64s(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
  }

std::string AsmEmitter::slotRef(int i) { return "-" + std::to_string(8 + 8 * i) + "(%rbp)"; }

int AsmEmitter::allocSlot() { return slotCounter_++; }

int AsmEmitter::frameBytes() const { return ((slotCounter_ + 1) * 8 + 15) & ~15; }

std::string AsmEmitter::newLabel(const std::string& pfx) {
    return ".L__a_" + pfx + std::to_string(labelCounter_++);
  }

void AsmEmitter::emitLabel(const std::string& l) { emit(l + ":"); }

void AsmEmitter::jmp(const std::string& l) { emit("    jmp " + l); }

// ---- value materialization ----------------------------------------------
uint64_t AsmEmitter::mkintA(int64_t v) const {
    uint64_t p = (((uint64_t)v) << 1) | 1ULL;
    return kTag | (p & kPayloadMask);
  }

// Integer literal -> aval, mirroring mkint_full: the inline 47-bit
// encoding only covers [-2^46, 2^46); wider values must box as T_INT64
// (mkintA alone would truncate the payload and miscompile the sign).
void AsmEmitter::emitIntLitA(std::int64_t v) {
    const std::int64_t lim = std::int64_t(1) << 46;
    if (v >= -lim && v < lim) {
      movImm(mkintA(v));
      return;
    }
    movImm((uint64_t)v);
    emitBoxInt64();
  }

void AsmEmitter::movImm(uint64_t v, const char* reg) {
    emit(std::string("    mov $") + u64s(v) + ", " + reg);
    if (rawRegOn()) {
      int c = rawRegCode(reg);
      if (c >= 0) rawDropReg(c);
    }
  }

void AsmEmitter::storeRax(int slot) {
    auto xi = loopXmm_.find(slot);
    if (xi != loopXmm_.end()) {
      emit(std::string("    movq %rax, %xmm") + std::to_string(xi->second));
      rawLearn(slot, kRRax);  // %rax still holds the stored bits
      return;
    }
    emit("    mov %rax, " + slotRef(slot));
    rawLearn(slot, kRRax);  // %rax still holds the stored bits
  }

void AsmEmitter::loadSlot(int slot, const char* reg) {
    auto xi = loopXmm_.find(slot);
    if (xi != loopXmm_.end()) {
      emit(std::string("    movq %xmm") + std::to_string(xi->second) +
           ", " + reg);
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

// Slot bits -> xmm register for SSE math: resident slots move reg->reg
// (movq), others load the aval word straight from the frame (movsd).
// Direct `movsd slotRef(...)` sites must route through here or they read
// a stale frame slot while the register is authoritative.
void AsmEmitter::loadSlotXmm(int slot, const char* xmm) {
    auto xi = loopXmm_.find(slot);
    if (xi != loopXmm_.end()) {
      emit(std::string("    movq %xmm") + std::to_string(xi->second) + ", " +
           xmm + "\n");
      return;
    }
    emit("    movsd " + slotRef(slot) + ", " + xmm + "\n");
  }

void AsmEmitter::leaSlot(int slot, const char* reg) {
    emit(std::string("    lea ") + slotRef(slot) + ", " + reg);
  }

// mem -> mem is impossible on x86; route slot copies through %rcx.
void AsmEmitter::copySlot(int from, int to) {
    if (from == to) return;  // identity: keeps register claims valid too
    // Counter mirrored in %r10 (see curGprSlot_): the frame slot may be
    // stale between calls, so read straight from the register.
    if (from == curGprSlot_ && from != to) {
      auto xt2 = loopXmm_.find(to);
      if (xt2 != loopXmm_.end()) {
        emit("    mov %r10, %rcx\n    movq %rcx, %xmm" +
             std::to_string(xt2->second));
      } else {
        emit("    mov %r10, %rcx\n    mov %rcx, " + slotRef(to));
      }
      rawLearn(to, kRRcx);
      return;
    }
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

uint64_t AsmEmitter::dblBits(double d) const {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
  }

// Byte string in .rodata (.byte list: safest for the astra assembler).
// Returns the label; bytes are NOT escaped so any content works.
std::string AsmEmitter::emitStrData(const std::string& bytes, bool nulTerm) {
    std::string l = ".L__a_d" + std::to_string(dataCounter_++);
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

void AsmEmitter::emitFramePush(const std::string& name, int moduleId, int line) {
    std::string path;
    if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
      path = graph_.at(moduleId).path;
    std::string nl = emitStrData(name, true);
    std::string fl = emitStrData(path, true);
    emit("    lea " + nl + "(%rip), %rdi\n");
    emit("    lea " + fl + "(%rip), %rsi\n");
    emit("    mov $" + std::to_string(line) + ", %edx\n");
    emit("    call a_frame_push\n");
}

void AsmEmitter::emitFramePop() {
    emit("    call a_frame_pop\n");
}

// aval string value -> fresh slot holding a_str_new(ptr, len).
int AsmEmitter::strLitSlot(const std::string& text) {
    std::string l = emitStrData(text, false);
    int s = allocSlot();
    emit("    lea " + l + "(%rip), %rdi\n");
    movImm(text.size(), "%rsi");
    emit("    call a_str_new\n    mov %rax, " + slotRef(s));
    return s;
  }

AsmEmitter::Argv AsmEmitter::allocArgv(int count) {
    Argv a;
    a.base = slotCounter_;
    a.n = count;
    for (int i = 0; i < count; ++i) allocSlot();
    return a;
  }

int AsmEmitter::argvSlot(const AsmEmitter::Argv& a, int k) const { return a.base + a.n - 1 - k; }

int AsmEmitter::argvPtr(const AsmEmitter::Argv& a) const { return argvSlot(a, 0); }

void AsmEmitter::argvStore(const AsmEmitter::Argv& a, int k, int valSlot) {
    copySlot(valSlot, argvSlot(a, k));
  }

void AsmEmitter::argvStoreImm(const AsmEmitter::Argv& a, int k, uint64_t v) {
    movImm(v, "%rcx");
    emit("    mov %rcx, " + slotRef(argvSlot(a, k)));
  }

// ---- slots / bindings ------------------------------------------------------
void AsmEmitter::pushScope() { scopes_.emplace_back(); }

void AsmEmitter::popScope() { scopes_.pop_back(); }

ABinding* AsmEmitter::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }

void AsmEmitter::bindLocal(const std::string& name, int slot) {
    ABinding b;
    b.kind = BKind::Local;
    b.slot = slot;
    scopes_.back()[name] = b;
  }

// Evaluate expr -> fresh slot holding the aval (null expr -> A_NONE slot).
int AsmEmitter::evalSlot(const Expr* e) {
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
int AsmEmitter::int64Slot(int avalS) {
    int s = allocSlot();
    loadSlot(avalS, "%rdi");
    emitAvalToI64();
    storeRax(s);
    return s;
  }

void AsmEmitter::storeBinding(const ABinding& b, int vs) {
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

void AsmEmitter::emitBindingValue(const ABinding& b) {
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

const char* AsmEmitter::binopSlow(const std::string& op) {
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
void AsmEmitter::call2slot(const char* sym, int a, int b) {
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }

void AsmEmitter::call3slot(const char* sym, int a, int b, int c) {
    loadSlot(c, "%rdx");
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }

void AsmEmitter::call4slot(const char* sym, int a, int b, int c, int d) {
    loadSlot(d, "%rcx");
    loadSlot(c, "%rdx");
    loadSlot(b, "%rsi");
    loadSlot(a, "%rdi");
    emit(std::string("    call ") + sym);
  }

// f(aval) -> rax from a slot.
void AsmEmitter::call1slot(const char* sym, int a) {
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
void AsmEmitter::emitUnboxInt(const char* dst, const std::string& slowL) {
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
void AsmEmitter::emitAvalToI64() {
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
void AsmEmitter::emitBoxInt64() {
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

void AsmEmitter::emitPanicStr(const std::string& msg) {
    std::string l = emitStrData(msg, true);
    emit("    lea " + l + "(%rip), %rdi\n    call a_panic_msg\n");
  }

// Emits the a_is_double test for both operand slots: each aval must be an
// untagged double (top16 != 0xFFFF). Jumps to slowL otherwise, so mixed
// int/double and non-numeric operands keep the exact runtime semantics.
void AsmEmitter::emitDoublePairCheck(int ls, int rs, const std::string& slowL) {
    for (int s : {ls, rs}) {
      loadSlot(s, "%rcx");
      emit("    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n    je " + slowL +
           "\n");
    }
  }

// Stores the low 64 bits of %xmm0 into %rax, canonicalizing a NaN result
// to kCanonNaN (ucomisd with itself sets PF iff the value is unordered,
// i.e. NaN — 2 instructions to detect, matching a_double_canon exactly).
void AsmEmitter::emitXmm0ToAval(int scratch, const std::string& doneL) {
    std::string nanL = newLabel("dnan");
    emit("    ucomisd %xmm0, %xmm0\n    jp " + nanL + "\n"
         "    movsd %xmm0, " + slotRef(scratch) + "\n"
         "    mov " + slotRef(scratch) + ", %rax\n"
         "    jmp " + doneL + "\n");
    emitLabel(nanL);
    movImm(kCanonNaN);
    emit("    jmp " + doneL + "\n");
  }

// Structural mirror of emitFloatXmm's grammar — see the header.
bool AsmEmitter::floatXmmOk(const Expr* e) {
    if (!e || failed_) return false;
    switch (e->kind) {
      case ExprKind::FloatLit:
        return true;
      case ExprKind::IntLit:
        return exprIsFloat(e);
      case ExprKind::Ident: {
        if (!exprIsFloat(e)) return false;
        const std::string& nm = static_cast<const IdentExpr&>(*e).name;
        ABinding* b = lookup(nm);
        if (!b) return false;
        return b->kind == BKind::Local || b->kind == BKind::Param ||
               b->kind == BKind::Global || b->kind == BKind::FnValue ||
               b->kind == BKind::StructCtor;
      }
      case ExprKind::Call: {
        const CallExpr& c = static_cast<const CallExpr&>(*e);
        if (c.builtinId != BuiltinId::F64 || c.args.size() != 1 ||
            !c.args[0].name.empty())
          return false;
        const Expr* arg = c.args[0].value.get();
        // emitRawExpr may call (a_env_get/a_int_full/...) for exotic
        // bindings; emitFloatXmm spills xmm0/xmm1 around it, so any
        // raw-safe int operand is safe here.
        return arg && exprIsInt(arg) && rawSafeExpr(arg);
      }
      case ExprKind::Binary: {
        const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
        if (b.op != "+" && b.op != "-" && b.op != "*") return false;
        return floatXmmOk(b.lhs.get()) && floatXmmOk(b.rhs.get());
      }
      default:
        return false;
    }
  }

// Call-free float expr -> `dst` xmm register holding raw IEEE-754 bits.
// Restricted grammar (see header): only xmm0/xmm1 are used as scratch
// and no instruction sequence here can emit a call, so values already
// sitting in those registers (or in loop-resident xmm2+) survive.
bool AsmEmitter::emitFloatXmm(const Expr* e, const char* dst) {
    if (!floatXmmOk(e)) return false;
    const std::string d = dst;
    if (e->kind == ExprKind::FloatLit) {
      movImm(dblBits(static_cast<const FloatLitExpr&>(*e).value));
      emit("    movq %rax, " + d + "\n");
      return true;
    }
    if (e->kind == ExprKind::IntLit) {
      // Float-typed int literal: convert like f64(i) would.
      movImm((uint64_t)static_cast<const IntLitExpr&>(*e).value);
      emit("    cvtsi2sd %rax, " + d + "\n");
      return true;
    }
    if (e->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr&>(*e).name;
      ABinding* b = lookup(nm);
      if (!b) return false;
      if (b->kind == BKind::Local || b->kind == BKind::Param) {
        if (b->slot < 0) return false;
        loadSlot(b->slot);  // rax = aval bits (xmm-forwarded if resident)
      } else if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
                 b->kind == BKind::StructCtor) {
        emit("    mov " + b->sym + "(%rip), %rax\n");
      } else {
        return false;  // Capture/Builtin/Module reads can call
      }
      emit("    movq %rax, " + d + "\n");
      return true;
    }
    if (e->kind == ExprKind::Call) {
      const CallExpr& c = static_cast<const CallExpr&>(*e);
      const Expr* arg = c.args[0].value.get();
      // emitRawExpr only calls for non-rawInt idents (unbox/boxing/
      // builtin reads); when every referenced name is a raw slot the
      // conversion is call-free and no spill is needed. Otherwise spill
      // both scratch registers around it (calls clobber xmm0/xmm1) and
      // restore the non-dst one afterwards.
      const char* other = d == "%xmm0" ? "%xmm1" : "%xmm0";
      bool mayCall = false;
      std::set<std::string> names;
      collectIdents(arg, names);
      for (const std::string& nm : names) {
        if (rawVars_.count(nm)) continue;
        ABinding* ib = lookup(nm);
        if (!ib || !ib->rawInt) { mayCall = true; break; }
      }
      int t0 = -1, t1 = -1;
      if (mayCall) {
        t0 = allocSlot(); t1 = allocSlot();
        emit("    movsd %xmm0, " + slotRef(t0) + "\n    movsd %xmm1, " +
             slotRef(t1) + "\n");
      }
      int s = allocSlot();
      emitRawExpr(arg, s);
      loadSlot(s, "%rax");
      emit("    cvtsi2sd %rax, " + d + "\n");
      if (mayCall)
        emit("    movsd " +
             slotRef(other == std::string("%xmm1") ? t1 : t0) + ", " +
             other + "\n");
      return true;
    }
    if (e->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
      if (b.op != "+" && b.op != "-" && b.op != "*") return false;
      const char* other = d == "%xmm0" ? "%xmm1" : "%xmm0";
      // Evaluate the left operand first, then spill it: evaluating the right
      // operand may be compound and recursively clobber the partner scratch
      // register, which previously corrupted the left operand.
      int lhsTmp = allocSlot();
      if (!emitFloatXmm(b.lhs.get(), dst)) return false;
      emit("    movsd " + d + ", " + slotRef(lhsTmp) + "\n");
      if (!emitFloatXmm(b.rhs.get(), other)) return false;
      emit("    movsd " + slotRef(lhsTmp) + ", " + d + "\n");
      emit(std::string("    ") +
           (b.op == "+" ? "addsd" : b.op == "-" ? "subsd" : "mulsd") + " " +
           std::string(other) + ", " + d + "\n");
      return true;
    }
    return false;
  }

// Double*double arithmetic with inline SSE2: both operands must be
// untagged doubles (checked via emitDoublePairCheck), then addsd/subsd/
// mulsd/divsd compute the IEEE result. Division by zero (either sign of
// zero) jumps to the shared divzL panic block — identical to the
// a_div/a_div_slow check `b == 0.0`. % / ** / mixed types stay on the
// slow path (fmod/pow semantics and mixed-type promotion live there).
void AsmEmitter::emitDoubleArith(const std::string& op, int ls, int rs,
                       const std::string& slowL, const std::string& doneL,
                       const std::string& divzL, bool skipPairCheck) {
    static const std::unordered_map<std::string, const char*> kSse = {
        {"+", "addsd"}, {"-", "subsd"}, {"*", "mulsd"}, {"/", "divsd"}};
    auto it = kSse.find(op);
    if (it == kSse.end()) {
      jmp(slowL);
      return;
    }
    rawInvAll();  // direct slotRef/slot traffic below bypasses the reg cache
    if (!skipPairCheck) emitDoublePairCheck(ls, rs, slowL);
    if (op == "/") {
      // x + x == 0 (mod 2^64) iff x is +0.0 or -0.0; C `b == 0.0` is true
      // for both zero signs, matching the runtime panic condition.
      loadSlot(rs, "%rcx");
      emit("    add %rcx, %rcx\n    jz " + divzL + "\n");
    }
    int t = allocSlot();
    loadSlotXmm(ls, "%xmm0");
    loadSlotXmm(rs, "%xmm1");
    emit(std::string("    ") + std::string(it->second) + " %xmm1, %xmm0\n");
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
void AsmEmitter::emitDoubleCmp(const std::string& op, int ls, int rs,
                     const std::string& slowL, const std::string& doneL) {
    static const std::set<std::string> kDCmp = {
        "==", "!=", "<", "<=", ">", ">="};
    if (!kDCmp.count(op)) {
      jmp(slowL);
      return;
    }
    rawInvAll();
    emitDoublePairCheck(ls, rs, slowL);
    loadSlotXmm(ls, "%xmm0");
    loadSlotXmm(rs, "%xmm1");
    emit("    xor %eax, %eax\n");
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
void AsmEmitter::emitArith(const std::string& op, int ls, int rs) {
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
    // FP fast lane: when both operands are doubles, go straight to the
    // SSE path instead of paying both emitUnboxInt dances first. Mixed
    // int/double (and everything else) still falls through to the int
    // path and then the runtime slow path — identical semantics either
    // way, just without the two rejected unbox sequences on every op.
    if (isArith) {
      std::string dblL = newLabel("dgate"), pastL = newLabel("pastdgate");
      for (int s : {ls, rs}) {
        loadSlot(s, "%rcx");
        emit("    shr $48, %rcx\n    cmp $0xFFFF, %ecx\n    je " +
             pastL + "\n");
      }
      emit("    jmp " + dblL + "\n");
      emitLabel(dblL);
      emitDoubleArith(op, ls, rs, slowL, doneL, divzL,
                      /*skipPairCheck=*/true);
      emitLabel(pastL);
    }
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

void AsmEmitter::loopXmmFlush() {
    if (loopXmm_.empty()) return;
    for (const auto& kv : loopXmm_) {
      emit(std::string("    movq %xmm") + std::to_string(kv.second) + ", " +
           slotRef(kv.first));
    }
  }

// Reload resident values after a call: slots were flushed before it, so
// movq m64->xmm restores each register without needing a scratch GPR
// (which could hold the call's result).
void AsmEmitter::loopXmmReload() {
    if (loopXmm_.empty()) return;
    for (const auto& kv : loopXmm_) {
      emit("    movq " + slotRef(kv.first) + ", %xmm" +
           std::to_string(kv.second));
    }
  }

// Install in-scope bindings into xmm registers for the duration of a
// loop. Raw-int/spec bindings always qualify; includeAval additionally
// admits plain locals/params (call-free loops only — see the header).
// Returns a marker for loopXmmExit. Bindings not touched by the body
// simply waste a register; correctness never depends on use.
int AsmEmitter::loopXmmEnter(bool includeAval) {
    const int marker = (int)loopXmmAdded_.size();
    // Nested loops keep the outer residency but do not install new xmm
    // registers: mixing two residency sets corrupts loop-carried values.
    if (!loopXmm_.empty()) return marker;
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
        if (!includeAval && !b.rawInt && !b.specRaw) continue;
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
void AsmEmitter::loopXmmExit(int marker) {
    loopXmmFlush();
    while ((int)loopXmmAdded_.size() > marker) {
      loopXmm_.erase(loopXmmAdded_.back());
      loopXmmAdded_.pop_back();
    }
  }

}  // namespace a::detail
