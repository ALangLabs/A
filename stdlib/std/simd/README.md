# `std.simd`

Standard library package `std.simd` for A.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/simd`)
- **Source path:** `stdlib/std/simd/`

## Import

```a
import simd
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/simd/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `vec_new`, `scale`, `dot_lists`, `hadamard`, `dot_unrolled`

## Example

```a
import simd

# Use the exports listed above from this package.
```

## Usage Notes

- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).
- Prefer reading `__init__.a` and source files for exact signatures/behavior in this version of A.

## Source Files

- `__init__.a`, `simd.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
