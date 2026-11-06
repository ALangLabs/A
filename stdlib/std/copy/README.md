# `std.copy`

Shallow and deep copy helpers adapted to A's container/runtime model (inspired by Python's `copy` module).

Provides practical copying for common A containers. Python-specific protocols and identity/memo semantics are not fully replicated.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/copy`)
- **Style:** Python-inspired API subset adapted to A semantics (not a drop-in CPython module)
- **Source path:** `stdlib/std/copy/`

## Import

```a
import copy
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/copy/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `copy`, `deepcopy`

## API Reference (Practical)

- `copy(x)` — shallow copy for common containers (lists/dicts where supported); scalars return as-is.
- `deepcopy(x)` — recursive deep copy for nested lists/dicts (best effort).

## Examples

```a
import copy

a = [1, [2, 3], 4]
b = copy.copy(a)
b[0] = 9
b[1][0] = 8
print(a)  # nested element is shared with a shallow copy

c = [1, [2, 3], {"x": [4]}]
d = copy.deepcopy(c)
d[1][0] = 7
d[2]["x"][0] = 6
print(c)
print(d)
```

## Usage Notes

- No cycle detection and no Python `__copy__`/`__deepcopy__` hook support.
- Behavior is adapted to A runtime/container semantics; test against your data shapes when copying complex values.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
