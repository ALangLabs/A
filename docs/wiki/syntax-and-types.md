# Syntax and types

Verified against `compiler/astra/frontend/lexer/` (keyword table, literal
scanners) and `examples/`/`tests/`.

## Lexical structure

- **Comments**: `#` line comments (including `##`, which is just a comment).
- **Docstrings**: `"""..."""` raw string statements document declarations —
  the first statement in a body, or directly above a declaration/module.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`; keywords are lowercase except
  `None`, `Some`/`Ok`/`Err` (capitalized builtins).
- **Indentation**: significant — blocks are introduced by `:` then an indented
  suite; `Indent`/`Dedent` are real tokens. Brackets `()[]{}` suppress
  newlines inside expressions.
- A UTF-8 BOM at the start of a file is accepted and ignored.
- **Keywords**: `fn if elif else while for in loop break continue return defer
  class metaclass pass self true false and or not import from
  as match None enum do type trait where comptime impl is del`.
  There is **no** `let`, `var`, or `struct` keyword — bindings are plain
  assignments (`x = 1`, `x: i64 = 1`) and types use `class`.

## Literals

| Literal | Example |
| --- | --- |
| Integer | `42`, `0xFF`, `0b1010`, `0o17`, underscores `1_000` |
| Float | `3.14`, `1e3` |
| Bool | `true`, `false` (lowercase keywords; `True`/`False` are not boolean literals) |
| Char | `'A'` (ASCII only — non-ASCII char literals are rejected; use a string for Unicode text) |
| String | `"text"`, escapes `\n \t \" \\`, interpolation `{expr}` |
| Single-quoted string | `'text'` — multi-char strings work like Python `'...'`: escapes decode but `{expr}` is **never** interpolated (handy for `str.format`/`%` templates) |
| f-string | `f"text {expr}"` — prefix selects the normal interpolating string |
| Raw string | `"""..."""` triple-quoted (no escapes/interpolation) |
| None | `None` |

Double-quoted strings interpolate with `{expr}`: `"sum = {2 + 3}"`,
`"Hello, {name}!"`. Write `{{`/`}}` for literal braces: `"{{key}}"` →
`{key}` (useful for JSON/`str.format` text inside interpolating strings).

## Operators

