# `std.textwrap`

> Text wrapping, indentation, and truncation helpers inspired by CPython's [`textwrap`](https://docs.python.org/3/library/textwrap.html) module.

`std.textwrap` is a **pure-A, practical subset** for formatting CLI output, logs, help text, and generated messages. It uses simple whitespace-aware wrapping and explicit parameters (compatible with A's current style).

> [!NOTE]
> This is **not a drop-in CPython module**. It intentionally omits regex-heavy `TextWrapper` behavior (hyphen breaking, sentence-end fixing, full tab handling options) in favor of predictable, portable behavior.

## Status

| Field | Value |
|---|---|
| Kind | Pure A stdlib package |
| Source | `stdlib/std/textwrap/__init__.a` |
| Import names | `textwrap` (explicit path import recommended for source layouts) |
| API style | Python-inspired subset |
| Wrapping strategy | Whitespace-based (simple, deterministic) |

## Import

```a
from "std/textwrap/__init__.a" import wrap, fill, dedent, indent, shorten
```

```a
import textwrap
print(textwrap.fill("hello world", 8))
```

## API reference

### `wrap(text, width) -> [String]`

Wrap `text` to `width` columns and return a list of lines.

- Collapses/splits on whitespace in a simple way.
- Very long words may be split to satisfy `width`.
- `width < 1` is treated as `1`.

### `fill(text, width) -> String`

Wrap `text` and join the resulting lines with `\n`.

### `dedent(text) -> String`

Remove common leading indentation from all non-blank lines.

- Preserves existing line endings where possible (`\n`, `\r\n`, `\r`).
- Blank/whitespace-only lines do not control the common margin.

### `indent(text, prefix) -> String`

Add `prefix` to the beginning of selected non-empty lines.

- Whitespace-only/blank lines are left unchanged (matching Python's practical intent for common cases).

### `shorten(text, width, placeholder) -> String`

Collapse whitespace and truncate to fit `width`.

- Appends `placeholder` when truncation is needed.
- All arguments are required in this A subset (no default parameters).

## Examples

### Wrap and fill terminal text

```a
from "std/textwrap/__init__.a" import wrap, fill

text = "This is a simple sentence that should wrap nicely for testing."
print(wrap(text, 20))
print(fill(text, 20))
```

### Format multi-line strings with `dedent` + `indent`

```a
from "std/textwrap/__init__.a" import dedent, indent

msg = dedent("    hello\n    world\n  end")
print(msg)
print(indent("a\n\nb", "> "))
```

### Create compact previews

```a
from "std/textwrap/__init__.a" import shorten

print(shorten("hello world this is long", 12, " [...]"))
```

## Differences from CPython

- No `TextWrapper` class and no regex-based word splitting.
- Function signatures are explicit (for example `shorten(text, width, placeholder)` requires all args).
- Advanced options (`max_lines`, `fix_sentence_endings`, `break_on_hyphens`, tab expansion controls) are limited/omitted.

## Validation

```bash
build/a run tests/integration/cases/stdlib/pytextwrap_basic.a
```

## See also

- [`docs/standard-library/README.md`](../../../docs/standard-library/README.md)
- [`stdlib/std/README.md`](../README.md)
- [CPython `textwrap` docs](https://docs.python.org/3/library/textwrap.html)
