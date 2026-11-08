#include <cstring>

#include "handlers_common.hpp"

void LanguageServer::codeActions(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  Value actions = Value::array();
  if (!document || !document->compilation || !params) { reply(id, std::move(actions)); return; }
  const Compilation& compilation = *document->compilation;
  const Value* rangeVal = params->find("range");
  int reqLine = -1;
  if (rangeVal) {
    const Value* s = rangeVal->find("start");
    if (s && s->find("line")) reqLine = static_cast<int>(s->find("line")->asInt()) + 1;
  }
  const Value* context = params->find("context");
  std::vector<std::string> requestedKinds;
  if (context) {
    const Value* kinds = context->find("only");
    if (kinds && kinds->isArray()) for (auto& k : kinds->asArray()) if (k.isString()) requestedKinds.push_back(k.asString());
  }
  auto wantKind = [&](const std::string& k) {
    if (requestedKinds.empty()) return true;
    for (auto& r : requestedKinds) if (r == k || r == "quickfix") return true;
    return false;
  };
  auto hasSourceFixAll = [&](const std::string& k) {
    if (requestedKinds.empty()) return false;
    for (auto& r : requestedKinds) if (r == "source.fixAll" || r == "source.fixAll.a" || r == k) return true;
    return false;
  };
  auto wantSource = [&](const std::string& k) {
    if (requestedKinds.empty()) return true;
    for (auto& r : requestedKinds) if (r == k || r == "source") return true;
    return false;
  };
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  auto pushEditAction = [&](const std::string& title, const std::string& kind,
                            const std::vector<std::pair<Span, std::string>>& edits,
                            const std::vector<Value>* diagItems = nullptr) {
    Value action = Value::object();
    action["title"] = title;
    action["kind"] = kind;
    Value changes = Value::object();
    Value eps = Value::array();
    for (const auto& [span, newText] : edits) {
      Value edit = Value::object();
      edit["range"] = rangeValue(document->text, span);
      edit["newText"] = newText;
      eps.push(std::move(edit));
    }
    changes[document->uri] = std::move(eps);
    Value wsEdit = Value::object();
    wsEdit["changes"] = std::move(changes);
    action["edit"] = std::move(wsEdit);
    if (diagItems) {
      action["diagnostics"] = *diagItems;
    } else {
      action["diagnostics"] = Value::array();
    }
    actions.push(std::move(action));
  };

  if (wantKind("quickfix") && file && tokens && reqLine > 0) {
    // Unused variable (W0001): offer prefix with `_`.
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine || d.code != "W0001") continue;
      const Token* tok = tokenAt(*tokens, d.span.line, d.span.col);
      if (!tok || tok->kind != Tok::Ident) continue;
      pushEditAction("Prefix with '_': _" + tok->lexeme, "quickfix",
                     {{tok->span, "_" + tok->lexeme}});
      break;
    }
    // Unreachable statement (W0002): offer to delete the statement's lines.
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine || d.code != "W0002") continue;
      int totalLines = 0;
      for (char c : document->text) if (c == '\n') ++totalLines;
      if (document->text.empty() || document->text.back() != '\n') ++totalLines;
      if (totalLines <= 0) break;
      const int startLine = std::max(1, d.span.line);
      const int endLine = std::max(startLine, d.span.endLine);
      // Delete whole lines: from column 0 of the first line through column 0
      // of the line after the statement (or EOF for a trailing statement).
      Span del{startLine, 1, endLine, 1, d.span.fileId};
      if (endLine < totalLines) {
        del.endLine = endLine + 1;
        del.endCol = 1;
      } else {
        const std::string last = lineText(document->text, totalLines);
        del.endLine = totalLines;
        del.endCol = static_cast<int>(last.size()) + 1;
      }
      pushEditAction("Remove unreachable statement", "quickfix", {{del, ""}});
      break;
    }
    // Any diagnostic: offer "Suppress with '# a: ignore[CODE]'".
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine) continue;
      std::string tail = d.code.empty() ? " # a: ignore" : " # a: ignore[" + d.code + "]";
      std::string lt = lineText(document->text, reqLine);
      Span insert{reqLine, static_cast<int>(lt.size()) + 1, reqLine,
                  static_cast<int>(lt.size()) + 1, 0};
      pushEditAction("Suppress" + (d.code.empty() ? std::string() : " " + d.code) +
                         " on this line",
                     "quickfix", {{insert, tail}});
      break;
    }
  }
  if (hasSourceFixAll("source.fixAll") && file && tokens) {
    // Fix all: prefix every unused-variable (W0001) diagnostic's name with
    // `_` in one workspace edit.
    std::vector<std::pair<Span, std::string>> edits;
    std::unordered_set<long long> seen;
    for (const Diagnostic* d : diagnosticsForPath(compilation, document->path)) {
      if (d->code != "W0001") continue;
      const Token* tok = tokenAt(*tokens, d->span.line, d->span.col);
      if (!tok || tok->kind != Tok::Ident) continue;
      const long long key = (long long)tok->span.line * 100000 + tok->span.col;
      if (!seen.insert(key).second) continue;
      edits.emplace_back(tok->span, "_" + tok->lexeme);
    }
    if (!edits.empty()) {
      pushEditAction("Prefix all unused variables with '_'", "source.fixAll.a", edits);
    }
  }
  // Organize imports: sort and dedupe inside contiguous top-level runs of
  // import/from-import lines. Comment and blank lines inside a run stay in
  // place (an attached comment may end up above a different import — the
  // documented conservative limitation), and nothing is reordered across
  // real code.
  if (wantSource("source.organizeImports") && file) {
    const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
    const a::Module* module =
        entry >= 0 && static_cast<std::size_t>(entry) < compilation.graph.modules.size()
            ? &compilation.graph.modules[static_cast<std::size_t>(entry)]
            : nullptr;
    if (module && module->program) {
      // 1-based line -> sort key, for single-line top-level import stmts.
      std::unordered_map<int, std::string> importKeys;
      auto moduleKey = [](bool quoted, const std::string& path,
                          bool isStd, const std::string& stdMod, bool isPy,
                          const std::string& pyMod, const std::string& bare,
                          int& rank) {
        if (quoted) {
          rank = 0;
          return "\"" + path + "\"";
        }
        if (isStd) {
          rank = 1;
          return "std:" + stdMod;
        }
        if (isPy) {
          rank = 2;
          return "python:" + pyMod;
        }
        rank = 3;
        return bare;
      };
      for (const auto& statement : module->program->statements) {
        if (!statement) continue;
        if (statement->kind == a::StmtKind::Import) {
          const auto& imp = static_cast<const a::ImportStmt&>(*statement);
          if (imp.span.line != imp.span.endLine) continue;
          int rank = 3;
          std::string mod = moduleKey(!imp.path.empty(), imp.path, imp.stdModuleImport,
                                      imp.stdModule, imp.pythonModuleImport,
                                      imp.pythonModule, imp.bareName, rank);
          std::string key = std::to_string(rank) + "i" + mod;
          if (!imp.alias.empty()) key += " as " + imp.alias;
          importKeys[imp.span.line] = std::move(key);
        } else if (statement->kind == a::StmtKind::FromImport) {
          const auto& imp = static_cast<const a::FromImportStmt&>(*statement);
          if (imp.span.line != imp.span.endLine) continue;
          int rank = 3;
          std::string mod = moduleKey(!imp.path.empty(), imp.path, imp.stdModuleImport,
                                      imp.stdModule, imp.pythonModuleImport,
                                      imp.pythonModule, imp.bareName, rank);
          std::string key = std::to_string(rank) + "f" + mod + " import ";
          for (std::size_t i = 0; i < imp.items.size(); ++i) {
            if (i) key += ",";
            key += imp.items[i].name;
            if (imp.items[i].alias != imp.items[i].name) {
              key += " as " + imp.items[i].alias;
            }
          }
          importKeys[imp.span.line] = std::move(key);
        }
      }
      if (importKeys.size() >= 2) {
        std::vector<std::string> lines = splitLinesKeep(document->text);
        auto content = [](std::string& line) {
          if (!line.empty() && line.back() == '\n') line.pop_back();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return line;
        };
        auto isPassive = [&](int lineNo) {
          const std::string trimmed = trimLine(content(lines[static_cast<std::size_t>(lineNo - 1)]));
          return trimmed.empty() || trimmed.front() == '#';
        };
        // Maximal runs of import/passive lines containing >= 2 imports.
        for (std::size_t s = 0; s < lines.size();) {
          if (!importKeys.count(static_cast<int>(s) + 1) && !isPassive(static_cast<int>(s) + 1)) {
            ++s;
            continue;
          }
          std::size_t e = s;
          int importCount = 0;
          while (e < lines.size() &&
                 (importKeys.count(static_cast<int>(e) + 1) ||
                  isPassive(static_cast<int>(e) + 1))) {
            if (importKeys.count(static_cast<int>(e) + 1)) ++importCount;
            ++e;
          }
          // `e` is exclusive; the run is [s, e).
          if (importCount >= 2) {
            // Surviving import lines (exact-text dedupe keeps the first
            // occurrence), each tagged with its sort key and original line.
            struct Entry {
              int line;
              std::string key;
              std::string text;
            };
            std::vector<Entry> entries;
            std::unordered_set<std::string> seenTexts;
            for (std::size_t i = s; i < e; ++i) {
              const int lineNo = static_cast<int>(i) + 1;
              auto it = importKeys.find(lineNo);
              if (it == importKeys.end()) continue;
              const std::string text = trimLine(content(lines[i]));
              if (!seenTexts.insert(text).second) continue;  // exact duplicate
              entries.push_back(Entry{lineNo, it->second, text});
            }
            std::stable_sort(entries.begin(), entries.end(),
                             [](const Entry& a, const Entry& b) { return a.key < b.key; });
            std::unordered_map<int, std::string> lineToText;
            std::vector<int> sortedLines;
            for (const auto& entry : entries) {
              lineToText[entry.line] = entry.text;
              sortedLines.push_back(entry.line);
            }
            // Rebuild the run: passive lines keep their position; import
            // lines are filled from the sorted list, and surplus positions
            // (removed duplicates) collapse away.
            std::vector<std::string> rebuilt;
            std::size_t cursor = 0;
            for (std::size_t i = s; i < e; ++i) {
              const int lineNo = static_cast<int>(i) + 1;
              if (importKeys.count(lineNo)) {
                if (cursor < sortedLines.size()) {
                  rebuilt.push_back(lineToText[sortedLines[cursor++]]);
                }
              } else {
                rebuilt.push_back(content(lines[i]));
              }
            }
            const std::string oldText = [&] {
              std::string joined;
              for (std::size_t i = s; i < e; ++i) {
                if (i > s) joined += "\n";
                joined += content(lines[i]);
              }
              return joined;
            }();
            std::string newText;
            for (std::size_t i = 0; i < rebuilt.size(); ++i) {
              if (i) newText += "\n";
              newText += rebuilt[i];
            }
            if (newText != oldText) {
              const std::string& lastLine = content(lines[e - 1]);
              Span span{static_cast<int>(s) + 1, 1, static_cast<int>(e),
                        static_cast<int>(lastLine.size()) + 1, 0};
              pushEditAction("Organize imports", "source.organizeImports",
                             {{span, newText}});
            }
          }
          s = e;
        }
      }
    }
  }
  reply(id, std::move(actions));
}

