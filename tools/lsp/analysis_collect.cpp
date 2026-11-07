#include "analysis_internal.hpp"

namespace a::lsp {
namespace {

// Walks every module AST and records call expressions together with the
// innermost enclosing named function/method. Mirrors IndexBuilder's coverage
// so no call site is missed (impl methods included, which the index skips).
class CallSiteCollector {
 public:
  explicit CallSiteCollector(const Compilation& compilation) : compilation_(compilation) {}

  std::vector<CallSite> run() {
    for (std::size_t i = 0; i < compilation_.graph.modules.size(); ++i) {
      const Module& module = compilation_.graph.modules[i];
      if (!module.program) continue;
      moduleId_ = static_cast<int>(i);
      callables_.clear();
      walkBlock(module.program->statements, std::string());
    }
    return std::move(sites_);
  }

 private:
  struct Callable {
    SymbolKind kind = SymbolKind::Function;
    std::string displayName;
    std::string detail;
    Span nameSpan;
    Span span;  // full declaration span (body included)
  };

  void walkBlock(const std::vector<StmtPtr>& body, const std::string& ownerType) {
    for (const auto& statement : body) {
      if (statement) walkStmt(*statement, ownerType);
    }
  }

  // `ownerType` is the enclosing class/impl target when the statement list
  // belongs to a type body; it is reset inside function bodies so nested
  // functions are not mistaken for methods.
  void walkStmt(Stmt& stmt, const std::string& ownerType) {
    switch (stmt.kind) {
      case StmtKind::Let:
        walkExpr(static_cast<LetStmt&>(stmt).init.get());
        return;
      case StmtKind::Assign: {
        auto& assign = static_cast<AssignStmt&>(stmt);
        walkExpr(assign.target.get());
        walkExpr(assign.value.get());
        return;
      }
      case StmtKind::Destructure:
        walkExpr(static_cast<DestructureStmt&>(stmt).value.get());
        return;
      case StmtKind::Expr:
        walkExpr(static_cast<ExprStmt&>(stmt).expr.get());
        return;
      case StmtKind::Return:
        walkExpr(static_cast<ReturnStmt&>(stmt).value.get());
        return;
      case StmtKind::Break:
      case StmtKind::Continue:
      case StmtKind::Import:
      case StmtKind::FromImport:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
        return;
      case StmtKind::Defer: {
        auto& defer = static_cast<DeferStmt&>(stmt);
        if (defer.body) walkStmt(*defer.body, ownerType);
        return;
      }
      case StmtKind::If: {
        auto& ifStmt = static_cast<IfStmt&>(stmt);
        for (auto& branch : ifStmt.branches) {
          walkExpr(branch.cond.get());
          walkBlock(branch.body, ownerType);
        }
        walkBlock(ifStmt.elseBody, ownerType);
        return;
      }
      case StmtKind::While: {
        auto& whileStmt = static_cast<WhileStmt&>(stmt);
        walkExpr(whileStmt.cond.get());
        walkBlock(whileStmt.body, ownerType);
        return;
      }
      case StmtKind::DoWhile: {
        auto& doStmt = static_cast<DoWhileStmt&>(stmt);
        walkBlock(doStmt.body, ownerType);
        walkExpr(doStmt.cond.get());
        return;
      }
      case StmtKind::For: {
        auto& forStmt = static_cast<ForStmt&>(stmt);
        walkExpr(forStmt.iterable.get());
        walkBlock(forStmt.body, ownerType);
        return;
      }
      case StmtKind::Loop: {
        walkBlock(static_cast<LoopStmt&>(stmt).body, ownerType);
        return;
      }
      case StmtKind::FnDecl:
        walkCallable(static_cast<FnDecl&>(stmt), ownerType);
        return;
      case StmtKind::StructDecl: {
        auto& decl = static_cast<StructDecl&>(stmt);
        for (auto& method : decl.methods) {
          if (method) walkCallable(*method, decl.name);
        }
        return;
      }
      case StmtKind::EnumDecl: {
        auto& decl = static_cast<EnumDecl&>(stmt);
        for (auto& variant : decl.variants) walkExpr(variant.value.get());
        return;
      }
      case StmtKind::ImplDecl: {
        auto& impl = static_cast<ImplDecl&>(stmt);
        for (auto& method : impl.methods) {
          if (method) walkCallable(*method, impl.targetName);
        }
        return;
      }
    }
  }

