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

namespace {

// Spans of identifiers written by the entry module's statements:
// assignment targets (including the receiver root of `x.f =` / `x[i] =`),
// destructuring names, `let` bindings, and `for` variables. Keyed by
// (line << 32) | col — collision-free for any realistic file.
using WriteSet = std::unordered_set<long long>;
inline long long writeKey(int line, int col) {
  return (static_cast<long long>(line) << 32) | static_cast<unsigned int>(col);
}
inline void markWrite(WriteSet& out, const Span& span) {
  if (span.valid()) out.insert(writeKey(span.line, span.col));
}

// Marks the root identifier of an assignment target chain: `x` in `x = `,
// `x.f = `, `x.f.g = `, and `x[i] = ` is the value being mutated.
void markAssignTarget(const a::Expr* expr, WriteSet& out) {
  if (!expr) return;
  switch (expr->kind) {
    case a::ExprKind::Ident:
      markWrite(out, expr->span);
      return;
    case a::ExprKind::Field:
      markAssignTarget(static_cast<const a::FieldExpr&>(*expr).object.get(), out);
      return;
    case a::ExprKind::SafeNav:
      markAssignTarget(static_cast<const a::SafeNavExpr&>(*expr).object.get(), out);
      return;
    case a::ExprKind::Index:
      markAssignTarget(static_cast<const a::IndexExpr&>(*expr).object.get(), out);
      return;
    default:
      return;
  }
}

void collectWriteSpans(const a::Stmt& stmt, WriteSet& out) {
  auto block = [&](const std::vector<a::StmtPtr>& body) {
    for (const auto& child : body) {
      if (child) collectWriteSpans(*child, out);
    }
  };
  switch (stmt.kind) {
    case a::StmtKind::Let:
      markWrite(out, static_cast<const a::LetStmt&>(stmt).nameSpan);
      break;
    case a::StmtKind::Assign:
      markAssignTarget(static_cast<const a::AssignStmt&>(stmt).target.get(), out);
      break;
    case a::StmtKind::Destructure:
      for (const Span& span : static_cast<const a::DestructureStmt&>(stmt).nameSpans) {
        markWrite(out, span);
      }
      break;
    case a::StmtKind::For:
      markWrite(out, static_cast<const a::ForStmt&>(stmt).varSpan);
      break;
    default:
      break;
  }
  switch (stmt.kind) {
    case a::StmtKind::Defer:
      if (static_cast<const a::DeferStmt&>(stmt).body) {
        collectWriteSpans(*static_cast<const a::DeferStmt&>(stmt).body, out);
      }
      return;
    case a::StmtKind::If: {
      const auto& ifStmt = static_cast<const a::IfStmt&>(stmt);
      for (const auto& branch : ifStmt.branches) block(branch.body);
      block(ifStmt.elseBody);
      return;
    }
    case a::StmtKind::While:
      block(static_cast<const a::WhileStmt&>(stmt).body);
      return;
    case a::StmtKind::DoWhile:
      block(static_cast<const a::DoWhileStmt&>(stmt).body);
      return;
    case a::StmtKind::For:
      block(static_cast<const a::ForStmt&>(stmt).body);
      return;
    case a::StmtKind::Loop:
      block(static_cast<const a::LoopStmt&>(stmt).body);
      return;
    case a::StmtKind::FnDecl:
      block(static_cast<const a::FnDecl&>(stmt).body);
      return;
    case a::StmtKind::StructDecl:
      for (const auto& method : static_cast<const a::StructDecl&>(stmt).methods) {
        if (method) collectWriteSpans(*method, out);
      }
      return;
    case a::StmtKind::ImplDecl:
      for (const auto& method : static_cast<const a::ImplDecl&>(stmt).methods) {
        if (method) collectWriteSpans(*method, out);
      }
      return;
    default:
      return;
  }
}

// In-place mutators from method_signatures.hpp: `xs.append(v)` writes `xs`.
bool isMutatingCall(const std::string& typeName, const std::string& method) {
  static const char* const kArray[] = {"append", "extend",  "insert", "remove_at",
                                       "pop",    "remove",  "clear",  "sort",
                                       "reverse"};
  static const char* const kMap[] = {"pop", "update", "clear"};
  static const char* const kSet[] = {"add", "remove", "discard", "clear"};
  const char* const* methods = nullptr;
  std::size_t count = 0;
  if (typeName == "array") {
    methods = kArray;
    count = sizeof(kArray) / sizeof(kArray[0]);
  } else if (typeName == "map") {
    methods = kMap;
    count = sizeof(kMap) / sizeof(kMap[0]);
  } else if (typeName == "set") {
    methods = kSet;
    count = sizeof(kSet) / sizeof(kSet[0]);
  } else {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (method == methods[i]) return true;
  }
  return false;
}

}  // namespace

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
      if (token.kind != Tok::Ident && token.kind != Tok::KwSelf) continue;
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
      if (token.kind != Tok::Ident && token.kind != Tok::KwSelf) continue;
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
  // prepareRename reports the range under the cursor (the occurrence being
  // renamed), not the declaration site — they differ when renaming a use.
  const Token* token = tokenAt(*tokens, line + 1, byteColumn);
  if (token && (token->kind == Tok::Ident || token->kind == Tok::KwSelf)) {
    reply(id, rangeValue(document->text, token->span));
    return;
  }
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

  // Write spans from the entry module's AST: assignment targets, binding
  // introductions, and for-loop variables.
  WriteSet writes;
  const int entryModule = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
  if (entryModule >= 0 &&
      static_cast<std::size_t>(entryModule) < compilation.graph.modules.size() &&
      compilation.graph.modules[static_cast<std::size_t>(entryModule)].program) {
    for (const auto& statement :
         compilation.graph.modules[static_cast<std::size_t>(entryModule)]
             .program->statements) {
      if (statement) collectWriteSpans(*statement, writes);
    }
  }

  Value highlights = Value::array();
  for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind != Tok::Ident && token.kind != Tok::KwSelf) continue;
    ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, i);
    if (!resolved.valid()) continue;
    if (resolved.symbol->span.line != target.symbol->span.line) continue;
    if (resolved.symbol->span.col != target.symbol->span.col) continue;
    if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
    // DocumentHighlightKind: 1 = Text, 2 = Read, 3 = Write. Writes are
    // assignment/binding targets and receivers of in-place mutating calls
    // (`xs.append(v)`); everything else reads.
    bool write = writes.count(writeKey(token.span.line, token.span.col)) != 0;
    if (!write && resolved.symbol && i + 3 < static_cast<int>(tokens->size())) {
      const Token& dot = (*tokens)[static_cast<std::size_t>(i + 1)];
      const Token& method = (*tokens)[static_cast<std::size_t>(i + 2)];
      const Token& paren = (*tokens)[static_cast<std::size_t>(i + 3)];
      if ((dot.kind == Tok::Dot || dot.kind == Tok::QuestionDot) &&
          method.kind == Tok::Ident && paren.kind == Tok::LParen &&
          isMutatingCall(resolved.symbol->typeName, method.lexeme)) {
        write = true;
      }
    }
    Value highlight = Value::object();
    highlight["range"] = rangeValue(document->text, token.span);
    highlight["kind"] = write ? 3 : 2;
    highlights.push(std::move(highlight));
  }
  reply(id, std::move(highlights));
}

}  // namespace a::lsp
