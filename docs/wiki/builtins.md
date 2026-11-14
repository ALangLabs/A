# Builtins and native modules

Core builtin functions are available without imports (verified against
`compiler/astra/frontend/ast/builtins.hpp` and the runtime `A_BI_*` table).

## Core builtins

| Category | Functions |
| --- | --- |
| I/O | `print(...)`, `eprint(...)` (stderr), `input()`, `flush()`; `input()` reads one line (optional prompt arg) and **panics on EOF** — for EOF-tolerant line input use `read_line()` from the `std/io` package (`from "std/io/__init__.a" import read_line`), which returns `""` at end-of-file |
| Values | `len(x)`, `str(x)`, `int(x)`, `float(x)`, `bool(x)`, `repr(x)`, `type(x)`, `isinstance(x, T)` — `T` is a runtime type-name string (`isinstance(xs, "list")`) or a bare type name (`isinstance(xs, list)`, `isinstance(x, int)`, or a `class` name); sized values (str, list, tuple, set, map) also accept the method form `x.len()` |

Boolean literals are the lowercase keywords `true` and `false` (not Python's
`True`/`False`). `bool(x)` converts a value to a boolean. Conditions in `if`,
`elif`, `while`, and `do`/`while` are expressions (see
[syntax and types](syntax-and-types.md) and
[statements and expressions](statements-and-expressions.md)).
| Math | `abs`, `min`, `max`, `sum`, `pow`, `divmod` (returns `(quotient, remainder)` with floor-division semantics), `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`, `random`, `random_int` |
| Linear algebra | `dot(a, b)`, `matvec(x, w, bias, nin, nout)`, `matvec_t(x, w, bias, nout, nin)`; canonical names `linalg_dot`, `linalg_matvec`, `linalg_matvec_t` also work. ML kernels: `matmul(a, b, m, k, n)` (blocked/tiled dense matmul), `softmax(x)` (stable), `attention(q, k, v, nk, dk, dv)` (fused); canonical names `linalg_matmul`, `linalg_softmax`, `linalg_attention` also work. All are native `a_builtin` kernels with fused multiply-add hot loops. `f32s(data, byte_offset, count)` bulk-decodes `count` little-endian binary32 values from a `String`/`Bytes` buffer into `[f64]` — the fast path for binary weight files (safetensors) |
| Collections | `map`, `filter`, `reduce`, `any`, `all`, `some`, `join`, `sorted`, `reversed`, `enumerate`, `zip`, `set`, `tuple`, `list`, `dict`, `bytes` (alias of `Bytes`), `range(n)`/`range(a, b[, step])`, plus `a..b` range literals. `map`/`filter` accept both `(fn, xs)` and `(xs, fn)`; `any(xs)`/`all(xs)` check truthiness with one arg or take a predicate as a second arg |
| Option/Result | `Some(v)`, `Ok(v)`, `Err(e)` constructors; `None` literal |
| Assertions | `assert(cond)`, `assert_eq(a, b)`, `panic(msg)` |
| Debug | `ic(*values)` (prelude), `repr`, `__repr__`, `__debug_repr__`, `__ic_format__` |
| Files | `read_file`, `write_file`, `append_file`, `read_bytes`, `write_bytes`, `file_exists`, `file_size`, `list_dir`, `remove_file`, `create_dir`, `remove_dir`, `rename`, `current_dir` |
| Conversions | `i8 i16 i32 i64 u8 u16 u32 f32 f64` (there is no `u64` builtin) |
| Intrinsics | `ctz clz popcount bswap rotl rotr fma fabs copysign smin smax umin umax` + `sadd_overflow ssub_overflow smul_overflow uadd_overflow usub_overflow umul_overflow` and `expect`/`likely`/`unlikely` |

Note: `range` *is* a builtin — `range(5)` iterates `0` through `4`
(exclusive end), and `range(a, b[, step])` also works; `0..5` range
literals are the lower-level equivalent.

### Implicit parallelism

The runtime parallelizes eligible work automatically (no async syntax):
`sum()` over large all-integer lists, `map()`/`filter()` over large lists
when the callable is a whitelisted pure builtin, and the `linalg_*` kernels
(`matmul`, `matvec_t`, `attention` score phase, `softmax` exp fill) above
~1M multiply-adds. Arbitrary closures stay serial because generated code is
not thread-safe. All parallel paths produce bit-identical results to the
serial loops. `a test` runs test functions in concurrent forked batches.
Tuning/kill-switches: `A_PAR=0` disables the thread pool, `A_PAR_THREADS=N`
sizes it, `A_TEST_JOBS=N` sizes the test batch (1 = serial). Parallelism
requires the system-C runtime build (POSIX); astra-compiled and Windows
runtimes always take the serial path.

