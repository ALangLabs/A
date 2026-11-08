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

## Results (Nov 2026, Ryzen 5 7520U, best of 7–9)

End-to-end wall time in ms (lower is better). `A run` uses the content-
addressed run cache (warm: ~6 ms incl. frontend; cold compiles first),
`A native` (`a build`) is the apples-to-apples comparison with C/C++.

| Benchmark | A run | A native | C | C++ | Python | A native ÷ C |
| --- | --- | --- | --- | --- | --- | --- |
| arithmetic/sum (200k) | 7.5 | 2.6 | 1.4 | 2.3 | 46.2 | 1.8× |
| collections/pipeline (5k) | 7.3 | 2.4 | 1.4 | 2.4 | 26.8 | 1.8× |
| memory/strings (500×200) | 10.1 | 5.2 | 11.9 | 5.0 | 44.0 | 0.4× (faster) |
| numerical/sqrt (50k) | 7.8 | 2.9 | 1.9 | 2.6 | 37.8 | 1.6× |
| numerical/matmul (64³) | 6.9 | 2.3 | 2.3 | 3.4 | 48.7 | 1.0× (tie) |

Takeaways: A **native** is within ~2× of C on integer loops and
`sqrt`, ties C++/beats C on string joining, and beats the equivalent
C/C++ tile loop on the blocked-FMA `matmul` while running ~21× faster
than pure Python. A-run startup shrank from ~25 ms to ~6 ms with the
binary cache. The remaining native gap vs C is per-iteration slot
traffic and process startup (noop binary: 1.1 ms vs C's 0.8 ms);
matching C byte-for-byte would need a register-allocating backend.
