#!/usr/bin/env python3
"""Compare A std:http against Python http.client and a raw-socket C++ client.

Same workload for all three: N sequential GETs, one fresh TCP connection per
request, headers + Content-Length body read, body length accumulated. The
Python and C++ sides also report a keep-alive row, which A cannot do yet
(std:http has no connection pooling), so those rows show the pooling ceiling.

Usage: python3 benchmarks/http/run.py [--reps 5]
Requires: build/a, g++ (for the C++ column), python3.
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
A = os.environ.get("A_BIN") or os.path.join(ROOT, "build", "a")
PORT = 8901
WORKLOADS = [("/tiny", 300), ("/k16", 200), ("/m1", 20), ("/m10", 5)]

A_SRC = """from std:http import get
import std:time

N = %d
PATH = "%s"

fn main():
    url = "http://127.0.0.1:%d" + PATH
    warm = get(url, None, None, 30000)
    print("warm=" + str(len(warm.body) + len(warm.body_bytes)))
    total = 0
    t0 = time.now()
    i = 0
    while i < N:
        r = get(url, None, None, 30000)
        total = total + len(r.body) + len(r.body_bytes)
        i = i + 1
    t1 = time.now()
    print("TIME {t1 - t0} {total}")
"""

def a_bench(path, n, tmp):
    src = os.path.join(tmp, "bench_lit.astra")
    with open(src, "w") as f:
        f.write(A_SRC % (n, path, PORT))
    out = subprocess.run([A, "run", src], capture_output=True, text=True, check=True).stdout
    line = [l for l in out.splitlines() if l.startswith("TIME")][0].split()
    return float(line[1]), int(line[2])

def other(cmd, extra_env):
    env = dict(os.environ, BENCH_PORT=str(PORT), **extra_env)
    out = subprocess.run(cmd, env=env, capture_output=True, text=True, check=True).stdout.split()
    return float(out[0]), int(out[1])

def py_bench(path, n, ka=False):
    e = {"BENCH_PATH": path, "BENCH_N": str(n)}
    if ka:
        e["BENCH_KEEPALIVE"] = "1"
    return other([sys.executable, os.path.join(HERE, "bench.py")], e)

def cpp_bench(binary, path, n, ka=False):
    e = {"BENCH_PATH": path, "BENCH_N": str(n)}
    if ka:
        e["BENCH_KEEPALIVE"] = "1"
    return other([binary], e)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    args = ap.parse_args()

    binary = os.path.join(tempfile.gettempdir(), "a_http_bench_cpp")
    subprocess.run(["g++", "-O2", "-o", binary, os.path.join(HERE, "bench.cpp")], check=True)
    server = subprocess.Popen([sys.executable, os.path.join(HERE, "srv.py"), str(PORT)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp = tempfile.mkdtemp(prefix="a_httpbench_")
    try:
        import time as _t
        _t.sleep(1.5)  # let the server bind
        print("%-7s %-5s %-20s %10s %12s %10s" % ("path", "n", "client", "total ms", "ms/req", "MB/s"))
        for path, n in WORKLOADS:
            cases = [
                ("A (std:http)", lambda: a_bench(path, n, tmp)),
                ("Python", lambda: py_bench(path, n)),
                ("C++ raw", lambda: cpp_bench(binary, path, n)),
            ]
            # Keep-alive only for bulk transfers: on tiny replies Python's
            # http.server interacts with delayed ACKs and adds a 40 ms stall
            # per request, which would measure the fixture, not the client.
            if path in ("/m1", "/m10"):
                cases += [
                    ("Python keep-alive", lambda: py_bench(path, n, True)),
                    ("C++ keep-alive", lambda: cpp_bench(binary, path, n, True)),
                ]
            for label, fn in cases:
                best = min(fn() for _ in range(args.reps))
                ms, total = best
                print("%-7s n=%-3d %-20s %10.1f %12.3f %10.2f" %
                      (path, n, label, ms, ms / n, (total / 1e6) / (ms / 1000.0)), flush=True)
    finally:
        server.terminate()

if __name__ == "__main__":
    main()
