# `std.http`

Standard library package `std.http` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** HTTP client/server helpers.
- **Source path:** `stdlib/std/http/`

## Import

```a
import http
```

## What It Provides

- **Functions:** `get`, `post`, `request`, `serve`, `serve_once`

## Example

```a
import http

# Inspect the available functions/constants in this build.
# Then call `http.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (Astra backend paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
