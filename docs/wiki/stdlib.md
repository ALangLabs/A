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

## Packages (`stdlib/std/`)

| Package | Contents |
| --- | --- |
| `collections` | `map_list`, `filter_list`, `reduce_list`, `sum_list`, `list_reverse`, `dict_get`, plus `list`, `dict`, `deque`, `counter`, `defaultdict`, `chainmap`, `ordereddict`, `namedtuple`, `userlist`, `userdict` |
| `math` | `sqrt`, `abs_val`, `min_val`, `max_val`, `pow_val`, `clamp`, constants, functions |
| `simd` | `vec_new`, `dot_lists`, `add_lists`, `scale_list`, `hadamard` (scalar-emulated fixed-width lanes; for real float kernels prefer the native `dot`/`matmul`/`softmax`/`attention` builtins, see [builtins](builtins.md)) |
| `http` | Synchronous HTTP/1.1 client, pure A over `std:net` (there are no `http_*` native builtins): `get`, `post`, `put`, `patch`, `delete`, `head`, `request`, `http_connection`, `https_connection`, `urlsplit`, `urlunsplit`, `quote`, `urlencode`, `Response` (`.status`, `.headers`, `.body`, `.body_bytes` (a `Bytes`), `.text()`, `.json()`, `.raise_for_status()`, `.is_ok`). Follows redirects, skips 1xx interim responses, handles Content-Length / chunked / close-delimited bodies, decodes `Content-Encoding: gzip|deflate` when zlib is present, sends binary payloads via `body_bytes`, verifies TLS (`verify=false` accepts self-signed; `A_TLS_CA_FILE` adds a private CA), honours `http_proxy`/`https_proxy`/`no_proxy` (HTTPS tunnels through CONNECT), keeps connections alive by default (an idle-socket pool keyed by scheme/host/port/proxy/verify, reused only when the peer has said nothing since the last response and the body was exactly delimited; `_pool_close_all()` empties it), sets `TCP_NODELAY` so a reused connection cannot stall on delayed ACKs, and treats a socket read timeout mid-body as a hard error rather than silently truncating (plain `recv()` cannot distinguish EOF from timeout). Import members directly (`from std:http import get`) — module-attribute access on the package also works. Optional arguments are positional (A has no keyword arguments); HEAD/204/304 responses carry no body. |
| `bytes`, `binary` | Byte utilities: `utf8_encode`/`utf8_decode`, `s2b`/`b2s`, `chr`/`ord`, varint codec — pair with `fs.read_bytes`/`write_bytes` for binary IO. The first-class `Bytes` type (`Bytes()`, `Bytes(n)`, `Bytes("s")`, `Bytes([i64])`) is a growable `char *` buffer with O(1) `len`, index get/set, slicing, iteration and `+`; use it instead of `[i64]` for payloads (8x less memory, ~6x faster socket reads) |
| `re` | Pure-A regex engine: `compile` → `Pattern` (`.search`, `.match_at`, `.fullmatch`, `.findall`, `.finditer`, `.count`, `.sub`, `.split`), module `search`/`match_at`/`fullmatch`/`findall`/`finditer`/`count`/`sub`/`split`/`escape`, `Match` (`.group`/`.groups`/`.start`/`.end`/`.span`). ASCII-only; no flags, lookaround, backreferences, or lazy quantifiers. |
| `random` | Seedable Park-Miller PRNG: `Random` (`seed`, `random`, `randint`, `randrange`, `choice`, `choices`, `sample`, `shuffle`, `shuffled`, `uniform`, `triangular`, `getrandbits`, `randbytes`) plus module-level API sharing a global stream; works under `a test`. |
| `path` | posixpath subset: `join`, `split`, `dirname`, `basename`, `splitext`, `splitdrive`, `normpath`, `abspath`, `relpath`, `commonpath`, `commonprefix`, `isabs`, `expanduser`, `exists`, `getsize` + constants; POSIX-only. |
| `os` | `getenv`/`getenv_default`/`environ_get`, `getcwd`, `listdir`/`scandir`, `makedirs`/`mkdir`, `rmdir`/`removedirs`, `remove`/`unlink`, `exists`/`lexists`/`isfile`/`isdir`/`getsize`, `ismount`, `urandom` + POSIX constants; no `environ`/`stat`/`rename`. |
| `datetime` | UTC `DateTime` (y/m/d/h/m/s/ms): `isoformat`, `strftime`, `timestamp`, `weekday`/`isoweekday`, `date_parts`/`time_parts`, `strptime`, `from_timestamp`/`utcfromtimestamp`/`now`, `add_seconds`/`add_ms`/`diff_ms`/`combine`, `is_leap_year`, `days_in_month`. |
| `iter`, `functools`, `operator`, `pprint`, `textwrap`, `shlex`, `keyword`, `statistics`, `fractions`, `heapq`, `copy`, `comptime`, `format` (csv/table), `getopt`, `json`, `fs`, `time`, `log`, `metrics`, `trace`, `net`, `string`, `bytes`, `binary`, `collections`, `core`, `testing`, `repl`, `enum`, `debug`, `prelude`; `iter` adds `chain`, `product`, `permutations`, `combinations`, `combinations_with_replacement`, `groupby`/`groupby_key`, `accumulate`, `takewhile`, `dropwhile`, `pairwise`, `batched`, `islice` |

Many packages are thin A wrappers over the native modules in
[builtins](builtins.md); others are pure A.

## Self-hosted REPL

`stdlib/std/repl/main.a` implements the IPython-style shell that `a repl`
launches on a TTY (line editing, history, highlighting, completion via
`repl` module builtins).
