#!/usr/bin/env python3
"""Cross-language benchmark, Python side (CPython 3.14)."""
import time

N = 20_000_000


def best_of(fn, reps=3):
    best = 1e18
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        d = (time.perf_counter() - t0) * 1000.0
        if d < best:
            best = d
    return best


def int_loop():
    s = 0
    i = 0
    while i < N:
        s = (s * 31 + i) % 1000000007
        i += 1


def float_loop():
    x = 1.0
    i = 0
    while i < N:
        x = x * 1.000001 + 0.5
        i += 1


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


S = 500_000


def gen_arr():
    xs = []
    x = 12345
    for _ in range(S):
        x = (x * 1103515245 + 12345) % 2147483648
        xs.append(x)
    return xs


def sort_run():
    xs = gen_arr()
    t0 = time.perf_counter()
    xs.sort()
    return (time.perf_counter() - t0) * 1000.0


K = 500_000


def dict_run():
    m = {}
    x = 999
    t0 = time.perf_counter()
    for i in range(K):
        x = (x * 1103515245 + 12345) % 2147483648
        m[x % 1000000] = i
    acc = 0
    for _ in range(K):
        x = (x * 1103515245 + 12345) % 2147483648
        acc += m.get(x % 1000000, 0)
    return (time.perf_counter() - t0) * 1000.0


M = 200_000


def str_run():
    t0 = time.perf_counter()
    parts = ["item" + str(i) for i in range(M)]
    out = ",".join(parts)
    return (time.perf_counter() - t0) * 1000.0


def main():
    print("int_loop %.3f" % best_of(int_loop))
    print("float_loop %.3f" % best_of(float_loop))
    print("fib30 %.3f" % best_of(lambda: fib(30)))
    print("sort500k %.3f" % min(sort_run() for _ in range(3)))
    print("dict500k %.3f" % min(dict_run() for _ in range(3)))
    print("str200k %.3f" % min(str_run() for _ in range(3)))


main()
