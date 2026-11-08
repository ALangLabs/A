#include "type-checker/sema_internal.hpp"

namespace a {
namespace {

bool patternIrrefutable(const Pattern& p) {
  switch (p.kind) {
    case Pattern::Kind::Wildcard:
    case Pattern::Kind::Binding:
      return true;
    case Pattern::Kind::Literal:
    case Pattern::Kind::Range:
      return false;
    case Pattern::Kind::Variant:
    case Pattern::Kind::Tuple:
    case Pattern::Kind::Or:
      for (const Pattern& sub : p.subpatterns) {
        if (!patternIrrefutable(sub)) return false;
      }
      return true;
  }
  return false;
}

}  // namespace

void Sema::checkPattern(Pattern& pattern, TypePtr subjectType, std::vector<PatternBinding>& bindings) {
  switch (pattern.kind) {
    case Pattern::Kind::Wildcard:
      return;
    case Pattern::Kind::Literal: {
      TypePtr valueType = inferExpr(*pattern.value);
      if (!unify(valueType, subjectType)) {
        sink_.error(pattern.span,
                    "This pattern has type " + typeName(valueType) + ", but the match subject has "
                    "type " + typeName(subjectType) + ".");
      }
      return;
    }
    case Pattern::Kind::Range:
      break;
    case Pattern::Kind::Binding: {
      for (const PatternBinding& b : bindings) {
        if (b.name == pattern.name) {
          sink_.error(pattern.span, "Duplicate pattern binding '" + pattern.name + "'.",
                      "Each name bound by one pattern must be unique.");
          return;
        }
      }
      PatternBinding binding;
      binding.name = pattern.name;
      binding.span = pattern.span;
      binding.type = subjectType;
      bindings.push_back(std::move(binding));
      return;
    }
    case Pattern::Kind::Tuple: {
      TypePtr subject = resolveType(subjectType);
      if (subject->kind != TypeKind::Tuple) {
        if (subject->kind != TypeKind::Error) {
          sink_.error(pattern.span, "A tuple pattern cannot match a value of type " +
                                        typeName(subjectType) + ".");
        }
        return;
      }
      if (subject->params.size() != pattern.subpatterns.size()) {
        sink_.error(pattern.span,
                    "This tuple has " + std::to_string(subject->params.size()) +
                        " element(s), but the pattern binds " +
                        std::to_string(pattern.subpatterns.size()) + ".");
        return;
      }
      for (std::size_t i = 0; i < pattern.subpatterns.size(); ++i) {
        checkPattern(pattern.subpatterns[i], subject->params[i], bindings);
      }
      return;
    }
    case Pattern::Kind::Or: {
      std::vector<PatternBinding> common;
      bool first = true;
      for (Pattern& alt : pattern.subpatterns) {
        std::vector<PatternBinding> local;
        checkPattern(alt, subjectType, local);
        if (first) {
          common = std::move(local);
          first = false;
          continue;
        }
        bool mismatch = local.size() != common.size();
        for (const PatternBinding& b : local) {
          bool found = false;
          for (const PatternBinding& c : common) {
            if (c.name == b.name && unify(b.type, c.type)) {
              found = true;
              break;
            }
          }
          if (!found) mismatch = true;
        }
        if (mismatch) {
          sink_.error(alt.span, "All '|' pattern alternatives must bind the same names and types.",
                      "Add bindings to the other alternatives, or use '_'.");
          return;
        }
      }
      for (PatternBinding& b : common) bindings.push_back(std::move(b));
      return;
    }
    case Pattern::Kind::Variant:
      break;
  }

  if (pattern.kind == Pattern::Kind::Variant) {
    TypePtr subject = resolveType(subjectType);
    if (subject->kind != TypeKind::Enum) {
      if (subject->kind != TypeKind::Error) {
        sink_.error(pattern.span,
                    "A variant pattern cannot match a value of type " + typeName(subjectType) +
                        ".",
                    "Variant patterns require a subject of algebraic enum type.");
      }
      return;
    }
    auto infoIt = enums_.find(subject->structName);
    if (infoIt == enums_.end() || !infoIt->second.algebraic) {
      sink_.error(pattern.span,
                  "A variant pattern cannot match a value of type " + typeName(subjectType) + ".",
                  "Only enums with payload variants support destructuring patterns.");
      return;
    }
    EnumInfo& info = infoIt->second;
    auto* ctor = dynamic_cast<FieldExpr*>(pattern.ctor.get());
    if (!ctor) {
      sink_.error(pattern.span, "Variant patterns must be qualified, e.g. Shape.Circle(...).");
      return;
    }
    if (!enumNamespaceUse(ctor->object, info.name)) {
      sink_.error(pattern.span,
                  "Variant pattern '" + ctor->field + "' does not belong to enum '" +
                      shortName(subject->structName) + "'.");
      return;
    }
    const VariantInfo* variant = info.findVariant(ctor->field);
    if (!variant) {
      sink_.error(ctor->fieldSpan, "Enum '" + shortName(info.name) + "' has no variant named '" +
                                       ctor->field + "'.");
      return;
    }
    if (variant->fields.size() != pattern.subpatterns.size()) {
      sink_.error(pattern.span,
                  "Variant '" + shortName(info.name) + "." + variant->name + "' has " +
                      std::to_string(variant->fields.size()) + " payload field(s), but the pattern "
                      "binds " +
                      std::to_string(pattern.subpatterns.size()) + ".",
                  "Bind each payload field, or use '_' for fields you do not need.");
      return;
    }
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < info.typeParamVars.size() && i < subject->params.size(); ++i) {
      mapping[info.typeParamVars[i]] = subject->params[i];
    }
    for (std::size_t i = 0; i < pattern.subpatterns.size(); ++i) {
      TypePtr fieldType =
          mapping.empty() ? variant->fields[i].second
                          : instantiateType(variant->fields[i].second, mapping);
      checkPattern(pattern.subpatterns[i], fieldType, bindings);
    }
    return;
  }

