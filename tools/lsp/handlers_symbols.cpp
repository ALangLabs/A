#include "handlers_common.hpp"

void LanguageServer::hover(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

  ResolvedSymbol resolved = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (resolved.valid()) {
    std::string description = a::lsp::describeSymbol(*resolved.symbol);
    // `impl Trait for Type` conformance shows up as a trailing comment,
    // like Rust's "impl Trait for Type" hover notes.
    if (!resolved.symbol->implementedTrait.empty()) {
      description += "\n// implements " + resolved.symbol->implementedTrait;
    }
    // Show the inferred type the way ty/pyrefly do (`x: i32`) when the
    // declaration has no explicit annotation.
    std::string inferred;
    for (const auto& var : compilation.sema.variables) {
      if (var.span.line == resolved.symbol->span.line &&
          var.span.col == resolved.symbol->span.col &&
          var.span.fileId == resolved.symbol->span.fileId && var.type) {
        std::string t = a::typeName(var.type);
        if (!t.empty() && t != "error" && description.find(": " + t) == std::string::npos) {
          bool annotated = resolved.symbol->detail.find(": ") != std::string::npos;
          if (!annotated || description.find(":") > description.find(t)) {
            description = resolved.symbol->name + ": " + t + "  (inferred)";
            if (!resolved.symbol->detail.empty() && resolved.symbol->detail != resolved.symbol->name)
              description += "\n" + resolved.symbol->detail;
          }
        }
        break;
      }
    }
    std::string markdown = "```a\n" + formatSignature(description) + "\n```";
    std::string documentation =
        docCommentText(compilation, *resolved.file, resolved.symbol->span);
    if (documentation.empty()) {
      documentation = a::lsp::synthesizeSymbolDoc(compilation, resolved);
    }
    const std::string body = renderDocSections(documentation);
    if (!body.empty()) markdown += "\n\n" + body;
    // Definition link, like pyrefly hover.
    if (resolved.file->path != file->path || resolved.symbol->span.line != line + 1) {
      markdown += "\n\n[Definition](file://" + resolved.file->path + "#L" +
                  std::to_string(resolved.symbol->span.line) + ")";
    }
    const std::string* text = compilation.textForPath(resolved.file->path);
    Value result = Value::object();
    Value contents = Value::object();
    contents["kind"] = "markdown";
    contents["value"] = markdown;
    result["contents"] = std::move(contents);
    if (text) result["range"] = rangeValue(*text, resolved.symbol->span);
    reply(id, std::move(result));
    return;
  }

  const Token* token = tokenAt(*tokens, line + 1, byteColumn);
  if (token && token->kind == Tok::Ident) {
    int index = tokenIndexAt(*tokens, line + 1, byteColumn);

    // `module.member` and primitive `receiver.method` forms.
    if (index >= 2) {
      const Token& dot = (*tokens)[static_cast<std::size_t>(index - 1)];
      const Token& baseToken = (*tokens)[static_cast<std::size_t>(index - 2)];
      if ((dot.kind == Tok::Dot || dot.kind == Tok::QuestionDot) &&
          baseToken.kind == Tok::Ident) {
        const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
        if (!moduleName.empty()) {
          const std::string* builtinName = a::nativeFunction(moduleName, token->lexeme);
          if (!builtinName) builtinName = a::nativeConstant(moduleName, token->lexeme);
          if (builtinName) {
            if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) {
              reply(id, hoverMarkdown(doc->signature, doc->documentation,
                                      doc->example ? doc->example : "",
                                      doc->seeAlso ? doc->seeAlso : ""));
              return;
            }
          }
        }
        ResolvedSymbol base =
            a::lsp::resolveToken(compilation, *file, *tokens, index - 2);
        if (base.valid()) {
          if (const std::vector<a::MethodSig>* table =
                  methodTableForType(base.symbol->typeName)) {
            if (const a::MethodSig* method = findMethodSig(*table, token->lexeme)) {
              reply(id, hoverMarkdown(method->signature, method->documentation,
                                      method->example ? method->example : ""));
              return;
            }
          }
        }
      }
    }

    if (!nativeModuleForAlias(*file, token->lexeme).empty()) {
      const std::string moduleName = nativeModuleForAlias(*file, token->lexeme);
      reply(id, hoverMarkdown("import " + moduleName,
                              "Native standard-library module."));
      return;
    }
    // Whole-module import alias (`import utils` / `import utils as u`):
    // hovering the alias describes the module like Pyright's import hover.
    for (const a::lsp::ImportDef& import : file->imports) {
      if (!import.wholeModule || import.alias != token->lexeme) continue;
      std::string markdown = "```a\nimport " + import.alias;
      if (!import.modulePath.empty() && import.modulePath != import.alias) {
        markdown += "  // " + import.modulePath;
      }
      markdown += "\n```";
      Value contents = Value::object();
      contents["kind"] = "markdown";
      contents["value"] = markdown;
      Value result = Value::object();
      result["contents"] = std::move(contents);
      reply(id, std::move(result));
      return;
    }
    if (const a::BuiltinDoc* doc = a::findBuiltinDoc(token->lexeme)) {
      reply(id, hoverMarkdown(doc->signature, doc->documentation,
                              doc->example ? doc->example : "",
                              doc->seeAlso ? doc->seeAlso : ""));
      return;
    }
  }
  replyNull(id);
}

