# `std.repl`

Standard library package `std.repl` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Persistent REPL evaluation hook.
- **Source path:** `stdlib/std/repl/`

## Import

```a
import repl
```

## What It Provides

- **Functions:** `eval`

## Example

```a
import repl

# Inspect the available functions/constants in this build.
# Then call `repl.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Source Files

- `complete.a`, `editor.a`, `highlight.a`, `history.a`, `main.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
