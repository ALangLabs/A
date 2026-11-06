#pragma once

// Diagnostic -> LSP conversion shared by the async worker publish, the
// synchronous publish path, and the LSP 3.17 pull-diagnostics handler.
// Keeping the mapping in one place means code, tags, and related
// information stay identical across all three.

#include <string>
#include <vector>

#include "analysis.hpp"
#include "json.hpp"

namespace a::lsp {

using a::Diagnostic;
using jsonlite::Value;

// One LSP Diagnostic item for `diagnostic`. `rangeText` is the source text
// the range is computed against (the diagnostic's own file); `locationPath`
// is the open document used for relatedInformation locations.
Value makeDiagnosticItem(const Compilation& compilation, const Diagnostic& diagnostic,
                         const std::string& rangeText, const std::string& locationPath);

// Diagnostics whose source file canonicalises to `path`.
std::vector<const Diagnostic*> diagnosticsForPath(const Compilation& compilation,
                                                  const std::string& path);

}  // namespace a::lsp
