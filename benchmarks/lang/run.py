#!/usr/bin/env python3
"""Run the cross-language benchmark suite (A vs C vs Rust vs Python vs Node).

Usage: python3 benchmarks/lang/run.py [--reps N] [A_BIN=/path/to/a]
Each language runs its own best-of-3; the runner repeats the whole program
`reps` times and keeps the fastest number per benchmark.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
A = os.environ.get("A_BIN") or os.path.join(ROOT, "build", "a")

LANGS = ["A", "C", "Rust", "Python", "Node"]
BENCHES = ["int_loop", "float_loop", "fib30", "sort500k", "dict500k", "str200k"]


def build(tmp):
    """Compile the native sides once; return {lang: argv}."""
    cmds = {}
    c_bin = os.path.join(tmp, "bench_c")
    subprocess.run(["gcc", "-O2", "-o", c_bin, os.path.join(HERE, "bench.c")], check=True)
    cmds["C"] = [c_bin]
    rs_bin = os.path.join(tmp, "bench_rs")
    subprocess.run(["rustc", "-O", "-o", rs_bin, os.path.join(HERE, "bench.rs")], check=True)
    cmds["Rust"] = [rs_bin]
    cmds["A"] = [A, "run", os.path.join(HERE, "bench.astra")]
    cmds["Python"] = [sys.executable, os.path.join(HERE, "bench.py")]
    cmds["Node"] = ["node", os.path.join(HERE, "bench.js")]
    return cmds


def one_run(argv):
    out = subprocess.run(argv, capture_output=True, text=True, timeout=1800)
    if out.returncode != 0:
        raise SystemExit(f"failed: {' '.join(argv)}\n{out.stderr[-2000:]}")
    vals = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in BENCHES:
            try:
                vals[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return vals


def main():
    reps = 2
    if "--reps" in sys.argv:
        reps = int(sys.argv[sys.argv.index("--reps") + 1])
    with tempfile.TemporaryDirectory() as tmp:
        cmds = build(tmp)
        best = {lang: {} for lang in LANGS}
        for _ in range(reps):
            for lang in LANGS:
                vals = one_run(cmds[lang])
                for k, v in vals.items():
                    cur = best[lang].get(k)
                    if cur is None or v < cur:
                        best[lang][k] = v

    # table
    w = 12
    head = f"{'benchmark':<11}" + "".join(f"{l:>{w}}" for l in LANGS) + f"{'A/C':>{8}}{'A/Py':>{8}}"
    print(head)
    print("-" * len(head))
    for b in BENCHES:
        avail = [best[l].get(b) for l in LANGS]
        if avail[0] is None:
            continue
        row = f"{b:<11}"
        for v in avail:
            row += f"{v:>{w}.1f}" if v is not None else f"{'-':>{w}}"
        c = best["C"].get(b)
        py = best["Python"].get(b)
        row += f"{(best['A'][b] / c if c else 0):>{7}.2f}x"
        row += f"{(best['A'][b] / py if py else 0):>{7}.2f}x"
        print(row)
    print("\nms, lower is better. A/C and A/Py < 1.0 means A is faster.")


main()
