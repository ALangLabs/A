# Performance

How A is measured, what the current numbers are, and how to reproduce them.

## Methodology

All figures come from [`tools/benchmark-runner/compare.py`](../../tools/benchmark-runner/compare.py),
which times the same algorithm implemented in A, CPython, and C++:

- **A** — `a build` native executable (best of N wall-clock runs). The A source
  is lowered to x86-64 assembly and assembled/linked by the staged `astra`
  backend together with the C runtime; no `g++` step is involved.
- **C++** — one translation unit (`benchmarks/equivalents/cpp/bench.cpp`)
  compiled once with `g++ -O2 -std=c++20`; `volatile` bounds keep the
  optimizer from folding the work away.
- **Python** — the matching CPython script under `benchmarks/equivalents/python/`.

```sh
python3 tools/benchmark-runner/compare.py --native --runs 5
python3 tools/benchmark-runner/compare.py --native --runs 5 --json results.json
```

`--native` measures the compiled A binary's runtime. Without it, `a run`
measures the full compile-and-execute path and is reported separately as a
startup/compile baseline, not a parity number.

## Latest results

Measured on an AMD Ryzen 5 7520U (x86-64, Linux), g++ 13.3.0, CPython 3.14.9,
best of 5 runs:

| Workload | A (`a build`) | C++ (`-O2`) | CPython 3.14 | A vs C++ |
| :--- | ---: | ---: | ---: | ---: |
| `arithmetic/sum` — 200k range additions | 2.22 ms | 2.42 ms | 43.65 ms | 0.9× |
| `collections/pipeline` — 5k-element filter/map/sum | 2.30 ms | 2.66 ms | 24.63 ms | 0.9× |
| `memory/strings` — 100k `str()` + join | 5.48 ms | 4.91 ms | 38.95 ms | 1.1× |
| `numerical/sqrt` — 50k `sqrt` + `round` | 2.63 ms | 3.19 ms | 34.02 ms | 0.8× |

Startup (wall time to launch a no-op program):

| Path | Time |
| :--- | ---: |
| A native binary (`a build` output) | ~2.4 ms |
| C++ binary (`g++ -O2` output) | ~2.4 ms |
| `a run` / `a build` (compile + execute a no-op) | ~40 ms |
| `python3 -c pass` | ~40 ms |

The `a run`/`a build` figure is end-to-end toolchain time (C++ frontend →
x86-64 assembly → astra assemble/link), comparable to `cargo build`-style
invocations rather than to process startup.

## Targets

- Parity gates apply to native `a build` binaries only (≤ ~1.1× of `g++ -O2`
  on the suite above).
- `a run`/`a build` numbers include toolchain compile time and are tracked
  separately; steady-state runtime parity is measured on `a build` output.
- Regressions should be reported with the same environment notes (CPU, OS,
  compiler versions, flags, run counts) as the table above.
