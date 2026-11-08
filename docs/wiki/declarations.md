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
- Generic type params: `fn id[T](x: T) -> T`; `comptime` params:
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
- Docstrings: a `"""..."""` string-literal statement at the top of a body
  (triple-quoted, raw — no escapes or interpolation). `##` comments are doc
  comments attached to declarations, not docstrings.

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

`comptime` params exist in the grammar (`comptime N: int`); specialization
coverage varies by backend (see `examples/advanced/`).
