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
    const std::string documentation =
        docCommentText(compilation, *resolved.file, resolved.symbol->span);
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
    case a::StmtKind::TraitDecl: {
      const auto& decl = static_cast<const a::TraitDecl&>(statement);
      symbol["name"] = decl.name;
      symbol["detail"] = "trait";
      symbol["kind"] = static_cast<int>(SymbolKind::Enum);
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
    default:
      return Value();
  }
  (void)compilation;
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
  for (const auto& statement : module.program->statements) {
    if (!statement) continue;
    Value symbol = documentSymbolFromStatement(compilation, document->text, *statement);
    if (!symbol.isNull()) result.push(std::move(symbol));
  }
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

  Value result = Value::array();
  std::unordered_set<std::string> seen;
  for (const auto& entry : documents_) {
    const Document& document = entry.second;
    if (!document.compilation) continue;
    const FileIndex* file = document.compilation->entryIndex();
    if (!file) continue;
    for (const SymbolDef& symbol : file->symbols) {
      if (!symbol.topLevel) continue;
      std::string nameLower = symbol.name;
      for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (!lowered.empty() && nameLower.find(lowered) == std::string::npos) continue;
      std::string key = file->path + ":" + std::to_string(symbol.span.line) + ":" +
                        std::to_string(symbol.span.col);
      if (!seen.insert(key).second) continue;
      Value item = Value::object();
      item["name"] = symbol.name;
      item["kind"] = static_cast<int>(symbol.kind);
      item["location"] =
          locationValue(*document.compilation, file->path, symbol.span);
      result.push(std::move(item));
    }
  }
  reply(id, std::move(result));
}


}  // namespace a::lsp
