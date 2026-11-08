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
#include "type-checker/method_signatures.hpp"
#include "type-checker/types.hpp"

namespace a::detail {

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
bool AsmEmitter::scanExprPure(const Expr* e, std::unordered_set<std::string>* writes) {
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

bool AsmEmitter::scanPatternPure(const Pattern& p,
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

bool AsmEmitter::scanStmtsPure(const std::vector<StmtPtr>& stmts,
                     std::unordered_set<std::string>* writes) {
    for (const StmtPtr& s : stmts)
      if (!scanStmtPure(s.get(), writes)) return false;
    return true;
  }

bool AsmEmitter::scanStmtPure(const Stmt* s,
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

void AsmEmitter::scanInlineExpr(AsmEmitter::InlineScan& sc, const Expr* e) {
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

void AsmEmitter::scanInlinePattern(AsmEmitter::InlineScan& sc, const Pattern& p) {
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

void AsmEmitter::scanInlineStmt(AsmEmitter::InlineScan& sc, const Stmt* s) {
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

// Value whitelist for the fill-loop idiom: scalar, non-allocating,
// non-aliasing expressions whose value cannot change across iterations
// (no calls, no heap reads/writes, no object-building literals, no
// ++/-- side effects). Anything else is re-evaluated per iteration and
// must stay on the generic loop path.
bool AsmEmitter::fillValueOk(const Expr* e) const {
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

const AsmEmitter::InlineInfo& AsmEmitter::inlineInfoFor(FnInfo* f) {
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

bool AsmEmitter::tryEmitInlineCall(FnInfo* f, const CallExpr& call) {
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
std::vector<const Expr*> AsmEmitter::positionalArgs(const CallExpr& call) {
    std::vector<const Expr*> out;
    for (auto& a : call.args) out.push_back(a.value.get());
    return out;
  }

// Rebuild a function-like scope for the callee so caller locals/params can
// never be captured by name lookup. Returns the saved scope stack in
// `saved`; caller restores it after emission.
void AsmEmitter::beginInlineScope(FnInfo* f,
                        std::vector<std::unordered_map<std::string, ABinding>>& saved) {
    saved = std::move(scopes_);
    scopes_.clear();
    if (!saved.empty()) scopes_.push_back(saved.front());
    else scopes_.emplace_back();
    pushScope();
    bindModuleImports(f->moduleId);
  }

void AsmEmitter::endInlineScope(
      std::vector<std::unordered_map<std::string, ABinding>>& saved) {
    scopes_ = std::move(saved);
  }

bool AsmEmitter::callArgsAllInt(const CallExpr& call) const {
    for (const CallArg& a : call.args) {
      TypePtr t = resolveType(a.value ? a.value->inferredType : nullptr);
      if (!t || t->kind != TypeKind::Int) return false;
    }
    return true;
  }

// len(x) with an array-typed argument: read AObj.len (offset 4) inline.
// Whitelisted runtime types all keep the element count in h.len
// (T_LIST/T_TUPLE/T_DICT/T_SET/T_VIEW); T_STR stores bytes and T_RANGE
// stores no count, so those — and anything unexpected — fall back to
// the a_len runtime call. Result aval left in %rax.
bool AsmEmitter::tryEmitFastLen(const Expr* arg) {
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
void AsmEmitter::tryEmitSqrtSlot(int id, int s) {
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

// float(i)/f64(i) with a raw-safe int operand: cvtsi2sd straight from the
// int64 slot instead of boxing the aval and calling a_float.
bool AsmEmitter::tryEmitFloatInt(const Expr* arg) {
    if (!arg || !exprIsInt(arg) || !rawSafeExpr(arg)) return false;
    int s = allocSlot();
    emitRawExpr(arg, s);
    loadSlot(s, "%rax");
    emit("    cvtsi2sd %rax, %xmm0\n");
    int t = allocSlot();
    std::string doneL = newLabel("f2d");
    emitXmm0ToAval(t, doneL);
    emitLabel(doneL);
    return true;
  }

// Unary conversions that map 1:1 onto runtime helpers: call the helper
// directly instead of staging an argv array for a_bi_call's want_args +
// indirect dispatch. All helpers panic/convert exactly like bi_<name>.
const char* AsmEmitter::unaryBuiltinSym(int id) {
    switch (id) {
      case 2: return "a_len";    // len
      case 3: return "a_str";    // str
      case 4: return "a_int";    // int / i64
      case 5: return "a_float";  // float / f64
      case 6: return "a_bool";   // bool
      case 53: return "a_repr";  // repr
      default: return nullptr;
    }
  }

// a_bi_call(id, n, argv): argv holds the n user args (no env slot).
void AsmEmitter::emitBiCallSlots(int id, const std::vector<int>& slots) {
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

void AsmEmitter::emitBiCall(int id, const CallExpr& call) {
    if (call.args.size() == 1 && call.args[0].name.empty()) {
      if (id == 2) {
        const Expr* arg = call.args[0].value.get();
        if (arg && arg->kind == ExprKind::Call) {
          const CallExpr& jc = static_cast<const CallExpr&>(*arg);
          if (jc.builtinId == BuiltinId::Join && jc.args.size() == 2 &&
              jc.args[0].name.empty() && jc.args[1].name.empty()) {
            int vs = evalSlot(jc.args[0].value.get());
            int ss = evalSlot(jc.args[1].value.get());
            loadSlot(vs, "%rdi");
            loadSlot(ss, "%rsi");
            emit("    call a_join_len\n");
            return;
          }
        }
      }
      if (id == 2 && tryEmitFastLen(call.args[0].value.get())) {
        return;
      }
      if (id == 12) {
        // Fused sqrt(f64(i)) for a raw-safe int operand: cvtsi2sd then
        // sqrtsd, all in %xmm0 — no intermediate spill slot (each spill
        // costs a store-forwarding stall). The conversion result is
        // finite by construction, so only the domain check + canon emit
        // of tryEmitFloatInt/tryEmitSqrtSlot apply.
        const Expr* arg = call.args[0].value.get();
        if (arg && arg->kind == ExprKind::Call &&
            static_cast<const CallExpr*>(arg)->builtinId == BuiltinId::F64 &&
            static_cast<const CallExpr*>(arg)->args.size() == 1 &&
            exprIsInt(static_cast<const CallExpr*>(arg)->args[0].value.get()) &&
            rawSafeExpr(static_cast<const CallExpr*>(arg)->args[0].value.get())) {
          const Expr* inner =
              static_cast<const CallExpr*>(arg)->args[0].value.get();
          std::string domL = newLabel("sqd"), doneL = newLabel("squareok"),
                      nanL = newLabel("sqrnan");
          rawInvAll();  // direct slotRef traffic below bypasses the reg cache
          int s = allocSlot();
          emitRawExpr(inner, s);
          loadSlot(s);
          emit("    cvtsi2sd %rax, %xmm0\n");
          emit("    xorpd %xmm1, %xmm1\n    ucomisd %xmm0, %xmm1\n    ja " +
               domL + "\n    sqrtsd %xmm0, %xmm0\n");
          emit("    ucomisd %xmm0, %xmm0\n");
          emit("    jp " + nanL + "\n");
          emit("    movq %xmm0, %rax\n");
          emit("    jmp " + doneL + "\n");
          emitLabel(nanL);
          emit("    mov $0x7ff8000000000000, %rax\n");
          emit("    jmp " + doneL + "\n");
          emitLabel(domL);
          emitPanicStr("math domain error");
          emitLabel(doneL);
          return;
        }
        int s = evalSlot(call.args[0].value.get());
        tryEmitSqrtSlot(id, s);
        return;
      }
      if (id == 5 && tryEmitFloatInt(call.args[0].value.get())) {
        return;
      }
      if (const char* sym = unaryBuiltinSym(id)) {
        int s = evalSlot(call.args[0].value.get());
        loadSlot(s, "%rdi");
        emit(std::string("    call ") + sym + "\n");
        return;
      }
    }
    std::vector<const Expr*> args = positionalArgs(call);
    std::vector<int> slots;
    for (const Expr* a : args) slots.push_back(evalSlot(a));
    emitBiCallSlots(id, slots);
  }

// py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
// the module/member strings, argv[2] the kwargs count, then positional
// args, then name/value pairs. The runtime resolves libpython.
void AsmEmitter::emitPyCall(const std::string& module, const std::string& member,
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
void AsmEmitter::emitACall(const Expr* callee, const CallExpr& call) {
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
void AsmEmitter::emitMethodCall(const Expr* objExpr, const std::string& field, const CallExpr& call) {
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
void AsmEmitter::placeArgs(const CallExpr& call, const std::vector<std::string>& paramNames,
                 std::vector<int>& paramSlots, std::size_t offset,
                 int variadicSlot) {
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
void AsmEmitter::emitDirectCall(const char* cname, const std::vector<int>& paramSlots) {
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
void AsmEmitter::emitCallExpr(const CallExpr& call) {
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
      // Sema already resolved the callee to an exact FnDecl (local or
      // prelude fallback): dispatch on it directly. Scope lookup by bare
      // name is ambiguous across modules — the first module to declare a
      // name wins the global binding.
      if (call.calleeFunction) {
        auto fit = fns_.find(call.calleeFunction);
        if (fit != fns_.end() && !fit->second->isMethod) {
          FnInfo* fi = fit->second;
          if (tryEmitInlineCall(fi, call)) return;
          std::vector<int> ps(fi->paramNames.size(), -1);
          placeArgs(call, fi->paramNames, ps, 0,
                    fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
          emitDirectCall(fi->cname.c_str(), ps);
          return;
        }
      }
      // Sema-resolved RHS to a runtime builtin (e.g. bare linalg names, or a
    // native-module from-import): dispatch on the builtin id directly, not
    // the ambiguous cross-module bare-name table.
    if (call.calleeIsBuiltin) {
      int bid = builtinRuntimeId(call.calleeBuiltin.empty() ? id.name
                                                             : call.calleeBuiltin);
      if (bid >= 0) {
        emitBiCall(bid, call);
        return;
      }
    }
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
          // File modules resolve members through resolveModuleMember below;
          // builtins with the same member name are only a fallback.
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
          // Function/struct in that module, following `from "x.a" import
          // name` re-exports in package __init__ modules.
          auto rm = a::resolveModuleMember(graph_, mb->moduleId, f.field);
          for (FnInfo* fi : fnList_) {
            if (!fi->isMethod && fi->moduleId == rm.first && fi->name == rm.second) {
              if (tryEmitInlineCall(fi, call)) return;
              std::vector<int> ps(fi->paramNames.size(), -1);
              placeArgs(call, fi->paramNames, ps, 0,
                        fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
              emitDirectCall(fi->cname.c_str(), ps);
              return;
            }
          }
          for (StructInfo* st : structList_) {
            if (st->moduleId == rm.first && st->name == rm.second) {
              std::vector<int> ps(st->fieldNames.size(), -1);
              placeArgs(call, st->fieldNames, ps);
              emitDirectCall(st->ctorCname.c_str(), ps);
              return;
            }
          }
          // Module-level let holding a callable: mod.handler(...).
          auto git = topLevelGlobals_.find(std::to_string(rm.first) + ":" + rm.second);
          if (git != topLevelGlobals_.end()) {
            std::vector<const Expr*> args = positionalArgs(call);
            std::vector<int> slots;
            for (const Expr* a : args) slots.push_back(evalSlot(a));
            Argv av = allocArgv((int)slots.size());
            for (std::size_t i = 0; i < slots.size(); ++i)
              argvStore(av, (int)i, slots[i]);
            emit("    mov " + git->second + "(%rip), %rdi\n");
            movImm(slots.size(), "%rsi");
            if (slots.empty()) {
              emit("    xor %edx, %edx");
            } else {
              leaSlot(argvPtr(av), "%rdx");
            }
            emit("    call a_call\n");
            return;
          }
          // Member not defined in the file module: fall back to the builtin
          // with the same name (nativeModules-style access via a file name).
          {
            int bid = builtinRuntimeId(f.field);
            if (bid >= 0) {
              emitBiCall(bid, call);
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
        if (!isRuntimeValue && !isBuiltinValueMethod(f.field)) {
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

}  // namespace a::detail
