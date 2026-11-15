#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinLog(const std::string& name, CallExpr& call) {

  if (name == "log_set_level") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in 'log.set_level'");
    return typeVoid();
  }

  if (name == "log_set_json") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeBool(), "in 'log.set_json'");
    return typeVoid();
  }

  if (name == "log_debug" || name == "log_info" || name == "log_warn" ||
      name == "log_error") {
    if (call.args.empty()) {
      sink_.error(call.span, "'" + name + "' expects a message.");
      return typeVoid();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the log message");
    if (call.args.size() == 2) {
      inferAnyCallArg(call, 1, "as log fields");
    } else if (call.args.size() > 2) {
      if ((call.args.size() - 1) % 2 != 0) {
        sink_.error(call.span,
                    "'" + name +
                        "' expects a message followed by field/value pairs or a fields map.");
        inferCallArgTypes(call);
        return typeVoid();
      }
      for (std::size_t i = 1; i + 1 < call.args.size(); i += 2) {
        checkArgAgainst(*call.args[i].value, typeString(), "as a log field name");
        inferAnyCallArg(call, i + 1, "as a log field value");
      }
    }
    return typeVoid();
  }

  // ------------------------------------------------------- metrics module

  return nullptr;
}

}  // namespace a
