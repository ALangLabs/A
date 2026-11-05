#pragma once

// Bridge between the interpreter's `repl.eval` builtin and the pipeline layer.
// The backend cannot depend on the pipeline (which owns ReplSession), so the
// driver registers a handler that evaluates source in a persistent REPL
// session. Returns the eval exit code: 0 ok, 1 compile error, 2 runtime error.

#include <iosfwd>
#include <string>

namespace a::kgen {

using ReplEvalHandler = int (*)(const std::string& source, std::istream& in,
                                std::ostream& out, std::ostream& err);

void setReplEvalHandler(ReplEvalHandler handler);
ReplEvalHandler replEvalHandler();

}  // namespace a::kgen
