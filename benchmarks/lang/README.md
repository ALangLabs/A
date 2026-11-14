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
| `int_loop` | 20M iterations of `s = (s*31+i) % 1e9+7` | 171 | 102 | 111 | 2561 | 390 | **1.68x** | **0.07x** |
| `float_loop` | 20M iterations of `x = x*1.000001 + 0.5` | 211 | 43.6 | 43.6 | 1274 | 43.7 | **4.8x** | **0.17x** |
| `fib30` | recursive `fib(30)` (1.6M calls) | 35.0 | 2.1 | 3.5 | 138 | 12.1 | 16.9x | **0.25x** |
| `sort500k` | sort 500k LCG ints | 118 | 71.7 | 11.4 | 246 | 300 | 1.65x | **0.48x** |
| `dict500k` | 500k inserts + 500k lookups | 255 | 22.4 | 84.8 | 628 | 65.3 | 11.4x | **0.41x** |
| `str200k` | build 200k strings, one join | 33 | 32.2 | 21.2 | 38.5 | 31.3 | 1.03x | **0.86x** |

Absolute numbers move 2-3x with machine load (this box builds in the
background); the ratios are stable across runs.

I/O-bound work is a different story — the HTTP client in
[`../http`](../http/README.md) reaches 599 MB/s on 1 MiB and 699 MB/s on 10 MiB
bodies on a keep-alive connection, and is faster than CPython on small
requests. Comparing the same TCP read into A's `Bytes` type vs an `[i64]`
array: 1000 MB/s vs 83 MB/s.

## What the numbers mean

**A is 14x faster than Python on integer loops, 1.9x on sorting, 1.2x on
string building and 2.4x on dict churn**, and it is 1.6-1.7x off C for
arithmetic/algorithm code — and at parity with C on string building. Three workloads are still far off, and all three are
structural rather than tuning:

1. **`fib30` — 16.9x off C.** A function call costs ~20 ns vs C's ~3 ns. Each
   call boxes the frame, resolves parameters and dispatches through `aval`
   values. This is the one place where a dynamic-language calling convention is
   still what runs.
2. **`dict500k` — 11.4x off C (3x off Rust's `HashMap`).** A's `dict` is
   generic: `aval` keys, a hash tag packed into the slot word, insertion-order
   `pairs[]` plus a separate open-addressed table, so a probe touches two cache
   lines where C's purpose-built int→int table touches one. 500k entries is
   ~16 MB, so this is a memory-bound measurement as much as an algorithmic one.
3. **`float_loop` — 4.9x off C.** Integer locals now run on the raw int64 path
   (see below), but floats still round-trip through `aval` slots: each `+`/`*`
   reloads both operands from the frame and canonicalizes NaN. Keeping a
   loop-carried double in an xmm register is the next structural fix.

`str200k` is now at parity with C (33 vs 32 ms) and 1.16x faster than CPython:
string building is no longer a weak spot.

### What this suite already fixed

| finding | effect |
|---|---|
| `mkint_full` `xmalloc`ed a heap box for every integer outside the 47-bit inline range | a 32-bit LCG step (`x*1103515245`) leaves the range on every iteration; int boxes now come from a 64 KiB bump arena: 541 ms → 156 ms (**3.5x**) on 2M iterations |
| loop counters never got a proven int64 range | an implicit `N = 20_000_000` local was never bound into the range env, a nested loop's recording pass was suppressed by the enclosing fixpoint, and a non-converged fixpoint poisoned the counter to "unknown"; range facts now survive all three, so `int_loop` runs on the raw int64 path: 414 ms → 171 ms (**2.4x**) |
| `+ - *` range propagation dropped to unknown on any possible overflow | saturating to the int64 extremes keeps the fact usable (a saturated interval is never accepted as raw-safe), which is what lets `(x*k+c) % 2^31` prove out |
| every dict probe loaded a separate `phash[]` array | the slot word carries a 32-bit hash tag, so one load rejects collisions and locates the pair: `dict500k` 406 ms → 255 ms (**1.6x**) |
| raw-op register cache let one slot claim a register another operand then overwrote | fixed (`rawLearn` drops the register's other claims); this was miscompiling `while i < N` into `N < N` once counters were promoted |
| float fast path spilled each intermediate to the frame | the register cache stays live across a float operand tree, and NaN canonicalization is one branch: `float_loop` 20M 228 ms → 179 ms (**1.3x** alone, measured without the suite's larger frame) |
| `len(str)` re-scanned the whole string for UTF-8 code points on every call | the count is cached per string in the unused high half of the header word (Python caches it too): `len(300 B)` 5M calls 585 ms → 98 ms (**6x**) |
| `str + str` / `str + int` went through a growable Buf (`format_into` reformatted a value that was already a string, then `buf_take` copied again) | both shapes build one exact-size string and copy twice: `"item" + str(i)` 200k 45 ms → 25 ms (**1.8x**) |
| every allocation touched fresh 4 KiB pages | A never frees values, so allocation-heavy loops fault continuously; arena chunks now come from `mmap` + `MADV_HUGEPAGE` (2 MiB) and int boxes use a 2 MiB chunk. On this box a minor fault costs ~5 us, so this is what `str200k` needed most: 49 ms → 30-33 ms (**1.5x**), i.e. parity with C |
| `list.sort()` was an insertion sort | 200k ints: 230 s → 0.03 s (**7700x**) |
| `str.contains` scanned with `memcmp` per offset | 200 KB miss: 1.7 ms → 0.006 ms (**280x**) |
| HTTP bodies were materialised as `[i64]` (8 B/byte) | 1 MiB body 94 → 388 MB/s, and the `Bytes` type took the raw read from 83 → 1000 MB/s |
| `json.parse` grew strings byte by byte | 820 KB parses in 42 ms, faster than CPython's C parser (73 ms) |

## Fair-comparison notes

- `dict500k`: C uses a hand-rolled open-addressing int→int table (22.4 ms);
  Rust uses `std::collections::HashMap` with SipHash (85 ms). A's `dict` is a
  general-purpose ordered map and is compared against both.
- `bench.astra` gives each benchmark its own LCG state variable on purpose: a
  variable shared between two sub-benchmarks loses its proven range, and the
  suite would then measure something other than the loop it names.
- `str200k`: the C version builds the result with an offset copy (a naive
  `strcat` loop is quadratic — 3.6 s — and would flatter A unfairly).
- Python's `str200k` and Node's sort are competitive because those paths are
  implemented in C inside the runtime (CPython's `str.join`, V8's sort with a
  comparator callback).
- No benchmark here is dominated by startup, JIT warm-up or I/O; each is timed
  around the hot loop only, except where the table says otherwise.
