// C++ equivalents of the A benchmark suite.
//
// Build:  g++ -O2 -std=c++20 bench.cpp -o bench-cpp
// Usage:  ./bench-cpp <arithmetic|collections|memory|numerical|matmul> <reps>
//
// The workload bounds are volatile so the optimizer cannot constant-fold the
// whole benchmark away; the result is printed and stored in a volatile sink.
// <reps> repeats the single-benchmark workload for stable timing.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static volatile long long g_sink;

long long arithmetic(int n) {
    long long total = 0;
    for (int i = 0; i < n; ++i) total += i;
    return total;
}

long long collections(int n) {
    std::vector<long long> data;
    data.reserve(n);
    for (int i = 0; i < n; ++i) data.push_back(i);
    long long total = 0;
    for (long long v : data)
        if (v % 3 == 0) total += v * 2;
    return total;
}

long long memory(int rounds, int inner) {
    long long total = 0;
    for (int r = 0; r < rounds; ++r) {
        std::vector<std::string> parts;
        parts.reserve(inner);
        for (int n = 0; n < inner; ++n) parts.push_back(std::to_string(n));
        std::string joined;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) joined += ',';
            joined += parts[i];
        }
        total += static_cast<long long>(joined.size());
    }
    return total;
}

double numerical(int n) {
    double acc = 0.0;
    for (int i = 1; i < n; ++i) acc += std::sqrt(static_cast<double>(i));
    return acc;
}

double matmul(int n) {
    std::vector<double> a((size_t)n * n), b((size_t)n * n), c((size_t)n * n, 0.0);
    std::fill(a.begin(), a.end(), 1.5);
    std::fill(b.begin(), b.end(), 2.5);
    const int TILE = 32;
    for (int i = 0; i < n; ++i) {
        double* orow = c.data() + (size_t)i * n;
        const double* arow = a.data() + (size_t)i * n;
        for (int jj = 0; jj < n; jj += TILE) {
            int jend = jj + TILE < n ? jj + TILE : n;
            for (int t = 0; t < n; ++t) {
                double av = arow[t];
                const double* brow = b.data() + (size_t)t * n;
                for (int j = jj; j < jend; ++j)
                    orow[j] = std::fma(av, brow[j], orow[j]);
            }
        }
    }
    return c[0];
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bench-cpp <arithmetic|collections|memory|numerical|matmul> <reps>\n");
        return 64;
    }
    std::string which = argv[1];
    int reps = std::atoi(argv[2]);

    volatile int an = 200000, cn = 5000, mr = 500, mi = 200, nn = 50000, mn = 64;
    auto t0 = std::chrono::steady_clock::now();
    if (which == "arithmetic") {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = arithmetic(an);
        g_sink = v;
        std::printf("%lld\n", v);
    } else if (which == "collections") {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = collections(cn);
        g_sink = v;
        std::printf("%lld\n", v);
    } else if (which == "memory") {
        long long v = 0;
        for (int r = 0; r < reps; ++r) v = memory(mr, mi);
        g_sink = v;
        std::printf("%lld\n", v);
    } else if (which == "numerical") {
        double v = 0;
        for (int r = 0; r < reps; ++r) v = numerical(nn);
        g_sink = static_cast<long long>(v);
        std::printf("%.1f\n", std::round(v));
    } else if (which == "matmul") {
        double v = 0;
        for (int r = 0; r < reps; ++r) v = matmul(mn);
        g_sink = static_cast<long long>(v);
        std::printf("%.1f\n", v);
    } else {
        return 64;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::fprintf(stderr, "%.4f\n", ms / reps);
    return 0;
}
