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
| `map<K, V>`, `set<T>` | dictionaries (`{"k": v}`) and sets (`{v}`); inferred only — `map`/`set` are not nameable in type annotations |
| `T?` | optional (`None` or `Some(v)`) |
| `Result<T, E>` | builtin sum type produced by `Ok(v)` / `Err(e)` (no import needed); `expr?` unwraps `Ok` and propagates `Err`. Note `std:prelude` defines a *separate* plain `Result` class with its own `Ok`/`Err`/`is_ok`/`unwrap` helpers that do not interoperate with `?` |
| `range` | produced by `a..b` literals; iterable in `for` (not nameable in annotations) |
| `fn(params) -> ret` | function values / lambdas |
| `Pointer<T>`, `Allocator`, `Json`, `IO` | low-level / interop types |
| user types | `class`, `enum`, `trait` declarations |

See [types in declarations](declarations.md) and
[expressions](statements-and-expressions.md).
