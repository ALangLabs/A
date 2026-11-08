/* Cross-language benchmark, C side (gcc -O2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile long sink;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

/* open-addressing int->int map, the same shape A's dict uses */
#define MCAP (1 << 20)
static long mkeys[MCAP], mvals[MCAP];
static unsigned char mused[MCAP];
static void mput(long k, long v) {
    unsigned long h = (unsigned long)k * 2654435761u;
    unsigned long i = h & (MCAP - 1);
    while (mused[i] && mkeys[i] != k) i = (i + 1) & (MCAP - 1);
    mused[i] = 1;
    mkeys[i] = k;
    mvals[i] = v;
}
static long mget(long k) {
    unsigned long h = (unsigned long)k * 2654435761u;
    unsigned long i = h & (MCAP - 1);
    while (mused[i]) {
        if (mkeys[i] == k) return mvals[i];
        i = (i + 1) & (MCAP - 1);
    }
    return -1;
}

int main(int argc, char **argv) {
    long N = 20000000;
    if (argc > 1) N = atol(argv[1]);

    /* int loop */
    double t0 = now_ms(), best = 1e18;
    for (int r = 0; r < 3; ++r) {
        double s = now_ms();
        long sum = 0;
        for (long i = 0; i < N; ++i) sum = (sum * 31 + i) % 1000000007;
        sink = sum;
        double d = now_ms() - s;
        if (d < best) best = d;
    }
    printf("int_loop %.3f\n", best);

    /* float loop */
    best = 1e18;
    for (int r = 0; r < 3; ++r) {
        double s = now_ms();
        double x = 1.0;
        for (long i = 0; i < N; ++i) x = x * 1.000001 + 0.5;
        sink = (long)x;
        double d = now_ms() - s;
        if (d < best) best = d;
    }
    printf("float_loop %.3f\n", best);

    /* fib(30) */
    long arg = argc + 29; /* opaque to the optimizer */
    best = 1e18;
    for (int r = 0; r < 3; ++r) {
        /* keep the call loop-variant so it cannot be hoisted out of the loop */
        long a = ((long)((size_t)&arg & 0) + (long)(r & 0)) + arg;
        double s = now_ms();
        sink = fib(a);
        double d = now_ms() - s;
        if (d < best) best = d;
    }
    printf("fib30 %.3f\n", best);

    /* sort 500k LCG ints */
    long S = 500000;
    long *buf = malloc(sizeof(long) * S);
    best = 1e18;
    for (int r = 0; r < 3; ++r) {
        unsigned long x = 12345;
        for (long i = 0; i < S; ++i) {
            x = (x * 1103515245u + 12345u) % 2147483648u;
            buf[i] = (long)x;
        }
        double s = now_ms();
        qsort(buf, (size_t)S, sizeof(long), cmp_long);
        double d = now_ms() - s;
        if (d < best) best = d;
        sink = buf[0] + buf[S - 1];
    }
    printf("sort500k %.3f\n", best);
    free(buf);

    /* dict: 500k inserts + 500k lookups */
    long K = 500000;
    best = 1e18;
    for (int r = 0; r < 3; ++r) {
        memset(mused, 0, sizeof mused);
        unsigned long x = 999;
        double s = now_ms();
        for (long i = 0; i < K; ++i) {
            x = (x * 1103515245u + 12345u) % 2147483648u;
            mput((long)x % 1000000, i);
        }
        long acc = 0;
        for (long i = 0; i < K; ++i) {
            x = (x * 1103515245u + 12345u) % 2147483648u;
            acc += mget((long)x % 1000000);
        }
        double d = now_ms() - s;
        if (d < best) best = d;
        sink = acc;
    }
    printf("dict500k %.3f\n", best);

    /* string build: 200k small strings, one join */
    long M = 200000;
    best = 1e18;
    for (int r = 0; r < 3; ++r) {
        char **parts = malloc(sizeof(char *) * M);
        double s = now_ms();
        for (long i = 0; i < M; ++i) {
            char tmp[32];
            int n = snprintf(tmp, sizeof tmp, "item%d", (int)i);
            parts[i] = malloc((size_t)n + 1);
            memcpy(parts[i], tmp, (size_t)n + 1);
        }
        size_t total = 1;
        for (long i = 0; i < M; ++i) total += strlen(parts[i]) + 1;
        char *out = malloc(total);
        size_t off = 0;
        for (long i = 0; i < M; ++i) {
            size_t len = strlen(parts[i]);
            if (i) out[off++] = ',';
            memcpy(out + off, parts[i], len);
            off += len;
            free(parts[i]);
        }
        out[off] = 0;
        double d = now_ms() - s;
        if (d < best) best = d;
        sink = (long)strlen(out);
        free(out);
        free(parts);
    }
    printf("str200k %.3f\n", best);
    return 0;
}
