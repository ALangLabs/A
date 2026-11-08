// C equivalents of the A benchmark suite.
//
// Build:  gcc -O2 -std=c11 bench.c -o bench-c -lm
// Usage:  ./bench-c <arithmetic|collections|memory|numerical|matmul> <reps>
//
// The workload bounds are volatile so the optimizer cannot constant-fold the
// whole benchmark away; the result is printed and stored in a volatile sink.
// <reps> repeats the single-benchmark workload for stable timing.
// Timings are printed to stderr as ms/rep.

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static volatile long long g_sink;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

long long arithmetic(int n) {
    long long total = 0;
    for (int i = 0; i < n; ++i) total += i;
    return total;
}

long long collections(int n) {
    long long total = 0;
    for (int i = 0; i < n; ++i)
        if (i % 3 == 0) total += (long long)i * 2;
    return total;
}

long long memory(int rounds, int inner) {
    // Same work as the A workload: build 0..inner as strings and join.
    long long total = 0;
    char **parts = (char **)malloc((size_t)inner * sizeof(char *));
    if (!parts) return 0;
    for (int r = 0; r < rounds; ++r) {
        size_t cap = 64, len = 0;
        char *joined = (char *)malloc(cap);
        if (!joined) break;
        joined[0] = '\0';
        for (int n = 0; n < inner; ++n) {
            char buf[16];
            int bl = snprintf(buf, sizeof buf, "%d", n);
            parts[n] = (char *)malloc((size_t)bl + 1);
            if (!parts[n]) continue;
            memcpy(parts[n], buf, (size_t)bl + 1);
            size_t need = len + (n ? 1 : 0) + (size_t)bl + 1;
            if (need > cap) {
                while (need > cap) cap *= 2;
                joined = (char *)realloc(joined, cap);
                if (!joined) break;
            }
            if (n) joined[len++] = ',';
            memcpy(joined + len, buf, (size_t)bl);
            len += (size_t)bl;
            joined[len] = '\0';
        }
        total += (long long)len;
        free(joined);
        for (int n = 0; n < inner; ++n) free(parts[n]);
    }
    free(parts);
    return total;
}

double numerical(int n) {
    double acc = 0.0;
    for (int i = 1; i < n; ++i) acc += sqrt((double)i);
    return acc;
}

// Same blocked ikj layout as the A runtime matmul (linalg.c):
// output row stays cache-hot, B rows stream, fma accumulate.
double matmul(int n) {
    double *a = (double *)malloc((size_t)n * n * sizeof(double));
    double *b = (double *)malloc((size_t)n * n * sizeof(double));
    double *c = (double *)calloc((size_t)n * n, sizeof(double));
    if (!a || !b || !c) { free(a); free(b); free(c); return 0.0; }
    for (int i = 0; i < n * n; ++i) { a[i] = 1.5; b[i] = 2.5; }
    const int TILE = 32;
    for (int i = 0; i < n; ++i) {
        double *orow = c + (size_t)i * n;
        const double *arow = a + (size_t)i * n;
        for (int jj = 0; jj < n; jj += TILE) {
            int jend = jj + TILE < n ? jj + TILE : n;
            for (int t = 0; t < n; ++t) {
                double av = arow[t];
                const double *brow = b + (size_t)t * n;
                for (int j = jj; j < jend; ++j)
                    orow[j] = fma(av, brow[j], orow[j]);
            }
        }
    }
    double s = c[0];
    free(a); free(b); free(c);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: bench-c <arithmetic|collections|memory|numerical|matmul> <reps>\n");
        return 64;
    }
    const char *which = argv[1];
    int reps = atoi(argv[2]);
    volatile int an = 200000, cn = 5000, mr = 500, mi = 200, nn = 50000, mn = 64;
    double t0 = now_ms();
    if (strcmp(which, "arithmetic") == 0) {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = arithmetic(an);
        g_sink = v;
        printf("%lld\n", v);
    } else if (strcmp(which, "collections") == 0) {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = collections(cn);
        g_sink = v;
        printf("%lld\n", v);
    } else if (strcmp(which, "memory") == 0) {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = memory(mr, mi);
        g_sink = v;
        printf("%lld\n", v);
    } else if (strcmp(which, "numerical") == 0) {
        double v = 0;
        for (int r = 0; r < reps; ++r) v = numerical(nn);
        g_sink = (long long)v;
        printf("%.1f\n", round(v));
    } else if (strcmp(which, "matmul") == 0) {
        double v = 0;
        for (int r = 0; r < reps; ++r) v = matmul(mn);
        g_sink = (long long)v;
        printf("%.1f\n", v);
    } else {
        return 64;
    }
    double ms = now_ms() - t0;
    fprintf(stderr, "%.4f\n", ms / reps);
    return 0;
}
