# Standard library

`stdlib/` contains A-source library code loaded through the module system —
there is no separate runtime for it; everything is compiled by the same
frontend/backend pipeline.

## Layout

- `stdlib/prelude.a` (and duplicate `stdlib/std/prelude/prelude.a`) — loaded
  before every program by the module loader; namespaced under `prelude.*`
  (e.g. `class Vec4`, `Result`, `Ok`, `prelude_version()`). Native modules are
  untouched by it.
- `stdlib/std/std.a` / `stdlib/std/__init__.a` — top-level re-export shims
  pulling in subpackages (`builtin`, `collections`, `math`, `simd`, `traits`,
  `memory`, ...).
- `stdlib/std/<pkg>/__init__.a` (+ domain files) — one package per directory:
  `binary`, `bisect`, `builtin`, `bytes`, `collections` (chainmap, counter,
  defaultdict, deque, dict, list, namedtuple, ordereddict, userdict, userlist),
  `colorsys`, `comptime`, `copy`, `core`, `debug`, `format` (csv, table),
  `fractions`, `fs`, `functools` (cache, dispatch, partial, reduce, sort),
  `getopt`, `gpu`, `graphlib`, `heapq`, `html`, `http`, `iter`, `json`,
  `keyword`, `log`, `math`, `memory`, `metrics`, `net`, `operator`, `pprint`,
  `prelude`, `pyenum` (has its own README.md), `repl` (complete, editor,
  highlight, history, main — the self-hosted IPython-style shell run by
  `a repl` on a TTY), `reprlib`, `shlex`, `simd`, `statistics`, `string`,
  `test`/`testing`, `textwrap`, `time`, `token`, `trace`, `traits`
  (comparable, copyable, equatable, hashable), `types`.

## Import forms

- Quoted relative paths: `import "lib/util.a"`, `from "lib/util.a" import add, Vec as Point`.
- Bare native modules: `import math`, `import std:prelude`.
- Python interop: `import python:<pkg>` / `from python:<pkg> import x` —
  resolved at runtime via the `py` native module (libpython `dlopen`'d; see
  [runtime.md](runtime.md)).

## Relationship to native modules

Many `std/` modules are thin A wrappers over the native-module table in
`compiler/astra/frontend/name-resolution/native_modules.hpp` (`math.*`,
`fs.*`, `json.*`, `net.*`, `py.*`, `kernel_ops.*`, ...), whose members map to
`A_BI_*` runtime builtins. Sema checks builtin calls through the
`sema_builtins_*.cpp` domain files; docs come from `builtin_docs_*.hpp`.

## How it fits

`ModuleLoader` resolves `std:`/quoted/bare imports against `stdlibRoot`
(`--stdlib`, `$A_STDLIB`, executable-relative, `./stdlib`; see
`defaultStdlibRoot()` in `module_loader.cpp`). `tests/integration/cases/stdlib/`
and `examples/stdlib/` exercise it. Related:
[Name resolution](frontend-name-resolution.md), [Kernels](kernels.md),
[examples](examples.md), [overview](overview.md).
