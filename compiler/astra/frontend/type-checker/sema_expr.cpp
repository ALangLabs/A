#include "type-checker/sema_internal.hpp"

namespace a {
// ------------------------------------------------------------- expressions

TypePtr Sema::inferExpr(Expr& expr) {
  TypePtr type = inferExprImpl(expr);
  expr.inferredType = resolveType(type);
  exprTypes_[&expr] = type;
  return type;
}

TypePtr Sema::inferExprImpl(Expr& expr) {
  if (auto* node = dynamic_cast<IntLitExpr*>(&expr)) {
    // Record for the end-of-module sized-type range check; exprTypes_ is
    // filled by inferExpr, so its presence means this node was seen before.
    if (!exprTypes_.count(&expr)) numericLiterals_.push_back(node);
    return freshVar(Constraint::Integer, expr.span, "an integer literal", /*allowDefault=*/true);
  }
  if (auto* node = dynamic_cast<FloatLitExpr*>(&expr)) {
    if (!exprTypes_.count(&expr)) numericLiterals_.push_back(node);
    return freshVar(Constraint::Floating, expr.span, "a float literal", /*allowDefault=*/true);
  }
  if (dynamic_cast<BoolLitExpr*>(&expr)) return typeBool();
  if (dynamic_cast<CharLitExpr*>(&expr)) return typeChar();
  if (auto* node = dynamic_cast<StringLitExpr*>(&expr)) return inferStringLiteral(*node);
  if (auto* node = dynamic_cast<IdentExpr*>(&expr)) return inferIdent(*node);
  if (auto* node = dynamic_cast<UnaryExpr*>(&expr)) return inferUnary(*node);
  if (auto* node = dynamic_cast<BinaryExpr*>(&expr)) return inferBinary(*node);
  if (auto* node = dynamic_cast<CallExpr*>(&expr)) return inferCall(*node);
  if (auto* node = dynamic_cast<IndexExpr*>(&expr)) return inferIndex(*node);
  if (auto* node = dynamic_cast<SliceExpr*>(&expr)) return inferSlice(*node);
  if (auto* node = dynamic_cast<FieldExpr*>(&expr)) return inferField(*node);
  if (auto* node = dynamic_cast<SafeNavExpr*>(&expr)) return inferSafeNav(*node);
  if (auto* node = dynamic_cast<ArrayLitExpr*>(&expr)) return inferArrayLiteral(*node);
  if (auto* node = dynamic_cast<MapLitExpr*>(&expr)) return inferMapLiteral(*node);
  if (auto* node = dynamic_cast<SetLitExpr*>(&expr)) return inferSetLiteral(*node);
  if (auto* node = dynamic_cast<TupleExpr*>(&expr)) return inferTuple(*node);
  if (auto* node = dynamic_cast<LambdaExpr*>(&expr)) return inferLambda(*node);
  if (auto* node = dynamic_cast<MatchExpr*>(&expr)) return inferMatch(*node);
  if (auto* node = dynamic_cast<TryExpr*>(&expr)) return inferTry(*node);
  if (auto* node = dynamic_cast<ComprehensionExpr*>(&expr)) return inferComprehension(*node);
  if (auto* node = dynamic_cast<RangeExpr*>(&expr)) {
    TypePtr start = inferExpr(*node->start);
    TypePtr end = inferExpr(*node->end);
    requireInteger(start, node->start->span, "The range start");
    requireInteger(end, node->end->span, "The range end");
    if (!unify(start, end)) {
      sink_.error(node->span, "Range endpoints must have the same integer type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    }
    if (node->step) {
      TypePtr step = inferExpr(*node->step);
      requireInteger(step, node->step->span, "The range step");
      if (!unify(step, start)) {
        reportMismatch(step, start, node->step->span, "in the range step");
      }
    }
    return typeRange(start);
  }
  if (auto* node = dynamic_cast<TernaryExpr*>(&expr)) {
    requireBool(inferExpr(*node->cond), node->cond->span, "The condition of '\?:'");
    TypePtr thenType = inferExpr(*node->thenValue);
    TypePtr elseType = inferExpr(*node->elseValue);
    if (!unify(thenType, elseType)) {
      sink_.error(node->span,
                  "Both branches of '\?:' must have the same type, found " + typeName(thenType) +
                      " and " + typeName(elseType) + ".");
      return typeError();
    }
    if (resolveType(thenType)->kind == TypeKind::Void) {
      sink_.error(node->span, "The branches of '\?:' cannot produce void.");
      return typeError();
    }
    return thenType;
  }
  return typeError();
}

TypePtr Sema::inferStringLiteral(StringLitExpr& expr) {
  for (auto& part : expr.parts) {
    if (!part.isExpr || !part.expr) continue;
    TypePtr type = inferExpr(*part.expr);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(part.expr->span, "Cannot interpolate a void value into a string.");
    }
  }
  return typeString();
}

TypePtr Sema::inferIdent(IdentExpr& expr) {
  if (VarSym* sym = lookupVar(expr.name)) {
    sym->used = true;
    return sym->type;
  }
  if (expr.name == "None") {
    TypePtr element = freshVar(Constraint::None, expr.span, "the value type of 'None'");
    element->allowUnresolved = true;
    return typeOptional(element);
  }
  if (auto imported = fromBindings_.find(expr.name); imported != fromBindings_.end()) {
    FromBinding& binding = imported->second;
    switch (binding.kind) {
      case FromBinding::Kind::Function:
        if (binding.native) {
          sink_.error(expr.span,
                      "Native function '" + expr.name + "' can only be called directly.",
                      "Wrap it in a lambda to pass it as a value, e.g. x => " + expr.name + "(x).");
          return typeError();
        }
        if (binding.type) return binding.type;
        return typeError();
      case FromBinding::Kind::Struct:
        sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                    "Construct it: " + expr.name + "(...).");
        return typeError();
      case FromBinding::Kind::Trait:
        sink_.error(expr.span, "'" + expr.name + "' is a trait, not a value.",
                    "Use it as a constraint (where " + expr.name +
                        ") or in 'impl " + expr.name + " for Type:'.");
        return typeError();
      case FromBinding::Kind::Value:
        return binding.type ? binding.type : typeError();
    }
  }
  if (auto alias = moduleAliases_.find(expr.name); alias != moduleAliases_.end()) {
    if (alias->second.python) {
      // Python modules are fully dynamic: members are resolved at runtime.
      auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
    }
    return alias->second.native ? typeNativeModule(alias->second.nativeName)
                                : typeModule(alias->second.moduleId);
  }
  {
    const std::string enumQ = qualified(expr.name);
    if (enums_.count(enumQ)) return typeEnum(enumQ);
  }
  if (TypePtr constant = builtinConstantType(expr.name)) return constant;
  const std::string q = qualified(expr.name);
  if (auto fn = functionsByName_.find(q); fn != functionsByName_.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig != signatures_.end()) {
      return typeFunction(sig->second.paramTypes, sig->second.retType);
    }
    return typeError();
  }
  if (structsByName_.count(q)) {
    sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                "Construct it: " + expr.name + "(...).");
    return typeError();
  }
  std::string undefMsg = "Undefined name '" + expr.name + "'.";
  if (std::string sug = suggestName(expr.name); !sug.empty())
    undefMsg += " Did you mean '" + sug + "'?";
  sink_.errorWithCode(expr.span, "E0001", undefMsg,
              "Declare it by assigning a value, e.g. " + expr.name + " = 0.");
  return typeError();
}

