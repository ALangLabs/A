// LSP core: the run loop, method dispatch, initialize capabilities, and
// workspace configuration. Feature methods live in handlers_*.cpp; the
// document store in documents.cpp; transport in transport.cpp.

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
#include <iostream>
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

int LanguageServer::run() {
  startWorker();
  std::string body;
  while (readMessage(std::cin, body)) {
    std::string error;
    Value message = jsonlite::parse(body, &error);
    if (!message.isObject()) {
      sendError(Value(), -32700, "parse error: " + error);
      continue;
    }
    if (!handle(message)) break;
  }
  stopWorker();
  return shutdownReceived_ ? 0 : 1;
}

bool LanguageServer::handle(const Value& message) {
  const Value* methodValue = message.find("method");
  const Value* idValue = message.find("id");
  const Value* params = message.find("params");
  const std::string method = methodValue && methodValue->isString() ? methodValue->asString() : "";
  const bool isRequest = idValue != nullptr;

  if (method == "initialize") {
    handleInitialize(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "initialized") return true;
  if (method == "shutdown") {
    shutdownReceived_ = true;
    replyNull(idValue ? *idValue : Value());
    return true;
  }
  if (method == "exit") return false;
  if (method == "textDocument/didOpen") {
    didOpen(params);
    return true;
  }
  if (method == "textDocument/didChange") {
    didChange(params);
    return true;
  }
  if (method == "textDocument/didSave") {
    didSave(params);
    return true;
  }
  if (method == "textDocument/didClose") {
    didClose(params);
    return true;
  }
  if (method == "workspace/didChangeConfiguration") {
    handleConfiguration(params);
    return true;
  }
  if (method == "$/setTrace" || method == "$/cancelRequest" || method == "workspace/didChangeWatchedFiles") {
    return true;
  }

  // Async checking: notifications enqueue background compiles (200ms
  // debounce); requests that need the index synchronously bring the
  // document up to date on demand (stale = just-edited, worker pending).
  static const std::unordered_set<std::string> kNeedsCompilation = {
      "textDocument/definition", "textDocument/hover", "textDocument/completion",
      "textDocument/signatureHelp", "textDocument/references", "textDocument/documentSymbol",
      "textDocument/formatting", "textDocument/rename", "textDocument/documentHighlight",
      "textDocument/foldingRange", "textDocument/prepareRename",
      "textDocument/semanticTokens/full", "textDocument/semanticTokens/range",
      "textDocument/inlayHint", "textDocument/codeAction", "textDocument/selectionRange",
      "textDocument/declaration", "textDocument/typeDefinition",
      "textDocument/implementation", "textDocument/prepareCallHierarchy",
      "textDocument/prepareTypeHierarchy", "textDocument/codeLens",
      "textDocument/rangeFormatting", "textDocument/documentLink",
      "textDocument/diagnostic",
  };
  if (kNeedsCompilation.count(method)) {
    if (Document* document = documentFrom(params)) ensureCompiled(document);
  } else if (method == "workspace/symbol") {
    for (auto& entry : documents_) ensureCompiled(&entry.second);
  }

  if (method == "textDocument/definition") {
    definition(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/hover") {
    hover(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/completion") {
    completion(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "completionItem/resolve") {
    completionResolve(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/signatureHelp") {
    signatureHelp(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/references") {
    references(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/documentSymbol") {
    documentSymbols(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/formatting") {
    formatting(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/rename") {
    rename(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/documentHighlight") {
    documentHighlights(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/foldingRange") {
    foldingRanges(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "workspace/symbol") {
    workspaceSymbols(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/prepareRename") {
    prepareRename(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/semanticTokens/full" ||
      method == "textDocument/semanticTokens/range") {
    semanticTokens(idValue ? *idValue : Value(), params,
                   method == "textDocument/semanticTokens/range");
    return true;
  }
  if (method == "textDocument/inlayHint") {
    inlayHints(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/codeAction") {
    codeActions(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/selectionRange") {
    selectionRanges(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/declaration" || method == "textDocument/typeDefinition" ||
      method == "textDocument/implementation") {
    definition(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/prepareCallHierarchy") {
    prepareCallHierarchy(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "callHierarchy/incomingCalls") {
    incomingCalls(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "callHierarchy/outgoingCalls") {
    outgoingCalls(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/prepareTypeHierarchy") {
    prepareTypeHierarchy(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "typeHierarchy/supertypes") {
    typeHierarchyRelated(idValue ? *idValue : Value(), params, true);
    return true;
  }
  if (method == "typeHierarchy/subtypes") {
    typeHierarchyRelated(idValue ? *idValue : Value(), params, false);
    return true;
  }
  if (method == "textDocument/codeLens") {
    codeLens(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/rangeFormatting") {
    rangeFormatting(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/diagnostic") {
    documentDiagnostic(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/moniker") {
    moniker(idValue ? *idValue : Value(), params);
    return true;
  }
  if (method == "textDocument/documentLink") {
    documentLinks(idValue ? *idValue : Value(), params);
    return true;
  }

  if (isRequest) {
    sendError(*idValue, -32601, "method not found: " + method);
  }
  return true;
}

void LanguageServer::handleInitialize(const Value& id, const Value* params) {
  modulePaths_.clear();
  std::string root;
  if (params) {
    if (const Value* options = params->find("initializationOptions")) {
      for (const std::string& path : readStringArray(options->find("modulePaths"))) {
        modulePaths_.push_back(path);
      }
      if (const Value* stdlib = options->find("stdlibRoot"); stdlib && stdlib->isString()) {
        stdlibRoot_ = stdlib->asString();
      }
    }
    if (const Value* folders = params->find("workspaceFolders"); folders && folders->isArray()) {
      for (const Value& folder : folders->asArray()) {
        if (const Value* uri = folder.find("uri"); uri && uri->isString()) {
          root = uriToPath(uri->asString());
          break;
        }
      }
    }
    if (root.empty()) {
      if (const Value* rootUri = params->find("rootUri"); rootUri && rootUri->isString()) {
        root = uriToPath(rootUri->asString());
      } else if (const Value* rootPath = params->find("rootPath");
                 rootPath && rootPath->isString()) {
        root = rootPath->asString();
      }
    }
  }
  if (!root.empty()) {
    modulePaths_.insert(modulePaths_.begin(), root);
    std::error_code error;
    fs::path build = fs::path(root) / "build";
    if (fs::is_directory(build, error)) modulePaths_.push_back(build.string());
  }

  Value capabilities = Value::object();
  capabilities["positionEncoding"] = "utf-16";
  Value sync = Value::object();
  sync["openClose"] = true;
  sync["change"] = 2;  // incremental sync (ty/pyrefly do the same)
  Value save = Value::object();
  save["includeText"] = true;
  sync["save"] = std::move(save);
  capabilities["textDocumentSync"] = std::move(sync);
  capabilities["definitionProvider"] = true;
  capabilities["declarationProvider"] = true;
  capabilities["typeDefinitionProvider"] = true;
  capabilities["implementationProvider"] = true;
  capabilities["referencesProvider"] = true;
  Value hover = Value::object();
  hover["workDoneProgress"] = false;
  capabilities["hoverProvider"] = std::move(hover);
  capabilities["documentSymbolProvider"] = true;
  capabilities["workspaceSymbolProvider"] = true;
  capabilities["documentFormattingProvider"] = true;
  capabilities["documentHighlightProvider"] = true;
  capabilities["foldingRangeProvider"] = true;
  capabilities["selectionRangeProvider"] = true;
  capabilities["inlayHintProvider"] = true;
  Value rename = Value::object();
  rename["prepareProvider"] = true;
  capabilities["renameProvider"] = std::move(rename);
  Value completion = Value::object();
  Value triggers = Value::array();
  triggers.push(".");
  triggers.push("\"");
  triggers.push(":");
  completion["triggerCharacters"] = std::move(triggers);
  completion["resolveProvider"] = true;
  capabilities["completionProvider"] = std::move(completion);
  Value signatureHelp = Value::object();
  Value signatureTriggers = Value::array();
  signatureTriggers.push("(");
  signatureTriggers.push(",");
  Value retrig = Value::array();
  retrig.push("(");
  retrig.push(",");
  signatureHelp["triggerCharacters"] = std::move(signatureTriggers);
  signatureHelp["retriggerCharacters"] = std::move(retrig);
  capabilities["signatureHelpProvider"] = std::move(signatureHelp);
  // Semantic tokens: legend [namespace,type,class,enum,function,method,
  // parameter,variable,property,keyword,comment,string,number,operator].
  Value tokenLegend = Value::object();
  Value tokenTypes = Value::array();
  for (const char* t : {"namespace", "type", "class", "enum", "function", "method",
                        "parameter", "variable", "property", "keyword", "comment",
                        "string", "number", "operator"}) tokenTypes.push(t);
  tokenLegend["tokenTypes"] = std::move(tokenTypes);
  Value tokenModifiers = Value::array();
  tokenModifiers.push("declaration");
  tokenModifiers.push("readonly");
  tokenModifiers.push("static");
  tokenLegend["tokenModifiers"] = std::move(tokenModifiers);
  Value semanticTokens = Value::object();
  semanticTokens["legend"] = std::move(tokenLegend);
  semanticTokens["full"] = true;
  semanticTokens["range"] = true;
  capabilities["semanticTokensProvider"] = std::move(semanticTokens);
  // Code actions: quick fixes and source fixes.
  Value codeAction = Value::object();
  Value actionKinds = Value::array();
  actionKinds.push("quickfix");
  actionKinds.push("refactor");
  actionKinds.push("source.fixAll");
  actionKinds.push("source.organizeImports");
  codeAction["codeActionKinds"] = std::move(actionKinds);
  capabilities["codeActionProvider"] = std::move(codeAction);
  Value callHierarchy = Value::object();
  callHierarchy["prepareProvider"] = true;
  capabilities["callHierarchyProvider"] = std::move(callHierarchy);
  Value typeHierarchy = Value::object();
  typeHierarchy["prepareProvider"] = true;
  capabilities["typeHierarchyProvider"] = std::move(typeHierarchy);
  Value codeLens = Value::object();
  codeLens["resolveProvider"] = false;
  capabilities["codeLensProvider"] = std::move(codeLens);
  capabilities["documentRangeFormattingProvider"] = true;
  Value diagnosticProvider = Value::object();
  diagnosticProvider["identifier"] = "a";
  diagnosticProvider["interFileDependencies"] = true;
  diagnosticProvider["workspaceDiagnostics"] = false;
  capabilities["diagnosticProvider"] = std::move(diagnosticProvider);
  capabilities["monikerProvider"] = true;
  Value documentLink = Value::object();
  documentLink["resolveProvider"] = false;
  capabilities["documentLinkProvider"] = std::move(documentLink);

  Value result = Value::object();
  result["capabilities"] = std::move(capabilities);
  Value info = Value::object();
  info["name"] = "a-language-server";
  info["version"] = kServerVersion;
  result["serverInfo"] = std::move(info);
  reply(id, std::move(result));
}

void LanguageServer::handleConfiguration(const Value* params) {
  if (!params) return;
  const Value* settings = params->find("settings");
  if (!settings) return;
  const Value* aSettings = settings->find("a");
  if (!aSettings) return;
  if (const Value* paths = aSettings->find("modulePaths")) {
    modulePaths_ = readStringArray(paths);
  }
  if (const Value* stdlib = aSettings->find("stdlibRoot"); stdlib && stdlib->isString()) {
    stdlibRoot_ = stdlib->asString();
  }
  for (auto& entry : documents_) enqueueCompile(entry.second);
}

}  // namespace a::lsp
