#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* growable buffers                                                    */
/* ------------------------------------------------------------------ */



void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->p = (char *)xrealloc(b->p, nc);
    b->cap = nc;
}

void buf_addn(Buf *b, const char *s, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }

void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }

void buf_addi64(Buf *b, int64_t v) {
    char tmp[24];
    char *end = tmp + sizeof(tmp);
    char *p = end;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    buf_addn(b, p, (size_t)(end - p));
}

aval buf_take(Buf *b) {
    aval s = a_str_new(b->p ? b->p : "", (uint64_t)b->len);
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    return s;
}

