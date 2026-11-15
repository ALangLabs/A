#include "tooling/formatter_internal.hpp"

#include <algorithm>
#include <string>

namespace a::tooling::detail {

std::string Formatter::indentText(int level) const {
    return std::string(static_cast<std::size_t>(std::max(0, level) * options_.indentWidth), ' ');
  }

void Formatter::ensureGap(int line, int& lastLine) {
    if (lastLine > 0 && line > lastLine + 1 && !output_.empty()) {
      if (!(output_.size() >= 2 && output_[output_.size() - 2] == '\n')) output_ += '\n';
    }
  }

void Formatter::emitHeader(const std::string& text, int level, const Span& span, int& lastLine) {
    if (span.valid()) ensureGap(span.line, lastLine);
    output_ += indentText(level) + text + '\n';
    if (span.valid()) lastLine = span.line;
    // A comment written after the header (for example `fn f():  # helper`)
    // stays on the header line.
    if (cursor_ < comments_.size() && comments_[cursor_].trailing &&
        comments_[cursor_].span.line == span.line) {
      std::string comment = comments_[cursor_].text;
      while (!comment.empty() && (comment.back() == '\r' || comment.back() == '\n')) {
        comment.pop_back();
      }
      output_.pop_back();
      output_ += "  " + comment + '\n';
      ++cursor_;
    }
  }

// Emits a docstring recorded on a declaration (`"""..."""` statement inside
// a type body). The stored text is the raw literal content, so it is written
// back verbatim between triple-quote delimiters.
void Formatter::emitDocstring(const std::string& doc, int level) {
    if (doc.empty()) return;
    // Trailing whitespace (including the indentation before the closing
    // quotes) is dropped so repeated formatting stays stable.
    std::string text = doc;
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
            text.back() == '\n')) {
      text.pop_back();
    }
    if (text.empty()) return;
    if (text.find("\"\"\"") != std::string::npos) {
      // A `"""` sequence cannot appear inside a triple-quoted literal; only a
      // single-line doc can be re-emitted safely as an escaped string.
      if (text.find('\n') == std::string::npos) {
        std::string escaped;
        for (char c : text) {
          if (c == '"' || c == '\\') escaped += '\\';
          escaped += c;
        }
        output_ += indentText(level) + "\"" + escaped + "\"\n";
      }
      return;
    }
    if (text.find('\n') == std::string::npos) {
      output_ += indentText(level) + "\"\"\"" + text + "\"\"\"\n";
      return;
    }
    output_ += indentText(level) + "\"\"\"\n";
    output_ += text;
    output_ += '\n';
    output_ += indentText(level) + "\"\"\"\n";
  }

std::string Formatter::postfixWrap(const std::string& text, int parent, int precedence) {
    if (precedence < parent) return "(" + text + ")";
    return text;
  }

std::string Formatter::joinList(const std::string& prefix, const std::vector<std::string>& items,
                              const std::string& suffix) {
    std::string text = prefix;
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i) text += ", ";
      text += items[i];
    }
    text += suffix;
    return text;
  }

// Continuation lines of a nested expression already carry absolute
// indentation; this only fixes the common case where a child expression
// produced newlines relative to its own indent level.
void Formatter::reindentContinuation(std::string& text, int level) const {
    std::size_t position = 0;
    while ((position = text.find('\n', position)) != std::string::npos) {
      ++position;
      std::size_t spaces = 0;
      while (position + spaces < text.size() && text[position + spaces] == ' ') ++spaces;
      if (spaces == 0) {
        text.insert(position, indentText(level));
        position += static_cast<std::size_t>(level * options_.indentWidth);
      }
    }
  }

}  // namespace a::tooling::detail
