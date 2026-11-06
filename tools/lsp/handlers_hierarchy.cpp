// Hierarchy handlers: call hierarchy (prepare/incoming/outgoing) and
// type hierarchy (prepare/supertypes/subtypes). Items carry a `data`
// blob (path, name-span line/col) between prepare and related requests.

#include "server.hpp"
#include "diagnostics.hpp"
#include "docs.hpp"
#include "text_utils.hpp"

#include "analysis.hpp"
#include "json.hpp"
#include "name-resolution/builtin_docs.hpp"
#include "name-resolution/native_modules.hpp"
#include "tooling/formatter.hpp"
#include "type-checker/method_signatures.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace a::lsp {

namespace fs = std::filesystem;
using a::Diagnostic;
using a::Severity;
using a::Span;
using a::Tok;
using a::Token;
using a::lsp::Compilation;
using a::lsp::FileIndex;
using a::lsp::ResolvedSymbol;
using a::lsp::SymbolDef;
using a::lsp::SymbolKind;

std::string LanguageServer::modulePathForId(const Compilation& compilation, int moduleId) const {
  if (moduleId < 0 ||
      static_cast<std::size_t>(moduleId) >= compilation.graph.modules.size()) {
    return {};
  }
  return compilation.graph.modules[static_cast<std::size_t>(moduleId)].path;
}

Value LanguageServer::itemDataFor(const std::string& path, int line, int col) const {
  Value data = Value::object();
  data["path"] = path;
  data["line"] = line;
  data["col"] = col;
  return data;
}

// Reads the `data` blob hierarchy items carry between prepare/incoming/
// outgoing requests: the declaration's file path and name-span position.
bool LanguageServer::itemTargetFromData(const Value* item, std::string* path, int* line, int* col) const {
  if (!item) return false;
  const Value* data = item->find("data");
  if (!data) return false;
  const Value* p = data->find("path");
  const Value* l = data->find("line");
  const Value* c = data->find("col");
  if (!p || !p->isString() || !l || !l->isNumber() || !c || !c->isNumber()) return false;
  *path = p->asString();
  *line = static_cast<int>(l->asInt());
  *col = static_cast<int>(c->asInt());
  return true;
}

// CallHierarchyItem for a resolved declaration (the "to" side, and the item
// prepareCallHierarchy returns).
Value LanguageServer::callItemForSymbol(const Compilation& compilation, const ResolvedSymbol& resolved) const {
  const SymbolDef& symbol = *resolved.symbol;
  int declModule = -1;
  const a::Stmt* decl =
      a::lsp::findDeclAt(compilation, resolved.file->path, symbol.span.line,
                         symbol.span.col, &declModule);
  const std::string* text = compilation.textForPath(resolved.file->path);
  static const std::string empty;
  const std::string& source = text ? *text : empty;
  Value item = Value::object();
  item["name"] = symbol.name;
  item["kind"] = static_cast<int>(symbol.kind);
  if (!symbol.detail.empty()) item["detail"] = symbol.detail;
  item["uri"] = pathToUri(resolved.file->path);
  item["range"] = rangeValue(source, decl ? decl->span : symbol.span);
  item["selectionRange"] = rangeValue(source, symbol.span);
  item["data"] = itemDataFor(resolved.file->path, symbol.span.line, symbol.span.col);
  return item;
}

// CallHierarchyItem for the enclosing callable of a call site (the "from"
// side of incomingCalls).
Value LanguageServer::callItemForSite(const Compilation& compilation, const a::lsp::CallSite& site) const {
  const std::string path = modulePathForId(compilation, site.moduleId);
  const std::string* text = compilation.textForModule(site.moduleId);
  static const std::string empty;
  const std::string& source = text ? *text : empty;
  Value item = Value::object();
  item["name"] = site.callableName;
  item["kind"] = static_cast<int>(site.callableKind);
  if (!site.callableDetail.empty()) item["detail"] = site.callableDetail;
  item["uri"] = pathToUri(path);
  item["range"] = rangeValue(source, site.callableSpan);
  item["selectionRange"] = rangeValue(source, site.callableNameSpan);
  item["data"] = itemDataFor(path, site.callableNameSpan.line, site.callableNameSpan.col);
  return item;
}