Value LanguageServer::documentSymbolFromStatement(const Compilation& compilation, const std::string& text,
                                  const a::Stmt& statement) {
  Value symbol = Value::object();
  switch (statement.kind) {
    case a::StmtKind::FnDecl: {
      const auto& fn = static_cast<const a::FnDecl&>(statement);
      symbol["name"] = fn.name;
      symbol["detail"] = a::lsp::signatureText(fn);
      symbol["kind"] = static_cast<int>(SymbolKind::Function);
      symbol["range"] = symbolRange(text, fn.span);
      symbol["selectionRange"] = symbolRange(text, fn.nameSpan);
      return symbol;
    }
    case a::StmtKind::StructDecl: {
      const auto& decl = static_cast<const a::StructDecl&>(statement);
      symbol["name"] = decl.name;
      symbol["detail"] = "class";
      symbol["kind"] = static_cast<int>(SymbolKind::Class);
      symbol["range"] = symbolRange(text, decl.span);
      symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
      Value children = Value::array();
      for (const auto& field : decl.fields) {
        Value child = Value::object();
        child["name"] = field.name;
        child["detail"] = a::lsp::typeExprText(field.type.get());
        child["kind"] = static_cast<int>(SymbolKind::Field);
        child["range"] = symbolRange(text, field.span);
        child["selectionRange"] = symbolRange(text, field.span);
        children.push(std::move(child));
      }
      for (const auto& method : decl.methods) {
        if (!method) continue;
        Value child = Value::object();
        child["name"] = method->name;
        child["detail"] = a::lsp::signatureText(*method);
        child["kind"] = static_cast<int>(SymbolKind::Method);
        child["range"] = symbolRange(text, method->span);
        child["selectionRange"] = symbolRange(text, method->nameSpan);
        Value methodChildren = Value::array();
        outlineBlockSymbols(compilation, text, method->body, methodChildren);
        if (!methodChildren.asArray().empty()) {
          child["children"] = std::move(methodChildren);
        }
        children.push(std::move(child));
      }
      symbol["children"] = std::move(children);
      return symbol;
    }
    case a::StmtKind::EnumDecl: {
      const auto& decl = static_cast<const a::EnumDecl&>(statement);
      symbol["name"] = decl.name;
      symbol["detail"] = "enum";
      symbol["kind"] = static_cast<int>(SymbolKind::Enum);
      symbol["range"] = symbolRange(text, decl.span);
      symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
      Value children = Value::array();
      for (const auto& variant : decl.variants) {
        Value child = Value::object();
        child["name"] = variant.name;
        child["detail"] = decl.name + "." + variant.name;
        child["kind"] = static_cast<int>(SymbolKind::EnumMember);
        child["range"] = symbolRange(text, variant.span);
        child["selectionRange"] = symbolRange(text, variant.span);
        children.push(std::move(child));
      }
      symbol["children"] = std::move(children);
      return symbol;
    }
    case a::StmtKind::Let: {
      const auto& decl = static_cast<const a::LetStmt&>(statement);
      symbol["name"] = decl.name;
      symbol["detail"] = a::lsp::typeExprText(decl.type.get());
      symbol["kind"] = static_cast<int>(SymbolKind::Variable);
      symbol["range"] = symbolRange(text, decl.span);
      symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
      return symbol;
    }
    case a::StmtKind::Assign: {
      const auto& assign = static_cast<const a::AssignStmt&>(statement);
      if (!assign.target || assign.target->kind != a::ExprKind::Ident) return Value();
      const auto& ident = static_cast<const a::IdentExpr&>(*assign.target);
      symbol["name"] = ident.name;
      symbol["kind"] = static_cast<int>(SymbolKind::Variable);
      symbol["range"] = symbolRange(text, assign.span);
      symbol["selectionRange"] = symbolRange(text, ident.span);
      return symbol;
    }
    case a::StmtKind::Destructure: {
      const auto& decl = static_cast<const a::DestructureStmt&>(statement);
      if (decl.names.size() != 1) return Value();
      symbol["name"] = decl.names.front();
      symbol["kind"] = static_cast<int>(SymbolKind::Variable);
      symbol["range"] = symbolRange(text, decl.span);
      symbol["selectionRange"] =
          symbolRange(text, decl.nameSpans.empty() ? decl.span : decl.nameSpans.front());
      return symbol;
    }
    case a::StmtKind::For: {
      const auto& stmt = static_cast<const a::ForStmt&>(statement);
      symbol["name"] = stmt.var;
      symbol["detail"] = "for";
      symbol["kind"] = static_cast<int>(SymbolKind::Variable);
      symbol["range"] = symbolRange(text, stmt.span);
      symbol["selectionRange"] = symbolRange(text, stmt.varSpan);
      return symbol;
    }
    case a::StmtKind::TraitDecl: {
      const auto& decl = static_cast<const a::TraitDecl&>(statement);
      symbol["name"] = decl.name;
      symbol["detail"] = "trait";
      symbol["kind"] = static_cast<int>(SymbolKind::Interface);
      symbol["range"] = symbolRange(text, decl.span);
      symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
      Value children = Value::array();
      for (const auto& method : decl.methods) {
        Value child = Value::object();
        child["name"] = method.name;
        child["detail"] = decl.name + "." + method.name;
        child["kind"] = static_cast<int>(SymbolKind::Method);
        child["range"] = symbolRange(text, method.span);
        child["selectionRange"] = symbolRange(text, method.span);
        children.push(std::move(child));
      }
      symbol["children"] = std::move(children);
      return symbol;
    }
    case a::StmtKind::ImplDecl: {
      const auto& impl = static_cast<const a::ImplDecl&>(statement);
      symbol["name"] = "impl " + impl.traitName + " for " + impl.targetName;
      symbol["detail"] = "impl";
      symbol["kind"] = static_cast<int>(SymbolKind::Interface);
      symbol["range"] = symbolRange(text, impl.span);
      symbol["selectionRange"] = symbolRange(text, impl.traitSpan);
      Value children = Value::array();
      for (const auto& method : impl.methods) {
        if (!method) continue;
        Value child = Value::object();
        child["name"] = method->name;
        child["detail"] = a::lsp::signatureText(*method);
        child["kind"] = static_cast<int>(SymbolKind::Method);
        child["range"] = symbolRange(text, method->span);
        child["selectionRange"] = symbolRange(text, method->nameSpan);
        children.push(std::move(child));
      }
      if (!children.asArray().empty()) symbol["children"] = std::move(children);
      return symbol;
    }
    default:
      return Value();
  }
  (void)compilation;
}

