// Open-document store and per-document utilities: didOpen/didChange/
// didSave/didClose, position/document lookup, token lookup, and
// docstring/doc-comment extraction.

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

Document* LanguageServer::documentFor(const std::string& uri) {
  auto it = documents_.find(uri);
  return it == documents_.end() ? nullptr : &it->second;
}

std::optional<std::pair<int, int>> LanguageServer::positionFrom(const Value* params, int* lineOut,
                                                int* characterOut) {
  if (!params) return std::nullopt;
  const Value* position = params->find("position");
  if (!position) return std::nullopt;
  const Value* line = position->find("line");
  const Value* character = position->find("character");
  if (!line || !character) return std::nullopt;
  *lineOut = static_cast<int>(line->asInt());
  *characterOut = static_cast<int>(character->asInt());
  return {{*lineOut, *characterOut}};
}

Document* LanguageServer::documentFrom(const Value* params) {
  if (!params) return nullptr;
  const Value* textDocument = params->find("textDocument");
  if (!textDocument) return nullptr;
  const Value* uri = textDocument->find("uri");
  if (!uri || !uri->isString()) return nullptr;
  return documentFor(uri->asString());
}

void LanguageServer::didOpen(const Value* params) {
  if (!params) return;
  const Value* textDocument = params->find("textDocument");
  if (!textDocument) return;
  const Value* uri = textDocument->find("uri");
  const Value* text = textDocument->find("text");
  if (!uri || !uri->isString() || !text || !text->isString()) return;

  Document document;
  document.uri = uri->asString();
  document.path = uriToPath(document.uri);
  document.text = text->asString();
  if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
    document.version = static_cast<int>(version->asInt());
  }
  log("open " + document.path);
  Document& stored = documents_[document.uri];
  stored = std::move(document);
  enqueueCompile(stored);
}

void LanguageServer::didChange(const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !params) return;
  const Value* textDocument = params->find("textDocument");
  if (textDocument) {
    if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
      document->version = static_cast<int>(version->asInt());
    }
  }
  const Value* changes = params->find("contentChanges");
  if (!changes || !changes->isArray() || changes->asArray().empty()) return;
  // Incremental sync (kind=2): apply each change's range splice in order.
  // A change without a range is a full-document replacement (kind=1).
  for (const Value& change : changes->asArray()) {
    const Value* text = change.find("text");
    if (!text || !text->isString()) continue;
    const Value* range = change.find("range");
    if (!range) {
      document->text = text->asString();
      continue;
    }
    const Value* start = range->find("start");
    const Value* end = range->find("end");
    if (!start || !end) { document->text = text->asString(); continue; }
    const Value* sl = start->find("line");
    const Value* sc = start->find("character");
    const Value* el = end->find("line");
    const Value* ec = end->find("character");
    if (!sl || !sc || !el || !ec) { document->text = text->asString(); continue; }
    // Convert (utf-16 line/character) to byte offsets.
    auto offsetOf = [&](int line0, int char0) -> std::size_t {
      std::string lineContent = lineText(document->text, line0 + 1);
      int byteCol0 = utf16ToByteColumn(lineContent, char0);
      std::size_t pos = 0;
      for (int l = 0; l < line0; ++l) {
        std::size_t nl = document->text.find('\n', pos);
        if (nl == std::string::npos) return document->text.size();
        pos = nl + 1;
      }
      return pos + static_cast<std::size_t>(byteCol0);
    };
    std::size_t s = offsetOf(static_cast<int>(sl->asInt()), static_cast<int>(sc->asInt()));
    std::size_t e = offsetOf(static_cast<int>(el->asInt()), static_cast<int>(ec->asInt()));
    if (s > document->text.size()) s = document->text.size();
    if (e > document->text.size()) e = document->text.size();
    if (e < s) std::swap(s, e);
    document->text.replace(s, e - s, text->asString());
  }
  enqueueCompile(*document);
}

void LanguageServer::didSave(const Value* params) {
  Document* document = documentFrom(params);
  if (!document) return;
  if (params) {
    const Value* text = params->find("text");
    if (text && text->isString()) document->text = text->asString();
  }
  enqueueCompile(*document);
}

void LanguageServer::didClose(const Value* params) {
  Document* document = documentFrom(params);
  if (!document) return;
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    ++generation_;  // cancel any pending compile for this document
    pendingJob_.reset();
  }
  publishDiagnostics(*document, {});
  documents_.erase(document->uri);
}

