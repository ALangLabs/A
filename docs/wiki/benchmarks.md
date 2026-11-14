# Benchmarks

`benchmarks/` holds small A programs for timing, plus reference equivalents in
other languages.

## Layout

- `benchmarks/arithmetic/sum.astra` — integer accumulation loop.
- `benchmarks/collections/pipeline.astra` — collection pipeline workload.
- `benchmarks/memory/strings.astra` — string/memory workload.
- `benchmarks/numerical/sqrt.astra` — floating-point workload.
- `benchmarks/numerical/matmul.astra` — native blocked `matmul` workload (64x64x64).
- `benchmarks/equivalents/` — same workloads in Python (`sum.py`,
  `pipeline.py`, `sqrt.py`, `strings.py`, `matmul.py`), C (`bench.c`),
  and C++ (`bench.cpp`) for comparison.
- `benchmarks/lang/` — cross-language (A/C/Rust/Python/Node) loop benchmarks:
  `benchmarks/lang/run.py`.
- `benchmarks/http/` — HTTP client benchmark (A/`std:http` vs Python vs raw
  C++ sockets): `benchmarks/http/run.py`.

## Running

`tools/benchmark-runner/bench_4way.py --a-only` times each `benchmarks/*/*.a`
with `a run` (no other languages needed);
`tools/benchmark-runner/bench_4way.py` compares all four (A run vs
native vs C vs C++ vs Python) and writes `--json` results.
Related: [Examples](examples.md),
[overview](overview.md).

## Results (Nov 2026, Ryzen 5 7520U, best of 7)

Historical snapshot from one reference machine — hardware- and
load-dependent, so treat the ratios as indicative rather than guaranteed.
Rerun `tools/benchmark-runner/bench_4way.py` for current numbers on your
own hardware.

End-to-end wall time in ms (lower is better). `A run` uses the content-
addressed run cache (warm: pre-parse depfile check + exec; cold compiles
first), `A native` (`a build`) is the apples-to-apples comparison with C/C++.

| Benchmark | A run | A native | C | C++ | Python | A native ÷ C |
| --- | --- | --- | --- | --- | --- | --- |
| arithmetic/sum (200k) | 6.7 | 1.8 | 1.1 | 2.1 | 45 | 1.7× |
| collections/pipeline (5k) | 6.9 | 2.2 | 1.1 | 2.2 | 24 | 2.1× |
| memory/strings (500×200) | 9.0 | 4.9 | 11.5 | 5.0 | 40 | 0.4× (faster) |
| numerical/sqrt (50k) | 7.2 | 2.6 | 2.0 | 2.6 | 35 | 1.3× |
| numerical/matmul (64³) | 6.0 | 2.0 | 2.3 | 3.2 | 47 | 0.9× (faster) |

(Note: wall numbers fluctuate ±30% under host load; treat ratios, not
the third significant digit, as load-independent.)

Takeaways: A **native** beats or ties C++ on all five workloads
(arithmetic and strings best), and beats C on string joining (2×) and
matmul. It trails C by ~1.3–2× on integer loops and `sqrt`, where the
gap is a handful of remaining slot round-trips in the hot loops (a fully
SSA/register-allocating backend would close it) plus a ~0.3 ms higher
process startup (noop binary: 1.1 ms vs C's 0.8 ms). A-run startup
shrank from ~25 ms to ~6–8 ms with the binary + depfile caches.
