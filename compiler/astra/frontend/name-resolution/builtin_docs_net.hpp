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
      {"net_tls_wrap", {"net_tls_wrap", "net.tls_wrap(handle, host[, verify]) -> i64",
                         "Starts TLS on a connected socket (HTTP CONNECT, STARTTLS)."}},
      {"net_send_bytes", {"net_send_bytes", "net.send_bytes(handle, bytes) -> i64",
                            "Bytes written from an [i64] byte array."}},
      {"net_recv_bytes", {"net_recv_bytes", "net.recv_bytes(handle, maxBytes) -> [i64]",
                            "Byte array read; empty at EOF or when nothing is waiting."}},
      {"net_recv_exact", {"net_recv_exact", "net.recv_exact(handle, byteCount) -> [i64]",
                            "Exactly byteCount bytes, one native call (HTTP body fast path); shorter only at EOF."}},
      {"net_recv_body", {"net_recv_body", "net.recv_body(handle, byteCount) -> (String?, [i64])",
                            "Whole body in one call; the text side is filled for UTF-8 payloads (flat buffer), otherwise the byte side."}},
      {"net_recv_body_all", {"net_recv_body_all", "net.recv_body_all(handle) -> (String?, [i64])",
                                "Close-delimited variant of net.recv_body: reads until EOF."}},
      {"net_inflate_bytes", {"net_inflate_bytes", "net.inflate_bytes(bytes, windowBits) -> [i64]",
                               "zlib decompression; 47 auto-detects gzip and zlib headers."}},
      {"net_inflate_available", {"net_inflate_available", "net.inflate_available() -> bool",
                                   "Whether zlib is available for gzip/deflate bodies."}},
      {"net_tls_available", {"net_tls_available", "net.tls_available() -> bool",
                              "Whether the toolchain was built with TLS support."}},
      {"net_recv_status", {"net_recv_status", "net.recv_status(handle) -> i64",
                            "Last recv outcome: 0 ok, 1 EOF, 2 error, 3 timeout; -1 bad handle."}},
  });
}

}  // namespace a
