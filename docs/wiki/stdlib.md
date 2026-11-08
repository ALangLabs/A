# Standard library

`stdlib/` is a tree of `.a` modules — ordinary A code using the same import
system as user code (verified against `stdlib/std/` layout).

## Entry points

- `import std:prelude` — helpers incl. `Ok`, `Err`, `is_ok`, `unwrap`,
  `unwrap_or`, `err_value`, `prelude_version()`.
- `import std:<pkg>` first checks the native modules (`math`, `io`, `fs`,
  `net`, `env`, … — see [builtins](builtins.md)); names with no native
  module resolve to a package under `stdlib/std/` (so `std:net` and
  `std:env` are the native modules, not A files).
- `stdlib/std/std.a` re-exports the common subset (`Int`, `Float`, `Bool`,
  `String`, `len_of`, `map_list`, `sqrt`, `Equatable`, arena helpers, …).

## Packages (`stdlib/std/`)

| Package | Contents |
| --- | --- |
| `builtin` | `Int`, `Float`, `Bool`, `String`, `len_of`, `range_list` |
| `collections` | `map_list`, `filter_list`, `reduce_list`, `sum_list`, `list_reverse`, `dict_get`, plus `list`, `dict`, `deque`, `counter`, `defaultdict`, `chainmap`, `ordereddict`, `namedtuple`, `userlist`, `userdict` |
| `math` | `sqrt`, `abs_val`, `min_val`, `max_val`, `pow_val`, `clamp`, constants, functions |
| `simd` | `vec_new`, `dot_lists`, `add_lists`, `scale_list`, `hadamard` |
| `traits` | `Equatable`, `Hashable`, `Comparable`, `eq`, `is_copyable` |
| `memory` | `arena_new`, `arena_pool`, `alloc`, `used`, `total` |
| `http` | Synchronous HTTP/1.1 client, pure A over `std:net` (there are no `http_*` native builtins): `get`, `post`, `put`, `patch`, `delete`, `head`, `request`, `http_connection`, `https_connection`, `urlsplit`, `urlunsplit`, `quote`, `urlencode`, `Response` (`.status`, `.headers`, `.body`, `.body_bytes`, `.text()`, `.json()`, `.raise_for_status()`, `.is_ok`). Follows redirects, skips 1xx interim responses, handles Content-Length / chunked / close-delimited bodies, decodes `Content-Encoding: gzip|deflate` when zlib is present, sends binary payloads via `body_bytes`, verifies TLS (`verify=false` accepts self-signed; `A_TLS_CA_FILE` adds a private CA), honours `http_proxy`/`https_proxy`/`no_proxy` (HTTPS tunnels through CONNECT), and treats a socket read timeout mid-body as a hard error rather than silently truncating (plain `recv()` cannot distinguish EOF from timeout). Import members directly (`from std:http import get`) — module-attribute access on the package also works. Optional arguments are positional (A has no keyword arguments); HEAD/204/304 responses carry no body. |
| `bytes`, `binary` | Byte-array (`[i64]` 0-255) utilities: `utf8_encode`/`utf8_decode`, `s2b`/`b2s`, `chr`/`ord`, varint codec — pair with `fs.read_bytes`/`write_bytes` for binary IO |
| `iter`, `functools`, `operator`, `pprint`, `textwrap`, `shlex`, `keyword`, `statistics`, `fractions`, `heapq`, `copy`, `comptime`, `format` (csv/table), `getopt`, `json`, `fs`, `time`, `log`, `metrics`, `trace`, `net`, `string`, `bytes`, `binary`, `collections`, `core`, `test`/`testing`, `gpu`, `repl`, `enum`, `debug`, `prelude` |

Many packages are thin A wrappers over the native modules in
[builtins](builtins.md); others are pure A.

## Self-hosted REPL

`stdlib/std/repl/main.a` implements the IPython-style shell that `a repl`
launches on a TTY (line editing, history, highlighting, completion via
`repl` module builtins).