TypePtr Sema::inferUnary(UnaryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == false) {
    // Prefix ++/-- : operand must be a mutable numeric variable.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        std::string m = "Undefined variable '" + ident->name + "'.";
        if (std::string sug = suggestName(ident->name); !sug.empty())
          m += " Did you mean '" + sug + "'?";
        sink_.error(expr.opSpan, m);
        return typeError();
      }
      sym->used = true;
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == true) {
    // Postfix i++ / i-- : same rules as prefix.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        std::string m = "Undefined variable '" + ident->name + "'.";
        if (std::string sug = suggestName(ident->name); !sug.empty())
          m += " Did you mean '" + sug + "'?";
        sink_.error(expr.opSpan, m);
        return typeError();
      }
      sym->used = true;
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if (expr.op == "!") {
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Error) return typeError();
    if (resolved->kind == TypeKind::Optional) return resolved->elem;
    sink_.error(expr.opSpan,
                "'!' can only unwrap an optional value, found " + typePhrase(operand) + ".",
                "Only values of type 'T?' can be unwrapped.");
    return typeError();
  }
  if (expr.op == "not") {
    if (!unify(operand, typeBool())) {
      sink_.error(expr.opSpan,
                  "'not' requires a bool operand, found " + typePhrase(operand) + ".");
      return typeError();
    }
    return typeBool();
  }

  // Record the effective value of literals behind unary '-' (and '--'-style
  // double negation) so the range check sees `-1`, not `1`. Nested minuses
  // are folded here; each inner unary already recorded its own override.
  if (expr.op == "-" && !expr.postfix) {
    const Expr* cur = expr.operand.get();
    bool neg = true;
    while (true) {
      const auto* un = dynamic_cast<const UnaryExpr*>(cur);
      if (!un || un->op != "-" || un->postfix || !un->operand) break;
      neg = !neg;
      cur = un->operand.get();
    }
    if (const auto* lit = dynamic_cast<const IntLitExpr*>(cur)) {
      literalValueOverrides_[lit] =
          neg && lit->value != INT64_MIN ? -lit->value : lit->value;
    }
  }

  TypePtr resolved = resolveType(operand);
  if (expr.op == "~") {
    if (resolved->kind == TypeKind::Int) return operand;
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Integer)) {
      return operand;
    }
    if (dynamicOptionalElem(operand)) return operand;
    sink_.error(expr.opSpan,
                "Operator '~' cannot be applied to " + typePhrase(operand) + ".",
                "'~' requires an integer operand.");
    return typeError();
  }
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
    return operand;
  }
  if (resolved->kind == TypeKind::Var) {
    if (constrainVar(operand, Constraint::Numeric)) return operand;
  }
  if (dynamicOptionalElem(operand)) return operand;
  sink_.error(expr.opSpan,
              "Operator '-' cannot be applied to " + typePhrase(operand) + ".");
  return typeError();
}

