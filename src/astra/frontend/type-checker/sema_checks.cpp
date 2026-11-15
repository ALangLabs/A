#include "type-checker/sema_internal.hpp"

#include <cfloat>
#include <cmath>
#include <cstdio>

namespace a {
void Sema::checkTypeVariables() {
  std::lock_guard<std::recursive_mutex> lock(gVarMutex);
  for (auto& info : typeVariables_) {
    TypePtr resolved = resolveType(info.var);
    if (!resolved || resolved->kind != TypeKind::Var) continue;
    // Defaulted types are flagged so literal range checks can tell an
    // inferred i32 apart from a user-written `i32` annotation.
    auto linkDefault = [&resolved](TypePtr type) {
      type->implicitDefault = true;
      resolved->link = std::move(type);
    };
    if (resolved->allowDefault) {
      if (resolved->constraint == Constraint::Integer) {
        linkDefault(typeInt(32, true));
        continue;
      }
      if (resolved->constraint == Constraint::Floating) {
        linkDefault(typeFloat(64));
        continue;
      }
    }
    if (resolved->constraint == Constraint::Integer) linkDefault(typeInt(32, true));
    else if (resolved->constraint == Constraint::Floating) linkDefault(typeFloat(64));
    else if (resolved->constraint == Constraint::Numeric) linkDefault(typeInt(64, true));
    continue;
  }
}

bool Sema::checkSizedIntRange(Span span, std::int64_t value, bool negated,
                              const TypePtr& type) {
  TypePtr resolved = resolveType(type);
  if (!resolved || resolved->kind != TypeKind::Int || resolved->implicitDefault) {
    return false;
  }
  if (sizedIntHolds(resolved, value, negated)) return false;
  sink_.error(span,
              "The value " + std::to_string(value) + " does not fit in type '" +
                  typeName(resolved) + "'.",
              "Use a wider integer type or a smaller value.");
  return true;
}

void Sema::checkLiteralRanges(std::size_t begin) {
  for (std::size_t i = begin; i < numericLiterals_.size(); ++i) {
    const Expr* expr = numericLiterals_[i];
    auto it = exprTypes_.find(expr);
    if (it == exprTypes_.end()) continue;
    TypePtr type = resolveType(it->second);
    if (!type) continue;

    if (const auto* lit = dynamic_cast<const IntLitExpr*>(expr)) {
      std::int64_t value = lit->value;
      bool negated = false;
      if (auto ov = literalValueOverrides_.find(expr);
          ov != literalValueOverrides_.end()) {
        value = ov->second;
        negated = true;
      }
      checkSizedIntRange(expr->span, value, negated, type);
      continue;
    }

    if (const auto* flit = dynamic_cast<const FloatLitExpr*>(expr)) {
      // Only narrow floats can overflow; f64 literals already fit f64, and an
      // implicit default means the user never asked for a sized type.
      if (type->kind != TypeKind::Float || type->bits >= 64 || type->implicitDefault) {
        continue;
      }
      const double v = flit->value;
      if (std::isfinite(v) && std::abs(v) <= static_cast<double>(FLT_MAX)) continue;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.6g", v);
      sink_.error(expr->span,
                  std::string("The value ") + buf + " does not fit in type '" +
                      typeName(type) + "'.",
                  "Use a smaller value or the 'f64' type.");
    }
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
