# Python stdlib porting guide

Goal: port CPython `Lib/` pure-Python modules to A under `stdlib/std/`, keeping
Python-compatible module names and APIs where A's runtime allows.

Python sources of truth: local `/usr/lib/python3.14/` (CPython 3.14) or
`gh api repos/python/cpython/contents/Lib/<path>` for `main`.

## Module placement & naming

- New packages go in `stdlib/std/<modname>/` with `__init__.a` plus one file
  per logical unit (mirror CPython's file split when it helps).
- Bare `import <modname>` resolves `stdlib/std/<modname>/__init__.a`
  automatically (module_loader checks `std/<name>/__init__.a`).
- **Do not** create packages that shadow native modules: `math`, `time`, `io`,
  `fs`, `strings`, `collections`, `core`, `json`, `binary`, `testing`, `log`,
  `metrics`, `trace`, `net`, `http`, `env`, `terminal`, `repl`, `arena`,
  `unsafe`, `random`(? verify), `prelude`, `std`, `builtin`, `iter`, `format`,
  `memory`, `simd`, `traits`, `comptime`, `gpu`, `test`, `repl`.
  Python modules colliding with those names get a `py` prefix package
  (e.g. `stdlib/std/pytime/`) or are skipped — check
  `compiler/stages/name-resolution/native_modules.hpp` first.
- `enum`, `type`, `match`, `loop`, `defer`, `pass`, `self`, `in`, `as`,
  `from`, `do`, `class`, `None`, `true`, `false` are **keywords** — never use
  them as identifiers. Python `None` -> `None`, `True/False` -> `true/false`.

## Semantic mapping (no exceptions, no generators)

- **No try/except/raise/yield/async.** Python exception-raising code becomes
  either a runtime error (`assert`/`panic` — matches existing stdlib style for
  contract violations) or a `Result`/`T?` return (`Ok`/`Err`, `Some`/`None`,
  `??`, `!.`). Document the choice in the module README.
- **No generators/iterators.** Anything Python exposes as an iterator/generator
  returns a materialized `[T]` list in A. `for` only iterates arrays, ranges,
  maps (keys), sets, strings (chars). `map.items()` gives `[(k,v)]` tuples —
  destructure via `item[0]`/`item[1]` (verify `for k, v in ...` support first).
- **No `isinstance`/`type()`/`getattr`/`hasattr`/`callable`/`open`.** Runtime
  type tag: `comptime.type_tag(x)` or `json.kind(x)` (returns
  `"null|bool|int|float|string|array|object"`).
- **No tuple destructuring in `for` headers** (verified: `for k, v in ...`
  is a parse error). Use `for item in m.items(): k = item[0]; v = item[1]`.
- **Keyword-named modules can't be bare-imported** (verified: `import enum`
  fails to parse). Packages named after A keywords (`enum`, `type`, `match`,
  `class`, `loop`, `do`, `pass`, `self`, `in`, `as`, `from`, `defer`, `where`,
  `comptime`, `impl`, `trait`, `fn`, `if`, `else`, `elif`, `while`, `for`,
  `break`, `continue`, `return`, `struct`, `metaclass`, `true`, `false`,
  `None`, `and`, `or`, `not`, `import`) must either use a `py`-prefixed dir
  (e.g. `stdlib/std/pyenum/`) or rely on quoted-path imports. Check by writing
  a tiny `import <name>` test file before committing to a name.
- **Missing builtins:** `sorted`, `reversed`, `enumerate`, `zip`, `bool()`,
  `range()` — use `arr.sort()`/`.reverse()` methods, `std.iter` helpers
  (`range_step`, `take`, `enumerate_pairs`, `zip_apply`, `repeat`), ranges
  `a..b`, `std.builtin.Bool(x)`.
- **Strings:** rich method set exists (`split`, `join`, `strip`, `replace`,
  `startswith`, case ops — ASCII-only). No regex engine — `re`-dependent
  modules need hand-rolled parsing or are deferred.
- **Numbers:** `int`/`float` builtins + `i64`/`f64` etc.; math via global
  builtins or `import math`. `time.now()` returns epoch **milliseconds**
  (Python uses seconds — convert).
- **Files:** `import fs` gives `read_file/write_file/append_file/file_exists/
  list_dir/remove_file/create_dir/remove_dir/rename/current_dir` (string
  paths; `remove_dir` only removes empty dirs; no rglob). No file-object IO —
  design APIs around whole-file strings or document the gap.
- **JSON:** `import json` — `parse/stringify/pretty/is_valid/get/at/len/has/
  keys/kind/as_*`. `parse` errors are runtime errors; check `is_valid` first.
- **Random:** global `random()`/`random_int(lo,hi)`; `testing.seed(v)` seeds
  the shared engine. No `random.choice/shuffle` yet — implement in-module.

## Style

- Match existing stdlib: snake_case fns, CamelCase types, `fn name(args):`
  (return annotations optional), doc comments `##`, 4-space indent, spaces only.
- Every ported module gets a `README.md` noting: source CPython file(s), API
  coverage vs Python (what's missing and why), and semantic deviations
  (exceptions -> panic/Result, iterators -> lists, etc.).
- Keep functions small and dependency-light; prefer returning `[T]`/`{K:V}`/
  `(A,B)` builtins over new structs unless the Python API is class-based.

## Testing

- Each module: add a golden test `tests/integration/cases/stdlib/<mod>_*.a`
  (+ `.out`, `.flags=test` if using `fn test_*`) exercising the main API.
- Run: `build/a run tests/integration/cases/stdlib/<name>.a` and
  `ctest --test-dir build -R integration --output-on-failure` (or the suite
  script `tests/integration/run_tests.sh build/a`).
- Modules must also pass `build/a check`.
