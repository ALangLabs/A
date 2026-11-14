# Modules and imports

Verified against `compiler/astra/frontend/parser` (ImportStmt/FromImportStmt),
`examples/modules/imports.astra`, and `tests/integration/cases/modules/`.

## Import forms

```python
import "lib/mathx.astra"                  # quoted path, relative to importer
import "lib/mathx.astra" as mx            # with alias
from "lib/mathx.astra" import add, Point  # selected names (as-aliases allowed)
from "lib/mathx.astra" import Vec as V

import math                           # bare native module
import std:math                       # stdlib package — but see note below
import std:prelude                    # prelude helpers (Ok/Err/is_ok/...)
import python:numpy                   # Python module (see python-interop)
from python:json import loads
```

- Quoted paths resolve relative to the importing file, then `-I` dirs,
  `A_PATH`, and the stdlib root (`--stdlib`, `$A_STDLIB`, `./stdlib`).
- Bare names resolve to native modules (`math`, `io`, `fs`, `json`, … — see
  [builtins](builtins.md)) or stdlib packages.
- `std:` imports reach `stdlib/std/`; `python:` imports are resolved at run
  time through libpython. When a `std:` name collides with a native module,
  the **native module wins**: `import std:math` binds the native `math`
  module (`math.abs`, `math.min`, `math.PI`, …), not the A-file wrappers in
  `stdlib/std/math/` (which expose `abs_val`, `gcd`, `comb`, … and are only
  reachable via quoted relative imports).
- Module names after `std:`/`python:` may be A keywords (`enum`, `type`,
  `match`, …), so keyword-named packages like `std/enum/` are importable:
  `from std:enum import Enum`. Plain `import std:enum` binds a keyword name
  — prefer `as` aliasing or the `from` form.
- Modules are namespaced; `import "lib/mathx.astra"` binds `mathx.*` members.
- Package `__init__.astra` re-exports resolve on every backend: a package can
  forward names with `from "impl.astra" import name`, and attribute access
  through the package binding follows the re-export to the module that
  actually declares it (`a::resolveModuleMember` lives in
  `frontend/codegen/codegen_calls.cpp` and `codegen_expr.cpp` for the
  x86-64 asm emitter, and in `codegen_native.cpp` for the per-arch native
  emitter). Example:
  `import "lib/pkg/__init__.astra" as pkg` then `pkg.greet()` calls the
  function re-exported by `pkg/__init__.astra`.
- Import cycles and missing modules are compile errors.

## Module files

Any `.astra` file is a module; `examples/modules/lib/mathx.astra` shows a typical
fixture. Top-level statements run once at import.

## Entry point: `entry name()`

Declare the program entry point with the `entry` keyword — an ordinary
function the compiler invokes once after all module top-level statements
have run (imports initialize first, then `entry`). At most one `entry`
function per module; multiple in the entry file is a compile error.

```python
entry main():
    ...
```

`entry` functions in imported modules are never invoked — a library that
happens to define one stays inert when imported, and the same file still
works when run directly. Top-level statements always run, so script-style
files need no `entry` at all. `a test` skips `entry` and drives
`fn test_*()` instead (see [testing](testing.md)).

`fn main` is just an ordinary function now — nothing calls it implicitly.
An entry file without `entry` is still valid: its top-level statements run
as the program. This is normal for script-style programs and
`a run -c` snippets; declare `entry` when you want an explicit entry point.