// CallHierarchyItem for the function an outgoingCalls request targets.
Value LanguageServer::callItemForFnDecl(const Compilation& compilation, const a::FnDecl& fn,
                        const std::string& path, int kind) const {
  const std::string* text = compilation.textForPath(path);
  static const std::string empty;
  const std::string& source = text ? *text : empty;
  Value item = Value::object();
  item["name"] = fn.name;
  item["kind"] = kind;
  item["detail"] = a::lsp::signatureText(fn);
  item["uri"] = pathToUri(path);
  item["range"] = rangeValue(source, fn.span);
  item["selectionRange"] = rangeValue(source, fn.nameSpan);
  item["data"] = itemDataFor(path, fn.nameSpan.line, fn.nameSpan.col);
  return item;
}

void LanguageServer::prepareCallHierarchy(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);
  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn =
      utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
  ResolvedSymbol resolved =
      a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (!resolved.valid() ||
      (resolved.symbol->kind != SymbolKind::Function &&
       resolved.symbol->kind != SymbolKind::Method)) {
    return replyNull(id);
  }
  Value result = Value::array();
  result.push(callItemForSymbol(compilation, resolved));
  reply(id, std::move(result));
}

// callHierarchy/incomingCalls and callHierarchy/outgoingCalls take only
// {item} (no textDocument), so they ensure every open document is compiled
// and search all of their module graphs.
void LanguageServer::incomingCalls(const Value& id, const Value* params) {
  Value result = Value::array();
  const Value* item = params ? params->find("item") : nullptr;
  std::string path;
  int line = 0;
  int col = 0;
  if (!itemTargetFromData(item, &path, &line, &col)) {
    reply(id, std::move(result));
    return;
  }

  for (auto& entry : documents_) ensureCompiled(&entry.second);
  std::unordered_map<const Compilation*, std::vector<a::lsp::CallSite>> sitesCache;
  std::unordered_set<std::string> seenSites;
  for (auto& entry : documents_) {
    Document& document = entry.second;
    if (!document.compilation) continue;
    const Compilation& compilation = *document.compilation;
    ResolvedSymbol target = findSymbolBySpan(compilation, path, line, col);
    if (!target.valid()) continue;
    auto& sites = sitesCache[&compilation];
    if (sites.empty()) sites = a::lsp::collectCallSites(compilation);
    for (const a::lsp::CallSite& site : sites) {
      const std::string modulePath = modulePathForId(compilation, site.moduleId);
      const std::string key =
          canonicalPath(modulePath) + "|" + std::to_string(site.callSpan.line) + ":" +
          std::to_string(site.callSpan.col) + ":" + std::to_string(site.callSpan.endCol) +
          "|" + std::to_string(site.calleeSpan.line) + ":" +
          std::to_string(site.calleeSpan.col);
      if (!seenSites.insert(key).second) continue;
      if (!site.inCallable) continue;
      ResolvedSymbol callee = resolveCallCallee(compilation, site);
      if (!callee.valid()) continue;
      if (callee.symbol->span.line != target.symbol->span.line ||
          callee.symbol->span.col != target.symbol->span.col ||
          canonicalPath(callee.file->path) != canonicalPath(target.file->path)) {
        continue;
      }
      Value incoming = Value::object();
      incoming["from"] = callItemForSite(compilation, site);
      Value fromRanges = Value::array();
      const std::string* text = compilation.textForPath(modulePath);
      static const std::string empty;
      fromRanges.push(rangeValue(text ? *text : empty, site.callSpan));
      incoming["fromRanges"] = std::move(fromRanges);
      incoming["to"] = callItemForSymbol(compilation, target);
      result.push(std::move(incoming));
    }
  }
  reply(id, std::move(result));
}