// Token lookup by (1-based line, 1-based byte column).
//
// Spans are half-open [col, endCol) (see lexer/span.hpp) and layout tokens
// (Newline, Indent, Dedent) are zero-width; the lexer attaches a Newline to
// the start of the following line (col 1..2) and Dedent tokens can sit at
// line starts too. If they were considered, hovering the first character
// of an identifier at line start (or a member right after '.') returned the
// preceding token and hover gave up -- hover failed on the most common
// stdlib calls (json.parse, math.sqrt, print at line start, ...). Skip
// layout tokens and use half-open containment so the point belongs to the
// token that starts there; matches resolveAt's identifier-only semantics.
const Token* LanguageServer::tokenAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
  for (const Token& token : tokens) {
    if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
        token.kind == Tok::Dedent) {
      continue;
    }
    if (token.span.line != line) continue;
    if (byteColumn < token.span.col || byteColumn >= token.span.endCol) continue;
    return &token;
  }
  return nullptr;
}

int LanguageServer::tokenIndexAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
        token.kind == Tok::Dedent) {
      continue;
    }
    if (token.span.line != line) continue;
    if (byteColumn < token.span.col || byteColumn >= token.span.endCol) continue;
    return static_cast<int>(i);
  }
  return -1;
}

int LanguageServer::tokenIndexBefore(const std::vector<Token>& tokens, int line, int byteColumn) const {
  int best = -1;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
        token.kind == Tok::Dedent) {
      continue;
    }
    if (token.span.line > line) break;
    if (token.span.line == line && token.span.col > byteColumn) break;
    best = static_cast<int>(i);
  }
  return best;
}

std::string LanguageServer::extractDocstringAbove(const std::string& source, int declLine) const {
  // Look backward from line immediately above declLine for closing """ or '''.
  std::vector<std::string> lines;
  {
    std::istringstream stream(source);
    std::string l;
    while (std::getline(stream, l)) lines.push_back(l);
  }
  int checkLine = declLine - 2; // 0-based index for line before declLine
  while (checkLine >= 0 && trimLine(lines[static_cast<std::size_t>(checkLine)]).empty()) {
    --checkLine;
  }
  if (checkLine < 0) return {};
  const std::string& endLineText = lines[static_cast<std::size_t>(checkLine)];
  std::string quoteType;
  std::size_t closePos = endLineText.rfind("\"\"\"");
  if (closePos != std::string::npos) {
    quoteType = "\"\"\"";
  } else {
    closePos = endLineText.rfind("'''");
    if (closePos != std::string::npos) quoteType = "'''";
  }
  if (quoteType.empty()) return {};

  // Check if it was a single-line docstring: """doc"""
  if (closePos >= 3) {
    std::size_t openPos = endLineText.rfind(quoteType, closePos - 3);
    if (openPos != std::string::npos) {
      return trimLine(endLineText.substr(openPos + 3, closePos - openPos - 3));
    }
  }

  // Multi-line: scan backwards for the opening quote
  std::vector<std::string> docLines;
  docLines.push_back(endLineText.substr(0, closePos));
  int startLineIdx = checkLine - 1;
  bool foundOpen = false;
  while (startLineIdx >= 0) {
    const std::string& cur = lines[static_cast<std::size_t>(startLineIdx)];
    std::size_t openPos = cur.find(quoteType);
    if (openPos != std::string::npos) {
      docLines.push_back(cur.substr(openPos + 3));
      foundOpen = true;
      break;
    }
    docLines.push_back(cur);
    --startLineIdx;
  }
  if (!foundOpen) return {};
  std::reverse(docLines.begin(), docLines.end());

  std::size_t minIndent = std::string::npos;
  for (std::size_t i = 1; i < docLines.size(); ++i) {
    const std::string& l = docLines[i];
    const std::size_t nb = l.find_first_not_of(" \t\r\n");
    if (nb != std::string::npos) {
      if (minIndent == std::string::npos || nb < minIndent) minIndent = nb;
    }
  }
  if (minIndent == std::string::npos) minIndent = 0;

  std::string result;
  for (std::size_t i = 0; i < docLines.size(); ++i) {
    std::string l = docLines[i];
    while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
    if (i > 0 && l.size() >= minIndent) l = l.substr(minIndent);
    if (i == 0 && trimLine(l).empty()) continue;
    if (!result.empty()) result += "\n";
    result += l;
  }
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
    result.pop_back();
  return result;
}

