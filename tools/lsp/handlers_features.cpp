// Feature handlers: hover, completion, signature help, document/workspace
// symbols, formatting, semantic tokens, inlay hints, code actions,
// selection/folding ranges, code lens, monikers, and document links.

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

void LanguageServer::formatting(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);

  a::tooling::FormatOptions options;
  if (params) {
    if (const Value* formatOptions = params->find("options")) {
      if (const Value* tabSize = formatOptions->find("tabSize"); tabSize && tabSize->isNumber()) {
        const int size = static_cast<int>(tabSize->asInt());
        if (size >= 1 && size <= 16) options.indentWidth = size;
      }
    }
  }

  a::DiagnosticSink sink;
  sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
  a::tooling::FormatResult result =
      a::tooling::formatSource(document->text, sink, options);

  Value edits = Value::array();
  if (!result.ok || sink.hasErrors() || result.text == document->text) {
    reply(id, std::move(edits));
    return;
  }

  Value edit = Value::object();
  edit["range"] = fullDocumentRange(document->text);
  edit["newText"] = result.text;
  edits.push(std::move(edit));
  reply(id, std::move(edits));
}

void LanguageServer::foldingRanges(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const std::vector<Token>* tokens = document->compilation->entryTokens();
  if (!tokens) return replyNull(id);

  Value result = Value::array();
  std::vector<int> starts;
  int lastCodeLine = 0;
  auto addRange = [&](int startLine, int endLine) {
    if (startLine <= 0 || endLine <= startLine) return;
    Value range = Value::object();
    range["startLine"] = startLine - 1;
    range["endLine"] = endLine - 1;
    range["kind"] = "region";
    result.push(std::move(range));
  };

  for (const Token& token : *tokens) {
    switch (token.kind) {
      case Tok::Indent:
        starts.push_back(lastCodeLine > 0 ? lastCodeLine : token.span.line);
        break;
      case Tok::Dedent:
        if (!starts.empty()) {
          const int startLine = starts.back();
          starts.pop_back();
          addRange(startLine, lastCodeLine);
        }
        break;
      case Tok::Newline:
      case Tok::Eof:
        break;
      default:
        if (token.span.endLine > 0) lastCodeLine = token.span.endLine;
        break;
    }
  }
  while (!starts.empty()) {
    addRange(starts.back(), lastCodeLine);
    starts.pop_back();
  }
  reply(id, std::move(result));
}

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

