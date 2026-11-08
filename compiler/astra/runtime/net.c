#include "a_runtime_internal.h"

static int net_tls_probe(void);
static ssize_t net_ssl_read(void *ssl, void *buf, int len);
static ssize_t net_ssl_write(void *ssl, const void *buf, int len);
static void net_ssl_shutdown(void *ssl);
static void net_ssl_free(void *ssl);

/* ------------------------------------------------------------------ */
/* net module: resolve + TLS probe                                     */
/* ------------------------------------------------------------------ */

aval bi_net_resolve(uint64_t n, aval *argv) {
    want_args("net.resolve", n, 1, 1);
    uint64_t hl;
    const char *host = str_data(argv[0], &hl);
    char hbuf[256];
    if (hl >= sizeof hbuf) panicf("net.resolve: host name too long");
    memcpy(hbuf, host, hl);
    hbuf[hl] = '\0';
    aval *items = NULL;
    size_t cnt = 0, cap = 0;
#if defined(__unix__) || defined(__APPLE__)
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hbuf, NULL, &hints, &res) == 0) {
        for (ai = res; ai; ai = ai->ai_next) {
            char buf[INET6_ADDRSTRLEN];
            void *addr = ai->ai_family == AF_INET
                             ? (void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                             : (void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
            if (!inet_ntop(ai->ai_family, addr, buf, sizeof buf)) continue;
            if (cnt == cap) {
                cap = cap ? cap * 2 : 8;
                items = (aval *)xrealloc(items, cap * sizeof(aval));
            }
            items[cnt++] = a_str_new(buf, strlen(buf));
        }
        freeaddrinfo(res);
    }
#endif
    aval out = list_from_vec(items, (uint64_t)cnt, 0);
    free(items);
    return out;
}

aval bi_net_tls_available(uint64_t n, aval *argv) {
    want_args("net.tls_available", n, 0, 0);
    return net_tls_probe() ? A_TRUE : A_FALSE;
}

/* ------------------------------------------------------------------ */
/* sockets: handle table + TCP/TLS builtins                            */
/* ------------------------------------------------------------------ */

#define NET_MAX_SOCKS 64

/* net.recv_status values stored per handle after each net.recv call. */
enum { NET_RX_OK = 0, NET_RX_EOF = 1, NET_RX_ERR = 2, NET_RX_TIMEOUT = 3 };

typedef struct {
    int fd;
    void *ssl;      /* SSL* when TLS, else NULL */
    int used;
    int recv_status; /* NET_RX_* from the last net.recv on this handle */
} NetSock;

static NetSock g_socks[NET_MAX_SOCKS];

static int net_sock_new(int fd) {
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (!g_socks[i].used) {
            g_socks[i].used = 1;
            g_socks[i].fd = fd;
            g_socks[i].ssl = NULL;
            g_socks[i].recv_status = NET_RX_OK;
            return i + 1;   /* handles are 1-based; 0 = invalid */
        }
    }
    close(fd);
    return 0;
}

static NetSock *net_sock_get(aval hv, const char *what) {
    int64_t h = expect_int64(hv, what);
    if (h < 1 || h > NET_MAX_SOCKS || !g_socks[h - 1].used)
        panicf("%s: bad socket handle %lld", what, (long long)h);
    return &g_socks[h - 1];
}

static void net_host_port(aval hv, aval pv, char *host, size_t hsz,
                          char *port, size_t psz) {
    uint64_t hl;
    const char *h = str_data(hv, &hl);
    if (hl >= hsz) panicf("net: host too long");
    memcpy(host, h, hl); host[hl] = '\0';
    /* The type checker takes the port as an integer; a string is still
       accepted for callers that format it themselves. */
    if (is_int_val(pv)) {
        snprintf(port, psz, "%lld", (long long)int_val_full(pv));
        return;
    }
    uint64_t pl;
    const char *p = str_data(pv, &pl);
    if (pl >= psz) panicf("net: port too long");
    memcpy(port, p, pl); port[pl] = '\0';
}

/* Connect to the first reachable address; returns fd or -1. */
static int net_tcp_connect_fd(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) { fd = s; break; }
        close(s);
    }
    freeaddrinfo(res);
    return fd;
}