  // Range pattern.
  TypePtr start = inferExpr(*pattern.rangeStart);
  TypePtr end = inferExpr(*pattern.rangeEnd);
  requireInteger(start, pattern.rangeStart->span, "The range pattern start");
  requireInteger(end, pattern.rangeEnd->span, "The range pattern end");
  if (!unify(start, end)) {
    sink_.error(pattern.span, "Range pattern endpoints must have the same type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    return;
  }
  if (!unify(start, subjectType)) {
    sink_.error(pattern.span,
                "This range pattern has type " + typeName(start) + ", but the match subject has "
                "type " + typeName(subjectType) + ".");
    return;
  }
}

void Sema::collectPatternCoverage(Pattern& pattern, const EnumInfo& info,
                                  std::vector<std::string>& covered, bool& catchAll) {
  if (pattern.kind == Pattern::Kind::Wildcard) {
    catchAll = true;
    return;
  }
  if (pattern.kind == Pattern::Kind::Literal) {
    auto* field = dynamic_cast<FieldExpr*>(pattern.value.get());
    if (!field || !enumNamespaceUse(field->object, info.name)) return;
    const VariantInfo* variant = info.findVariant(field->field);
    if (!variant || !variant->fields.empty()) return;
    if (std::find(covered.begin(), covered.end(), variant->name) == covered.end()) {
      covered.push_back(variant->name);
    }
    return;
  }
  if (!patternIrrefutable(pattern)) return;
  if (pattern.kind == Pattern::Kind::Variant) {
    auto* ctor = dynamic_cast<FieldExpr*>(pattern.ctor.get());
    const VariantInfo* variant = ctor ? info.findVariant(ctor->field) : nullptr;
    if (variant && std::find(covered.begin(), covered.end(), variant->name) == covered.end()) {
      covered.push_back(variant->name);
    }
    return;
  }
  if (pattern.kind == Pattern::Kind::Or) {
    for (Pattern& alt : pattern.subpatterns) {
      collectPatternCoverage(alt, info, covered, catchAll);
    }
  }
}

TypePtr Sema::inferMatch(MatchExpr& expr) {
  TypePtr subject = inferExpr(*expr.subject);
  TypePtr result;
  bool sawResult = false;

  const EnumInfo* enumInfo = nullptr;
  TypePtr subjectResolved = resolveType(subject);
  if (subjectResolved->kind == TypeKind::Enum) {
    auto found = enums_.find(subjectResolved->structName);
    if (found != enums_.end() && found->second.algebraic) enumInfo = &found->second;
  }

  for (auto& arm : expr.arms) {
    pushScope();
    std::vector<PatternBinding> bindings;
    checkPattern(arm.pattern, subject, bindings);
    for (const PatternBinding& binding : bindings) {
      declareVar(binding.name, binding.span, binding.type, false, "pattern binding");
    }
    if (arm.guard) {
      requireBool(inferExpr(*arm.guard), arm.guard->span, "A match guard");
    }
    TypePtr armType = inferExpr(*arm.result);
    if (resolveType(armType)->kind == TypeKind::Void) {
      sink_.error(arm.result->span, "A match arm must produce a value.");
      armType = typeError();
    }
    if (!sawResult) {
      result = armType;
      sawResult = true;
    } else if (!unify(result, armType)) {
      sink_.error(arm.result->span,
                  "All match arms must produce the same type, found " + typeName(result) +
                      " and " + typeName(armType) + ".");
    }
    popScope();
  }

  if (enumInfo) {
    std::vector<std::string> covered;
    bool catchAll = false;
    for (auto& arm : expr.arms) {
      if (arm.guard) continue;
      collectPatternCoverage(arm.pattern, *enumInfo, covered, catchAll);
      if (catchAll) break;
    }
    if (!catchAll) {
      std::vector<std::string> missing;
      for (const VariantInfo& variant : enumInfo->variants) {
        if (std::find(covered.begin(), covered.end(), variant.name) == covered.end()) {
          missing.push_back(variant.name);
        }
      }
      if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) {
          if (i) list += i + 1 == missing.size() ? " and " : ", ";
          list += "'" + missing[i] + "'";
        }
        sink_.error(expr.span, std::string("Non-exhaustive match: no arm covers variant") +
                                  (missing.size() == 1 ? "" : "s") + " " + list + ".",
                    "Add arms for the missing variant(s), or a final '_ => ...' arm.");
      }
    }
  } else {
    const MatchArm& last = expr.arms.back();
    if (last.pattern.kind != Pattern::Kind::Wildcard || last.guard) {
      sink_.error(expr.span,
                  "A match expression must end with an unguarded '_' arm.",
                  "Add '_ => ...' so every value is handled.");
    }
  }

  return sawResult ? result : typeError();
}

