#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addNetDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // net
      {"net_resolve", {"net_resolve", "net.resolve(host) -> [String]",
                       "Numeric addresses in resolver order."}},
      {"net_tcp_listen", {"net_tcp_listen", "net.tcp_listen(host, port) -> i64",
                           "Listening socket; port 0 asks the OS for one."}},
      {"net_tcp_connect", {"net_tcp_connect", "net.tcp_connect(host, port) -> i64",
                            "Connected TCP socket."}},
      {"net_accept", {"net_accept", "net.accept(listener) -> i64",
                       "Next incoming connection; blocks unless a timeout is set."}},
      {"net_send", {"net_send", "net.send(handle, data) -> i64", "Bytes written."}},
      {"net_recv", {"net_recv", "net.recv(handle, maxBytes) -> String",
                     "Up to maxBytes bytes; empty at EOF or when nothing is waiting."}},
      {"net_close", {"net_close", "net.close(handle)", "Releases the socket."}},
      {"net_local_port", {"net_local_port", "net.local_port(handle) -> i64", "The bound port."}},
      {"net_set_timeout", {"net_set_timeout", "net.set_timeout(handle, millis)",
                            "Send/receive timeout; 0 clears it."}},
      {"net_set_nonblocking", {"net_set_nonblocking", "net.set_nonblocking(handle, bool)",
                                "Non-blocking mode."}},
      {"net_poll", {"net_poll", "net.poll(handles, timeoutMillis) -> [i64]",
                     "Handles that are ready to read."}},
      {"net_udp_socket", {"net_udp_socket", "net.udp_socket() -> i64", "Unbound datagram socket."}},
      {"net_udp_bind", {"net_udp_bind", "net.udp_bind(handle, host, port)",
                         "Binds a datagram socket; port 0 is ephemeral."}},
      {"net_udp_send", {"net_udp_send", "net.udp_send(handle, host, port, data) -> i64",
                         "Bytes sent in one datagram."}},
      {"net_udp_recv", {"net_udp_recv", "net.udp_recv(handle, maxBytes) -> (String, String, i64)",
                         "Payload, peer host, and peer port; (\"\", \"\", -1) when idle."}},
      {"net_tls_connect", {"net_tls_connect", "net.tls_connect(host, port) -> i64",
                            "TLS client socket; requires a build with OpenSSL."}},
      {"net_tls_available", {"net_tls_available", "net.tls_available() -> bool",
                              "Whether the toolchain was built with TLS support."}},

      // http
      {"http_get", {"http_get", "http.get(url) -> (i64, {String: String}, String)",
                     "GET request; returns status, headers, and body."}},
      {"http_post", {"http_post", "http.post(url, body) -> (i64, {String: String}, String)",
                      "POST request; returns status, headers, and body."}},
      {"http_request", {"http_request",
                         "http.request(socket, method, path, headers, body) -> (i64, {String: String}, String)",
                         "One request over an established net socket."}},
      {"http_serve", {"http_serve", "http.serve(listener, handler)",
                       "Serves connections until a handler returns status 0."}},
      {"http_serve_once", {"http_serve_once", "http.serve_once(listener, handler) -> bool",
                            "Handles one connection; true when it was served."}},
  });
}

}  // namespace a
