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

// `obj?.field`: None propagates; Some(v) -> Some(v.field) (or v.field when
// the field itself is optional). Mirrors the old C emitter.
void AsmEmitter::emitSafeNav(const SafeNavExpr& x, const Expr* e) {
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
void AsmEmitter::emitPatternTest(const Pattern& p, int subjS, const std::string& failL) {
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

void AsmEmitter::bindPatternVariables(const Pattern& p, int subjS) {
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
void AsmEmitter::collectIdents(const Expr* e, std::set<std::string>& out) {
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

void AsmEmitter::collectPatternIdents(const Pattern& p, std::set<std::string>& out) {
    if (p.value) collectIdents(p.value.get(), out);
    if (p.rangeStart) collectIdents(p.rangeStart.get(), out);
    if (p.rangeEnd) collectIdents(p.rangeEnd.get(), out);
    if (p.ctor) collectIdents(p.ctor.get(), out);
    for (const Pattern& sub : p.subpatterns) collectPatternIdents(sub, out);
  }

// Creation site: build the closure env, fill captures, wrap in a FNON.
void AsmEmitter::emitLambda(const LambdaExpr& x) {
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
void AsmEmitter::emitLambdaFn(const AsmEmitter::PendingLambda& pl) {
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
    emitFramePush("<lambda>", pl.moduleId, x.span.line);
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
    int lamRet = allocSlot();
    storeRax(lamRet);
    emitFramePop();
    loadSlot(lamRet);
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

void AsmEmitter::emitMatch(const MatchExpr& m) {
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

// `[f(x) for x in lo..hi[..step] if c]`: counted int64 loop equivalent to
// the generic iterator tail, but without a_range_new/a_iter_new/a_iter_next.
// The loop variable is materialized as an aval each pass (inline int for
// small values) so element/condition expressions see the same type as the
// iterator path.
void AsmEmitter::emitComprehensionRange(const ComprehensionExpr& x, const Expr* loE,
                              const Expr* hiE, const Expr* stE) {
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
    {
      std::string okL = newLabel("crok");
      std::string msg = emitStrData("range() step must be positive, found 0.", true);
      loadSlot(stepS, "%rax");
      emit("    test %rax, %rax\n    jnz " + okL + "\n");
      emit("    lea " + msg + "(%rip), %rdi\n    call a_panic_msg\n");
      emitLabel(okL);
    }
    bool strIntElem = false;
    if (x.element && x.element->kind == ExprKind::Call) {
      const CallExpr& ce = static_cast<const CallExpr&>(*x.element);
      if (ce.builtinId == BuiltinId::Str && ce.args.size() == 1 &&
          ce.args[0].name.empty() && ce.args[0].value &&
          ce.args[0].value->kind == ExprKind::Ident &&
          static_cast<const IdentExpr&>(*ce.args[0].value).name == x.var)
        strIntElem = true;
    }
    bool fillStrRange = strIntElem && !x.isSet && !x.condition;
    int accS = allocSlot(), varS = allocSlot();
    if (x.isSet) {
      emit("    call a_set_new\n");
    } else {
      movImm(0, "%rdi");
      movImm(0, "%rsi");
      emit("    call a_list_new\n");
    }
    storeRax(accS);
    if (fillStrRange) {
      call4slot("a_list_fill_str_range", accS, curS, endS, stepS);
      loadSlot(accS);
      return;
    }
    pushScope();
    bindLocal(x.var, varS);
    std::string loopL = newLabel("crange"), contL = newLabel("crangec"),
            endL = newLabel("cranged"), bodyL = newLabel("crangeb"),
            mergedL = newLabel("crangem");
    emitLabel(loopL);
    emit("    mov " + slotRef(curS) + ", %rax\n    mov " + slotRef(endS) +
         ", %rcx\n    mov " + slotRef(stepS) + ", %rdx\n");
    emit("    test %rdx, %rdx\n    jns " + bodyL + "_p\n");
    emit("    cmp %rcx, %rax\n    jg " + bodyL + "\n    jmp " + endL + "\n");
    emitLabel(bodyL + "_p");
    emit("    cmp %rcx, %rax\n    jl " + bodyL + "\n    jmp " + endL + "\n");
    emitLabel(bodyL);
    if (!(strIntElem && !x.condition)) {
      loadSlot(curS);
      emitBoxInt64();
      storeRax(varS);
    }
    if (x.condition) {
      condJump(x.condition.get(), contL, false);
    }
    if (strIntElem) {
      loadSlot(accS, "%rdi");
      loadSlot(curS, "%rsi");
      emit(std::string("    call ") +
           (x.isSet ? "a_set_add_int_str" : "a_list_push_int_str") + "\n");
    } else {
      int es = evalSlot(x.element.get());
      loadSlot(accS, "%rdi");
      loadSlot(es, "%rsi");
      emit(std::string("    call ") + (x.isSet ? "a_set_add" : "a_list_push") + "\n");
    }
    emitLabel(contL);
    emitRangeIncr(curS, endS, stepS);
    jmp(loopL);
    emitLabel(endL);
    popScope();
    loadSlot(accS);
    jmp(mergedL);
    emitLabel(mergedL);
  }

// `[f(x) for x in it if c]` / `{...}`: build via a_iter_new/a_iter_next and
// a_list_push/a_set_add, mirroring the C emitter's loop. Array/tuple-typed
// iterables get the same indexed fast loop as `for` (with generic
// fallback); everything else uses the iterator tail directly.
void AsmEmitter::emitComprehension(const ComprehensionExpr& x) {
    const Expr* loE = nullptr;
    const Expr* hiE = nullptr;
    const Expr* stE = nullptr;
    const Expr* iter = x.iterable.get();
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
      emitComprehensionRange(x, loE, hiE, stE);
      return;
    }
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
void AsmEmitter::emitComprehensionIterTail(int iv, const ComprehensionExpr& x,
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
void AsmEmitter::emitTry(const TryExpr& x) {
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
        emitFramePop();
        loadSlot(tS);
        emit("    leave\n    ret\n");
      }
      emitLabel(doneL);
    }
    loadSlot(tS);
  }

// Binary exprs that are not plain (l,r) runtime calls.
void AsmEmitter::emitBinary(const BinaryExpr& x) {
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
void AsmEmitter::emitField(const Expr* e) {
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
            // Follow `from "x.a" import name` re-exports in package
            // __init__ modules to the module that declares the member.
            auto rm = a::resolveModuleMember(graph_, mb->moduleId, chain[0]);
            auto it = topLevelGlobals_.find(std::to_string(rm.first) + ":" + rm.second);
            if (it != topLevelGlobals_.end()) {
              emit("    mov " + it->second + "(%rip), %rax");
              return;
            }
            for (FnInfo* fi : fnList_) {
              if (!fi->isMethod && fi->moduleId == rm.first && fi->name == rm.second) {
                emit("    mov " + fi->globalVar + "(%rip), %rax");
                return;
              }
            }
            for (StructInfo* st : structList_) {
              if (st->moduleId == rm.first && st->name == rm.second) {
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
          // Native module member as a value: math.PI, math.sqrt.
          if (chain.size() == 1) {
            if (const std::string* cn = nativeConstant(mb->aux, chain[0])) {
              double v = *cn == "PI"   ? 3.14159265358979323846
                        : *cn == "E"    ? 2.71828182845904523536
                                        : 0.0;
              movImm(dblBits(v));
              return;
            }
            if (const std::string* fn = nativeFunction(mb->aux, chain[0])) {
              int bid = builtinRuntimeId(*fn);
              if (bid >= 0) {
                movImm((uint64_t)bid, "%rdi");
                emit("    call a_builtin\n");
                return;
              }
            }
            int bid = builtinRuntimeId(chain[0]);
            if (bid >= 0) {
              movImm((uint64_t)bid, "%rdi");
              emit("    call a_builtin\n");
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

// ---- expressions ---------------------------------------------------------------
void AsmEmitter::emitExpr(const Expr* e) {
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

}  // namespace a::detail
