#include "ownership/ownership_internal.hpp"

#include <utility>
#include <vector>

namespace a::detail {

// --------------------------------------------------------- expressions

void Analyzer::useExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return;
      case ExprKind::StringLit: {
        const auto& literal = static_cast<const StringLitExpr&>(expr);
        for (const auto& part : literal.parts) {
          if (part.isExpr && part.expr) useExpr(*part.expr);
        }
        return;
      }
      case ExprKind::Ident:
        useVariable(static_cast<const IdentExpr&>(expr));
        return;
      case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        useExpr(*unary.operand);
        return;
      }
      case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        useExpr(*binary.lhs);
        useExpr(*binary.rhs);
        return;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          const auto& callee = static_cast<const IdentExpr&>(*call.callee);
          if (callee.name == "move") {
            analyzeMove(call);
            return;
          }
          useVariable(callee);
        } else if (call.callee->kind == ExprKind::Field) {
          const auto& field = static_cast<const FieldExpr&>(*call.callee);
          useExpr(*field.object);
          if (field.object->kind == ExprKind::Ident && isStructuralMutator(field.field)) {
            const auto& object = static_cast<const IdentExpr&>(*field.object);
            // Method calls on a view go through the view itself; the runtime
            // rejects structural mutation of a view on its own.
            if (lenderOf_.count(object.name)) {
              auto kind = borrowKind_.find(object.name);
              if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
                sink_.error(field.fieldSpan,
                            "Cannot call '" + field.field + "' through shared borrow '" +
                                object.name + "'.",
                            "Use borrow_mut to obtain a mutable borrow.");
              }
            } else {
              requireNotBorrowed(lenderChain(object.name), "call '" + field.field + "' on",
                                 field.fieldSpan);
            }
          }
        } else {
          useExpr(*call.callee);
        }
        for (const auto& arg : call.args) useExpr(*arg.value);
        return;
      }
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        useExpr(*index.object);
        useExpr(*index.index);
        return;
      }
      case ExprKind::Slice: {
        const auto& slice = static_cast<const SliceExpr&>(expr);
        useExpr(*slice.object);
        if (slice.start) useExpr(*slice.start);
        if (slice.end) useExpr(*slice.end);
        if (slice.step) useExpr(*slice.step);
        return;
      }
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        useExpr(*field.object);
        return;
      }
      case ExprKind::SafeNav: {
        const auto& nav = static_cast<const SafeNavExpr&>(expr);
        useExpr(*nav.object);
        return;
      }
      case ExprKind::ArrayLit: {
        for (const auto& element : static_cast<const ArrayLitExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
      case ExprKind::Range: {
        const auto& range = static_cast<const RangeExpr&>(expr);
        useExpr(*range.start);
        useExpr(*range.end);
        if (range.step) useExpr(*range.step);
        return;
      }
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        useExpr(*ternary.cond);
        useExpr(*ternary.thenValue);
        useExpr(*ternary.elseValue);
        return;
      }
      case ExprKind::Tuple: {
        for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
      case ExprKind::Lambda:
      case ExprKind::Match:
      case ExprKind::Try:
      case ExprKind::Comprehension:
        analyzeComplexExpr(expr);
        return;
      case ExprKind::MapLit: {
        for (const auto& entry : static_cast<const MapLitExpr&>(expr).entries) {
          useExpr(*entry.first);
          useExpr(*entry.second);
        }
        return;
      }
      case ExprKind::SetLit: {
        for (const auto& element : static_cast<const SetLitExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
    }
  }

void Analyzer::analyzeComplexExpr(const Expr& expr) {
    if (const auto* lambda = dynamic_cast<const LambdaExpr*>(&expr)) {
      Snapshot saved = snapshot();
      scopes_.emplace_back();
      for (const auto& param : lambda->params) declare(param.name);
      useExpr(*lambda->body);
      popScope();
      restore(saved);
      return;
    }
    if (const auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
      useExpr(*match->subject);
      for (const auto& arm : match->arms) {
        if (arm.guard) useExpr(*arm.guard);
        useExpr(*arm.result);
      }
      return;
    }
    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr)) {
      useExpr(*tryExpr->operand);
      return;
    }
    if (const auto* comprehension = dynamic_cast<const ComprehensionExpr*>(&expr)) {
      useExpr(*comprehension->iterable);
      Snapshot saved = snapshot();
      scopes_.emplace_back();
      declare(comprehension->var);
      if (comprehension->condition) useExpr(*comprehension->condition);
      useExpr(*comprehension->element);
      popScope();
      restore(saved);
    }
  }

void Analyzer::analyzeMove(const CallExpr& call) {
    if (call.args.size() != 1) return;
    const Expr& value = *call.args[0].value;
    if (value.kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(value);
      OwnershipState* state = findState(ident.name);
      if (state && state->moved) {
        sink_.error(ident.span, "Use of moved value '" + ident.name + "'.",
                    "The value was already moved out of this variable.");
      }
      requireNotBorrowed(lenderChain(ident.name), "move", value.span);
      if (!state) {
        scopes_.back().vars[ident.name] = OwnershipState{};
        state = &scopes_.back().vars[ident.name];
      }
      state->moved = true;
      state->movedAt = value.span;
      return;
    }
    useExpr(value);
  }

}  // namespace a::detail
