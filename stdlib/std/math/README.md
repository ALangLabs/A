# `std.math`

Math functions and constants, backed by the native `math` module with A-friendly usage.

Use `import math` for common numeric operations. The native module also exposes random helpers; wrappers in this package may expose a curated subset.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Math functions and constants (native-backed wrappers are also provided in std.math A modules).
- **Source path:** `stdlib/std/math/`

## Import

```a
import math
```

## What It Provides

- **Functions:** `sqrt`, `abs`, `min`, `max`, `pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`, `random`, `random_int`
- **Constants:** `PI`, `E`

## API Reference (Practical)

- Common functions (native-backed): `sqrt`, `abs`, `min`, `max`, `pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`.
- Constants: `PI`, `E`.
- Additional native helpers may include `random`, `random_int` (toolchain-dependent).

## Examples

```a
import math

print(math.sqrt(81))
print(math.pow(2, 10))
print(math.sin(0))
print(math.floor(3.9))
print(math.PI)
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- Check `compiler/stages/name-resolution/native_modules.hpp` and package source for the exact exported surface in your build.
- Numeric behavior follows A's current runtime/type semantics.

## Source Files

- `__init__.a`, `constants.a`, `functions.a`, `math.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