TypePtr Sema::inferTry(TryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  TypePtr resolved = resolveType(operand);
  if (resolved->kind == TypeKind::Optional) {
    if (currentReturnType_) {
      TypePtr ret = resolveType(currentReturnType_);
      if (ret->kind == TypeKind::Optional) {
        if (!unify(ret->elem, resolved->elem)) {
          sink_.error(expr.span, "The value produced by '? does not match the function's "
                                 "optional return type.");
        }
      } else if (ret->kind != TypeKind::Error) {
        sink_.error(expr.span,
                    "Using '?' on an optional requires the function to return an optional.",
                    "Return '" + typeName(resolved) + "' or change the function's return type "
                    "to " + typeName(resolved) + ".");
      }
    }
    return resolved->elem;
  }
  if (resolved->kind != TypeKind::Result) {
    if (resolved->kind != TypeKind::Error) {
      std::string message =
          "The '?' operator requires a Result or Optional value, found '" +
          typeName(resolved) + "'.";
      if (resolved->kind == TypeKind::Enum) {
        sink_.error(expr.operand->span, message,
                    "Match on the enum value, or return a Result from a helper instead.");
      } else {
        sink_.error(expr.operand->span, message);
      }
    }
    return typeError();
  }
  // The enclosing function must return a compatible Result.
  if (currentReturnType_) {
    TypePtr ret = resolveType(currentReturnType_);
    if (ret->kind == TypeKind::Result) {
      if (!unify(ret->errorType, resolved->errorType)) {
        sink_.error(expr.span, "The error type of '?' does not match the function's return type.");
      }
    } else if (ret->kind == TypeKind::Optional) {
      sink_.error(expr.span,
                  "Using '?' on a Result inside a function returning an optional loses the "
                  "error value.",
                  "Return a Result, or map the error to a value first.");
    }
  }
  return resolved->elem;
}

TypePtr Sema::inferComprehension(ComprehensionExpr& expr) {
  TypePtr elementType =
      iterableElementType(inferExpr(*expr.iterable), expr.iterable->span, "and comprehensions");

  pushScope();
  declareVar(expr.var, expr.varSpan, elementType, false, "loop variable");
  if (expr.condition) {
    requireBool(inferExpr(*expr.condition), expr.condition->span, "The comprehension condition");
  }
  TypePtr result = inferExpr(*expr.element);
  if (resolveType(result)->kind == TypeKind::Void) {
    sink_.error(expr.element->span, "A comprehension element must produce a value.");
    result = typeError();
  }
  popScope();
  return expr.isSet ? typeSet(result) : typeArray(result);
}

// ---------------------------------------------------------------- helpers

