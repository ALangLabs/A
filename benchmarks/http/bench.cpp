// raw POSIX-socket HTTP/1.1 client: same shape as the A client (no pooling),
// plus an optional keep-alive mode (BENCH_KEEPALIVE=1).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_port = 8901;

static int connect_to(const char* host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    ::inet_pton(AF_INET, host, &a.sin_addr);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// Reads one response (headers + Content-Length body) from fd.
// Reusable: leftover bytes beyond the body stay in `buf`.
static size_t read_response(int fd, std::string& buf, bool& ok) {
    ok = false;
    for (;;) {
        auto pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            size_t bodyStart = pos + 4;
            size_t len = 0;
            // Content-Length is the only framing this server uses.
            std::string head = buf.substr(0, pos);
            auto cl = head.find("Content-Length:");
            if (cl != std::string::npos) len = strtoul(head.c_str() + cl + 15, nullptr, 10);
            while (buf.size() - bodyStart < len) {
                char tmp[65536];
                ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
                if (r <= 0) return 0;
                buf.append(tmp, (size_t)r);
            }
            buf.erase(0, bodyStart + len);
            ok = true;
            return len;
        }
        char tmp[65536];
        ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
        if (r <= 0) return 0;
        buf.append(tmp, (size_t)r);
    }
}

static size_t one(const std::string& path, bool keepalive, int& fd, std::string& buf) {
    if (fd < 0) {
        fd = connect_to("127.0.0.1", g_port);
        if (fd < 0) { perror("connect"); exit(1); }
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(g_port) + "\r\n";
    req += keepalive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    size_t off = 0;
    while (off < req.size()) {
        ssize_t w = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (w <= 0) { perror("send"); exit(1); }
        off += (size_t)w;
    }
    bool ok = false;
    size_t n = read_response(fd, buf, ok);
    if (!ok) { fprintf(stderr, "bad response\n"); exit(1); }
    if (!keepalive) { ::close(fd); fd = -1; buf.clear(); }
    return n;
}

int main() {
    const char* p = getenv("BENCH_PATH");
    int n = atoi(getenv("BENCH_N"));
    if (const char* p = getenv("BENCH_PORT")) g_port = atoi(p);
    bool ka = getenv("BENCH_KEEPALIVE") && atoi(getenv("BENCH_KEEPALIVE")) == 1;
    std::string path = p ? p : "/tiny";
    int fd = -1;
    std::string buf;
    size_t total = one(path, ka, fd, buf);  // warm
    total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) total += one(path, ka, fd, buf);
    auto t1 = std::chrono::steady_clock::now();
    if (fd >= 0) ::close(fd);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    printf("%lld %zu\n", (long long)ms, total);
    return 0;
}
