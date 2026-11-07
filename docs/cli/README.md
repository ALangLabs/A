# A CLI Reference

The A toolchain provides the `a` CLI (`build/a`) and LSP server (`build/a-lsp`). A convenience wrapper `./ac` may run `a --backend=llvm` if present.

## Commands

```text
a <command> [options] <file.a>

repl             start the interactive REPL
run <file.a>     compile and execute a program
build <file.a>   compile to a native binary (-o <bin>)
check <file.a>   type-check without running
test <file.a>    run every top-level fn test_*()
fmt <file.a>     format source (stdout; -w to write)
doc <file.a>     generate documentation (markdown; --json)
ir <file.a>      print the typed SSA IR
version          print version
help             show help
```

## Options

| Option | Applies to | Description |
|---|---|---|
| `-o <file>` | `build`, `doc` | Output path (binary output name or doc output) |
| `-c <code>` | `run` (or alone) | Run inline A source instead of a file |
| `-w, --write` | `fmt` | Rewrite source in place |
| `--check` | `fmt` | Exit non-zero if file needs formatting |
| `--indent <n>` | `fmt` | Indentation width (default 4, range 1-16) |
| `--json` | `doc`, `check` | Structured JSON output |
| `--parallel` | `check` | Check modules in parallel (same diagnostics) |
| `-j <n>, --jobs <n>` | `check` | Parallel jobs (default: serial; `-j 0` = all cores) |
| `--opt` | `ir` | Run optimization passes before printing IR |
| `-O0`/`-O1`/`-O2`/`-O3`/`-Os`/`-Oz`/`-Ofast` | `run`, `build`, `ir` | LLVM/clang-style optimization level (for `ir` it selects the pass pipeline; for `run`/`build` it is forwarded to the Astra backend) |
| `--opt-level=N` | `run`, `build`, `ir` | Alias of `-O`, with `N` in 0..3 |
| `--optnone` | `run`, `build`, `ir` | Disable all optimizations (LLVM `optnone` attribute) |
| `-I <dir>` | module loading | Add module search directory (repeatable) |
| `--stdlib <dir>` | module loading | Override standard library directory |
| `--backend <name>` / `--backend=<name>` | compatibility | Accepted for compatibility; A v2 uses the Astra backend |
| `--no-color` | diagnostics | Disable colored diagnostics |
| `-h, --help` | global | Show help |
| `-V, --version` | global | Print version |

## Examples

```sh
# Build toolchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j

# Run / check / test
./build/a run examples/basics/hello.a
./build/a check examples/classes/point.a
./build/a test examples/advanced/testing.a

# Build native binary via Astra (compiler/astra/)
./build/a build examples/basics/hello.a -o /tmp/hello-a
/tmp/hello-a

# The astra driver is a C compiler; it consumes the C emitted by the
# frontend (via `a build`), not .a sources directly:
./build/astra/astra -c /tmp/emitted.c -o /tmp/hello.o
./build/astra/astra link /tmp/hello.o -o /tmp/hello-c

# Print typed SSA IR
./build/a ir examples/basics/hello.a
./build/a ir --opt examples/basics/hello.a

# Format
./build/a fmt examples/basics/hello.a
./build/a fmt --check examples/basics/hello.a
./build/a fmt -w examples/basics/hello.a

# Generate docs
./build/a doc examples/classes/point.a
./build/a doc --json examples/classes/point.a

# Module/import search controls
A_STDLIB=/path/to/stdlib ./build/a run main.a
./build/a run -I ./lib -I ./vendor main.a
./build/a run --stdlib ./stdlib main.a
```

## `a doc` — documentation generator

`a doc <file.a>` extracts documentation and prints Markdown to stdout;
`--json` prints a structured JSON document instead, and `-o <file>` writes
either format to a file. Only lexing and parsing run (no type checking), so
the command fails only on syntax errors.

**Doc sources.** For each documented declaration, the doc text is picked in
this order:

1. `##` comment lines directly above the declaration. Consecutive `##`
   lines form a single doc block; `##` comments trailing code on the same
   line are ignored. `##` comment blocks before the first declaration that
   are not attached to it (e.g. followed by a blank line) are joined into
   the module doc.
2. A docstring (bare string literal, typically `"""..."""`) as the first
   statement of a function body.
3. A docstring recorded by the parser directly above the declaration —
   inside `class`/`struct`, `enum`, and `trait` bodies the first string
   literal documents the type/trait/enum itself and later ones document the
   member that follows; inside `impl` bodies a docstring documents the
   method that follows it.
4. A docstring statement directly above a top-level declaration (documents
   the module when it is the first statement of the file; for the module
   doc, detached `##` blocks take precedence over the docstring).

**Coverage.** Top-level functions, classes/structs/metaclasses (with fields
and methods), enums (with variants), traits (with signatures), type aliases,
and module-level `let` variables. `impl` blocks are parsed — including any
docstrings they contain — but are not emitted as documentation items.

**Markdown output** has a `# <module>` title, optional module doc, a table of
contents, and `## Functions` / `## Types` / `## Constants and variables`
sections; each item shows its signature in a fenced `a` code block plus its
doc, parameters, fields/variants, and methods.

**JSON output** is a single object: `{"module", "path", "doc", "items":[…]}`.
Each item has `kind` (`function`, `class`, `struct`, `metaclass`, `enum`,
`trait`, `type`, or `variable`), `name`, `signature`, `doc`,
`returnType`, `private`, `params` (`{name, type}`), `fields`, `variants`,
and nested `methods` (same item shape).

```sh
./build/a doc examples/classes/point.a
./build/a doc -o docs/point.md examples/classes/point.a
./build/a doc --json examples/classes/point.a
```

## Exit codes (integration harness conventions)

The integration test harness uses:

- `.a` — test case source
- `.out` — expected stdout
- `.err` — expected stderr substrings
- `.exit` — expected exit code
- `.in` — stdin input
- `.flags` — command/flags override (e.g. `check`, `test`)

If no `.exit` is present, the harness defaults to exit `0` unless the case is configured otherwise; `.err` alone does not imply a non-zero exit without `.exit`.

## LSP

The language server binary is `build/a-lsp` (stdio). Use `python3 tools/lsp/smoke_test.py build/a-lsp` for the smoke test.