void LanguageServer::completion(const Value& id, const Value* params) {
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

  std::unordered_set<std::string> seen;
  Value items = Value::array();

  auto pushItem = [&](const std::string& name, int kind, const std::string& detail,
                      const std::string& documentation = {}) {
    if (!seen.insert(name).second) return;
    Value item = Value::object();
    item["label"] = name;
    item["kind"] = kind;
    item["insertText"] = name;
    item["filterText"] = name;
    // Snippets for control flow (ty/pyrefly offer these).
    if (name == "if") { item["insertText"] = "if ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "while") { item["insertText"] = "while ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "for") { item["insertText"] = "for ${1:item} in ${2:iterable}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "fn") { item["insertText"] = "fn ${1:name}(${2:params}) -> ${3:void}:\n    $0"; item["insertTextFormat"] = 2; }
    if (!detail.empty()) item["detail"] = detail;
    if (!documentation.empty()) {
      Value data = Value::object();
      data["documentation"] = documentation;
      item["data"] = std::move(data);
      item["documentation"] = Value::object();
      item["documentation"]["kind"] = "markdown";
      item["documentation"]["value"] = documentation;
    }
    items.push(std::move(item));
  };

  auto pushMethods = [&](const std::vector<a::MethodSig>& table) {
    for (const a::MethodSig& method : table) {
      pushItem(method.name, 2, method.signature, method.documentation);
    }
  };

  // Member completion after "." or "?.".
  int previous = tokenIndexBefore(*tokens, line + 1, byteColumn - 1);
  int base = -1;
  if (previous >= 0) {
    const Token& token = (*tokens)[static_cast<std::size_t>(previous)];
    if (token.kind == Tok::Dot || token.kind == Tok::QuestionDot) {
      if (previous >= 1) base = previous - 1;
    } else if (previous >= 1) {
      const Token& before =
          (*tokens)[static_cast<std::size_t>(previous - 1)];
      if ((before.kind == Tok::Dot || before.kind == Tok::QuestionDot) && previous >= 2) {
        base = previous - 2;
      }
    }
  }

  if (base >= 0) {
    const Token& baseToken = (*tokens)[static_cast<std::size_t>(base)];
    for (const a::lsp::ImportDef& import : file->imports) {
      if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
      const FileIndex* target = compilation.indexForModule(import.moduleId);
      if (!target) continue;
      for (const SymbolDef& symbol : target->symbols) {
        if (symbol.topLevel) {
          pushItem(symbol.name, completionKind(symbol.kind), symbol.detail);
        }
      }
    }

    if (baseToken.kind == Tok::Ident) {
      const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
      if (!moduleName.empty()) {
        for (const a::lsp::MemberInfo& member : a::lsp::nativeModuleMembers(moduleName)) {
          pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                   member.documentation);
        }
      }
      ResolvedSymbol resolved =
          a::lsp::resolveToken(compilation, *file, *tokens, base);
      if (resolved.valid()) {
        std::string typeName = resolved.symbol->typeName;
        if (typeName.empty() &&
            (resolved.symbol->kind == SymbolKind::Struct ||
             resolved.symbol->kind == SymbolKind::Class ||
             resolved.symbol->kind == SymbolKind::Enum)) {
          typeName = resolved.symbol->name;
        }
        if (const std::vector<a::MethodSig>* table = methodTableForType(typeName)) {
          pushMethods(*table);
        } else if (!typeName.empty()) {
          for (const SymbolDef* member :
               a::lsp::structMembers(compilation, *resolved.file, typeName)) {
            pushItem(member->name, completionKind(member->kind), member->detail);
          }
        }
      }
    } else if (baseToken.kind == Tok::String) {
      pushMethods(a::stringMethods());
    } else if (baseToken.kind == Tok::RBracket) {
      pushMethods(a::arrayMethods());
    } else if (baseToken.kind == Tok::RBrace) {
      // Distinguish `{key: value}` maps from `{element}` sets by looking
      // for a colon at the literal's top level.
      bool isMap = false;
      int depth = 0;
      for (int i = base - 1; i >= 0; --i) {
        const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
        if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
            inner.kind == Tok::RParen) {
          ++depth;
        } else if (inner.kind == Tok::LBrace) {
          if (depth == 0) break;
          --depth;
        } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
          --depth;
        } else if (inner.kind == Tok::Colon && depth == 0) {
          isMap = true;
          break;
        }
      }
      pushMethods(isMap ? a::mapMethods() : a::setMethods());
    }

    // Never fall back to the global list after a dot; empty is correct
    // for receivers with no members.
    Value result = Value::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    reply(id, std::move(result));
    return;
  }

  for (const char* keyword : kKeywords) pushItem(keyword, 14, "keyword");
  for (const auto& entry : a::builtinDocs()) {
    const std::string& name = entry.first;
    if (a::isStdlibBuiltin(name) || name == "PI" || name == "E") continue;
    pushItem(name, 3, entry.second.signature);
  }
  for (const SymbolDef* symbol :
       a::lsp::visibleSymbols(compilation, *file, line + 1, byteColumn)) {
    pushItem(symbol->name, completionKind(symbol->kind), symbol->detail);
  }
  for (const a::lsp::ImportDef& import : file->imports) {
    pushItem(import.alias, import.wholeModule ? 9 : 18, import.modulePath);
  }

  // `import <TAB>`, `from <TAB>`, and `from module import <TAB>`.
  std::string currentLine = lineText(document->text, line + 1);
  std::string prefix = currentLine.substr(
      0, std::min<std::size_t>(byteColumn - 1, currentLine.size()));
  const bool insideWord =
      !prefix.empty() && !std::isspace(static_cast<unsigned char>(prefix.back()));
  std::vector<std::string> words;
  {
    std::istringstream stream(prefix);
    std::string word;
    while (stream >> word) words.push_back(word);
  }
  std::string previousWord;
  if (!words.empty()) {
    previousWord =
        insideWord && words.size() >= 2 ? words[words.size() - 2] : words.back();
  }
  if (previousWord == "import" || previousWord == "from") {
    const std::string& typed = insideWord ? words.back() : std::string();
    if (typed.rfind("std:", 0) == 0) {
      // `import std:<TAB>` / `from std:<TAB>`: complete stdlib packages.
      for (const auto& module : a::nativeModules()) {
        pushItem(module.first, 9, "native module");
      }
    } else if (previousWord == "from" && words.size() >= 2) {
      const std::size_t needed = insideWord ? 3 : 2;
      if (words.size() >= needed) {
        const std::size_t moduleIndex = insideWord ? words.size() - 3 : words.size() - 2;
        std::string moduleName = words[moduleIndex];
        if (moduleName.rfind("std:", 0) == 0) moduleName.erase(0, 4);
        if (a::isNativeModule(moduleName)) {
          for (const a::lsp::MemberInfo& member :
               a::lsp::nativeModuleMembers(moduleName)) {
            pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                     member.documentation);
          }
        }
      }
    } else {
      pushItem("std:", 9, "stdlib package");
      for (const auto& module : a::nativeModules()) {
        pushItem(module.first, 9, "native module");
      }
    }
  }

  Value result = Value::object();
  result["isIncomplete"] = false;
  result["items"] = std::move(items);
  reply(id, std::move(result));
}

