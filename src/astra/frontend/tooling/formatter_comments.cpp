#include "tooling/formatter_internal.hpp"

#include <string>

namespace a::tooling::detail {

// ------------------------------------------------------------- comments

void Formatter::emitComment(const Comment& comment, int level, int& lastLine) {
    ensureGap(comment.span.line, lastLine);
    std::string text = comment.text;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    output_ += indentText(level) + text + '\n';
    lastLine = comment.span.endLine > 0 ? comment.span.endLine : comment.span.line;
  }

// Emits pending comments whose source line is strictly before `line`.
void Formatter::emitLeadingComments(int line, int level, int& lastLine) {
    while (cursor_ < comments_.size() && comments_[cursor_].span.line < line) {
      emitComment(comments_[cursor_], level, lastLine);
      ++cursor_;
    }
  }

// Emits pending comments inside a statement's source range before the
// statement itself. This keeps comments that were written inside multiline
// expressions from being lost when the expression is collapsed.
void Formatter::emitInteriorComments(const Span& span, int level, int& lastLine) {
    if (!span.valid()) return;
    while (cursor_ < comments_.size() && comments_[cursor_].span.line < span.endLine) {
      emitComment(comments_[cursor_], level, lastLine);
      ++cursor_;
    }
  }

bool Formatter::pendingTrailingComment(const Span& span, int& commentIndex) const {
    if (!span.valid() || cursor_ >= comments_.size()) return false;
    const Comment& comment = comments_[cursor_];
    if (comment.span.line != span.endLine || comment.span.col < span.endCol) return false;
    commentIndex = static_cast<int>(cursor_);
    return true;
  }

void Formatter::attachTrailing(const Span& span, int& lastLine) {
    int index = 0;
    if (!pendingTrailingComment(span, index)) return;
    std::string text = comments_[static_cast<std::size_t>(index)].text;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    if (!output_.empty() && output_.back() == '\n') output_.pop_back();
    if (!output_.empty() && output_.back() == '\r') output_.pop_back();
    output_ += "  " + text + '\n';
    lastLine = span.endLine;
    cursor_ = static_cast<std::size_t>(index) + 1;
  }

void Formatter::emitLeftoverComments(int limitLine, int level, int& lastLine) {
    const int minimumColumn = level * options_.indentWidth + 1;
    while (cursor_ < comments_.size()) {
      const Comment& comment = comments_[cursor_];
      if (comment.span.line >= limitLine) break;
      if (comment.span.line <= lastLine) {
        ++cursor_;
        continue;
      }
      if (comment.span.col < minimumColumn) break;
      emitComment(comment, level, lastLine);
      ++cursor_;
    }
  }

}  // namespace a::tooling::detail
