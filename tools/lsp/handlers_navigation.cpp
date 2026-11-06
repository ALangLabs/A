// Navigation handlers: go-to-definition (including builtin/method doc
// tables), references, rename, and document highlights.

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

bool LanguageServer::validIdentifier(const std::string& name) {
  if (name.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  for (const char* keyword : kKeywords) {
    if (name == keyword) return false;
  }
  return true;
}

std::vector<std::pair<std::string, Span>> LanguageServer::collectReferences(
    const Compilation& compilation, const ResolvedSymbol& target) const {
  std::vector<std::pair<std::string, Span>> references;
  if (!target.valid()) return references;
  const std::string targetPath = canonicalPath(target.file->path);
  for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
    if (moduleId >= compilation.tokensByModule.size()) continue;
    const FileIndex& file = compilation.indexes[moduleId];
    const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
      const Token& token = tokens[static_cast<std::size_t>(i)];
      if (token.kind != Tok::Ident) continue;
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
      if (!resolved.valid()) continue;
      if (resolved.symbol->span.line != target.symbol->span.line) continue;
      if (resolved.symbol->span.col != target.symbol->span.col) continue;
      if (canonicalPath(resolved.file->path) != targetPath) continue;
      references.emplace_back(resolved.file->path, token.span);
    }
  }
  return references;
}

// Source-tree root of the A repo (the directory containing
// compiler/astra/frontend/...), found by walking up from the document. Builtin
// definitions live in the compiler sources, so go-to-definition needs
// this to locate `builtin_docs.hpp` / `method_signatures.hpp`.
std::string LanguageServer::sourceTreeRoot(const std::string& filePath) {
  fs::path dir = fs::path(filePath).parent_path();
  for (int depth = 0; depth < 12 && !dir.empty(); ++depth) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "compiler" / "stages" / "name-resolution" /
                                "builtin_docs.hpp", ec)) {
      return dir.string();
    }
    dir = dir.parent_path();
  }
  return {};
}

std::string LanguageServer::readSourceFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return input ? std::string(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>())
               : "";
}

// A location at the entry line `{"<name>",` in a compiler doc table.
Value LanguageServer::lineLocation(const std::string& path, int oneBasedLine) {
  Value location = Value::object();
  location["uri"] = pathToUri(path);
  Value range = Value::object();
  Value start = Value::object();
  start["line"] = oneBasedLine - 1;
  start["character"] = 0;
  Value end = Value::object();
  end["line"] = oneBasedLine - 1;
  end["character"] = 1;
  range["start"] = std::move(start);
  range["end"] = std::move(end);
  location["range"] = std::move(range);
  return location;
}

// First `{"<name>",` entry line of `text`; 0 when absent.
int LanguageServer::docEntryLine(const std::string& text, const std::string& name) {
  const std::string needle = "{\"" + name + "\",";
  std::size_t pos = 0;
  int line = 1;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    const std::string raw =
        text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const std::string trimmed = trimLine(raw);
    if (trimmed.rfind(needle, 0) == 0) return line;
    if (end == std::string::npos) break;
    pos = end + 1;
    ++line;
  }
  return 0;
}

// Go-to-definition target for a builtin (e.g. `ic`): the `builtin_docs.hpp`
// entry, like Python's typeshed lookup. Empty Value when not found.
Value LanguageServer::builtinDocLocation(const std::string& documentPath, const std::string& builtinName) {
  const std::string root = sourceTreeRoot(documentPath);
  if (root.empty() || !a::findBuiltinDoc(builtinName)) return Value();
  const std::string header = (fs::path(root) / "compiler" / "stages" /
                              "name-resolution" / "builtin_docs.hpp").string();
  const std::string text = readSourceFile(header);
  const int line = docEntryLine(text, builtinName);
  return line > 0 ? lineLocation(header, line) : Value();
}