- Arithmetic: `+ - * / // % **` (power; `//` is floor division — `-7 // 2 == -4`)
- Comparison: `== != < <= > >=`, plus `is` / `is not` (equality semantics)
- Chained comparison: `a < b < c` means `a < b and b < c` (the middle operand
  must be a name or literal so it isn't evaluated twice)
- Logic: `and or not` (words, not `&&`/`||`/`!`-as-logic — `!` is unwrap/`!=`)
- Bitwise: `& | ^ ~ << >>`
- Unary: `-`, `not`, `!` (postfix unwrap), `++`/`--` (pre/post)
- Compound assignment: `+= -= *= /= //= %= &= |= ^= <<= >>=`, and chained
  `x = y = z = v` (reference values share, like Python)
- Other: `..` range (`0..5`, `0..10..2` stepped), ternary `cond ? a : b`,
  inline-if `x if c else y`, coalesce `a ?? b`, walrus `x := expr`
  (statement level), safe-nav `?.`, call/index `()[]`, slice `[lo..hi..step]`,
  try `expr?`, statement separator `;` (`x = 1; y = 2`)
- Repetition: `"ab" * 3`, `[0] * 4`, `(1, 2) * 3` repeat strings/lists/tuples
- Formatting: `"%d %s" % (a, b)` printf-style, `"{} {0}".format(a, b)`
  (`'...'` literals avoid `{}` interpolation for format strings)

Note: `%` on strings formats (`"%05.1f" % 3.14`); on numbers it follows C
remainder semantics for integers (`-7 % 3 == -1`) but Python-style floored
semantics for floats (`-7.0 % 3.0 == 2.0`).

## Built-in types

| Type | Notes |
| --- | --- |
| `bool`, `char`, `String`/`str` | `char` is a single ASCII character; strings are UTF-8 with methods (`strip`, `upper`, `split`, `contains`, `startswith`, `endswith`, `join`, `lower`, …) |
| `int`, `float` | canonical numeric types — `int` is a signed 64-bit integer, `float` a 64-bit double; literals and arithmetic default to them |
| `i8 i16 i32 i64`, `u8 u16 u32 u64`, `f32` | explicit-width types for low-level/FFI code; `i64`/`f64` spell the same types as `int`/`float` |
| `[T]` / `T[N]` | dynamic arrays and fixed-size arrays — fixed sizes use a C-like suffix (`int[3]`, `i32[2][3]` for two rows of three) |
| `(a, b, …)` | tuples |
| `{K: V}`, `set<T>`/`{T}` | dictionaries (`{"k": v}`) and sets (`{v}`) |
| `T?` | optional (`None` or `Some(v)`) |
| `T \| U` | union — accepts either side (see below) |
| `any` / `Any` | dynamic top type — accepts and is accepted by every type |
| `list[T]`, `dict[K, V]`, `set[T]`, `tuple[T1, T2]` | Python-typing spellings of `[T]`, `{K: V}`, `{T}`, `(T1, T2)` — also `List`/`Dict`/`Set`/`Tuple`/`FrozenSet`/`Sequence`/`Mapping` |
| `tuple[T, ...]` | homogeneous variable-length tuple |
| `Optional[T]`, `Union[T, U]`, `T \| None` | `Optional[T]` ≡ `T?`; `T \| None` ≡ `T?`; `Union[...]` ≡ `\|` |
| `fn(params) -> ret` | function values / lambdas (also usable as a parameter/return/field type) |
| `Result<T, E>` | builtin sum type produced by `Ok(v)` / `Err(e)` (no import needed); `expr?` unwraps `Ok` and propagates `Err`. Note `std:prelude` defines a *separate* plain `Result` class with its own `Ok`/`Err`/`is_ok`/`unwrap` helpers that do not interoperate with `?` |
| `range` | produced by `a..b` literals; iterable in `for` (not nameable in annotations) |
| `Pointer<T>`, `Allocator`, `Json`, `IO` | low-level / interop types |
| user types | `class`, `enum`, `trait` declarations |

### Python-`typing` compatibility

Annotations accept Python-`typing` spellings without any import:

```python
fn first(xs: list[int]) -> int: return xs[0]
fn lookup(m: dict[str, int]) -> int: return m["k"]
fn either(x: int | str) -> str: return "ok"
fn maybe(x: int | None) -> int: return x ?? 0   # == int?
fn cb(f: fn(int) -> int) -> int: return f(1)
fn anything(x: any) -> any: return x
type IntList = list[int]                        # type alias
v = "5" as int                                  # == cast<int>("5"); runtime no-op
y = cast<list[int]>([1, 2])
```

`Name[T, ...]` square brackets work anywhere `<T>` generic arguments do
(`Box[int]` ≡ `Box<i64>`-style explicit args, `vector[float, 4]`). Bare
container names (`list`, `dict`, `set`, `tuple`) annotate the container kind
with `any` elements. `Literal[...]`, `Callable[...]`, `Type[T]`, and
`issubclass` are not supported (`Callable` suggests `fn(T) -> R`; `isinstance`
exists and takes a runtime type-name string).

Union/any semantics: `T | U` constrains assignments, arguments, and return
types (a `list[int]` parameter rejects `list[str]`, an `int | str` parameter
accepts either). *Uses* of union/any-typed values — method calls, field
access, indexing, arithmetic — are checked dynamically at runtime, like
untyped values. A union-typed value assigned to a concrete type must have
every member fit; assigning to a union target needs only one member to fit.

See [types in declarations](declarations.md) and
[expressions](statements-and-expressions.md).
