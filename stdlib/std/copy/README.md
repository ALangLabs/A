# `std.copy`

> Shallow/deep copy helpers for A values, inspired by CPython's [`copy`](https://docs.python.org/3/library/copy.html) module.

`std.copy` provides **practical copying utilities** adapted to A's container and runtime semantics. It is useful when you need an independent copy of common array/map values or to preserve scalar values unchanged.

> [!IMPORTANT]
> This implementation is **adapted to A**. It uses JSON-compatible serialization strategies where available for portable deep copying of common containers. It is **not a drop-in replacement** for CPython's `copy` (no memo/cycle graph, no `__copy__`/`__deepcopy__` protocol, and behavior may differ for identity-sensitive or non-JSON-native values).

## Status

| Field | Value |
|---|---|
| Kind | Pure A stdlib package |
| Source | `stdlib/std/copy/__init__.a` |
| Import names | `copy` (explicit path import recommended for source layouts) |
| API style | Python-inspired subset, A-adapted |
| Deep-copy strategy | JSON stringify/parse for supported container shapes |

## Import

```a
from "std/copy/__init__.a" import copy, deepcopy
```

```a
import copy
import json
print(json.stringify(copy.deepcopy([1, 2, 3])))
```

## API reference

### `copy(x)`

Return a copy of `x` for common containers/scalars.

- Arrays/maps: copied (implementation uses a JSON-compatible copy strategy for portability).
- Scalars/other values: returned as-is.

### `deepcopy(x)`

Return an independent deep copy for nested arrays/maps in common A usage.

- Nested containers: deep-copied via JSON-compatible serialization.
- Scalars: returned as-is.
- No cycle detection.

## Examples

### Copy arrays and maps

```a
from "std/copy/__init__.a" import copy, deepcopy
import json

a = [1, 2, 3, 4]
b = copy(a)
print(json.stringify(b))

m = {"x": 1, "y": 2}
n = copy(m)
print(json.stringify(n))

nested = [[1, 2], [3, 4]]
d = deepcopy(nested)
print(json.stringify(d))
```

### Scalars

```a
from "std/copy/__init__.a" import copy, deepcopy

print(copy(5))          # 5
print(deepcopy("hi"))   # hi
```

## Differences from CPython

- No `copy.Error` / `copy.replace`.
- No cycle-detection memo.
- No `__copy__` / `__deepcopy__` / `__reduce__` hooks.
- Deep copy is implemented with JSON serialization for supported values; values not representable/compatible with that strategy may behave differently than CPython.

## Validation

```bash
build/a run tests/integration/cases/stdlib/pycopy_basic.a
```

## See also

- [`docs/standard-library/README.md`](../../../docs/standard-library/README.md)
- [`stdlib/std/README.md`](../README.md)
- [CPython `copy` docs](https://docs.python.org/3/library/copy.html)
