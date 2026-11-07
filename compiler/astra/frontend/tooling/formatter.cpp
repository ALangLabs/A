#include "tooling/formatter.hpp"
#include "tooling/formatter_internal.hpp"

#include <climits>
#include <memory>
#include <utility>
#include <vector>

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"

namespace a::tooling::detail {

Formatter::Formatter(std::string source, std::vector<Comment> comments, FormatOptions options)
      : source_(std::move(source)),
        comments_(std::move(comments)),
        options_(options) {
    if (options_.indentWidth < 1) options_.indentWidth = 4;
    if (options_.maxLineWidth < 20) options_.maxLineWidth = 100;
  }

std::string Formatter::run(Program& program) {
    emitStatements(program.statements, 0, INT_MAX / 4, 0);

    std::size_t end = output_.size();
    while (end > 0 && (output_[end - 1] == '\n' || output_[end - 1] == '\r')) --end;
    output_.resize(end);
    if (!output_.empty()) output_ += '\n';
    return output_;
  }

}  // namespace a::tooling::detail

namespace a::tooling {

FormatResult formatSource(const std::string& source, DiagnosticSink& sink,
                          const FormatOptions& options) {
  FormatResult result;
  Lexer lexer(source, sink, 0);
  std::vector<Token> tokens = lexer.tokenize();
  std::vector<Comment> comments = lexer.takeComments();
  if (sink.hasErrors()) return result;

  Parser parser(std::move(tokens), sink);
  std::unique_ptr<Program> program = parser.parseProgram();
  if (!program || sink.hasErrors()) return result;

  detail::Formatter formatter(source, std::move(comments), options);
  result.text = formatter.run(*program);
  result.ok = true;
  return result;
}

}  // namespace a::tooling