// Go-to-definition target for a primitive method (e.g. `words.strip()`):
// the `method_signatures.hpp` entry inside the receiver type's table.
Value LanguageServer::methodDocLocation(const std::string& documentPath, const std::string& typeName,
                        const std::string& methodName) {
  const std::vector<a::MethodSig>* table = methodTableForType(typeName);
  if (!table || !findMethodSig(*table, methodName)) return Value();
  const char* tableFn = nullptr;
  if (table == &a::arrayMethods()) tableFn = "arrayMethods";
  else if (table == &a::mapMethods()) tableFn = "mapMethods";
  else if (table == &a::setMethods()) tableFn = "setMethods";
  else if (table == &a::stringMethods()) tableFn = "stringMethods";
  if (!tableFn) return Value();
  const std::string root = sourceTreeRoot(documentPath);
  if (root.empty()) return Value();
  const std::string header = (fs::path(root) / "compiler" / "stages" /
                              "type-checker" / "method_signatures.hpp").string();
  const std::string text = readSourceFile(header);
  const std::size_t tablePos = text.find(std::string(tableFn) + "()");
  if (tablePos == std::string::npos) return Value();
  const std::string section = text.substr(tablePos);
  const int line = docEntryLine(section, methodName);
  if (line <= 0) return Value();
  int lineOffset = 0;
  for (std::size_t pos = 0; pos < tablePos; ++pos) {
    if (text[pos] == '\n') ++lineOffset;
  }
  return lineLocation(header, line + lineOffset);
}

void LanguageServer::definition(const Value& id, const Value* params) {
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
    reply(id, locationValue(compilation, resolved.file->path, resolved.symbol->span));
    return;
  }

  // Pyright-style fallback: ctrl+click on the module alias in the import
  // statement (or as a member receiver, `utils.helper()`) opens the module.
  // `from m import n` resolves to the member when it can be found.
  const Token* token = tokenAt(*tokens, line + 1, byteColumn);
  if (token && token->kind == Tok::Ident) {
    auto importModulePath = [&](const a::lsp::ImportDef& def) -> const std::string* {
      if (def.moduleId < 0 ||
          static_cast<std::size_t>(def.moduleId) >= compilation.graph.modules.size()) {
        return nullptr;
      }
      return &compilation.graph.modules[static_cast<std::size_t>(def.moduleId)].path;
    };
    auto jumpToModule = [&](const a::lsp::ImportDef& def, const std::string& path) {
      if (!def.wholeModule && !def.member.empty()) {
        const a::lsp::FileIndex* target = compilation.indexForModule(def.moduleId);
        if (target) {
          for (const a::lsp::SymbolDef& sym : target->symbols) {
            if (sym.name == def.member) {
              reply(id, locationValue(compilation, path, sym.span));
              return true;
            }
          }
        }
      }
      reply(id, locationValue(compilation, path, Span{1, 1, 1, 2, 0}));
      return true;
    };
    // The cursor sits inside an import statement's own span. For a
    // from-import, `span` covers the member and `moduleSpan` the module.
    for (const a::lsp::ImportDef& def : file->imports) {
      auto covers = [&](const Span& span) {
        return span.valid() && span.line == line + 1 && byteColumn >= span.col &&
               byteColumn < span.endCol;
      };
      if (covers(def.moduleSpan)) {
        const std::string* path = importModulePath(def);
        if (path) {
          reply(id, locationValue(compilation, *path, Span{1, 1, 1, 2, 0}));
          return;
        }
      }
      if (covers(def.span)) {
        const std::string* path = importModulePath(def);
        if (path && jumpToModule(def, *path)) return;
      }
    }
    // A whole-module alias used as the base of a member access.
    for (const a::lsp::ImportDef& def : file->imports) {
      if (!def.wholeModule || def.alias != token->lexeme) continue;
      const std::string* path = importModulePath(def);
      if (path && jumpToModule(def, *path)) return;
    }
    // Ctrl+click on a builtin (like Python's typeshed lookup): bare
    // builtins (ic, print, ...) and native module members (json.parse)
    // land on the `builtin_docs.hpp` doc entry; primitive methods
    // (words.strip()) land on the `method_signatures.hpp` entry.
    const int index = tokenIndexAt(*tokens, line + 1, byteColumn);
    if (index >= 2 && token->kind == Tok::Ident) {
      const Token& dot = (*tokens)[static_cast<std::size_t>(index - 1)];
      const Token& baseToken = (*tokens)[static_cast<std::size_t>(index - 2)];
      if ((dot.kind == Tok::Dot || dot.kind == Tok::QuestionDot) &&
          baseToken.kind == Tok::Ident) {
        const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
        if (!moduleName.empty()) {
          const std::string* builtinName = a::nativeFunction(moduleName, token->lexeme);
          if (!builtinName) builtinName = a::nativeConstant(moduleName, token->lexeme);
          if (builtinName) {
            Value location = builtinDocLocation(document->path, *builtinName);
            if (!location.isNull()) {
              reply(id, std::move(location));
              return;
            }
          }
        }
        ResolvedSymbol base =
            a::lsp::resolveToken(compilation, *file, *tokens, index - 2);
        if (base.valid()) {
          Value location =
              methodDocLocation(document->path, base.symbol->typeName, token->lexeme);
          if (!location.isNull()) {
            reply(id, std::move(location));
            return;
          }
        }
      }
    }
    if (token->kind == Tok::Ident) {
      Value location = builtinDocLocation(document->path, token->lexeme);
      if (!location.isNull()) {
        reply(id, std::move(location));
        return;
      }
    }
  }
  return replyNull(id);
}

