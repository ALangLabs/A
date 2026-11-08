# Syntax and types

Verified against `compiler/astra/frontend/lexer/` (keyword table, literal
scanners) and `examples/`/`tests/`.

## Lexical structure

- **Comments**: `#` line comments; `##` doc comments attach to declarations.
- **Docstrings**: `"""..."""` raw string statement at the top of a body.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`; keywords are lowercase except
  `None`, `Some`/`Ok`/`Err` (capitalized builtins).
- **Indentation**: significant — blocks are introduced by `:` then an indented
  suite; `Indent`/`Dedent` are real tokens. Brackets `()[]{}` suppress
  newlines inside expressions.
- **Keywords**: `fn if elif else while for in loop break continue return defer
  class metaclass pass self true false and or not import from
  as match None enum do type trait where comptime impl`.
  There is **no** `let`, `var`, or `struct` keyword — bindings are plain
  assignments (`x = 1`, `x: i64 = 1`) and types use `class`.

## Literals

| Literal | Example |
| --- | --- |
| Integer | `42`, `0xFF`, `0b1010`, `0o17`, underscores `1_000` |
| Float | `3.14`, `1e3` |
| Bool | `true`, `false` |
| Char | `'A'` |
| String | `"text"`, escapes `\n \t \" \\`, interpolation `{expr}` |
| Raw string | `"""..."""` triple-quoted (no escapes/interpolation) |
| None | `None` |

Strings interpolate with `{expr}`: `"sum = {2 + 3}"`, `"Hello, {name}!"`.

## Operators

- Arithmetic: `+ - * / % **` (power)
- Comparison: `== != < <= > >=`
- Logic: `and or not` (words, not `&&`/`||`/`!`-as-logic — `!` is unwrap/`!=`)
- Bitwise: `& | ^ ~ << >>`
- Unary: `-`, `not`, `!` (postfix unwrap), `++`/`--` (pre/post)
- Compound assignment: `+= -= *= /= %= &= |= ^= <<= >>=`
- Other: `..` range (`0..5`, `0..10..2` stepped), ternary `cond ? a : b`,
  coalesce `a ?? b`, safe-nav `?.`, call/index `()[]`, slice `[lo..hi..step]`,
  try `expr?`

## Built-in types

| Type | Notes |
| --- | --- |
| `bool`, `char`, `String`/`str` | `char` is a Unicode code point; strings are UTF-8 with methods (`strip`, `upper`, `split`, `contains`, `startswith`, `endswith`, `join`, `lower`, …) |
| `int`/`i64`, `i8 i16 i32`, `u8 u16 u32 u64`, `float`/`f64`, `f32` | sized ints/floats; literals default to `i64`/`f64` |
| `[T]` / `[T; N]` | dynamic and fixed-size arrays |
| `(a, b, …)` | tuples |
| `map<K, V>`, `set<T>` | dictionaries and sets |
| `T?` | optional (`None` or `Some(v)`) |
| `Result<T, E>` | via `std:prelude` (`Ok`/`Err`) |
| `range` | produced by `a..b` literals; iterable in `for` |
| `fn(params) -> ret` | function values / lambdas |
| `Pointer<T>`, `Allocator`, `Json`, `IO` | low-level / interop types |
| user types | `class`, `enum`, `trait` declarations |

See [types in declarations](declarations.md) and
[expressions](statements-and-expressions.md).
