# A Standard Library & API Reference

This reference documents the current A standard library surface as implemented by:

- `compiler/astra/frontend/name-resolution/native_modules.hpp` (native module namespaces)
- `compiler/astra/frontend/name-resolution/builtin_docs.hpp` (builtin docs/signatures used by tooling)
- `compiler/astra/frontend/type-checker/sema.cpp` (builtin recognition and type behavior)
- `compiler/astra/frontend/type-checker/method_signatures.hpp` (array/map/set/string method surfaces)
- `stdlib/prelude.a` and `stdlib/std/*`
- integration tests in `tests/integration/cases/`

> **Runtime note:** A currently executes with dynamic/Python-style semantics in many paths. Some stdlib/native-backed features (especially `json`, `binary`, `testing`, `log`, `metrics`, `trace`, `net`, `http`) are implemented in the runtime/builtin layer and are primarily exercised through the interpreter/KGEN execution engine path. Availability/performance characteristics may differ on the LLVM AOT path.

## 1. Prelude (auto-loaded)

`stdlib/prelude.a` is auto-loaded by the module loader when a stdlib root is found. It provides namespaced vocabulary and helpers.

### 1.1 `prelude_version()`

```a
import std:prelude
print(prelude.prelude_version())  # "0.0.1"
```

### 1.2 `Vec4` (SIMD-like value type, width 4)

```a
class Vec4:   # provided by prelude; shown for shape
    x
    y
    z
    w
```

Methods:

| Method | Description |
|---|---|
| `get(i)` | Returns lane `x/y/z/w` for indices 0..3 (else `w`) |
| `to_list()` | Returns `[x, y, z, w]` |
| `add(other)` | Lane-wise addition, returns `Vec4` |
| `scale(s)` | Scales all lanes by `s`, returns `Vec4` |
| `dot(other)` | Dot product |
| `sum()` | Sum of lanes |

### 1.3 Result helpers

Provided by `prelude` (import it first); signatures:

```a
import std:prelude

# prelude.Ok(value)        # -> Result(ok=true, value, error=None)
# prelude.Err(message)     # -> Result(ok=false, value=None, error)
# prelude.unwrap(r)        # returns value or panics with r.error
# prelude.is_ok(r) -> bool
# prelude.is_err(r) -> bool
# prelude.unwrap_or(r, fallback)
# prelude.err_value(r)
```

Example:

```a
import std:prelude

fn divide(a, b):
    if b == 0:
        return prelude.Err("division by zero")
    return prelude.Ok(a / b)

print(prelude.is_ok(divide(10, 2)))
print(prelude.unwrap_or(divide(10, 0), -1))
```

## 2. Global builtins and constants

These names are recognized by the type checker/runtime as builtins or builtin constants (subject to shadowing rules). Some are also exposed via native modules.

Individual signatures, descriptions, and examples for every builtin live in the auto-generated reference (built from the same doc tables the type checker uses):

- **[Built-in functions](builtins.md)** — every builtin function and constant, with examples (generated from `builtin_docs.hpp`).
- **[Primitive type methods](methods.md)** — array/map/set/string methods (generated from `method_signatures.hpp`).

Highlights:

### 2.1 Core I/O and conversion
`print`, `eprint`, `open`, `close`, `stdout`, `stderr`, `input`, `flush`, `len`, `str`, `int`, `float` (full signatures in the reference).

### 2.2 Math / numeric builtins
`abs`, `min`, `max`, `pow`, `sqrt`, trig/log/rounding, `random`, `random_int`, `sum`.

### 2.3 Collection / functional builtins
`join`, `map`, `filter`, `reduce`, `any`, `all`, `set`.

### 2.4 Optionals / results / assertions / control helpers
`Some`, `Ok`, `Err`, `panic`, `assert`, `assert_eq`, `ic` (debug inspection: prints `ic| label: value` + context to stderr and returns the value).

### 2.5 Memory / ownership / borrow / unsafe builtins
`move`, `copy`, `borrow`, `borrow_mut`, `unsafe_get/set/address_of/load/store/offset` (full signatures in the reference).

