# A Language Specification (Current Implementation)

This document describes the syntax and semantics implemented by the A compiler in this repository. It is based on the lexer, parser, AST, type checker, name resolution/module loader, and integration tests.

## 1. Source files and execution model

- Source files use the `.a` extension.
- A program is a sequence of top-level statements.
- Indentation defines blocks (Python-style suites introduced by `:`).
- **Spaces only** for indentation; tabs are rejected by the lexer.
- Newlines terminate statements in most contexts.
- The runtime semantics are currently **dynamic / Python-style** in important areas:
  - plain assignment can create or rebind variables
  - many operations are checked at runtime
  - annotations are optional and used for documentation, tooling, and typed checking where applied
- The toolchain supports `run`, `check`, `test`, `build`, `fmt`, `doc`, and `ir` commands (see [CLI Reference](../cli/README.md)).

## 2. Lexical structure

### 2.1 Comments

```a
# line comment
## doc comment (captured by lexer as a doc comment token/comment record)
```

`##` comments are recognized as doc comments by the lexer/tooling.

Python-style docstrings are also supported: a bare string literal (typically
triple-quoted) documents the module when it is the first statement, a function
when it is the first statement of its body or sits directly above the `fn`,
and a type/trait/enum or member when it appears inside the body — the first
string-literal statement documents the type itself, later ones document the
member that follows. `a doc` and `a-lsp` hover pick these up; `##` comments
take precedence when both are present.

### 2.2 Identifiers

Identifiers start with a letter or `_`, followed by letters, digits, or `_`.

Examples:

```a
name
_value
Point2D
json_parse
```

### 2.3 Keywords

Reserved keywords (lexer):

```text
fn if elif else while for in loop break continue return defer
class metaclass pass self true false and or not
import from as match None enum do type trait where comptime impl
```

Notes:
- `Class` and `Metaclass` are also accepted as keyword forms for `class`/`metaclass`.
- `true`/`false` are boolean literals; `None` is the null/empty optional literal identifier.

### 2.4 Literals

#### Integer literals

```a
0
42
1_000_000
0xFF        # hexadecimal
0b1010      # binary
```

#### Float literals

```a
3.14
0.5
1e9
2.5e-3
1_2.3_4
```

A `.` starts a fractional part only when followed by a digit, so `1..5` lexes as `1`, `..`, `5`.

#### Boolean literals

```a
true
false
```

#### Character literals

```a
'a'
'\n'
'\x41'
'\u{1F600}'
```

#### String literals

```a
"hello"
"line\nbreak"
"sum = {2 + 3}"      # interpolation
"""multi-line
raw text"""
```

