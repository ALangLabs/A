#include "analysis_internal.hpp"

namespace a::lsp {
class IndexBuilder {
 public:
  IndexBuilder(FileIndex& file, int moduleId) : file_(file), moduleId_(moduleId) {}

  void build(Program& program) {
    file_.scopes.push_back(ScopeInfo{-1, 0, kWholeFileSpan, {}});
    walkBlock(program.statements, 0);
  }

 private:
  int addScope(Span span, int parent, std::string implTarget = {}) {
    int depth = parent >= 0 ? file_.scopes[static_cast<std::size_t>(parent)].depth + 1 : 0;
    file_.scopes.push_back(ScopeInfo{parent, depth, span, std::move(implTarget)});
    return static_cast<int>(file_.scopes.size()) - 1;
  }

  int addSymbol(const std::string& name, Span span, SymbolKind kind, std::string detail,
                std::string typeName, int scope, bool hoisted, bool parameter = false) {
    if (name.empty()) return -1;
    SymbolDef symbol;
    symbol.name = name;
    symbol.span = span;
    symbol.kind = kind;
    symbol.detail = std::move(detail);
    symbol.typeName = std::move(typeName);
    symbol.scope = scope;
    symbol.hoisted = hoisted;
    symbol.topLevel = scope == 0;
    symbol.parameter = parameter;
    file_.symbols.push_back(std::move(symbol));
    return static_cast<int>(file_.symbols.size()) - 1;
  }

  // Registers a function's parameters in `fnScope`. `selfType`, when
  // non-empty, is stamped onto the `self` parameter so `self.field` and
  // `self.method()` resolve to the enclosing type's members.
  void addParams(const FnDecl& fn, int fnScope, const std::string& selfType) {
    for (const auto& param : fn.params) {
      std::string type = param.type ? typeExprText(param.type.get()) : std::string();
      if (param.name == "self" && type.empty() && !selfType.empty()) type = selfType;
      addSymbol(param.name, param.span, SymbolKind::Variable,
                varDetail(param.name, type, false, false), type, fnScope, true,
                /*parameter=*/true);
    }
  }

  bool visibleInScopeChain(int scope, const std::string& name) const {
    for (int current = scope; current >= 0;
         current = file_.scopes[static_cast<std::size_t>(current)].parent) {
      for (const SymbolDef& symbol : file_.symbols) {
        if (symbol.scope == current && symbol.name == name) return true;
      }
    }
    return false;
  }

  void walkBlock(const std::vector<StmtPtr>& body, int scope) {
    for (const auto& statement : body) {
      if (statement) walkStmt(*statement, scope);
    }
  }

  void walkBody(const std::vector<StmtPtr>& body, const Span& fallback, int parentScope) {
    int scope = addScope(blockSpan(body, fallback), parentScope);
    walkBlock(body, scope);
  }