std::string LanguageServer::extractDocstring(const std::string& source, int startLine) const {
  // startLine is 1-based line of the declaration (or body).
  // Look for """ or ''' starting on startLine or succeeding lines.
  std::istringstream stream(source);
  std::string line;
  int currentLine = 1;
  bool foundStart = false;
  std::string quoteType;
  std::vector<std::string> docLines;

  while (std::getline(stream, line)) {
    if (currentLine >= startLine) {
      if (!foundStart) {
        const std::size_t firstNonBlank = line.find_first_not_of(" \t\r\n");
        if (firstNonBlank != std::string::npos) {
          std::string rem = line.substr(firstNonBlank);
          if (rem.rfind("\"\"\"", 0) == 0) {
            quoteType = "\"\"\"";
          } else if (rem.rfind("'''", 0) == 0) {
            quoteType = "'''";
          }
          if (!quoteType.empty()) {
            foundStart = true;
            std::string after = rem.substr(3);
            const std::size_t closing = after.find(quoteType);
            if (closing != std::string::npos) {
              // Single-line docstring: """doc"""
              return trimLine(after.substr(0, closing));
            }
            docLines.push_back(after);
          } else if (currentLine > startLine + 1) {
            // Reached other statements without finding docstring
            break;
          }
        }
      } else {
        // Inside multi-line docstring
        const std::size_t closing = line.find(quoteType);
        if (closing != std::string::npos) {
          docLines.push_back(line.substr(0, closing));
          break;
        }
        docLines.push_back(line);
      }
    }
    ++currentLine;
  }

  if (!foundStart || docLines.empty()) return {};

  // Dedent common leading whitespace (like Python textwrap.dedent)
  std::size_t minIndent = std::string::npos;
  for (std::size_t i = 1; i < docLines.size(); ++i) {
    const std::string& l = docLines[i];
    const std::size_t nb = l.find_first_not_of(" \t\r\n");
    if (nb != std::string::npos) {
      if (minIndent == std::string::npos || nb < minIndent) minIndent = nb;
    }
  }
  if (minIndent == std::string::npos) minIndent = 0;

  std::string result;
  for (std::size_t i = 0; i < docLines.size(); ++i) {
    std::string l = docLines[i];
    while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
    if (i > 0 && l.size() >= minIndent) {
      l = l.substr(minIndent);
    }
    if (i == 0 && trimLine(l).empty()) continue;
    if (!result.empty()) result += "\n";
    result += l;
  }
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
    result.pop_back();
  return result;
}

std::string LanguageServer::docCommentText(const Compilation& compilation, const FileIndex& file,
                           const Span& span) const {
  if (!span.valid()) return {};
  int moduleId = -1;
  const std::string canonical = canonicalPath(file.path);
  for (std::size_t i = 0; i < compilation.graph.modules.size(); ++i) {
    if (canonicalPath(compilation.graph.modules[i].path) == canonical) {
      moduleId = static_cast<int>(i);
      break;
    }
  }

  // 1. Check for leading `##` doc comments above the symbol.
  const std::vector<a::Comment>* comments = compilation.commentsForModule(moduleId);
  if (comments) {
    std::vector<std::string> lines;
    int expected = span.line - 1;
    std::size_t start = comments->size();
    for (std::size_t i = 0; i < comments->size(); ++i) {
      if ((*comments)[i].span.line == expected) { start = i; break; }
      if ((*comments)[i].span.line > expected) break;
    }
    if (start < comments->size()) {
      for (std::size_t index = start + 1; index > 0; --index) {
        const a::Comment& comment = (*comments)[index - 1];
        if (!comment.doc || comment.trailing || comment.span.line != expected) break;
        std::string line = comment.text;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("##", 0) == 0) line.erase(0, 2);
        if (!line.empty() && line.front() == ' ') line.erase(0, 1);
        lines.push_back(std::move(line));
        --expected;
      }
      if (!lines.empty()) {
        std::string text;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
          if (!text.empty()) text += '\n';
          text += *it;
        }
        return text;
      }
    }
  }

  // 2. Check for Python-style `"""docstring"""` or `'''docstring'''` right
  // above or as the first statement in the function / class / struct body.
  const std::string* source = compilation.textForPath(file.path);
  if (source) {
    // Check immediately inside body (e.g. line span.line + 1)
    std::string ds = extractDocstring(*source, span.line + 1);
    if (!ds.empty()) return ds;
    // Or immediately above declaration
    if (span.line > 1) {
      ds = extractDocstringAbove(*source, span.line);
      if (!ds.empty()) return ds;
    }
  }

  return {};
}

Value LanguageServer::fullDocumentRange(const std::string& text) const {
  int lines = 0;
  for (char c : text) {
    if (c == '\n') ++lines;
  }
  std::string lastLine = lineText(text, lines + 1);
  Value range = Value::object();
  Value start = Value::object();
  start["line"] = 0;
  start["character"] = 0;
  Value end = Value::object();
  end["line"] = lines;
  end["character"] = byteToUtf16Column(lastLine, static_cast<int>(lastLine.size()));
  range["start"] = std::move(start);
  range["end"] = std::move(end);
  return range;
}

}  // namespace a::lsp