void LanguageServer::completionResolve(const Value& id, const Value* params) {
  if (!params || !params->isObject()) return replyNull(id);
  const Value* item = params;
  if (const Value* nested = params->find("item"); nested && nested->isObject()) item = nested;
  Value result = *item;
  const Value* data = item->find("data");
  const Value* documentation = data && data->isObject() ? data->find("documentation") : nullptr;
  if (documentation && documentation->isString()) {
    Value contents = Value::object();
    contents["kind"] = "markdown";
    contents["value"] = documentation->asString();
    result["documentation"] = std::move(contents);
  }
  reply(id, std::move(result));
}

void LanguageServer::signatureHelp(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  const int cursorLine = line + 1;
  const int byteColumn = utf16ToByteColumn(lineText(document->text, cursorLine), character) + 1;

  // Innermost unmatched bracket whose opening is before the cursor.
  std::vector<int> stack;
  for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind == Tok::Eof) break;
    if (token.span.line > cursorLine ||
        (token.span.line == cursorLine && token.span.col >= byteColumn)) {
      break;
    }
    if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
      stack.push_back(i);
    } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
               token.kind == Tok::RBrace) {
      if (!stack.empty()) stack.pop_back();
    }
  }
  if (stack.empty() || (*tokens)[static_cast<std::size_t>(stack.back())].kind != Tok::LParen) {
    return replyNull(id);
  }
  const int openIndex = stack.back();
  if (openIndex == 0) return replyNull(id);

  // Callee: `name(`, `module.member(`, or `receiver.method(`.
  std::string label;
  const Token& callee = (*tokens)[static_cast<std::size_t>(openIndex - 1)];
  if (callee.kind != Tok::Ident) return replyNull(id);
  const bool qualified = openIndex >= 3 &&
                         ((*tokens)[static_cast<std::size_t>(openIndex - 2)].kind == Tok::Dot ||
                          (*tokens)[static_cast<std::size_t>(openIndex - 2)].kind ==
                              Tok::QuestionDot);
  const Token& baseToken =
      qualified ? (*tokens)[static_cast<std::size_t>(openIndex - 3)] : callee;
  const bool qualifiedOnDot =
      qualified && (baseToken.kind == Tok::Ident || baseToken.kind == Tok::String ||
                    baseToken.kind == Tok::RBracket || baseToken.kind == Tok::RBrace);
  if (!qualified && baseToken.kind != Tok::Ident) return replyNull(id);

  const std::string moduleAlias =
      qualifiedOnDot && baseToken.kind == Tok::Ident ? nativeModuleForAlias(*file, baseToken.lexeme)
                                                     : std::string();
  if (!moduleAlias.empty()) {
    const std::string* builtinName = a::nativeFunction(moduleAlias, callee.lexeme);
    if (!builtinName) builtinName = a::nativeConstant(moduleAlias, callee.lexeme);
    if (builtinName) {
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) label = doc->signature;
    }
  }
  if (label.empty()) {
    const bool primitiveBase = baseToken.kind != Tok::Ident &&
                               baseToken.kind != Tok::Eof &&
                               (baseToken.kind == Tok::String ||
                                baseToken.kind == Tok::RBracket ||
                                baseToken.kind == Tok::RBrace);
    std::string typeName;
    if (primitiveBase) {
      typeName = baseToken.kind == Tok::String
                     ? "String"
                     : (baseToken.kind == Tok::RBracket ? "array" : "map");
      if (baseToken.kind == Tok::RBrace) {
        int depth = 0;
        bool isMap = false;
        for (int i = openIndex - 4; i >= 0; --i) {
          const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
          if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
              inner.kind == Tok::RParen) {
            ++depth;
          } else if (inner.kind == Tok::LBrace) {
            if (depth == 0) break;
            --depth;
          } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
            --depth;
          } else if (inner.kind == Tok::Colon && depth == 0) {
            isMap = true;
            break;
          }
        }
        typeName = isMap ? "map" : "set";
      }
    } else if (baseToken.kind == Tok::Ident) {
      ResolvedSymbol resolved =
          a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 3);
      if (resolved.valid()) typeName = resolved.symbol->typeName;
    }
    const std::vector<a::MethodSig>* table = methodTableForType(typeName);
    if (table) {
      if (const a::MethodSig* method = findMethodSig(*table, callee.lexeme)) {
        label = method->signature;
      }
    }
  }
  if (label.empty() && baseToken.kind == Tok::Ident) {
    ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 1);
    if (resolved.valid() && !resolved.symbol->detail.empty()) {
      label = resolved.symbol->detail;
    }
  }
  if (label.empty() && qualifiedOnDot && baseToken.kind == Tok::Ident) {
    auto found = compilation.sema.functions.find(baseToken.lexeme + "." + callee.lexeme);
    if (found == compilation.sema.functions.end()) {
      found = compilation.sema.functions.find(callee.lexeme);
    }
    if (found != compilation.sema.functions.end() && found->second) {
      label = a::lsp::signatureText(*found->second);
    }
  }
  if (label.empty() && !qualifiedOnDot) {
    if (const a::BuiltinDoc* doc = a::findBuiltinDoc(callee.lexeme)) label = doc->signature;
  }
  if (label.empty()) return replyNull(id);

  // Split the parameter list of the label into display segments.
  std::vector<std::string> parameters;
  std::size_t start = label.find('(');
  if (start != std::string::npos) {
    int depth = 0;
    std::size_t segmentStart = start + 1;
    for (std::size_t i = start; i < label.size(); ++i) {
      char c = label[i];
      if (c == '(' || c == '[' || c == '{') {
        ++depth;
      } else if (c == ')' || c == ']' || c == '}') {
        --depth;
        if (depth == 0) {
          std::string last = label.substr(segmentStart, i - segmentStart);
          if (!last.empty()) parameters.push_back(last);
          break;
        }
      } else if (c == ',' && depth == 1) {
        parameters.push_back(label.substr(segmentStart, i - segmentStart));
        segmentStart = i + 1;
      }
    }
  }

  // Active parameter: top-level commas between the open paren and cursor.
  int activeParameter = 0;
  int depth = 0;
  for (int i = openIndex + 1; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind == Tok::Eof) break;
    if (token.span.line > cursorLine ||
        (token.span.line == cursorLine && token.span.col >= byteColumn)) {
      break;
    }
    if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
      ++depth;
    } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
               token.kind == Tok::RBrace) {
      if (depth == 0) break;
      --depth;
    } else if (token.kind == Tok::Comma && depth == 0) {
      ++activeParameter;
    }
  }
  if (activeParameter > 0 && parameters.empty()) activeParameter = 0;
  if (static_cast<std::size_t>(activeParameter) >= parameters.size() && !parameters.empty()) {
    activeParameter = static_cast<int>(parameters.size()) - 1;
  }

  Value signature = Value::object();
  signature["label"] = label;
  Value parameterValues = Value::array();
  for (const std::string& parameter : parameters) {
    Value entry = Value::object();
    std::string text = parameter;
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    entry["label"] = text;
    parameterValues.push(std::move(entry));
  }
  if (!parameters.empty()) signature["parameters"] = std::move(parameterValues);
  Value signatures = Value::array();
  signatures.push(std::move(signature));

  Value result = Value::object();
  result["signatures"] = std::move(signatures);
  result["activeSignature"] = 0;
  result["activeParameter"] = activeParameter;
  reply(id, std::move(result));
}