// Emits an outline symbol for every declaration-bearing statement in `body`,
// recursing into function/method bodies and nested blocks so the outline is
// hierarchical like Pyright's (locals and nested functions appear as
// children of their enclosing function).
void LanguageServer::outlineBlockSymbols(const Compilation& compilation, const std::string& text,
                                         const std::vector<a::StmtPtr>& body, Value& out) {
  for (const auto& statement : body) {
    if (statement) outlineStmtSymbol(compilation, text, *statement, out);
  }
}

// Emits outline symbols for comprehension variables inside `expr`
// (`[n * n for n in nums]` binds `n`), recursing through every expression
// form. Each variable becomes a Variable child of the enclosing symbol.
void LanguageServer::outlineExprSymbols(const Compilation& compilation, const std::string& text,
                                        const a::Expr* expr, Value& out) {
  if (!expr) return;
  auto sub = [&](const a::Expr* e) { outlineExprSymbols(compilation, text, e, out); };
  switch (expr->kind) {
    case a::ExprKind::Comprehension: {
      const auto& comp = static_cast<const a::ComprehensionExpr&>(*expr);
      Value sym = Value::object();
      sym["name"] = comp.var;
      sym["detail"] = "for";
      sym["kind"] = static_cast<int>(SymbolKind::Variable);
      sym["range"] = symbolRange(text, comp.span);
      sym["selectionRange"] = symbolRange(text, comp.varSpan);
      Value children = Value::array();
      sub(comp.iterable.get());
      sub(comp.element.get());
      sub(comp.condition.get());
      if (!children.asArray().empty()) sym["children"] = std::move(children);
      out.push(std::move(sym));
      return;
    }
    case a::ExprKind::Unary:
      sub(static_cast<const a::UnaryExpr&>(*expr).operand.get());
      return;
    case a::ExprKind::Binary: {
      const auto& e = static_cast<const a::BinaryExpr&>(*expr);
      sub(e.lhs.get());
      sub(e.rhs.get());
      return;
    }
    case a::ExprKind::Call: {
      const auto& e = static_cast<const a::CallExpr&>(*expr);
      sub(e.callee.get());
      for (const auto& arg : e.args) sub(arg.value.get());
      return;
    }
    case a::ExprKind::Index: {
      const auto& e = static_cast<const a::IndexExpr&>(*expr);
      sub(e.object.get());
      sub(e.index.get());
      return;
    }
    case a::ExprKind::Slice: {
      const auto& e = static_cast<const a::SliceExpr&>(*expr);
      sub(e.object.get());
      sub(e.start.get());
      sub(e.end.get());
      sub(e.step.get());
      return;
    }
    case a::ExprKind::Field:
      sub(static_cast<const a::FieldExpr&>(*expr).object.get());
      return;
    case a::ExprKind::SafeNav:
      sub(static_cast<const a::SafeNavExpr&>(*expr).object.get());
      return;
    case a::ExprKind::ArrayLit:
      for (const auto& el : static_cast<const a::ArrayLitExpr&>(*expr).elements) sub(el.get());
      return;
    case a::ExprKind::MapLit:
      for (const auto& entry : static_cast<const a::MapLitExpr&>(*expr).entries) {
        sub(entry.first.get());
        sub(entry.second.get());
      }
      return;
    case a::ExprKind::SetLit:
      for (const auto& el : static_cast<const a::SetLitExpr&>(*expr).elements) sub(el.get());
      return;
    case a::ExprKind::Range: {
      const auto& e = static_cast<const a::RangeExpr&>(*expr);
      sub(e.start.get());
      sub(e.end.get());
      sub(e.step.get());
      return;
    }
    case a::ExprKind::Ternary: {
      const auto& e = static_cast<const a::TernaryExpr&>(*expr);
      sub(e.cond.get());
      sub(e.thenValue.get());
      sub(e.elseValue.get());
      return;
    }
    case a::ExprKind::Tuple:
      for (const auto& el : static_cast<const a::TupleExpr&>(*expr).elements) sub(el.get());
      return;
    case a::ExprKind::Lambda:
      sub(static_cast<const a::LambdaExpr&>(*expr).body.get());
      return;
    case a::ExprKind::Match: {
      const auto& e = static_cast<const a::MatchExpr&>(*expr);
      sub(e.subject.get());
      for (const auto& arm : e.arms) {
        sub(arm.pattern.value.get());
        sub(arm.pattern.rangeStart.get());
        sub(arm.pattern.rangeEnd.get());
        sub(arm.guard.get());
        sub(arm.result.get());
      }
      return;
    }
    case a::ExprKind::Try:
      sub(static_cast<const a::TryExpr&>(*expr).operand.get());
      return;
    case a::ExprKind::StringLit:
      for (const auto& part : static_cast<const a::StringLitExpr&>(*expr).parts) {
        sub(part.expr.get());
      }
      return;
    default:
      return;
  }
}

