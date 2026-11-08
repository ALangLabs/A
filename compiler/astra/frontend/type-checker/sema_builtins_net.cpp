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

  if (name == "net_tcp_listen" || name == "net_tcp_connect" ||
      name == "net_tls_connect" || name == "net_tls_wrap") {
    const bool wrap = name == "net_tls_wrap";
    const std::size_t high = name == "net_tcp_listen" ? 2 : 3;
    if (call.args.size() < 2 || call.args.size() > high) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    if (wrap) {
      requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                     "The socket handle");
      checkArgAgainst(*call.args[1].value, typeString(), "as the host name");
    } else {
      checkArgAgainst(*call.args[0].value, typeString(), "as the host name");
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The port number");
    }
    if (call.args.size() == 3) {
      checkArgAgainst(*call.args[2].value, typeBool(), "as the verify flag");
    }
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

  if (name == "net_send_bytes") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    checkArgAgainst(*call.args[1].value, typeArray(typeInt(64, true)),
                    "as the bytes to send");
    return typeInt(64, true);
  }

  if (name == "net_recv_exact") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeArray(typeInt(64, true));
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The socket handle");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The byte count");
    return typeArray(typeInt(64, true));
  }

  if (name == "net_recv_bytes" || name == "net_inflate_bytes") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeArray(typeInt(64, true));
    }
    if (name == "net_recv_bytes") {
      requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                     "The socket handle");
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The maximum byte count");
    } else {
      checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                      "as the compressed bytes");
      requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                     "The zlib windowBits");
    }
    return typeArray(typeInt(64, true));
  }

  if (name == "net_inflate_available") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeBool();
  }

  if (name == "net_tls_available") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeBool();
  }

  // ------------------------------------------------------------- kernels
  // CPU kernel builtins behind the `kernels` package. The interpreter validates
  // shapes at run time; here we only fix the result shape so wrappers can stay
  // thin and numeric-generic.

  return nullptr;
}

}  // namespace a