void LanguageServer::outgoingCalls(const Value& id, const Value* params) {
  Value result = Value::array();
  const Value* item = params ? params->find("item") : nullptr;
  std::string path;
  int line = 0;
  int col = 0;
  if (!itemTargetFromData(item, &path, &line, &col)) {
    reply(id, std::move(result));
    return;
  }

  for (auto& entry : documents_) ensureCompiled(&entry.second);
  const std::string canonical = canonicalPath(path);
  // Prefer the compilation of the document the item was prepared in; any
  // open document whose graph contains the module works as a fallback.
  const Compilation* compilation = nullptr;
  for (auto& entry : documents_) {
    if (!entry.second.compilation) continue;
    bool owns = canonicalPath(entry.second.path) == canonical;
    for (const a::Module& module : entry.second.compilation->graph.modules) {
      if (canonicalPath(module.path) == canonical) {
        if (owns || !compilation) compilation = entry.second.compilation.get();
        break;
      }
    }
    if (compilation && canonicalPath(entry.second.path) == canonical) break;
  }
  if (!compilation) {
    reply(id, std::move(result));
    return;
  }

  int moduleId = -1;
  const a::Stmt* decl = a::lsp::findDeclAt(*compilation, path, line, col, &moduleId);
  if (!decl || decl->kind != a::StmtKind::FnDecl) {
    reply(id, std::move(result));
    return;
  }
  const a::FnDecl& fn = static_cast<const a::FnDecl&>(*decl);
  const std::string modulePath = modulePathForId(*compilation, moduleId);
  int kind = 12;  // SymbolKind::Function
  if (item) {
    if (const Value* k = item->find("kind"); k && k->isNumber()) {
      kind = static_cast<int>(k->asInt());
    }
  }
  const Value fromItem = callItemForFnDecl(*compilation, fn, modulePath, kind);

  for (const a::lsp::CallSite& site : a::lsp::collectCallSites(*compilation)) {
    if (!site.inCallable || site.moduleId != moduleId) continue;
    if (site.callableNameSpan.line != fn.nameSpan.line ||
        site.callableNameSpan.col != fn.nameSpan.col) {
      continue;
    }
    ResolvedSymbol callee = resolveCallCallee(*compilation, site);
    if (!callee.valid()) continue;
    Value outgoing = Value::object();
    outgoing["from"] = fromItem;
    Value fromRanges = Value::array();
    const std::string* text = compilation->textForPath(modulePath);
    static const std::string empty;
    fromRanges.push(rangeValue(text ? *text : empty, site.callSpan));
    outgoing["fromRanges"] = std::move(fromRanges);
    outgoing["to"] = callItemForSymbol(*compilation, callee);
    result.push(std::move(outgoing));
  }
  reply(id, std::move(result));
}

// TypeHierarchyItem for a collected type declaration.
Value LanguageServer::typeItemForDecl(const Compilation& compilation,
                      const a::lsp::TypeDeclInfo& decl) const {
  const std::string path = modulePathForId(compilation, decl.moduleId);
  const std::string* text = compilation.textForModule(decl.moduleId);
  static const std::string empty;
  const std::string& source = text ? *text : empty;
  Value item = Value::object();
  item["name"] = decl.name;
  item["kind"] = static_cast<int>(decl.kind);
  if (!decl.detail.empty()) item["detail"] = decl.detail;
  item["uri"] = pathToUri(path);
  item["range"] = rangeValue(source, decl.span);
  item["selectionRange"] = rangeValue(source, decl.nameSpan);
  item["data"] = itemDataFor(path, decl.nameSpan.line, decl.nameSpan.col);
  return item;
}

// All type declarations visible to any open document (deduplicated by
// declaration span so shared modules appear once).
std::vector<LanguageServer::TypeDeclPair> LanguageServer::allTypeDecls() {
  for (auto& entry : documents_) ensureCompiled(&entry.second);
  std::vector<TypeDeclPair> decls;
  std::unordered_set<std::string> seen;
  for (auto& entry : documents_) {
    if (!entry.second.compilation) continue;
    const Compilation& compilation = *entry.second.compilation;
    for (const a::lsp::TypeDeclInfo& info : a::lsp::collectTypeDecls(compilation)) {
      const std::string path = modulePathForId(compilation, info.moduleId);
      const std::string key = canonicalPath(path) + "|" +
                              std::to_string(info.nameSpan.line) + ":" +
                              std::to_string(info.nameSpan.col);
      if (!seen.insert(key).second) continue;
      decls.emplace_back(&compilation, info);
    }
  }
  return decls;
}

const LanguageServer::TypeDeclPair* LanguageServer::findTypeDecl(
    const std::vector<LanguageServer::TypeDeclPair>& decls,
                                 const std::string& path, int line, int col) const {
  const std::string canonical = canonicalPath(path);
  for (const TypeDeclPair& pair : decls) {
    const std::string declPath = modulePathForId(*pair.first, pair.second.moduleId);
    if (canonicalPath(declPath) != canonical) continue;
    if (pair.second.nameSpan.line == line && pair.second.nameSpan.col == col) {
      return &pair;
    }
  }
  return nullptr;
}

const LanguageServer::TypeDeclPair* LanguageServer::findTypeDeclByName(
    const std::vector<LanguageServer::TypeDeclPair>& decls,
                                       const std::string& name) const {
  // Exact match first, then last-segment match for qualified base names.
  for (const TypeDeclPair& pair : decls) {
    if (pair.second.name == name) return &pair;
  }
  const std::string leaf = simpleName(name);
  for (const TypeDeclPair& pair : decls) {
    if (simpleName(pair.second.name) == leaf) return &pair;
  }
  return nullptr;
}

