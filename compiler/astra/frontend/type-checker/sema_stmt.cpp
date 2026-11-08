#include "type-checker/sema_internal.hpp"

namespace a {
// ------------------------------------------------------------- statement checks

void Sema::checkStatements(std::vector<StmtPtr>& statements) {
  bool terminated = false;
  bool warned = false;
  for (auto& stmt : statements) {
    if (terminated && !warned) {
      sink_.warning(stmt->span, "W0002", "Unreachable statement.",
                    "Control cannot reach this statement because an earlier one always exits.");
      warned = true;
    }
    checkStatement(*stmt);
    if (!terminated && terminatesFlow(*stmt)) terminated = true;
  }
}

void Sema::checkStatement(Stmt& stmt) {
  if (auto* node = dynamic_cast<LetStmt*>(&stmt)) {
    checkLet(*node);
    return;
  }
  if (auto* node = dynamic_cast<AssignStmt*>(&stmt)) {
    checkAssign(*node);
    return;
  }
  if (auto* node = dynamic_cast<DestructureStmt*>(&stmt)) {
    checkDestructure(*node);
    return;
  }
  if (auto* node = dynamic_cast<ExprStmt*>(&stmt)) {
    inferExpr(*node->expr);
    return;
  }
  if (auto* node = dynamic_cast<IfStmt*>(&stmt)) {
    checkIf(*node);
    return;
  }
  if (auto* node = dynamic_cast<WhileStmt*>(&stmt)) {
    checkWhile(*node);
    return;
  }
  if (auto* node = dynamic_cast<DoWhileStmt*>(&stmt)) {
    checkDoWhile(*node);
    return;
  }
  if (auto* node = dynamic_cast<ForStmt*>(&stmt)) {
    checkFor(*node);
    return;
  }
  if (auto* node = dynamic_cast<LoopStmt*>(&stmt)) {
    checkLoop(*node);
    return;
  }
  if (auto* node = dynamic_cast<ReturnStmt*>(&stmt)) {
    checkReturn(*node);
    return;
  }
  if (auto* node = dynamic_cast<DeferStmt*>(&stmt)) {
    checkDefer(*node);
    return;
  }
  if (auto* node = dynamic_cast<BreakStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'break' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'break' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<ContinueStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'continue' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'continue' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<FnDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Functions can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<TraitDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Traits can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<ImplDecl*>(&stmt)) {
    sink_.error(node->span, "Conformances ('impl ... for') can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<StructDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Structs can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<EnumDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Enums can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<ImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
    return;
  }
  if (auto* node = dynamic_cast<FromImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
    return;
  }
}

void Sema::checkLet(LetStmt& stmt) {
  TypePtr initType = inferExpr(*stmt.init);
  TypePtr variableType = initType;
  if (stmt.type) {
    TypePtr declared = resolveTypeExpr(*stmt.type);
    if (resolveType(declared)->kind == TypeKind::Void) {
      sink_.error(stmt.span, "A variable cannot have type void.");
      declared = typeError();
    }
    if (!unify(initType, declared)) {
      reportMismatch(initType, declared, stmt.init->span,
                     "in the initializer for '" + stmt.name + "'");
    } else {
      checkFixedArrayLength(*stmt.init, declared);
    }
    variableType = declared;
  } else if (resolveType(initType)->kind == TypeKind::Void) {
    sink_.error(stmt.init->span,
                "Cannot initialize '" + stmt.name + "' with a void value.");
    variableType = typeError();
  }

  declareVar(stmt.name, stmt.nameSpan, variableType, stmt.mutableBinding,
             "variable", false);
}

void Sema::checkAssign(AssignStmt& stmt) {
  if (auto* ident = dynamic_cast<IdentExpr*>(stmt.target.get())) {
    VarSym* sym = lookupVar(ident->name);
    if (!sym) {
      if (isBuiltinConstant(ident->name) || ident->name == "None") {
        sink_.error(ident->span,
                    "Cannot assign to the builtin constant '" + ident->name + "'.");
        inferExpr(*stmt.value);
        return;
      }
      if (stmt.op != "=") {
        sink_.error(stmt.target->span,
                    "Undefined variable '" + ident->name + "' in compound assignment.");
        return;
      }
      TypePtr valueType = inferExpr(*stmt.value);
      if (resolveType(valueType)->kind == TypeKind::Void) {
        sink_.error(stmt.value->span,
                    "Cannot initialize '" + ident->name + "' with a void value.");
        valueType = typeError();
      }
      declareVar(ident->name, ident->span, valueType, true);
      return;
    }

    sym->mutableBinding = true;
    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      sym->used = true;
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, sym->type, stmt.opSpan)) return;
      if (!unify(valueType, sym->type)) {
        reportMismatch(valueType, sym->type, stmt.value->span,
                       "in assignment to '" + ident->name + "'");
      } else {
        checkFixedArrayLength(*stmt.value, sym->type);
      }
    } else {
      // Dynamic: reassignment may change type (Python-style).
      sym->type = valueType;
    }
    return;
  }

  if (dynamic_cast<FieldExpr*>(stmt.target.get()) ||
      dynamic_cast<IndexExpr*>(stmt.target.get())) {
    TypePtr targetType = inferExpr(*stmt.target);
    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, targetType, stmt.opSpan)) return;
    }
    if (!unify(valueType, targetType)) {
      reportMismatch(valueType, targetType, stmt.value->span, "in assignment");
    } else {
      checkFixedArrayLength(*stmt.value, targetType);
    }
    return;
  }

  sink_.error(stmt.target->span,
              "Invalid assignment target.",
              "Only variables, struct fields, and array elements can be assigned to.");
}

