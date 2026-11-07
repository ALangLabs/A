#include "handlers_common.hpp"

void LanguageServer::codeLens(const Value& id, const Value* params) {
  Value result = Value::array();
  Document* document = documentFrom(params);
  if (!document || !document->compilation) {
    reply(id, std::move(result));
    return;
  }
  const Compilation& compilation = *document->compilation;
  const FileIndex* entry = compilation.entryIndex();
  if (!entry) {
    reply(id, std::move(result));
    return;
  }

  // Count resolved occurrences of every symbol across all modules, the same
  // scan references() performs, grouped by declaration site.
  std::unordered_map<std::string, int> refCounts;
  for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
    if (moduleId >= compilation.tokensByModule.size()) continue;
    const FileIndex& file = compilation.indexes[moduleId];
    const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
      const Token& token = tokens[static_cast<std::size_t>(i)];
      if (token.kind != Tok::Ident) continue;
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
      if (!resolved.valid()) continue;
      const std::string key = canonicalPath(resolved.file->path) + "|" +
                              std::to_string(resolved.symbol->span.line) + ":" +
                              std::to_string(resolved.symbol->span.col);
      ++refCounts[key];
    }
  }

  const std::string entryPath = canonicalPath(entry->path);
  for (const SymbolDef& symbol : entry->symbols) {
    const bool interesting = symbol.kind == SymbolKind::Function ||
                             symbol.kind == SymbolKind::Method ||
                             symbol.kind == SymbolKind::Struct ||
                             symbol.kind == SymbolKind::Class ||
                             symbol.kind == SymbolKind::Enum;
    if (!interesting) continue;
    const std::string key = entryPath + "|" + std::to_string(symbol.span.line) + ":" +
                            std::to_string(symbol.span.col);
    const int usages = std::max(0, refCounts[key] - 1);  // minus the declaration
    if (usages <= 0) continue;
    std::string lt = lineText(document->text, symbol.span.line);
    Value lens = Value::object();
    Value range = Value::object();
    Value start = Value::object();
    start["line"] = symbol.span.line - 1;
    start["character"] = 0;
    Value end = Value::object();
    end["line"] = symbol.span.line - 1;
    end["character"] = byteToUtf16Column(lt, static_cast<int>(lt.size()));
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    lens["range"] = std::move(range);
    Value command = Value::object();
    command["title"] = std::to_string(usages) + " reference" + (usages == 1 ? "" : "s");
    command["command"] = "editor.action.showReferences";
    Value arguments = Value::array();
    arguments.push(pathToUri(entry->path));
    Value position = Value::object();
    position["line"] = symbol.span.line - 1;
    position["character"] =
        byteToUtf16Column(lt, std::max(0, symbol.span.col - 1));
    arguments.push(std::move(position));
    arguments.push(Value::array());
    command["arguments"] = std::move(arguments);
    lens["command"] = std::move(command);
    result.push(std::move(lens));
  }
  reply(id, std::move(result));
}

void LanguageServer::moniker(const Value& id, const Value* params) {
  const Value* textDocument = params ? params->find("textDocument") : nullptr;
  const Value* uri = textDocument ? textDocument->find("uri") : nullptr;
  if (!uri || !uri->isString()) return replyNull(id);
  Value result = Value::object();
  result["scheme"] = "a";
  result["uri"] = uri->asString();
  result["unique"] = canonicalPath(uriToPath(uri->asString()));
  reply(id, std::move(result));
}

// Links for http(s) URLs inside comments, like clangd's comment links.
void LanguageServer::documentLinks(const Value& id, const Value* params) {
  Value result = Value::array();
  Document* document = documentFrom(params);
  if (!document || !document->compilation) {
    reply(id, std::move(result));
    return;
  }
  const Compilation& compilation = *document->compilation;
  const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
  const std::vector<a::Comment>* comments = compilation.commentsForModule(entry);
  if (!comments) {
    reply(id, std::move(result));
    return;
  }
  for (const a::Comment& comment : *comments) {
    std::size_t pos = 0;
    while ((pos = comment.text.find("http", pos)) != std::string::npos) {
      const bool isHttp = comment.text.compare(pos, 7, "http://") == 0;
      const bool isHttps = comment.text.compare(pos, 8, "https://") == 0;
      if (!isHttp && !isHttps) {
        ++pos;
        continue;
      }
      std::size_t end = pos;
      while (end < comment.text.size() &&
             !std::isspace(static_cast<unsigned char>(comment.text[end])) &&
             comment.text[end] != ')' && comment.text[end] != ']' &&
             comment.text[end] != '>' && comment.text[end] != '"' &&
             comment.text[end] != '\'') {
        ++end;
      }
      // Trim trailing punctuation that is almost never part of the URL.
      while (end > pos && (comment.text[end - 1] == '.' || comment.text[end - 1] == ',' ||
                           comment.text[end - 1] == ';' || comment.text[end - 1] == ':')) {
        --end;
      }
      const std::string url = comment.text.substr(pos, end - pos);
      Value link = Value::object();
      // comment.span covers the whole comment including '#'; text columns
      // map 1:1 onto source columns (comments are single-line).
      const int startCol = comment.span.col + static_cast<int>(pos);
      const int endCol = comment.span.col + static_cast<int>(end);
      link["range"] = rangeValue(
          document->text,
          Span{comment.span.line, startCol, comment.span.line, endCol, comment.span.fileId});
      link["target"] = url;
      result.push(std::move(link));
      pos = end;
    }
  }
  reply(id, std::move(result));
}

}  // namespace a::lsp
