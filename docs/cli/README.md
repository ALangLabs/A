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
./build/tcc/astra -c /tmp/emitted.c -o /tmp/hello.o
./build/tcc/astra link /tmp/hello.o -o /tmp/hello-c

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
