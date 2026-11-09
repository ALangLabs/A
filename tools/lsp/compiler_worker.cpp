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

namespace {

// Mirrors the CLI's apm auto-discovery (findProjectDir in
// cli/main.cpp): walks up from the document looking for an apm.json and
// returns its directory ("" when there is none), so bare `import <pkg>`
// resolves into the project's installed packages just like `a run`.
std::string findProjectDir(const std::string& filePath) {
  std::error_code ec;
  fs::path dir = fs::path(filePath).parent_path();
  if (dir.empty()) return {};
  for (fs::path d = fs::absolute(dir, ec); !d.empty(); d = d.parent_path()) {
    if (fs::is_regular_file(d / "apm.json", ec)) return d.string();
    if (d == d.parent_path()) break;
  }
  return {};
}

// The module search paths used to analyze a document: `basePaths` with
// the document's apm project packages dir prepended.
std::vector<std::string> modulePathsFor(const std::string& filePath,
                                        const std::vector<std::string>& basePaths) {
  std::vector<std::string> paths = basePaths;
  const std::string root = findProjectDir(filePath);
  if (!root.empty()) {
    std::error_code ec;
    const fs::path pkgDir = fs::path(root) / ".apm" / "packages";
    if (fs::is_directory(pkgDir, ec)) {
      std::string s = pkgDir.string();
      if (std::find(paths.begin(), paths.end(), s) == paths.end()) {
        paths.insert(paths.begin(), s);
      }
    }
  }
  return paths;
}

}  // namespace

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
    auto compilation =
        a::lsp::analyzeDocument(job.path, job.text, modulePathsFor(job.path, job.modulePaths),
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
                                                   modulePathsFor(document->path, modulePaths_),
                                                   stdlibRoot_);
  document->compiledVersion = document->version;
}

// Legacy synchronous path (no async publish). Handlers now use
// ensureCompiled() which reuses the worker's debounce instead.
void LanguageServer::recompile(Document& document) {
  document.compilation = a::lsp::analyzeDocument(document.path, document.text,
                                                  modulePathsFor(document.path, modulePaths_),
                                                  stdlibRoot_);
  document.compiledVersion = document.version;
  log("compiled " + document.path + " (sync, errors: " +
      std::to_string(document.compilation ? document.compilation->sink.errorCount() : 0) + ")");
  publishDiagnosticsFor(document);
}

}  // namespace a::lsp
