#pragma once

// Deterministic, on-the-fly documentation for symbols that have no
// docstring. Where Python's hover shows signature + docstring, A's hover
// falls back to a summary derived from the declaration's AST and Sema
// results: return type, mutated parameters, called functions, and notable
// body features (loops, `?` propagation, lambdas, ...). No docs need to be
// written or committed for hover/completion/signature-help to stay useful.

#include <string>

#include "analysis.hpp"

namespace a::lsp {

// Returns a markdown doc body synthesizing what can be inferred about the
// resolved symbol, or an empty string when the symbol kind carries no
// useful summary (variables already show their inferred type, ...).
// Callers should prefer a real docstring (docCommentText) and only use
// this as a fallback.
std::string synthesizeSymbolDoc(const Compilation& compilation,
                                const ResolvedSymbol& resolved);

}  // namespace a::lsp
