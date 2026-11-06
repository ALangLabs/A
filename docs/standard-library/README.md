# A Standard Library & API Reference

This reference documents the current A standard library surface as implemented by:

- `compiler/stages/name-resolution/native_modules.hpp` (native module namespaces)
- `compiler/stages/name-resolution/builtin_docs.hpp` (builtin docs/signatures used by tooling)
- `compiler/stages/type-checker/sema.cpp` (builtin recognition and type behavior)
- `compiler/stages/type-checker/method_signatures.hpp` (array/map/set/string method surfaces)
- `stdlib/prelude.a` and `stdlib/std/*`
- integration tests in `tests/integration/cases/`

> **Runtime note:** A currently executes with dynamic/Python-style semantics in many paths. Some stdlib/native-backed features (especially `json`, `binary`, `testing`, `log`, `metrics`, `trace`, `net`, `http`) are implemented in the runtime/builtin layer and are primarily exercised through the interpreter/KGEN execution engine path. Availability/performance characteristics may differ on the LLVM AOT path.

## 1. Prelude (auto-loaded)

`stdlib/prelude.a` is auto-loaded by the module loader when a stdlib root is found. It provides namespaced vocabulary and helpers.

### 1.1 `prelude_version()`

```a
print(prelude_version())  # "0.0.1"
```

### 1.2 `Vec4` (SIMD-like value type, width 4)

```a
struct Vec4:
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

```a
fn Ok(value):        # -> Result(ok=true, value, error=None)
fn Err(message):     # -> Result(ok=false, value=None, error)
fn unwrap(r):        # returns value or panics with r.error
fn is_ok(r) -> bool
fn is_err(r) -> bool
fn unwrap_or(r, fallback)
fn err_value(r)
fn map_result(r, f: fn(i64) -> i64)
fn and_then(r, f: fn(i64) -> Result) -> Result
fn vectorize(f: fn(i64) -> i64, xs: [i64]) -> [i64]
```

Example:

```a
fn divide(a, b):
    if b == 0:
        return Err("division by zero")
    return Ok(a / b)