void LanguageServer::outlineStmtSymbol(const Compilation& compilation, const std::string& text,
                                       const a::Stmt& stmt, Value& out) {
  Value symbol = documentSymbolFromStatement(compilation, text, stmt);
  // Comprehension variables in the statement's own expressions become
  // children of its symbol (or siblings when the statement has none).
  Value exprChildren = Value::array();
  auto scan = [&](const a::Expr* e) {
    if (e) outlineExprSymbols(compilation, text, e, exprChildren);
  };
  switch (stmt.kind) {
    case a::StmtKind::Let:
      scan(static_cast<const a::LetStmt&>(stmt).init.get());
      break;
    case a::StmtKind::Assign: {
      const auto& assign = static_cast<const a::AssignStmt&>(stmt);
      scan(assign.target.get());
      scan(assign.value.get());
      break;
    }
    case a::StmtKind::Destructure:
      scan(static_cast<const a::DestructureStmt&>(stmt).value.get());
      break;
    case a::StmtKind::Expr:
      scan(static_cast<const a::ExprStmt&>(stmt).expr.get());
      break;
    case a::StmtKind::Return:
      scan(static_cast<const a::ReturnStmt&>(stmt).value.get());
      break;
    case a::StmtKind::If: {
      const auto& ifStmt = static_cast<const a::IfStmt&>(stmt);
      for (const auto& branch : ifStmt.branches) scan(branch.cond.get());
      break;
    }
    case a::StmtKind::While:
      scan(static_cast<const a::WhileStmt&>(stmt).cond.get());
      break;
    case a::StmtKind::DoWhile:
      scan(static_cast<const a::DoWhileStmt&>(stmt).cond.get());
      break;
    case a::StmtKind::For:
      scan(static_cast<const a::ForStmt&>(stmt).iterable.get());
      break;
    default:
      break;
  }
  if (stmt.kind == a::StmtKind::FnDecl) {
    const auto& fn = static_cast<const a::FnDecl&>(stmt);
    Value children = Value::array();
    outlineBlockSymbols(compilation, text, fn.body, children);
    for (auto& child : exprChildren.asArray()) children.push(std::move(child));
    if (!children.asArray().empty() && !symbol.isNull()) {
      symbol["children"] = std::move(children);
    }
  } else if (stmt.kind == a::StmtKind::For) {
    const auto& forStmt = static_cast<const a::ForStmt&>(stmt);
    Value children = std::move(exprChildren);
    outlineBlockSymbols(compilation, text, forStmt.body, children);
    if (!children.asArray().empty() && !symbol.isNull()) {
      symbol["children"] = std::move(children);
    }
  } else if (!exprChildren.asArray().empty()) {
    if (!symbol.isNull()) {
      symbol["children"] = std::move(exprChildren);
    } else {
      for (auto& child : exprChildren.asArray()) out.push(std::move(child));
    }
  }
  if (!symbol.isNull()) {
    out.push(std::move(symbol));
    return;
  }
  // Compound statements produce no symbol themselves; their inner
  // declarations surface as siblings at this level.
  auto sub = [&](const std::vector<a::StmtPtr>& body) {
    outlineBlockSymbols(compilation, text, body, out);
  };
  switch (stmt.kind) {
    case a::StmtKind::If: {
      const auto& ifStmt = static_cast<const a::IfStmt&>(stmt);
      for (const auto& branch : ifStmt.branches) sub(branch.body);
      sub(ifStmt.elseBody);
      return;
    }
    case a::StmtKind::While:
      sub(static_cast<const a::WhileStmt&>(stmt).body);
      return;
    case a::StmtKind::DoWhile:
      sub(static_cast<const a::DoWhileStmt&>(stmt).body);
      return;
    case a::StmtKind::Loop:
      sub(static_cast<const a::LoopStmt&>(stmt).body);
      return;
    case a::StmtKind::Defer:
      if (static_cast<const a::DeferStmt&>(stmt).body) {
        outlineStmtSymbol(compilation, text, *static_cast<const a::DeferStmt&>(stmt).body, out);
      }
      return;
    default:
      return;
  }
}

