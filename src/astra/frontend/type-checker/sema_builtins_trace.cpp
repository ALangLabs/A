#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinTrace(const std::string& name, CallExpr& call) {

  if (name == "trace_start") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the span name");
    return typeInt(64, true);
  }

  if (name == "trace_end") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.end'");
    return typeVoid();
  }

  if (name == "trace_current") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeInt(64, true);
  }

  if (name == "trace_set_attribute") {
    if (call.args.size() != 3) {
      builtinArityError(name, call, 3);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.set_attribute'");
    checkArgAgainst(*call.args[1].value, typeString(), "as the attribute name");
    inferAnyCallArg(call, 2, "as the attribute value");
    return typeVoid();
  }

  if (name == "trace_add_event") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The span passed to 'trace.add_event'");
    checkArgAgainst(*call.args[1].value, typeString(), "as the event name");
    return typeVoid();
  }

  if (name == "trace_inject") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeString();
  }

  if (name == "trace_extract") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the trace context");
    return typeInt(64, true);
  }

  if (name == "trace_spans") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeArray(typeJson());
  }

  if (name == "trace_reset") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeVoid();
  }

  // ----------------------------------------------------------- net module

  return nullptr;
}

}  // namespace a
