#include "tooling/formatter_internal.hpp"

#include "tooling/ast_text.hpp"

#include <string>
#include <vector>

namespace a::tooling::detail {

// -------------------------------------------------------- expressions

std::string Formatter::exprText(const Expr& expr, int parentPrecedence, int indent) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::CharLit: {
        const std::string literal = source_.slice(expr.span);
        if (!literal.empty()) return literal;
        if (expr.kind == ExprKind::IntLit) {
          return std::to_string(static_cast<const IntLitExpr&>(expr).value);
        }
        if (expr.kind == ExprKind::FloatLit) {
          return std::to_string(static_cast<const FloatLitExpr&>(expr).value);
        }
        return "0";
      }
      case ExprKind::BoolLit:
        return static_cast<const BoolLitExpr&>(expr).value ? "true" : "false";
      case ExprKind::StringLit: {
        const std::string literal = source_.slice(expr.span);
        if (!literal.empty()) return literal;
        return "\"\"";
      }
      case ExprKind::Ident:
        return static_cast<const IdentExpr&>(expr).name;
      case ExprKind::Unary:
        return unaryText(static_cast<const UnaryExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Binary:
        return binaryText(static_cast<const BinaryExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Call:
        return callText(static_cast<const CallExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        return postfixWrap(exprText(*index.object, 17, indent) + "[" +
                               exprText(*index.index, 0, indent) + "]",
                           parentPrecedence, 17);
      }
      case ExprKind::Slice:
        return sliceText(static_cast<const SliceExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        return postfixWrap(exprText(*field.object, 17, indent) + "." + field.field,
                           parentPrecedence, 17);
      }
      case ExprKind::SafeNav: {
        const auto& field = static_cast<const SafeNavExpr&>(expr);
        return postfixWrap(exprText(*field.object, 17, indent) + "?." + field.field,
                           parentPrecedence, 17);
      }
      case ExprKind::ArrayLit:
        return arrayText(static_cast<const ArrayLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::MapLit:
        return mapText(static_cast<const MapLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::SetLit:
        return setText(static_cast<const SetLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Range:
        return rangeText(static_cast<const RangeExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        const std::string text = exprText(*ternary.cond, 2, indent) + " ? " +
                                 exprText(*ternary.thenValue, 0, indent) + " : " +
                                 exprText(*ternary.elseValue, 2, indent);
        return postfixWrap(text, parentPrecedence, 2);
      }
      case ExprKind::Tuple:
        return tupleText(static_cast<const TupleExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Lambda:
        return lambdaText(static_cast<const LambdaExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Comprehension: {
        const auto& comp = static_cast<const ComprehensionExpr&>(expr);
        std::string text = std::string(comp.isSet ? "{" : "[") +
                           exprText(*comp.element, 0, indent) + " for " + comp.var +
                           " in " + exprText(*comp.iterable, 0, indent);
        if (comp.condition) text += " if " + exprText(*comp.condition, 0, indent);
        text += comp.isSet ? "}" : "]";
        return postfixWrap(text, parentPrecedence, 17);
      }
      case ExprKind::Match:
        return matchText(static_cast<const MatchExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Try: {
        const auto& tryExpr = static_cast<const TryExpr&>(expr);
        return postfixWrap(exprText(*tryExpr.operand, 17, indent) + "?", parentPrecedence, 17);
      }
      default:
        break;
    }
    return {};
  }

std::string Formatter::unaryText(const UnaryExpr& unary, int parent, int indent) {
    if (unary.op == "!") {
      return postfixWrap(exprText(*unary.operand, 17, indent) + "!", parent, 17);
    }
    if (unary.postfix) {
      return postfixWrap(exprText(*unary.operand, 17, indent) + unary.op, parent, 17);
    }
    if (unary.op == "not") {
      const std::string text = "not " + exprText(*unary.operand, 6, indent);
      return postfixWrap(text, parent, 6);
    }
    const std::string text = unary.op + exprText(*unary.operand, 15, indent);
    return postfixWrap(text, parent, 15);
  }

std::string Formatter::binaryText(const BinaryExpr& binary, int parent, int indent) {
    const int precedence = binaryPrecedence(binary.op);
    const bool rightAssociative = binary.op == "**";
    const int lhsParent = rightAssociative ? precedence + 1 : precedence;
    const int rhsParent = rightAssociative ? precedence : precedence + 1;
    const std::string text = exprText(*binary.lhs, lhsParent, indent) + " " + binary.op + " " +
                             exprText(*binary.rhs, rhsParent, indent);
    return postfixWrap(text, parent, precedence);
  }

std::string Formatter::callText(const CallExpr& call, int parent, int indent) {
    std::string callee = exprText(*call.callee, 17, indent);
    if (!call.typeArgs.empty()) {
      std::vector<std::string> targs;
      targs.reserve(call.typeArgs.size());
      for (const TypeExprPtr& ta : call.typeArgs)
        targs.push_back(typeText(ta.get()));
      callee += "<";
      for (size_t i = 0; i < targs.size(); ++i) {
        if (i) callee += ", ";
        callee += targs[i];
      }
      callee += ">";
    }
    std::vector<std::string> args;
    args.reserve(call.args.size());
    for (const CallArg& arg : call.args) {
      std::string text;
      if (!arg.name.empty()) text = arg.name + "=";
      text += exprText(*arg.value, 0, indent + 1);
      args.push_back(std::move(text));
    }

    const std::string flat = joinList(callee + "(", args, ")");
    bool multiline = false;
    for (const std::string& arg : args) {
      if (arg.find('\n') != std::string::npos) multiline = true;
    }
    if (static_cast<int>(flat.size() + indent * options_.indentWidth) > options_.maxLineWidth) {
      multiline = true;
    }

    std::string text;
    if (multiline && !args.empty()) {
      text = callee + "(\n";
      for (std::string& arg : args) {
        reindentContinuation(arg, indent + 1);
        text += indentText(indent + 1) + arg + ",\n";
      }
      text += indentText(indent) + ")";
    } else {
      text = flat;
    }
    return postfixWrap(text, parent, 17);
  }

std::string Formatter::enclosedText(const std::string& open, const std::string& close,
                           const std::vector<std::string>& parts, int parent, int indent) {
    const std::string flat = open + joinList("", parts, "") + close;
    bool multiline = false;
    for (const std::string& part : parts) {
      if (part.find('\n') != std::string::npos) multiline = true;
    }
    if (static_cast<int>(flat.size() + indent * options_.indentWidth) > options_.maxLineWidth) {
      multiline = true;
    }

    std::string text;
    if (multiline && !parts.empty()) {
      text = open + "\n";
      for (const std::string& part : parts) {
        text += indentText(indent + 1) + part + ",\n";
      }
      text += indentText(indent) + close;
    } else {
      text = flat;
    }
    return postfixWrap(text, parent, 17);
  }

std::string Formatter::arrayText(const ArrayLitExpr& array, int parent, int indent) {
    std::vector<std::string> elements;
    elements.reserve(array.elements.size());
    for (const auto& element : array.elements) {
      elements.push_back(exprText(*element, 0, indent + 1));
    }
    return enclosedText("[", "]", elements, parent, indent);
  }

std::string Formatter::mapText(const MapLitExpr& map, int parent, int indent) {
    std::vector<std::string> entries;
    entries.reserve(map.entries.size());
    for (const auto& entry : map.entries) {
      entries.push_back(exprText(*entry.first, 0, indent + 1) + ": " +
                        exprText(*entry.second, 0, indent + 1));
    }
    return enclosedText("{", "}", entries, parent, indent);
  }

std::string Formatter::setText(const SetLitExpr& set, int parent, int indent) {
    std::vector<std::string> elements;
    elements.reserve(set.elements.size());
    for (const auto& element : set.elements) {
      elements.push_back(exprText(*element, 0, indent + 1));
    }
    return enclosedText("{", "}", elements, parent, indent);
  }

std::string Formatter::tupleText(const TupleExpr& tuple, int parent, int indent) {
    std::string text = "(";
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
      if (i) text += ", ";
      text += exprText(*tuple.elements[i], 0, indent);
    }
    if (tuple.elements.size() == 1) text += ",";
    text += ")";
    return postfixWrap(text, parent, 17);
  }

std::string Formatter::rangeText(const RangeExpr& range, int parent, int indent) {
    std::string text;
    if (range.start) text += exprText(*range.start, 9, indent);
    text += "..";
    if (range.end) text += exprText(*range.end, 9, indent);
    if (range.step) text += ".." + exprText(*range.step, 9, indent);
    return postfixWrap(text, parent, 8);
  }

std::string Formatter::sliceText(const SliceExpr& slice, int parent, int indent) {
    std::string text = exprText(*slice.object, 17, indent) + "[";
    if (slice.start) text += exprText(*slice.start, 0, indent);
    text += "..";
    if (slice.end) text += exprText(*slice.end, 0, indent);
    if (slice.step) text += ".." + exprText(*slice.step, 0, indent);
    text += "]";
    return postfixWrap(text, parent, 17);
  }

std::string Formatter::lambdaText(const LambdaExpr& lambda, int parent, int indent) {
    std::string text = "fn" + paramsText(lambda.params,
        [this, indent](const Expr& e) { return exprText(e, 0, indent + 1); });
    if (lambda.returnType) text += " -> " + typeText(lambda.returnType.get());
    text += " => " + exprText(*lambda.body, 0, indent);
    return postfixWrap(text, parent, 0);
  }

std::string Formatter::patternText(const Pattern& pattern, int indent) {
    if (pattern.kind == Pattern::Kind::Wildcard) return "_";
    if (pattern.kind == Pattern::Kind::Range) {
      std::string text;
      if (pattern.rangeStart) text += exprText(*pattern.rangeStart, 9, indent);
      text += "..";
      if (pattern.rangeEnd) text += exprText(*pattern.rangeEnd, 9, indent);
      return text;
    }
    return pattern.value ? exprText(*pattern.value, 0, indent) : "_";
  }

std::string Formatter::matchText(const MatchExpr& match, int parent, int indent) {
    std::string text = "match " + exprText(*match.subject, 0, indent) + ":\n";
    int lastLine = match.subject->span.valid() ? match.subject->span.endLine : 0;
    for (const MatchArm& arm : match.arms) {
      const int armLine = arm.pattern.span.valid() ? arm.pattern.span.line : lastLine + 1;
      if (lastLine > 0 && armLine > lastLine + 1) text += '\n';
      text += indentText(indent + 1) + patternText(arm.pattern, indent + 1);
      if (arm.guard) text += " if " + exprText(*arm.guard, 0, indent + 1);
      text += " => " + exprText(*arm.result, 0, indent + 1) + "\n";
      lastLine = arm.result && arm.result->span.valid() ? arm.result->span.endLine : armLine;
    }
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return postfixWrap(text, parent, 2);
  }

}  // namespace a::tooling::detail
