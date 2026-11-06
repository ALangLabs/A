# `std.trace`

Standard library package `std.trace` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Tracing/span utilities.
- **Source path:** `stdlib/std/trace/`

## Import

```a
import trace
```

## What It Provides

- **Functions:** `start`, `end`, `current`, `set_attribute`, `add_event`, `inject`, `extract`, `spans`, `reset`

## Example

```a
import trace

# Inspect the available functions/constants in this build.
# Then call `trace.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
