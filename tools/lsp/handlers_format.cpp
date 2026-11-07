#include "handlers_common.hpp"

void LanguageServer::formatting(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);

  a::tooling::FormatOptions options;
  if (params) {
    if (const Value* formatOptions = params->find("options")) {
      if (const Value* tabSize = formatOptions->find("tabSize"); tabSize && tabSize->isNumber()) {
        const int size = static_cast<int>(tabSize->asInt());
        if (size >= 1 && size <= 16) options.indentWidth = size;
      }
    }
  }

  a::DiagnosticSink sink;
  sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
  a::tooling::FormatResult result =
      a::tooling::formatSource(document->text, sink, options);

  Value edits = Value::array();
  if (!result.ok || sink.hasErrors() || result.text == document->text) {
    reply(id, std::move(edits));
    return;
  }

  Value edit = Value::object();
  edit["range"] = fullDocumentRange(document->text);
  edit["newText"] = result.text;
  edits.push(std::move(edit));
  reply(id, std::move(edits));
}

void LanguageServer::foldingRanges(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const std::vector<Token>* tokens = document->compilation->entryTokens();
  if (!tokens) return replyNull(id);

  Value result = Value::array();
  std::vector<int> starts;
  int lastCodeLine = 0;
  auto addRange = [&](int startLine, int endLine) {
    if (startLine <= 0 || endLine <= startLine) return;
    Value range = Value::object();
    range["startLine"] = startLine - 1;
    range["endLine"] = endLine - 1;
    range["kind"] = "region";
    result.push(std::move(range));
  };

  for (const Token& token : *tokens) {
    switch (token.kind) {
      case Tok::Indent:
        starts.push_back(lastCodeLine > 0 ? lastCodeLine : token.span.line);
        break;
      case Tok::Dedent:
        if (!starts.empty()) {
          const int startLine = starts.back();
          starts.pop_back();
          addRange(startLine, lastCodeLine);
        }
        break;
      case Tok::Newline:
      case Tok::Eof:
        break;
      default:
        if (token.span.endLine > 0) lastCodeLine = token.span.endLine;
        break;
    }
  }
  while (!starts.empty()) {
    addRange(starts.back(), lastCodeLine);
    starts.pop_back();
  }
  reply(id, std::move(result));
}

void LanguageServer::rangeFormatting(const Value& id, const Value* params) {
  Value edits = Value::array();
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) {
    reply(id, std::move(edits));
    return;
  }
  const Value* rangeVal = params->find("range");
  if (!rangeVal) {
    reply(id, std::move(edits));
    return;
  }
  const Value* reqStart = rangeVal->find("start");
  const Value* reqEnd = rangeVal->find("end");
  if (!reqStart || !reqEnd) {
    reply(id, std::move(edits));
    return;
  }
  const int reqStartLine = static_cast<int>(reqStart->find("line")
                                                 ? reqStart->find("line")->asInt()
                                                 : 0);
  const int reqStartChar = static_cast<int>(reqStart->find("character")
                                                ? reqStart->find("character")->asInt()
                                                : 0);
  const int reqEndLine = static_cast<int>(
      reqEnd->find("line") ? reqEnd->find("line")->asInt() : 0);
  const int reqEndChar = static_cast<int>(reqEnd->find("character")
                                              ? reqEnd->find("character")->asInt()
                                              : 0);

  a::tooling::FormatOptions options;
  if (const Value* formatOptions = params->find("options")) {
    if (const Value* tabSize = formatOptions->find("tabSize");
        tabSize && tabSize->isNumber()) {
      const int size = static_cast<int>(tabSize->asInt());
      if (size >= 1 && size <= 16) options.indentWidth = size;
    }
  }

  a::DiagnosticSink sink;
  sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
  a::tooling::FormatResult result =
      a::tooling::formatSource(document->text, sink, options);
  if (!result.ok || sink.hasErrors() || result.text == document->text) {
    reply(id, std::move(edits));
    return;
  }

  const std::vector<std::string> oldLines = splitLinesKeep(document->text);
  const std::vector<std::string> newLines = splitLinesKeep(result.text);
  const int lineCount = static_cast<int>(oldLines.size());
  auto endOfFilePos = [&]() {
    Value pos = Value::object();
    pos["line"] = lineCount - 1;
    pos["character"] =
        byteToUtf16Column(oldLines.back(), static_cast<int>(oldLines.back().size()));
    return pos;
  };
  auto insideRange = [&](int startLine, int startChar, int endLine, int endChar) {
    const bool afterStart = startLine > reqStartLine ||
                            (startLine == reqStartLine && startChar >= reqStartChar);
    const bool beforeEnd = endLine < reqEndLine ||
                           (endLine == reqEndLine && endChar <= reqEndChar);
    return afterStart && beforeEnd;
  };

  for (const auto& hunk : diffLineHunks(oldLines, newLines)) {
    const int oldStart = hunk[0];
    const int oldEnd = hunk[1];
    // Edit range: start of oldStart line through start of oldEnd line (or
    // EOF when the hunk reaches the last line).
    Value edit = Value::object();
    Value range = Value::object();
    Value start = Value::object();
    if (oldStart < lineCount) {
      start["line"] = oldStart;
      start["character"] = 0;
    } else {
      start = endOfFilePos();
    }
    Value end = Value::object();
    int endLine = 0;
    int endChar = 0;
    if (oldEnd < lineCount) {
      endLine = oldEnd;
      endChar = 0;
    } else {
      end = endOfFilePos();
    }
    end["line"] = endLine;
    end["character"] = endChar;
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    if (!insideRange(static_cast<int>(range["start"]["line"].asInt()),
                     static_cast<int>(range["start"]["character"].asInt()),
                     static_cast<int>(range["end"]["line"].asInt()),
                     static_cast<int>(range["end"]["character"].asInt()))) {
      continue;
    }
    std::string newText;
    for (int i = hunk[2]; i < hunk[3]; ++i) newText += newLines[static_cast<std::size_t>(i)];
    edit["range"] = std::move(range);
    edit["newText"] = std::move(newText);
    edits.push(std::move(edit));
  }
  reply(id, std::move(edits));
}


}  // namespace a::lsp