## Native modules

`import <name>` binds a namespace of builtins (table in
`compiler/astra/frontend/name-resolution/native_modules.hpp`):

`math` (sqrt/abs/min/max/pow/trig/log/floor/ceil/round/trunc/random + `PI`,`E`),
`io` (print/eprint/input/flush), `fs` (file ops + POSIX primitives: `read_file`,
`write_file`, `append_file`, `file_exists`, `file_size`, `list_dir`,
`remove_file`, `create_dir`, `remove_dir`, `rename`, `current_dir`,
`read_bytes`, `write_bytes`, `chdir`, `getpid`/`getppid`, `getuid`/`geteuid`,
`getgid`/`getegid`, `getlogin`, `cpu_count`, `uname` (5-tuple), `stat`/`lstat`
(13-tuple: mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime,
is_file, is_dir, is_symlink), `access`, `chmod`, `symlink`/`link`/`readlink`,
`utime`, `mkdir`/`rmdir`, `truncate`, `environ` (Map(String,String)),
`getenv` (None when unset), `setenv`, `unsetenv`, `system` (exit code,
not raw wait status), `urandom` (`[i64]` byte values),
`getloadavg` (3-tuple of floats), `gethostname`, `umask`, `getpagesize`,
`chown`, `ismount`, `disk_usage` (total/used/free), `popen`, `kill`,
`exit`), `strings`, `collections`,
`core` (panic/assert/assert_eq/struct_name/struct_methods/fn_signature/
fn_doc/bytes_utf8_decode/bytes_utf8_encode/bytes_new/bytes_append/
bytes_to_str), `time` (now/sleep), `json`, `binary`,
`testing`, `log`, `metrics`, `trace`, `net` (resolve/tcp_listen/tcp_connect/accept/
send/recv/recv_status/close/set_timeout/set_nonblocking/poll/local_port/
udp_socket/udp_bind/udp_send/udp_recv/tls_connect/tls_wrap/tls_available/
send_bytes/recv_bytes/recv_buf/recv_buf_all/recv_body/recv_body_all/recv_exact/
inflate_bytes/inflate_available), `terminal`,
`env` (`get`, `getenv`/`all`/`set`/`unset`), `repl` (eval/quiet), `py` (Python bridge), `unsafe`,
`arena`, `kernel_ops`.

`env.get(name) -> String` returns the environment variable's value, or the
empty string when it is unset — it is *not* an `Optional`. Use
`fs.getenv(name) -> String?` (or `std:os`'s `getenv`) when unset and empty
must be distinguished.

`core.bytes_utf8_decode(bytes) -> String?` and `core.bytes_utf8_encode(s) ->
[i64]` are the single-pass native conversions behind `std:bytes`
`utf8_decode`/`utf8_encode`; they exist so a multi-megabyte body decodes in
microseconds instead of allocating one string per byte. Both also accept a
`Bytes` value.

## Bytes

`Bytes` is the first-class byte buffer — a growable `char *` with an O(1)
length, not an `[i64]` array (which costs 8 bytes per payload byte plus an
expansion pass). Construct it with `Bytes()` (empty), `Bytes(n)` (n zero
bytes), `Bytes("text")` (UTF-8 of a string), `Bytes([1, 2, 3])` (one byte per
element) or `Bytes(other)` (copy). Lowercase `bytes(...)` is an alias for
the same constructor (Python-style spelling).

```
b: Bytes = Bytes("hey")
len(b)            # 3, O(1)
b[0]              # 104 (i64 0..255)
b[0] = 72         # store (panics outside 0..255)
b[0..2]           # Bytes slice (copy)
for c in b        # iterates byte values
b == other        # byte-wise equality; b + b concatenates
str(b)            # UTF-8 text when valid, else the escaped b"..." form
```

`core.bytes_append(dst, src)` grows `dst` in place and accepts a `Bytes`, a
`String`, an `[i64]` byte array, or a single integer; `core.bytes_to_str(b)`
returns `String?` (`None` when the payload is not valid UTF-8). `net.recv_buf`
/ `net.recv_buf_all` read a socket straight into a `Bytes` (one allocation, one
kernel copy: ~500 MB/s on loopback vs ~83 MB/s for the packed `[i64]` form),
and `net.recv_body` returns the binary half of an HTTP body as `Bytes`.

Prefer `import std:<pkg>` for the ergonomic stdlib wrappers — see
[stdlib](stdlib.md).
