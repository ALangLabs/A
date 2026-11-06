# `std.collections`

Standard library package `std.collections` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Native collection/functional helpers. Note: bare `import collections` resolves to this native module; the pure-A Python-like `std.collections` package is accessed via quoted imports.
- **Source path:** `stdlib/std/collections/`

## Import

```a
# Bare import resolves to the native collections helper module
import collections

# Python-style pure-A container ports (explicit package path)
from "std/collections/__init__.a" import Counter, deque, defaultdict, OrderedDict
```

## What It Provides

- **Functions:** `len`, `map`, `filter`, `reduce`, `any`, `all`, `sum`, `join`, `set`

## Example

```a
import collections

# Inspect the available functions/constants in this build.
# Then call `collections.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Important Import Note

`import collections` resolves to the **native** collections module in A. To use the pure-A Python-style container ports in this package, import explicitly from `std/collections/__init__.a`.

## Source Files

- `__init__.a`, `chainmap.a`, `collections.a`, `counter.a`, `defaultdict.a`, `deque.a`, `dict.a`, `list.a`, `namedtuple.a`, `ordereddict.a`, `userdict.a`, `userlist.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
