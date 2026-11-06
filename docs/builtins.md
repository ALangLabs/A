# A — Built-in Functions and Constants

<!-- AUTO-GENERATED from the compiler doc tables by `tools/gen-docs/generate.py`. Do not edit by hand. -->

Every name documented here is available in every A module **without an import**. These are the same doc tables the type checker and the language server read (`builtin_docs.hpp`), so signatures and examples always match the running toolchain.

Functions marked with a module subscript (for example `math.sqrt`) are
also available through their native module; methods are documented on
[Methods](methods.md). Type names (`int`, `f64`, `String`, …) are in
[Keywords](keywords.md).

## TOC

 * [Built-in functions](#built-in-functions)
 * [Built-in constants](#built-in-constants)
 * [Numeric kernels (`kernels_*`)](#kernels)
 * [Primitive type methods](methods.md)

## Built-in functions

### `__debug_context__() -> String`

Returns `file:line in fn()` or module context.

### `__debug_repr__(value) -> String`

Returns a debug string for a value.

### `__debug_time__() -> String`

Returns a local debug timestamp `HH:MM:SS.mmm`.

### `__file__()`

Returns the current source file path.

### `__func__()`

Returns the current function name, or `<module>`.

### `__ic_format__(context[, label, value]) -> String`

Formats a litprinter-style `ic|` line. With one arg it formats a breadcrumb; with label/value it formats an inspection.

### `__line__()`

Returns the current 1-based source line.

### `abs(x) -> number`

Absolute value, keeping the argument type.

Example:

```a
>>> abs(-7)
7
```

See also: `min`, `max`

### `acos(x) -> f64`

Arc cosine, returning radians.

### `all(values, fn) -> bool`

True when the predicate holds for every element.

Example:

```a
>>> all([2, 4, 6], fn(x) => x % 2 == 0)
true
```

See also: `any`

### `any(values, fn) -> bool`

True when the predicate holds for some element.

Example:

```a
>>> any([1, 2, 3], fn(x) => x > 2)
true
```

See also: `all`

### `append_file(path, text)`

Appends text to a file.

Example:

```a
>>> append_file('log.txt', 'line 1\n')
```

See also: `write_file`

### `arena_alloc(allocator, size) -> i64`

Allocates bytes and returns a handle; errors when out of space.

### `arena_new(capacity) -> Allocator`

Bump allocator: allocations are fast, release is all-or-nothing.

### `arena_pool(capacity) -> Allocator`

Pool allocator: released blocks are reused for equal sizes.

### `arena_region(capacity) -> Allocator`

Region allocator: releases must happen in reverse allocation order.

### `arena_release(allocator, handle)`

Returns an allocation to the allocator.

### `arena_reset(allocator)`

Frees every allocation the allocator owns.

### `arena_total(allocator) -> i64`

Bytes of capacity.

### `arena_used(allocator) -> i64`

Bytes currently handed out.

### `asin(x) -> f64`

Arc sine, returning radians.

### `assert(condition[, message])`

Runtime error when the condition is false.

### `assert_eq(actual, expected[, message])`

Runtime error when the two values differ.

### `atan(x) -> f64`

Arc tangent, returning radians.

### `atan2(y, x) -> f64`

Angle of the vector (x, y) in radians.

### `binary.decode(bytes) -> Json?`

Decodes bytes; malformed or truncated input yields None.

### `binary.encode(value) -> [i64]`

Self-describing bytes, one entry per byte (0-255).

### `borrow(variable)`

Immutable borrow tracked by the analyzer.

### `borrow_mut(variable)`

Mutable borrow tracked by the analyzer.

### `bswap(x) -> i64`

Byte-swaps the 64-bit value.

### `ceil(x) -> f64`

Rounds up to the nearest integer value.

### `close(stream: IO)`

Closes a handle returned by open().

Example:

```a
>>> close(f)
```

See also: `open`

### `clz(x) -> i64`

Count of leading zero bits (64 for 0).

### `copy(value) -> T`

Deep-copies a value.

### `copysign(x, y) -> f64`

Magnitude of x with the sign of y.

### `cos(x) -> f64`

Cosine of x, in radians.

### `create_dir(path)`

Creates a directory and any missing parents.

Example:

```a
>>> create_dir('build')
```

See also: `remove_dir`

### `ctz(x) -> i64`

Count of trailing zero bits (64 for 0).

### `current_dir() -> String`

The process working directory.

Example:

```a
>>> current_dir()
'/home/user/project'
```

See also: `list_dir`

### `env.get(name) -> String`

Environment variable value, or empty.

### `eprint(*objects, sep: String = " ", end: String = "\n", file: IO = stderr, flush: bool = false) -> void`

Writes values to standard error (or file), separated, with an ending.

Example:

```a
>>> eprint('Error: bad input')
```

See also: `print`

### `Err(error) -> Result`

Wraps a value as a failed result.

### `exp(x) -> f64`

e raised to the x.

### `expect(cond, hint?) -> value`

Branch hint (llvm.expect analog); returns cond.

### `fabs(x) -> f64`

Absolute value as f64.

### `file_exists(path) -> bool`

Whether a path exists.

Example:

```a
>>> file_exists('main.a')
true
```

See also: `read_file`

### `filter(values, fn) -> array`

Keeps the elements for which a predicate is true.

Example:

```a
>>> filter([1, 2, 3, 4], fn(x) => x % 2 == 0)
[2, 4]
```

See also: `map`

### `float(value) -> f64`

Converts a number, bool, or numeric string to f64.

### `floor(x) -> f64`

Rounds down to the nearest integer value.

### `flush()`

Flushes buffered standard output.

Example:

```a
>>> flush()
```

### `fma(a, b, c) -> f64`

Fused multiply-add: a*b+c with a single rounding.

### `http.get(url) -> (i64, {String: String}, String)`

GET request; returns status, headers, and body.

### `http.post(url, body) -> (i64, {String: String}, String)`

POST request; returns status, headers, and body.

### `http.request(socket, method, path, headers, body) -> (i64, {String: String}, String)`

One request over an established net socket.

### `http.serve(listener, handler)`

Serves connections until a handler returns status 0.

### `http.serve_once(listener, handler) -> bool`

Handles one connection; true when it was served.

### `ic(*values) -> value | (values) | void`

Debug-inspects values to stderr with file/function context and source labels (litprinter-style), and returns the inspected value(s) unchanged.

Example:

```a
>>> x = 42
>>> ic(x)
ic| x: 42
>>> ic(x + 1)
ic| [app.a:2 in main()] >>> x + 1: 43
```

See also: `print`, `eprint`, `__debug_context__`, `__debug_time__`, `__debug_repr__`

### `input([prompt]) -> String`

Reads one line from standard input.

Example:

```a
>>> name = input('Name? ')
>>> name
Ada
```

### `int(value) -> i64`

Converts a number, bool, or numeric string to i64.

### `join(values, separator) -> String`

Concatenates an array of strings with a separator.

Example:

```a
>>> join(['a', 'b', 'c'], '-')
'a-b-c'
```

See also: `String.join`

### `json.as_array(doc) -> [Json]?`

A JSON array, or None.

### `json.as_bool(doc) -> bool?`

A JSON bool, or None.

### `json.as_float(doc) -> f64?`

Any JSON number as f64, or None.

### `json.as_int(doc) -> i64?`

An integral JSON number as i64, or None.

### `json.as_map(doc) -> {String: Json}?`

A JSON object as a map, or None.

### `json.as_str(doc) -> String?`

The JSON string, or None when the kind differs.

### `json.at(doc, index) -> Json?`

Value at an array index, or None.

Example:

```a
>>> doc = json.parse('[1, 2, 3]')
>>> json.at(doc, 1)
2
```

See also: `json.len`

### `json.get(doc, key) -> Json?`

Value for an object key, or None.

Example:

```a
>>> doc = json.parse('{"a": 1}')
>>> json.get(doc, 'a')
1
>>> json.get(doc, 'b')
None
```

See also: `json.has`

### `json.has(doc, key) -> bool`

Key presence in an object.

### `json.is_valid(text) -> bool`

Whether the text parses, without raising errors.

Example:

```a
>>> json.is_valid('[1, 2]')
true
>>> json.is_valid('nope')
false
```

See also: `json.parse`

### `json.keys(doc) -> [String]`

Object keys in document order.

### `json.len(doc) -> i64`

Members of an array or object, characters of a string.

### `json.parse(text) -> Json`

Parses one JSON document; malformed input is a runtime error.

Example:

```a
>>> import std:json
>>> doc = json.parse('[1, 2, 3]')
>>> doc
[1, 2, 3]
```

See also: `json.stringify`

### `json.pretty(value) -> String`

Indented JSON text with two-space indent.

Example:

```a
>>> json.pretty(doc)
'[
  1,
  2
]'
```

See also: `json.stringify`

### `json.stringify(value) -> String`

Compact JSON text for any serializable A value.

Example:

```a
>>> import std:json
>>> json.stringify([1, 2, 3])
'[1, 2, 3]'
```

See also: `json.parse`

### `json.kind(doc) -> String`

One of null, bool, int, float, string, array, object.

### `len(value) -> i64`

Length of a string, array, map, or set.

Example:

```a
>>> len('A language')
9
>>> len([1, 2, 3])
3
```

See also: `str`

### `likely(cond) -> value`

Marks a condition as likely; returns it unchanged.

### `list_dir(path) -> [String]`

Sorted entry names of a directory.

Example:

```a
>>> list_dir('.')
['README.md', 'build', 'main.a']
```

See also: `file_exists`

### `log(x) -> f64`

Natural logarithm.

### `log10(x) -> f64`

Base-10 logarithm.

### `log2(x) -> f64`

Base-2 logarithm.

### `log.debug(message[, fields])`

Debug-level record to stderr.

### `log.error(message[, fields])`

Error record to stderr.

### `log.info(message[, fields])`

Info-level record to stderr.

### `log.set_json(bool)`

True writes one JSON object per line.

### `log.set_level(level)`

One of debug, info, warn, or error.

### `log.warn(message[, fields])`

Warning record to stderr.

### `map(values, fn) -> array`

Applies a function to every element.

Example:

```a
>>> map([1, 2, 3], fn(x) => x * 2)
[2, 4, 6]
```

See also: `filter`

### `max(a, b) -> number`

The larger of two values of the same numeric type.

Example:

```a
>>> max(3, 7)
7
```

See also: `min`

### `metrics.counter(name, increment[, labels])`

Adds a non-negative integer to a counter.

### `metrics.gauge(name, value[, labels])`

Sets a gauge to the value.

### `metrics.histogram(name, value[, labels])`

Records one observation in a histogram.

### `metrics.render() -> String`

Prometheus text exposition format.

### `metrics.reset()`

Forgets every series.

### `min(a, b) -> number`

The smaller of two values of the same numeric type.

Example:

```a
>>> min(3, 7)
3
```

See also: `max`

### `move(value) -> T`

Moves ownership out of a binding.

### `net.accept(listener) -> i64`

Next incoming connection; blocks unless a timeout is set.

### `net.close(handle)`

Releases the socket.

### `net.local_port(handle) -> i64`

The bound port.

### `net.poll(handles, timeoutMillis) -> [i64]`

Handles that are ready to read.

### `net.recv(handle, maxBytes) -> String`

Up to maxBytes bytes; empty at EOF or when nothing is waiting.

### `net.resolve(host) -> [String]`

Numeric addresses in resolver order.

### `net.send(handle, data) -> i64`

Bytes written.

### `net.set_nonblocking(handle, bool)`

Non-blocking mode.

### `net.set_timeout(handle, millis)`

Send/receive timeout; 0 clears it.

### `net.tcp_connect(host, port) -> i64`

Connected TCP socket.

### `net.tcp_listen(host, port) -> i64`

Listening socket; port 0 asks the OS for one.

### `net.tls_available() -> bool`

Whether the toolchain was built with TLS support.

### `net.tls_connect(host, port) -> i64`

TLS client socket; requires a build with OpenSSL.

### `net.udp_bind(handle, host, port)`

Binds a datagram socket; port 0 is ephemeral.

### `net.udp_recv(handle, maxBytes) -> (String, String, i64)`

Payload, peer host, and peer port; ("", "", -1) when idle.

### `net.udp_send(handle, host, port, data) -> i64`

Bytes sent in one datagram.

### `net.udp_socket() -> i64`

Unbound datagram socket.

### `now() -> i64`

Current time as Unix epoch milliseconds.

### `Ok(value) -> Result`

Wraps a value as a successful result.

### `open(path: String, mode: String = "w") -> IO`

Opens a file for writing; mode is "w" (create/truncate), "a" (append), or "r".

Example:

```a
>>> f = open('out.txt', 'w')
>>> print('A!', file=f)
>>> close(f)
```

See also: `close`, `print`

### `panic(message)`

Aborts with exit status 2 and the message.

### `popcount(x) -> i64`

Number of set bits.

### `pow(base, exponent) -> f64`

Raises base to the exponent.

Example:

```a
>>> pow(2, 10)
1024.0
```

### `print(*objects, sep: String = " ", end: String = "\n", file: IO = stdout, flush: bool = false) -> void`

Writes values to a stream, separated, with an ending.

Example:

```a
>>> print('Hello, A!')
Hello, A!
>>> print(1, 2, sep='-')
1-2
```

See also: `eprint`, `open`, `close`, `flush`

### `random() -> f64`

A pseudo-random float in [0, 1).

### `random_int(low, high) -> i64`

A pseudo-random integer between low and high, inclusive.

### `read_file(path) -> String`

Reads an entire text file.

Example:

```a
>>> text = read_file('notes.txt')
>>> text
Hello, A!
```

See also: `write_file`, `file_exists`

### `reduce(values, initial, fn) -> value`

Folds an array from left to right.

Example:

```a
>>> reduce([1, 2, 3], 0, fn(acc, x) => acc + x)
6
```

See also: `sum`

### `remove_dir(path)`

Removes an empty directory.

Example:

```a
>>> remove_dir('build')
```

See also: `create_dir`

### `remove_file(path)`

Deletes a file.

Example:

```a
>>> remove_file('tmp.txt')
```

See also: `file_exists`

### `rename(old, new)`

Renames or moves a path.

Example:

```a
>>> rename('old.a', 'new.a')
```

See also: `remove_file`

### `repl.eval(source) -> i64`

Evaluates A source in the persistent REPL session; 0 ok, 1 compile error, 2 runtime error.

### `rotl(x, k) -> i64`

Rotates left by k bits (k taken mod 64).

### `rotr(x, k) -> i64`

Rotates right by k bits (k taken mod 64).

### `round(x) -> f64`

Rounds to the nearest integer value.

### `sadd_overflow(a, b) -> (i64, bool)`

Signed addition; returns (wrapped result, overflowed).

### `set(values) -> Set`

Builds a set from an array, range, set, or string.

Example:

```a
>>> set([1, 2, 2, 3])
{1, 2, 3}
```

See also: `Set methods`

### `sin(x) -> f64`

Sine of x, in radians.

### `sleep(milliseconds)`

Pauses for a non-negative duration.

### `smax(a, b) -> i64`

Signed maximum.

### `smin(a, b) -> i64`

Signed minimum.

### `smul_overflow(a, b) -> (i64, bool)`

Signed multiplication; returns (wrapped result, overflowed).

### `Some(value) -> T?`

Wraps a value in an optional.

### `sqrt(x) -> f64`

Square root of x.

Example:

```a
>>> import std:math
>>> math.sqrt(16)
4.0
```

See also: `floor`, `ceil`, `round`

### `ssub_overflow(a, b) -> (i64, bool)`

Signed subtraction; returns (wrapped result, overflowed).

### `str(value) -> String`

Converts a value to its string representation.

Example:

```a
>>> str(42)
'42'
>>> str([1, 2])
'[1, 2]'
```

See also: `print`

### `sum(values) -> number`

Sum of an array of numbers.

Example:

```a
>>> sum([1, 2, 3])
6
```

See also: `reduce`

### `tan(x) -> f64`

Tangent of x, in radians.

### `terminal.flush()`

Flushes stdout.

### `terminal.home() -> String`

The user's home directory.

### `terminal.isatty() -> bool`

Whether stdin and stdout are terminals.

### `terminal.raw_mode(enabled) -> bool`

Switches the terminal to per-character input; returns the previous state.

### `terminal.read_char() -> String?`

Reads one UTF-8 character; None on EOF.

### `terminal.read_char_ms(timeoutMs) -> String?`

Reads one UTF-8 character, or None on timeout/EOF.

### `terminal.size() -> [i64]`

Terminal size as [columns, rows].

### `terminal.write(text)`

Writes text to stdout without a trailing newline.

### `testing.for_all_bool(runs, fn(bool) -> bool) -> bool`

Random bool property over generated inputs.

### `testing.for_all_elem(values, fn(T) -> bool) -> bool`

Checks the property for every element of an array.

### `testing.for_all_float(runs, low, high, fn(f64) -> bool) -> bool`

Random float property; the first counterexample goes to stderr.

### `testing.for_all_int(runs, low, high, fn(i64) -> bool) -> bool`

Random int property; the first counterexample goes to stderr.

### `testing.for_all_string(runs, maxLength, fn(String) -> bool) -> bool`

Random printable-ASCII string property.

### `testing.seed(value)`

Fixes the generator so a run is reproducible.

### `trace.add_event(span, name)`

Timestamped event on a span.

### `trace.current() -> i64`

The active span id, or 0 when nothing is active.

### `trace.end(span)`

Finishes a span and pops it from the stack.

### `trace.extract(traceparent) -> i64`

Continues a trace from a W3C traceparent header.

### `trace.inject() -> String`

traceparent header for the active trace, or "".

### `trace.reset()`

Forgets every span.

### `trace.set_attribute(span, key, value)`

Attaches an attribute; the same key overwrites.

### `trace.spans() -> [Json]`

One record per span in creation order.

### `trace.start(name) -> i64`

Begins a span nested under the active one; returns its id.

### `trunc(x) -> f64`

Truncates toward zero.

### `uadd_overflow(a, b) -> (i64, bool)`

Unsigned addition; returns (wrapped result, overflowed).

### `umax(a, b) -> i64`

Unsigned maximum (bit-pattern compare).

### `umin(a, b) -> i64`

Unsigned minimum (bit-pattern compare).

### `umul_overflow(a, b) -> (i64, bool)`

Unsigned multiplication; returns (wrapped result, overflowed).

### `unlikely(cond) -> value`

Marks a condition as unlikely; returns it unchanged.

### `unsafe_address_of(array, index) -> *T`

Raw pointer to an array element.

### `unsafe_get(array, index) -> T`

Unchecked element access; no bounds check.

### `unsafe_load(pointer) -> T`

Reads the value a pointer refers to.

### `unsafe_offset(pointer, count) -> *T`

Moves a pointer by a number of elements.

### `unsafe_set(array, index, value)`

Unchecked element assignment; no bounds check.

### `unsafe_store(pointer, value)`

Writes through a raw pointer.

### `usub_overflow(a, b) -> (i64, bool)`

Unsigned subtraction; returns (wrapped result, overflowed).

### `write_file(path, text)`

Creates or replaces a file with text.

Example:

```a
>>> write_file('out.txt', 'Hello, A!')
```

See also: `read_file`, `append_file`

## Built-in constants

These compile-time constants are available without import.

### `E: f64`

Euler's number, the base of the natural logarithm.

### `PI: f64`

The ratio of a circle's circumference to its diameter.

### `stderr: IO`

The standard error stream handle.

### `stdout: IO`

The standard output stream handle.

## Numeric kernels (`kernels_*`)

Tensor/numeric primitives exposed through the `kernels` package.

| Function | Description |
|---|---|
| `kernels_attention` | kernel_ops.attention(q, k, v) -> [f64] — Single-head scaled dot-product attention output for one query. |
| `kernels_attention_scores` | kernel_ops.attention_scores(q, k) -> [f64] — Scaled dot-product attention scores for one query and a key matrix. |
| `kernels_avg_pool1d` | kernel_ops.avg_pool1d(input, size, stride) -> [f64] — 1D average pooling. |
| `kernels_bias_relu` | kernel_ops.bias_relu(xs, bias) -> [f64] — Elementwise max(x + bias, 0) as floats. |
| `kernels_conv1d` | kernel_ops.conv1d(input, kernel, stride) -> [number] — 1D cross-correlation convolution. |
| `kernels_dequantize_int8` | kernel_ops.dequantize_int8(xs, scale) -> [f64] — Dequantizes int8 values by a positive scale. |
| `kernels_diag` | kernel_ops.diag(mat) -> [number] — Main diagonal of a rectangular numeric matrix. |
| `kernels_dot` | kernel_ops.dot(a, b) -> number — Dot product of two equal-length numeric arrays. |
| `kernels_elu` | kernel_ops.elu(xs, alpha) -> [f64] — Elementwise exponential linear unit. |
| `kernels_eye` | kernel_ops.eye(n) -> [[i64]] — n by n identity matrix. |
| `kernels_full` | kernel_ops.full(shape, value) -> array — Nested array filled with a numeric value. |
| `kernels_fused_add` | kernel_ops.fused_add(a, b) -> [number] — Elementwise a + b over two numeric arrays. |
| `kernels_fused_clamp` | kernel_ops.fused_clamp(xs, low, high) -> [number] — Elementwise clamp of xs into [low, high]. |
| `kernels_fused_div` | kernel_ops.fused_div(a, b) -> [number] — Elementwise a / b over two numeric arrays. |
| `kernels_fused_exp` | kernel_ops.fused_exp(xs) -> [f64] — Elementwise e^x as floats. |
| `kernels_fused_log` | kernel_ops.fused_log(xs) -> [f64] — Elementwise natural log as floats. |
| `kernels_fused_max` | kernel_ops.fused_max(a, b) -> [number] — Elementwise maximum of two numeric arrays. |
| `kernels_fused_min` | kernel_ops.fused_min(a, b) -> [number] — Elementwise minimum of two numeric arrays. |
| `kernels_fused_mul` | kernel_ops.fused_mul(a, b) -> [number] — Elementwise a * b over two numeric arrays. |
| `kernels_fused_mul_add` | kernel_ops.fused_mul_add(a, b, c) -> [number] — Elementwise a * b + c over three numeric arrays. |
| `kernels_fused_pow` | kernel_ops.fused_pow(a, b) -> [number] — Elementwise a ** b as floats. |
| `kernels_fused_reciprocal` | kernel_ops.fused_reciprocal(xs) -> [f64] — Elementwise 1 / x as floats. |
| `kernels_fused_scale` | kernel_ops.fused_scale(xs, scale) -> [number] — Elementwise xs * scale. |
| `kernels_fused_sqrt` | kernel_ops.fused_sqrt(xs) -> [f64] — Elementwise square root as floats. |
| `kernels_fused_sub` | kernel_ops.fused_sub(a, b) -> [number] — Elementwise a - b over two numeric arrays. |
| `kernels_gelu` | kernel_ops.gelu(xs) -> [f64] — Elementwise Gaussian error linear unit. |
| `kernels_layer_norm` | kernel_ops.layer_norm(xs, eps) -> [f64] — Layer normalization of a single vector. |
| `kernels_layer_norm_rows` | kernel_ops.layer_norm_rows(mat, eps) -> [[f64]] — Layer normalization applied to each matrix row. |
| `kernels_leaky_relu` | kernel_ops.leaky_relu(xs, alpha) -> [f64] — Elementwise leaky ReLU. |
| `kernels_matmul` | kernel_ops.matmul(a, b) -> [[number]] — Dense matrix multiply of two rectangular numeric matrices. |
| `kernels_matvec` | kernel_ops.matvec(mat, vec) -> [number] — Matrix-vector product; each row must match vec's length. |
| `kernels_max_pool1d` | kernel_ops.max_pool1d(input, size, stride) -> [number] — 1D max pooling. |
| `kernels_mish` | kernel_ops.mish(xs) -> [f64] — Elementwise Mish activation. |
| `kernels_normalize` | kernel_ops.normalize(xs) -> [f64] — L2-normalize a numeric vector. |
| `kernels_ones` | kernel_ops.ones(shape) -> array — Nested one array for a positive integer shape. |
| `kernels_outer` | kernel_ops.outer(a, b) -> [[number]] — Outer product of two vectors. |
| `kernels_quantize_int8` | kernel_ops.quantize_int8(xs, scale) -> [i64] — Symmetric int8 quantization with a positive scale. |
| `kernels_reduce_argmax` | kernel_ops.argmax(xs) -> i64 — Index of the first maximum in a non-empty array. |
| `kernels_reduce_argmin` | kernel_ops.argmin(xs) -> i64 — Index of the first minimum in a non-empty array. |
| `kernels_reduce_max` | kernel_ops.reduce_max(xs) -> number — Maximum of a non-empty numeric array. |
| `kernels_reduce_mean` | kernel_ops.reduce_mean(xs) -> f64 — Mean of a non-empty numeric array. |
| `kernels_reduce_min` | kernel_ops.reduce_min(xs) -> number — Minimum of a non-empty numeric array. |
| `kernels_reduce_norm` | kernel_ops.reduce_norm(xs, ord) -> f64 — Vector norm; ord must be 1 or 2. |
| `kernels_reduce_prod` | kernel_ops.reduce_prod(xs) -> number — Product of a numeric array (1 for empty is rejected; empty fails). |
| `kernels_reduce_std` | kernel_ops.reduce_std(xs) -> f64 — Population standard deviation of a non-empty numeric array. |
| `kernels_reduce_sum` | kernel_ops.reduce_sum(xs) -> number — Sum of a numeric array (0 for empty). |
| `kernels_reduce_var` | kernel_ops.reduce_var(xs) -> f64 — Population variance of a non-empty numeric array. |
| `kernels_relu` | kernel_ops.relu(xs) -> [number] — Elementwise max(x, 0). |
| `kernels_reshape` | kernel_ops.reshape(data, shape) -> array — Reshapes a flat array into a nested shape; total size must match. |
| `kernels_rms_norm` | kernel_ops.rms_norm(xs, eps) -> [f64] — Root-mean-square normalization of a vector. |
| `kernels_sigmoid` | kernel_ops.sigmoid(xs) -> [f64] — Elementwise 1 / (1 + e^-x). |
| `kernels_silu` | kernel_ops.silu(xs) -> [f64] — Elementwise Sigmoid-weighted Linear Unit (SiLU/Swish). |
| `kernels_softmax` | kernel_ops.softmax(xs) -> [f64] — Numerically stable softmax of a non-empty vector. |
| `kernels_softmax_rows` | kernel_ops.softmax_rows(mat) -> [[f64]] — Row-wise softmax of a numeric matrix. |
| `kernels_softplus` | kernel_ops.softplus(xs) -> [f64] — Elementwise softplus, computed stably. |
| `kernels_tanh` | kernel_ops.tanh(xs) -> [f64] — Elementwise hyperbolic tangent. |
| `kernels_trace` | kernel_ops.trace(mat) -> number — Trace of a square numeric matrix. |
| `kernels_transpose` | kernel_ops.transpose(mat) -> [[number]] — Transpose of a rectangular numeric matrix. |
| `kernels_zeros` | kernel_ops.zeros(shape) -> array — Nested zero array for a positive integer shape. |

