#include "a_runtime_internal.h"

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
    return A_FALSE;
}

