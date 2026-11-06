# A Standard Library (`stdlib/std`)

The A standard library lives in `stdlib/std/` and is organized as package directories (`stdlib/std/<package>/`). It combines:

- **Native-backed modules** implemented in the C++ toolchain/runtime and exposed to A programs (for example `math`, `fs`, `json`, `time`, `net`, `http`).
- **Pure-A packages** implemented in `.a` source files (for example `bisect`, `keyword`, `colorsys`, `textwrap`, `statistics`, `copy`, `functools`).

Many pure-A modules follow **Python-inspired APIs** where that makes sense for users, but they are **adapted to A’s current semantics and runtime**. They are practical subsets/ports, not drop-in CPython replacements.

---

## Table of contents

- [Runtime and compatibility notes](#runtime-and-compatibility-notes)
- [Importing modules](#importing-modules)
- [Quick start examples](#quick-start-examples)
- [Package catalog](#package-catalog)
- [Choosing the right module](#choosing-the-right-module)
- [Documentation layout](#documentation-layout)
- [Testing and validation](#testing-and-validation)
- [Contributing to stdlib](#contributing-to-stdlib)

---

## Runtime and compatibility notes

### A semantics (current state)
A currently executes with **dynamic, Python-style behavior in many paths**:
- plain assignment creates mutable variables
- type annotations are optional
- runtime arithmetic/type behavior follows the current interpreter/toolchain implementation

This affects how Python-inspired stdlib modules behave.

### Common differences vs Python
When using Python-inspired modules in A, expect some intentional differences:

| Python behavior | A stdlib adaptation |
|---|---|
| Default parameters (`def f(x, y=1)`) | Many A ports use explicit parameters or `*_range`/helper variants |
| Exceptions (`ValueError`, `StatisticsError`, etc.) | Contract violations often use runtime `panic`/assertion-style failures |
| `*args` / `**kwargs`, decorators, descriptors | Usually not supported; APIs use explicit arrays/records/call helpers |
| Dynamic object protocols (`__copy__`, `__getattr__`, regex engine, etc.) | Subset behavior or best-effort runtime adaptation |
| Full CPython stdlib coverage | Only feasible subsets are implemented (especially where C extensions/OS internals are required) |

> **Source of truth:** for exact behavior in your build, read the package README, the package `__init__.a`/source files, and the integration tests under `tests/integration/cases/stdlib/`.

---

## Importing modules

### Standard imports
```a
import math
import fs
import json
import time
import core

import string
import textwrap
import statistics
import copy
import bisect
import keyword
import functools
import heapq
```

### Explicit stdlib path imports (useful in source layouts / tooling contexts)
```a
from "std/string/__init__.a" import *
from "std/textwrap/__init__.a" import wrap, fill
```

### Important: `collections` has two faces
Bare `import collections` resolves to the **native** collections/functional helper module in A.

To use the **pure-A Python-style containers** (such as `Counter`, `deque`, `defaultdict`, `OrderedDict`, `namedtuple` ports), import them explicitly from the package path:

```a
# Native collections helpers (bare import)
import collections

# Pure-A Python-style containers
from "std/collections/__init__.a" import Counter, deque, defaultdict, OrderedDict
```

---

## Quick start examples

### Text utilities (`string`, `textwrap`)
```a
import string
import textwrap

print(string.ascii_lowercase)
print(string.capwords("  hello   world  ", None))  # pass None for whitespace behavior

text = "A ships a practical standard library with Python-inspired modules adapted to A."
print(textwrap.wrap(text, 28))
print(textwrap.fill(text, 28))
print(textwrap.shorten(text, 42, " [...]"))
```

### Statistics
```a
import statistics

data = [1, 2, 3, 4, 4]
print(statistics.mean(data))
print(statistics.median(data))
print(statistics.variance(data))
print(statistics.stdev(data))
```

### Sorted lists (`bisect`)
```a
import bisect

values = [1, 3, 5, 7]
bisect.insort(values, 4)
print(values)                 # [1, 3, 4, 5, 7]
print(bisect.bisect_left(values, 4))
print(bisect.bisect_right(values, 4))
```

### Copying containers (`copy`)
```a
import copy

a = [1, [2, 3], {"x": [4]}]
b = copy.copy(a)      # shallow
c = copy.deepcopy(a)  # deep (best-effort for common containers)

b[1][0] = 99
c[2]["x"][0] = 77

print(a)
print(b)
print(c)
```

### JSON + filesystem
```a
import json
import fs

path = "data.json"
fs.write_file(path, json.stringify({"name": "Ada", "scores": [1, 2, 3]}))

doc = json.parse(fs.read_file(path))
print(json.kind(doc))
print(json.is_valid("{\"ok\":true}"))
print(json.stringify(doc))
```

### Time and core assertions
```a
import time
import core

start = time.now()
time.sleep(0.01)
core.assert(time.now() >= start, "time did not advance")
```

---

## Package catalog

> Each package has a detailed README at `stdlib/std/<package>/README.md` with API notes, examples, and A-specific caveats.

| Package | Kind | Purpose | Typical import |
|---|---|---|---|
| `binary` | native-backed | Binary encode/decode of A values | `import binary` |
| `bisect` | pure A (Python subset) | Sorted insertion/search for lists | `import bisect` |
| `builtin` | pure A | Wrappers for builtin conversions/predicates/helpers | `from "std/builtin/__init__.a" import ...` |
| `bytes` | pure A | Byte/char/UTF-8 helper utilities | package import / source |
| `collections` | mixed | Native helpers + pure-A Python-style containers (see note above) | `from "std/collections/__init__.a" import Counter` |
| `colorsys` | pure A (Python port) | RGB/YIQ/HLS/HSV/YCbCr conversions | `import colorsys` |
| `comptime` | pure A | Compile-time/runtime helper utilities | package import / source |
| `copy` | pure A (adapted subset) | Shallow/deep copying for common containers | `import copy` |
| `core` | native-backed | `panic`, assertions | `import core` |
| `format` | pure A | Formatting helpers (CSV/table) | package import / source |
| `fractions` | pure A (Python subset) | Exact rational arithmetic | `import fractions` |
| `fs` | native-backed | File and directory operations | `import fs` |
| `functools` | pure A (Python subset) | Functional helpers (`partial`, cache, reduce, dispatch, sort helpers) | `import functools` |
| `getopt` | pure A (Python subset) | Command-line option parsing | `import getopt` |
| `gpu` | pure A (stub/fallback) | Device/buffer/kernel abstractions | package import / source |
| `graphlib` | pure A (Python subset) | Topological sorting | `import graphlib` |
| `heapq` | pure A (Python subset) | Heap queue algorithms | `import heapq` |
| `html` | pure A (Python subset) | HTML escape/unescape | `import html` |
| `http` | native-backed | HTTP client/server primitives | `import http` |
| `iter` | pure A | Iteration/functional helpers | package import / source |
| `json` | native-backed | JSON parse/serialize/validate/query | `import json` |
| `keyword` | pure A (adapted) | A/Python keyword data and predicates | `import keyword` |
| `log` | native-backed | Structured logging | `import log` |
| `math` | native-backed + wrappers | Math functions/constants | `import math` |
| `memory` | pure A wrappers | Arena/pool/region allocator wrappers | package import / source |
| `metrics` | native-backed | Counters/gauges/histograms | `import metrics` |
| `net` | native-backed | TCP/UDP/polling/TLS primitives | `import net` |
| `operator` | pure A (adapted subset) | Operator-function equivalents | `import operator` |
| `pprint` | pure A (Python subset) | Pretty-printing / readable formatting | `import pprint` |
| `prelude` | compiler-managed | Auto-loaded vocabulary/helpers namespace | `import prelude` (stdlib use) |
| `repl` | mixed | REPL runtime hook + self-hosted REPL components | `import repl` / source |
| `reprlib` | pure A (Python subset) | Size-limited representations | `import reprlib` |
| `shlex` | pure A (Python subset) | Shell-like lexing and quoting | `import shlex` |
| `simd` | pure A (emulated) | SIMD-style/vector helpers | package import / source |
| `statistics` | pure A (Python subset) | Mean/median/variance/stdev and more | `import statistics` |
| `string` | pure A (Python subset) | ASCII constants and `capwords` | `import string` |
| `testing` | native-backed | Property-testing helpers | `import testing` |
| `test` | pure A | Lightweight assertions/reporting | `import test` |
| `textwrap` | pure A (Python subset) | Wrap/fill/dedent/indent/shorten | `import textwrap` |
| `time` | native-backed | Timestamps and sleeping | `import time` |
| `token` | pure A (compat) | Python token-type constants (compat tooling) | `import token` |
| `trace` | native-backed | Tracing/spans/context propagation helpers | `import trace` |
| `traits` | pure A | Trait-like helper contracts/utilities | package import / source |
| `types` | pure A (Python subset) | Type tags/names and `SimpleNamespace` | `import types` |

---

## Choosing the right module

- **Parsing JSON / config-like data:** `json`
- **Reading/writing files:** `fs`
- **CLI text formatting:** `string`, `textwrap`, `format`
- **Math and numeric utilities:** `math`, `statistics`, `fractions`
- **Algorithms over lists:** `bisect`, `heapq`, `graphlib`, `functools`
- **Python-like containers (A ports):** `std/collections` (quoted import) and `types`
- **Assertions/runtime control:** `core`, `test`
- **Network/HTTP:** `net`, `http`
- **Observability:** `log`, `metrics`, `trace`

---

## Documentation layout

- **This index:** `stdlib/std/README.md`
- **Per-package docs (detailed):** `stdlib/std/<package>/README.md`
- **Implementation source:** `stdlib/std/<package>/__init__.a` and sibling `.a` files
- **Extended language/toolchain stdlib reference:** `docs/standard-library/README.md`
- **Native module registry (toolchain):** `compiler/stages/name-resolution/native_modules.hpp`
- **Builtin/LSP docs (toolchain):** `compiler/stages/name-resolution/builtin_docs.hpp`
- **Behavioral tests:** `tests/integration/cases/stdlib/`

---

## Testing and validation

Stdlib behavior is validated primarily with golden integration tests.

```sh
# Build the toolchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j

# Run a specific stdlib test
build/a run tests/integration/cases/stdlib/pystring_basic.a
build/a run tests/integration/cases/stdlib/pytextwrap_basic.a
build/a run tests/integration/cases/stdlib/pystatistics_basic.a
build/a run tests/integration/cases/stdlib/pycopy_basic.a

# Run the full integration suite
tests/integration/run_tests.sh build/a
```

For module work, also add/update the corresponding `.out` (and `.err`/`.exit`/`.flags` when needed).

---

## Contributing to stdlib

When adding a new module or extending an existing one:

1. **Implement the module** in `stdlib/std/<package>/` (`.a` files). Add native/toolchain support only when necessary.
2. **Match A conventions** (current semantics, clear error behavior, and compatibility notes for Python-inspired ports).
3. **Add tests** under `tests/integration/cases/stdlib/` (golden `.a` + expected output/behavior files).
4. **Document it**
   - `stdlib/std/<package>/README.md` (how to import, API, examples, caveats)
   - `stdlib/std/README.md` (catalog/index updates if needed)
   - `docs/standard-library/README.md` (extended reference updates when appropriate)
5. **Update tooling metadata when applicable** (for native/builtin surfaces, LSP docs and registrations in the compiler).
6. **Keep claims accurate**: prefer tested behavior over aspirational API descriptions.

If you’re porting a Python standard library module, start from the subset that is genuinely useful and implementable in A, document deviations explicitly, and add runnable examples/tests that demonstrate the supported behavior.
