# A — Keywords

Keywords in this list are *reserved words* in A: they can never be used as
identifiers. The authoritative list is the lexer
(`compiler/astra/frontend/lexer/token.hpp`, the `Kw*` tokens); the type checker,
the language server, and this list are kept in sync from it.

| Keyword | Kind | Role |
|---|---|---|
| `fn` | declaration | function/method declaration or lambda |
| `class`, `metaclass` | declaration | class / metaclass declaration |
| `enum` | declaration | enum declaration |
| `trait` | declaration | trait declaration |
| `impl` | declaration | implementation of a trait for a type |
| `type` | declaration | type alias |
| `comptime` | declaration | compile-time parameter modifier |
| `import`, `from`, `as` | imports | import statements |
| `if`, `elif`, `else` | control flow | conditional statements |
| `while`, `do`, `loop` | control flow | loops |
| `for`, `in` | control flow | iteration |
| `break`, `continue` | control flow | loop control |
| `return` | control flow | function return |
| `defer` | control flow | run when leaving a scope |
| `match` | control flow | pattern matching |
| `pass` | control flow | no-op placeholder |
| `where` | control flow / generics | constraint clause |
| `and`, `or`, `not` | operators | boolean logic |
| `self` | misc | current instance inside a method |
| `true`, `false` | literals | boolean literals |
| `None` | literals | empty optional |

Python equivalents and notes are given per keyword below. (Uppercase
`Class`/`Metaclass` are also accepted as keyword forms for `class` /
`metaclass`.)

---

## `fn` — function, method, or lambda

Declares a function or a method, provides the lambda introducer, and is
A's only callable-introduction keyword (Python's `def`).

```a
fn add(a, b):
    return a + b

class Point:
    x
    y

    fn sum(self):
        return self.x + self.y

double = fn(n) => n * 2
```

Parameters may be typed (`name: String`) and the return may be typed
(`-> String`); generic functions use `fn name<T>(x: T) -> T`.

## `class` / `metaclass` — class declarations

Declares a class (Python's `class`). `metaclass` declares a class whose
instances are classes and supports overriding class-level behavior
(Python's `metaclass`).

```a
class Point:
    x
    y

    fn sum(self):
        return self.x + self.y

class Dog(Animal):
    pass
```

Fields are written as bare names or `name: Type`; methods with `fn`
inside the body. Instances are constructed by calling the class name:
`Point(3, 4)`.

## `enum`

Declares an enumeration (Python's `enum` module, not a keyword there).

```a
enum Color:
    Red
    Green = 5
    Blue

print(Color.Red)
```

## `trait`

Declares a trait (like Rust's `trait`) — a bundle of method signatures.

```a
trait Equatable:
    fn eq(self, other) -> bool
```

## `impl`

Declares an implementation of a trait (or a default impl block) for a
type, e.g. `impl Equatable for Point:`.

## `type`

Declares a type alias.

```a
type UserId = i64
```

## `comptime`

Marks a parameter evaluated at compile time.

```a
fn repeat(comptime N: int, x):
    return x
```

## `import`, `from`, `as`

Import statements (Python's `import` / `from ... import` / `import ... as`).

```a
import std:math
import "lib/util.a" as util
from "lib/util.a" import add, Point
import python:json
from python:math import sqrt, pi
```

## `if`, `elif`, `else`

Conditional blocks (Python's `if` / `elif` / `else`).

```a
if n < 0:
    print("negative")
elif n == 0:
    print("zero")
else:
    print("positive")
```

## `while`

Loop while a condition holds (Python's `while`).

```a
i = 0
while i < 3:
    print(i)
    i += 1
```

## `do` — do…while loop

Post-condition loop: `do` introduces the body, `while` closes it with
the condition.

## `loop`

Infinite loop terminated by `break` (Rust's `loop`).

```a
loop:
    print("hello")
    break
```

## `for`, `in`

Iterates over arrays, strings, maps/sets (keys), and ranges
(Python's `for ... in ...`).

```a
for n in [1, 2, 3]:
    print(n)

for i in 0..5:
    print(i)
```

`in` also works as an operator in expressions (`x in xs`).

## `break`, `continue`

Loop control, as in Python.

## `return`

Exits the current function with a value (Python's `return`). Bare
`return` (or reaching the end) yields `void`/`None`.

## `defer`

Schedules a statement to run when leaving the current scope/function,
in LIFO order (Go's `defer`).

```a
fn demo():
    defer print("cleanup")
    print("work")
```

## `match`

Pattern-matching expression (Python's `match`).

```a
label = match code:
    200 => "OK"
    400..499 => "Client error"
    _ => "other"
```

## `pass`

A no-op statement; used as the sole body of a block (Python's `pass`).

## `where`

Adds a constraint clause to a generic declaration (Rust's `where`).

```a
fn same(a: Point, b: Point) -> bool where Equatable:
    return a.eq(b)
```

## `and`, `or`, `not`

Boolean operators (Python's `and` / `or` / `not`).

## `self`

The current instance inside a method (Python's `self`).

## `true`, `false`, `None`

Literals: the two booleans and the empty optional. `None` is also the
initial value of optionals; `Some(value)` wraps a value.

---

## Not keywords (common confusion)

These are *not* reserved in A and can be used as identifiers:

| Name | Why people think so | Reality |
|---|---|---|
| `struct` | Rust/C-style keyword | does not exist; use `class` |
| `async`, `await` | Python keywords | A has no async (identifiers) |
| `var`, `const`, `let` | other languages | plain `x = 1` assignment; `const` is an identifier |
| `yield` | Python | not a keyword; use arrays for computed sequences |
| `try`/`except`/`raise` | Python | use `Result`/`Ok`/`Err` or `panic` |
| `True`/`False` | Python literals | A uses lowercase `true`/`false` |

> **Python's `keyword.kwlist` vs A's:** `docs/../stdlib/std/keyword/__init__.a`
> ships Python's keyword list verbatim (`python_kwlist`); it is not A's
> list. A's reserved words are the 33 above.
