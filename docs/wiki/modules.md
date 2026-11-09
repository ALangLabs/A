# Modules and imports

Verified against `compiler/astra/frontend/parser` (ImportStmt/FromImportStmt),
`examples/modules/imports.a`, and `tests/integration/cases/modules/`.

## Import forms

```python
import "lib/mathx.a"                  # quoted path, relative to importer
import "lib/mathx.a" as mx            # with alias
from "lib/mathx.a" import add, Point  # selected names (as-aliases allowed)
from "lib/mathx.a" import Vec as V

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
- Modules are namespaced; `import "lib/mathx.a"` binds `mathx.*` members.
- Package `__init__.a` re-exports resolve on every backend: a package can
  forward names with `from "impl.a" import name`, and attribute access
  through the package binding follows the re-export to the module that
  actually declares it (`a::resolveModuleMember` lives in
  `frontend/codegen/codegen_calls.cpp` and `codegen_expr.cpp` for the
  x86-64 asm emitter, and in `codegen_native.cpp` for the per-arch native
  emitter). Example:
  `import "lib/pkg/__init__.a" as pkg` then `pkg.greet()` calls the
  function re-exported by `pkg/__init__.a`.
- Import cycles and missing modules are compile errors.

## Module files

Any `.a` file is a module; `examples/modules/lib/mathx.a` shows a typical
fixture. Top-level statements run once at import.

## Entry point: `fn main()`

If the entry file (the one passed to `a run`/`a build`) declares `fn main()`,
it is invoked automatically once all module top-level statements have run —
imports initialize first, then `main()`. No explicit `main()` call is needed,
and calling it yourself runs it twice.

`fn main` is never invoked for imported modules: a library that happens to
define `main` stays inert when imported. `a test` skips `main` entirely and
drives `fn test_*()` instead (see [testing](testing.md)).

`fn main` may declare parameters, but the harness never supplies arguments:
default values are honored (`fn main(x = 5)` sees `x == 5`), while parameters
without defaults are left uninitialized — unlike a normal call, which would
be an arity error. Do not rely on non-defaulted `main` parameters (a
trailing `*args` likewise does not collect an empty list).

An entry file without `fn main` is still valid: its top-level statements
run as the program, and the compiler prints a warning first —
`warning: '<file>' has no 'fn main'; only top-level statements will run`.
That warning is expected for script-style programs and for `a run -c`
snippets; declaring `fn main()` silences it. The warning is only emitted
for the entry file, never for imported modules.
