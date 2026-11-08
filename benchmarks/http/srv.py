import socketserver, http.server, sys

BODIES = {
    "/tiny": b"ok",
    "/k16": b"x" * (16 * 1024),
    "/m1": b"y" * (1024 * 1024),
    "/m10": b"z" * (10 * 1024 * 1024),
}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True  # otherwise keep-alive clients see 40 ms delayed ACKs
    def log_message(self, *a):
        pass
    def do_GET(self):
        body = BODIES.get(self.path, b"")
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        self.wfile.write(body)

class Srv(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 512

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else int(__import__("os").environ.get("BENCH_PORT", "8901"))
    with Srv(("127.0.0.1", port), H) as s:
        s.serve_forever()
