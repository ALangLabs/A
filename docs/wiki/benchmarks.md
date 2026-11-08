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
  `pipeline.py`, `sqrt.py`, `strings.py`) and C++ (`bench.cpp`) for comparison.

## Running

`tools/benchmark-runner/run.sh [a-binary]` times each `benchmarks/*/*.a`
with `a run` (env `RUNS`, default 3; best time reported);
`tools/benchmark-runner/compare.py` compares result sets. See
[tools-scripts](tools-scripts.md). Related: [Examples](examples.md),
[overview](overview.md).