// Semantic tokens: delta-encode [type, modifiers] per token, per the
// legend in initialize. Mirrors pyrefly's semanticTokens.
void LanguageServer::semanticTokens(const Value& id, const Value* params, bool rangeOnly) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);
  int rangeStartLine = -1, rangeEndLine = -1;
  if (rangeOnly) {
    const Value* range = params->find("range");
    if (range) {
      const Value* s = range->find("start");
      const Value* e = range->find("end");
      if (s && e) {
        rangeStartLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0) + 1;
        rangeEndLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0) + 1;
      }
    }
  }
  // Legend indices: 0 namespace,1 type,2 class,3 enum,4 function,
  // 5 method,6 parameter,7 variable,8 property,9 keyword,10 comment,
  // 11 string,12 number,13 operator.
  struct LexInfo { int line, col, len, type, mod; };
  std::vector<LexInfo> lexemes;
  auto pushLex = [&](Span span, int type, int mod = 0) {
    if (!span.valid()) return;
    if (rangeOnly && rangeStartLine >= 0 && (span.line < rangeStartLine || span.line > rangeEndLine)) return;
    std::string line = lineText(document->text, span.line);
    int startChar = byteToUtf16Column(line, std::max(0, span.col - 1));
    int endCol = span.endCol > span.col ? span.endCol : span.col + 1;
    if (span.endLine != span.line) endCol = (int)line.size() + 1;
    int len = byteToUtf16Column(line, std::max(0, endCol - 1)) - startChar;
    if (len <= 0) len = 1;
    lexemes.push_back({span.line - 1, startChar, len, type, mod});
  };
  // Precompute definition spans for classification.
  std::unordered_set<long long> defKeys;
  auto keyOf = [](Span s) { return (long long)s.fileId * 100000000 + (long long)s.line * 10000 + s.col; };
  for (const SymbolDef& sym : file->symbols) {
    int t = 7;
    if (sym.kind == SymbolKind::Struct || sym.kind == SymbolKind::Class) t = 2;
    else if (sym.kind == SymbolKind::Enum) t = 3;
    else if (sym.kind == SymbolKind::Function) t = 4;
    else if (sym.kind == SymbolKind::Method) t = 5;
    else if (sym.kind == SymbolKind::Field || sym.kind == SymbolKind::Property) t = 8;
    else if (sym.kind == SymbolKind::Module) t = 0;
    else if (sym.kind == SymbolKind::Constant) t = 7;
    defKeys.insert(keyOf(sym.span));
    pushLex(sym.span, t, 1);  // declaration modifier
  }
  for (const Token& tok : *tokens) {
    if (tok.kind == Tok::Eof) continue;
    if (defKeys.count(keyOf(tok.span))) continue;
    int t = -1;
    switch (tok.kind) {
      case Tok::String: t = 11; break;
      case Tok::Int: case Tok::Float: t = 12; break;
      case Tok::Char: t = 11; break;
      case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
      case Tok::Percent: case Tok::EqEq: case Tok::NotEq: case Tok::Lt:
      case Tok::Gt: case Tok::Le: case Tok::Ge: case Tok::Eq: case Tok::Bang:
      case Tok::Question: case Tok::Dot: case Tok::Comma: case Tok::Colon:
      case Tok::QuestionDot: case Tok::QuestionQuestion: t = 13; break;
      default: break;
    }
    if (t >= 0) { pushLex(tok.span, t); continue; }
    // Keywords are lexed as Idents with keyword lexemes in A.
    bool isKw = false;
    for (const char* kw : kKeywords) if (tok.lexeme == kw) { isKw = true; break; }
    if (isKw) { pushLex(tok.span, 9); continue; }
    if (tok.kind == Tok::Ident) {
      // Type name? builtin type names classify as `type`.
      if (a::builtinTypeByName(tok.lexeme)) { pushLex(tok.span, 1); continue; }
      if (a::findBuiltinDoc(tok.lexeme)) { pushLex(tok.span, 4); continue; }
      ResolvedSymbol r = a::lsp::resolveToken(compilation, *file, *tokens,
          (int)(&tok - &(*tokens)[0]));
      if (r.valid()) {
        int tt = 7;
        switch (r.symbol->kind) {
          case SymbolKind::Struct: case SymbolKind::Class: tt = 2; break;
          case SymbolKind::Enum: tt = 3; break;
          case SymbolKind::Function: tt = 4; break;
          case SymbolKind::Method: case SymbolKind::Field: case SymbolKind::Property: tt = 8; break;
          case SymbolKind::Module: tt = 0; break;
          default: tt = 7; break;
        }
        pushLex(tok.span, tt);
      }
    }
  }
  // Comments get type 10.
  if (const std::vector<a::Comment>* comments = compilation.commentsForModule(
          compilation.graph.entry >= 0 ? compilation.graph.entry : 0)) {
    for (const a::Comment& c : *comments) pushLex(c.span, 10);
  }
  std::sort(lexemes.begin(), lexemes.end(), [](const LexInfo& a, const LexInfo& b) {
    if (a.line != b.line) return a.line < b.line;
    return a.col < b.col;
  });
  Value data = Value::array();
  int prevLine = 0, prevCol = 0;
  for (const LexInfo& lex : lexemes) {
    int deltaLine = lex.line - prevLine;
    int deltaCol = deltaLine == 0 ? lex.col - prevCol : lex.col;
    data.push(deltaLine); data.push(deltaCol); data.push(lex.len);
    data.push(lex.type); data.push(lex.mod);
    prevLine = lex.line; prevCol = lex.col;
  }
  Value result = Value::object();
  result["data"] = std::move(data);
  reply(id, std::move(result));
}