### 2.6 Allocator / arena builtins
`arena_new/pool/region/alloc/release/reset/used/total`.

### 2.7 Builtin constants
`PI`, `E`, the sized integer/string extrema (`I8_MIN`…`U64_MAX`, `F32_MIN`…, etc.), `stdout`, `stderr`, `None`.

## 3. Collection and primitive method API

Method surfaces are defined in `compiler/astra/frontend/type-checker/method_signatures.hpp` and used by the type checker/LSP.

Full reference (signatures and descriptions for every method, generated from
those tables):

- **[Primitive type methods](methods.md)** — array/map/set/string methods.

Highlights: arrays are mutable with `append/pop/sort/reverse/copy` and the
rich string method set (`split/strip/replace/startswith/is*` predicates)
mirrors the Python surface; maps support `get/pop` + `m[k]` access;
sets support `add/remove/discard/union/intersection/difference`.

## 4. Native modules (toolchain-backed)

Import native modules with the `std:` prefix:

```a
import std:math
import std:fs
import std:json
from std:net import tcp_connect
```

Native modules and ported `std/*` packages are both reached through the
`std:` prefix (`import std:<name>` / `from std:<name> import ...`). Bare
`import <name>` is also accepted as shorthand for native modules and
auto-resolves to `stdlib/std/<name>/__init__.a` for ported packages.

Native module members map to runtime builtins (see `native_modules.hpp`).

### 4.1 `math`

Functions: `sqrt, abs, min, max, pow, sin, cos, tan, asin, acos, atan, atan2, exp, log, log2, log10, floor, ceil, round, trunc, random, random_int`  
Constants: `PI`, `E`

```a
import std:math
print(math.sqrt(81))
print(math.PI)
```

### 4.2 `io`

Functions: `print, eprint, input, flush`

```a
import std:io
io.print("hello")
```

### 4.3 `fs`

Functions: `read_file, write_file, append_file, file_exists, list_dir, remove_file, create_dir, remove_dir, rename, current_dir`

```a
import std:fs
fs.write_file("/tmp/a-demo.txt", "hi")
print(fs.read_file("/tmp/a-demo.txt"))
print(fs.file_exists("/tmp/a-demo.txt"))
```

### 4.4 `strings`

Functions: `len, str, join` (namespace wrappers over shared builtins)

### 4.5 `collections`

Functions: `len, map, filter, reduce, any, all, sum, join, set`

### 4.6 `unsafe`

Functions: `get, set, address_of, load, store, offset` (mapped to `unsafe_*` builtins)

### 4.7 `arena`

Functions: `new, pool, region, alloc, release, reset, used, total`

### 4.8 `core`

Functions: `panic, assert, assert_eq`

### 4.9 `time`

Functions: `now() -> i64` (Unix epoch ms), `sleep(milliseconds)`

### 4.10 `json` (native-backed)

Functions: `parse, stringify, pretty, is_valid, get, at, len, has, keys, kind, as_str, as_int, as_float, as_bool, as_array, as_map`

Tooling signatures:

| API | Signature |
|---|---|
| `json.parse` | `json.parse(text) -> Json` |
| `json.stringify` | `json.stringify(value) -> String` |
| `json.pretty` | `json.pretty(value) -> String` |
| `json.is_valid` | `json.is_valid(text) -> bool` |
| `json.get` | `json.get(doc, key) -> Json?` |
| `json.at` | `json.at(doc, index) -> Json?` |
| `json.len` | `json.len(doc) -> i64` |
| `json.has` | `json.has(doc, key) -> bool` |
| `json.keys` | `json.keys(doc) -> [String]` |
| `json.kind` | `json.kind(doc) -> String` (`null,bool,int,float,string,array,object`) |
| `json.as_*` | `json.as_str(doc) -> String?`, `as_int -> i64?`, `as_float -> f64?`, `as_bool -> bool?`, `as_array -> [Json]?`, `as_map -> {String: Json}?` |

