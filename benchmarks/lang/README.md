# Cross-language benchmarks: A vs C vs Rust vs Python vs Node

Same workload, same sizes, same algorithm in every language; each program
reports its own **best of 3** runs, and the runner repeats the whole program
`--reps` times and keeps the fastest number per benchmark.

```sh
python3 benchmarks/lang/run.py --reps 3          # full table
A_BIN=build/a python3 benchmarks/lang/run.py     # pick the A binary
```

Each language is compiled/run the way it is normally used: `gcc -O2`,
`rustc -O`, CPython 3.14, Node 24 (JIT warmed), `a run` (A compiles to a native
executable). Loops carry a serial data dependency (`s = (s*31 + i) % 1000000007`)
so no compiler can turn them into a closed form or vectorize them away, and
`fib` is called through an opaque boundary so repeated reps cannot be
memoized. Where a language needed help to stay honest (an opaque `fib`
argument in C and Rust), the comment in the source says so.

## Results (x86-64, loopback, 8-core box, best of 3 × 3)

ms, lower is better:

| benchmark | what it does | A | C | Rust | Python | Node | A/C | A/Python |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `int_loop` | 20M iterations of `s = (s*31+i) % 1e9+7` | 414 | 102 | 111 | 2559 | 390 | **4.1x** | **0.16x** |
| `float_loop` | 20M iterations of `x = x*1.000001 + 0.5` | 179 | 43.6 | 43.6 | 1264 | 43.6 | **4.1x** | **0.14x** |
| `fib30` | recursive `fib(30)` (1.6M calls) | 34.0 | 2.1 | 3.5 | 138 | 12.1 | 16.4x | **0.25x** |
| `sort500k` | sort 500k LCG ints | 122 | 70.7 | 11.7 | 242 | 277 | 1.7x | **0.50x** |
| `dict500k` | 500k inserts + 500k lookups | 406 | 21.6 | 83.7 | 602 | 65.4 | 18.8x | **0.67x** |
| `str200k` | build 200k strings, one join | 53 | 30.4 | 21.3 | 36.9 | 26.9 | 1.7x | 1.4x |

Absolute numbers move 2-3x with machine load (this box builds in the
background); the ratios are stable across runs.

I/O-bound work is a different story — the HTTP client in
[`../http`](../http/README.md) reaches 388 MB/s on 1 MiB and 610 MB/s on 10 MiB
bodies (C++ raw sockets: 807 / 1542). Comparing the same TCP read into A's
`Bytes` type vs an `[i64]` array: 1000 MB/s vs 83 MB/s.

## What the numbers mean

**A is 6-7x faster than Python on numeric loops and 2x on sorting**, and it is
1.7-4x off C for everything that is arithmetic or algorithm. Two workloads are
far off, and both are structural rather than tuning:

1. **`fib30` — 16x off C.** A function call costs ~20 ns vs C's ~3 ns. Each
   call boxes the frame, resolves parameters and dispatches through `aval`
   values.
2. **`dict500k` — 19x off C (4.8x off Rust's `HashMap`).** A's `dict` is
   generic: `aval` keys, cached mixed hashes, `values_equal` on every probe,
   insertion-order `pairs[]` plus a separate slot table. C's number here is a
   purpose-built int→int open-addressing table (best case); Rust's SipHash
   `HashMap` is the fair "library" comparison at 84 ms.
3. **`int_loop` / `float_loop` — 4.1x off C.** Indexed locals live in the
   frame, each arithmetic op re-checks the `aval` tags, and A's inline integer
   range is 47 bits: values outside ±7e13 are heap-boxed (see below).

### What this suite already fixed

| finding | effect |
|---|---|
| `mkint_full` `xmalloc`ed a heap box for every integer outside the 47-bit inline range | a 32-bit LCG step (`x*1103515245`) leaves the range on every iteration; int boxes now come from a 64 KiB bump arena: 541 ms → 156 ms (**3.5x**) on 2M iterations |
| `list.sort()` was an insertion sort | 200k ints: 230 s → 0.03 s (**7700x**) |
| `str.contains` scanned with `memcmp` per offset | 200 KB miss: 1.7 ms → 0.006 ms (**280x**) |
| HTTP bodies were materialised as `[i64]` (8 B/byte) | 1 MiB body 94 → 388 MB/s, and the `Bytes` type took the raw read from 83 → 1000 MB/s |
| `json.parse` grew strings byte by byte | 820 KB parses in 42 ms, faster than CPython's C parser (73 ms) |

## Fair-comparison notes

- `dict500k`: C uses a hand-rolled open-addressing int→int table (21.6 ms);
  Rust uses `std::collections::HashMap` with SipHash (84 ms). A's `dict` is a
  general-purpose ordered map and is compared against both.
- `str200k`: the C version builds the result with an offset copy (a naive
  `strcat` loop is quadratic — 3.6 s — and would flatter A unfairly).
- Python's `str200k` and Node's sort are competitive because those paths are
  implemented in C inside the runtime (CPython's `str.join`, V8's sort with a
  comparator callback).
- No benchmark here is dominated by startup, JIT warm-up or I/O; each is timed
  around the hot loop only, except where the table says otherwise.
