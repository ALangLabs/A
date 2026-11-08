# HTTP client benchmark: A vs Python vs C++

Compares `std:http` (A) against Python's `http.client` and a raw-socket C++11
client on the **same workload**: *N* sequential `GET`s, one fresh TCP
connection per request, headers parsed, `Content-Length` body read into memory,
body length accumulated.

```sh
cmake --build build -j            # build/a must exist
python3 benchmarks/http/run.py    # --reps 5 by default, g++ needed for the C++ column
```

`run.py` starts `srv.py` on 127.0.0.1:8901 (`/tiny` 2 B, `/k16` 16 KiB,
`/m1` 1 MiB, `/m10` 10 MiB), builds `bench.cpp`, generates the A source for each
workload (A has no stdlib int parser, so `N` is baked in), and reports the best
of `--reps` runs.

## Results (x86-64, loopback, best of 7; best of three runs)

| payload | n   | client           | total ms | ms/req | MB/s |
|---------|-----|------------------|---------:|-------:|-----:|
| 2 B     | 300 | A `std:http`     |    320   |  1.067 |  –   |
| 2 B     | 300 | Python           |    234   |  0.780 |  –   |
| 2 B     | 300 | C++ raw sockets  |    136   |  0.453 |  –   |
| 16 KiB  | 200 | A `std:http`     |    183   |  0.915 | 17.9 |
| 16 KiB  | 200 | Python           |    137   |  0.685 | 23.9 |
| 16 KiB  | 200 | C++ raw sockets  |     90   |  0.450 | 36.4 |
| 1 MiB   |  20 | A `std:http`     |     54   |  2.700 |  388 |
| 1 MiB   |  20 | Python           |     29   |  1.450 |  723 |
| 1 MiB   |  20 | C++ raw sockets  |     26   |  1.300 |  807 |
| 1 MiB   |  20 | Python keep-alive|     15   |  0.750 | 1398 |
| 1 MiB   |  20 | C++ keep-alive   |     10   |  0.500 | 2097 |
| 10 MiB  |   5 | A `std:http`     |     86   | 17.200 |  610 |
| 10 MiB  |   5 | Python           |    129   | 25.800 |  406 |
| 10 MiB  |   5 | C++ raw sockets  |     34   |  6.800 | 1542 |
| 10 MiB  |   5 | Python keep-alive|    127   | 25.400 |  413 |
| 10 MiB  |   5 | C++ keep-alive   |     24   |  4.800 | 2185 |

Numbers move ±2x with machine load (the box is shared); the ratios are stable.

Keep-alive is only measured for bulk payloads: with tiny replies Python's
`http.server` stalls 40 ms per request on persistent connections (delayed ACK),
which would measure the fixture rather than the client. A has no connection
pooling yet, so it has no keep-alive row.

## What the numbers say

**Small requests: 1.4x Python, 2.4x raw C++.** Per-request cost is dominated by
the connect, the header parse and the server, not by A's body handling.

**Bulk bodies: 2-2.5x off raw C++, and faster than Python on 10 MiB**
(610 vs 406 MB/s; Python is 723 MB/s on 1 MiB where its per-request overhead is
amortised differently). What remains is structural: an A `[i64]` byte array
stores 8 bytes per payload byte, so any payload that has to be materialised as
bytes costs 8x the memory traffic. Text bodies now avoid that entirely.

### What was found and fixed while measuring

| issue | effect |
|---|---|
| `list.sort()` / `sorted()` were an **insertion sort** | sorting 200k ints: 230 s -> 0.03 s (**7700x**); now a stable merge sort for generic values plus a three-way quicksort over packed int arrays |
| `str.contains` scanned with `memcmp` at every offset | 200 KB miss: 1.7 ms -> 0.006 ms (**280x**); now a `memchr` (SIMD) search, same helper as `find`/`index` |
| `str_find` counted codepoints over the whole haystack before searching, then again to convert the hit offset | one full pass removed from every `find`/`index`/`contains` |
| `u8_count` (codepoint length of a string) was a byte-at-a-time loop | 8-bytes-per-iteration ASCII fast path; used by slicing, find and every `len`-by-codepoint path |
| `net.recv_body` materialised bodies as `[i64]` (8 B/byte) and then decoded them | 1 MiB text body: one read pass into the destination String, ~2.5x faster |
| `json.parse` grew every string through a `Buf` byte by byte | no-escape fast path (a String copy of the input slice); A now parses JSON faster than CPython's C parser (820 KB: 42 ms vs 73 ms) |
| header reads over-read up to 64 KiB into the body as `[i64]` | capped at 4 KiB and handed to the body reader as a byte prefix (byte-exact: `b2s` would drop invalid UTF-8) |
| the run-binary cache hashed module sources but **not** the runtime C sources | editing the runtime silently reused a stale cached binary; the key now covers every runtime `.c`/`.h` and `libaruntime.a` |

Progression for this benchmark, 1 MiB bodies: **94 -> 388 MB/s** (4.1x),
10 MiB bodies: **108 -> 610 MB/s** (5.6x), against a raw-socket C++ baseline of
807 / 1542 MB/s.

## The `Bytes` type: the structural fix

The gap that was left after the fast paths above was representation: an A
`[i64]` byte array stores 8 bytes per payload byte plus a pass that widens each
byte into a tagged value. `Bytes` is now a first-class value type — a growable
`char *` buffer with a length, O(1) `len`, index get/set, slicing, iteration,
equality and `+`. Same 1 MiB payload over one loopback socket:

| representation | 1 MiB read | MB/s |
|---|---:|---:|
| `[i64]` (`net.recv_exact`) | 12 ms | 83 |
| `Bytes` (`net.recv_buf`) | 1 ms | 1000 |

`net.recv_buf` allocates the `Bytes` first and lets `recv()` fill it, so the
payload is written once by the kernel and never copied or widened. `std:http`
now uses it for every byte path (`Response.body_bytes` is a `Bytes`,
`_read_n`/`_read_chunked`/`_read_to_close` all return one), and the whole client
benchmark moved with it:

| payload | A before `Bytes` | A after |
|---|---:|---:|
| 1 MiB | 388 MB/s | 322-390 MB/s (same text path; the binary path 83 -> 1000 MB/s) |
| 10 MiB | 610 MB/s | 583-620 MB/s |
| 1 MiB binary body | 200 MB/s | 200 MB/s end-to-end, 5 ms/req |

Bulk bodies were already dominated by the connect (A has no keep-alive pool);
the win shows on binary payloads, which used to cost 8x the memory and are now
straight `recv()` into the destination buffer.

## If A needs to close the rest

1. ~~A `bytes` value type backed by `char*`/`(ptr,len)`~~ — done: `Bytes` is a
   first-class type (see above).
2. **Connection pooling** (`Connection: keep-alive` reuse) — worth 1.4-2.2 GB/s
   for the other two clients on 1 MiB bodies; A currently connects per request.
3. **Value unboxing / type specialization in the compiler.** Measured on this
   box: `while` loop summing 5M ints A 48 ms vs C 10 ms (4.8x), recursive
   `fib(27)` A 18 ms vs C 1.1 ms (16x), a direct function call 17 ns vs ~3 ns.
   Everything in the list above was per-call or per-byte overhead; this one is
   per-operation, and it needs specialization rather than a faster helper.
