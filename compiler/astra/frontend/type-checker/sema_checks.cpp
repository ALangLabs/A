#include "type-checker/sema_internal.hpp"

namespace a {
void Sema::checkTypeVariables() {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  for (auto& info : typeVariables_) {
    TypePtr resolved = resolveType(info.var);
    if (!resolved || resolved->kind != TypeKind::Var) continue;
    if (resolved->allowDefault) {
      if (resolved->constraint == Constraint::Integer) {
        resolved->link = typeInt(32, true);
        continue;
      }
      if (resolved->constraint == Constraint::Floating) {
        resolved->link = typeFloat(64);
        continue;
      }
    }
    if (resolved->constraint == Constraint::Integer) resolved->link = typeInt(32, true);
    else if (resolved->constraint == Constraint::Floating) resolved->link = typeFloat(64);
    else if (resolved->constraint == Constraint::Numeric) resolved->link = typeInt(64, true);
    continue;
  }
}

void Sema::checkDeclaredVariables() {
  for (auto& info : declaredVariables_) {
    TypePtr resolved = resolveType(info.type);
    if (resolved && resolved->kind == TypeKind::Void) {
      sink_.error(info.span, "Variable '" + info.name + "' cannot have type void.");
    }
  }
}

void Sema::checkReturnPaths() {
  for (auto& entry : checkedFunctions_) {
    const FnDecl* decl = entry.first;
    TypePtr ret = resolveType(entry.second);
    if (!ret || ret->kind == TypeKind::Void || ret->kind == TypeKind::Var) continue;
    if (!blockReturns(decl->body)) {
      sink_.error(decl->nameSpan,
                  "Not all paths in function '" + decl->name + "' return a value of type '" +
                      typeName(ret) + "'.",
                  "Add a 'return' at the end, or use if/else so that every branch returns.");
    }
  }
}

bool Sema::blockReturns(const std::vector<StmtPtr>& body) {
  if (body.empty()) return false;
  // Statements after a terminator are unreachable; the last reachable one
  // decides whether the block returns.
  const Stmt* last = nullptr;
  for (const auto& stmt : body) {
    if (last && terminatesFlow(*last)) break;
    last = stmt.get();
  }
  return last && statementReturns(*last);
}

bool Sema::statementReturns(const Stmt& stmt) {
  if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
  if (auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
    if (node->elseBody.empty()) return false;
    for (const auto& branch : node->branches) {
      if (!blockReturns(branch.body)) return false;
    }
    return blockReturns(node->elseBody);
  }
  if (dynamic_cast<const LoopStmt*>(&stmt)) return true;
  return false;
}

}  // namespace a