  void walkStmt(Stmt& stmt, int scope) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        auto& decl = static_cast<LetStmt&>(stmt);
        std::string type =
            decl.type ? typeExprText(decl.type.get()) : exprTypeName(decl.init.get());
        addSymbol(decl.name, decl.nameSpan,
                  SymbolKind::Variable,
                  varDetail(decl.name, type, decl.mutableBinding, false), type,
                  scope, false);
        walkExpr(decl.init.get(), scope);
        return;
      }
      case StmtKind::Assign: {
        auto& assign = static_cast<AssignStmt&>(stmt);
        if (assign.target && assign.target->kind == ExprKind::Ident) {
          auto& ident = static_cast<IdentExpr&>(*assign.target);
          if (!visibleInScopeChain(scope, ident.name)) {
            std::string type = exprTypeName(assign.value.get());
            addSymbol(ident.name, ident.span, SymbolKind::Variable,
                      varDetail(ident.name, type, true, false), type, scope, false);
          }
        } else {
          walkExpr(assign.target.get(), scope);
        }
        walkExpr(assign.value.get(), scope);
        return;
      }
      case StmtKind::Destructure: {
        auto& decl = static_cast<DestructureStmt&>(stmt);
        for (std::size_t i = 0; i < decl.names.size(); ++i) {
          if (decl.names[i] == "_") continue;
          Span span = i < decl.nameSpans.size() ? decl.nameSpans[i] : decl.span;
          addSymbol(decl.names[i], span, SymbolKind::Variable,
                    varDetail(decl.names[i], {}, decl.mutableBinding, false), {}, scope, false);
        }
        walkExpr(decl.value.get(), scope);
        return;
      }
      case StmtKind::Expr:
        walkExpr(static_cast<ExprStmt&>(stmt).expr.get(), scope);
        return;
      case StmtKind::Return:
        walkExpr(static_cast<ReturnStmt&>(stmt).value.get(), scope);
        return;
      case StmtKind::Break:
      case StmtKind::Continue:
        return;
      case StmtKind::Defer: {
        auto& defer = static_cast<DeferStmt&>(stmt);
        if (defer.body) walkStmt(*defer.body, scope);
        return;
      }
      case StmtKind::If: {
        auto& ifStmt = static_cast<IfStmt&>(stmt);
        for (auto& branch : ifStmt.branches) {
          walkExpr(branch.cond.get(), scope);
          walkBody(branch.body, stmt.span, scope);
        }
        walkBody(ifStmt.elseBody, stmt.span, scope);
        return;
      }
      case StmtKind::While: {
        auto& whileStmt = static_cast<WhileStmt&>(stmt);
        walkExpr(whileStmt.cond.get(), scope);
        walkBody(whileStmt.body, stmt.span, scope);
        return;
      }
      case StmtKind::DoWhile: {
        auto& doStmt = static_cast<DoWhileStmt&>(stmt);
        walkBody(doStmt.body, stmt.span, scope);
        walkExpr(doStmt.cond.get(), scope);
        return;
      }
      case StmtKind::For: {
        auto& forStmt = static_cast<ForStmt&>(stmt);
        walkExpr(forStmt.iterable.get(), scope);
        int loopScope = addScope(stmt.span, scope);
        addSymbol(forStmt.var, forStmt.varSpan, SymbolKind::Variable,
                  varDetail(forStmt.var, {}, false, false), {}, loopScope, true);
        walkBlock(forStmt.body, loopScope);
        return;
      }
      case StmtKind::Loop: {
        auto& loop = static_cast<LoopStmt&>(stmt);
        walkBody(loop.body, stmt.span, scope);
        return;
      }
      case StmtKind::FnDecl: {
        auto& fn = static_cast<FnDecl&>(stmt);
        addSymbol(fn.name, fn.nameSpan, SymbolKind::Function, signatureText(fn), {}, scope, true);
        Span bodySpan = blockSpan(fn.body, stmt.span);
        int fnScope = addScope(bodySpan, scope);
        addParams(fn, fnScope, {});
        walkBlock(fn.body, fnScope);
        return;
      }
      case StmtKind::StructDecl: {
        auto& decl = static_cast<StructDecl&>(stmt);
        const SymbolKind kind = SymbolKind::Class;
        int structIndex =
            addSymbol(decl.name, decl.nameSpan, kind, "class " + decl.name,
                      decl.name, scope, true);
        int memberScope = addScope(stmt.span, scope);
        if (structIndex >= 0) file_.symbols[static_cast<std::size_t>(structIndex)].memberScope =
            memberScope;
        for (auto& field : decl.fields) {
          std::string type = field.type ? typeExprText(field.type.get()) : std::string();
          addSymbol(field.name, field.span, SymbolKind::Field,
                    varDetail(field.name, type, false, false), type, memberScope, true);
        }
        for (auto& method : decl.methods) {
          if (!method) continue;
          addSymbol(method->name, method->nameSpan, SymbolKind::Method, signatureText(*method),
                    {}, memberScope, true);
          Span bodySpan = blockSpan(method->body, method->span);
          int fnScope = addScope(bodySpan, memberScope);
          addParams(*method, fnScope, decl.name);
          walkBlock(method->body, fnScope);
        }
        return;
      }
      case StmtKind::EnumDecl: {
        auto& decl = static_cast<EnumDecl&>(stmt);
        int enumIndex =
            addSymbol(decl.name, decl.nameSpan, SymbolKind::Enum, "enum " + decl.name, decl.name,
                      scope, true);
        int memberScope = addScope(stmt.span, scope);
        if (enumIndex >= 0) file_.symbols[static_cast<std::size_t>(enumIndex)].memberScope =
            memberScope;
        for (auto& variant : decl.variants) {
          addSymbol(variant.name, variant.span, SymbolKind::EnumMember,
                    decl.name + "." + variant.name, decl.name, memberScope, true);
          walkExpr(variant.value.get(), scope);
        }
        return;
      }
      case StmtKind::TraitDecl: {
        auto& traitDecl = static_cast<TraitDecl&>(stmt);
        addSymbol(traitDecl.name, traitDecl.nameSpan, SymbolKind::Enum, "trait " + traitDecl.name, traitDecl.name, scope, true);
        for (const auto& sig : traitDecl.methods) {
          addSymbol(sig.name, sig.span, SymbolKind::Method,
                    traitDecl.name + "." + sig.name, sig.name, scope, true);
        }
        return;
      }
      case StmtKind::ImplDecl: {
        auto& impl = static_cast<ImplDecl&>(stmt);
        addSymbol(impl.targetName, impl.targetSpan, SymbolKind::Enum,
                  "impl " + impl.traitName + " for " + impl.targetName, impl.traitName,
                  scope, true);
        // Index impl methods like struct methods, but tag the member scope
        // with the implemented type so `x.method()` resolves through
        // findStructMember's impl-scope fallback.
        int memberScope = addScope(stmt.span, scope, impl.targetName);
        for (auto& method : impl.methods) {
          if (!method) continue;
          addSymbol(method->name, method->nameSpan, SymbolKind::Method, signatureText(*method),
                    {}, memberScope, true);
          Span bodySpan = blockSpan(method->body, method->span);
          int fnScope = addScope(bodySpan, memberScope);
          addParams(*method, fnScope, impl.targetName);
          walkBlock(method->body, fnScope);
        }
        return;
      }
      case StmtKind::TypeAlias: {
        auto& alias = static_cast<TypeAliasDecl&>(stmt);
        addSymbol(alias.name, alias.nameSpan, SymbolKind::TypeParameter, "type " + alias.name, "", scope, true);
        return;
      }
      case StmtKind::Import: {
        auto& import = static_cast<ImportStmt&>(stmt);
        ImportDef def;
        def.alias = importAlias(import.alias, import.path, import.bareName);
        def.modulePath = !import.stdModule.empty() ? import.stdModule
                            : import.path.empty()    ? import.bareName
                                                     : import.path;
        def.moduleId = import.moduleId;
        def.wholeModule = true;
        def.span = import.targetSpan;
        file_.imports.push_back(std::move(def));
        return;
      }
      case StmtKind::FromImport: {
        auto& import = static_cast<FromImportStmt&>(stmt);
        std::string path = !import.stdModule.empty() ? import.stdModule
                              : import.path.empty()    ? import.bareName
                                                       : import.path;
        for (const auto& item : import.items) {
          ImportDef def;
          def.alias = item.alias;
          def.modulePath = path;
          def.moduleId = import.moduleId;
          def.member = item.name;
          def.wholeModule = false;
          def.span = item.span;
          def.moduleSpan = import.targetSpan;
          file_.imports.push_back(std::move(def));
        }
        return;
      }
    }
  }

  void walkExpr(Expr* expr, int scope) {
    if (!expr) return;
    switch (expr->kind) {
      case ExprKind::Unary:
        walkExpr(static_cast<UnaryExpr&>(*expr).operand.get(), scope);
        return;
      case ExprKind::Binary: {
        auto& binary = static_cast<BinaryExpr&>(*expr);
        walkExpr(binary.lhs.get(), scope);
        walkExpr(binary.rhs.get(), scope);
        return;
      }
      case ExprKind::Call: {
        auto& call = static_cast<CallExpr&>(*expr);
        walkExpr(call.callee.get(), scope);
        for (auto& arg : call.args) walkExpr(arg.value.get(), scope);
        return;
      }
      case ExprKind::Index: {
        auto& index = static_cast<IndexExpr&>(*expr);
        walkExpr(index.object.get(), scope);
        walkExpr(index.index.get(), scope);
        return;
      }
      case ExprKind::Slice: {
        auto& slice = static_cast<SliceExpr&>(*expr);
        walkExpr(slice.object.get(), scope);
        walkExpr(slice.start.get(), scope);
        walkExpr(slice.end.get(), scope);
        walkExpr(slice.step.get(), scope);
        return;
      }
      case ExprKind::Field:
        walkExpr(static_cast<FieldExpr&>(*expr).object.get(), scope);
        return;
      case ExprKind::SafeNav:
        walkExpr(static_cast<SafeNavExpr&>(*expr).object.get(), scope);
        return;
      case ExprKind::ArrayLit: {
        for (auto& element : static_cast<ArrayLitExpr&>(*expr).elements) {
          walkExpr(element.get(), scope);
        }
        return;
      }
      case ExprKind::Range: {
        auto& range = static_cast<RangeExpr&>(*expr);
        walkExpr(range.start.get(), scope);
        walkExpr(range.end.get(), scope);
        walkExpr(range.step.get(), scope);
        return;
      }
      case ExprKind::Ternary: {
        auto& ternary = static_cast<TernaryExpr&>(*expr);
        walkExpr(ternary.cond.get(), scope);
        walkExpr(ternary.thenValue.get(), scope);
        walkExpr(ternary.elseValue.get(), scope);
        return;
      }
      case ExprKind::Tuple: {
        for (auto& element : static_cast<TupleExpr&>(*expr).elements) {
          walkExpr(element.get(), scope);
        }
        return;
      }
      case ExprKind::Lambda: {
        auto& lambda = static_cast<LambdaExpr&>(*expr);
        int lambdaScope = addScope(lambda.span, scope);
        for (const auto& param : lambda.params) {
          std::string type = param.type ? typeExprText(param.type.get()) : std::string();
          addSymbol(param.name, param.span, SymbolKind::Variable,
                    varDetail(param.name, type, false, false), type, lambdaScope, true,
                    /*parameter=*/true);
        }
        walkExpr(lambda.body.get(), lambdaScope);
        return;
      }
      case ExprKind::Comprehension: {
        auto& comprehension = static_cast<ComprehensionExpr&>(*expr);
        walkExpr(comprehension.iterable.get(), scope);
        int compScope = addScope(comprehension.span, scope);
        addSymbol(comprehension.var, comprehension.varSpan, SymbolKind::Variable,
                  varDetail(comprehension.var, {}, false, false), {}, compScope, true);
        walkExpr(comprehension.element.get(), compScope);
        walkExpr(comprehension.condition.get(), compScope);
        return;
      }
      case ExprKind::Match: {
        auto& match = static_cast<MatchExpr&>(*expr);
        walkExpr(match.subject.get(), scope);
        for (auto& arm : match.arms) {
          walkExpr(arm.pattern.value.get(), scope);
          walkExpr(arm.pattern.rangeStart.get(), scope);
          walkExpr(arm.pattern.rangeEnd.get(), scope);
          walkExpr(arm.guard.get(), scope);
          walkExpr(arm.result.get(), scope);
        }
        return;
      }
      case ExprKind::StringLit: {
        for (auto& part : static_cast<StringLitExpr&>(*expr).parts) {
          walkExpr(part.expr.get(), scope);
        }
        return;
      }
      default:
        return;
    }
  }

  FileIndex& file_;
  int moduleId_ = -1;
};