void Sema::checkDestructure(DestructureStmt& stmt) {
  TypePtr valueType = inferExpr(*stmt.value);
  TypePtr resolved = resolveType(valueType);

  // Dynamic: always bind names (arity/type checked at runtime).
  for (std::size_t i = 0; i < stmt.names.size(); ++i) {
    const std::string& name = stmt.names[i];
    if (name == "_") continue;
    TypePtr element = typeError();
    if (resolved->kind == TypeKind::Tuple && i < resolved->params.size())
      element = resolved->params[i];
    if (resolveType(element)->kind == TypeKind::Void) element = typeError();
    declareVar(name, stmt.nameSpans[i], element, stmt.mutableBinding);
  }
}

std::vector<std::pair<std::string, bool>> Sema::narrowingFacts(Expr& cond, bool truthy) {
  std::vector<std::pair<std::string, bool>> facts;
  auto isNoneExpr = [](Expr* e) -> bool {
    if (!e) return false;
    if (auto* id = dynamic_cast<IdentExpr*>(e)) return id->name == "None";
    return false;
  };
  if (auto* unary = dynamic_cast<UnaryExpr*>(&cond)) {
    if ((unary->op == "not" || unary->op == "!") && unary->operand) {
      return narrowingFacts(*unary->operand, !truthy);
    }
    return facts;
  }
  if (auto* bin = dynamic_cast<BinaryExpr*>(&cond)) {
    if (bin->op == "and") {
      if (truthy) {
        facts = narrowingFacts(*bin->lhs, true);
        auto more = narrowingFacts(*bin->rhs, true);
        facts.insert(facts.end(), more.begin(), more.end());
      }
      // `a and b` false: neither operand is known, no facts.
      return facts;
    }
    if (bin->op == "or") {
      if (!truthy) {
        facts = narrowingFacts(*bin->lhs, false);
        auto more = narrowingFacts(*bin->rhs, false);
        facts.insert(facts.end(), more.begin(), more.end());
      }
      return facts;
    }
    if (bin->op == "==" || bin->op == "!=") {
      std::string varName;
      if (auto* lhs = dynamic_cast<IdentExpr*>(bin->lhs.get())) {
        if (isNoneExpr(bin->rhs.get())) varName = lhs->name;
      } else if (auto* rhs = dynamic_cast<IdentExpr*>(bin->rhs.get())) {
        if (isNoneExpr(bin->lhs.get())) varName = rhs->name;
      }
      if (!varName.empty()) {
        bool nonNone = (bin->op == "!=") == truthy;
        facts.emplace_back(varName, nonNone);
      }
    }
  }
  return facts;
}

std::vector<Sema::NarrowRestore> Sema::applyNarrowing(Expr& cond, bool truthy) {
  std::vector<NarrowRestore> restores;
  for (auto& [name, nonNone] : narrowingFacts(cond, truthy)) {
    VarSym* sym = lookupVar(name);
    if (!sym) continue;
    // Avoid double-narrowing the same symbol in one condition.
    bool already = false;
    for (auto& r : restores) if (r.sym == sym) { already = true; break; }
    if (already) continue;
    TypePtr resolved = resolveType(sym->type);
    if (!nonNone) continue;  // only refine the non-None branch (T? -> T)
    if (resolved && resolved->kind == TypeKind::Optional && resolved->elem) {
      restores.push_back(NarrowRestore{sym, sym->type});
      sym->type = resolved->elem;
    }
  }
  return restores;
}

void Sema::restoreNarrowing(std::vector<NarrowRestore>& restores) {
  for (auto& r : restores) {
    if (r.sym) r.sym->type = r.saved;
  }
  restores.clear();
}

