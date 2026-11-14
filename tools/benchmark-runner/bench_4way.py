#!/usr/bin/env python3
"""4-way benchmark: A vs C vs C++ vs Python on the benchmarks/ suite.

Measures end-to-end wall time (best of N runs) for A run vs native vs C vs
C++ vs Python, including the matmul workload (blocked ikj in every language).

Usage:
    python3 tools/benchmark-runner/bench_4way.py [--a build/a] [--runs 7] [--json results.json]
    python3 tools/benchmark-runner/bench_4way.py --a-only [--a build/a] [--runs 3]

--a-only times just the A programs (like run.sh did); the default needs gcc,
g++, python3.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# name -> (A source, C/C++ workload key, Python source)
BENCHES = {
    "arithmetic/sum": ("benchmarks/arithmetic/sum.astra", "arithmetic",
                       "benchmarks/equivalents/python/sum.py"),
    "collections/pipeline": ("benchmarks/collections/pipeline.astra", "collections",
                             "benchmarks/equivalents/python/pipeline.py"),
    "memory/strings": ("benchmarks/memory/strings.astra", "memory",
                       "benchmarks/equivalents/python/strings.py"),
    "numerical/sqrt": ("benchmarks/numerical/sqrt.astra", "numerical",
                       "benchmarks/equivalents/python/sqrt.py"),
    "numerical/matmul": ("benchmarks/numerical/matmul.astra", "matmul",
                         "benchmarks/equivalents/python/matmul.py"),
}


def time_cmd(cmd: list[str], runs: int) -> tuple[float, float, str]:
    times: list[float] = []
    output = ""
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        times.append((time.perf_counter() - start) * 1000.0)
        output = proc.stdout.strip()
        if proc.returncode != 0:
            raise RuntimeError(
                f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr}")
    return min(times), statistics.median(times), output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", default=os.path.join(ROOT, "build", "a"))
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--json", default="")
    parser.add_argument("--a-only", action="store_true",
                        help="time only the A programs (no C/C++/Python)")
    args = parser.parse_args()

    if not os.access(args.a, os.X_OK):
        print(f"error: A binary not found at {args.a}", file=sys.stderr)
        return 1

    if args.a_only:
        print(f"{'benchmark':28s} {'best (ms)':>9s} {'output':>10s}")
        for name, (src_a, _k, _p) in BENCHES.items():
            best, _med, out = time_cmd([args.a, "run", os.path.join(ROOT, src_a)],
                                        args.runs)
            print(f"{name:28s} {best:9.2f} {out:>10s}")
        return 0

    tmp = tempfile.mkdtemp(prefix="a-bench4-")

    def build(src, out, flags):
        subprocess.run(flags + [src, "-o", out], check=True, capture_output=True)
        return out

    c_bin = os.path.join(tmp, "bench-c")
    subprocess.run(["gcc", "-O2", "-std=c11",
                    os.path.join(ROOT, "benchmarks", "equivalents", "c", "bench.c"),
                    "-o", c_bin, "-lm"], check=True, capture_output=True)
    cpp_bin = build(os.path.join(ROOT, "benchmarks", "equivalents", "cpp", "bench.cpp"),
                    os.path.join(tmp, "bench-cpp"), ["g++", "-O2", "-std=c++20"])
    py = sys.executable

    native_a = {}
    for name, (src_a, _k, _p) in BENCHES.items():
        bin_a = os.path.join(tmp, name.replace("/", "_") + "_native")
        subprocess.run([args.a, "build", os.path.join(ROOT, src_a), "-o", bin_a],
                       check=True, capture_output=True)
        native_a[name] = bin_a

    results = {}
    print(f"{'benchmark':26s} {'A-run ms':>9s} {'A-nat ms':>9s} {'C ms':>9s} {'C++ ms':>9s} {'Py ms':>9s} {'A nat/C':>8s}  output")
    print("-" * 120)
    for name, (src_a, key, src_py) in BENCHES.items():
        a_run_best, a_run_med, a_out = time_cmd([args.a, "run", os.path.join(ROOT, src_a)], args.runs)
        a_nat_best, a_nat_med, _ = time_cmd([native_a[name]], args.runs)
        c_best, c_med, c_out = time_cmd([c_bin, key, "1"], args.runs)
        cpp_best, cpp_med, cpp_out = time_cmd([cpp_bin, key, "1"], args.runs)
        py_best, py_med, py_out = time_cmd([py, os.path.join(ROOT, src_py)], args.runs)
        results[name] = {
            "a_run": {"best_ms": a_run_best, "median_ms": a_run_med},
            "a_native": {"best_ms": a_nat_best, "median_ms": a_nat_med},
            "c": {"best_ms": c_best, "median_ms": c_med, "output": c_out},
            "cpp": {"best_ms": cpp_best, "median_ms": cpp_med, "output": cpp_out},
            "python": {"best_ms": py_best, "median_ms": py_med, "output": py_out},
            "output": a_out,
        }
        print(f"{name:26s} {a_run_best:9.2f} {a_nat_best:9.2f} {c_best:9.2f} {cpp_best:9.2f} {py_best:9.2f} {a_nat_best/c_best:8.2f}x  {a_out}")

    startup = {}
    with tempfile.NamedTemporaryFile("w", suffix=".astra", delete=False) as fh:
        fh.write("fn main() -> void:\n    print(0)\n\nmain()\n")
        noop = fh.name
    try:
        startup["a_run_ms"] = time_cmd([args.a, "run", noop], args.runs)[0]
    finally:
        os.unlink(noop)
    startup["a_native_ms"] = time_cmd([native_a["arithmetic/sum"]], 1)[0]
    startup["c_ms"] = time_cmd([c_bin, "arithmetic", "0"], args.runs)[0]
    startup["cpp_ms"] = time_cmd([cpp_bin, "arithmetic", "0"], args.runs)[0]
    startup["python_ms"] = time_cmd([py, "-c", "pass"], args.runs)[0]
    print()
    print("startup: " + "  ".join(f"{k} {v:.2f} ms" for k, v in startup.items()))

    if args.json:
        payload = {"runs": args.runs, "benchmarks": results, "startup": startup}
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
