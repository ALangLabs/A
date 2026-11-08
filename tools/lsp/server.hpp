#pragma once

// LanguageServer: JSON-RPC / LSP core. The class declaration lives here so
// handler implementations can be split across translation units:
//
//   server.cpp               - run loop, dispatch, initialize/configuration
//   transport.cpp            - stdio framing, replies/notifications
//   documents.cpp            - open-document store, didOpen/didChange/..., tokens
//   compiler_worker.cpp      - debounced background type checking
//   diagnostics.cpp          - publishDiagnostics / pull diagnostics
//   handlers_navigation.cpp  - definition, references, rename, highlights
//   handlers_format.cpp     - formatting, folding/range formatting
//   handlers_symbols.cpp    - hover, document/workspace symbols
//   handlers_completion.cpp - completion, signature help
//   handlers_semantic.cpp   - semantic tokens, hints, actions, lens, links
//   handlers_hierarchy.cpp   - call hierarchy and type hierarchy
//   main.cpp                 - entry point (--version/--help, stdio setup)
//
// Shared helpers live in text_utils.hpp (paths, UTF-16, LSP ranges) and
// docs.hpp (signatures, doc rendering, builtin/method tables).

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "analysis.hpp"
#include "json.hpp"

namespace a::lsp {

using jsonlite::Value;

inline constexpr const char* kServerVersion = "0.0.1";

struct Document {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
  int compiledVersion = -1;
  std::unique_ptr<Compilation> compilation;
};

class LanguageServer {
 public:
  LanguageServer() = default;

  int run();

 private:
  // ---- background (async) type checking ---------------------------------
  // didOpen/didChange only snapshot text and enqueue; a worker thread runs
  // analyzeDocument (Sema) and publishes diagnostics. Requests that need a
  // compilation call ensureCompiled() first, which synchronously fills any
  // staleness left by the debounce (like ty/pyrefly: async diagnostics,
  // request-time analysis for completeness).
  struct OpenDocSnapshot {
    std::string uri;
    std::string path;
    int version = 0;
    std::string text;
  };
  struct CompileJob {
    std::string uri;
    std::string path;
    std::string text;
    int version = 0;
    uint64_t generation = 0;
    std::vector<std::string> modulePaths;
    std::string stdlibRoot;
    std::vector<OpenDocSnapshot> open;
  };

  static constexpr int kDebounceMs = 200;

  using TypeDeclPair = std::pair<const Compilation*, a::lsp::TypeDeclInfo>;