  void walkCallable(FnDecl& fn, const std::string& ownerType) {
    Callable ctx;
    ctx.kind = ownerType.empty() ? SymbolKind::Function : SymbolKind::Method;
    ctx.displayName = ownerType.empty() ? fn.name : ownerType + "." + fn.name;
    ctx.detail = signatureText(fn);
    ctx.nameSpan = fn.nameSpan;
    ctx.span = blockSpan(fn.body, fn.span);
    callables_.push_back(ctx);
    walkBlock(fn.body, std::string());
    callables_.pop_back();
  }

  void recordCall(CallExpr& call) {
    CallSite site;
    site.moduleId = moduleId_;
    site.callSpan = call.span;
    if (call.callee) {
      if (const auto* field = dynamic_cast<const FieldExpr*>(call.callee.get())) {
        site.calleeSpan = field->fieldSpan;
      } else if (const auto* safe = dynamic_cast<const SafeNavExpr*>(call.callee.get())) {
        site.calleeSpan = safe->fieldSpan;
      } else {
        site.calleeSpan = call.callee->span;
      }
    }
    for (const CallArg& arg : call.args) {
      if (arg.name.empty() && arg.value) site.positionalArgSpans.push_back(arg.value->span);
    }
    if (!callables_.empty()) {
      const Callable& ctx = callables_.back();
      site.inCallable = true;
      site.callableKind = ctx.kind;
      site.callableName = ctx.displayName;
      site.callableDetail = ctx.detail;
      site.callableNameSpan = ctx.nameSpan;
      site.callableSpan = ctx.span;
    }
    sites_.push_back(std::move(site));
  }

  void walkExpr(Expr* expr) {
    if (!expr) return;
    switch (expr->kind) {
      case ExprKind::Unary:
        walkExpr(static_cast<UnaryExpr&>(*expr).operand.get());
        return;
      case ExprKind::Binary: {
        auto& binary = static_cast<BinaryExpr&>(*expr);
        walkExpr(binary.lhs.get());
        walkExpr(binary.rhs.get());
        return;
      }
      case ExprKind::Call: {
        auto& call = static_cast<CallExpr&>(*expr);
        recordCall(call);
        walkExpr(call.callee.get());
        for (auto& arg : call.args) walkExpr(arg.value.get());
        return;
      }
      case ExprKind::Index: {
        auto& index = static_cast<IndexExpr&>(*expr);
        walkExpr(index.object.get());
        walkExpr(index.index.get());
        return;
      }
      case ExprKind::Slice: {
        auto& slice = static_cast<SliceExpr&>(*expr);
        walkExpr(slice.object.get());
        walkExpr(slice.start.get());
        walkExpr(slice.end.get());
        walkExpr(slice.step.get());
        return;
      }
      case ExprKind::Field:
        walkExpr(static_cast<FieldExpr&>(*expr).object.get());
        return;
      case ExprKind::SafeNav:
        walkExpr(static_cast<SafeNavExpr&>(*expr).object.get());
        return;
      case ExprKind::ArrayLit: {
        for (auto& element : static_cast<ArrayLitExpr&>(*expr).elements) {
          walkExpr(element.get());
        }
        return;
      }
      case ExprKind::MapLit: {
        for (auto& entry : static_cast<MapLitExpr&>(*expr).entries) {
          walkExpr(entry.first.get());
          walkExpr(entry.second.get());
        }
        return;
      }
      case ExprKind::SetLit: {
        for (auto& element : static_cast<SetLitExpr&>(*expr).elements) {
          walkExpr(element.get());
        }
        return;
      }
      case ExprKind::Range: {
        auto& range = static_cast<RangeExpr&>(*expr);
        walkExpr(range.start.get());
        walkExpr(range.end.get());
        walkExpr(range.step.get());
        return;
      }
      case ExprKind::Ternary: {
        auto& ternary = static_cast<TernaryExpr&>(*expr);
        walkExpr(ternary.cond.get());
        walkExpr(ternary.thenValue.get());
        walkExpr(ternary.elseValue.get());
        return;
      }
      case ExprKind::Tuple: {
        for (auto& element : static_cast<TupleExpr&>(*expr).elements) {
          walkExpr(element.get());
        }
        return;
      }
      case ExprKind::Lambda: {
        auto& lambda = static_cast<LambdaExpr&>(*expr);
        walkExpr(lambda.body.get());
        return;
      }
      case ExprKind::Comprehension: {
        auto& comprehension = static_cast<ComprehensionExpr&>(*expr);
        walkExpr(comprehension.iterable.get());
        walkExpr(comprehension.element.get());
        walkExpr(comprehension.condition.get());
        return;
      }
      case ExprKind::Match: {
        auto& match = static_cast<MatchExpr&>(*expr);
        walkExpr(match.subject.get());
        for (auto& arm : match.arms) {
          walkExpr(arm.pattern.value.get());
          walkExpr(arm.pattern.rangeStart.get());
          walkExpr(arm.pattern.rangeEnd.get());
          walkExpr(arm.guard.get());
          walkExpr(arm.result.get());
        }
        return;
      }
      case ExprKind::Try:
        walkExpr(static_cast<TryExpr&>(*expr).operand.get());
        return;
      case ExprKind::StringLit: {
        for (auto& part : static_cast<StringLitExpr&>(*expr).parts) {
          walkExpr(part.expr.get());
        }
        return;
      }
      default:
        return;
    }
  }

