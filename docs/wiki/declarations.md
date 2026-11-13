# Declarations

Top-level and member declarations, verified against
`compiler/astra/frontend/parser/parser_decl.cpp`, `ast/stmts.hpp`, and
`examples/`.

## Functions

```python
fn add(a, b):
    return a + b

fn greet(name: String) -> String:
    return "Hello, {name}"
```

- Optional parameter/return type annotations; types may be omitted.
- Default values on trailing params: `fn f(x = 1)`; variadic `*args` collects
  extras into a list.
- Generic type params: `fn id<T>(x: T) -> T`; generic bounds use
  `<T: Trait>`. `comptime` params:
  `fn f(comptime N: int)`; constraints via `where`:
  `fn same(a: Point, b: Point) -> bool where Equatable:`.
- Lambdas: `double = fn(n) => n * 2`; `(fn(x, y) => x + y)(20, 22)`.

## Classes

```python
class Point:
    x: i64
    y: i64

    fn length(self) -> float:
        ...

p = Point(3, 4)          # positional or named: Point(x=1, y=2)
```

- Fields are bare names or `name: Type`; methods take `self`.
- Inheritance: `class Dog(Animal):`; metaclass: `class C(metaclass=M):`
  (also `class C(Base, metaclass=M)`).
- Docstrings: a `"""..."""` string-literal statement documents a declaration —
  the first statement inside a body, or directly above the declaration
  (triple-quoted, raw — no escapes or interpolation). `#`/`##` are plain
  comments, not documentation.

## Enums

```python
enum Color:
    Red
    Green = 5      # explicit values allowed; later variants auto-increment
    Blue
```

Access: `Color.Red`. Algebraic variants carry fields (payload enums); `match`
handles them.

## Traits and impls

```python
trait Equatable:
    fn eq(self, other) -> bool

class Point: ...
impl Equatable for Point:
    pass
```

Traits support inheritance (`trait Hashable(Equatable)`) and `where` lists.
`impl` verifies the type provides every signature.

## Type aliases and comptime

```python
type UserId = i64
id: UserId = 7
```

`comptime` params (`fn f(comptime N: int, x: int)`) are specialized at each
call site: `f(4, x)` clones the function, removes the comptime parameter from
the signature, and substitutes `N` with the literal `4` throughout the body
before type checking, so every specialization is zero-cost ordinary code. The
argument must be a compile-time constant (literals, constant arithmetic,
`len`/`range`/`str`/`int`/`float`/`bool`/`abs` on constants). Comptime params
may sit anywhere in the parameter list; all call arguments must be
positional. Inside a body, `comptime if COND:` / `comptime for i in xs:` are
evaluated at compile time — only the taken branch is kept and `comptime for`
unrolls — when the condition/iterable is a compile-time constant (including
substituted comptime params); otherwise they degrade to ordinary runtime
`if`/`for`. Comptime params on methods, comptime types, and `reflect` are not
implemented yet. Constraints via `where` do work (see above).