```a
import std:json
doc = json.parse("\{\"a\": 1, \"b\": [true, null]\}")
print(json.kind(doc))
print(json.stringify(doc))
```

### 4.11 `binary` (native-backed)

| API | Signature | Description |
|---|---|---|
| `binary.encode` | `binary.encode(value) -> [i64]` | Self-describing bytes (0-255 per entry) |
| `binary.decode` | `binary.decode(bytes) -> Json?` | Decode bytes; malformed/truncated returns `None` |

### 4.12 `testing` (native-backed)

| API | Signature | Description |
|---|---|---|
| `testing.seed` | `testing.seed(value)` | Fix generator for reproducibility |
| `testing.for_all_int` | `testing.for_all_int(runs, low, high, fn(i64)->bool) -> bool` | Random int property |
| `testing.for_all_float` | `testing.for_all_float(runs, low, high, fn(f64)->bool) -> bool` | Random float property |
| `testing.for_all_bool` | `testing.for_all_bool(runs, fn(bool)->bool) -> bool` | Random bool property |
| `testing.for_all_string` | `testing.for_all_string(runs, maxLength, fn(String)->bool) -> bool` | Random printable ASCII string property |
| `testing.for_all_elem` | `testing.for_all_elem(values, fn(T)->bool) -> bool` | Check property for every element |

### 4.13 `log` (native-backed)

`log.set_level(level)` (`debug|info|warn|error`), `log.set_json(bool)`, `log.debug/info/warn/error(message[, fields])`.

### 4.14 `metrics` (native-backed)

`metrics.counter(name, increment[, labels])`, `metrics.gauge(name, value[, labels])`, `metrics.histogram(name, value[, labels])`, `metrics.render() -> String` (Prometheus text exposition), `metrics.reset()`.

### 4.15 `trace` (native-backed)

`trace.start(name) -> i64`, `trace.end(span)`, `trace.current() -> i64`, `trace.set_attribute(span, key, value)`, `trace.add_event(span, name)`, `trace.inject() -> String`, `trace.extract(traceparent) -> i64`, `trace.spans() -> [Json]`, `trace.reset()`.

### 4.16 `net` (native-backed)

TCP/UDP/TLS/polling primitives:

| API | Signature |
|---|---|
| `net.resolve` | `net.resolve(host) -> [String]` |
| `net.tcp_listen` | `net.tcp_listen(host, port) -> i64` |
| `net.tcp_connect` | `net.tcp_connect(host, port) -> i64` |
| `net.accept` | `net.accept(listener) -> i64` |
| `net.send` | `net.send(handle, data) -> i64` |
| `net.recv` | `net.recv(handle, maxBytes) -> String` |
| `net.close` | `net.close(handle)` |
| `net.local_port` | `net.local_port(handle) -> i64` |
| `net.set_timeout` | `net.set_timeout(handle, millis)` |
| `net.set_nonblocking` | `net.set_nonblocking(handle, bool)` |
| `net.poll` | `net.poll(handles, timeoutMillis) -> [i64]` |
| `net.udp_socket` | `net.udp_socket() -> i64` |
| `net.udp_bind` | `net.udp_bind(handle, host, port)` |
| `net.udp_send` | `net.udp_send(handle, host, port, data) -> i64` |
| `net.udp_recv` | `net.udp_recv(handle, maxBytes) -> (String, String, i64)` |
| `net.tls_connect` | `net.tls_connect(host, port) -> i64` (requires OpenSSL build) |
| `net.tls_available` | `net.tls_available() -> bool` |

### 4.17 `http` (native-backed)

| API | Signature |
|---|---|
| `http.get` | `http.get(url) -> (i64, {String: String}, String)` |
| `http.post` | `http.post(url, body) -> (i64, {String: String}, String)` |
| `http.request` | `http.request(socket, method, path, headers, body) -> (i64, {String: String}, String)` |
| `http.serve` | `http.serve(listener, handler)` (handler returning status 0 stops serving) |
| `http.serve_once` | `http.serve_once(listener, handler) -> bool` |

> TLS/HTTPS support depends on the build having OpenSSL enabled.

