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
| `http` | Synchronous HTTP/1.1 client (requests-style), pure A over `std:net` (no `http_*` native builtins): module `get`, `post`, `put`, `patch`, `delete`, `head`, `options`, `request`, `session()`/`Session`, `codes`/`status_codes`, `http_connection`/`https_connection`, `urlsplit`/`urlunsplit`/`quote`/`urlencode`. `Response`: `.status`/`.status_code()`, `.reason`, `.headers`, `.url`, `.encoding`, `.is_ok`/`.ok()`, `.body`/`.text()`, `.body_bytes`/`.content()`, `.json()`, `.raise_for_status()` (returns self, panics on 4xx/5xx), `.is_redirect()`/`.is_permanent_redirect()`/`.is_client_error()`/`.is_server_error()`, `.cookies()`, `.iter_lines()`/`.iter_content(n)`, `.links()`. Follows redirects, skips 1xx interim responses, handles Content-Length/chunked/close-delimited bodies, decodes `gzip`/`deflate` when zlib is present, verifies TLS (`verify=false` accepts self-signed; `A_TLS_CA_FILE` adds a private CA), honours `http_proxy`/`https_proxy`/`no_proxy` (HTTPS CONNECT), keeps connections alive (idle pool; `_pool_close_all()` empties it). **A differences vs `requests`:** no keyword args (positional optionals), methods/properties that are Python properties are methods in A (e.g. `.content()`, `.status_code()`, `.ok()`), errors panic instead of raising exception types, and streaming/history/advanced auth/hooks/cert files/multipart are limited. HEAD/204/304 carry no body. |
| `bytes` | Byte utilities: `utf8_encode`/`utf8_decode`, `s2b`/`b2s`, `chr`/`ord`, varint codec — pair with `fs.read_bytes`/`write_bytes` for binary IO. The first-class `Bytes` type (`Bytes()`, `Bytes(n)`, `Bytes("s")`, `Bytes([i64])`) is a growable `char *` buffer with O(1) `len`, index get/set, slicing, iteration and `+`; use it instead of `[i64]` for payloads (8x less memory, ~6x faster socket reads). (`binary` is a native module — `binary_encode`/`binary_decode` — not a package; see [builtins](builtins.md).) |
| `re` | Pure-A regex engine: `compile` → `Pattern` (`.search`, `.match_at`, `.fullmatch`, `.findall`, `.finditer`, `.count`, `.sub`, `.split`), module `search`/`match_at`/`fullmatch`/`findall`/`finditer`/`count`/`sub`/`split`/`escape`, `Match` (`.group`/`.groups`/`.start`/`.end`/`.span`). ASCII-only; no flags, lookaround, backreferences, or lazy quantifiers. |
| `random` | Seedable Park-Miller PRNG: `Random` (`seed`, `random`, `randint`, `randrange`, `choice`, `choices`, `sample`, `shuffle`, `shuffled`, `uniform`, `triangular`, `getrandbits`, `randbytes`) plus module-level API sharing a global stream; works under `a test`. |
| `path` | posixpath subset: `join`, `split`, `dirname`, `basename`, `splitext`, `splitdrive`, `normpath`, `abspath`, `relpath`, `commonpath`, `commonprefix`, `isabs`, `expanduser`, `exists`, `getsize` + constants; POSIX-only. |
| `os` | Python-compatible `os` package split into focused modules (`constants`, `environment`, `process`, `stat`, `dirs`, `files`, `pathns`, `randbytes`, `termsize`, `syscalls`). Constants (`name`, `sep`, `pathsep`, `linesep`, `curdir`, `pardir`, `extsep`, `devnull`, `F_OK`/`R_OK`/`W_OK`/`X_OK`, `SEEK_*`, `SIG*` signal numbers); environment (`getenv` (None for unset), `getenv_default`, `environ_get`, `environ_all`, `putenv`/`unsetenv`, `environ` mapping facade); process (`getcwd`, `chdir`, `getpid`/`getppid`, `getuid`/`geteuid`/`getgid`/`getegid`, `getlogin`, `cpu_count`, `uname` → `Uname`, `system` (exit code), `getloadavg`, `gethostname`, `umask`, `getpagesize`, `kill`, `_exit`); stat (`StatResult` with `st_mode`/`st_ino`/`st_dev`/`st_nlink`/`st_uid`/`st_gid`/`st_size`/`st_atime`/`st_mtime`/`st_ctime`/`is_file`/`is_dir`/`is_symlink`, `stat`/`lstat`/`try_stat`/`try_lstat`, `exists`/`lexists`/`isfile`/`isdir`/`islink`/`getsize`/`access`/`chmod`/`chown`/`utime`/`samefile`/`samestat`/`ismount`/`disk_usage`); dirs (`listdir`, `scandir` → `DirEntry`, `mkdir`/`makedirs`/`rmdir`/`removedirs`, `walk`/`fwalk`); files (`remove`/`unlink`/`rename`/`replace`/`renames`/`truncate`/`link`/`symlink`/`readlink`); `path` namespace object (`PathNamespace`: `join`/`join2`/`split`/`dirname`/`basename`/`splitext`/`normpath`/`abspath`/`relpath`/`commonpath`/`commonprefix`/`isabs`/`exists`/`getsize`); `urandom`/`getrandom` (→ `[i64]` byte values); `popen` (shell stdout capture); `get_terminal_size`. POSIX paths are UTF-8 strings; there is no file-descriptor API, and `fwalk` raises. |
| `datetime` | UTC `DateTime` (y/m/d/h/m/s/ms): `isoformat`, `strftime`, `timestamp`, `weekday`/`isoweekday`, `date_parts`/`time_parts`, `strptime`, `from_timestamp`/`utcfromtimestamp`/`now`, `add_seconds`/`add_ms`/`diff_ms`/`combine`, `is_leap_year`, `days_in_month`. |
| `iter`, `functools`, `operator`, `pprint`, `textwrap`, `shlex`, `keyword`, `statistics`, `fractions`, `heapq`, `copy`, `comptime`, `format` (csv/table), `getopt`, `json`, `string`, `bytes`, `collections`, `repl`, `enum`, `debug`, `prelude`; `iter` adds `chain`, `product`, `permutations`, `combinations`, `combinations_with_replacement`, `groupby`/`groupby_key`, `accumulate`, `takewhile`, `dropwhile`, `pairwise`, `batched`, `islice`; `io` adds `StringIO`/`BytesIO`/`read_line` (via `from "std/io/__init__.a" import …` — bare `io` is the native module); `queue` adds `Queue`/`LifoQueue`/`PriorityQueue`; `bisect` adds `bisect_left`/`bisect_right`/`insort*`/`bisort`; `swiftcli` provides argparse-style CLI parsing (`ArgumentParser`/`add_argument`/`parse_args`/`parse_known_args`/subparsers, actions like `store_true`/`store_false`/`store_const`/`append`/`append_const`/`count`, `nargs`, `choices`, `required`, `dest`, `set_defaults`, `allow_abbrev`) plus the original declarative helpers (`new`/`Parser`, `Option`/`Positional`/`Sub`/`ParseResult`, `args_from_env`/`split_args`) |

Note: several names exist both as native modules and as A packages under
`stdlib/std/` — `math`, `io`, `collections`, `json`, `repl`. For those,
`import std:<name>` binds the **native** module (see
[builtins](builtins.md)); reach the A package with a quoted path like
`import "std/collections/__init__.a"`. Names with no native module
(`bytes`, `re`, `http`, `os`, `path`, `random`, `datetime`, …) resolve
to the package directly.

Many packages are thin A wrappers over the native modules in
[builtins](builtins.md); others are pure A.

## Self-hosted REPL

`stdlib/std/repl/main.a` implements the IPython-style shell that `a repl`
launches on a TTY (line editing, history, highlighting, completion via
`repl` module builtins).
