#include "backend/kgen/execution_engine.hpp"
#include "backend/kgen/execution_engine_impl.hpp"
namespace a::kgen {
int runWithEngine(const ModuleGraph& g, const SemaResult& s, DiagnosticSink& sink, std::istream& in, std::ostream& out, std::ostream& err) {
  KGENExecutionEngine interp(g, s, sink, in, out, err);
  int c = interp.run();
  sink.flush(err, false);
  return c;
}
int testWithEngine(const ModuleGraph& g, const SemaResult& s, DiagnosticSink& sink, std::istream& in, std::ostream& out, std::ostream& err) {
  KGENExecutionEngine interp(g, s, sink, in, out, err);
  int c = interp.runTests(out);
  sink.flush(err, false);
  return c;
}
}
