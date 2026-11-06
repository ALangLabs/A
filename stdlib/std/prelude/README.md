# `std.prelude`

Standard library package `std.prelude` for A.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/prelude`)
- **Source path:** `stdlib/std/prelude/`

## Import

```a
import prelude
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/prelude/__init__.a" import *
```

## What It Provides


## Example

```a
import prelude

# Use the exports listed above from this package.
```

## Usage Notes

- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).
- Prefer reading `__init__.a` and source files for exact signatures/behavior in this version of A.

## Source Files

- `__init__.a`, `prelude.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
