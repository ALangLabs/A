# CLI and tooling

The `a` binary (`./build/a`) is the single entry point. `a-lsp` is the
language server; the VS Code extension wraps both. Both are built by the
default `cmake --build build` (`all` target) — `--target a` builds only
`a`, `--target a-lsp` only the server. CMake option `A_ENABLE_LTO=ON`
enables link-time optimization for Release builds (off by default for
faster dev builds).

## Commands

| Command | Description |
| --- | --- |
| `a run <file.astra>` | Compile and execute. `-c 'code'` runs inline source. |
| `a build <file.astra> -o <bin>` | Compile to a native executable (default `a.out`). |
| `a check <file.astra>` | Type-check only; `--json` for machine-readable diagnostics. |
| `a test [file\|dir]` | Run `fn test_*()` in a file, or recursively discover test files in a directory/codebase (default: `.`). `--filter <substr>` selects matching test names (in scan mode, files with no matches are skipped). |
| `a fmt <file.astra>` | Format to stdout; `-w` writes in place, `--check` exits nonzero if unformatted, `--indent <n>`. |
| `a doc <file.astra>` | Generate docs (markdown; `--json`, `-o <file>`). |
| `a repl` | Interactive REPL — self-hosted A shell on a TTY, line REPL when piped. |
| `a version` / `a help` | Version and usage. |

## Options

- `-I <dir>` — module search directory (repeatable).
- `--stdlib <dir>` — override stdlib root (else `$A_STDLIB` / auto-detect).
- `-j <n>`, `--jobs <n>`, `--parallel` — parallel module checking.
- `-O0 -O1 -O2 -O3 -Os -Oz -Ofast`, `--opt-level=N` (0..3; out-of-range
  values like `9` or `-1` are rejected), `--optnone` — optimization.
- `--no-color` — plain diagnostics.
- Execution backend — `run`/`test`/`repl` always compile natively (the
  only backend; there is no interpreter). Codegen lowers the checked AST to
  MLIR and compiles it **in-process** by default
  (parse -> canonicalize -> LLVM IR -> `llvm::PassBuilder` opt ->
  `TargetMachine` -> `.o` -> clang link with `libaruntime.a`). The
  `mlir-translate`/`opt`/`llc` subprocess chain remains as an escape
  hatch: `A_BACKEND=subprocess` (aliases `mlir-subprocess`,
  `llvm-subprocess`, `tools`) or `A_MLIR_NATIVE=0`. Scalar values are emitted unboxed
  (`i64`/`f64`/`i1`) wherever Sema proves Int/Float/Bool types; set
  `A_MLIR_TYPED=0` for the boxed-aval emitter. On non-Windows hosts a
  sibling
  `.ambc`/`.ambc.d` package written by `a precompile` is used
  automatically (`A_AMBC=0` disables). Packages record source mtimes
  (`src` records in `ambc.meta`, `<name>.ambc.srcs` for whole-graph
  packages): a changed or missing source falls through to the normal
  pipeline automatically. Packages written before staleness tracking
  (no `src` records) are still trusted unconditionally. `a precompile`
  follows `A_MLIR_TYPED` too: typed `a`-dialect units by default,
  boxed llvm-dialect units with `A_MLIR_TYPED=0`.
- `--filter <substr>` — `a test` only: run just the `test_*` functions whose
  name contains the substring (e.g. `a test foo.astra --filter dict`, or
  `a test --filter dict` to select across discovered files). For a single
  file, a filter that matches no tests is an error; in directory scan mode,
  files with no matching tests are skipped and an error is reported only if
  no tests match anywhere.
- `--target <arch>` — the LLVM backend is host-only: the host arch is
  the only supported target today (`x86_64`, `aarch64`/`arm64`);
  non-host targets are rejected with a clear error. A lowers to MLIR's
  llvm dialect (`frontend/mlir/`), compiled in-process by default and
  linked against `libaruntime.a` via `clang` (the subprocess fallback
  uses `mlir-translate` + `opt -O<n>` → `llc -filetype=obj` → `clang`,
  tools found via `A_MLIR_TRANSLATE`/`A_LLVM_OPT`/`A_LLVM_LLC`/
  `A_LLVM_CC` or PATH). `a run` executes natively; `a test`/`a repl`
  require the host target.

### Host platform notes

- **Windows**: `a run`/`a build`/`a test` produce and run PE32+
  executables; `a test` uses the serial path (no fork batching) and the
  runtime thread pool is disabled. Winsock2 provides plain TCP, but TLS
  and zlib stay unavailable (no `dlopen`), `import python:<pkg>` interop
  is stubbed out, and POSIX-only facilities (e.g. `terminal` raw mode)
  degrade.
- **Linux / macOS**: full feature set, including Python interop, TLS/zlib,
  and the parallel runtime/test paths.

### `i8086` bare-metal target

Unsupported. The backend is host-only and there is no longer a
per-arch assembler backend, so `--target i8086` (aliases `i86`, `8086`,
`8088`) is rejected like every other non-host target;
`tests/integration/i8086_image.sh` is inert.

Environment: `A_STDLIB`, `A_RUNTIME_C` (path to a_runtime.c overriding the
staged-runtime lookup),
`A_TARGET`, `A_LLVM_CC`/`A_LLVM_OPT`/`A_LLVM_LLC`, `A_DEBUG_SRC`,
`A_JOBS`,
`A_GC` (`0` disables the garbage collector), `A_GC_STATS` (`1` prints
per-collection stats to stderr), `A_GC_DEBUG` (`1` enables collector
stage markers).

## Cache

`a run`/`a build`/`a test` use content-addressed run caching, on by
default: the compiled binary is stored under a central cache root
(`$A_CACHE_DIR` when set, else the platform user-cache directory) and
reused when the module contents, compiler version, backend flags, and
LLVM tool stats are unchanged. `A_NO_CACHE=1` disables caching; it
applies only to the host target and nothing is written beside your
sources. The per-module object cache (`A_SPLIT_UNITS`/`A_DEBUG_UNITS`,
the former `objcache` split-unit path) was removed — those variables
have no effect.

## Editor support

- `a-lsp` — stdio LSP server: diagnostics, hover, definition, references,
  rename, completion, signature help, formatting, semantic tokens, call/type
  hierarchy, inlay hints, code actions.
- `tools/vscode-a` — VS Code extension (syntax, snippets, tasks
  Run/Build/Test/Check/Format, check-on-save).
