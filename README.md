<p align="center">
  <h1 align="center"><b>The A Programming Language</b></h1>
  <p align="center">
    <strong>Fast as C++. Expressive as Python. Statically typed. Memory safe.</strong>
  </p>
  <p align="center">
    <a href="#-benchmarks">Benchmarks</a> •
    <a href="#-quickstart">Quickstart</a> •
    <a href="#-language-tour">Language Tour</a> •
    <a href="#-tooling">Tooling</a> •
    <a href="#-editor-support">Editor Support</a> •
    <a href="#-key-features">Features</a> •
    <a href="docs/">Docs</a>
  </p>
</p>

---

## ⚡ What is A?

**A** is a high-performance, statically typed systems and application language designed with modern indentation-based syntax. It brings together the speed of native machine code and the ergonomic elegance of Python, giving developers predictable execution, zero-overhead abstractions, and unmatched readability.

```a
class Vector2D:
    x: float
    y: float

    fn length_squared(self) -> float:
        return self.x ** 2.0 + self.y ** 2.0

point = Vector2D(3.0, 4.0)
print("Length squared: {point.length_squared()}")  # 25.0
```

---

## 🚀 Key Features

* **⚡ C++ Parity Performance**: Native compilation (`a build`) producing standalone machine-code binaries with ~2.4 ms startup, matching or beating optimized C++ (`g++ -O2`) on the benchmark suite.
* **🐍 Python-Grade Ergonomics**: Python-style natural variable declarations (`x = 10`), clean indented blocks, and the fast `a run` compile-and-execute path.
* **🔗 Python Interop**: `import python:<pkg>` / `from python:<pkg> import name` embeds CPython directly — call NumPy, PyTorch, `json`, or any installed package with A syntax (interpreter path; needs Python 3 dev files at build time).
* **🏛️ Classes, Inheritance & Metaclasses**: Clean object-oriented programming with `class Child(Parent):` and declarative `metaclass Meta: pass`.
* **🛠️ 25+ First-Class Operators**: Rich operator support including arithmetic (`+ - * / % **`), bitwise (`& | ^ ~ << >>`), containment (`in`, `not in`), functional pipelines (`|>`), and safe null-coalescing (`??`, `?.`).
* **🔒 Static Type Safety & Inference**: Unification-based global type inference with checked arithmetic, zero undefined behavior, and deterministic cleanup via `defer`.
* **🧰 Production Tooling**: An AST formatter (`a fmt`), documentation generator (`a doc`), dependency-free language server (`a-lsp`), and VS Code extension with live diagnostics, navigation, completion, formatting, rename, folding, and syntax highlighting.

---

## 📊 Benchmarks

Best-of-5 wall time on identical workloads — A compiled to a native binary
(`a build`) vs. C++ (`g++ -O2 -std=c++20`) vs. CPython. Measured on an
AMD Ryzen 5 7520U (x86-64 Linux), g++ 13.3.0, CPython 3.14.9:

| Workload | Task | A Native | C++ (`-O2`) | CPython 3.14 | A vs. C++ |
|:---|:---|---:|---:|---:|---:|
| **Arithmetic** | 200,000 range additions | **2.22 ms** | 2.42 ms | 43.65 ms | **0.9×** |
| **Collections** | 5,000-element filter/map/sum pipeline | **2.30 ms** | 2.66 ms | 24.63 ms | **0.9×** |
| **Memory** | 100k `str()` + join | **5.48 ms** | 4.91 ms | 38.95 ms | **1.1×** |
| **Numerical** | 50,000 `sqrt` + `round` | **2.63 ms** | 3.19 ms | 34.02 ms | **0.8×** |

**Startup** (no-op program, wall time): A native binary **~2.4 ms**, C++ binary
**~2.4 ms**, CPython **~40 ms**. End-to-end `a run`/`a build` of a no-op —
frontend → x86-64 assembly → astra assemble/link — is **~40 ms**.

*Reproduce with `python3 tools/benchmark-runner/compare.py --native --runs 5`;
methodology and targets in [`docs/performance/`](docs/performance/README.md).*

---

## 🛠 Quickstart

