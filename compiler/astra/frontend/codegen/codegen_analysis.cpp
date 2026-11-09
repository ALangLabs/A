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
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

namespace a::detail {

bool AsmEmitter::rawRegOn() const { return rawRegDepth_ > 0; }

int AsmEmitter::rawRegCode(const char* reg) const {
    for (int i = 0; i < 9; ++i)
      if (std::strcmp(reg, kRawRegNames[i]) == 0) return i;
    return -1;
  }

// Register r was overwritten: forget every slot->r claim.
void AsmEmitter::rawDropReg(int r) {
    if (!rawRegOn()) return;
    for (auto it = rawRegOf_.begin(); it != rawRegOf_.end();) {
      if (it->second == r) it = rawRegOf_.erase(it);
      else ++it;
    }
  }

void AsmEmitter::rawInvAll() { rawRegOf_.clear(); }

// Slot's memory now holds the bits in register r; drop the slot's stale
// claims in other registers (its old contents are gone) and drop every other
// slot's claim on r (that register now mirrors this slot only).
void AsmEmitter::rawLearn(int slot, int r) {
    if (!rawRegOn()) return;
    // One register mirrors at most one slot: claiming r for `slot` proves the
    // bits of any other slot previously said to live in r are gone.
    rawDropReg(r);
    rawRegOf_.erase(slot);
    rawRegOf_[slot] = r;
  }

// `a <cmp> b` with int operands materialized as raw int64 and compared
// directly: comparisons cannot trap or overflow, so both sides only need
// the rawBinFastOk operand gate (safe-op restriction does not apply).
bool AsmEmitter::rawCmpOk(const BinaryExpr& b) {
    static const std::set<std::string> kCmp = {"==", "!=", "<", "<=", ">", ">="};
    return kCmp.count(b.op) && exprIsInt(b.lhs.get()) &&
           exprIsInt(b.rhs.get()) && rawSafeExpr(b.lhs.get()) &&
           rawSafeExpr(b.rhs.get());
  }

// Structural mirror of tryRawCondJump for the promotion coverage scan.
bool AsmEmitter::rawCondOk(const Expr* e) {
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
bool AsmEmitter::tryRawCondJump(const Expr* cond, const std::string& label,
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
      static const std::unordered_map<std::string, const char*> kInv = {
          {"je", "jne"}, {"jne", "je"}, {"jl", "jge"}, {"jle", "jg"},
          {"jg", "jle"}, {"jge", "jl"}};
      if (it != kJcc.end() && rawCmpOk(b)) {
        // Leaf operands (raw int idents/literals) materialize straight
        // into %rax/%rcx — the `while i < n` head with no temp slots.
        if (rawLeafOk(b.lhs.get()) && rawLeafOk(b.rhs.get())) {
          emitRawLeafReg(b.lhs.get(), "%rax");
          emitRawLeafReg(b.rhs.get(), "%rcx");
          emit("    cmp %rcx, %rax\n");
          const char* cc = it->second;
          if (!jumpIfTrue) cc = kInv.at(cc);
          emit(std::string("    ") + cc + " " + label + "\n");
          return true;
        }
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

// Truthiness test on the aval in %rax -> jcc `label`. Shared tail of
// condJump and the slow lanes of the comparison fast paths.
void AsmEmitter::emitTruthyJump(const std::string& label, bool jumpIfTrue) {
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

// `a <cmp> b` on statically-int operands whose values live in aval slots:
// unbox both inline (emitUnboxInt covers inline ints and boxed T_INT64;
// doubles and everything else take the slow lane) and branch on the raw
// int64 compare directly. The slow lane calls the same a_*_slow helper the
// generic path uses and feeds its bool aval through emitTruthyJump, so
// mixed-type semantics are identical — only the all-int case gets cheaper.
bool AsmEmitter::tryAvalCmpJump(const Expr* cond, const std::string& label,
                      bool jumpIfTrue) {
    if (!cond || failed_) return false;
    if (cond->kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(*cond);
      if (u.op != "not" || !u.operand) return false;
      return tryAvalCmpJump(u.operand.get(), label, !jumpIfTrue);
    }
    if (cond->kind != ExprKind::Binary) return false;
    const BinaryExpr& b = static_cast<const BinaryExpr&>(*cond);
    static const std::unordered_map<std::string, const char*> kJcc = {
        {"==", "je"}, {"!=", "jne"}, {"<", "jl"},
        {"<=", "jle"}, {">", "jg"}, {">=", "jge"}};
    static const std::unordered_map<std::string, const char*> kInv = {
        {"je", "jne"}, {"jne", "je"}, {"jl", "jge"}, {"jle", "jg"},
        {"jg", "jle"}, {"jge", "jl"}};
    static const std::unordered_map<std::string, const char*> kSlow = {
        {"==", "a_eq_slow"}, {"!=", "a_ne_slow"}, {"<", "a_lt_slow"},
        {"<=", "a_le_slow"}, {">", "a_gt_slow"}, {">=", "a_ge_slow"}};
    auto it = kJcc.find(b.op);
    if (it == kJcc.end()) return false;
    if (!exprIsInt(b.lhs.get()) || !exprIsInt(b.rhs.get())) return false;
    const bool lhsLit = b.lhs->kind == ExprKind::IntLit &&
        rawIntLitFits(static_cast<const IntLitExpr&>(*b.lhs).value);
    const bool rhsLit = b.rhs->kind == ExprKind::IntLit &&
        rawIntLitFits(static_cast<const IntLitExpr&>(*b.rhs).value);
    int ls = lhsLit ? -1 : evalSlot(b.lhs.get());
    int rs = rhsLit ? -1 : evalSlot(b.rhs.get());
    std::string slowL = newLabel("acs"), doneL = newLabel("acd");
    movImm(kTagInt, "%rdx");
    if (lhsLit) {
      movImm((uint64_t)static_cast<const IntLitExpr&>(*b.lhs).value, "%r8");
    } else {
      loadSlot(ls, "%rdi");
      emitUnboxInt("%r8", slowL);
    }
    if (rhsLit) {
      movImm((uint64_t)static_cast<const IntLitExpr&>(*b.rhs).value, "%r9");
    } else {
      loadSlot(rs, "%rdi");
      emitUnboxInt("%r9", slowL);
    }
    const char* cc = it->second;
    if (!jumpIfTrue) cc = kInv.at(cc);
    emit("    cmp %r9, %r8\n    " + std::string(cc) + " " + label + "\n"
         "    jmp " + doneL + "\n");
    emitLabel(slowL);
    if (lhsLit) {
      int t = allocSlot();
      movImm(mkintA(static_cast<const IntLitExpr&>(*b.lhs).value));
      storeRax(t);
      ls = t;
    }
    if (rhsLit) {
      int t = allocSlot();
      movImm(mkintA(static_cast<const IntLitExpr&>(*b.rhs).value));
      storeRax(t);
      rs = t;
    }
    call2slot(kSlow.at(b.op), ls, rs);
    emitTruthyJump(label, jumpIfTrue);
    emitLabel(doneL);
    return true;
  }

// Jump helper: evaluate cond expr -> truthy test -> jcc.
// Fast paths cover A_TRUE/A_FALSE/A_NONE, inline ints, and untagged
// doubles inline; everything else (boxed ints, strings, objects) goes
// through a_truthy_raw_slow.
void AsmEmitter::condJump(const Expr* cond, const std::string& label, bool jumpIfTrue) {
    if (tryRawCondJump(cond, label, jumpIfTrue)) return;
    if (tryAvalCmpJump(cond, label, jumpIfTrue)) return;
    emitExpr(cond);
    emitTruthyJump(label, jumpIfTrue);
  }

// Conservative check for whether an otherwise-undeclared assignment target
// could resolve to a global/function/struct/enum binding rather than a fresh
// local. Used only by the raw-int path; false positives just disable it.
bool AsmEmitter::rawNameMayBeGlobal(const std::string& nm) const {
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
      if (kv.second.variantName == nm) return true;
    }
    return false;
  }

bool AsmEmitter::rawInlineShapeOk(const FnDecl* fn) const {
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

// Raw int64 helpers used only while emitting a raw-inline body. Slots in
// rawVars_ hold untagged int64 values, not avals.
bool AsmEmitter::rawIntLitFits(std::int64_t v) const {
    const std::int64_t lim = std::int64_t(1) << 46;
    return v >= -lim && v < lim;
  }

// ---- Phase 5: raw int64 expressions & scalar promotion -------------------
// Ops whose int64 result is always an integer (or a panic): bitwise, shifts
// (panic on out-of-range amount), '~', and '%'. '+', '-', '*', '/' are
// excluded on purpose: int64 overflow (and INT64_MIN / -1) produces a
// *double* aval that a raw int64 slot cannot represent, so those stay on
// the aval path where the slow helpers implement overflow->double.
bool AsmEmitter::rawSafeBinOp(const std::string& op) {
    return op == "&" || op == "|" || op == "^" || op == "<<" ||
           op == ">>" || op == "%";
  }

// Direct-call target eligible for raw (int64) inlining, or nullptr.
// Mirrors the gates in tryEmitInlineCall; used by both the emitter and the
// promotion analysis so they always agree.
FnInfo* AsmEmitter::rawCallTarget(const CallExpr& call) {
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
bool AsmEmitter::rawSafeExpr(const Expr* e) {
    if (!e || failed_) return false;
    switch (e->kind) {
      case ExprKind::IntLit:
        return true;
      case ExprKind::Ident: {
        const std::string& nm = static_cast<const IdentExpr&>(*e).name;
        if (rawVars_.count(nm)) return true;
        ABinding* b = lookup(nm);
        // Promoted raw-int locals hold int64 in their slot by construction.
        if (b && b->slot >= 0 && b->rawInt && !b->specRaw) return true;
        // Aval bindings (incl. specRaw, whose demoted state is an aval)
        // only qualify where range analysis proves the value stays inside
        // int64. An int-*typed* aval can still hold a double after int64
        // overflow promotion (+,-,*,/), and unboxing it panics instead of
        // computing the mixed-type result the runtime slow path produces.
        return exprRangeFlat(e).known;
      }
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

// rawSafeExpr variant for speculative self-assigns (`acc = acc + i`):
// mirrors rawSafeExpr but a bare Ident naming `self` is accepted — the
// specRaw protocol keeps that name's current int64 in its raw slot while
// the demotion flag is 0, and emitSpecAssign handles the demoted state.
// Only the leaf is relaxed: +- * subtrees still need arithFits (their
// raw ops have no jo-deopt point inside emitRawExpr).
bool AsmEmitter::rawSafeExprSelf(const Expr* e, const std::string& self) {
    if (!e || failed_) return false;
    if (e->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr&>(*e).name;
      if (nm == self) return true;
      return rawSafeExpr(e);
    }
    if (e->kind == ExprKind::IntLit) return true;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr& u = static_cast<const UnaryExpr&>(*e);
      if (u.op == "~") return rawSafeExprSelf(u.operand.get(), self);
      if (u.op == "-" && !u.postfix && exprIsInt(e) &&
          rawSafeExprSelf(u.operand.get(), self)) {
        ARange o = exprRangeFlat(u.operand.get());
        return o.known && o.lo > rngI64Min();
      }
      return false;
    }
    if (e->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*e);
      if (rawSafeBinOp(b.op))
        return rawSafeExprSelf(b.lhs.get(), self) &&
               rawSafeExprSelf(b.rhs.get(), self);
      if ((b.op == "+" || b.op == "-" || b.op == "*") &&
          exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
          rawSafeExprSelf(b.lhs.get(), self) &&
          rawSafeExprSelf(b.rhs.get(), self))
        return arithFits(exprRangeFlat(b.lhs.get()),
                         exprRangeFlat(b.rhs.get()), b.op);
      return false;
    }
    if (e->kind == ExprKind::Call) {
      const CallExpr& c = static_cast<const CallExpr&>(*e);
      if (!rawCallTarget(c)) return false;
      for (const CallArg& a : c.args)
        if (!rawSafeExprSelf(a.value.get(), self)) return false;
      return true;
    }
    return false;
  }

// emitBinary fast-path gate: a safe-op binary whose operands are all raw
// int64 materializable. Kept in sync with the emitBinary call site and the
// promotion coverage scan.
bool AsmEmitter::rawBinFastOk(const BinaryExpr& b) {
    return exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
           rawSafeExpr(&b);
  }

// Raw-expression entry point. Each top-level call opens a fresh
// register-cache region; nested calls (rawRegDepth_ > 0) share the
// caller's cache so intermediates forward through the whole tree. The
// cache is dropped on exit so no facts leak into unmodeled code.
bool AsmEmitter::emitRawExpr(const Expr* e, int out) {
    if (rawRegDepth_ == 0) rawRegOf_.clear();
    ++rawRegDepth_;
    bool ok = emitRawExprImpl(e, out);
    --rawRegDepth_;
    if (rawRegDepth_ == 0) rawRegOf_.clear();
    return ok;
  }

// Leaf raw int64 materialization predicate — mirrors emitRawLeafReg's
// acceptance exactly (it must be side-effect free so callers can check
// both operands before emitting either).
bool AsmEmitter::rawLeafOk(const Expr* e) {
    if (!e) return false;
    if (e->kind == ExprKind::IntLit)
      return rawIntLitFits(static_cast<const IntLitExpr&>(*e).value);
    if (e->kind != ExprKind::Ident) return false;
    const std::string& nm = static_cast<const IdentExpr&>(*e).name;
    if (rawVars_.count(nm)) return true;
    ABinding* b = lookup(nm);
    // rawInt locals hold int64 in their slot; specRaw needs the flag
    // check and aval bindings need boxing — neither is a leaf.
    return b && b->slot >= 0 && b->rawInt && !b->specRaw;
  }

// Materialize a rawLeafOk expr directly into `reg` (a GPR name like
// "%rax"): fitting literals via movImm, raw slots via the residency-
// aware slot reads (xmm mirror / %r10 counter / plain load).
bool AsmEmitter::emitRawLeafReg(const Expr* e, const char* reg) {
    if (!e || failed_) return false;
    if (e->kind == ExprKind::IntLit) {
      std::int64_t v = static_cast<const IntLitExpr&>(*e).value;
      if (!rawIntLitFits(v)) return false;
      movImm((uint64_t)v, reg);
      return true;
    }
    const std::string& nm = static_cast<const IdentExpr&>(*e).name;
    auto rv = rawVars_.find(nm);
    if (rv != rawVars_.end()) {
      loadSlot(rv->second, reg);
      return true;
    }
    ABinding* b = lookup(nm);
    if (!b || b->slot < 0 || !b->rawInt || b->specRaw) return false;
    auto xr = loopXmm_.find(b->slot);
    if (xr != loopXmm_.end()) {
      emit(std::string("    movq %xmm") + std::to_string(xr->second) +
           ", " + reg + "\n");
      int c = rawRegCode(reg);
      if (c >= 0) rawDropReg(c);  // reg rewritten outside the helpers
      return true;
    }
    if (b->slot == curGprSlot_) {
      emit(std::string("    mov %r10, ") + reg + "\n");
      int c = rawRegCode(reg);
      if (c >= 0) rawDropReg(c);
      return true;
    }
    loadSlot(b->slot, reg);
    return true;
  }

// `L <op> R -> out` for leaf operands and the ops emitRawBin handles
// with one instruction: skips the per-node temp slots entirely — the
// common `i = i + 1` / `s*31 + i` shapes stay in registers end to end.
bool AsmEmitter::emitRawLeafPair(const BinaryExpr& b, int out) {
    static const std::unordered_map<std::string, const char*> kInsn = {
        {"+", "add"}, {"-", "sub"}, {"*", "imul"},
        {"&", "and"}, {"|", "or"}, {"^", "xor"}};
    auto it = kInsn.find(b.op);
    if (it == kInsn.end()) return false;
    // +,-,* reach here only under rawSafeExpr's no-overflow proof (same
    // invariant emitRawBin relies on); & | ^ cannot overflow at all.
    if (!rawLeafOk(b.lhs.get()) || !rawLeafOk(b.rhs.get())) return false;
    emitRawLeafReg(b.lhs.get(), "%rax");
    emitRawLeafReg(b.rhs.get(), "%rcx");
    emit(std::string("    ") + it->second + " %rcx, %rax\n");
    if (rawRegOn()) { rawDropReg(kRRax); rawDropReg(kRRcx); }
    storeRax(out);
    return true;
  }

// Exactly one leaf operand: emit the composite side first (its result
// slot claim survives the leaf's register pick in the common case), then
// the leaf straight into its operand register, then the slot into the
// other — one temp slot instead of two, one load instead of two.
bool AsmEmitter::emitRawLeafMixed(const BinaryExpr& b, int out) {
    static const std::unordered_map<std::string, const char*> kInsn = {
        {"+", "add"}, {"-", "sub"}, {"*", "imul"},
        {"&", "and"}, {"|", "or"}, {"^", "xor"}};
    auto it = kInsn.find(b.op);
    if (it == kInsn.end()) return false;
    const bool lhsLeaf = rawLeafOk(b.lhs.get());
    const bool rhsLeaf = rawLeafOk(b.rhs.get());
    if (lhsLeaf == rhsLeaf) return false;  // both/neither: other paths
    const Expr* leaf = lhsLeaf ? b.lhs.get() : b.rhs.get();
    const Expr* other = lhsLeaf ? b.rhs.get() : b.lhs.get();
    int os = allocSlot();
    if (!emitRawExpr(other, os)) return false;
    // lhs leaf -> %rax, rhs leaf -> %rcx (matches emitRawBin's layout).
    emitRawLeafReg(leaf, lhsLeaf ? "%rax" : "%rcx");
    loadSlot(os, lhsLeaf ? "%rcx" : "%rax");
    emit(std::string("    ") + it->second + " %rcx, %rax\n");
    if (rawRegOn()) { rawDropReg(kRRax); rawDropReg(kRRcx); }
    storeRax(out);
    return true;
  }

bool AsmEmitter::emitRawExprImpl(const Expr* e, int out) {
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
        // Both arms must agree on the register-cache fact at doneL, or a
        // later loadSlot(out) can be elided on one path while the value is
        // live in a different register (a counter compared in a loop
        // condition once compared the demotion flag and never exited).
        loadSlot(b->slot);
        storeRax(out);
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
      // Leaf-operand fast paths: both-leaf pairs and single-leaf mixes
      // (`i+1`, `s*31`, `x & mask`) compute with fewer temp slots.
      if (emitRawLeafPair(b, out)) return true;
      if (emitRawLeafMixed(b, out)) return true;
      int ls = allocSlot(), rs = allocSlot();
      if (!emitRawExpr(b.lhs.get(), ls) || !emitRawExpr(b.rhs.get(), rs))
        return false;
      if (b.op == "%" && emitRawModConst(b, ls, rs, out)) return true;
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

void AsmEmitter::emitRawBin(const std::string& op, int ls, int rs, int out) {
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

// x % C with C a positive int32 constant and x provably non-negative:
// unsigned magic-multiply division (M = floor(2^64/C)) replaces idiv.
// q = mulhi(x, M) is floor(x/C) or one less; r = x - q*C in [0, 2C) is
// fixed by a single conditional subtract.
bool AsmEmitter::emitRawModConst(const BinaryExpr& b, int ls, int rs, int out) {
    (void)rs;
    if (b.rhs->kind != ExprKind::IntLit) return false;
    const std::int64_t d = static_cast<const IntLitExpr&>(*b.rhs).value;
    if (d < 2 || d > INT32_MAX) return false;
    const ARange lr = exprRangeFlat(b.lhs.get());
    if (!lr.known || lr.lo < 0 || lr.hi > rngI64Max()) return false;
    // floor(2^64/d) without 128-bit math: for non-power-of-two d,
    // UINT64_MAX/d == floor(2^64/d); powers of two need one more.
    const uint64_t magic =
        UINT64_MAX / (uint64_t)d + (((uint64_t)d & ((uint64_t)d - 1)) == 0 ? 1 : 0);
    std::string doneL = newLabel("rmmc");
    loadSlot(ls, "%rax");
    // Save x in %r8 before mul rewrites %rax:%rdx: the remainder then
    // computes x - q*C without reloading the operand slot or dropping
    // every other register-cache claim (rawInvAll + reload used to cost
    // a memory round-trip per '%' in raw loops).
    emit("    mov %rax, %r8\n");
    if (rawRegOn()) rawDropReg(kRR8);
    movImm(magic, "%rdx");
    emit("    mul %rdx\n"
         "    mov %rdx, %rcx\n"
         "    imul $" + std::to_string(d) + ", %rcx\n");
    rawDropReg(kRRax);   // mul rewrote %rax:%rdx outside the reg cache's
    rawDropReg(kRRcx);   // model; %rcx now holds q*C
    emit("    mov %r8, %rax\n    sub %rcx, %rax\n"
         "    cmp $" + std::to_string(d) + ", %rax\n"
         "    jl " + doneL + "\n"
         "    sub $" + std::to_string(d) + ", %rax\n");
    emitLabel(doneL);
    rawDropReg(kRRax);
    storeRax(out);
    return true;
  }

// argsRaw: argSlots already hold raw int64 (skip the aval->int64 unbox).
// keepRaw: leave the raw int64 result in %rax instead of boxing to aval.
// rawVars_ is saved/restored so raw-inline calls can nest inside
// emitRawExpr (e.g. `f(g(x))` or a promoted `x = f(y)`).
void AsmEmitter::emitRawBody(FnInfo* f, const std::vector<int>& argSlots,
                   bool argsRaw, bool keepRaw) {
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

__int128 AsmEmitter::rngI64Min() { return -(((__int128)1) << 63); }

__int128 AsmEmitter::rngI64Max() { return (((__int128)1) << 63) - 1; }

ARange AsmEmitter::rngSpan(__int128 lo, __int128 hi) {
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

void AsmEmitter::rngJoin(ARange& a, const ARange& b) {
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

bool AsmEmitter::rngIsConst(const ARange& r, __int128 v) {
    return r.known && r.lo == v && r.hi == v;
  }

// Overflow proof shared by rawSafeExpr and rangeBinOp: the mathematical
// result of a+b / a-b / a*b must fit int64 for EVERY value in the operand
// intervals (interval arithmetic is exact for these bilinear ops, so the
// corners decide).
bool AsmEmitter::arithFits(const ARange& a, const ARange& b,
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
ARange AsmEmitter::rangeBinOp(const std::string& op, const ARange& l,
                    const ARange& r) const {
    if (op == "+" || op == "-" || op == "*") {
      if (!l.known || !r.known) return ARange{};
      __int128 mn = 0, mx = 0;
      if (op == "+") {
        mn = l.lo + r.lo;
        mx = l.hi + r.hi;
      } else if (op == "-") {
        mn = l.lo - r.hi;
        mx = l.hi - r.lo;
      } else {
        __int128 ps[4] = {l.lo * r.lo, l.lo * r.hi, l.hi * r.lo, l.hi * r.hi};
        mn = ps[0];
        mx = ps[0];
        for (int i = 1; i < 4; ++i) {
          if (ps[i] < mn) mn = ps[i];
          if (ps[i] > mx) mx = ps[i];
        }
      }
      // A result escaping int64 is a *double* aval at runtime, not an
      // int64: no int64 interval describes it, so the range must be
      // unknown. (Saturating to the int64 extremes used to leave a
      // "known" range like [INT64_MAX,INT64_MAX] for `INT64_MAX + 1`,
      // which let arithFits/rawSafeExpr prove `y - 1` or `y % 3` raw-safe
      // and then panic unboxing the double.)
      const __int128 kMin = rngI64Min(), kMax = rngI64Max();
      if (mn < kMin || mx > kMax) return ARange{};
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

template <typename Lookup>
ARange AsmEmitter::exprRangeIn(const Expr* e, Lookup&& lk) const {
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
ARange AsmEmitter::exprRangeFlat(const Expr* e) const {
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
// Upper bound on the accumulator's value at the start of the loop:
// union of the ranges of every `=`-write to it that textually precedes
// the loop in the current function body (see tryEmitForAccum).
bool AsmEmitter::accEntryRange(const std::string& accName, const ForStmt& st,
                               ARange& out) const {
  if (!curFnBody_) return false;
  auto unknownLookup = [](const std::string&) { return ARange{}; };
  bool poisoned = false;
  bool found = false;
  // Walks `stmts` in order collecting the union of ranges of `=`-writes to
  // accName. Stops (without descending) at the target loop itself; leaves
  // `poisoned` set when the entry value cannot be bounded.
  std::function<void(const std::vector<StmtPtr>&)> walkList =
      [&](const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& sp : stmts) {
      if (poisoned) return;
      const Stmt* s = sp.get();
      if (s == static_cast<const Stmt*>(&st)) return;  // target: stop
      if (!s) continue;
      switch (s->kind) {
        case StmtKind::Let: {
          const LetStmt& ls = static_cast<const LetStmt&>(*s);
          if (ls.name == accName) {
            ARange r = ls.init ? exprRangeIn(ls.init.get(), unknownLookup)
                               : ARange{};
            if (!r.known) { poisoned = true; return; }
            if (found) rngJoin(out, r); else out = r;
            found = true;
          }
          break;
        }
        case StmtKind::Assign: {
          const AssignStmt& as = static_cast<const AssignStmt&>(*s);
          const Expr* te = as.target.get();
          if (te && te->kind == ExprKind::Ident &&
              static_cast<const IdentExpr*>(te)->name == accName) {
            if (as.op != "=") { poisoned = true; return; }
            ARange r = as.value ? exprRangeIn(as.value.get(), unknownLookup)
                                : ARange{};
            if (!r.known) { poisoned = true; return; }
            if (found) rngJoin(out, r); else out = r;
            found = true;
          }
          break;
        }
        case StmtKind::Expr: {
          const Expr* ex = static_cast<const ExprStmt&>(*s).expr.get();
          if (ex && ex->kind == ExprKind::Unary) {
            const UnaryExpr* ue = static_cast<const UnaryExpr*>(ex);
            if ((ue->op == "++" || ue->op == "--") && ue->operand &&
                ue->operand->kind == ExprKind::Ident &&
                static_cast<const IdentExpr*>(ue->operand.get())->name ==
                    accName)
              poisoned = true;
          }
          break;
        }
        case StmtKind::Destructure: {
          for (const std::string& n :
               static_cast<const DestructureStmt&>(*s).names)
            if (n == accName) poisoned = true;
          break;
        }
        case StmtKind::If: {
          const IfStmt& is = static_cast<const IfStmt&>(*s);
          for (const IfStmt::Branch& br : is.branches) {
            if (poisoned) return;
            walkList(br.body);
          }
          walkList(is.elseBody);
          break;
        }
        case StmtKind::While:
          walkList(static_cast<const WhileStmt&>(*s).body);
          break;
        case StmtKind::DoWhile:
          walkList(static_cast<const DoWhileStmt&>(*s).body);
          break;
        case StmtKind::For:
          walkList(static_cast<const ForStmt&>(*s).body);
          break;
        case StmtKind::Loop:
          walkList(static_cast<const LoopStmt&>(*s).body);
          break;
        default:
          break;  // Defer bodies run at block exit, after this loop
      }
    }
  };
  walkList(*curFnBody_);
  if (!found || poisoned) return false;
  return out.known;
}


bool AsmEmitter::nameRangeKnown(const std::string& nm, ARange& out) const {
    auto it = nameRanges_.find(nm);
    if (it == nameRanges_.end() || !it->second.known) return false;
    out = it->second;
    return true;
  }

bool AsmEmitter::incDecProven(const std::string& nm, bool isInc) const {
    ARange r;
    if (!nameRangeKnown(nm, r)) return false;
    return arithFits(r, rngSpan(1, 1), isInc ? "+" : "-");
  }

// `nm <op>= v` stays int64: structural for ops that cannot overflow
// (& | ^ % << >> on int64 always yield int64-or-panic, and emitRawBin
// reproduces the panic paths), range-proven for + - *.
bool AsmEmitter::assignOpRawOk(const std::string& nm, const std::string& base,
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
bool AsmEmitter::specWriteOk(const AssignStmt& st) {
    if (!st.value) return false;
    if (st.op.size() > 1 && st.op.back() == '=') {
      const std::string base = st.op.substr(0, st.op.size() - 1);
      return (base == "+" || base == "-" || base == "*") &&
             exprIsInt(st.value.get()) && rawSafeExpr(st.value.get());
    }
    if (st.op == "=" && st.value->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*st.value);
      if (!(b.op == "+" || b.op == "-" || b.op == "*")) return false;
      // `acc = acc <op> e` is the spelled-out accumulator idiom: reads of
      // the target itself are supplied by the specRaw slot/flag protocol.
      const std::string self =
          st.target->kind == ExprKind::Ident
              ? static_cast<const IdentExpr&>(*st.target).name
              : std::string();
      return exprIsInt(b.lhs.get()) && exprIsInt(b.rhs.get()) &&
             rawSafeExprSelf(b.lhs.get(), self) &&
             rawSafeExprSelf(b.rhs.get(), self);
    }
    return false;
  }

// ---- range dataflow -------------------------------------------------------
void AsmEmitter::rangeJoinInto(ARangeEnv& dst, const ARangeEnv& src) {
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

ARangeEnv AsmEmitter::rangeTruncate(const ARangeEnv& env, int depth) const {
    ARangeEnv t;
    t.scopes.assign(env.scopes.begin(),
                    env.scopes.begin() + std::min<int>(depth, env.scopes.size()));
    return t;
  }

bool AsmEmitter::rangeEnvEq(const ARangeEnv& a, const ARangeEnv& b) const {
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

void AsmEmitter::rangeRecord(const ARangeEnv& env) {
    if (!rangeRecording_ || rangeAbort_) return;
    for (const auto& sc : env.scopes)
      for (const auto& kv : sc) {
        auto it = nameRanges_.find(kv.first);
        if (it == nameRanges_.end())
          nameRanges_[kv.first] = kv.second;
        else
          rngJoin(it->second, kv.second);
      }
  }

// Expr side effects that can change a local's value outside the stmt-level
// writes: ++/-- targets and names captured by lambdas (a stored lambda may
// run later). Everything else in an expr is range-neutral.
void AsmEmitter::rangePoisonExpr(const Expr* e, ARangeEnv& env) {
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

void AsmEmitter::rangePoisonPattern(const Pattern& p, ARangeEnv& env) {
    if (p.value) rangePoisonExpr(p.value.get(), env);
    if (p.rangeStart) rangePoisonExpr(p.rangeStart.get(), env);
    if (p.rangeEnd) rangePoisonExpr(p.rangeEnd.get(), env);
    if (p.ctor) rangePoisonExpr(p.ctor.get(), env);
    for (const Pattern& sub : p.subpatterns) rangePoisonPattern(sub, env);
  }

// A `defer` body runs at scope exit, not at its lexical position: any name
// it assigns must be unknown for everything after the defer statement.
void AsmEmitter::rangePoisonDefer(const Stmt* s, ARangeEnv& env) {
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
void AsmEmitter::rangeCollectAssigns(const Stmt* s, std::unordered_set<std::string>& out) {
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

void AsmEmitter::rangeBlock(const std::vector<StmtPtr>& stmts, ARangeEnv& env) {
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
void AsmEmitter::rangeNarrowCond(const Expr* cond, ARangeEnv& env) {
    if (!cond) return;
    if (cond->kind == ExprKind::Binary) {
      const BinaryExpr& b = static_cast<const BinaryExpr&>(*cond);
      if (b.op == "and") {  // both sides hold inside the body
        rangeNarrowCond(b.lhs.get(), env);
        rangeNarrowCond(b.rhs.get(), env);
        return;
      }
      if (b.op == "or") return;  // either side may be false: no facts
      const Expr* le = b.lhs.get();
      const Expr* re = b.rhs.get();
      if (!le || !re) return;
      std::string op = b.op;
      const Expr* idE = nullptr;
      const Expr* valE = nullptr;
      if (le->kind == ExprKind::Ident) {
        idE = le;
        valE = re;
      } else if (re->kind == ExprKind::Ident) {
        // c < i  ==  i > c: flip to keep the ident on the left.
        idE = re;
        valE = le;
        static const std::unordered_map<std::string, std::string> kFlip = {
            {"<", ">"}, {"<=", ">="}, {">", "<"}, {">=", "<="},
            {"==", "=="}, {"!=", "!="}};
        auto f = kFlip.find(op);
        if (f == kFlip.end()) return;
        op = f->second;
      } else {
        return;
      }
      ARange other =
          exprRangeIn(valE, [&env](const std::string& n) { return env.find(n); });
      if (!other.known) return;
      const std::string& nm = static_cast<const IdentExpr&>(*idE).name;
      ARange cur = env.find(nm);
      __int128 lo = cur.known ? cur.lo : rngI64Min();
      __int128 hi = cur.known ? cur.hi : rngI64Max();
      if (op == "<") {
        if (other.hi <= rngI64Min()) return;  // empty: not a useful fact
        hi = hi < other.hi - 1 ? hi : other.hi - 1;
      } else if (op == "<=") {
        hi = hi < other.hi ? hi : other.hi;
      } else if (op == ">") {
        if (other.lo >= rngI64Max()) return;
        lo = lo > other.lo + 1 ? lo : other.lo + 1;
      } else if (op == ">=") {
        lo = lo > other.lo ? lo : other.lo;
      } else if (op == "==") {
        // Intersect: the value equals `other`, so both bounds apply.
        if (other.lo > lo) lo = other.lo;
        if (other.hi < hi) hi = other.hi;
      } else {
        return;
      }
      if (lo > hi) return;  // contradictory: leave the interval alone
      if (!env.assign(nm, rngSpan(lo, hi))) env.bindTop(nm, rngSpan(lo, hi));
      return;
    }
    if (cond->kind == ExprKind::Unary) return;  // `not` carries no facts here
}
void AsmEmitter::rangeLoop(const std::vector<StmtPtr>& body, const std::string* varName,
                 const ARange* varRange, ARangeEnv& env, const Expr* cond) {
    static constexpr int kMaxRangeIter = 8;
    const int postDepth = (int)env.scopes.size();
    // Loop-entry facts, before the condition narrows the body entry.
    ARangeEnv entry = env;
    if (varName) {
      env.scopes.emplace_back();
      env.scopes.back()[*varName] = varRange ? *varRange : ARange{};
    }
    const int headDepth = (int)env.scopes.size();
    ARangeEnv head = env;
    // Every iteration's body starts with the loop condition true: fold in the
    // bounds it implies so counters keep proven ranges (this is what keeps
    // while-loop bodies on the raw int64 path).
    if (cond) rangeNarrowCond(cond, head);
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
    // Fixpoint states are transient (they grow monotonically until they
    // stabilize), so nothing they see may be recorded as a fact. Nested
    // loops re-enable recording for their own recording pass, which starts
    // from the enclosing loop's converged head — that is what gives an
    // inner while-counter a proven range at all.
    const bool savedRecording = rangeRecording_;
    rangeRecording_ = false;
    for (int round = 0; round < 6 && !converged && !rangeAbort_; ++round) {
      ARangeEnv prev;
      for (int it = 0; it < kMaxRangeIter && !rangeAbort_; ++it) {
        prev = head;
        ARangeEnv bodyEnv = head;
        rangeBlock(body, bodyEnv);
        rangeJoinInto(head, bodyEnv);
        if (hasCont) rangeJoinInto(head, cont);
        // Re-impose the condition's facts after every join: they hold at the
        // loop head by construction (an iteration only starts when the
        // condition is true), and without this the counter creeps upward one
        // increment per fixpoint step and the widening pass has to poison it.
        if (cond) rangeNarrowCond(cond, head);
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
      // Directional widening: push only the bound that moved out to the
      // extreme instead of discarding the interval. A counter `i = i + 1`
      // creeps upward one step per fixpoint round, so its upper bound moves
      // and its lower bound (the pre-loop value) does not: widening to
      // [lo, +inf] then intersecting with the loop condition gives the tight
      // [lo, cond] range the raw int64 path needs, where poisoning used to
      // leave the name entirely unknown.
      bool any = false;
      for (std::size_t i = 0; i < head.scopes.size() && i < prev.scopes.size();
           ++i) {
        for (const auto& kv : head.scopes[i]) {
          auto f = prev.scopes[i].find(kv.first);
          if (!kv.second.known) continue;
          if (f == prev.scopes[i].end() || !f->second.known) continue;
          const bool loMoved = kv.second.lo != f->second.lo;
          const bool hiMoved = kv.second.hi != f->second.hi;
          if (!loMoved && !hiMoved) continue;
          __int128 lo = loMoved ? rngI64Min() : kv.second.lo;
          __int128 hi = hiMoved ? rngI64Max() : kv.second.hi;
          if (lo <= hi) head.scopes[i][kv.first] = rngSpan(lo, hi);
          any = true;
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
        if (cond) rangeNarrowCond(cond, head);
        if (varName && varRange)
          head.scopes[postDepth][*varName] = *varRange;
        if (rangeEnvEq(head, prev)) break;
      }
    }
    --rangeFreeze_;
    rangeLoops_.pop_back();
    rangeRecording_ = savedRecording;
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
    // break exits. The for-var scope is dropped. With a narrowed condition
    // the head also carries body-entry facts that do not hold on exit, so
    // start the post state from the loop-entry env instead.
    ARangeEnv post = rangeTruncate(cond ? entry : head, postDepth);
    if (hasBrk) rangeJoinInto(post, brk);
    env.scopes = std::move(post.scopes);
  }

// Prescan equivalent of rangePoisonExpr over a stmt list (no env writes,
// only poison side effects on `env`).
void AsmEmitter::rangePrescanStmt(const Stmt* s, ARangeEnv& env) {
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
bool AsmEmitter::rangeIncDec(const Expr* e, ARangeEnv& env) {
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

void AsmEmitter::rangeStmt(const Stmt* s, ARangeEnv& env) {
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
          // A's implicit declarations (`x = v` with no earlier binding)
          // create the local here; bind it so its range is recorded and the
          // while-condition narrowing has something to intersect with.
          if (!env.assign(nm, r)) env.bindTop(nm, r);
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
        rangeLoop(st.body, nullptr, nullptr, env, st.cond.get());
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

void AsmEmitter::computeNameRanges(const FnDecl* fn) {
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

// Walk every expr child (mirrors collectIdents) marking names that must
// not be promoted: ++/-- targets (they store avals) and anything captured
// by a lambda (captures snapshot avals).
void AsmEmitter::promoteMarkExpr(const Expr* e, AsmEmitter::PromoteScan& ps) {
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

void AsmEmitter::promoteMarkPattern(const Pattern& p, AsmEmitter::PromoteScan& ps) {
    if (p.value) promoteMarkExpr(p.value.get(), ps);
    if (p.rangeStart) promoteMarkExpr(p.rangeStart.get(), ps);
    if (p.rangeEnd) promoteMarkExpr(p.rangeEnd.get(), ps);
    if (p.ctor) promoteMarkExpr(p.ctor.get(), ps);
    for (const Pattern& sub : p.subpatterns) promoteMarkPattern(sub, ps);
  }

// Write-side scan: record which names have only raw-safe int writes and
// which are disqualified (++/--, lambda captures, compound assigns,
// destructure/for bindings, non-int or non-raw initializers).
void AsmEmitter::promoteWriteStmt(const Stmt* s, AsmEmitter::PromoteScan& ps) {
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

void AsmEmitter::promoteWriteStmts(const std::vector<StmtPtr>& stmts, AsmEmitter::PromoteScan& ps) {
    for (const StmtPtr& s : stmts) promoteWriteStmt(s.get(), ps);
  }

// Condition positions: tryRawCondJump emits raw int comparisons, `not`,
// and bare int exprs without boxing, so reads inside them stay raw.
void AsmEmitter::promoteCoverCond(const Expr* e, AsmEmitter::PromoteScan& ps) {
    if (rawCondOk(e)) return;
    promoteCoverExpr(e, ps);
  }

// Read-side scan: mark pending names whose reads are NOT inside a raw
// region. Mirrors the emission rules: safe-op binaries with raw-safe int
// operands emit fully raw (emitBinary fast path); calls to raw-inline
// fns with raw-safe int args materialize args raw; everything else is an
// aval position where a bare promoted ident would have to box.
void AsmEmitter::promoteCoverExpr(const Expr* e, AsmEmitter::PromoteScan& ps) {
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
        // f64(x) with a raw int argument is emitted fully raw
        // (tryEmitFloatInt: cvtsi2sd straight from the raw slot), so the
        // argument read stays inside a raw region — same as the raw-inline
        // function-call case below. All other calls are aval positions.
        if (c.builtinId == BuiltinId::F64 && c.args.size() == 1 &&
            exprIsInt(c.args[0].value.get()) &&
            rawSafeExpr(c.args[0].value.get()))
          return;
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

void AsmEmitter::promoteCoverPattern(const Pattern& p, AsmEmitter::PromoteScan& ps) {
    if (p.value) promoteCoverExpr(p.value.get(), ps);
    if (p.rangeStart) promoteCoverExpr(p.rangeStart.get(), ps);
    if (p.rangeEnd) promoteCoverExpr(p.rangeEnd.get(), ps);
    if (p.ctor) promoteCoverExpr(p.ctor.get(), ps);
    for (const Pattern& sub : p.subpatterns) promoteCoverPattern(sub, ps);
  }

void AsmEmitter::promoteCoverStmt(const Stmt* s, AsmEmitter::PromoteScan& ps) {
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
        bool rhsRaw = false;
        if (st.target->kind == ExprKind::Ident && st.op.size() > 1 &&
            st.op.back() == '=') {
          const std::string& nm =
              static_cast<const IdentExpr&>(*st.target).name;
          const std::string base = st.op.substr(0, st.op.size() - 1);
          const bool arith = base == "+" || base == "-" || base == "*";
          const bool safe = base == "&" || base == "|" || base == "^" ||
                            base == "%" || base == "<<" || base == ">>";
          if ((arith || safe) && st.value && exprIsInt(st.value.get()) &&
              rawSafeExpr(st.value.get()) &&
              (ps.safe.count(nm) || ps.spec.count(nm)))
            rhsRaw = true;
        }
        if (!rhsRaw && st.op == "=" && st.target->kind == ExprKind::Ident &&
            st.value) {
          const std::string& nm =
              static_cast<const IdentExpr&>(*st.target).name;
          if (ps.safe.count(nm) || ps.spec.count(nm)) {
            if (exprIsInt(st.value.get()) && rawSafeExpr(st.value.get())) {
              rhsRaw = true;
            } else if (st.value->kind == ExprKind::Binary) {
              const BinaryExpr& bb =
                  static_cast<const BinaryExpr&>(*st.value);
              // Self-referential `acc = acc <op> e` writes stay raw via
              // the specRaw protocol (mirrors specWriteOk).
              if ((bb.op == "+" || bb.op == "-" || bb.op == "*") &&
                  exprIsInt(bb.lhs.get()) && exprIsInt(bb.rhs.get()) &&
                  rawSafeExprSelf(bb.lhs.get(), nm) &&
                  rawSafeExprSelf(bb.rhs.get(), nm))
                rhsRaw = true;
            }
          }
        }
        if (!rhsRaw) promoteCoverExpr(st.value.get(), ps);
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

void AsmEmitter::computePromoteSet(const FnDecl* fn) {
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

bool AsmEmitter::allIntParamsAndReturn(const FnDecl* fn) const {
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

}  // namespace a::detail

#pragma GCC diagnostic pop