aval bi_net_tcp_connect(uint64_t n, aval *argv) {
    want_args("net.tcp_connect", n, 2, 2);
    char host[256], port[16];
    net_host_port(argv[0], argv[1], host, sizeof host, port, sizeof port);
    int fd = net_tcp_connect_fd(host, port);
    if (fd < 0) return mkint_full(-1);
    int h = net_sock_new(fd);
    if (!h) { close(fd); return mkint_full(-1); }
    return mkint_full(h);
}

aval bi_net_send(uint64_t n, aval *argv) {
    want_args("net.send", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.send");
    uint64_t len;
    const char *data = str_data(argv[1], &len);
    size_t off = 0;
    while (off < len) {
        ssize_t w;
        if (s->ssl) {
            w = net_ssl_write(s->ssl, data + off, (int)(len - off));
        } else {
            w = send(s->fd, data + off, len - off, MSG_NOSIGNAL);
        }
        if (w <= 0) break;
        off += (size_t)w;
    }
    return mkint_full((int64_t)off);
}

aval bi_net_recv(uint64_t n, aval *argv) {
    want_args("net.recv", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.recv");
    int64_t maxb = expect_int64(argv[1], "net.recv maxBytes");
    if (maxb <= 0) maxb = 1;
    if (maxb > (1 << 20)) maxb = 1 << 20;
    char *buf = (char *)xmalloc((size_t)maxb);
    ssize_t r;
    if (s->ssl) {
        r = net_ssl_read(s->ssl, buf, (int)maxb);
    } else {
        r = recv(s->fd, buf, (size_t)maxb, 0);
    }
    if (r > 0) {
        s->recv_status = NET_RX_OK;
    } else if (r == 0) {
        s->recv_status = NET_RX_EOF;
    } else {
        s->recv_status = (errno == EAGAIN || errno == EWOULDBLOCK)
                             ? NET_RX_TIMEOUT : NET_RX_ERR;
    }
    if (r <= 0) { free(buf); return a_str_new("", 0); }
    aval out = a_str_new(buf, (uint64_t)r);
    free(buf);
    return out;
}

aval bi_net_recv_status(uint64_t n, aval *argv) {
    want_args("net.recv_status", n, 1, 1);
    int64_t h = expect_int64(argv[0], "net.recv_status");
    if (h < 1 || h > NET_MAX_SOCKS || !g_socks[h - 1].used)
        return mkint_full(-1);
    return mkint_full(g_socks[h - 1].recv_status);
}

aval bi_net_close(uint64_t n, aval *argv) {
    want_args("net.close", n, 1, 1);
    int64_t h = expect_int64(argv[0], "net.close");
    if (h >= 1 && h <= NET_MAX_SOCKS && g_socks[h - 1].used) {
        NetSock *s = &g_socks[h - 1];
        if (s->ssl) { net_ssl_shutdown(s->ssl); net_ssl_free(s->ssl); s->ssl = NULL; }
        if (s->fd >= 0) close(s->fd);
        s->fd = -1;
        s->used = 0;
    }
    return A_NONE;
}

aval bi_net_set_timeout(uint64_t n, aval *argv) {
    want_args("net.set_timeout", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.set_timeout");
    int64_t ms = expect_int64(argv[1], "net.set_timeout millis");
    if (ms < 0) ms = 0;
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return A_NONE;
}

aval bi_net_local_port(uint64_t n, aval *argv) {
    want_args("net.local_port", n, 1, 1);
    NetSock *s = net_sock_get(argv[0], "net.local_port");
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getsockname(s->fd, (struct sockaddr *)&ss, &sl) != 0) return mkint_full(-1);
    if (ss.ss_family == AF_INET)
        return mkint_full(ntohs(((struct sockaddr_in *)&ss)->sin_port));
    if (ss.ss_family == AF_INET6)
        return mkint_full(ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    return mkint_full(-1);
}

/* TLS via dlopen'd OpenSSL — no link-time dependency. */
typedef struct ssl_ctx_st NET_SSL_CTX;
typedef struct ssl_st NET_SSL;
typedef struct ssl_method_st NET_SSL_METHOD;
static NET_SSL_METHOD *(*p_net_TLS_client_method)(void);
static NET_SSL_CTX *(*p_net_SSL_CTX_new)(const NET_SSL_METHOD *);
static NET_SSL *(*p_net_SSL_new)(NET_SSL_CTX *);
static int (*p_net_SSL_set_fd)(NET_SSL *, int);
static int (*p_net_SSL_connect)(NET_SSL *);
static int (*p_net_SSL_read)(NET_SSL *, void *, int);
static int (*p_net_SSL_write)(NET_SSL *, const void *, int);
static int (*p_net_SSL_shutdown)(NET_SSL *);
static void (*p_net_SSL_free)(NET_SSL *);
static void (*p_net_SSL_CTX_free)(NET_SSL_CTX *);
/* Optional: SNI, peer hostname check, verify result. NULL when the loaded
   OpenSSL is too old to export them. SNI goes through SSL_ctrl because
   SSL_set_tlsext_host_name is a public macro, not an exported symbol. */
typedef int (*net_ssl_verify_cb)(int, void *);
static int (*p_net_SSL_CTX_set_verify)(NET_SSL_CTX *, int, net_ssl_verify_cb);
static int (*p_net_SSL_CTX_set_default_verify_paths)(NET_SSL_CTX *);
static long (*p_net_SSL_ctrl)(NET_SSL *, int, long, void *);
static int (*p_net_SSL_set1_host)(NET_SSL *, const char *);
static long (*p_net_SSL_get_verify_result)(NET_SSL *);
static void (*p_net_SSL_set_verify)(NET_SSL *, int, net_ssl_verify_cb);
static int (*p_net_SSL_CTX_load_verify_locations)(NET_SSL_CTX *, const char *, const char *);
static NET_SSL_CTX *g_net_ssl_ctx = NULL;
static int g_net_ssl_tried = 0;

#define NET_SSL_VERIFY_PEER 0x01
/* OpenSSL constants for the SNI SSL_ctrl call (ssl.h defines these as macros). */
#define NET_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define NET_TLSEXT_NAMETYPE_host_name 0

static int net_ssl_sym(void *h, const char *name, void **slot) {
    void *p = dlsym(h, name);
    if (!p) return 0;
    *slot = p;
    return 1;
}

static int net_tls_probe(void) {
    if (g_net_ssl_tried) return g_net_ssl_ctx != NULL;
    g_net_ssl_tried = 1;
    const char *names[] = {"libssl.so.3", "libssl.so", "libssl.so.1.1", NULL};
    void *h = NULL;
    for (int i = 0; names[i]; i++) {
        h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) break;
    }
    if (!h) return 0;
    if (!net_ssl_sym(h, "TLS_client_method", (void **)&p_net_TLS_client_method) &&
        !net_ssl_sym(h, "SSLv23_client_method", (void **)&p_net_TLS_client_method))
        return 0;
    if (!net_ssl_sym(h, "SSL_CTX_new", (void **)&p_net_SSL_CTX_new) ||
        !net_ssl_sym(h, "SSL_new", (void **)&p_net_SSL_new) ||
        !net_ssl_sym(h, "SSL_set_fd", (void **)&p_net_SSL_set_fd) ||
        !net_ssl_sym(h, "SSL_connect", (void **)&p_net_SSL_connect) ||
        !net_ssl_sym(h, "SSL_read", (void **)&p_net_SSL_read) ||
        !net_ssl_sym(h, "SSL_write", (void **)&p_net_SSL_write) ||
        !net_ssl_sym(h, "SSL_shutdown", (void **)&p_net_SSL_shutdown) ||
        !net_ssl_sym(h, "SSL_free", (void **)&p_net_SSL_free) ||
        !net_ssl_sym(h, "SSL_CTX_free", (void **)&p_net_SSL_CTX_free))
        return 0;
    /* Optional symbols: absence just downgrades TLS features. */
    net_ssl_sym(h, "SSL_CTX_set_verify", (void **)&p_net_SSL_CTX_set_verify);
    net_ssl_sym(h, "SSL_CTX_set_default_verify_paths",
                (void **)&p_net_SSL_CTX_set_default_verify_paths);
    net_ssl_sym(h, "SSL_ctrl", (void **)&p_net_SSL_ctrl);
    net_ssl_sym(h, "SSL_set1_host", (void **)&p_net_SSL_set1_host);
    net_ssl_sym(h, "SSL_get_verify_result",
                (void **)&p_net_SSL_get_verify_result);
    net_ssl_sym(h, "SSL_set_verify", (void **)&p_net_SSL_set_verify);
    net_ssl_sym(h, "SSL_CTX_load_verify_locations",
                (void **)&p_net_SSL_CTX_load_verify_locations);
    g_net_ssl_ctx = p_net_SSL_CTX_new(p_net_TLS_client_method());
    if (!g_net_ssl_ctx) return 0;
    /* The ctx does not disable verification, so enable peer-cert checking:
       default CA paths + SSL_VERIFY_PEER. Without this SSL_set1_host and
       SSL_get_verify_result would be no-ops. */
    if (p_net_SSL_CTX_set_default_verify_paths)
        p_net_SSL_CTX_set_default_verify_paths(g_net_ssl_ctx);
    /* A_TLS_CA_FILE adds a private CA bundle on top of the system roots. */
    const char *cafile = getenv("A_TLS_CA_FILE");
    if (cafile && *cafile && p_net_SSL_CTX_load_verify_locations)
        p_net_SSL_CTX_load_verify_locations(g_net_ssl_ctx, cafile, NULL);
    if (p_net_SSL_CTX_set_verify)
        p_net_SSL_CTX_set_verify(g_net_ssl_ctx, NET_SSL_VERIFY_PEER, NULL);
    return 1;
}

static ssize_t net_ssl_read(void *ssl, void *buf, int len) {
    return p_net_SSL_read((NET_SSL *)ssl, buf, len);
}
static ssize_t net_ssl_write(void *ssl, const void *buf, int len) {
    return p_net_SSL_write((NET_SSL *)ssl, buf, len);
}
static void net_ssl_shutdown(void *ssl) {
    p_net_SSL_shutdown((NET_SSL *)ssl);
}
static void net_ssl_free(void *ssl) {
    p_net_SSL_free((NET_SSL *)ssl);
}

/* Handshake TLS over an already-connected fd. Returns the SSL* or NULL. */
static NET_SSL *net_tls_handshake(int fd, const char *host, int verify) {
    NET_SSL *ssl = p_net_SSL_new(g_net_ssl_ctx);
    if (!ssl) return NULL;
    /* SNI + expected peer hostname for cert verification. Virtual-hosted
       servers abort the handshake when SNI is missing. */
    if (p_net_SSL_ctrl)
        p_net_SSL_ctrl(ssl, NET_SSL_CTRL_SET_TLSEXT_HOSTNAME,
                       NET_TLSEXT_NAMETYPE_host_name, (void *)host);
    if (verify) {
        if (p_net_SSL_set1_host) p_net_SSL_set1_host(ssl, host);
    } else if (p_net_SSL_set_verify) {
        p_net_SSL_set_verify(ssl, 0, NULL);
    }
    if (p_net_SSL_set_fd(ssl, fd) != 1 || p_net_SSL_connect(ssl) != 1 ||
        (verify && p_net_SSL_get_verify_result &&
         p_net_SSL_get_verify_result(ssl) != 0)) {
        p_net_SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

aval bi_net_tls_connect(uint64_t n, aval *argv) {
    want_args("net.tls_connect", n, 2, 3);
    char host[256], port[16];
    net_host_port(argv[0], argv[1], host, sizeof host, port, sizeof port);
    int verify = n == 3 ? truthy(argv[2]) : 1;
    if (!net_tls_probe()) return mkint_full(-1);
    int fd = net_tcp_connect_fd(host, port);
    if (fd < 0) return mkint_full(-1);
    NET_SSL *ssl = net_tls_handshake(fd, host, verify);
    if (!ssl) {
        close(fd);
        return mkint_full(-1);
    }
    int h = net_sock_new(fd);
    if (!h) {
        p_net_SSL_free(ssl);
        close(fd);
        return mkint_full(-1);
    }
    g_socks[h - 1].ssl = ssl;
    return mkint_full(h);
}

/* TLS over an existing connection (HTTP CONNECT tunnels, STARTTLS). */
aval bi_net_tls_wrap(uint64_t n, aval *argv) {
    want_args("net.tls_wrap", n, 2, 3);
    NetSock *s = net_sock_get(argv[0], "net.tls_wrap");
    uint64_t hl;
    const char *h = str_data(argv[1], &hl);
    char host[256];
    if (hl >= sizeof host) panicf("net.tls_wrap: host too long");
    memcpy(host, h, hl);
    host[hl] = '\0';
    int verify = n == 3 ? truthy(argv[2]) : 1;
    if (!net_tls_probe()) return mkint_full(-1);
    if (s->ssl) { p_net_SSL_free(s->ssl); s->ssl = NULL; }
    NET_SSL *ssl = net_tls_handshake(s->fd, host, verify);
    if (!ssl) return mkint_full(-1);
    s->ssl = ssl;
    return argv[0];
}


/* ------------------------------------------------------------------ */
/* net: listening sockets, readiness, UDP                              */
/* ------------------------------------------------------------------ */

aval bi_net_tcp_listen(uint64_t n, aval *argv) {
    want_args("net.tcp_listen", n, 2, 2);
    char host[256], port[16];
    net_host_port(argv[0], argv[1], host, sizeof host, port, sizeof port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host[0] ? host : NULL, port, &hints, &res) != 0 || !res)
        return mkint_full(-1);
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(s, ai->ai_addr, ai->ai_addrlen) == 0 && listen(s, 128) == 0) {
            fd = s;
            break;
        }
        close(s);
    }
    freeaddrinfo(res);
    if (fd < 0) return mkint_full(-1);
    int h = net_sock_new(fd);
    if (!h) { close(fd); return mkint_full(-1); }
    return mkint_full(h);
}

aval bi_net_accept(uint64_t n, aval *argv) {
    want_args("net.accept", n, 1, 1);
    NetSock *s = net_sock_get(argv[0], "net.accept");
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0) return mkint_full(-1);
    int h = net_sock_new(fd);
    if (!h) { close(fd); return mkint_full(-1); }
    return mkint_full(h);
}

