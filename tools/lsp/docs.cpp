// Documentation helpers: signature formatting, doc-section rendering,
// hover markdown, and builtin/method table lookups. See docs.hpp.

#include "docs.hpp"

#include "text_utils.hpp"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

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

const std::vector<a::MethodSig>* methodTableForType(const std::string& typeName) {
  if (typeName == "array") return &a::arrayMethods();
  if (typeName == "map") return &a::mapMethods();
  if (typeName == "set") return &a::setMethods();
  if (typeName == "String") return &a::stringMethods();
  return nullptr;
}

const a::MethodSig* findMethodSig(const std::vector<a::MethodSig>& table,
                                  const std::string& name) {
  for (const a::MethodSig& method : table) {
    if (name == method.name) return &method;
  }
  return nullptr;
}

// Maps an import alias (`json`, or `import json as j` → `j`) to the native
// module it refers to; empty when the alias is not a native module.
std::string nativeModuleForAlias(const a::lsp::FileIndex& file, const std::string& alias) {
  for (const a::lsp::ImportDef& import : file.imports) {
    if (import.alias == alias && a::isNativeModule(import.modulePath)) return import.modulePath;
  }
  return {};
}

SignatureParts splitSignature(const std::string& signature) {
  SignatureParts parts;
  const std::size_t open = signature.find('(');
  if (open == std::string::npos) {
    parts.head = signature;
    return parts;
  }
  int depth = 0;
  std::size_t close = std::string::npos;
  for (std::size_t i = open; i < signature.size(); ++i) {
    const char c = signature[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) { close = i; break; }
    }
  }
  if (close == std::string::npos) {
    parts.head = signature;
    return parts;
  }
  parts.callable = true;
  parts.head = signature.substr(0, open);
  parts.tail = signature.substr(close + 1);
  const std::string inside = signature.substr(open + 1, close - open - 1);
  std::string current;
  depth = 0;
  for (char c : inside) {
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      parts.params.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  const std::string last = current;
  bool blank = true;
  for (char c : last) if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
  if (!blank) parts.params.push_back(last);
  for (std::string& param : parts.params) {
    const std::size_t begin = param.find_first_not_of(" \t");
    const std::size_t end = param.find_last_not_of(" \t\r\n");
    param = (begin == std::string::npos) ? "" : param.substr(begin, end - begin + 1);
  }
  return parts;
}

// Pyright-style layout: signatures with 3+ parameters or longer than ~48
// characters render as a multi-line block with one indented parameter per
// line, e.g.
//   fn print(
//       value
//   )
// Short signatures stay on one line.
std::string formatSignature(const std::string& signature) {
  const SignatureParts parts = splitSignature(signature);
  if (!parts.callable || parts.params.empty()) return signature;
  if (parts.params.size() < 3 && signature.size() <= 48) return signature;
  std::string formatted = parts.head + "(\n";
  for (std::size_t i = 0; i < parts.params.size(); ++i) {
    formatted += "    " + parts.params[i];
    if (i + 1 < parts.params.size()) formatted += ",";
    formatted += "\n";
  }
  formatted += ")" + parts.tail;
  return formatted;
}

namespace {

std::string lowerAscii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

}  // namespace