### Prerequisites
* CMake 3.20+
* C++20 compiler (GCC 11+, Clang 14+, or MSVC 19.29+)
* The staged `astra` backend (built with the project, `build/astra/` + `build/astralib/`) for `a build`/`a run`/`a test` — the frontend emits x86-64 assembly and `astra` assembles/links it with the A runtime into a native executable (no `g++` build step)

### Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Running Programs
```bash
# 1. Standard execution:
./build/a run examples/classes/inheritance.a

# 2. Type-check without running:
./build/a check program.a

# 3. Run every `fn test_*()` in a program:
./build/a test program.a

# 4. Compile to an optimized standalone native binary:
./build/a build program.a -o my_app
./my_app

# 5. Interactive REPL (state persists across lines, `>>>` echo):
./build/a repl

# 6. Inline source, like `python -c`:
./build/a -c 'print("hello from A")'

# 7. Format and document it:
./build/a fmt -w program.a
./build/a doc -o docs/program.md program.a

# 8. Inspect the typed SSA IR (optionally after optimization):
./build/a ir program.a
./build/a ir --opt program.a
```

### Python interop

A embeds the CPython found at build time, so any installed Python package is
callable with A syntax — no FFI boilerplate:

```a
import python:json
from python:math import sqrt

print(json.dumps({"a": 1}))   # {"a": 1}
print(sqrt(2))                # 1.4142135623730951
```

Works in `a run`, `a test`, `a repl`, and `a -c` (interpreter path only).

`a` does **not** link one Python at build time — like Mojo, it discovers a
CPython at runtime and `dlopen`s its `libpython`. On the first `python:`
import it probes, in order: `A_PYTHON`/`A_PYTHON_LIB` overrides, a local
`.venv`/`venv`, `VIRTUAL_ENV`/`UV_PROJECT_ENVIRONMENT`/`CONDA_PREFIX`,
`python3`/`python` on `PATH`, uv-managed installs
(`~/.local/share/uv/python/*`), then system libraries — preferring the
environment that actually contains the module you're importing, so
`uv pip install numpy` / `uv add torch` just works. The chosen
interpreter's `site-packages` is added to `sys.path`, and `A_PYTHONPATH`
(colon-separated) adds extra dirs. Build-time CMake only needs Python
headers (`-DPython3_EXECUTABLE` still works as a hint).

Python objects support `.attr`, `obj[key]`, calls, `==`, and A's operators
(`+ - * / % **`, comparisons, `in`) are forwarded to CPython — so
`np_array * 2` and `5 in torch_tensor` behave exactly like Python.
See `examples/python-interop/` for runnable examples, including NumPy and
PyTorch under `optional/`.

---

## 🔍 Language Tour

### 1. Variables & Clean Declarations
```a
# Natural variable assignment (mutable in place)
count = 10
count += 5
print(count)  # 15

# Explicit immutable constants
const PI = 3.14159265359
const APP_NAME: str = "A-Engine"
```

### 2. Classes & Inheritance
```a
class Shape:
    name: str

    fn describe(self) -> str:
        return "Shape: {self.name}"

class Circle(Shape):
    radius: float

    fn area(self) -> float:
        return 3.14159 * (self.radius ** 2.0)

c = Circle("UnitCircle", 10.0)
print(c.describe(), "Area:", c.area())
```

### 3. Functional Pipelines & Comprehensions
```a
# Clean readable data pipelines using |>
sum_of_evens = [x for x in 0..100]
    |> filter(fn(n) => n % 2 == 0)
    |> map(fn(n) => n * 2)
    |> sum()

print("Result:", sum_of_evens)
```

### 4. Containment & Modern Operators
```a
items = ["apple", "banana", "cherry"]
if "banana" in items:
    print("Found banana!")

if "grape" not in items:
    print("Grape is missing")

# Safe null navigation & Elvis fallback
user_theme = user?.settings?.theme ?? "dark"
```

---

## 📚 Standard Library

`import math`, `from json import parse` — the toolchain resolves modules
against the file's directory, `-I` paths, `A_PATH`, and the stdlib root
(`--stdlib <dir>` to override). Native modules are built into the toolchain;
`stdlib/` additionally holds A-file modules and per-module notes.

