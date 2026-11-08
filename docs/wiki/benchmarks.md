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

End-to-end wall time in ms (lower is better). `A run` uses the content-
addressed run cache (warm: pre-parse depfile check + exec; cold compiles
first), `A native` (`a build`) is the apples-to-apples comparison with C/C++.

| Benchmark | A run | A native | C | C++ | Python | A native ÷ C |
| --- | --- | --- | --- | --- | --- | --- |
| arithmetic/sum (200k) | 6.9 | 2.1 | 1.6 | 2.5 | 44 | 1.4× |
| collections/pipeline (5k) | 6.9 | 2.5 | 1.4 | 2.5 | 31 | 1.7× |
| memory/strings (500×200) | 9.9 | 5.7 | 11.7 | 5.3 | 43 | 0.5× (faster) |
| numerical/sqrt (50k) | 7.3 | 2.9 | 1.8 | 2.8 | 37 | 1.6× |
| numerical/matmul (64³) | 7.2 | 2.5 | 2.5 | 3.8 | 49 | 1.0× (tie) |

(Note: wall numbers fluctuate ±30% under host load; treat ratios, not
the third significant digit, as load-independent.)

Takeaways: A **native** ties or beats C++ on all five workloads,
matches/beats C on string joining (2×) and matmul, and runs ~20×
faster than pure Python. It trails C by ~1.5–1.6× on integer loops
and `sqrt`, where the gap is a handful of remaining slot round-trips
in the hot loops (a fully SSA/register-allocating backend would
close it) plus a ~0.3 ms higher process startup (noop binary:
1.1 ms vs C's 0.8 ms). A-run startup shrank from ~25 ms to ~6–8 ms
with the binary + depfile caches.