// Selection range: identifier -> statement line -> enclosing indent blocks
// -> whole file, linked through SelectionRange.parent per the LSP spec.
void LanguageServer::selectionRanges(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  Value result = Value::array();
  if (!document || !document->compilation || !params) { reply(id, std::move(result)); return; }
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) { reply(id, std::move(result)); return; }
  std::vector<std::pair<int,int>> positions;
  const Value* posArr = params->find("positions");
  if (posArr && posArr->isArray()) {
    for (auto& p : posArr->asArray()) {
      int l = static_cast<int>(p.find("line") ? p.find("line")->asInt() : 0);
      int c = static_cast<int>(p.find("character") ? p.find("character")->asInt() : 0);
      positions.emplace_back(l, c);
    }
  }
  int totalLines = 0;
  for (char c : document->text) if (c == '\n') ++totalLines;
  if (totalLines == 0) totalLines = 1;

  auto indentOf = [&](int line) -> int {
    const std::string lt = lineText(document->text, line);
    int indent = 0;
    while (indent < static_cast<int>(lt.size()) && lt[static_cast<std::size_t>(indent)] == ' ') ++indent;
    return indent;
  };
  // First non-space column (0-based) of a line, or -1 for blank lines.
  auto contentStart = [&](int line) -> int {
    const std::string lt = lineText(document->text, line);
    for (std::size_t i = 0; i < lt.size(); ++i) {
      if (lt[i] != ' ' && lt[i] != '\t' && lt[i] != '\r') return static_cast<int>(i);
    }
    return -1;
  };
  // Suite-continuation keywords sit at the header's indent but belong to
  // the same block (`elif`, `else`, `except`, `finally`).
  auto isContinuation = [&](int line) -> bool {
    const std::string lt = trimLine(lineText(document->text, line));
    static const char* kHeads[] = {"elif", "else", "except", "finally"};
    for (const char* head : kHeads) {
      const std::size_t n = std::strlen(head);
      if (lt.compare(0, n, head) != 0) continue;
      if (lt.size() == n ||
          !(std::isalnum(static_cast<unsigned char>(lt[n])) || lt[n] == '_')) {
        return true;
      }
    }
    return false;
  };

  for (auto& [line0, char0] : positions) {
    int line = line0 + 1;
    int byteCol = utf16ToByteColumn(lineText(document->text, line), char0) + 1;
    // Innermost -> outermost spans.
    std::vector<Span> chain;
    const Token* tok = tokenAt(*tokens, line, byteCol);
    if (tok) {
      chain.push_back(tok->span);
    } else {
      chain.push_back(Span{line, byteCol, line, byteCol, 0});
    }
    // Statement level: the whole logical line's content.
    {
      const std::string lt = lineText(document->text, line);
      const int start = contentStart(line);
      if (start >= 0 && static_cast<int>(lt.size()) > start) {
        chain.push_back(Span{line, start + 1, line, static_cast<int>(lt.size()) + 1, 0});
      }
    }
    // Enclosing indent blocks, outward.
    int blockIndent = indentOf(line);
    int blockLine = line;
    for (int depth = 0; depth < 24; ++depth) {
      // Find the header: nearest preceding non-blank line indented less
      // than the current block.
      int header = -1;
      int headerIndent = -1;
      for (int l = blockLine - 1; l >= 1; --l) {
        const int ind = indentOf(l);
        if (contentStart(l) < 0) continue;  // blank
        if (ind < blockIndent) { header = l; headerIndent = ind; break; }
      }
      if (header < 0) break;
      // Block end: last line before a non-blank, non-continuation line
      // indented at or below the header.
      int endLine = header;
      for (int l = header + 1; l <= totalLines; ++l) {
        const int ind = indentOf(l);
        if (contentStart(l) < 0) continue;  // blank lines belong inside
        if (ind <= headerIndent && !isContinuation(l)) break;
        endLine = l;
      }
      const int hStart = contentStart(header);
      const std::string et = lineText(document->text, endLine);
      chain.push_back(Span{header, hStart >= 0 ? hStart + 1 : 1,
                           endLine, static_cast<int>(et.size()) + 1, 0});
      if (endLine <= header) break;
      blockIndent = headerIndent;
      blockLine = header;
      if (headerIndent == 0) break;
    }
    // Whole file as the outermost range.
    chain.push_back(Span{1, 1, totalLines,
                         static_cast<int>(lineText(document->text, totalLines).size()) + 1, 0});
    // Link innermost -> outermost through `parent` (a SelectionRange, not
    // an array, per the spec).
    Value node;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      Value next = Value::object();
      next["range"] = rangeValue(document->text, *it);
      if (!node.isNull()) next["parent"] = std::move(node);
      node = std::move(next);
    }
    result.push(std::move(node));
  }
  reply(id, std::move(result));
}

}  // namespace a::lsp
