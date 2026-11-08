#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinNet(const std::string& name, CallExpr& call) {

  if (name == "net_resolve") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the host name");
    return typeArray(typeString());
  }

  if (name == "net_tcp_listen" || name == "net_tcp_connect" || name == "net_tls_connect") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the host name");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The port number");
    return typeInt(64, true);
  }

  if (name == "net_accept" || name == "net_local_port" || name == "net_close" ||
      name == "net_recv_status") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return name == "net_close" ? typeVoid() : typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    return name == "net_close" ? typeVoid() : typeInt(64, true);
  }

  if (name == "net_send" || name == "net_recv") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return name == "net_recv" ? typeString() : typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    if (name == "net_send") {
      checkArgAgainst(*call.args[1].value, typeString(), "as the data to send");
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The maximum byte count");
    return typeString();
  }

  if (name == "net_set_timeout") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The timeout in milliseconds");
    return typeVoid();
  }

  if (name == "net_set_nonblocking") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeBool(), "as the non-blocking flag");
    return typeVoid();
  }

  if (name == "net_poll") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeArray(typeInt(64, true));
    }
    checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                    "as the socket handles");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The poll timeout");
    return typeArray(typeInt(64, true));
  }

  if (name == "net_udp_socket") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeInt(64, true);
  }

  if (name == "net_udp_bind") {
    if (call.args.size() != 3) {
      builtinArityError(name, call, 3);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the bind address");
    requireInteger(inferExpr(*call.args[2].value), call.args[2].value->span,
                   "The port number");
    return typeVoid();
  }

  if (name == "net_udp_send") {
    if (call.args.size() != 4) {
      builtinArityError(name, call, 4);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the destination host");
    requireInteger(inferExpr(*call.args[2].value), call.args[2].value->span,
                   "The destination port");
    checkArgAgainst(*call.args[3].value, typeString(), "as the datagram");
    return typeInt(64, true);
  }

  if (name == "net_udp_recv") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeTuple({typeString(), typeString(), typeInt(64, true)});
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The maximum byte count");
    return typeTuple({typeString(), typeString(), typeInt(64, true)});
  }

  if (name == "net_tls_available") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeBool();
  }

  // ---------------------------------------------------------- http module

  if (name == "http_get" || name == "http_post") {
    const std::size_t expected = name == "http_get" ? 1 : 2;
    TypePtr response = typeTuple(
        {typeInt(64, true), typeMap(typeString(), typeString()), typeString()});
    if (call.args.size() != expected) {
      builtinArityError(name, call, expected);
      inferCallArgTypes(call);
      return response;
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the URL");
    if (name == "http_post") checkArgAgainst(*call.args[1].value, typeString(), "as the body");
    return response;
  }

  if (name == "http_request") {
    TypePtr response = typeTuple(
        {typeInt(64, true), typeMap(typeString(), typeString()), typeString()});
    if (call.args.size() != 5) {
      builtinArityError(name, call, 5);
      inferCallArgTypes(call);
      return response;
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeString(), "as the HTTP method");
    checkArgAgainst(*call.args[2].value, typeString(), "as the request path");
    checkArgAgainst(*call.args[3].value, typeMap(typeString(), typeString()),
                    "as the request headers");
    checkArgAgainst(*call.args[4].value, typeString(), "as the request body");
    return response;
  }

  if (name == "http_serve" || name == "http_serve_once") {
    TypePtr handler = typeFunction(
        {typeString(), typeString(), typeMap(typeString(), typeString()), typeString()},
        typeTuple({typeInt(64, true), typeMap(typeString(), typeString()), typeString()}));
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return name == "http_serve" ? typeVoid() : typeBool();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The listening socket handle");
    checkArgAgainst(*call.args[1].value, handler, "in the HTTP handler");
    return name == "http_serve" ? typeVoid() : typeBool();
  }

  // ------------------------------------------------------------- kernels
  // CPU kernel builtins behind the `kernels` package. The interpreter validates
  // shapes at run time; here we only fix the result shape so wrappers can stay
  // thin and numeric-generic.

  return nullptr;
}

}  // namespace a
