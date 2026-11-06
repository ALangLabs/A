// Background type checking: debounced, latest-wins worker that runs
// analyzeDocument off the request path and publishes diagnostics.
// Requests that need the index call ensureCompiled() on demand.

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
#include <chrono>

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

void LanguageServer::startWorker() {
  if (worker_.joinable()) return;
  worker_ = std::thread([this] { workerLoop(); });
}

void LanguageServer::stopWorker() {
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    stop_ = true;
  }
  jobCv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void LanguageServer::enqueueCompile(Document& document) {
  CompileJob job;
  job.uri = document.uri;
  job.path = document.path;
  job.text = document.text;
  job.version = document.version;
  job.modulePaths = modulePaths_;
  job.stdlibRoot = stdlibRoot_;
  for (const auto& entry : documents_) {
    job.open.push_back(OpenDocSnapshot{entry.second.uri, entry.second.path,
                                       entry.second.version, entry.second.text});
  }
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    job.generation = ++generation_;
    pendingJob_ = std::move(job);
  }
  jobCv_.notify_all();
}

void LanguageServer::workerLoop() {
  while (!stop_) {
    CompileJob job;
    {
      std::unique_lock<std::mutex> lock(jobMutex_);
      if (!jobCv_.wait_for(lock, std::chrono::milliseconds(kDebounceMs),
                           [&] { return stop_ || pendingJob_.has_value(); })) {
        continue;  // debounce settled with nothing pending
      }
      if (stop_ && !pendingJob_.has_value()) return;
      if (!pendingJob_.has_value()) continue;
      job = std::move(*pendingJob_);
      pendingJob_.reset();
    }
    auto compilation = a::lsp::analyzeDocument(job.path, job.text, job.modulePaths,
                                                job.stdlibRoot);
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      if (job.generation != generation_) continue;  // stale; a newer edit won
    }
    // Publish diagnostics for every open doc (mirrors publishDiagnosticsFor
    // but from snapshots so the worker never touches Document state).
    for (const OpenDocSnapshot& open : job.open) {
      Value params = Value::object();
      params["uri"] = open.uri;
      if (open.version > 0) params["version"] = open.version;
      Value diagnostics = Value::array();
      for (const Diagnostic* diagnostic : diagnosticsForPath(*compilation, open.path)) {
        const a::SourceFile* source = compilation->sink.file(diagnostic->span.fileId);
        const std::string& text = source ? source->text() : open.text;
        diagnostics.push(makeDiagnosticItem(*compilation, *diagnostic, text, open.path));
      }
      params["diagnostics"] = std::move(diagnostics);
      notify("textDocument/publishDiagnostics", std::move(params));
    }
    log("compiled " + job.path + " (async, errors: " +
        std::to_string(compilation->sink.errorCount()) + ")");
  }
}

// Synchronously bring Document.compilation up to date on demand. Worker
// results are diagnostics-only; request handlers need the full index.
void LanguageServer::ensureCompiled(Document* document) {
  if (!document) return;
  if (document->compilation && document->compiledVersion == document->version) return;
  document->compilation = a::lsp::analyzeDocument(document->path, document->text,
                                                   modulePaths_, stdlibRoot_);
  document->compiledVersion = document->version;
}

// Legacy synchronous path (no async publish). Handlers now use
// ensureCompiled() which reuses the worker's debounce instead.
void LanguageServer::recompile(Document& document) {
  document.compilation = a::lsp::analyzeDocument(document.path, document.text, modulePaths_,
                                                  stdlibRoot_);
  document.compiledVersion = document.version;
  log("compiled " + document.path + " (sync, errors: " +
      std::to_string(document.compilation ? document.compilation->sink.errorCount() : 0) + ")");
  publishDiagnosticsFor(document);
}

}  // namespace a::lsp
