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
import std:math                       # stdlib package
import std:prelude                    # prelude helpers (Ok/Err/is_ok/...)
import python:numpy                   # Python module (see python-interop)
from python:json import loads
```

- Quoted paths resolve relative to the importing file, then `-I` dirs,
  `A_PATH`, and the stdlib root (`--stdlib`, `$A_STDLIB`, `./stdlib`).
- Bare names resolve to native modules (`math`, `io`, `fs`, `json`, … — see
  [builtins](builtins.md)) or stdlib packages.
- `std:` imports reach `stdlib/std/`; `python:` imports are resolved at run
  time through libpython.
- Module names after `std:`/`python:` may be A keywords (`enum`, `type`,
  `match`, …), so keyword-named packages like `std/enum/` are importable:
  `from std:enum import Enum`. Plain `import std:enum` binds a keyword name
  — prefer `as` aliasing or the `from` form.
- Modules are namespaced; `import "lib/mathx.a"` binds `mathx.*` members.
- Package `__init__.a` re-exports resolve on every backend: a package can
  forward names with `from "impl.a" import name`, and attribute access
  through the package binding follows the re-export to the module that
  actually declares it (shared `a::resolveModuleMember` in `emitC` and
  `emitAsm`, mirrored in `emitNative`). Example:
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
drives `fn test_*()` instead. Parameters are optional — omitted arguments
behave like a normal call with missing args (defaults apply; a trailing
`*args` collects an empty list).