  bool logging() const;
  void log(const std::string& message);
  void reply(const Value& id, Value result);
  void replyNull(const Value& id);
  void sendError(const Value& id, int code, const std::string& message);
  void notify(const std::string& method, Value params);
  static bool readMessage(std::istream& input, std::string& body);
  void writeMessage(const Value& message);
  bool handle(const Value& message);
  void handleInitialize(const Value& id, const Value* params);
  void handleConfiguration(const Value* params);
  void startWorker();
  void stopWorker();
  void enqueueCompile(Document& document);
  void workerLoop();
  void ensureCompiled(Document* document);
  Document* documentFor(const std::string& uri);
  std::optional<std::pair<int, int>> positionFrom(const Value* params, int* lineOut,
                                                  int* characterOut);
  Document* documentFrom(const Value* params);
  void didOpen(const Value* params);
  void didChange(const Value* params);
  void didSave(const Value* params);
  void didClose(const Value* params);
  void recompile(Document& document);
  void publishDiagnosticsFor(const Document& document);
  void publishDiagnostics(const Document& document, const std::vector<Value>& diagnostics);
  const Token* tokenAt(const std::vector<Token>& tokens, int line, int byteColumn) const;
  int tokenIndexAt(const std::vector<Token>& tokens, int line, int byteColumn) const;
  int tokenIndexBefore(const std::vector<Token>& tokens, int line, int byteColumn) const;
  std::string extractDocstringAbove(const std::string& source, int declLine) const;
  std::string extractDocstring(const std::string& source, int startLine) const;
  std::string docCommentText(const Compilation& compilation, const FileIndex& file,
                             const Span& span) const;
  static bool validIdentifier(const std::string& name);
  std::vector<std::pair<std::string, Span>> collectReferences(
      const Compilation& compilation, const ResolvedSymbol& target) const;
  Value fullDocumentRange(const std::string& text) const;
  void formatting(const Value& id, const Value* params);
  void rename(const Value& id, const Value* params);
  void documentHighlights(const Value& id, const Value* params);
  void foldingRanges(const Value& id, const Value* params);
  std::string sourceTreeRoot(const std::string& filePath);
  std::string readSourceFile(const std::string& path);
  static Value lineLocation(const std::string& path, int oneBasedLine);
  static int docEntryLine(const std::string& text, const std::string& name);
  Value builtinDocLocation(const std::string& documentPath, const std::string& builtinName);
  Value methodDocLocation(const std::string& documentPath, const std::string& typeName,
                          const std::string& methodName);
  void definition(const Value& id, const Value* params);
  void hover(const Value& id, const Value* params);
  void references(const Value& id, const Value* params);
  Value documentSymbolFromStatement(const Compilation& compilation, const std::string& text,
                                    const a::Stmt& statement);
  void outlineBlockSymbols(const Compilation& compilation, const std::string& text,
                           const std::vector<a::StmtPtr>& body, Value& out);
  void outlineStmtSymbol(const Compilation& compilation, const std::string& text,
                         const a::Stmt& stmt, Value& out);
  void outlineExprSymbols(const Compilation& compilation, const std::string& text,
                          const a::Expr* expr, Value& out);
  void documentSymbols(const Value& id, const Value* params);
  void workspaceSymbols(const Value& id, const Value* params);
  void completion(const Value& id, const Value* params);
  void completionResolve(const Value& id, const Value* params);
  void signatureHelp(const Value& id, const Value* params);
  void prepareRename(const Value& id, const Value* params);
  void semanticTokens(const Value& id, const Value* params, bool rangeOnly);
  void inlayHints(const Value& id, const Value* params);
  void codeActions(const Value& id, const Value* params);
  void selectionRanges(const Value& id, const Value* params);
  std::string modulePathForId(const Compilation& compilation, int moduleId) const;
  Value itemDataFor(const std::string& path, int line, int col) const;
  bool itemTargetFromData(const Value* item, std::string* path, int* line, int* col) const;
  Value callItemForSymbol(const Compilation& compilation, const ResolvedSymbol& resolved) const;
  Value callItemForSite(const Compilation& compilation, const a::lsp::CallSite& site) const;
  Value callItemForFnDecl(const Compilation& compilation, const a::FnDecl& fn,
                          const std::string& path, int kind) const;
  void prepareCallHierarchy(const Value& id, const Value* params);
  void incomingCalls(const Value& id, const Value* params);
  void outgoingCalls(const Value& id, const Value* params);
  Value typeItemForDecl(const Compilation& compilation,
                        const a::lsp::TypeDeclInfo& decl) const;
  std::vector<TypeDeclPair> allTypeDecls();
  const TypeDeclPair* findTypeDecl(const std::vector<TypeDeclPair>& decls,
                                   const std::string& path, int line, int col) const;
  const TypeDeclPair* findTypeDeclByName(const std::vector<TypeDeclPair>& decls,
                                         const std::string& name) const;
  void prepareTypeHierarchy(const Value& id, const Value* params);
  void typeHierarchyRelated(const Value& id, const Value* params, bool supertypes);
  std::vector<std::string> paramNamesForSymbol(const Compilation& compilation,
                                               const ResolvedSymbol& resolved) const;
  void codeLens(const Value& id, const Value* params);
  void rangeFormatting(const Value& id, const Value* params);
  void documentDiagnostic(const Value& id, const Value* params);
  void moniker(const Value& id, const Value* params);
  void documentLinks(const Value& id, const Value* params);

  std::map<std::string, Document> documents_;
  std::vector<std::string> modulePaths_;
  std::string stdlibRoot_;
  bool shutdownReceived_ = false;

  // Async-checking worker state (debounced, latest-wins).
  std::mutex outMutex_;
  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::optional<CompileJob> pendingJob_;
  uint64_t generation_ = 0;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace a::lsp
