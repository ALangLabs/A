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
  `break`, `continue`, `return`, `class`, `metaclass`, `true`, `false`,
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
  builtins or `import std:math`. `time.now()` returns epoch **milliseconds**
  (Python uses seconds — convert).
- **Files:** `import std:fs` gives `read_file/write_file/append_file/file_exists/
  list_dir/remove_file/create_dir/remove_dir/rename/current_dir` (string
  paths; `remove_dir` only removes empty dirs; no rglob). No file-object IO —
  design APIs around whole-file strings or document the gap.
- **JSON:** `import std:json` — `parse/stringify/pretty/is_valid/get/at/len/has/
  keys/kind/as_*`. `parse` errors are runtime errors; check `is_valid` first.
- **Random:** global `random()`/`random_int(lo,hi)`; `testing.seed(v)` seeds
  the shared engine. No `random.choice/shuffle` yet — implement in-module.

## Type-checker hazards (verified — read before writing dynamic code)

- **Untyped-param helper functions can hang semantic analysis.** Calling
  `fn f(x)` where `x` is later used in typed operations (`x + ""`, `len(x)`,
  `x[i]`) from multiple call sites with different types can make `a check`/
  `a run` non-terminate (observed >4 min). Keep dynamic-dispatch helpers
  self-contained or annotate params `x: Json` and use only `json.*` accessors
  (`json.kind/as_int/as_float/as_bool/as_str/as_array/as_map/len`) — `Json`
  params accept any argument and allow dynamic operators, but `len`,
  iteration, and indexing do NOT work on `Json` (use `json.len`,
  `json.as_array(x)`, etc.).
- **`json.kind(x)` returns**: `"null" "bool" "number" "string" "array"
  "object"` (NOT int/float — all numbers are `"number"`). It **unwraps
  optionals**: `Some(5)` reports `"number"`, so `None` vs `Some` is
  indistinguishable through it. Sets report `"array"`.
- **`json.as_*` return real optionals** (`Some(v)`/`None`). After an
  `x != None` / `x == None` check the value is **narrowed** — do NOT apply
  `!` (errors "can only unwrap an optional"); use the narrowed value directly.
- **`== None` inside function bodies can miscompile** under the native
  backend; prefer `x == None` only at top level or use `str(x) == "None"`
  probes where semantics allow.
- **`str(x)` is total** — accepts every value without constraining its type;
  useful for kind dispatch (`str(x)` of None is `"None"`, of a map is a
  JSON-ish repr).
- **Chars vs Strings:** `s.chars()` yields `char` values; `char == String`
  is a type error, `char + int` errors, chars can't be indexed into strings.
  `join(char_array, "")` rebuilds a String; `str(char)` gives a 1-char
  String. No `ord`/`chr` builtins — use a 256-entry `\xNN` literal table +
  `.chars()` + `.index(c)` for codepoint lookup (see `stdlib/std/types/tags.a`
  `_slice` helper style).
- **String literals treat `{`/`}` as interpolation** — escape as `\{` `\}`.
- **`a % b` is C fmod** (sign of dividend), not Python floor-mod; `a / b`
  truncates for ints; `max`/`min` take exactly 2 args; no `sorted`/`reversed`/
  `enumerate`/`zip`/`open`/`isinstance`/`getattr` builtins.
- **Class constructors via module alias** (`pkg.Struct(...)`) can fail
  ("unknown class") — `from pkg import Struct` works.
- **Native modules win bare imports**: `import std:collections` resolves the
  NATIVE module, not `stdlib/std/collections/` — Python-compat packages
  sharing a native name need quoted-path imports or a different name.

## Style

- Match existing stdlib: snake_case fns, CamelCase types, `fn name(args):`
  (return annotations optional), doc comments `##` or `"""` docstrings,
  4-space indent, spaces only.
- Every ported module gets a `README.md` noting: source CPython file(s), API
  coverage vs Python (what's missing and why), and semantic deviations
  (exceptions -> panic/Result, iterators -> lists, etc.).
- Keep functions small and dependency-light; prefer returning `[T]`/`{K:V}`/
  `(A,B)` builtins over new classes unless the Python API is class-based.

## Testing

- Each module: add a golden test `tests/integration/cases/stdlib/<mod>_*.a`
  (+ `.out`, `.flags=test` if using `fn test_*`) exercising the main API.
- Run: `build/a run tests/integration/cases/stdlib/<name>.a` and
  `ctest --test-dir build -R integration --output-on-failure` (or the suite
  script `tests/integration/run_tests.sh build/a`).
- Modules must also pass `build/a check`.

## Status (ported so far)

Done + tested: heapq, graphlib, functools, colorsys, keyword, token, types,
reprlib, pprint, collections (classes), operator*, bisect*, shlex*,
bytes (new util pkg), html, fractions.
(* = partial/in-progress by other workers — verify before relying)

Queued: struct, hashlib/hmac, uuid, calendar, ipaddress, urllib.parse,
statistics, fnmatch/glob, sched, csv, difflib, base64/binascii/quopri,
string, numbers, copy trio, codecs, configparser, argparse, getopt,
textwrap, datetime, abc, pyenum (keyword-blocked name), pickle, tomllib.
Hard tier (needs runtime primitives): re, os/io/pathlib, threading,
subprocess, asyncio, tkinter, importlib machinery.
