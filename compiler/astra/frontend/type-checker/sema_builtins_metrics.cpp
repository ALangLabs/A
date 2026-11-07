#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinMetrics(const std::string& name, CallExpr& call) {

  if (name == "metrics_counter" || name == "metrics_gauge" || name == "metrics_histogram") {
    if (call.args.size() < 2 || (call.args.size() > 3 && (call.args.size() - 2) % 2 != 0)) {
      sink_.error(call.span,
                  "'" + name +
                      "' expects a name, a value, and optional labels (a map or key/value pairs).");
      inferCallArgTypes(call);
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the metric name");
    if (name == "metrics_counter") {
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The increment passed to 'metrics.counter'");
    } else {
      requireNumeric(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The value passed to '" + name + "'");
    }
    if (call.args.size() == 3) {
      inferAnyCallArg(call, 2, "as metric labels");
    } else if (call.args.size() > 3) {
      for (std::size_t i = 2; i + 1 < call.args.size(); i += 2) {
        checkArgAgainst(*call.args[i].value, typeString(), "as a metric label name");
        inferAnyCallArg(call, i + 1, "as a metric label value");
      }
    }
    return typeVoid();
  }

  if (name == "metrics_render") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeString();
  }

  if (name == "metrics_reset") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeVoid();
  }

  // --------------------------------------------------------- trace module

  return nullptr;
}

}  // namespace a
