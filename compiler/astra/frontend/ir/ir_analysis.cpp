#include "ir/ir_internal.hpp"

namespace a::ir {
TypePtr intType() { return typeInt(64, true); }

bool lookupEnv(const StringMap<int>& env, const std::string& name, int& out) {
  auto found = env.find(name);
  if (found == env.end()) return false;
  out = found->second;
  return true;
}

// Names assigned by a statement list. Loop headers need this before the body is
// lowered so the phis that merge the preheader and backedge values exist.
void collectAssignedExpr(const Expr& expr, std::set<std::string>& names);
void collectAssignedStmt(const Stmt& stmt, std::set<std::string>& names);

void collectAssigned(const std::vector<StmtPtr>& stmts, std::set<std::string>& names) {
  for (const auto& stmt : stmts) {
    switch (stmt->kind) {
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(*stmt);
        if (assign.target->kind == ExprKind::Ident) {
          names.insert(static_cast<const IdentExpr&>(*assign.target).name);
        } else {
          collectAssignedExpr(*assign.target, names);
        }
        collectAssignedExpr(*assign.value, names);
        break;
      }
      case StmtKind::Destructure: {
        const auto& destructure = static_cast<const DestructureStmt&>(*stmt);
        for (const auto& name : destructure.names) {
          if (name != "_") names.insert(name);
        }
        collectAssignedExpr(*destructure.value, names);
        break;
      }
      case StmtKind::Expr:
        collectAssignedExpr(*static_cast<const ExprStmt&>(*stmt).expr, names);
        break;
      case StmtKind::If: {
        const auto& branch = static_cast<const IfStmt&>(*stmt);
        for (const auto& part : branch.branches) {
          collectAssignedExpr(*part.cond, names);
          collectAssigned(part.body, names);
        }
        collectAssigned(branch.elseBody, names);
        break;
      }
      case StmtKind::While:
        collectAssigned(static_cast<const WhileStmt&>(*stmt).body, names);
        break;
      case StmtKind::DoWhile:
        collectAssigned(static_cast<const DoWhileStmt&>(*stmt).body, names);
        break;
      case StmtKind::For:
        collectAssigned(static_cast<const ForStmt&>(*stmt).body, names);
        break;
      case StmtKind::Loop:
        collectAssigned(static_cast<const LoopStmt&>(*stmt).body, names);
        break;
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(*stmt);
        if (defer.body) collectAssignedStmt(*defer.body, names);
        break;
      }
      default:
        break;
    }
  }
}

void collectAssignedStmt(const Stmt& stmt, std::set<std::string>& names) {
  switch (stmt.kind) {
    case StmtKind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      if (assign.target->kind == ExprKind::Ident) {
        names.insert(static_cast<const IdentExpr&>(*assign.target).name);
      }
      collectAssignedExpr(*assign.value, names);
      break;
    }
    case StmtKind::Expr:
      collectAssignedExpr(*static_cast<const ExprStmt&>(stmt).expr, names);
      break;
    default:
      break;
  }
}

void collectAssignedExpr(const Expr& expr, std::set<std::string>& names) {
  switch (expr.kind) {
    case ExprKind::Ident:
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
      return;
    case ExprKind::StringLit: {
      for (const auto& part : static_cast<const StringLitExpr&>(expr).parts) {
        if (part.isExpr && part.expr) collectAssignedExpr(*part.expr, names);
      }
      return;
    }
    case ExprKind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if ((unary.op == "++" || unary.op == "--") && unary.operand->kind == ExprKind::Ident) {
        names.insert(static_cast<const IdentExpr&>(*unary.operand).name);
      }
      collectAssignedExpr(*unary.operand, names);
      return;
    }
    case ExprKind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      collectAssignedExpr(*binary.lhs, names);
      collectAssignedExpr(*binary.rhs, names);
      return;
    }
    case ExprKind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      collectAssignedExpr(*call.callee, names);
      for (const auto& arg : call.args) collectAssignedExpr(*arg.value, names);
      return;
    }
    case ExprKind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expr);
      collectAssignedExpr(*index.object, names);
      collectAssignedExpr(*index.index, names);
      return;
    }
    case ExprKind::Slice: {
      const auto& slice = static_cast<const SliceExpr&>(expr);
      collectAssignedExpr(*slice.object, names);
      if (slice.start) collectAssignedExpr(*slice.start, names);
      if (slice.end) collectAssignedExpr(*slice.end, names);
      if (slice.step) collectAssignedExpr(*slice.step, names);
      return;
    }
    case ExprKind::Field: {
      collectAssignedExpr(*static_cast<const FieldExpr&>(expr).object, names);
      return;
    }
    case ExprKind::SafeNav: {
      collectAssignedExpr(*static_cast<const SafeNavExpr&>(expr).object, names);
      return;
    }
    case ExprKind::ArrayLit: {
      for (const auto& element : static_cast<const ArrayLitExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::Range: {
      const auto& range = static_cast<const RangeExpr&>(expr);
      collectAssignedExpr(*range.start, names);
      collectAssignedExpr(*range.end, names);
      if (range.step) collectAssignedExpr(*range.step, names);
      return;
    }
    case ExprKind::Ternary: {
      const auto& ternary = static_cast<const TernaryExpr&>(expr);
      collectAssignedExpr(*ternary.cond, names);
      collectAssignedExpr(*ternary.thenValue, names);
      collectAssignedExpr(*ternary.elseValue, names);
      return;
    }
    case ExprKind::Tuple: {
      for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::MapLit: {
      for (const auto& entry : static_cast<const MapLitExpr&>(expr).entries) {
        collectAssignedExpr(*entry.first, names);
        collectAssignedExpr(*entry.second, names);
      }
      return;
    }
    case ExprKind::SetLit: {
      for (const auto& element : static_cast<const SetLitExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::Lambda:
    case ExprKind::Comprehension:
    case ExprKind::Match:
    case ExprKind::Try:
      return;
  }
}

// LLVM cast family: maps a source/destination type pair to the LLVM cast
// opcode ("" when the pair needs no cast, e.g. identical types).
std::string castKindFor(const TypePtr& srcIn, const TypePtr& dstIn) {
  TypePtr src = defaultType(resolveType(srcIn));
  TypePtr dst = defaultType(resolveType(dstIn));
  if (!src || !dst) return "";
  if (src->kind == TypeKind::Int && dst->kind == TypeKind::Int) {
    if (src->bits > dst->bits) return "trunc";
    if (src->bits < dst->bits) return src->isSigned ? "sext" : "zext";
    return src->isSigned == dst->isSigned ? "" : "bitcast";
  }
  if (src->kind == TypeKind::Int && dst->kind == TypeKind::Float) {
    return src->isSigned ? "sitofp" : "uitofp";
  }
  if (src->kind == TypeKind::Float && dst->kind == TypeKind::Int) {
    return dst->isSigned ? "fptosi" : "fptoui";
  }
  if (src->kind == TypeKind::Float && dst->kind == TypeKind::Float) {
    if (src->bits > dst->bits) return "fptrunc";
    if (src->bits < dst->bits) return "fpext";
    return "";
  }
  return "";
}

// Numeric conversion builtins lower to LLVM `cast` instructions when the
// operand/result types are known; anything else stays a runtime call.
bool isConversionName(const std::string& name) {
  static const char* names[] = {"i8",  "i16", "i32", "i64", "u8",
                                "u16", "u32", "u64", "f32", "f64",
                                "int", "float"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

}  // namespace a::ir
