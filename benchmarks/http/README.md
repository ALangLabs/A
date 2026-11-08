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

## Results (x86-64, loopback, best of 5)

| payload | n   | client           | total ms | ms/req | MB/s |
|---------|-----|------------------|---------:|-------:|-----:|
| 2 B     | 300 | A `std:http`     |    238   |  0.793 |  –   |
| 2 B     | 300 | Python           |    214   |  0.713 |  –   |
| 2 B     | 300 | C++ raw sockets  |    134   |  0.447 |  –   |
| 16 KiB  | 200 | A `std:http`     |    198   |  0.990 | 16.6 |
| 16 KiB  | 200 | Python           |    148   |  0.740 | 22.1 |
| 16 KiB  | 200 | C++ raw sockets  |     92   |  0.460 | 35.6 |
| 1 MiB   |  20 | A `std:http`     |    222   | 11.100 | 94.5 |
| 1 MiB   |  20 | Python           |     28   |  1.400 |  749 |
| 1 MiB   |  20 | C++ raw sockets  |     24   |  1.200 |  874 |
| 1 MiB   |  20 | Python keep-alive|     12   |  0.600 | 1748 |
| 1 MiB   |  20 | C++ keep-alive   |      9   |  0.450 | 2330 |
| 10 MiB  |   5 | A `std:http`     |    485   | 97.000 |  108 |
| 10 MiB  |   5 | Python           |    127   | 25.400 |  413 |
| 10 MiB  |   5 | C++ raw sockets  |     38   |  7.600 | 1380 |
| 10 MiB  |   5 | Python keep-alive|    122   | 24.400 |  430 |
| 10 MiB  |   5 | C++ keep-alive   |     25   |  5.000 | 2097 |

Keep-alive is only measured for bulk payloads: with tiny replies Python's
`http.server` stalls 40 ms per request on persistent connections (delayed ACK),
which would measure the fixture rather than the client. A has no connection
pooling yet, so it has no keep-alive row.

## What the numbers say

**Small requests: A is within ~10–20 % of Python, ~1.8× behind raw C++.**
Per-request cost is dominated by the TCP connect plus header parsing, and A's
client does that in one socket per request like the others. Nothing here is
algorithmically wrong; the gap is interpreter/dispatch overhead.

**Large bodies: A runs at ~95–108 MB/s against 400–1400 MB/s.**
The reason is structural: an A `[i64]` byte array stores **8 bytes per byte of
payload**. A 10 MiB body costs an 80 MiB allocation and two passes over the
data (recv into a scratch buffer, expand to int64; then UTF-8 decode). Python's
`bytes` and C++'s `std::string` are flat `char` buffers, so they are memory
bandwidth bound.

This is already ~6× faster than before this benchmark existed
(17 MB/s → 100 MB/s), from two changes:

* `net.recv_exact(handle, byteCount)` — reads the whole body in one native call
  straight into packed `[i64]` storage, replacing an A-level
  `recv` + `list.extend` + `slice` loop that cost ~35 ns per byte.
* `core.bytes_utf8_decode/encode` — single-pass native UTF-8 conversion
  instead of per-character `chr()`/string concatenation (a 1 MiB text body
  decoded in 1180 ms before, 16 ms now).

## If A needs to close the bulk gap

1. **A `bytes` value type backed by `char*`/`(ptr,len)`** instead of `[i64]`.
   Bodies become flat buffers, `body_bytes` stops costing 8× memory, and the
   transport can run at libc `recv` speed. This is the real fix.
2. **Lazy `body_bytes`** — decode `.body` directly from the socket
   (`recv_exact_text`) and only materialize `body_bytes` when asked, so a text
   response never pays the int64 expansion.
3. **Connection pooling** (`Connection: keep-alive` reuse) — worth 1.4–2.3 GB/s
   for the other two clients on 1 MiB bodies; A currently pays a connect per
   request.
