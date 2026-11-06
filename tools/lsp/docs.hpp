#pragma once

// Documentation / signature helpers shared by hover, completion, and
// signature help. These read the same tables the type checker uses:
// builtinDocs() for interpreter builtins and the primitive method tables
// in method_signatures.hpp.

#include <string>
#include <vector>

#include "analysis.hpp"
#include "json.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/method_signatures.hpp"

namespace a::lsp {

using jsonlite::Value;

// Matches the lexer keyword set (compiler/astra/frontend/lexer/token.hpp Kw*
// tokens) so semantic tokens color exactly the real keywords.
inline constexpr const char* kKeywords[] = {
    "fn", "if", "elif", "else", "while", "do", "for", "in", "loop",
    "break", "continue", "return", "defer", "class", "metaclass", "enum",
    "self", "true", "false", "and", "or", "not", "import", "from", "as", "match",
    "None", "pass", "type", "trait", "impl", "comptime", "where",
};

const std::vector<a::MethodSig>* methodTableForType(const std::string& typeName);
const a::MethodSig* findMethodSig(const std::vector<a::MethodSig>& table,
                                  const std::string& name);

// Maps an import alias (`json`, or `import json as j` -> `j`) to the native
// module it refers to; empty when the alias is not a native module.
std::string nativeModuleForAlias(const a::lsp::FileIndex& file, const std::string& alias);

// Splits a signature string into its head, top-level parameters, and tail.
// For `fn name(a: i64, b: String) -> i64` this yields head `fn name`,
// params {`a: i64`, `b: String`}, tail ` -> i64`. Constants (`PI: f64`) and
// anything without parentheses return no parameters.
struct SignatureParts {
  std::string head;
  std::vector<std::string> params;
  std::string tail;
  bool callable = false;
};

SignatureParts splitSignature(const std::string& signature);

// Pyright-style layout: signatures with 3+ parameters or longer than ~48
// characters render as a multi-line block with one indented parameter per
// line, e.g.
//   fn print(
//       value
//   )
// Short signatures stay on one line.
std::string formatSignature(const std::string& signature);

// Renders a docstring body with structured sections (Args:/Parameters:,
// Returns:, Example:/Examples:, See Also:) as Pyright/Pylance-style markdown.
// Plain text without section headings is returned unchanged.
std::string renderDocSections(const std::string& documentation);

Value hoverMarkdown(const std::string& signature, const std::string& documentation,
                    const std::string& example = "", const std::string& seeAlso = "");

int completionKind(SymbolKind kind);
std::string simpleName(const std::string& name);

// Finds the symbol declared at `line`/`col` (1-based) in the module at `path`.
ResolvedSymbol findSymbolBySpan(const Compilation& compilation, const std::string& path,
                                int line, int col);

// Resolves a collected call site's callee token to its declaration, the same
// way resolveAt does for cursor positions.
ResolvedSymbol resolveCallCallee(const Compilation& compilation,
                                 const a::lsp::CallSite& site);

}  // namespace a::lsp
