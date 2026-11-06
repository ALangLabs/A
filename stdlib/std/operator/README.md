# `std.operator`

Standard library package `std.operator` for A.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/operator`)
- **Style:** Python-inspired API subset adapted to A semantics (not a drop-in CPython module)
- **Source path:** `stdlib/std/operator/`

## Import

```a
import operator
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/operator/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `lt`, `le`, `eq`, `ne`, `ge`, `gt`, `truth`, `not_`, `is_`, `is_not`, `is_none`, `is_not_none`, `abs`, `add`, `and_`, `floordiv`, `index`, `inv`, `invert`, `lshift`, `mod`, `mul`, `neg`, `or_`, `pos`, `pow`, `rshift`, `sub`, `truediv`, `xor`, `concat`, `contains`, `countOf`, `delitem`, `getitem`, `indexOf`, `setitem`, `length_hint`, `itemgetter`, `itemgetter_all`, `repeat_seq`, `repeat`

## Example

```a
import operator

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