// Inlay hints: inferred type after `let` without annotation, and
// parameter names at call sites with multiple args (ty/pyrefly style).
void LanguageServer::inlayHints(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  if (!file) return replyNull(id);
  const Value* rangeVal = params->find("range");
  int startLine = 0, endLine = INT_MAX;
  if (rangeVal) {
    const Value* s = rangeVal->find("start");
    const Value* e = rangeVal->find("end");
    if (s && e) {
      startLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0);
      endLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0);
    }
  }
  Value hints = Value::array();
  // Inferred types for variables without annotations.
  for (const auto& var : compilation.sema.variables) {
    if (!var.type) continue;
    if (var.span.line <= 0) continue;
    int varLine = var.span.line - 1;
    if (varLine < startLine || varLine > endLine) continue;
    const SymbolDef* sym = nullptr;
    for (const SymbolDef& s : file->symbols) {
      if (s.span.line == var.span.line && s.span.col == var.span.col) { sym = &s; break; }
    }
    if (!sym) continue;
    // Skip if already annotated (detail contains ": ").
    if (sym->detail.find(": ") != std::string::npos) continue;
    std::string t = a::typeName(var.type);
    if (t.empty() || t == "error" || t == "unknown") continue;
    // Skip unresolved type variables (`maybe = None` infers `?`).
    if (t.find("unknown") != std::string::npos || t.find("an ") != std::string::npos) continue;
    Value hint = Value::object();
    Value pos = Value::object();
    std::string lt = lineText(document->text, var.span.line);
    pos["line"] = var.span.line - 1;
    pos["character"] = byteToUtf16Column(lt, std::max(0, var.span.endCol - 1));
    hint["position"] = std::move(pos);
    hint["label"] = ": " + t;
    hint["kind"] = 1;  // Type
    hint["paddingRight"] = true;
    hint["tooltip"] = "Inferred type (from a check)";
    hints.push(std::move(hint));
  }
  // Parameter-name hints at call sites (ty/pyrefly style): `add(x, y)`
  // shows `lhs:`/`rhs:` before positional arguments whose callee resolves
  // to a known function or struct constructor.
  const int entryModule = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
  for (const a::lsp::CallSite& site : a::lsp::collectCallSites(compilation)) {
    if (site.moduleId != entryModule || site.positionalArgSpans.empty()) continue;
    ResolvedSymbol callee = resolveCallCallee(compilation, site);
    if (!callee.valid()) continue;
    const std::vector<std::string> names = paramNamesForSymbol(compilation, callee);
    if (names.empty()) continue;
    const std::string* moduleText = compilation.textForModule(site.moduleId);
    const std::string& source = moduleText ? *moduleText : document->text;
    const std::size_t argCount =
        std::min(names.size(), site.positionalArgSpans.size());
    for (std::size_t i = 0; i < argCount; ++i) {
      const std::string& name = names[i];
      if (name.empty() || name == "_" || name == "self") continue;
      const Span& argSpan = site.positionalArgSpans[i];
      if (!argSpan.valid()) continue;
      Value hint = Value::object();
      Value pos = Value::object();
      pos["line"] = argSpan.line - 1;
      pos["character"] =
          byteToUtf16Column(lineText(source, argSpan.line),
                            std::max(0, argSpan.col - 1));
      hint["position"] = std::move(pos);
      hint["label"] = name + ": ";
      hint["kind"] = 2;  // Parameter
      hint["paddingRight"] = false;
      hints.push(std::move(hint));
    }
  }
  Value result = Value::array();
  // Return array of hints directly.
  reply(id, std::move(hints));
}