TypePtr Sema::numericBinaryResult(const std::string& op, TypePtr lhs, TypePtr rhs, Span span) {
      (void)span; (void)op;  // Dynamic: promotion happens at runtime.
      TypePtr l = resolveType(lhs);
      TypePtr r = resolveType(rhs);
      bool ln = isNumericType(l) || l->kind == TypeKind::Var;
      bool rn = isNumericType(r) || r->kind == TypeKind::Var;
      if (ln && rn) {
        if (l->kind == TypeKind::Var) constrainVar(lhs, Constraint::Numeric);
        if (r->kind == TypeKind::Var) constrainVar(rhs, Constraint::Numeric);
        if (l->kind == TypeKind::Float || r->kind == TypeKind::Float) return typeFloat(64);
        return lhs;
      }
      if (l->kind == TypeKind::Var || r->kind == TypeKind::Var) return lhs;
      // Dynamic: any operand types accepted (runtime decides / coerces).
      return lhs;
    }

    TypePtr Sema::inferBinary(BinaryExpr& expr) {
  const std::string& op = expr.op;

  TypePtr lhs = inferExpr(*expr.lhs);
  // `x != None and x.field ...`: narrow Optional for the RHS (ty/pyrefly flow).
  std::vector<NarrowRestore> narrowed;
  if (op == "and") narrowed = applyNarrowing(*expr.lhs, true);
  TypePtr rhs = inferExpr(*expr.rhs);
  restoreNarrowing(narrowed);

  if (op == "and" || op == "or") {
    requireBool(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireBool(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    return typeBool();
  }

  if (op == "??") {
    TypePtr leftResolved = resolveType(lhs);
    if (leftResolved->kind == TypeKind::Error) return rhs;
    if (leftResolved->kind != TypeKind::Optional) {
      sink_.error(expr.opSpan,
                  "The left side of '\?\?' must be optional, found " + typePhrase(lhs) + ".",
                  "Use '\?\?' with a value of type 'T?', e.g. maybeName ?? \"guest\".");
      return typeError();
    }
    if (!unify(leftResolved->elem, rhs)) {
      reportMismatch(rhs, leftResolved->elem, expr.rhs->span, "in the fallback of '\?\?'");
    }
    return leftResolved->elem;
  }

  if (op == "..") {
    requireInteger(lhs, expr.lhs->span, "The range start");
    requireInteger(rhs, expr.rhs->span, "The range end");
    if (!unify(lhs, rhs)) {
      return typeError();
    }
    return typeRange(lhs);
  }

  if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
    requireInteger(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireInteger(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    if (!unify(lhs, rhs)) {
      return typeError();
    }
    return lhs;
  }

  if (op == "in" || op == "not in") {
    TypePtr container = resolveType(rhs);
    switch (container->kind) {
      case TypeKind::Array:
      case TypeKind::Set:
        if (!unify(lhs, container->elem)) {
          reportMismatch(lhs, container->elem, expr.lhs->span, "in the left operand of '" + op + "'");
        }
        break;
      case TypeKind::Map:
        if (!container->params.empty() && !unify(lhs, container->elem)) {
          reportMismatch(lhs, container->elem, expr.lhs->span, "in the map key of '" + op + "'");
        }
        break;
      case TypeKind::String:
        if (!unify(lhs, typeString())) {
          reportMismatch(lhs, typeString(), expr.lhs->span, "in the left operand of '" + op + "'");
        }
        break;
      case TypeKind::Range:
        requireInteger(lhs, expr.lhs->span, "The left operand of '" + op + "'");
        break;
      case TypeKind::Var:
      case TypeKind::Error:
        break;
      case TypeKind::Optional:
        // A dynamic optional payload may be a container at runtime.
        if (dynamicOptionalElem(container)) break;
        sink_.error(expr.rhs->span,
                    "Right operand of '" + op + "' must be an array, map, set, string, or range, "
                    "found " + typePhrase(container) + ".");
        break;
      default:
        sink_.error(expr.rhs->span,
                    "Right operand of '" + op + "' must be an array, map, set, string, or range, "
                    "found " + typePhrase(container) + ".");
        break;
    }
    return typeBool();
  }

  const bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" ||
                          op == "<=" || op == ">=";

  if (comparison) {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    // `T?` is transparent at runtime (the payload or None), so comparing an
    // optional against a non-optional compares the payload — `opt == 5` is
    // false when opt is None, not a type error. unify() would otherwise try
    // to bind the other side's variable to the Optional itself and fail for
    // constrained vars (e.g. int literals).
    TypePtr cmpL = lhs, cmpR = rhs;
    if (lhsResolved->kind == TypeKind::Optional &&
        rhsResolved->kind != TypeKind::Optional)
      cmpL = lhsResolved->elem;
    if (rhsResolved->kind == TypeKind::Optional &&
        lhsResolved->kind != TypeKind::Optional)
      cmpR = rhsResolved->elem;
    if (!unify(cmpL, cmpR)) {
      sink_.error(expr.opSpan,
                  "Cannot compare " + typePhrase(lhsResolved) + " and " +
                      typePhrase(rhsResolved) + ".");
      return typeError();
    }
    TypePtr type = resolveType(lhs);
    switch (type->kind) {
      case TypeKind::Int:
      case TypeKind::Float:
      case TypeKind::String:
      case TypeKind::Char:
      case TypeKind::Var:
        return typeBool();
      case TypeKind::Optional: {
        if (op != "==" && op != "!=") {
          sink_.error(expr.opSpan, "Optionals can only be compared with '==' and '!='.");
          return typeError();
        }
        // `x == None` / `x != None` is valid for any Optional (ty/pyrefly
        // allow `is not None` on any type). Other payload comparisons stay
        // dynamic at runtime.
        (void)type;
        return typeBool();
      }
      case TypeKind::Bool:
        if (op == "==" || op == "!=") return typeBool();
        sink_.error(expr.opSpan, "Only '==' and '!=' can compare bool values.");
        return typeError();
      case TypeKind::Enum:
        if (op == "==" || op == "!=") return typeBool();
        sink_.error(expr.opSpan, "Only '==' and '!=' can compare enum values.");
        return typeError();
      default:
        return typeBool();  // dynamic: runtime compares
    }
  }

  if (op == "+") {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    bool lhsString = lhsResolved->kind == TypeKind::String;
    bool rhsString = rhsResolved->kind == TypeKind::String;
    if (lhsString || rhsString) {
      // str + any concatenates (the runtime formats the other operand).
      return typeString();
    }

    // The remaining '+' forms the runtime supports: numeric+numeric,
    // Bytes+Bytes, list+list, tuple+tuple. Classify each operand; an
    // unresolved (Constraint::None) variable or Json may still become any
    // of them, so those stay dynamic.
    auto plusShape = [](const TypePtr& t) -> int {
      if (!t || t->kind == TypeKind::Error || t->kind == TypeKind::Json) return 0;
      if (t->kind == TypeKind::Optional) {
        // `T?` is only a static fiction: at runtime the value is either the
        // payload or None. When the payload is still an unresolved variable
        // (e.g. `map.get` on a dynamic receiver) the whole optional stays
        // dynamic; a concrete payload keeps its real shape below.
        TypePtr e = resolveType(t->elem);
        return (e && e->kind == TypeKind::Var) ? 0 : 5;
      }
      if (t->kind == TypeKind::Var) {
        return t->constraint == Constraint::None ? 0 : 1;  // constrained => numeric
      }
      if (isNumericType(t)) return 1;
      if (t->kind == TypeKind::Bytes) return 2;
      if (t->kind == TypeKind::Array) return 3;
      if (t->kind == TypeKind::Tuple) return 4;
      return 5;
    };
    const int ls = plusShape(lhsResolved);
    const int rs = plusShape(rhsResolved);
    if (ls == 0 && rs == 0) {
      return numericBinaryResult(op, lhs, rhs, expr.opSpan);
    }
    if (ls == 0 || rs == 0) {
      // One side still unknown: bind it to the known operand when that
      // operand has a '+'-able shape, so the result type is useful; other
      // shapes (bool, map, struct, ...) stay fully dynamic.
      const bool lhsUnknown = ls == 0;
      TypePtr known = lhsUnknown ? rhs : lhs;
      TypePtr dyn = lhsUnknown ? lhs : rhs;
      const int ks = lhsUnknown ? rs : ls;
      if (ks >= 1 && ks <= 4 && unify(dyn, known)) return known;
      return numericBinaryResult(op, lhs, rhs, expr.opSpan);
    }
    if (ls == 1 && rs == 1) {
      return numericBinaryResult(op, lhs, rhs, expr.opSpan);
    }
    if (ls == rs && ls >= 2 && ls <= 4) {
      // Same-shape concatenation; unify so shared element types stay
      // consistent, but stay permissive (the runtime concatenates anyway).
      unify(lhs, rhs);
      return lhs;
    }
    sink_.error(expr.opSpan,
                "Operator '+' cannot be applied to " + typePhrase(lhsResolved) +
                    " and " + typePhrase(rhsResolved) + ".");
    return typeError();
  }

  if (op == "*") {
    // str * int repeats the string; the count must be an integer (the
    // runtime only accepts int operands there, not floats).
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    if (lhsResolved->kind == TypeKind::String ||
        rhsResolved->kind == TypeKind::String) {
      TypePtr countSide = lhsResolved->kind == TypeKind::String ? rhs : lhs;
      TypePtr count = resolveType(countSide);
      bool countOk = count->kind == TypeKind::Int ||
                     (count->kind == TypeKind::Var &&
                      count->constraint == Constraint::Integer);
      if (count->kind == TypeKind::Var && count->constraint == Constraint::None) {
        if (constrainVar(countSide, Constraint::Integer)) countOk = true;
      }
      // A dynamic optional may hold an int count at runtime.
      if (!countOk && dynamicOptionalElem(countSide)) countOk = true;
      if (countOk) return typeString();
      // String * non-int still reaches the numeric check below and errors.
    }
  }

  if (op == "-" || op == "*" || op == "/" || op == "//" || op == "%" || op == "**") {
    // Arithmetic other than '+' requires numeric operands; the runtime
    // panics on anything else (e.g. "a" - "b", true + 1 handled above).
    TypePtr ln = resolveType(lhs)->kind == TypeKind::Error
                     ? typeError()
                     : requireNumeric(lhs, expr.lhs->span,
                                      "The left operand of '" + op + "'");
    TypePtr rn = resolveType(rhs)->kind == TypeKind::Error
                     ? typeError()
                     : requireNumeric(rhs, expr.rhs->span,
                                      "The right operand of '" + op + "'");
    if (resolveType(ln)->kind == TypeKind::Error ||
        resolveType(rn)->kind == TypeKind::Error) {
      return typeError();
    }
    return numericBinaryResult(op, lhs, rhs, expr.opSpan);
  }

  return numericBinaryResult(op, lhs, rhs, expr.opSpan);
}

TypePtr Sema::inferCall(CallExpr& expr) {
  if (auto* ident = dynamic_cast<IdentExpr*>(expr.callee.get())) {
    const std::string& name = ident->name;
    if (VarSym* sym = lookupVar(name)) {
      sym->used = true;  // using a value as a call target counts as a use
      return checkFunctionValueCall(name, sym->type, expr);
    }
    if (auto imported = fromBindings_.find(name); imported != fromBindings_.end()) {
      FromBinding& binding = imported->second;
      if (binding.kind == FromBinding::Kind::Struct) {
        auto sym = structs_.find(binding.structName);
        if (sym == structs_.end()) return typeError();
        return checkStructConstruction(sym->second, expr);
      }
      if (binding.python) {
        for (auto& arg : expr.args) inferExpr(*arg.value);
        auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
      }
      if (binding.kind == FromBinding::Kind::Function && !binding.native) {
        auto sigIt = signatures_.find(binding.fn);
        if (sigIt == signatures_.end()) return typeError();
        return checkFunctionCall(sigIt->second, expr);
      }
      if (binding.kind == FromBinding::Kind::Function && binding.native) {
        expr.calleeIsBuiltin = true;
        expr.calleeBuiltin = binding.nativeName;
        return checkBuiltinCall(binding.nativeName, expr);
      }
      if (!binding.type) {
        for (auto& arg : expr.args) inferExpr(*arg.value);
        return typeError();
      }
      return checkFunctionValueCall(name, binding.type, expr);
    }
    if (moduleAliases_.count(name)) {
      sink_.error(ident->span, "'" + name + "' is a module, not a function.",
                  "Call a member instead, e.g. " + name + ".function(...).");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    const std::string q = qualified(name);
    auto structIt = structs_.find(q);
    if (structIt != structs_.end()) {
      expr.calleeStruct = structIt->second.decl;
      expr.calleeResolved = true;
      return checkStructConstruction(structIt->second, expr);
    }
    auto fnIt = functionsByName_.find(q);
    if (fnIt != functionsByName_.end()) {
      // Record the declaration so the interpreter can call it directly instead
      // of re-resolving the name through the scope chain on every call.
      expr.calleeFunction = fnIt->second;
      expr.calleeResolved = true;
      auto sigIt = signatures_.find(fnIt->second);
      if (sigIt == signatures_.end()) return typeError();
      return checkFunctionCall(sigIt->second, expr);
    }
    if (isKnownBuiltin(name)) {
      expr.builtinId = builtinIdForName(name);
      expr.calleeIsBuiltin = true;
      expr.calleeBuiltin = name;
      return checkBuiltinCall(name, expr);
    }
    if (isBuiltinConstant(name) || name == "None") {
      sink_.error(ident->span, "'" + name + "' is a constant, not a function.");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    // Prelude functions act like Python builtins: callable unqualified.
    if (const FnDecl* preludeFn = findPreludeFunction(name)) {
      expr.calleeFunction = preludeFn;
      expr.calleeResolved = true;
      auto sigIt = signatures_.find(preludeFn);
      if (sigIt == signatures_.end()) return typeError();
      return checkFunctionCall(sigIt->second, expr);
    }
    std::string undefFn = "Undefined function '" + name + "'.";
    if (std::string sug = suggestName(name); !sug.empty())
      undefFn += " Did you mean '" + sug + "'?";
    sink_.error(ident->span, undefFn,
                "Functions must be declared before they are called.");
    return typeError();
  }

  if (auto* field = dynamic_cast<FieldExpr*>(expr.callee.get())) {
    TypePtr objectType = inferExpr(*field->object);
    TypePtr object = resolveType(objectType);
    if (object->kind == TypeKind::Module) {
      ModuleAlias alias;
      alias.native = object->moduleId < 0;
      alias.moduleId = object->moduleId;
      alias.nativeName = object->structName;
      return moduleMemberCall(alias, field->field, expr);
    }
    if (object->kind == TypeKind::Enum) {
      auto enumIt = enums_.find(object->structName);
      if (enumIt != enums_.end() && enumIt->second.algebraic &&
          enumNamespaceUse(field->object, object->structName)) {
        return checkVariantConstruction(enumIt->second, *field, expr);
      }
    }
    return checkMethodCall(objectType, *field, expr);
  }

  TypePtr calleeType = inferExpr(*expr.callee);
  TypePtr resolved = resolveType(calleeType);
  if (resolved->kind == TypeKind::Function) {
    return checkFunctionValueCall("this expression", calleeType, expr);
  }
  if (resolved->kind == TypeKind::Var) {
    // Dynamic value: callability is decided at runtime.
    for (auto& arg : expr.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (resolved->kind != TypeKind::Error) {
    sink_.error(expr.callee->span,
                "Values of type '" + typeName(resolved) + "' are not callable.",
                "Only functions and lambdas can be called.");
  }
  for (auto& arg : expr.args) inferExpr(*arg.value);
  return typeError();
}

}  // namespace a
