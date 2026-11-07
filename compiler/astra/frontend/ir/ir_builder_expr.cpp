#include "ir/ir_internal.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "name-resolution/module_loader.hpp"
#include "strings/utf8.hpp"
#include "type-checker/sema.hpp"

namespace a::ir::detail {

// --------------------------------------------------------- expressions

int Builder::lowerExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return emitConst(expr);
      case ExprKind::StringLit:
        return lowerString(static_cast<const StringLitExpr&>(expr));
      case ExprKind::Ident:
        return lowerIdent(static_cast<const IdentExpr&>(expr));
      case ExprKind::Unary:
        return lowerUnary(static_cast<const UnaryExpr&>(expr));
      case ExprKind::Binary:
        return lowerBinary(static_cast<const BinaryExpr&>(expr));
      case ExprKind::Call:
        return lowerCall(static_cast<const CallExpr&>(expr));
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        // LLVM getelementptr: a[i][j] flattens into one multi-index op.
        std::vector<const IndexExpr*> chain;
        const Expr* base = &expr;
        while (base->kind == ExprKind::Index) {
          const auto* node = static_cast<const IndexExpr*>(base);
          chain.push_back(node);
          base = node->object.get();
        }
        if (chain.size() > 1) {
          Inst inst;
          inst.op = Op::GetElementPtr;
          inst.type = defaultType(expr.inferredType);
          inst.span = expr.span;
          inst.operands.push_back(lowerExpr(*base));
          for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            inst.operands.push_back(lowerExpr(*(*it)->index));
          }
          return emit(std::move(inst));
        }
        Inst inst;
        inst.op = Op::Index;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*index.object), lowerExpr(*index.index)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Slice: {
        const auto& slice = static_cast<const SliceExpr&>(expr);
        Inst inst;
        inst.op = Op::Slice;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*slice.object),
                         slice.start ? lowerExpr(*slice.start) : -1,
                         slice.end ? lowerExpr(*slice.end) : -1,
                         slice.step ? lowerExpr(*slice.step) : -1};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        Inst inst;
        inst.op = Op::Field;
        inst.type = defaultType(expr.inferredType);
        inst.text = field.field;
        inst.operands = {lowerExpr(*field.object)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::SafeNav: {
        const auto& nav = static_cast<const SafeNavExpr&>(expr);
        Inst inst;
        inst.op = Op::Field;
        inst.type = defaultType(expr.inferredType);
        inst.text = nav.field + "?";
        inst.operands = {lowerExpr(*nav.object)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::ArrayLit: {
        const auto& array = static_cast<const ArrayLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Array;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : array.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::Tuple: {
        const auto& tuple = static_cast<const TupleExpr&>(expr);
        Inst inst;
        inst.op = Op::Tuple;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : tuple.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::MapLit: {
        const auto& map = static_cast<const MapLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Map;
        inst.type = defaultType(expr.inferredType);
        for (const auto& entry : map.entries) {
          inst.operands.push_back(lowerExpr(*entry.first));
          inst.operands.push_back(lowerExpr(*entry.second));
        }
        return emit(std::move(inst));
      }
      case ExprKind::SetLit: {
        const auto& set = static_cast<const SetLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Set;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : set.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::Range: {
        const auto& range = static_cast<const RangeExpr&>(expr);
        Inst inst;
        inst.op = Op::Range;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*range.start), lowerExpr(*range.end)};
        if (range.step) inst.operands.push_back(lowerExpr(*range.step));
        return emit(std::move(inst));
      }
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        Inst inst;
        inst.op = Op::Select;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*ternary.cond), lowerExpr(*ternary.thenValue),
                         lowerExpr(*ternary.elseValue)};
        return emit(std::move(inst));
      }
      case ExprKind::Lambda: {
        Inst inst;
        inst.op = Op::Lambda;
        inst.type = defaultType(expr.inferredType);
        inst.text = "<lambda>";
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Comprehension: {
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "comprehension";
        inst.type = defaultType(expr.inferredType);
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Match: {
        const auto& match = static_cast<const MatchExpr&>(expr);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "match";
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*match.subject)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Try: {
        const auto& tryExpr = static_cast<const TryExpr&>(expr);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "try";
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*tryExpr.operand)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
    }
    Inst inst;
    inst.op = Op::Opaque;
    inst.text = "unsupported";
    return emit(std::move(inst));
  }

int Builder::lowerString(const StringLitExpr& literal) {
    bool pure = true;
    for (const auto& part : literal.parts) {
      if (part.isExpr && part.expr) {
        pure = false;
        break;
      }
    }
    if (pure) {
      Inst inst;
      inst.op = Op::ConstString;
      inst.type = typeString();
      for (const auto& part : literal.parts) inst.stringValue += part.text;
      inst.span = literal.span;
      return emit(std::move(inst));
    }
    Inst concat;
    concat.op = Op::StringConcat;
    concat.type = typeString();
    concat.span = literal.span;
    for (const auto& part : literal.parts) {
      if (part.isExpr && part.expr) {
        concat.operands.push_back(lowerExpr(*part.expr));
      } else if (!part.text.empty()) {
        Inst text;
        text.op = Op::ConstString;
        text.type = typeString();
        text.stringValue = part.text;
        concat.operands.push_back(emit(std::move(text)));
      }
    }
    return emit(std::move(concat));
  }

int Builder::lowerIdent(const IdentExpr& ident) {
    int value = lookup(ident.name);
    if (value >= 0) return value;
    Inst inst;
    inst.op = Op::Global;
    inst.type = defaultType(ident.inferredType);
    inst.text = ident.name;
    inst.span = ident.span;
    return emit(std::move(inst));
  }

int Builder::lowerUnary(const UnaryExpr& unary) {
    if ((unary.op == "++" || unary.op == "--") && unary.operand->kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(*unary.operand);
      int old = lowerIdent(ident);
      int one = emitConstInt(1, defaultType(unary.operand->inferredType));
      int updated = emitBinary(unary.op == "++" ? "+" : "-", old, one,
                               defaultType(unary.operand->inferredType), unary.opSpan);
      assignVar(ident.name, updated);
      return unary.postfix ? old : updated;
    }
    Inst inst;
    inst.op = Op::Unary;
    inst.text = unary.op;
    inst.type = defaultType(unary.inferredType);
    inst.operands = {lowerExpr(*unary.operand)};
    inst.span = unary.span;
    return emit(std::move(inst));
  }

int Builder::lowerBinary(const BinaryExpr& binary) {
    if (binary.binop == BinOp::Range) {
      Inst inst;
      inst.op = Op::Range;
      inst.type = defaultType(binary.inferredType);
      inst.operands = {lowerExpr(*binary.lhs), lowerExpr(*binary.rhs)};
      return emit(std::move(inst));
    }
    // Short-circuit `and`/`or` are lowered as strict binary operations; the
    // core IR tracks data flow and loops, not operand evaluation order.
    int lhs = lowerExpr(*binary.lhs);
    int rhs = lowerExpr(*binary.rhs);
    return emitBinary(binary.op, lhs, rhs, defaultType(binary.inferredType), binary.opSpan);
  }

int Builder::lowerCall(const CallExpr& call) {
    Inst inst;
    inst.span = call.span;
    inst.type = defaultType(call.inferredType);
    if (call.callee->kind == ExprKind::Ident && call.args.size() == 1) {
      const auto& callee = static_cast<const IdentExpr&>(*call.callee);
      if (lookup(callee.name) < 0 && isConversionName(callee.name)) {
        int arg = lowerExpr(*call.args[0].value);
        std::string kind = castKindFor(typeOf(arg), builtinTypeByName(callee.name));
        if (!kind.empty()) {
          Inst cast;
          cast.op = Op::Cast;
          cast.text = std::move(kind);
          cast.type = inst.type;
          cast.operands = {arg};
          cast.span = call.span;
          return emit(std::move(cast));
        }
      }
    }
    if (call.callee->kind == ExprKind::Ident) {
      const auto& callee = static_cast<const IdentExpr&>(*call.callee);
      if (lookup(callee.name) < 0) {
        inst.op = Op::Call;
        inst.text = callee.name;
      } else {
        inst.op = Op::CallValue;
        inst.operands.push_back(lowerExpr(*call.callee));
      }
    } else if (call.callee->kind == ExprKind::Field) {
      const auto& field = static_cast<const FieldExpr&>(*call.callee);
      inst.op = Op::Call;
      inst.text = "." + field.field;
      inst.operands.push_back(lowerExpr(*field.object));
    } else {
      inst.op = Op::CallValue;
      inst.operands.push_back(lowerExpr(*call.callee));
    }
    for (const auto& arg : call.args) inst.operands.push_back(lowerExpr(*arg.value));
    return emit(std::move(inst));
  }

}  // namespace a::ir::detail
