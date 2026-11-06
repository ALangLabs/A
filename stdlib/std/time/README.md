# `std.time`

Basic time primitives for timestamps and sleeping.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Time primitives.
- **Source path:** `stdlib/std/time/`

## Import

```a
import time
```

## What It Provides

- **Functions:** `now`, `sleep`

## API Reference (Practical)

- `now()` — current time value (runtime-defined numeric/time representation).
- `sleep(seconds)` — pause execution for the requested duration.

## Examples

```a
import time

start = time.now()
time.sleep(0.01)
print(time.now() >= start)
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- Time precision/units are runtime-defined; use it for simple timing and delays.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
