# `std.builtin`

Standard library package `std.builtin` for A.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/builtin`)
- **Source path:** `stdlib/std/builtin/`

## Import

```a
import builtin
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/builtin/__init__.a" import *
```

## What It Provides

- **Exports:** re-exported from submodules (see `__init__.a`).

## Example

```a
import builtin

# Use the exports listed above from this package.
```

## Usage Notes

- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).
- Prefer reading `__init__.a` and source files for exact signatures/behavior in this version of A.

## Source Files

- `__init__.a`, `bool.a`, `builtin.a`, `debug_assert.a`, `float.a`, `int.a`, `len.a`, `range.a`, `string.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