// Code actions: quick fixes tied to diagnostics (like ty/pyrefly).
void LanguageServer::codeActions(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  Value actions = Value::array();
  if (!document || !document->compilation || !params) { reply(id, std::move(actions)); return; }
  const Compilation& compilation = *document->compilation;
  const Value* rangeVal = params->find("range");
  int reqLine = -1;
  if (rangeVal) {
    const Value* s = rangeVal->find("start");
    if (s && s->find("line")) reqLine = static_cast<int>(s->find("line")->asInt()) + 1;
  }
  const Value* context = params->find("context");
  std::vector<std::string> requestedKinds;
  if (context) {
    const Value* kinds = context->find("only");
    if (kinds && kinds->isArray()) for (auto& k : kinds->asArray()) if (k.isString()) requestedKinds.push_back(k.asString());
  }
  auto wantKind = [&](const std::string& k) {
    if (requestedKinds.empty()) return true;
    for (auto& r : requestedKinds) if (r == k) return true;
    return false;
  };
  auto hasSourceFixAll = [&](const std::string& k) {
    if (requestedKinds.empty()) return true;
    for (auto& r : requestedKinds) if (r == "source.fixAll" || r == "source.fixAll.a" || r == k) return true;
    return false;
  };
  if (!wantKind("quickfix")) { reply(id, std::move(actions)); return; }
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (file && tokens && reqLine > 0) {
    // Unused variable (W0001): offer prefix with `_`.
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine || d.code != "W0001") continue;
      const Token* tok = tokenAt(*tokens, d.span.line, d.span.col);
      if (!tok || tok->kind != Tok::Ident) continue;
      std::string newName = "_" + tok->lexeme;
      Value action = Value::object();
      action["title"] = "Prefix with '_': " + newName;
      action["kind"] = "quickfix";
      Value edit = Value::object();
      edit["range"] = rangeValue(document->text, tok->span);
      edit["newText"] = newName;
      Value changes = Value::object();
      Value eps = Value::array(); eps.push(std::move(edit));
      changes[document->uri] = std::move(eps);
      Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
      action["edit"] = std::move(wsEdit);
      action["diagnostics"] = Value::array();
      actions.push(std::move(action));
      break;
    }
    // Any diagnostic: offer "Suppress with '# a: ignore[CODE]'".
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine) continue;
      std::string tail = d.code.empty() ? " # a: ignore" : " # a: ignore[" + d.code + "]";
      Value action = Value::object();
      action["title"] = "Suppress" + (d.code.empty() ? std::string() : " " + d.code) + " on this line";
      action["kind"] = "quickfix";
      std::string lt = lineText(document->text, reqLine);
      Value edit = Value::object();
      Value insertPos = Value::object();
      insertPos["line"] = reqLine - 1;
      insertPos["character"] = byteToUtf16Column(lt, (int)lt.size());
      edit["range"] = rangeValue(document->text, Span{reqLine, (int)lt.size() + 1, reqLine, (int)lt.size() + 1, 0});
      edit["newText"] = tail;
      Value changes = Value::object();
      Value eps = Value::array(); eps.push(std::move(edit));
      changes[document->uri] = std::move(eps);
      Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
      action["edit"] = std::move(wsEdit);
      actions.push(std::move(action));
      break;
    }
  }
  if (hasSourceFixAll("source.fixAll")) {
    // No bulk fix available yet; return what we have.
  }
  reply(id, std::move(actions));
}