void Sema::requireBool(TypePtr type, Span span, const std::string& what) {
      TypePtr resolved = resolveType(type);
      if (resolved->kind == TypeKind::Bool) return;
      if (resolved->kind == TypeKind::Var) { unify(type, typeBool()); return; }
      if (resolved->kind == TypeKind::Error) return;
      sink_.error(span, what + " must be bool, found " + typePhrase(type) + ".");
    }

    void Sema::requireInteger(TypePtr type, Span span, const std::string& what) {
  TypePtr resolved = resolveType(type);
  if (resolved->kind == TypeKind::Int) return;
  if (resolved->kind == TypeKind::Var) {
    if (constrainVar(type, Constraint::Integer)) return;
    return;
  }
  if (resolved->kind == TypeKind::Error) return;
  sink_.error(span, what + " must be an integer, found " + typePhrase(type) + ".");
}

TypePtr Sema::requireNumeric(TypePtr type, Span span, const std::string& what) {
  TypePtr resolved = resolveType(type);
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) return type;
  if (resolved->kind == TypeKind::Var && constrainVar(type, Constraint::Numeric)) return type;
  sink_.error(span, what + " must be numeric, found " + typePhrase(type) + ".");
  return typeError();
}

bool Sema::checkCompoundOp(const std::string& op, TypePtr targetType, Span span) {
      (void)op; (void)span;
      TypePtr type = resolveType(targetType);
      if (type->kind == TypeKind::Var) { constrainVar(targetType, Constraint::Numeric); return true; }
      return true;
    }

    namespace {
    // True when a type still carries an unresolved inference variable anywhere
    // in its structure. Such a mismatch is not conclusive yet: the variable may
    // be bound later (e.g. by another branch or a later assignment), so we stay
    // quiet instead of rejecting code that is merely not fully inferred.
    bool containsUnresolvedVar(const TypePtr& raw) {
      TypePtr t = resolveType(raw);
      if (!t) return true;
      switch (t->kind) {
        case TypeKind::Var: return true;
        case TypeKind::Error: return false;
        case TypeKind::Array:
        case TypeKind::Range:
        case TypeKind::Optional:
        case TypeKind::Set:
        case TypeKind::Bytes:
        case TypeKind::Pointer:
          return containsUnresolvedVar(t->elem);
        case TypeKind::Result:
          return containsUnresolvedVar(t->elem) || containsUnresolvedVar(t->errorType);
        case TypeKind::Map:
          if (containsUnresolvedVar(t->elem)) return true;
          for (const auto& p : t->params) if (containsUnresolvedVar(p)) return true;
          return false;
        case TypeKind::Tuple:
        case TypeKind::Function:
          for (const auto& p : t->params) if (containsUnresolvedVar(p)) return true;
          return t->kind == TypeKind::Function && containsUnresolvedVar(t->ret);
        default:
          return false;
      }
    }
    }  // namespace

    void Sema::reportMismatch(TypePtr actual, TypePtr expected, Span span,
                              const std::string& context) {
      TypePtr actualResolved = resolveType(actual);
      TypePtr expectedResolved = resolveType(expected);
      // Never complain about an already-failed check or a type that still has
      // unresolved inference variables: unannotated expressions legitimately
      // infer as Var and bind later.
      if (actualResolved->kind == TypeKind::Error || expectedResolved->kind == TypeKind::Error)
        return;
      // Only enforce types the programmer wrote down. Element/return types that
      // were merely inferred (e.g. a list element type from its first append)
      // stay dynamic, matching the language's Python-style collection behavior.
      if (!expectedResolved->fromAnnotation) return;
      if (containsUnresolvedVar(actual) || containsUnresolvedVar(expected)) return;
      std::string message = "Expected " + typePhrase(expected) + ", found " + typePhrase(actual);
      if (!context.empty()) message += " " + context;
      message += ".";
      sink_.error(span, message);
    }

    std::string Sema::typePhrase(const TypePtr& type) const {
  TypePtr resolved = resolveType(type);
  if (!resolved) return "a value";
  if (resolved->kind == TypeKind::Var) {
    switch (resolved->constraint) {
      case Constraint::Integer: return "an integer";
      case Constraint::Floating: return "a float";
      case Constraint::Numeric: return "a number";
      case Constraint::None: return "a value of unknown type";
    }
  }
  std::string name = typeName(resolved);
  char first = name.empty() ? 'x' : name[0];
  bool vowelSound = first == 'a' || first == 'e' || first == 'i' || first == 'o';
  return (vowelSound ? "an " : "a ") + name;
}

}  // namespace a