void Sema::checkIf(IfStmt& stmt) {
  bool prevBranchAlwaysReturns = false;
  for (std::size_t i = 0; i < stmt.branches.size(); ++i) {
    auto& branch = stmt.branches[i];
    TypePtr condType = inferExpr(*branch.cond);
    requireBool(condType, branch.cond->span, "The 'if' condition");
    bool isLastBranch = (i + 1 == stmt.branches.size());
    if (!prevBranchAlwaysReturns && isLastBranch && stmt.elseBody.empty() &&
        narrowingFacts(*branch.cond, true).empty()) {
      // No-op: most branches have no narrowing; fall through.
    }
    // Elif: previous branches were false, so their negated facts apply.
    std::vector<NarrowRestore> outerRestores;
    for (std::size_t j = 0; j < i; ++j) {
      auto nested = applyNarrowing(*stmt.branches[j].cond, false);
      outerRestores.insert(outerRestores.end(), nested.begin(), nested.end());
    }
    auto restores = applyNarrowing(*branch.cond, true);
    pushScope();
    checkStatements(branch.body);
    popScope();
    restoreNarrowing(restores);
    restoreNarrowing(outerRestores);
    if (!branch.body.empty() && terminatesFlow(*branch.body.back())) prevBranchAlwaysReturns = true;
  }
  if (!stmt.elseBody.empty()) {
    std::vector<NarrowRestore> outerRestores;
    for (auto& branch : stmt.branches) {
      auto nested = applyNarrowing(*branch.cond, false);
      outerRestores.insert(outerRestores.end(), nested.begin(), nested.end());
    }
    pushScope();
    checkStatements(stmt.elseBody);
    popScope();
    restoreNarrowing(outerRestores);
  }
}

void Sema::checkWhile(WhileStmt& stmt) {
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkDoWhile(DoWhileStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
}

TypePtr Sema::iterableElementType(TypePtr iterableType, Span span, const std::string& hint) {
  TypePtr resolved = resolveType(iterableType);
  if (resolved->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Range ||
      resolved->kind == TypeKind::Set) {
    return resolved->elem;
  }
  if (resolved->kind == TypeKind::Map) return resolved->elem;  // keys
  if (resolved->kind == TypeKind::String) return typeChar();
  if (resolved->kind == TypeKind::Bytes) return typeInt(64, true);
  if (resolved->kind == TypeKind::Tuple) {
    // Tuples iterate element-by-element at runtime (a_iter handles them), but
    // elements may be heterogeneous, so the loop variable is unconstrained.
    return freshVar(Constraint::None, span,
                    "the element type iterated over a tuple");
  }
  sink_.error(span,
              "Cannot iterate over " + typePhrase(resolved) + ".",
              "Loops " + hint + " iterate over arrays, tuples, strings, maps (keys), sets, "
              "and integer "
              "ranges ('0..10').");
  return typeError();
}

void Sema::checkFor(ForStmt& stmt) {
  TypePtr elementType =
      iterableElementType(inferExpr(*stmt.iterable), stmt.iterable->span, "and comprehensions");

  ++loopDepth_;
  pushScope();
  declareVar(stmt.var, stmt.varSpan, elementType, false, "loop variable");
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkLoop(LoopStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkReturn(ReturnStmt& stmt) {
  if (!currentFunction_) {
    sink_.error(stmt.span, "'return' used outside of a function.");
    return;
  }
  if (deferDepth_ > 0) {
    sink_.error(stmt.span, "'return' cannot be used inside a defer statement.");
  }
  sawReturn_ = true;

  TypePtr expected = currentReturnType_;
  TypePtr resolvedExpected = resolveType(expected);

  if (stmt.value) {
    TypePtr valueType = inferExpr(*stmt.value);
    if (resolvedExpected->kind == TypeKind::Void && currentFunction_->returnType) {
      sink_.error(stmt.value->span,
                  "Function '" + currentFunction_->name +
                      "' has return type void and cannot return a value.");
      return;
    }
    if (!unify(valueType, expected)) {
      reportMismatch(valueType, expected, stmt.value->span,
                     "in the return value of '" + currentFunction_->name + "'");
    } else {
      checkFixedArrayLength(*stmt.value, expected);
    }
  } else {
    if (resolvedExpected->kind != TypeKind::Var &&
        resolvedExpected->kind != TypeKind::Void) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name + "' must return a value of type '" +
                      typeName(resolvedExpected) + "'.");
      return;
    }
    if (!unify(expected, typeVoid())) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name +
                      "' is expected to return a value, but this 'return' has none.");
    }
  }
}

void Sema::checkDefer(DeferStmt& stmt) {
  ++deferDepth_;
  pushScope();
  checkStatement(*stmt.body);
  popScope();
  --deferDepth_;
}

}  // namespace a