// Selection range: identifier -> enclosing indent block -> next block.
void LanguageServer::selectionRanges(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  Value result = Value::array();
  if (!document || !document->compilation || !params) { reply(id, std::move(result)); return; }
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) { reply(id, std::move(result)); return; }
  std::vector<std::pair<int,int>> positions;
  const Value* posArr = params->find("positions");
  if (posArr && posArr->isArray()) {
    for (auto& p : posArr->asArray()) {
      int l = static_cast<int>(p.find("line") ? p.find("line")->asInt() : 0);
      int c = static_cast<int>(p.find("character") ? p.find("character")->asInt() : 0);
      positions.emplace_back(l, c);
    }
  }
  for (auto& [line0, char0] : positions) {
    int line = line0 + 1;
    int byteCol = utf16ToByteColumn(lineText(document->text, line), char0) + 1;
    Value entry = Value::object();
    Value current = Value::object();
    const Token* tok = tokenAt(*tokens, line, byteCol);
    if (tok) current["range"] = rangeValue(document->text, tok->span);
    else { Value r = Value::object(); r["start"] = positionValue(document->text, Span{line, byteCol, line, byteCol, 0}); r["end"] = r["start"]; current["range"] = std::move(r); }
    // Outward: expand to the indent block containing the line.
    int indent = 0;
    std::string lt = lineText(document->text, line);
    while (indent < (int)lt.size() && lt[indent] == ' ') ++indent;
    int blockStartLine = line;
    while (blockStartLine > 1) {
      std::string prev = lineText(document->text, blockStartLine - 1);
      int pi = 0; while (pi < (int)prev.size() && prev[pi] == ' ') ++pi;
      if (pi >= indent && !prev.empty()) --blockStartLine;
      else break;
    }
    int blockEndLine = line;
    int totalLines = 0; for (char c : document->text) if (c == '\n') ++totalLines;
    while (blockEndLine <= totalLines) {
      std::string next = lineText(document->text, blockEndLine + 1);
      if (next.empty()) break;
      int ni = 0; while (ni < (int)next.size() && next[ni] == ' ') ++ni;
      if (ni > indent) ++blockEndLine;
      else break;
    }
    Value ranges = Value::array();
    ranges.push(current);
    Value outer = Value::object();
    Span blockSpan{blockStartLine, 1, blockEndLine, (int)lineText(document->text, blockEndLine).size() + 1, 0};
    outer["range"] = rangeValue(document->text, blockSpan);
    Value outerArr = Value::array(); outerArr.push(std::move(outer));
    current["parent"] = std::move(outerArr);
    entry["ranges"] = std::move(ranges);
    result.push(std::move(entry));
  }
  reply(id, std::move(result));
}

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

