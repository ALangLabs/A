# Benchmarks

`benchmarks/` holds small A programs for timing, plus reference equivalents in
other languages.

## Layout

- `benchmarks/arithmetic/sum.a` — integer accumulation loop.
- `benchmarks/collections/pipeline.a` — collection pipeline workload.
- `benchmarks/memory/strings.a` — string/memory workload.
- `benchmarks/numerical/sqrt.a` — floating-point workload.
- `benchmarks/numerical/matmul.a` — native blocked `matmul` workload (64x64x64).
- `benchmarks/equivalents/` — same workloads in Python (`sum.py`,
  `pipeline.py`, `sqrt.py`, `strings.py`, `matmul.py`), C (`bench.c`),
  and C++ (`bench.cpp`) for comparison.

## Running

`tools/benchmark-runner/run.sh [a-binary]` times each `benchmarks/*/*.a`
with `a run` (env `RUNS`, default 3; best time reported);
`tools/benchmark-runner/compare.py` compares A vs C++ vs Python;
`tools/benchmark-runner/bench_4way.py` compares all four (A run vs
native vs C vs C++ vs Python) and writes `--json` results. See
[tools-scripts](tools-scripts.md). Related: [Examples](examples.md),
[overview](overview.md).

## Results (Nov 2026, Ryzen 5 7520U, best of 7)

End-to-end wall time in ms (lower is better). `A run` pays the
~23–28 ms frontend+compile startup each invocation; `A native`
(`a build`) is the apples-to-apples comparison with C/C++.

| Benchmark | A run | A native | C | C++ | Python | A native ÷ C |
| --- | --- | --- | --- | --- | --- | --- |
| arithmetic/sum (200k) | 26.0 | 2.9 | 1.2 | 2.2 | 45.7 | 2.5× slower |
| collections/pipeline (5k) | 24.1 | 2.3 | 1.2 | 2.4 | 24.4 | 1.9× slower |
| memory/strings (500×200) | 28.4 | 5.3 | 11.6 | 5.0 | 39.6 | 0.5× (faster) |
| numerical/sqrt (50k) | 25.5 | 3.0 | 1.5 | 2.5 | 34.5 | 2.0× slower |
| numerical/matmul (64³) | 25.4 | 1.9 | 2.1 | 3.4 | 49.4 | 0.9× (faster) |

Takeaways: A **native** is within ~2× of C/C++ on loops/math,
matches C++ (and beats naive C) on string joining, and the
blocked-FMA `matmul` builtin beats the equivalent C/C++ tile
loop while being ~26× faster than pure Python. Python is fastest
only at startup-heavy tiny jobs (`python -c pass` ≈ 25 ms vs
`a run` ≈ 23 ms here). `A run` numbers are dominated by per-run
compile startup — always use `a build` binaries for CPU comparisons.