void LanguageServer::documentSymbols(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
  if (entry < 0 || static_cast<std::size_t>(entry) >= compilation.graph.modules.size()) {
    return replyNull(id);
  }
  const a::Module& module = compilation.graph.modules[static_cast<std::size_t>(entry)];
  if (!module.program) return replyNull(id);

  Value result = Value::array();
  outlineBlockSymbols(compilation, document->text, module.program->statements, result);
  reply(id, std::move(result));
}

void LanguageServer::workspaceSymbols(const Value& id, const Value* params) {
  std::string query;
  if (params) {
    const Value* queryValue = params->find("query");
    if (queryValue && queryValue->isString()) query = queryValue->asString();
  }
  std::string lowered = query;
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  struct Hit {
    int score = 2;
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    std::string container;
    Value location;
  };
  std::vector<Hit> hits;
  std::unordered_set<std::string> seen;
  for (const auto& entry : documents_) {
    const Document& document = entry.second;
    if (!document.compilation) continue;
    const FileIndex* file = document.compilation->entryIndex();
    if (!file) continue;
    // Container name for a symbol: the nearest enclosing callable's body
    // scope (functions/methods store it in memberScope), a type/enum member
    // scope, or an impl block's target type — found by walking the scope
    // chain outward from the symbol's own scope.
    auto containerFor = [&](const SymbolDef& sym) -> std::string {
      for (int scopeIdx = sym.scope; scopeIdx >= 0;
           scopeIdx = file->scopes[static_cast<std::size_t>(scopeIdx)].parent) {
        for (const SymbolDef& owner : file->symbols) {
          if (owner.memberScope >= 0 && owner.memberScope == scopeIdx) {
            return owner.name;
          }
        }
        if (!file->scopes[static_cast<std::size_t>(scopeIdx)].implTarget.empty()) {
          return file->scopes[static_cast<std::size_t>(scopeIdx)].implTarget;
        }
      }
      return {};
    };
    for (const SymbolDef& symbol : file->symbols) {
      // Top-level declarations, type/enum members (fields, methods,
      // variants), and locals: variables, parameters, and loop/comprehension
      // bindings scoped to their enclosing function or block.
      const bool member = symbol.memberScope >= 0 ||
                          symbol.kind == SymbolKind::Method ||
                          symbol.kind == SymbolKind::EnumMember ||
                          symbol.kind == SymbolKind::Field;
      if (!symbol.topLevel && !member && symbol.kind != SymbolKind::Variable) continue;
      std::string nameLower = symbol.name;
      for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      int score = 2;
      if (lowered.empty()) {
        score = 0;
      } else if (nameLower == lowered) {
        score = 0;
      } else if (nameLower.rfind(lowered, 0) == 0) {
        score = 1;
      } else if (nameLower.find(lowered) == std::string::npos) {
        continue;
      }
      std::string key = file->path + ":" + std::to_string(symbol.span.line) + ":" +
                        std::to_string(symbol.span.col);
      if (!seen.insert(key).second) continue;
      Hit hit;
      hit.score = score;
      hit.name = symbol.name;
      hit.kind = symbol.kind;
      hit.container = containerFor(symbol);
      hit.location = locationValue(*document.compilation, file->path, symbol.span);
      hits.push_back(std::move(hit));
    }
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.score != b.score) return a.score < b.score;
    return a.name < b.name;
  });
  Value result = Value::array();
  for (const Hit& hit : hits) {
    Value item = Value::object();
    item["name"] = hit.name;
    item["kind"] = static_cast<int>(hit.kind);
    if (!hit.container.empty()) item["containerName"] = hit.container;
    item["location"] = hit.location;
    result.push(std::move(item));
  }
  reply(id, std::move(result));
}


}  // namespace a::lsp