aval bi_net_set_nonblocking(uint64_t n, aval *argv) {
    want_args("net.set_nonblocking", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.set_nonblocking");
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags < 0) return A_NONE;
    if (truthy(argv[1])) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(s->fd, F_SETFL, flags);
    return A_NONE;
}

aval bi_net_poll(uint64_t n, aval *argv) {
    want_args("net.poll", n, 2, 2);
    int64_t ms = expect_int64(argv[1], "net.poll timeout");
    if (ms < 0) ms = 0;
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    aval *ready = NULL;
    size_t cnt = 0, cap = 0;
    struct pollfd *fds = (struct pollfd *)xmalloc((a->h.len ? a->h.len : 1) * sizeof *fds);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        int64_t h = expect_int64(arr_elem(a, i), "net.poll handle");
        fds[i].fd = (h >= 1 && h <= NET_MAX_SOCKS && g_socks[h - 1].used) ? g_socks[h - 1].fd : -1;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    if (poll(fds, (nfds_t)a->h.len, (int)ms) > 0) {
        for (uint64_t i = 0; i < a->h.len; ++i) {
            if (!fds[i].revents) continue;
            if (cnt == cap) {
                cap = cap ? cap * 2 : 8;
                ready = (aval *)xrealloc(ready, cap * sizeof(aval));
            }
            ready[cnt++] = arr_elem(a, i);
        }
    }
    free(fds);
    aval out = list_from_vec(ready, (uint64_t)cnt, 0);
    free(ready);
    return out;
}