| Module | Provides |
| :--- | :--- |
| `math` | `sqrt` `sin` `log` `round` `random` … plus `PI`, `E` |
| `io` | `print` `eprint` `input` `flush` |
| `fs` | read/write/append files, directory listing, rename |
| `strings` | `len` `str` `join` (plus Python-style dot methods on `String`) |
| `collections` | `map` `filter` `reduce` `any` `all` `sum` `set` (dot methods on arrays/maps/sets) |
| `core` | `panic` `assert` `assert_eq` |
| `time` | `now` `sleep` |
| `unsafe` `arena` | raw pointer access; bump/pool/region allocators |
| `json` `binary` | parsing, reading, pretty-printing; big-endian byte encode/decode |
| `testing` | seeded property testing (`for_all_int`, …) |
| `log` `metrics` `trace` | structured logging, Prometheus counters/gauges/histograms, span propagation |
| `net` `http` | TCP/UDP sockets, optional TLS, `get`/`post`/`serve` |
| `test` (A-file) | assertion harness: `check` `equal` `require` `report` |

Interpreter-backed modules (json, binary, testing, log, metrics, trace, net,
http) run under `a run` and `a test`; `a build` reports when a compiled backend
cannot lower them. The full member tables and semantics live in
[`docs/standard-library/`](docs/standard-library/README.md), and
[`examples/README.md`](examples/README.md) indexes runnable programs.

---

## 📂 Project Organization

```
├── benchmarks/               # Cross-language benchmark suites (arithmetic, collections, memory, numerical, real-world)
├── cmake/                    # AOptions.cmake (options, warnings, LTO) + libastra.cmake (backend/runtime build)
├── compiler/
│   └── astra/                # The Astra toolchain
│       ├── frontend/         # C++ frontend (a.hpp umbrella; CMake target a_frontend)
│       │   ├── lexer/        # Indentation-aware lexer, tokens, spans, source locations
│       │   ├── parser/       # Recursive-descent parser (core/decl/stmt/type/expr/pattern/string)
│       │   ├── ast/          # Typed AST nodes (exprs, stmts, kinds, type exprs, builtins)
│       │   ├── diagnostics/  # DiagnosticSink + SourceFile
│       │   ├── name-resolution/  # Module loader, native module table, builtin docs (per-domain tables)
│       │   ├── type-checker/ # Sema & unification type inference (+ builtin domain checkers)
│       │   ├── ownership/    # Move/borrow analysis (`move(x)`, view borrows)
│       │   ├── ir/           # Typed SSA IR: builder, analysis, printer
│       │   ├── optimization/ # Constant folding, DCE, SCCP, inlining, LICM, instcombine
│       │   ├── codegen/      # x86-64 AT&T assembly emitter (emit/analysis/calls/decls/expr/stmt)
│       │   ├── tooling/      # AST formatter, doc generator, shared printers
│       │   ├── pipeline/     # Frontend → asm → Astra → native executable driver
│       │   ├── driver/       # Version, CLI glue
│       │   └── cli/          # `a run|build|check|test|fmt|doc|ir|repl` (main.cpp)
│       ├── backend/          # Astra C backend: assembler/linker producing native ELF
│       └── runtime/          # C runtime modules linked into compiled programs
│           ├── a_runtime.c/.h        # Runtime entry + public API
│           ├── core_*.c, value_*.c   # Values, buffers, dicts, arrays, formatting
│           ├── builtins_*.c          # Builtin functions by domain
│           ├── methods_*.c           # Dot-method dispatch by domain
│           └── fs/json/binary/log/metrics/trace/net/arena/unsafe/python/terminal/time/intrinsics.c
├── stdlib/                   # Module notes + A-file modules (math, json, net, …)
│   └── std/                  # Self-hosted stdlib in A (Mojo std/ analog)
├── kernels/                  # A-written compute kernels (Mojo MAX analog)
├── examples/                 # Runnable programs, indexed by examples/README.md
├── docs/                     # Language spec, type system, compiler design, performance
├── tools/
│   ├── lsp/                  # a-lsp: diagnostics, navigation, completion, hover (handlers_*.cpp)
│   ├── vscode-a/             # VS Code extension (grammar + LSP client)
│   └── benchmark-runner/     # run.sh + compare.py
└── tests/                    # Automated golden test suite (integration/, ir/, tooling/)
```

