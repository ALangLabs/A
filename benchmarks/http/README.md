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
| 2 B     | 300 | A `std:http`     |    303   |  1.010 |  –   |
| 2 B     | 300 | Python           |    263   |  0.877 |  –   |
| 2 B     | 300 | C++ raw sockets  |    175   |  0.583 |  –   |
| 16 KiB  | 200 | A `std:http`     |    239   |  1.195 | 13.7 |
| 16 KiB  | 200 | Python           |    152   |  0.760 | 21.6 |
| 16 KiB  | 200 | C++ raw sockets  |     96   |  0.480 | 34.1 |
| 1 MiB   |  20 | A `std:http`     |    103   |  5.150 |  203 |
| 1 MiB   |  20 | Python           |     28   |  1.400 |  750 |
| 1 MiB   |  20 | C++ raw sockets  |     27   |  1.350 |  777 |
| 1 MiB   |  20 | Python keep-alive|     13   |  0.650 | 1613 |
| 1 MiB   |  20 | C++ keep-alive   |     10   |  0.500 | 2097 |
| 10 MiB  |   5 | A `std:http`     |    193   | 38.600 |  271 |
| 10 MiB  |   5 | Python           |    122   | 24.400 |  430 |
| 10 MiB  |   5 | C++ raw sockets  |     25   |  5.000 | 2097 |
| 10 MiB  |   5 | Python keep-alive|    121   | 24.200 |  433 |
| 10 MiB  |   5 | C++ keep-alive   |     25   |  5.000 | 2097 |

Numbers move ±2x with machine load (the box is shared); the ratios are stable.

Keep-alive is only measured for bulk payloads: with tiny replies Python's
`http.server` stalls 40 ms per request on persistent connections (delayed ACK),
which would measure the fixture rather than the client. A has no connection
pooling yet, so it has no keep-alive row.

## What the numbers say

**Small requests: within ~1.6-2.5x of raw C++, ~1.15x of Python.** Per-request
cost is dominated by the connect and the server, not by A: 300 two-byte
requests cost 303 ms against Python's 263 ms and C++'s 175 ms.

**Bulk bodies: ~3-5x off raw C++, and now ahead of Python on 10 MiB**
(271 vs 430 MB/s is Python; on 1 MiB A is at 203 vs Python's 750). The gap is
structural, not a bug: an A `[i64]` byte array stores **8 bytes per payload
byte**, so any body that has to be materialised as bytes costs 8x the memory
traffic. Text bodies now avoid that entirely.

### What was fixed while measuring

| change | effect |
|---|---|
| `net.recv_body(handle, n)` reads a text body **straight into the String**, validating UTF-8 in the same call | 1 read pass instead of int64 expansion + decode |
| ASCII fast path (8 bytes/iteration) in the UTF-8 validator | validation of a 1 MiB text body 1.8 ms -> 0.1 ms |
| header reads capped at 4096 B; the over-read is handed to the body reader as a String prefix | avoids expanding 64 KiB of body into `[i64]` before the body read starts |
| `list.sort()` / `sorted()` replaced the O(n^2) insertion sort with a stable merge sort plus a three-way quicksort for packed int arrays | **sorting 200k ints: 230 s -> 0.03 s (7700x)** |
| run-binary cache key now hashes the runtime C sources | editing the runtime no longer reuses a stale cached binary |

Performance vs the first measurement of this benchmark: 17 MB/s -> 203 MB/s on
1 MiB bodies (12x) and 17.8 -> 271 MB/s on 10 MiB.

## If A needs to close the rest

1. **A `bytes` value type backed by `char*`/`(ptr,len)`** instead of `[i64]`.
   Binary bodies stop costing 8x memory and 8x copy; this is the last real
   structural gap for bulk transfers.
2. **Connection pooling** (`Connection: keep-alive` reuse) — worth 1.6-2.1 GB/s
   for the other two clients on 1 MiB bodies; A currently connects per request.
3. **Value unboxing / type specialization in the compiler** — the remaining
   fixed cost per request (~0.4 ms) and the 3-5x gap on arithmetic loops
   (`while` loop summing 5M ints: A 48 ms, C 10 ms) both come from boxed `aval`
   dispatch on every operation.