void LanguageServer::references(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* entry = compilation.entryIndex();
  const std::vector<Token>* entryTokens = compilation.entryTokens();
  if (!entry || !entryTokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

  ResolvedSymbol target = a::lsp::resolveAt(compilation, *entry, *entryTokens, line + 1, byteColumn);
  if (!target.valid()) return replyNull(id);

  bool includeDeclaration = true;
  if (params) {
    if (const Value* context = params->find("context")) {
      if (const Value* include = context->find("includeDeclaration");
          include && include->isBool()) {
        includeDeclaration = include->asBool();
      }
    }
  }

  Value result = Value::array();
  for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
    const FileIndex& file = compilation.indexes[moduleId];
    if (moduleId >= compilation.tokensByModule.size()) continue;
    const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
      const Token& token = tokens[static_cast<std::size_t>(i)];
      if (token.kind != Tok::Ident) continue;
      if (!includeDeclaration && token.span.line == target.symbol->span.line &&
          token.span.col == target.symbol->span.col &&
          token.span.endCol == target.symbol->span.endCol) {
        continue;
      }
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
      if (!resolved.valid()) continue;
      if (resolved.symbol->span.line != target.symbol->span.line) continue;
      if (resolved.symbol->span.col != target.symbol->span.col) continue;
      if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
      result.push(locationValue(compilation, resolved.file->path, token.span));
    }
  }
  reply(id, std::move(result));
}

void LanguageServer::rename(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) return replyNull(id);
  const Value* newNameValue = params->find("newName");
  if (!newNameValue || !newNameValue->isString()) return replyNull(id);
  const std::string newName = newNameValue->asString();
  if (!validIdentifier(newName)) {
    sendError(id, -32602, "invalid identifier: " + newName);
    return;
  }

  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
  ResolvedSymbol target =
      a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (!target.valid()) return replyNull(id);

  const auto references = collectReferences(compilation, target);
  if (references.empty()) return replyNull(id);

  Value changes = Value::object();
  for (const auto& [path, span] : references) {
    const std::string uri = pathToUri(path);
    if (!changes.contains(uri)) changes[uri] = Value::array();
    const std::string* text = compilation.textForPath(path);
    static const std::string empty;
    Value edit = Value::object();
    edit["range"] = rangeValue(text ? *text : empty, span);
    edit["newText"] = newName;
    changes[uri].push(std::move(edit));
  }

  Value workspaceEdit = Value::object();
  workspaceEdit["changes"] = std::move(changes);
  reply(id, std::move(workspaceEdit));
}

void LanguageServer::prepareRename(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);
  int line = 0, character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
  ResolvedSymbol target = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (!target.valid()) return replyNull(id);
  reply(id, rangeValue(document->text, target.symbol->span));
}

void LanguageServer::documentHighlights(const Value& id, const Value* params) {
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
  ResolvedSymbol target =
      a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
  if (!target.valid()) return replyNull(id);

  Value highlights = Value::array();
  for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind != Tok::Ident) continue;
    ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, i);
    if (!resolved.valid()) continue;
    if (resolved.symbol->span.line != target.symbol->span.line) continue;
    if (resolved.symbol->span.col != target.symbol->span.col) continue;
    if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
    Value highlight = Value::object();
    highlight["range"] = rangeValue(document->text, token.span);
    highlight["kind"] = 1;  // Text
    highlights.push(std::move(highlight));
  }
  reply(id, std::move(highlights));
}

}  // namespace a::lsp