  const Compilation& compilation_;
  std::vector<CallSite> sites_;
  std::vector<Callable> callables_;
  int moduleId_ = -1;
};

bool declNameSpanMatches(const Stmt& stmt, int line, int col) {
  Span span;
  switch (stmt.kind) {
    case StmtKind::FnDecl:
      span = static_cast<const FnDecl&>(stmt).nameSpan;
      break;
    case StmtKind::StructDecl:
      span = static_cast<const StructDecl&>(stmt).nameSpan;
      break;
    case StmtKind::EnumDecl:
      span = static_cast<const EnumDecl&>(stmt).nameSpan;
      break;
    case StmtKind::TraitDecl:
      span = static_cast<const TraitDecl&>(stmt).nameSpan;
      break;
    case StmtKind::TypeAlias:
      span = static_cast<const TypeAliasDecl&>(stmt).nameSpan;
      break;
    default:
      return false;
  }
  return span.line == line && span.col == col;
}

void childStmts(const Stmt& stmt, std::vector<const Stmt*>& out) {
  auto addBlock = [&](const std::vector<StmtPtr>& body) {
    for (const auto& child : body) {
      if (child) out.push_back(child.get());
    }
  };
  switch (stmt.kind) {
    case StmtKind::Defer:
      if (static_cast<const DeferStmt&>(stmt).body) {
        out.push_back(static_cast<const DeferStmt&>(stmt).body.get());
      }
      return;
    case StmtKind::If: {
      const auto& ifStmt = static_cast<const IfStmt&>(stmt);
      for (const auto& branch : ifStmt.branches) addBlock(branch.body);
      addBlock(ifStmt.elseBody);
      return;
    }
    case StmtKind::While:
      addBlock(static_cast<const WhileStmt&>(stmt).body);
      return;
    case StmtKind::DoWhile:
      addBlock(static_cast<const DoWhileStmt&>(stmt).body);
      return;
    case StmtKind::For:
      addBlock(static_cast<const ForStmt&>(stmt).body);
      return;
    case StmtKind::Loop:
      addBlock(static_cast<const LoopStmt&>(stmt).body);
      return;
    case StmtKind::FnDecl:
      addBlock(static_cast<const FnDecl&>(stmt).body);
      return;
    case StmtKind::StructDecl: {
      for (const auto& method : static_cast<const StructDecl&>(stmt).methods) {
        if (method) out.push_back(method.get());
      }
      return;
    }
    case StmtKind::ImplDecl: {
      for (const auto& method : static_cast<const ImplDecl&>(stmt).methods) {
        if (method) out.push_back(method.get());
      }
      return;
    }
    default:
      return;
  }
}

const Stmt* findDeclDeep(const Stmt& stmt, int line, int col) {
  if (declNameSpanMatches(stmt, line, col)) return &stmt;
  std::vector<const Stmt*> children;
  childStmts(stmt, children);
  for (const Stmt* child : children) {
    if (const Stmt* hit = findDeclDeep(*child, line, col)) return hit;
  }
  return nullptr;
}

}  // namespace

std::vector<CallSite> collectCallSites(const Compilation& compilation) {
  return CallSiteCollector(compilation).run();
}

std::vector<TypeDeclInfo> collectTypeDecls(const Compilation& compilation) {
  std::vector<TypeDeclInfo> decls;
  for (std::size_t i = 0; i < compilation.graph.modules.size(); ++i) {
    const Module& module = compilation.graph.modules[i];
    if (!module.program) continue;
    for (const auto& statement : module.program->statements) {
      if (!statement) continue;
      TypeDeclInfo info;
      info.moduleId = static_cast<int>(i);
      info.span = statement->span;
      if (statement->kind == StmtKind::StructDecl) {
        const auto& decl = static_cast<const StructDecl&>(*statement);
        info.name = decl.name;
        info.kind = SymbolKind::Class;
        info.detail = "class " + decl.name;
        info.nameSpan = decl.nameSpan;
        if (!decl.baseClass.empty()) info.parents.push_back(decl.baseClass);
      } else if (statement->kind == StmtKind::EnumDecl) {
        const auto& decl = static_cast<const EnumDecl&>(*statement);
        info.name = decl.name;
        info.kind = SymbolKind::Enum;
        info.detail = "enum " + decl.name;
        info.nameSpan = decl.nameSpan;
      } else if (statement->kind == StmtKind::TraitDecl) {
        const auto& decl = static_cast<const TraitDecl&>(*statement);
        info.name = decl.name;
        info.kind = SymbolKind::Interface;
        info.detail = "trait " + decl.name;
        info.nameSpan = decl.nameSpan;
        info.parents = decl.parents;
      } else {
        continue;
      }
      decls.push_back(std::move(info));
    }
  }
  return decls;
}

const Stmt* findDeclAt(const Compilation& compilation, const std::string& path, int line,
                       int col, int* moduleIdOut) {
  const std::string canonical = canonicalPath(path);
  for (std::size_t i = 0; i < compilation.graph.modules.size(); ++i) {
    const Module& module = compilation.graph.modules[i];
    if (canonicalPath(module.path) != canonical || !module.program) continue;
    for (const auto& statement : module.program->statements) {
      if (!statement) continue;
      if (const Stmt* hit = findDeclDeep(*statement, line, col)) {
        if (moduleIdOut) *moduleIdOut = static_cast<int>(i);
        return hit;
      }
    }
  }
  return nullptr;
}

std::vector<MemberInfo> nativeModuleMembers(const std::string& moduleName) {
  std::vector<MemberInfo> members;
  const NativeModuleSpec* spec = nativeModuleSpec(moduleName);
  if (!spec) return members;
  auto add = [&](const StringMap<std::string>& entries, bool isConstant) {
    for (const auto& entry : entries) {
      MemberInfo info;
      info.name = entry.first;
      info.isConstant = isConstant;
      if (const BuiltinDoc* doc = findBuiltinDoc(entry.second)) {
        info.signature = doc->signature;
        info.documentation = doc->documentation;
      }
      members.push_back(std::move(info));
    }
  };
  add(spec->functions, false);
  add(spec->constants, true);
  std::sort(members.begin(), members.end(),
            [](const MemberInfo& a, const MemberInfo& b) { return a.name < b.name; });
  return members;
}
}  // namespace a::lsp
