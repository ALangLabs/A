#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinJson(const std::string& name, CallExpr& call) {

  if (name == "json_parse") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeJson();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in the text passed to 'json.parse'");
    return typeJson();
  }

  if (name == "json_stringify" || name == "json_pretty") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    inferAnyCallArg(call, 0, "to 'json." + std::string(name == "json_stringify" ? "stringify" : "pretty") + "'");
    return typeString();
  }

  if (name == "json_is_valid") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeBool();
    }
    checkArgAgainst(*call.args[0].value, typeString(), "in 'json.is_valid'");
    return typeBool();
  }

  if (name == "json_get" || name == "json_has") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return name == "json_get" ? typeOptional(typeJson()) : typeBool();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json'");
    checkArgAgainst(*call.args[1].value, typeString(), "in the key passed to 'json'");
    return name == "json_get" ? typeOptional(typeJson()) : typeBool();
  }

  if (name == "json_at") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeOptional(typeJson());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json.at'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to 'json.at'");
    return typeOptional(typeJson());
  }

  if (name == "json_len") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.len'");
    return typeInt(64, true);
  }

  if (name == "json_keys") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.keys'");
    return typeArray(typeString());
  }

  if (name == "json_type") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in 'json.type'");
    return typeString();
  }

  if (name == "json_as_str" || name == "json_as_int" || name == "json_as_float" ||
      name == "json_as_bool" || name == "json_as_array" || name == "json_as_map") {
    TypePtr inner = typeJson();
    if (name == "json_as_str") inner = typeString();
    if (name == "json_as_int") inner = typeInt(64, true);
    if (name == "json_as_float") inner = typeFloat(64);
    if (name == "json_as_bool") inner = typeBool();
    if (name == "json_as_array") inner = typeArray(typeJson());
    if (name == "json_as_map") inner = typeMap(typeString(), typeJson());
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeOptional(inner);
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'json'");
    return typeOptional(inner);
  }

  // -------------------------------------------------------- binary module

  if (name == "binary_encode") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeInt(64, true));
    }
    inferAnyCallArg(call, 0, "to 'binary.encode'");
    return typeArray(typeInt(64, true));
  }

  if (name == "binary_decode") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeOptional(typeJson());
    }
    checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                    "in the bytes passed to 'binary.decode'");
    return typeOptional(typeJson());
  }

  // ------------------------------------------------------- testing module

  return nullptr;
}

}  // namespace a