void LanguageServer::rangeFormatting(const Value& id, const Value* params) {
  Value edits = Value::array();
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) {
    reply(id, std::move(edits));
    return;
  }
  const Value* rangeVal = params->find("range");
  if (!rangeVal) {
    reply(id, std::move(edits));
    return;
  }
  const Value* reqStart = rangeVal->find("start");
  const Value* reqEnd = rangeVal->find("end");
  if (!reqStart || !reqEnd) {
    reply(id, std::move(edits));
    return;
  }
  const int reqStartLine = static_cast<int>(reqStart->find("line")
                                                 ? reqStart->find("line")->asInt()
                                                 : 0);
  const int reqStartChar = static_cast<int>(reqStart->find("character")
                                                ? reqStart->find("character")->asInt()
                                                : 0);
  const int reqEndLine = static_cast<int>(
      reqEnd->find("line") ? reqEnd->find("line")->asInt() : 0);
  const int reqEndChar = static_cast<int>(reqEnd->find("character")
                                              ? reqEnd->find("character")->asInt()
                                              : 0);

  a::tooling::FormatOptions options;
  if (const Value* formatOptions = params->find("options")) {
    if (const Value* tabSize = formatOptions->find("tabSize");
        tabSize && tabSize->isNumber()) {
      const int size = static_cast<int>(tabSize->asInt());
      if (size >= 1 && size <= 16) options.indentWidth = size;
    }
  }

  a::DiagnosticSink sink;
  sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
  a::tooling::FormatResult result =
      a::tooling::formatSource(document->text, sink, options);
  if (!result.ok || sink.hasErrors() || result.text == document->text) {
    reply(id, std::move(edits));
    return;
  }

  const std::vector<std::string> oldLines = splitLinesKeep(document->text);
  const std::vector<std::string> newLines = splitLinesKeep(result.text);
  const int lineCount = static_cast<int>(oldLines.size());
  auto endOfFilePos = [&]() {
    Value pos = Value::object();
    pos["line"] = lineCount - 1;
    pos["character"] =
        byteToUtf16Column(oldLines.back(), static_cast<int>(oldLines.back().size()));
    return pos;
  };
  auto insideRange = [&](int startLine, int startChar, int endLine, int endChar) {
    const bool afterStart = startLine > reqStartLine ||
                            (startLine == reqStartLine && startChar >= reqStartChar);
    const bool beforeEnd = endLine < reqEndLine ||
                           (endLine == reqEndLine && endChar <= reqEndChar);
    return afterStart && beforeEnd;
  };

  for (const auto& hunk : diffLineHunks(oldLines, newLines)) {
    const int oldStart = hunk[0];
    const int oldEnd = hunk[1];
    // Edit range: start of oldStart line through start of oldEnd line (or
    // EOF when the hunk reaches the last line).
    Value edit = Value::object();
    Value range = Value::object();
    Value start = Value::object();
    if (oldStart < lineCount) {
      start["line"] = oldStart;
      start["character"] = 0;
    } else {
      start = endOfFilePos();
    }
    Value end = Value::object();
    int endLine = 0;
    int endChar = 0;
    if (oldEnd < lineCount) {
      endLine = oldEnd;
      endChar = 0;
    } else {
      end = endOfFilePos();
    }
    end["line"] = endLine;
    end["character"] = endChar;
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    if (!insideRange(static_cast<int>(range["start"]["line"].asInt()),
                     static_cast<int>(range["start"]["character"].asInt()),
                     static_cast<int>(range["end"]["line"].asInt()),
                     static_cast<int>(range["end"]["character"].asInt()))) {
      continue;
    }
    std::string newText;
    for (int i = hunk[2]; i < hunk[3]; ++i) newText += newLines[static_cast<std::size_t>(i)];
    edit["range"] = std::move(range);
    edit["newText"] = std::move(newText);
    edits.push(std::move(edit));
  }
  reply(id, std::move(edits));
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