### 4.18 `terminal`, `env`, `repl` (native-backed, interactive REPL)

Terminal primitives used by the self-hosted A REPL (`stdlib/std/repl/`, the
IPython/`_pyrepl` analog run by `a repl` on a TTY). They are also usable from
any interpreted program.

| Module | API |
|---|---|
| `terminal` | `isatty() -> bool`, `raw_mode(enabled) -> bool`, `read_char() -> String?`, `read_char_ms(timeoutMs) -> String?`, `write(text)`, `flush()`, `size() -> [i64]` (`[cols, rows]`), `home() -> String` |
| `env` | `get(name) -> String` (empty when unset) |
| `repl` | `eval(source) -> i64` — evaluates source in the persistent REPL session (only inside `a repl`; 0 ok, 1 compile error, 2 runtime error, 3 nested eval rejected) |

The REPL UI itself is written in A: `stdlib/std/repl/{main,editor,highlight,complete,history}.a`
provides an IPython-style shell: `In [n]:` prompts, `...:` continuation,
persistent multiline-safe `~/.a_history`, up/down + PgUp/PgDn history,
Ctrl-R reverse-i-search, Tab completion (keywords/builtins/modules/user
names) with a selection menu, live syntax highlighting, ghost-text
autosuggest, bracketed-paste multiline input, and readline/emacs editing
(Ctrl-A/E/K/U/W/Y/F/B/T/_, undo, Alt-f/b/d/u/l/./Backspace/Enter,
Ctrl-Left/Right word motion, Ctrl-L). Non-code lines mimic IPython:
`name?`/`name??` inspection, `%` line magics (`%help`, `%magic`,
`%history`, `%time`, `%pwd`, `%ls`, `%who`, `%env`, `%save`, `%load`,
`%run`, `%reset`), and `!cmd` shell escapes (reported unsupported — no
shell builtin exists). Piped (non-TTY) `a repl` keeps the simple `>>>`
line mode.

## 5. `std/*` package library

`stdlib/std/` contains Mojo-inspired package structure. Some packages export pure A helpers; others are backed by native modules.

| Package | Kind | Notes |
|---|---|---|
| `std/builtin` | A helpers | wrappers/types/helpers (`Int`, `Float`, `String`, `len_of`, `range_list`, assertions) |
| `std/math` | A wrapper + native `math` | `functions.a`, `constants.a` |
| `std/collections` | A helpers | list/dict helpers over builtin behavior |
| `std/iter` | A helpers | iteration/functional helpers |
| `std/format` | A helpers | CSV/table formatting helpers |
| `std/memory` | A wrapper | arena wrappers over `arena` builtins |
| `std/simd` | A helpers | `Vec4`/`Vec8` helpers; scalar-emulated SIMD surface |
| `std/gpu` | A stubs/CPU fallback | Mojo/MAX-shaped API; `available=false` on CPU fallback |
| `std/comptime` | A helpers | compile-time-oriented helpers / runtime tags/ranges |
| `std/traits` | A traits/helpers | `Equatable`, `Hashable`, `Comparable`, `Copyable` etc. |
| `std/test` | A harness | `check`, `equal`, `require`, `summary`, `report` |
| `std/prelude` | package entry | auto-loaded vocabulary lives in `stdlib/prelude.a` |
| `std/core`, `std/fs`, `std/time`, `std/json`, `std/binary`, `std/testing`, `std/log`, `std/metrics`, `std/trace`, `std/net`, `std/http` | native-backed package stubs | `import <name>` resolves to native implementation |

### 5.1 `std.test` harness API

```a
from "std/test/test.a" import check, equal, require, report, summary

fn test_math():
    check("truthy", 1 + 1 == 2)
    equal("sum", 4, 2 + 2)
    require(true, "must hold")
```

## 6. `a test` discovery

`a test <file.a>` runs top-level functions whose names start with `test_`.

```a
fn test_addition():
    assert_eq(2 + 2, 4)
```

Use `assert`, `assert_eq`, `panic`, and/or `std/test` helpers for failures.
