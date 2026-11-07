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
    for (auto& r : requestedKinds) if (r == k) return true;
    return false;
  };
  auto hasSourceFixAll = [&](const std::string& k) {
    if (requestedKinds.empty()) return true;
    for (auto& r : requestedKinds) if (r == "source.fixAll" || r == "source.fixAll.a" || r == k) return true;
    return false;
  };
  if (!wantKind("quickfix")) { reply(id, std::move(actions)); return; }
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (file && tokens && reqLine > 0) {
    // Unused variable (W0001): offer prefix with `_`.
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine || d.code != "W0001") continue;
      const Token* tok = tokenAt(*tokens, d.span.line, d.span.col);
      if (!tok || tok->kind != Tok::Ident) continue;
      std::string newName = "_" + tok->lexeme;
      Value action = Value::object();
      action["title"] = "Prefix with '_': " + newName;
      action["kind"] = "quickfix";
      Value edit = Value::object();
      edit["range"] = rangeValue(document->text, tok->span);
      edit["newText"] = newName;
      Value changes = Value::object();
      Value eps = Value::array(); eps.push(std::move(edit));
      changes[document->uri] = std::move(eps);
      Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
      action["edit"] = std::move(wsEdit);
      action["diagnostics"] = Value::array();
      actions.push(std::move(action));
      break;
    }
    // Any diagnostic: offer "Suppress with '# a: ignore[CODE]'".
    for (const auto& d : compilation.sink.diagnostics()) {
      if (d.span.line != reqLine) continue;
      std::string tail = d.code.empty() ? " # a: ignore" : " # a: ignore[" + d.code + "]";
      Value action = Value::object();
      action["title"] = "Suppress" + (d.code.empty() ? std::string() : " " + d.code) + " on this line";
      action["kind"] = "quickfix";
      std::string lt = lineText(document->text, reqLine);
      Value edit = Value::object();
      Value insertPos = Value::object();
      insertPos["line"] = reqLine - 1;
      insertPos["character"] = byteToUtf16Column(lt, (int)lt.size());
      edit["range"] = rangeValue(document->text, Span{reqLine, (int)lt.size() + 1, reqLine, (int)lt.size() + 1, 0});
      edit["newText"] = tail;
      Value changes = Value::object();
      Value eps = Value::array(); eps.push(std::move(edit));
      changes[document->uri] = std::move(eps);
      Value wsEdit = Value::object(); wsEdit["changes"] = std::move(changes);
      action["edit"] = std::move(wsEdit);
      actions.push(std::move(action));
      break;
    }
  }
  if (hasSourceFixAll("source.fixAll")) {
    // No bulk fix available yet; return what we have.
  }
  reply(id, std::move(actions));
}

// Selection range: identifier -> enclosing indent block -> next block.
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
  for (auto& [line0, char0] : positions) {
    int line = line0 + 1;
    int byteCol = utf16ToByteColumn(lineText(document->text, line), char0) + 1;
    Value entry = Value::object();
    Value current = Value::object();
    const Token* tok = tokenAt(*tokens, line, byteCol);
    if (tok) current["range"] = rangeValue(document->text, tok->span);
    else { Value r = Value::object(); r["start"] = positionValue(document->text, Span{line, byteCol, line, byteCol, 0}); r["end"] = r["start"]; current["range"] = std::move(r); }
    // Outward: expand to the indent block containing the line.
    int indent = 0;
    std::string lt = lineText(document->text, line);
    while (indent < (int)lt.size() && lt[indent] == ' ') ++indent;
    int blockStartLine = line;
    while (blockStartLine > 1) {
      std::string prev = lineText(document->text, blockStartLine - 1);
      int pi = 0; while (pi < (int)prev.size() && prev[pi] == ' ') ++pi;
      if (pi >= indent && !prev.empty()) --blockStartLine;
      else break;
    }
    int blockEndLine = line;
    int totalLines = 0; for (char c : document->text) if (c == '\n') ++totalLines;
    while (blockEndLine <= totalLines) {
      std::string next = lineText(document->text, blockEndLine + 1);
      if (next.empty()) break;
      int ni = 0; while (ni < (int)next.size() && next[ni] == ' ') ++ni;
      if (ni > indent) ++blockEndLine;
      else break;
    }
    Value ranges = Value::array();
    ranges.push(current);
    Value outer = Value::object();
    Span blockSpan{blockStartLine, 1, blockEndLine, (int)lineText(document->text, blockEndLine).size() + 1, 0};
    outer["range"] = rangeValue(document->text, blockSpan);
    Value outerArr = Value::array(); outerArr.push(std::move(outer));
    current["parent"] = std::move(outerArr);
    entry["ranges"] = std::move(ranges);
    result.push(std::move(entry));
  }
  reply(id, std::move(result));
}

}  // namespace a::lsp