aval bi_net_udp_socket(uint64_t n, aval *argv) {
    want_args("net.udp_socket", n, 0, 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return mkint_full(-1);
    int h = net_sock_new(fd);
    if (!h) { close(fd); return mkint_full(-1); }
    return mkint_full(h);
}

aval bi_net_udp_bind(uint64_t n, aval *argv) {
    want_args("net.udp_bind", n, 3, 3);
    NetSock *s = net_sock_get(argv[0], "net.udp_bind");
    uint64_t al;
    const char *addr = str_data(argv[1], &al);
    int64_t port = expect_int64(argv[2], "net.udp_bind port");
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%lld", (long long)port);
    char addrbuf[256];
    if (al >= sizeof addrbuf) panicf("net.udp_bind: address too long");
    memcpy(addrbuf, addr, al);
    addrbuf[al] = '\0';
    if (getaddrinfo(addrbuf[0] ? addrbuf : NULL, portbuf, &hints, &res) != 0 || !res)
        return A_NONE;
    bind(s->fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return A_NONE;
}

aval bi_net_udp_send(uint64_t n, aval *argv) {
    want_args("net.udp_send", n, 4, 4);
    NetSock *s = net_sock_get(argv[0], "net.udp_send");
    uint64_t hl;
    const char *h = str_data(argv[1], &hl);
    char host[256], port[16];
    if (hl >= sizeof host) panicf("net.udp_send: host too long");
    memcpy(host, h, hl);
    host[hl] = '\0';
    snprintf(port, sizeof port, "%lld", (long long)expect_int64(argv[2], "net.udp_send port"));
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return mkint_full(-1);
    uint64_t len;
    const char *data = str_data(argv[3], &len);
    ssize_t sent = sendto(s->fd, data, len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return mkint_full(sent < 0 ? -1 : (int64_t)sent);
}

aval bi_net_udp_recv(uint64_t n, aval *argv) {
    want_args("net.udp_recv", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.udp_recv");
    int64_t maxb = expect_int64(argv[1], "net.udp_recv maxBytes");
    if (maxb <= 0) maxb = 1;
    if (maxb > (1 << 20)) maxb = 1 << 20;
    char *buf = (char *)xmalloc((size_t)maxb);
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;
    ssize_t r = recvfrom(s->fd, buf, (size_t)maxb, 0, (struct sockaddr *)&from, &fl);
    aval items[3];
    if (r < 0) {
        s->recv_status = (errno == EAGAIN || errno == EWOULDBLOCK) ? NET_RX_TIMEOUT
                                                                  : NET_RX_ERR;
        items[0] = a_str_new("", 0);
        items[1] = a_str_new("", 0);
        items[2] = mkint_full(0);
    } else {
        char hostbuf[INET6_ADDRSTRLEN] = "";
        int port = 0;
        getnameinfo((struct sockaddr *)&from, fl, hostbuf, sizeof hostbuf, NULL, 0,
                    NI_NUMERICHOST);
        if (from.ss_family == AF_INET)
            port = ntohs(((struct sockaddr_in *)&from)->sin_port);
        else if (from.ss_family == AF_INET6)
            port = ntohs(((struct sockaddr_in6 *)&from)->sin6_port);
        s->recv_status = NET_RX_OK;
        items[0] = a_str_new(buf, (uint64_t)r);
        items[1] = a_str_new(hostbuf, strlen(hostbuf));
        items[2] = mkint_full(port);
    }
    free(buf);
    return list_from_vec(items, 3, 1);
}

/* ------------------------------------------------------------------ */
/* net: byte-array I/O and zlib decompression                          */
/* ------------------------------------------------------------------ */

/* Reads a [i64] byte list into a malloc'd buffer (values clamped to 0..255). */
static char *net_bytes_in(aval v, uint64_t *len) {
    aval tmp = collect_iterable(v);
    AArr *a = (AArr *)obj_of(tmp);
    char *buf = (char *)xmalloc(a->h.len ? a->h.len : 1);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        int64_t b = expect_int64(arr_elem(a, i), "byte");
        buf[i] = (char)(unsigned char)(b & 0xff);
    }
    *len = a->h.len;
    return buf;
}

static aval net_bytes_out(const char *buf, uint64_t len) {
    /* Packed int storage: one allocation, no per-byte tagged values. */
    int64_t *items = (int64_t *)xmalloc((len ? len : 1) * sizeof(int64_t));
    for (uint64_t i = 0; i < len; ++i) items[i] = (unsigned char)buf[i];
    aval out = a_list_from_i64(items, len);
    free(items);
    return out;
}

aval bi_net_send_bytes(uint64_t n, aval *argv) {
    want_args("net.send_bytes", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.send_bytes");
    uint64_t len;
    char *data = net_bytes_in(argv[1], &len);
    size_t off = 0;
    while (off < len) {
        ssize_t w = s->ssl ? net_ssl_write(s->ssl, data + off, (int)(len - off))
                           : send(s->fd, data + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
    free(data);
    return mkint_full((int64_t)off);
}

aval bi_net_recv_bytes(uint64_t n, aval *argv) {
    want_args("net.recv_bytes", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.recv_bytes");
    int64_t maxb = expect_int64(argv[1], "net.recv_bytes maxBytes");
    if (maxb <= 0) maxb = 1;
    if (maxb > (1 << 20)) maxb = 1 << 20;
    char *buf = (char *)xmalloc((size_t)maxb);
    ssize_t r = s->ssl ? net_ssl_read(s->ssl, buf, (int)maxb)
                       : recv(s->fd, buf, (size_t)maxb, 0);
    if (r > 0) {
        s->recv_status = NET_RX_OK;
    } else if (r == 0) {
        s->recv_status = NET_RX_EOF;
    } else {
        s->recv_status = (errno == EAGAIN || errno == EWOULDBLOCK)
                             ? NET_RX_TIMEOUT : NET_RX_ERR;
    }
    aval out = r > 0 ? net_bytes_out(buf, (uint64_t)r) : net_bytes_out("", 0);
    free(buf);
    return out;
}

/* Read exactly nBytes from the socket in one native call. This is the HTTP
   body fast path: the pure-A loop pays ~35 ns per byte for list.extend and
   slice copies, which caps 1 MiB bodies at ~17 MB/s. Returns the bytes read
   (shorter than requested only at EOF/error). */
aval bi_net_recv_exact(uint64_t n, aval *argv) {
    want_args("net.recv_exact", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.recv_exact");
    int64_t want = expect_int64(argv[1], "net.recv_exact byteCount");
    if (want < 0) want = 0;
    /* Bytes live as int64 elements, so they are expanded straight into the
       packed array while reading: one allocation, one pass over the data. */
    int64_t *items = (int64_t *)xmalloc((size_t)(want ? want : 1) * sizeof(int64_t));
    char tmp[65536];
    size_t got = 0;
    while (got < (size_t)want) {
        size_t room = (size_t)want - got;
        size_t chunk = room < sizeof tmp ? room : sizeof tmp;
        ssize_t r = s->ssl ? net_ssl_read(s->ssl, tmp, (int)chunk)
                           : recv(s->fd, tmp, chunk, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; ++i) items[got + (size_t)i] = (unsigned char)tmp[i];
        got += (size_t)r;
    }
    return a_list_take_i64(items, got);
}

/* Read up to `want` bytes (want < 0: until EOF) from s into a fresh buffer. */
static char *net_read_to_buf(NetSock *s, int64_t want, size_t *got_out) {
    size_t cap = want > 0 ? (size_t)want : 65536;
    char *buf = (char *)xmalloc(cap);
    size_t got = 0;
    for (;;) {
        if (got == cap) {
            if (want > 0) break;          /* exact read satisfied */
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
        size_t chunk = cap - got;
        if (chunk > 65536) chunk = 65536;
        ssize_t r = s->ssl ? net_ssl_read(s->ssl, buf + got, (int)chunk)
                           : recv(s->fd, buf + got, chunk, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    *got_out = got;
    return buf;
}

/* Body result shared by recv_body/recv_body_all: a (String?, [i64]) pair with
   exactly one side filled. Text payloads stay a flat char buffer end to end;
   only payloads that are not UTF-8 pay for int64 byte storage (8x memory). */
static aval net_body_result(char *buf, size_t got) {
    aval parts[2];
    if (a_utf8_valid(buf, got)) {
        parts[0] = a_str_new(buf, got);
        parts[1] = a_list_from_i64(NULL, 0);
    } else {
        int64_t *ints = (int64_t *)xmalloc((got ? got : 1) * sizeof(int64_t));
        for (size_t i = 0; i < got; ++i) ints[i] = (unsigned char)buf[i];
        parts[0] = A_NONE;
        parts[1] = a_list_take_i64(ints, got);
    }
    free(buf);
    return a_tuple_new(2, parts);
}

/* net.recv_body(handle, byteCount) -> (String?, [i64]) */
aval bi_net_recv_body(uint64_t n, aval *argv) {
    want_args("net.recv_body", n, 2, 2);
    NetSock *s = net_sock_get(argv[0], "net.recv_body");
    int64_t want = expect_int64(argv[1], "net.recv_body byteCount");
    if (want < 0) want = 0;
    size_t got = 0;
    char *buf = net_read_to_buf(s, want, &got);
    return net_body_result(buf, got);
}

/* net.recv_body_all(handle) -> (String?, [i64]) — close-delimited bodies. */
aval bi_net_recv_body_all(uint64_t n, aval *argv) {
    want_args("net.recv_body_all", n, 1, 1);
    NetSock *s = net_sock_get(argv[0], "net.recv_body_all");
    size_t got = 0;
    char *buf = net_read_to_buf(s, -1, &got);
    return net_body_result(buf, got);
}

/* zlib via dlopen: gzip/deflate bodies without a link-time dependency. */
typedef struct {
    const unsigned char *next_in;
    unsigned avail_in;
    unsigned long total_in;
    unsigned char *next_out;
    unsigned avail_out;
    unsigned long total_out;
    const char *msg;
    void *state;
    void *(*zalloc)(void *, unsigned, unsigned);
    void (*zfree)(void *, void *);
    void *opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
} NetZStream;

static int (*p_net_inflateInit2_)(NetZStream *, int, const char *, int);
static int (*p_net_inflate)(NetZStream *, int);
static int (*p_net_inflateEnd)(NetZStream *);
static const char *(*p_net_zlibVersion)(void);
static int g_net_zlib_tried = 0;
static int g_net_zlib_ok = 0;

#define NET_ZLIB_ZEQ 0
#define NET_ZLIB_ZSTREAM_END 1
#define NET_ZLIB_ZBUF_ERROR (-5)

static void net_zlib_probe(void) {
    if (g_net_zlib_tried) return;
    g_net_zlib_tried = 1;
    const char *names[] = {"libz.so.1", "libz.so", "libz.dylib", NULL};
    void *h = NULL;
    for (int i = 0; names[i]; i++) {
        h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) break;
    }
    if (!h) return;
    if (!net_ssl_sym(h, "inflateInit2_", (void **)&p_net_inflateInit2_) ||
        !net_ssl_sym(h, "inflate", (void **)&p_net_inflate) ||
        !net_ssl_sym(h, "inflateEnd", (void **)&p_net_inflateEnd) ||
        !net_ssl_sym(h, "zlibVersion", (void **)&p_net_zlibVersion))
        return;
    g_net_zlib_ok = 1;
}

aval bi_net_inflate_available(uint64_t n, aval *argv) {
    want_args("net.inflate_available", n, 0, 0);
    net_zlib_probe();
    return g_net_zlib_ok ? A_TRUE : A_FALSE;
}

aval bi_net_inflate_bytes(uint64_t n, aval *argv) {
    want_args("net.inflate_bytes", n, 2, 2);
    net_zlib_probe();
    if (!g_net_zlib_ok) panicf("net.inflate_bytes: zlib not available");
    int wbits = (int)expect_int64(argv[1], "net.inflate_bytes windowBits");
    uint64_t len;
    char *src = net_bytes_in(argv[0], &len);
    uint64_t cap = len * 4 > 4096 ? len * 4 : 4096;
    char *out = (char *)xmalloc(cap);
    uint64_t got = 0;
    NetZStream zs;
    memset(&zs, 0, sizeof zs);
    if (p_net_inflateInit2_(&zs, wbits, p_net_zlibVersion(), (int)sizeof zs) != NET_ZLIB_ZEQ) {
        free(src);
        free(out);
        panicf("net.inflate_bytes: inflateInit2 failed");
    }
    zs.next_in = (const unsigned char *)src;
    zs.avail_in = (unsigned)len;
    int rc = NET_ZLIB_ZEQ;
    for (;;) {
        zs.next_out = (unsigned char *)out + got;
        zs.avail_out = (unsigned)(cap - got);
        rc = p_net_inflate(&zs, NET_ZLIB_ZEQ);
        got = cap - zs.avail_out;
        if (rc == NET_ZLIB_ZSTREAM_END) break;
        if (rc != NET_ZLIB_ZEQ) break;
        if (zs.avail_out == 0) {
            cap = cap * 2;
            out = (char *)xrealloc(out, cap);
            continue;
        }
        if (zs.avail_in == 0) break; /* truncated stream */
    }
    p_net_inflateEnd(&zs);
    free(src);
    if (rc != NET_ZLIB_ZSTREAM_END) {
        free(out);
        panicf("net.inflate_bytes: corrupt or truncated compressed data");
    }
    aval result = net_bytes_out(out, got);
    free(out);
    return result;
}
