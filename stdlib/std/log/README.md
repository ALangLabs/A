# `std.log`

Standard library package `std.log` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Structured logging.
- **Source path:** `stdlib/std/log/`

## Import

```a
import log
```

## What It Provides

- **Functions:** `set_level`, `set_json`, `debug`, `info`, `warn`, `error`

## Example

```a
import log

# Inspect the available functions/constants in this build.
# Then call `log.<member>(...)` using the API overview above.
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