---

## 🧰 Tooling

The toolchain ships with first-party developer tools built on the same lexer,
parser, and AST as the compiler.

### Command line: `a`

```
a <command> [options] <file.a>

  repl             start the interactive REPL (state persists; 'exit' to quit)
  run <file.a>     compile and execute a program
  run -c <code>    run inline A source (also: a -c <code>)
  build <file.a>   compile to a native binary (-o <bin>)
  check <file.a>   type-check without running (--json for structured output)
  test <file.a>    run every `fn test_*()` in a program
  fmt <file.a>     format a source file (prints to stdout)
  doc <file.a>     generate documentation (markdown, prints to stdout)
  ir <file.a>      print the typed SSA intermediate representation
  version | help

Options:
  -o <file>        output path (for 'build' and 'doc')
  -c <code>        run inline A source (for 'run', or alone: `a -c "..."`)
  -w, --write      rewrite the source file in place (for 'fmt')
  --check          exit nonzero if the file needs formatting (for 'fmt')
  --indent <n>     indentation width in spaces (for 'fmt', default 4)
  --json           emit structured JSON (for 'doc' and 'check')
  --opt            run optimization passes when printing IR (for 'ir')
  -I <dir>         add a module search directory (repeatable)
  --stdlib <dir>   override the standard library directory
  --no-color       disable colored diagnostics
```

### Formatter: `a fmt`

An AST-based formatter that normalises indentation, spacing, declarations, and
expressions while preserving comments, literal spelling, and semantics.

```bash
a fmt program.a             # print formatted source
a fmt --check program.a     # CI check: exit 1 when changes are needed
a fmt -w program.a          # rewrite in place
a fmt -w --indent 2 app.a   # choose indentation width
```

Formatting is idempotent, and a file with lexer/parser errors is never
rewritten. (Formatter lives in `compiler/astra/frontend/tooling/`, shared by `a fmt`
and the LSP.)

### Documentation generator: `a doc`

Generates API documentation from `##` doc comments and Python-style
docstrings attached to declarations, with Markdown and structured JSON
output.

```a
## Adds two numbers.
fn add(a: i32, b: i32) -> i32:
    return a + b
```

```bash
a doc geometry.a                    # Markdown to stdout
a doc -o docs/geometry.md geometry.a
a doc --json geometry.a             # machine-readable API model
```

Doc sources, in precedence order: `##` comment lines directly above a
declaration (consecutive lines form one doc block; trailing `##` comments
are ignored), a docstring as the first statement of a function body, a
docstring directly above a declaration, and docstrings inside class, enum,
and trait bodies (first documents the type/trait/enum itself, later ones
document the member that follows) or `impl` bodies (document the method
that follows). When both a `##` comment and a docstring are present, the
`##` comment wins.

Functions, classes, structs, metaclasses, enums, traits, type aliases,
fields, methods, variants, and module-level variables (rendered under
"Constants and variables") are included; `impl` blocks are parsed but not
emitted. Markdown output groups items into
Functions, Types, and Constants and variables sections with a table of
contents; `--json` emits `{module, path, doc, items[]}` where each item
carries `kind`, `name`, `signature`, `doc`, `returnType`, `params`,
`fields`, `variants`, and `methods`. Only lexing and parsing run — no type
checking — so `a doc` fails only on syntax errors. (Docgen lives in
`compiler/astra/frontend/tooling/`.)

---

## 🧩 Editor Support

A ships with first-class editor tooling built directly on the compiler front
end, so diagnostics and navigation match `a check`.

### Language server: `a-lsp`

`tools/lsp/` implements the Language Server Protocol over stdio
with no third-party dependencies.