void LanguageServer::prepareTypeHierarchy(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);
  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn =
      utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
  ResolvedSymbol resolved =
      a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (!resolved.valid() ||
      (resolved.symbol->kind != SymbolKind::Struct &&
       resolved.symbol->kind != SymbolKind::Class &&
       resolved.symbol->kind != SymbolKind::Enum)) {
    return replyNull(id);
  }
  const std::vector<TypeDeclPair> decls = allTypeDecls();
  if (const TypeDeclPair* pair =
          findTypeDecl(decls, resolved.file->path, resolved.symbol->span.line,
                       resolved.symbol->span.col)) {
    Value result = Value::array();
    result.push(typeItemForDecl(*pair->first, pair->second));
    reply(id, std::move(result));
    return;
  }
  // Fallback: the index knows the symbol but no AST declaration matched
  // (e.g. analysis stopped early). Still return a usable item.
  const std::string* text = compilation.textForPath(resolved.file->path);
  static const std::string empty;
  const std::string& source = text ? *text : empty;
  Value item = Value::object();
  item["name"] = resolved.symbol->name;
  item["kind"] = static_cast<int>(resolved.symbol->kind);
  if (!resolved.symbol->detail.empty()) item["detail"] = resolved.symbol->detail;
  item["uri"] = pathToUri(resolved.file->path);
  item["range"] = rangeValue(source, resolved.symbol->span);
  item["selectionRange"] = rangeValue(source, resolved.symbol->span);
  item["data"] = itemDataFor(resolved.file->path, resolved.symbol->span.line,
                           resolved.symbol->span.col);
  Value result = Value::array();
  result.push(std::move(item));
  reply(id, std::move(result));
}

void LanguageServer::typeHierarchyRelated(const Value& id, const Value* params, bool supertypes) {
  Value result = Value::array();
  const Value* item = params ? params->find("item") : nullptr;
  std::string path;
  int line = 0;
  int col = 0;
  if (!itemTargetFromData(item, &path, &line, &col)) {
    reply(id, std::move(result));
    return;
  }
  const std::vector<TypeDeclPair> decls = allTypeDecls();
  const TypeDeclPair* self = findTypeDecl(decls, path, line, col);
  if (!self) {
    reply(id, std::move(result));
    return;
  }
  if (supertypes) {
    for (const std::string& parent : self->second.parents) {
      if (const TypeDeclPair* base = findTypeDeclByName(decls, parent)) {
        result.push(typeItemForDecl(*base->first, base->second));
      }
    }
  } else {
    const std::string selfPath = modulePathForId(*self->first, self->second.moduleId);
    for (const TypeDeclPair& pair : decls) {
      const std::string pairPath = modulePathForId(*pair.first, pair.second.moduleId);
      if (canonicalPath(pairPath) == canonicalPath(selfPath) &&
          pair.second.nameSpan.line == self->second.nameSpan.line &&
          pair.second.nameSpan.col == self->second.nameSpan.col) {
        continue;  // never report the type as its own subtype
      }
      for (const std::string& parent : pair.second.parents) {
        if (pair.second.name == self->second.name ||
            simpleName(parent) == simpleName(self->second.name) ||
            parent == self->second.name) {
          result.push(typeItemForDecl(*pair.first, pair.second));
          break;
        }
      }
    }
  }
  reply(id, std::move(result));
}

// Parameter names for a resolved call target: FnDecl params (methods drop
// `self`) or StructDecl fields for constructor calls.
std::vector<std::string> LanguageServer::paramNamesForSymbol(const Compilation& compilation,
                                             const ResolvedSymbol& resolved) const {
  int moduleId = -1;
  const a::Stmt* decl =
      a::lsp::findDeclAt(compilation, resolved.file->path, resolved.symbol->span.line,
                         resolved.symbol->span.col, &moduleId);
  if (!decl) return {};
  if (decl->kind == a::StmtKind::FnDecl) {
    const auto& fn = static_cast<const a::FnDecl&>(*decl);
    std::vector<std::string> names;
    for (const auto& param : fn.params) names.push_back(param.name);
    if (!names.empty() && names.front() == "self") names.erase(names.begin());
    return names;
  }
  if (decl->kind == a::StmtKind::StructDecl) {
    std::vector<std::string> names;
    for (const auto& field : static_cast<const a::StructDecl&>(*decl).fields) {
      names.push_back(field.name);
    }
    return names;
  }
  return {};
}

}  // namespace a::lsp
