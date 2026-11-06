# `std.core`

Core runtime helpers for assertions and aborting execution.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Core runtime assertions and panic.
- **Source path:** `stdlib/std/core/`

## Import

```a
import core
```

## What It Provides

- **Functions:** `panic`, `assert`, `assert_eq`

## API Reference (Practical)

- `panic(message)` — abort with an error message.
- `assert(condition[, message])` — runtime assertion (tooling/runtime support may vary).
- `assert_eq(actual, expected[, message])` — equality assertion helper.

## Examples

```a
import core

core.assert(1 + 1 == 2, "math broke")
# core.panic("stop here")
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (Astra backend paths) and host environment.
- `panic` is commonly used by stdlib ports to report contract violations in A (where Python might raise).

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
