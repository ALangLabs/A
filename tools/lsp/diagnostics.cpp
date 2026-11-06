// Diagnostic -> LSP conversion (see diagnostics.hpp). One mapping shared by
// the async worker publish, the synchronous publish path, and pull
// diagnostics, so code/tags/relatedInformation stay identical everywhere.

#include "diagnostics.hpp"

#include "server.hpp"
#include "text_utils.hpp"

#include <string>
#include <vector>

namespace a::lsp {

using a::Diagnostic;
using a::Severity;

Value makeDiagnosticItem(const Compilation& compilation, const Diagnostic& diagnostic,
                         const std::string& rangeText, const std::string& locationPath) {
  Value item = Value::object();
  item["range"] = rangeValue(rangeText, diagnostic.span);
  item["severity"] = diagnostic.severity == Severity::Error ? 1 : 2;
  item["message"] = diagnostic.message;
  item["source"] = "a";
  if (!diagnostic.code.empty()) {
    item["code"] = diagnostic.code;
    Value desc = Value::object();
    desc["href"] = "https://github.com/search?q=" + diagnostic.code + "+repo:own/a-lang";
    item["codeDescription"] = std::move(desc);
  }
  Value tags = Value::array();
  if (diagnostic.code == "W0002") tags.push(1);  // Unnecessary
  if (!tags.asArray().empty()) item["tags"] = std::move(tags);
  if (!diagnostic.help.empty()) {
    Value related = Value::array();
    Value info = Value::object();
    info["location"] = locationValue(compilation, locationPath, diagnostic.span);
    info["message"] = diagnostic.help;
    related.push(std::move(info));
    item["relatedInformation"] = std::move(related);
  }
  return item;
}

std::vector<const Diagnostic*> diagnosticsForPath(const Compilation& compilation,
                                                  const std::string& path) {
  std::vector<const Diagnostic*> result;
  const std::string canonical = canonicalPath(path);
  for (const Diagnostic& diagnostic : compilation.sink.diagnostics()) {
    if (!diagnostic.span.valid()) continue;
    const a::SourceFile* source = compilation.sink.file(diagnostic.span.fileId);
    if (!source) continue;
    if (canonicalPath(source->path()) != canonical) continue;
    result.push_back(&diagnostic);
  }
  return result;
}

namespace {

std::string rangeTextFor(const Compilation& compilation, const Diagnostic& diagnostic,
                         const std::string& fallback) {
  const a::SourceFile* source = compilation.sink.file(diagnostic.span.fileId);
  return source ? source->text() : fallback;
}

}  // namespace

void LanguageServer::publishDiagnosticsFor(const Document& document) {
  if (!document.compilation) return;
  const Compilation& compilation = *document.compilation;

  for (auto& entry : documents_) {
    Document& open = entry.second;
    Value params = Value::object();
    params["uri"] = open.uri;
    if (open.version > 0) params["version"] = open.version;
    Value diagnostics = Value::array();
    for (const Diagnostic* diagnostic : diagnosticsForPath(compilation, open.path)) {
      diagnostics.push(makeDiagnosticItem(compilation, *diagnostic,
                                          rangeTextFor(compilation, *diagnostic, open.text),
                                          open.path));
    }
    params["diagnostics"] = std::move(diagnostics);
    notify("textDocument/publishDiagnostics", std::move(params));
  }
}

void LanguageServer::publishDiagnostics(const Document& document,
                                        const std::vector<Value>& diagnostics) {
  Value params = Value::object();
  params["uri"] = document.uri;
  if (document.version > 0) params["version"] = document.version;
  Value array = Value::array();
  for (const Value& item : diagnostics) array.push(item);
  params["diagnostics"] = std::move(array);
  notify("textDocument/publishDiagnostics", std::move(params));
}

void LanguageServer::documentDiagnostic(const Value& id, const Value* params) {
  Value result = Value::object();
  result["kind"] = "full";
  Value items = Value::array();
  Document* document = documentFrom(params);
  if (document && document->compilation) {
    const Compilation& compilation = *document->compilation;
    for (const Diagnostic* diagnostic : diagnosticsForPath(compilation, document->path)) {
      items.push(makeDiagnosticItem(compilation, *diagnostic,
                                    rangeTextFor(compilation, *diagnostic, document->text),
                                    document->path));
    }
  }
  result["items"] = std::move(items);
  reply(id, std::move(result));
}

}  // namespace a::lsp