// Renders a docstring body with structured sections (Args:/Parameters:,
// Returns:, Example:/Examples:, See Also:) as Pyright/Pylance-style markdown.
// Plain text without section headings is returned unchanged.
std::string renderDocSections(const std::string& documentation) {
  if (documentation.empty()) return "";
  static const std::pair<const char*, const char*> kSections[] = {
      {"args:", "Args:"}, {"arguments:", "Args:"}, {"parameters:", "Parameters:"},
      {"params:", "Parameters:"}, {"returns:", "Returns:"}, {"return:", "Returns:"},
      {"example:", "Example:"}, {"examples:", "Example:"},
      {"see also:", "See Also:"}, {"note:", "Note:"},
  };
  std::vector<std::string> lines;
  {
    std::istringstream stream(documentation);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(trimLine(line));
  }
  auto headingFor = [](const std::string& line) -> const char* {
    const std::string lowered = lowerAscii(line);
    for (const auto& [prefix, label] : kSections) {
      if (lowered.rfind(prefix, 0) == 0) return label;
    }
    return nullptr;
  };
  bool anySection = false;
  for (const std::string& line : lines) if (headingFor(line)) anySection = true;
  if (!anySection) return documentation;

  const char* section = nullptr;
  bool exampleMode = false;
  std::string rendered;
  auto flushBlock = [&]() {
    if (!rendered.empty()) rendered += "\n";
  };
  for (std::string line : lines) {
    if (const char* label = headingFor(line)) {
      if (exampleMode) { rendered += "```\n"; exampleMode = false; }
      flushBlock();
      rendered += std::string("**") + label + "**";
      const std::size_t colon = line.find(':');
      const std::string rest = colon == std::string::npos ? "" : trimLine(line.substr(colon + 1));
      if (!rest.empty()) rendered += " " + rest;
      rendered += "\n";
      section = label;
      continue;
    }
    if (line.empty()) { rendered += "\n"; continue; }
    const bool isExample = section && std::string(section) == "Example:";
    if (isExample) {
      if (!exampleMode) { rendered += "```a\n"; exampleMode = true; }
      rendered += line + "\n";
      continue;
    }
    const bool isParams = section && (std::string(section) == "Args:" || std::string(section) == "Parameters:");
    if (isParams && line.rfind("- ", 0) != 0 && line.rfind("* ", 0) != 0) {
      rendered += "- " + line + "\n";
    } else {
      rendered += line + "\n";
    }
  }
  if (exampleMode) rendered += "```\n";
  while (!rendered.empty() && (rendered.back() == '\n' || rendered.back() == '\r')) rendered.pop_back();
  return rendered;
}

Value hoverMarkdown(const std::string& signature, const std::string& documentation,
                    const std::string& example, const std::string& seeAlso) {
  std::string markdown = "```a\n" + formatSignature(signature) + "\n```";
  const std::string body = renderDocSections(documentation);
  if (!body.empty()) markdown += "\n\n" + body;
  if (!example.empty()) {
    markdown += "\n\n**Example:**\n\n```a\n" + example + "\n```";
  }
  if (!seeAlso.empty()) {
    markdown += "\n\n**See Also:** ";
    bool first = true;
    std::size_t pos = 0;
    while (pos <= seeAlso.size()) {
      const std::size_t comma = seeAlso.find(',', pos);
      const std::string entry = trimLine(seeAlso.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
      if (!entry.empty()) {
        if (!first) markdown += ", ";
        markdown += "`" + entry + "`";
        first = false;
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  Value contents = Value::object();
  contents["kind"] = "markdown";
  contents["value"] = std::move(markdown);
  Value result = Value::object();
  result["contents"] = std::move(contents);
  return result;
}

int completionKind(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::Function: return 3;
    case SymbolKind::Method: return 2;
    case SymbolKind::Struct: return 22;
    case SymbolKind::Class: return 7;
    case SymbolKind::Enum: return 13;
    case SymbolKind::EnumMember: return 20;
    case SymbolKind::Field: return 5;
    case SymbolKind::Constant: return 21;
    case SymbolKind::Module: return 9;
    case SymbolKind::Interface: return 8;
    case SymbolKind::TypeParameter: return 25;
    default: return 6;
  }
}

std::string simpleName(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

// Finds the symbol declared at `line`/`col` (1-based) in the module at `path`.
ResolvedSymbol findSymbolBySpan(const Compilation& compilation, const std::string& path,
                                int line, int col) {
  const std::string canonical = canonicalPath(path);
  for (const FileIndex& file : compilation.indexes) {
    if (canonicalPath(file.path) != canonical) continue;
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.span.line == line && symbol.span.col == col) {
        return ResolvedSymbol{&symbol, &file};
      }
    }
  }
  return {};
}

// Resolves a collected call site's callee token to its declaration, the same
// way resolveAt does for cursor positions.
ResolvedSymbol resolveCallCallee(const Compilation& compilation,
                                 const a::lsp::CallSite& site) {
  if (site.moduleId < 0 ||
      site.moduleId >= static_cast<int>(compilation.tokensByModule.size())) {
    return {};
  }
  const FileIndex* file = compilation.indexForModule(site.moduleId);
  if (!file) return {};
  const std::vector<Token>& tokens =
      compilation.tokensByModule[static_cast<std::size_t>(site.moduleId)];
  for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
    const Token& token = tokens[static_cast<std::size_t>(i)];
    if (token.kind != Tok::Ident) continue;
    if (token.span.line != site.calleeSpan.line || token.span.col != site.calleeSpan.col ||
        token.span.endCol != site.calleeSpan.endCol) {
      continue;
    }
    return a::lsp::resolveToken(compilation, *file, tokens, i);
  }
  return {};
}

}  // namespace a::lsp
