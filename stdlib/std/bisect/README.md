# `std.bisect`

Sorted-list search and insertion helpers inspired by Python's `bisect` module.

Pure-A implementation of bisection algorithms for maintaining sorted lists. Because A does not support Python-style default parameters, range variants are provided explicitly.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/bisect`)
- **Style:** Python-inspired API subset adapted to A semantics (not a drop-in CPython module)
- **Source path:** `stdlib/std/bisect/`

## Import

```a
import bisect
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/bisect/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `bisect_left_range`, `bisect_left`, `bisect_right_range`, `bisect_right`, `insort_left_range`, `insort_left`, `insort_right_range`, `insort_right`, `bisect`, `bisect_range`, `insort`, `insort_range`

## API Reference (Practical)

- `bisect_left(a, x)`, `bisect_right(a, x)`, `bisect(a, x)` — insertion points.
- `bisect_left_range(a, x, lo, hi)`, `bisect_right_range(a, x, lo, hi)`, `bisect_range(a, x, lo, hi)` — explicit bounds.
- `insort_left(a, x)`, `insort_right(a, x)`, `insort(a, x)` — in-place insertion.
- `insort_left_range(...)`, `insort_right_range(...)`, `insort_range(...)` — in-place insertion with bounds.

## Examples

```a
import bisect

a = [1, 3, 5]
bisect.insort(a, 4)
print(a)
print(bisect.bisect_left(a, 4))
print(bisect.bisect_right(a, 4))
```

## Usage Notes

- Use `*_range` functions instead of Python's `lo`/`hi` default arguments.
- `key=` is not supported in this subset.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
