# `std.string`

> ASCII character constants and `capwords`, inspired by CPython's [`string`](https://docs.python.org/3/library/string.html) module.

This package provides a **pure-A, practical subset** of Python's `string` module: portable ASCII character sets and a simple word-capitalization helper. It is designed for text utilities, validators, and formatting helpers written in A.

> [!NOTE]
> This is **not a drop-in CPython module**. It follows Python naming/behavior where feasible, but it is adapted to A's semantics and runtime.

## Status

| Field | Value |
|---|---|
| Kind | Pure A stdlib package |
| Source | `stdlib/std/string/__init__.a` |
| Import names | `string` (explicit path import recommended for source layouts) |
| API style | Python-inspired subset |
| CPython coverage | Constants + `capwords` (no `Formatter`/`Template`) |

## When to use it

Use `std.string` when you need:
- well-known ASCII character groups (`digits`, `ascii_letters`, `punctuation`, etc.)
- simple title-like word capitalization for CLI/output text
- portable constants instead of retyping character literals

## Import

```a
# Explicit path import (recommended in source-tree workflows)
from "std/string/__init__.a" import ascii_letters, digits, punctuation, capwords
```

```a
# Module-style import (when your environment resolves bare stdlib module names to this package)
import string
print(string.ascii_lowercase)
```

## API reference

### Constants

| Name | Description |
|---|---|
| `whitespace` | ASCII whitespace characters: space, tab, newline, carriage return. |
| `ascii_lowercase` | `abcdefghijklmnopqrstuvwxyz` |
| `ascii_uppercase` | `ABCDEFGHIJKLMNOPQRSTUVWXYZ` |
| `ascii_letters` | `ascii_lowercase + ascii_uppercase` |
| `digits` | `0123456789` |
| `hexdigits` | `0123456789abcdefABCDEF` |
| `octdigits` | `01234567` |
| `punctuation` | ASCII punctuation characters. |
| `printable` | Digits + ASCII letters + punctuation + whitespace. |

### Functions

#### `capwords(s, sep) -> String`

Capitalize words using a split/capitalize/join strategy.

- `s`: input text (converted to string)
- `sep`: separator to split on. Pass `None` for whitespace behavior (split on whitespace runs, collapse separators, join with a single space). Pass a string separator (for example `"-"`) to split/join on that exact separator.

```a
from "std/string/__init__.a" import capwords

print(capwords("  hello   world  ", None))   # Hello World
print(capwords("a-b-c", "-"))                # A-B-C
print(capwords("ALREADY MIXED case", None))  # Already Mixed Case
```

## Examples

### Validate a simple identifier prefix

```a
from "std/string/__init__.a" import ascii_letters, digits

fn is_simple_ident(s):
    if len(s) == 0:
        return false
    first = str(s[0])
    if not ascii_letters.contains(first) and first != "_":
        return false
    i = 1
    while i < len(s):
        ch = str(s[i])
        if not ascii_letters.contains(ch) and not digits.contains(ch) and ch != "_":
            return false
        i = i + 1
    return true

print(is_simple_ident("hello_world2"))
print(is_simple_ident("2bad"))
```

### Format labels from separators

```a
from "std/string/__init__.a" import capwords

labels = ["user-name", "created-at", "is-active"]
i = 0
while i < len(labels):
    print(capwords(labels[i], "-").replace("-", " "))
    i = i + 1
```

## Differences from CPython

- `capwords` takes **exactly two arguments** in this A subset (`None` is used instead of a Python default parameter).
- Only **ASCII** constants are provided (no locale-aware/Unicode classification).
- `Formatter` and `Template` are not implemented.

## Validation

```bash
build/a run tests/integration/cases/stdlib/pystring_basic.a
```

## See also

- [`docs/standard-library/README.md`](../../../docs/standard-library/README.md)
- [`stdlib/std/README.md`](../README.md)
- [CPython `string` docs](https://docs.python.org/3/library/string.html)