- Normal strings support escapes and interpolation.
- Triple-quoted strings (`"""..."""`) are treated as raw strings (content captured as-is; interpolation/escape decoding behavior follows parser implementation for raw strings). In statement position they double as docstrings (see [Comments](#21-comments)).
- Interpolation uses `{expr}` inside normal strings.

Supported escapes (lexer/parser support includes):

```text
\n \t \r \a \b \f \v \0 \xNN \u{...} \\ \" \' \{ \}
```

#### Array literals

```a
[]
[1, 2, 3]
["a", "b"]
```

#### Map literals

```a
{}
{"a": 1, "b": 2}
```

`{}` is an empty map. An empty set is created with `set()`.

#### Set literals

```a
{1, 2, 3}
{"x", "y"}
```

A brace literal becomes a set when elements are not `key: value` pairs.

#### Tuple literals

```a
(1, 2)
("ok", 42)
```

Parentheses without a comma are grouping, not a 1-element tuple.

### 2.5 Operators and punctuation

Tokens/operators implemented by the lexer/parser include:

```text
( ) [ ] { } , : . .. -> => |>
+ - * / % **        ! ? ?. ?? ~ & | ^ << >>
== != < > <= >=
= += -= *= /= %= &= |= ^= <<= >>=
++ --
```

Keywords used as operators in expressions/statements: `and`, `or`, `not`, `in`, `not in`.

## 3. Program structure

### 3.1 Top-level declarations/statements

Common top-level forms:

```a
import "lib/util.a"
from "lib/util.a" import helper
import math

fn main():
    print("hi")

class Point:
    x
    y

enum Color:
    Red
    Green

trait Equatable:
    fn eq(self, other) -> bool

impl Equatable for Point:
    pass

type UserId = i64
```

Imports are top-level only.

### 3.2 Blocks and indentation

A `:` introduces a block (suite). A suite may be:

- an indented block on the next lines, or
- an inline/simple form in some constructs supported by the parser (e.g. expression-bodied functions / simple statements where accepted)

```a
if ok:
    print("yes")
else:
    print("no")
```

### 3.3 `pass`

`pass` is accepted as a no-op placeholder in supported contexts (e.g. empty class/impl bodies).

## 4. Types and annotations

A is dynamically executed, but supports optional type syntax/annotations.

### 4.1 Built-in / named types

The simple type names — the ones to reach for in everyday code:

```text
int float str bool char void
```

`int` is a 64-bit signed integer, `float` a 64-bit float, `str`/`String` the
string type, `bool` a boolean, `char` a single character, and `void` the
absence of a value. `str` and `String` are interchangeable.

For code that needs an exact width (binary formats, FFI, SIMD lanes), the
fixed-width types are also available:

```text
i8 i16 i32 i64
u8 u16 u32 u64
f32 f64
Allocator Json Pointer
```

### 4.2 Composite type syntax

```a
[T]              # array
T?               # optional
(T, U)           # tuple
{K: V}           # map type
{T}              # set type
fn(T, U) -> R    # function type
T[N]             # fixed-size array type syntax (parsed)
module.Type      # qualified type name
Foo<T>           # generic type arguments (parsed/resolved where supported)
```

Examples:

```a
fn sum(xs: [i64]) -> i64:
    total = 0
    for x in xs:
        total += x
    return total

fn lookup(m: {String: i64}, key: String) -> i64?:
    return m.get(key)
```

### 4.3 Type aliases

```a
type UserId = i64
type Name = String
```

## 5. Statements

### 5.1 Assignment and rebinding

Plain assignment creates or rebinds a variable in the current dynamic/runtime model:

```a
x = 1
x = x + 1
name = "A"
```

Typed declaration form is also parsed/supported:

```a
count: i64 = 0
title: String = "hello"
```

Compound assignments:

```a
x += 1
x -= 1
x *= 2
x /= 2
x %= 3
x &= mask
x |= bits
x ^= bits
x <<= 1
x >>= 1
```

### 5.2 Destructuring assignment

Tuple destructuring is supported (parser/type checker/runtime support; integration-tested patterns):

```a
pair = (1, 2)
a, b = pair
print(a, b)
```

Use `_` to ignore a value where supported by patterns/destructuring contexts.

### 5.3 Expression statements

Any expression can be used as a statement:

```a
print("hi")
p.sum()
```

### 5.4 `if` / `elif` / `else`

```a
if n < 0:
    print("negative")
elif n == 0:
    print("zero")
else:
    print("positive")
```

### 5.5 `while`

```a
i = 0
while i < 3:
    print(i)
    i += 1
```

### 5.6 `do ... while` (parsed)

The parser supports `do`/`while` syntax; use integration tests/code behavior as the source of truth for exact runtime semantics in the current backend.

### 5.7 `for ... in`

```a
for n in [1, 2, 3]:
    print(n)

for i in 0..5:
    print(i)
```

`for` iterates over iterable/runtime-supported values (arrays, ranges, maps/sets/strings and other runtime iterables where supported).

### 5.8 `loop`

```a
i = 0
loop:
    if i >= 3:
        break
    print(i)
    i += 1
```

`break` and `continue` are valid inside loops.

### 5.9 `return`

```a
fn add(a, b):
    return a + b
```

### 5.10 `defer`

`defer` schedules a statement/body to run when leaving the current scope/function flow (LIFO behavior is implemented; see tests).

```a
fn demo():
    defer print("cleanup")
    print("work")
```

### 5.11 `import` and `from ... import`

```a
import math
import "lib/util.a"
import "lib/util.a" as util
from "lib/util.a" import add, Point
import python:json
from python:math import sqrt, pi
```

- Bare imports like `import math` resolve to native modules or stdlib modules as configured by the module loader.
- Quoted imports load `.a` modules from filesystem/module paths.
- `import python:<pkg>` / `from python:<pkg> import name` embed CPython (Mojo-style interop). Requires a build with Python 3 dev headers; runs in the interpreter (`a run`/`a test`/`a repl`/`a -c`), not the LLVM/native backends. Scalars, strings, arrays, maps, sets, tuples, optionals, results, classes, and enum variants convert automatically; other objects (modules, functions, class instances) stay live Python objects supporting `.attr`, `obj[key]`, calls, `==`, and A's operators (`+ - * / % **`, comparisons, `in` — forwarded to CPython's `PyNumber_*`/rich-compare/`sq_contains`).
- The interpreter is discovered at runtime and `dlopen`ed — no libpython is linked into `a`. Discovery order: `A_PYTHON`/`A_PYTHON_LIB`, local `.venv`/`venv`, `VIRTUAL_ENV`/`UV_PROJECT_ENVIRONMENT`/`CONDA_PREFIX`, `python3`/`python` on `PATH`, uv-managed installs, system libraries; the environment containing the imported module is preferred. The chosen interpreter's `site-packages` joins `sys.path`, `A_PYTHONPATH` (colon-separated) adds more, and `-DPython3_EXECUTABLE` at configure time only selects the headers used to compile the interop layer.
- Imports are only allowed at module top level.

## 6. Functions

### 6.1 Declarations

```a
fn add(a, b):
    return a + b

fn greet(name: String) -> String:
    return "Hello, {name}"
```

Features supported by the parser/AST/type checker (with varying levels of specialization/enforcement):

- parameters with optional type annotations
- return type annotations (`-> T`)
- generic type parameters: `fn id[T](x: T) -> T`
- `comptime` parameters: `fn f(comptime N: int, ...)`
- `where` clauses / trait constraints: `fn same(a: T, b: T) -> bool where Equatable`

### 6.2 Lambdas

```a
double = fn(n) => n * 2
add = fn(a, b) => a + b
print((fn() => 42)())
```

Lambdas are first-class values. Capture behavior is implemented by the runtime/backend (integration tests demonstrate snapshot/value capture semantics and shared reference behavior for captured objects).

### 6.3 Calls

```a
add(1, 2)
math.sqrt(9)
point.sum()
```

The language supports positional arguments and named-argument syntax in call parsing/AST (exact runtime/type behavior depends on callee).

## 7. Expressions and precedence

Expressions are parsed with precedence/associativity rules (see `Parser::binaryPrecedence`).

### 7.1 Primary expressions

- literals (`int`, `float`, `bool`, `char`, `string`)
- identifiers and `self`
- `None`
- lambdas (`fn(...) => expr`)
- `match` expressions
- parenthesized expressions / tuples
- array literals and comprehensions
- map/set literals and set comprehensions

### 7.2 Postfix / access / call expressions

```a
obj.field
obj?.field        # optional chaining / safe navigation
arr[i]            # indexing
arr[start..end]   # slicing (and stepped forms where parsed/supported)
f(x, y)           # call
value!            # unwrap/propagation-related postfix operator in AST/runtime
value?            # try/propagation operator parsed in postfix contexts
x++               # postfix increment (parsed as unary postfix)
x--               # postfix decrement
```

> Note: Some operators/tokens exist in the AST/lexer/parser for compatibility or evolving semantics. Use current tests and compiler behavior for edge-case semantics.

### 7.3 Prefix/unary operators

```a
-x
not cond
~bits
++x
--x
```

### 7.4 Arithmetic and bitwise

```a
a + b  a - b  a * b  a / b  a % b  a ** b
a & b  a | b  a ^ b  a << n  a >> n
```

### 7.5 Comparisons and logic

```a
a == b  a != b  a < b  a <= b  a > b  a >= b
x in xs
x not in xs
a and b
a or b
not a
```

### 7.6 Ranges

```a
0..10
1..10..2      # stepped range forms are parsed/supported in tested cases
```

Ranges are commonly used with `for` and some slicing/comprehension forms.

### 7.7 Ternary / conditional expression

```a
value = cond ? thenValue : elseValue
```

### 7.8 Null-coalescing

```a
value = maybeValue ?? fallback
```

### 7.9 Pipeline operator

The right side of `|>` must be a call to a plain function. The left value becomes the first argument.

```a
print(5 |> square())
value = 3 |> square() |> add(1)
print(items |> join("-"))
```

### 7.10 Comprehensions

Array comprehension:

```a
doubled = [n * 2 for n in 0..5]
evens = [n for n in 0..10 if n % 2 == 0]
```

Set comprehension:

```a
nums = {n * 2 for n in 0..4}
```

## 8. Pattern matching (`match`)

`match` is an expression with pattern arms:

```a
label = match code:
    200 => "OK"
    400..499 => "Client error"
    _ if code < 0 => "negative"
    _ => "other"
```

Supported pattern forms (parser/AST):

- wildcard: `_`
- literal patterns (int/float/string/char/bool, incl. unary numeric literals where supported)
- range patterns: `start..end` (inclusive, per tests)
- binding patterns: `name`
- tuple patterns: `(a, b)`
- variant/qualified patterns: `Enum.Variant(...)` / dotted constructor paths
- enum tag/qualified literal patterns
- or-patterns: `a | b`

Arms use `=>` and may include guards with `if`.

## 9. Classes, methods, and inheritance syntax

### 9.1 Classes

`class` and `metaclass` declarations define class-like types.

```a
class Point:
    x
    y

    fn sum(self):
        return self.x + self.y

p = Point(3, 4)
print(p.sum())
```

Fields may be declared with or without annotations; methods are declared with `fn` inside the body.

### 9.2 Construction and field access

```a
p = Point(3, 4)
named = Point(y=10, x=20)
print(p.x, named.y)
```

### 9.3 Inheritance / metaclass syntax

The parser accepts base-class and metaclass forms in declarations, e.g.:

```a
class Animal:
    fn speak(self):
        return "..."

class Dog(Animal):
    fn speak(self):
        return "woof"
```

(Use current tests/implementation behavior for full inheritance dispatch semantics.)

## 10. Enums

### 10.1 Simple (integer-valued) enums

```a
enum Color:
    Red
    Green = 5
    Blue

print(Color.Red)
print(Color.Green)
```

Variants may have explicit integer values.

### 10.2 Algebraic enums / payload variants

The parser/AST/type system support algebraic variants with payload fields, and variant patterns in `match`.

Conceptual form:

```a
enum Shape:
    Circle(r: f64)
    Rect(w: f64, h: f64)
```

Pattern matching uses qualified variant patterns:

```a
match s:
    Shape.Circle(r) => r
    _ => 0
```

## 11. Traits, impls, generics, and comptime

### 11.1 Traits

```a
trait Equatable:
    fn eq(self, other) -> bool
```

### 11.2 Implementations

```a
impl Equatable for Point:
    pass
```

The checker validates trait/impl conformance where implemented.

### 11.3 `where` clauses

```a
fn same(a: T, b: T) -> bool where Equatable:
    return a.eq(b)
```

### 11.4 Generics

Functions, classes, and type positions support generic parameter syntax in the parser/type checker:

```a
fn identity[T](x: T) -> T:
    return x
```

### 11.5 `comptime` parameters

`comptime` parameters are parsed and represented in the AST/type checker; specialization behavior may be partial depending on backend/path.

```a
fn repeat(comptime N: int, x):
    return x
```

## 12. Optionals and results

### 12.1 Optionals

- `T?` is optional type syntax.
- `None` represents the empty optional.
- `Some(value)` wraps a value.
- `??` provides a fallback.
- `?.` safely navigates fields through optionals.
- `!` is used for unwrap-style operations (and `?` appears as propagation/try-related syntax in the AST/parser; see tests for exact behavior).

```a
missing = None
present = Some(41)
print(missing ?? 0)
print(present!)
print(maybeUser?.address?.city)
```

### 12.2 Results

`Ok(value)` and `Err(error)` create result values; result helpers and pattern/API behavior are available through runtime/builtins and prelude support.

```a
fn divide(a, b):
    if b == 0:
        return Err("division by zero")
    return Ok(a / b)
```

## 13. Modules and packages

### 13.1 Module resolution

- `import math` / `import core` resolve to native modules (backed by C++ runtime bindings).
- quoted imports resolve to `.a` files (relative/module paths and stdlib resolution rules apply).
- the module loader auto-loads the prelude before the entry program when a stdlib root is found (`stdlib/prelude.a` / `std/prelude` variants).

### 13.2 Native modules

Native modules are implemented by the toolchain and exposed as module namespaces (see [Standard Library API Reference](../standard-library/README.md)).

### 13.3 `std/*` packages

The repository includes `stdlib/std/...` packages. Some are pure A wrappers; others are package stubs backed by native modules (`core`, `fs`, `json`, `http`, `net`, `time`, `testing`, `log`, `metrics`, `trace`, `binary`).

## 14. Runtime, memory, and unsafe features

- A uses runtime-managed values/objects in the interpreter/KGEN execution engine path.
- Unsafe and arena APIs exist for low-level/experimental memory operations.
- Ownership/move/borrow-related syntax and builtins appear in the language/tooling; use current implementation/tests for exact guarantees, especially on the LLVM/KGEN native path.

## 15. Testing

Functions named `test_*` at top level are discovered/executed by `a test`.

```a
fn test_addition():
    assert_eq(1 + 1, 2)
```

See [CLI Reference](../cli/README.md) and [Standard Library API Reference](../standard-library/README.md) for `assert`/`assert_eq`/`testing` APIs.
