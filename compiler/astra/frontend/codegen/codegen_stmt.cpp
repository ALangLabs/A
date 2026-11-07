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

// ---- statements -------------------------------------------------------------
// Emit deferred statements of frames [depth, size) innermost-first, LIFO
// within each frame. Frames are moved out so a `defer` inside a defer body
// registers on the parent frame instead of invalidating iteration.
// If `type` is a struct with a `drop` method, return its FnInfo.
FnInfo* AsmEmitter::dropFnFor(TypePtr type) {
    TypePtr t = resolveType(type);
    if (!t || t->kind != TypeKind::Struct) return nullptr;
    auto sit = structs_.find(shortName(t->structName));
    if (sit == structs_.end()) return nullptr;
    auto mit = sit->second->methods.find("drop");
    return mit == sit->second->methods.end() ? nullptr : mit->second;
  }

void AsmEmitter::emitDropNow(int slot, FnInfo* fn) {
    emitDirectCall(fn->cname.c_str(), {slot});
  }

void AsmEmitter::emitDropRec(const AsmEmitter::DropRec& d) {
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
AsmEmitter::DropRec* AsmEmitter::findDropRec(const ABinding& b) {
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

void AsmEmitter::emitDeferredDownTo(int depth) {
    while ((int)deferStack_.size() > depth) {
      ScopeFrame frame = std::move(deferStack_.back());
      deferStack_.pop_back();
      for (auto it = frame.defers.rbegin(); it != frame.defers.rend(); ++it)
        emitStmt(*it, false);
      for (auto it = frame.drops.rbegin(); it != frame.drops.rend(); ++it)
        emitDropRec(*it);
    }
  }

void AsmEmitter::emitDeferredAll() { emitDeferredDownTo(0); }

// Return/try-propagate path: emit deferred statements of frames [depth,
// size) innermost-first, LIFO within each frame, WITHOUT popping — later
// code in the same function still needs its frames. Inlining uses `depth`
// to run only the callee's frames and leave the caller's defers untouched.
void AsmEmitter::emitDeferredPeekDownTo(int depth) {
    for (int fi = (int)deferStack_.size() - 1; fi >= depth; --fi) {
      ScopeFrame& f = deferStack_[fi];
      for (auto it = f.defers.rbegin(); it != f.defers.rend(); ++it)
        emitStmt(*it, false);
      for (auto it = f.drops.rbegin(); it != f.drops.rend(); ++it)
        emitDropRec(*it);
    }
  }

void AsmEmitter::emitDeferredPeekAll() { emitDeferredPeekDownTo(0); }

void AsmEmitter::emitBody(const std::vector<StmtPtr>& stmts, bool topLevel) {
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

void AsmEmitter::emitStmt(const Stmt* s, bool topLevel) {
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
        emitFramePop();
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
        const bool xmmOk =
            scanStmtsPure(st.body, nullptr) &&
            scanExprPure(st.cond.get(), nullptr);
        int xmmMark = xmmOk ? loopXmmEnter() : -1;
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
        if (xmmOk) loopXmmExit(xmmMark);
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
void AsmEmitter::emitRangeIncr(int curS, int endS, int stepS) {
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

// `for i in lo..hi[..step]` whose entire body is `xs[i] = v` with v a
// loop-invariant scalar lowers to one a_list_fill_range call instead of
// the per-element loop. curS/endS/stepS are the raw int64 range slots
// already computed by emitFor; xs and v are evaluated once (both are
// loop-invariant: xs is a plain ident, v passes fillValueOk and names
// neither i nor xs). The runtime helper reproduces a_setitem's
// normalize_index panics element-by-element.
bool AsmEmitter::tryEmitForFill(const ForStmt& st, int curS, int endS, int stepS) {
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

void AsmEmitter::emitFor(const ForStmt& st) {
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
      if (promoteNames_.count(st.var) &&
          (loopXmm_.empty() || (int)loopXmmAdded_.size() > xmmMark)) {
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
void AsmEmitter::emitForIterTail(int itV, const ForStmt& st, const std::string& mergedL) {
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
void AsmEmitter::emitForArrayLoopTail(const ForStmt& st, int iS, int varS,
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
void AsmEmitter::emitForArray(const ForStmt& st) {
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
      if (varRaw &&
          (loopXmm_.empty() || (int)loopXmmAdded_.size() > xmmMark)) {
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
void AsmEmitter::initSpecSlots(ABinding* b) {
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
void AsmEmitter::emitSpecAssign(ABinding* b, const AssignStmt& st) {
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

void AsmEmitter::emitAssign(const AssignStmt& st) {
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
bool AsmEmitter::exprIsInt(const Expr* e) const {
    TypePtr t = e ? resolveType(e->inferredType) : nullptr;
    return t && t->kind == TypeKind::Int;
  }

}  // namespace a::detail