| Capability | What it does |
| :--- | :--- |
| **Diagnostics** | Live lex, parse, and type errors with spans, messages, and compiler help text |
| **Go to definition** | Locals, parameters, functions, classes, fields, methods, enum variants, and imported module members |
| **Find references** | Every use of a symbol, including across imported modules |
| **Hover** | Signatures and types for symbols, plus documentation for builtins, native module members (`json.parse`), and dot methods (`"abc".strip`) |
| **Completion** | Keywords, builtins, visible bindings, module members after `.` — including native modules (`json.`) and the string/array/map/set method sets — plus `import`/`from … import` candidates; docs load on demand via `completionItem/resolve` |
| **Signature help** | Call signature with the active parameter highlighted for user functions, builtins, module members, and dot methods (triggered by `(` and `,`) |
| **Document symbols** | Outline of functions, classes, fields, methods, enums, variants, and top-level bindings |
| **Workspace symbols** | `Ctrl+T` / `Cmd+T` search across open A documents |
| **Formatting** | Full-document formatting through the same AST formatter as `a fmt` |
| **Rename** | Rename a symbol across its declaration and every resolved reference |
| **Document highlights** | Highlight every occurrence of the symbol under the cursor |
| **Folding** | Collapse function, class, loop, match, and conditional bodies |

Build it with the rest of the toolchain:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target a-lsp -j
./build/a-lsp --version
```

### VS Code extension

`tools/vscode-a/` adds TextMate syntax highlighting and connects VS Code to
`a-lsp`:

- Keywords (`var` `const` `pvt` `type` `fn` `class` `enum` `import` …),
  declarations, types (`Json`, fixed arrays, `T?` optionals), builtins, numbers,
  operators (`..`, `++`, `--`, `|>`, `??`, `?.`), and comments
- Strings with `{interpolation}`, raw `"""` strings, char literals, and escapes
- `#` line comments, bracket matching, auto-closing pairs, completion triggers
  (`.`, `"`, `(`, `,`), and indentation rules for colon-introduced blocks
- Diagnostics, go to definition (`F12`), references (`Shift+F12`), hover,
  completion with documentation on expand, signature help, document outline,
  and workspace symbols
- Format Document (`Shift+Alt+F`), Rename Symbol (`F2`), document
  highlights, and folding ranges

Try it in development:

```bash
# Open tools/vscode-a in VS Code / VS Code Insiders, then press F5.
```

Or install the prebuilt extension from the [GitHub
Release](https://github.com/ALangLabs/A/releases) assets:

```bash
# Use whichever CLI you have installed:
code --install-extension a-language-0.0.1.vsix
code-insiders --install-extension a-language-0.0.1.vsix
```

To package it from a checkout instead:

```bash
cd tools/vscode-a
npx --yes @vscode/vsce package
```

The extension finds `a-lsp` in this order:

1. The `a.server.path` setting
2. The `A_LSP_PATH` environment variable
3. `build/a-lsp` or `build-*/a-lsp` inside the workspace
4. `a-lsp` on `PATH`

Useful settings:

| Setting | Default | Purpose |
| :--- | :--- | :--- |
| `a.server.path` | `""` | Absolute path to `a-lsp` |
| `a.server.args` | `[]` | Extra server arguments, such as `["--stdio"]` |
| `a.server.modulePaths` | `[]` | Extra directories for `import` resolution |

Run **A: Restart Language Server** from the command palette after rebuilding
the server. See [`tools/lsp/README.md`](tools/lsp/README.md)
and [`tools/vscode-a/README.md`](tools/vscode-a/README.md) for details.

### Other editors

Any LSP client can use `a-lsp`; start it over stdio and send `file://` document
URIs. Set `A_LSP_LOG=1` for lifecycle logging on stderr.

---

## 🧪 Testing

Run the full test suite (language integration tests, tooling tests, formatter
and doc-generator CLI tests, plus the language server protocol smoke test):

```bash
ctest --test-dir build --output-on-failure
```

Or run the pieces directly:

```bash
tests/integration/run_tests.sh build/a
./build/a-tooling-tests
tests/tooling/run_cli_tests.sh build/a
python3 tools/lsp/smoke_test.py build/a-lsp
```

---

## 📜 License & Acknowledgments

Distributed under the MIT License. See [LICENSE](LICENSE) for details. Detailed specifications and compiler internals can be found in [`docs/`](docs/).