std::unique_ptr<Compilation> analyzeDocument(const std::string& path, const std::string& text,
                                             const std::vector<std::string>& modulePaths,
                                             const std::string& stdlibRoot) {
  auto compilation = std::make_unique<Compilation>();
  compilation->path = path;
  compilation->text = text;

  ModuleLoaderOptions options;
  options.modulePaths = modulePaths;
  options.stdlibRoot = stdlibRoot.empty() ? defaultStdlibRoot() : stdlibRoot;

  ModuleLoader loader(std::move(options), compilation->sink);
  compilation->graph = loader.load(path, text);

  const std::string canonicalEntry = canonicalPath(path);
  compilation->indexes.resize(compilation->graph.modules.size());
  compilation->tokensByModule.resize(compilation->graph.modules.size());
  compilation->commentsByModule.resize(compilation->graph.modules.size());
  compilation->textsByModule.resize(compilation->graph.modules.size());

  for (std::size_t i = 0; i < compilation->graph.modules.size(); ++i) {
    Module& module = compilation->graph.modules[i];
    std::string source =
        canonicalPath(module.path) == canonicalEntry ? text : readFileText(module.path);
    compilation->textsByModule[i] = source;

    DiagnosticSink scratch;
    Lexer lexer(source, scratch, static_cast<int>(i));
    compilation->tokensByModule[i] = lexer.tokenize();
    compilation->commentsByModule[i] = lexer.comments();

    FileIndex& index = compilation->indexes[i];
    index.path = module.path;
    IndexBuilder builder(index, module.id);
    if (module.program) builder.build(*module.program);
  }

  if (!compilation->sink.hasErrors()) {
    Sema sema(compilation->graph, compilation->sink);
    // Multi-module projects (and the async background worker) benefit from
    // parallel body checking; single-file edits stay serial.
    if (compilation->graph.modules.size() > 1) {
      compilation->sema = sema.analyzeParallel(0);
    } else {
      compilation->sema = sema.analyze();
    }
    compilation->analyzed = !compilation->sink.hasErrors();
  }

  return compilation;
}
}  // namespace a::lsp