print(is_ok(divide(10, 2)))
print(unwrap_or(divide(10, 0), -1))
```

## 2. Global builtins and constants

These names are recognized by the type checker/runtime as builtins or builtin constants (subject to shadowing rules). Some are also exposed via native modules.

### 2.1 Core I/O and conversion

| API | Signature (tooling) | Description |
|---|---|---|
| `print` | `print(*objects, sep: String = " ", end: String = "\n", file: IO = stdout, flush: bool = false) -> void` | Write values to a stream, separated, with an ending |
| `eprint` | `eprint(*objects, sep: String = " ", end: String = "\n", file: IO = stderr, flush: bool = false) -> void` | Write values to stderr (or `file`), separated, with an ending |
| `open` | `open(path: String, mode: String = "w") -> IO` | Open a file (`"w"`, `"a"`, `"r"`) for `print(file=...)` |
| `close` | `close(stream: IO)` | Close a handle returned by `open()` |
| `stdout` / `stderr` | `stdout: IO`, `stderr: IO` | Stream handles usable as `print(file=stdout)` |
| `input` | `input([prompt]) -> String` | Read a line from stdin |
| `flush` | `flush()` | Flush stdout |
| `len` | `len(value) -> i64` | Length of string/array/map/set (and supported containers) |
| `str` | `str(value) -> String` | Convert value to string |
| `int` | `int(value) -> i64` | Convert number/bool/numeric string to `i64` |
| `float` | `float(value) -> f64` | Convert number/bool/numeric string to `f64` |

### 2.2 Math / numeric builtins

| API | Signature (tooling) | Description |
|---|---|---|
| `abs` | `abs(x) -> number` | Absolute value |
| `min` | `min(a, b) -> number` | Smaller of two values |
| `max` | `max(a, b) -> number` | Larger of two values |
| `pow` | `pow(base, exponent) -> f64` | Power |
| `sqrt` | `sqrt(x) -> f64` | Square root |
| `sin/cos/tan` | `sin(x) -> f64` etc. | Trig (radians) |
| `asin/acos/atan/atan2` | `atan2(y, x) -> f64` | Inverse trig / angle |
| `exp/log/log2/log10` | `log(x) -> f64` etc. | Exponential/logarithms |
| `floor/ceil/round/trunc` | `round(x) -> f64` etc. | Rounding |
| `random` | `random() -> f64` | Pseudo-random float in `[0, 1)` |
| `random_int` | `random_int(low, high) -> i64` | Inclusive pseudo-random integer |
| `sum` | `sum(values) -> number` | Sum numeric array |

Sized conversion builtins are recognized by the checker: `i8,i16,i32,i64,u8,u16,u32,u64,f32,f64`.

### 2.3 Collection / functional builtins

| API | Signature (tooling) | Description |
|---|---|---|
| `join` | `join(values, separator) -> String` | Join array of strings |
| `map` | `map(values, fn) -> array` | Apply function to elements |
| `filter` | `filter(values, fn) -> array` | Keep elements matching predicate |
| `reduce` | `reduce(values, initial, fn) -> value` | Left fold |
| `any` | `any(values, fn) -> bool` | Any element matches |
| `all` | `all(values, fn) -> bool` | All elements match |
| `set` | `set(values) -> Set` | Build set from iterable-like values |

### 2.4 Optionals / results / assertions / control helpers

| API | Signature (tooling) | Description |
|---|---|---|
| `Some` | `Some(value) -> T?` | Wrap in optional |
| `Ok` | `Ok(value) -> Result` | Success result |
| `Err` | `Err(error) -> Result` | Error result |
| `panic` | `panic(message)` | Abort with message (exit status 2) |
| `assert` | `assert(condition[, message])` | Runtime assertion |
| `assert_eq` | `assert_eq(actual, expected[, message])` | Equality assertion |

### 2.5 Memory / ownership / borrow / unsafe builtins

| API | Signature (tooling) | Description |
|---|---|---|
| `move` | `move(value) -> T` | Move ownership out of a binding (tracked semantics) |
| `copy` | `copy(value) -> T` | Deep copy a value |
| `borrow` | `borrow(variable)` | Immutable borrow (analyzer-tracked) |
| `borrow_mut` | `borrow_mut(variable)` | Mutable borrow (analyzer-tracked) |
| `unsafe_get` | `unsafe_get(array, index) -> T` | Unchecked element access |
| `unsafe_set` | `unsafe_set(array, index, value)` | Unchecked element assignment |
| `unsafe_address_of` | `unsafe_address_of(array, index) -> *T` | Raw pointer to element |
| `unsafe_load` | `unsafe_load(pointer) -> T` | Load through raw pointer |
| `unsafe_store` | `unsafe_store(pointer, value)` | Store through raw pointer |
| `unsafe_offset` | `unsafe_offset(pointer, count) -> *T` | Pointer offset |

### 2.6 Allocator / arena builtins

| API | Signature (tooling) | Description |
|---|---|---|
| `arena_new` | `arena_new(capacity) -> Allocator` | Bump allocator |
| `arena_pool` | `arena_pool(capacity) -> Allocator` | Pool allocator (reuses equal-size blocks) |
| `arena_region` | `arena_region(capacity) -> Allocator` | Region allocator (reverse-order release) |
| `arena_alloc` | `arena_alloc(allocator, size) -> i64` | Allocate bytes, returns handle |
| `arena_release` | `arena_release(allocator, handle)` | Release allocation |
| `arena_reset` | `arena_reset(allocator)` | Free all allocations |
| `arena_used` | `arena_used(allocator) -> i64` | Bytes in use |
| `arena_total` | `arena_total(allocator) -> i64` | Capacity |

### 2.7 Builtin constants

Recognized constants include (see `builtin_docs.hpp` / `sema.cpp`):

```text
PI, E
I8_MIN, I8_MAX, I16_MIN, I16_MAX, I32_MIN, I32_MAX, I64_MIN, I64_MAX
U8_MAX, U16_MAX, U32_MAX
F32_MIN, F32_MAX, F64_MIN, F64_MAX
```

## 3. Collection and primitive method API

Method surfaces are defined in `compiler/stages/type-checker/method_signatures.hpp` and used by the type checker/LSP.

### 3.1 Array methods

| Method | Signature | Description |
|---|---|---|
| `append` | `append(v)` | Add element to end |
| `extend` | `extend(xs)` | Append all elements from another array |
| `insert` | `insert(i, v)` | Insert before index |
| `remove_at` | `remove_at(i)` | Remove and return element at index |
| `pop` | `pop([i])` | Remove/return last (or index) |
| `remove` | `remove(v)` | Remove first occurrence |
| `clear` | `clear()` | Remove all elements |
| `copy` | `copy()` | Shallow copy |
| `count` | `count(v)` | Count occurrences |
| `index` | `index(v[, start[, end]])` | First index; error if absent |
| `contains` | `contains(v)` | Membership test |
| `sort` | `sort()` | Sort in place |
| `reverse` | `reverse()` | Reverse in place |
| `first` | `first()` | First element as optional |
| `last` | `last()` | Last element as optional |
| `join` | `join(sep)` | Join elements with separator |

### 3.2 Map methods

| Method | Signature | Description |
|---|---|---|
| `get` | `get(k[, default])` | Value for key, default or `None` |
| `pop` | `pop(k[, default])` | Remove key and return value/default |
| `contains` | `contains(k)` | Key membership |
| `keys` | `keys()` | Keys array |
| `values` | `values()` | Values array |
| `items` | `items()` | Array of `(key, value)` tuples |
| `update` | `update(other)` | Copy entries from another map |
| `copy` | `copy()` | Shallow copy |
| `clear` | `clear()` | Remove all entries |

Indexing (`m[key]`) and assignment (`m[key] = value`) are also supported by the language/runtime.

### 3.3 Set methods

| Method | Signature | Description |
|---|---|---|
| `add` | `add(v)` | Insert value |
| `remove` | `remove(v)` | Remove value; error if absent |
| `discard` | `discard(v)` | Remove if present |
| `contains` | `contains(v)` | Membership |
| `union` | `union(s)` | Union |
| `intersection` | `intersection(s)` | Intersection |
| `difference` | `difference(s)` | Difference |
| `issubset` | `issubset(s)` | Subset test |
| `issuperset` | `issuperset(s)` | Superset test |
| `copy` | `copy()` | Shallow copy |
| `clear` | `clear()` | Remove all elements |

### 3.4 String methods

| Method | Signature | Description |
|---|---|---|
| `capitalize/casefold/lower/upper/swapcase/title` | `upper()` etc. | Case transforms |
| `center/ljust/rjust/zfill` | `center(width[, fill])` | Padding/alignment |
| `count` | `count(sub[, start[, end]])` | Count substring occurrences |
| `find/rfind` | `find(sub[, start[, end]])` | Index or `-1` |
| `index/rindex` | `index(sub[, start[, end]])` | Index or error |
| `startswith/endswith` | `startswith(prefix[, start[, end]])` | Prefix/suffix tests |
| `expandtabs` | `expandtabs([tabsize])` | Expand tabs |
| `strip/lstrip/rstrip` | `strip([chars])` | Trim characters |
| `partition/rpartition` | `partition(sep)` | Split once into 3-tuple |
| `removeprefix/removesuffix` | `removeprefix(affix)` | Remove affix when present |
| `replace` | `replace(old, new[, count])` | Replace occurrences |
| `split/rsplit/splitlines` | `split([sep[, maxsplit]])` | Split into arrays |
| `join` | `join(parts)` | Join array with string separator |
| `isalnum/isalpha/isascii/isdecimal/isdigit/isidentifier/islower/isnumeric/isprintable/isspace/istitle/isupper` | `isdigit()` etc. | Character-class predicates |
| `contains` | `contains(sub)` | Substring membership |
| `repeat` | `repeat(n)` | Repeat string |
| `chars` | `chars()` | Array of characters |

Strings also support interpolation (`"{expr}"`), indexing/slicing operations as implemented by the runtime, and concatenation/operators where supported.

## 4. Native modules (toolchain-backed)

Import native modules by bare name:

```a
import math
import fs
import json
from net import tcp_connect
```

Native module members map to runtime builtins (see `native_modules.hpp`).

### 4.1 `math`

Functions: `sqrt, abs, min, max, pow, sin, cos, tan, asin, acos, atan, atan2, exp, log, log2, log10, floor, ceil, round, trunc, random, random_int`  
Constants: `PI`, `E`

```a
import math
print(math.sqrt(81))
print(math.PI)
```

### 4.2 `io`

Functions: `print, eprint, input, flush`

```a
import io
io.print("hello")
```

### 4.3 `fs`

Functions: `read_file, write_file, append_file, file_exists, list_dir, remove_file, create_dir, remove_dir, rename, current_dir`

```a
import fs
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
import json
doc = json.parse("{\"a\": 1, \"b\": [true, null]}")
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
provides `In [n]:` prompts, persistent `~/.a_history`, up/down history,
Ctrl-R reverse-i-search, Tab completion (keywords/builtins/modules/user
names), live syntax highlighting, ghost-text autosuggest, multiline
bracket/`:` continuation, and kill/yank editing (Ctrl-A/E/K/U/W/Y, Ctrl-L).
Piped (non-TTY) `a repl` keeps the simple `>>>` line mode.

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
