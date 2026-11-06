# `std.token`

Standard library package `std.token` for A.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/token`)
- **Style:** Python-inspired API subset adapted to A semantics (not a drop-in CPython module)
- **Source path:** `stdlib/std/token/`

## Import

```a
import token
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/token/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `isterminal`, `isnonterminal`, `iseof`, `ISTERMINAL`, `ISNONTERMINAL`, `ISEOF`

## Example

```a
import token

# Use the exports listed above from this package.
```

## Usage Notes

- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).
- Prefer reading `__init__.a` and source files for exact signatures/behavior in this version of A.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
