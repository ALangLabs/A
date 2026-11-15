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
- `--native` — execution backend for `run`/`test`/`repl` (the only
  backend; there is no interpreter). Codegen lowers the checked AST to
  MLIR's llvm dialect and translates it to LLVM IR (`mlir-translate`),
  then compiles with `opt`/`llc`/`clang`. The direct LLVM-IR emitter is
  still reachable for debugging via the `A_BACKEND=llvm` environment
  variable — it is not exposed as a CLI flag.
- `--filter <substr>` — `a test` only: run just the `test_*` functions whose
  name contains the substring (e.g. `a test foo.astra --filter dict`, or
  `a test --filter dict` to select across discovered files). For a single
  file, a filter that matches no tests is an error; in directory scan mode,
  files with no matching tests are skipped and an error is reported only if
  no tests match anywhere.
- `--target <arch>` — the LLVM backend is host-only: the host arch is
  the only supported target today (`x86_64`, `aarch64`/`arm64`);
  non-host targets are rejected with a clear error. A lowers to LLVM IR
  (`frontend/llvm/`), then `opt -O<n>` → `llc -filetype=obj` → `clang`
  links against `libaruntime.a` (tools found via `A_LLVM_OPT`/`A_LLVM_LLC`/
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

Unsupported. The LLVM backend is host-only and there is no longer a
per-arch assembler backend, so `--target i8086` (aliases `i86`, `8086`,
`8088`) is rejected like every other non-host target. The hand-written
runtime subset remains in `src/astra/runtime/i8086_runtime.s` for
reference, and `tests/integration/i8086_image.sh` is inert.

Environment: `A_STDLIB`, `A_RUNTIME_C` (path to a_runtime.c overriding the
staged-runtime lookup),
`A_TARGET`, `A_LLVM_CC`/`A_LLVM_OPT`/`A_LLVM_LLC`, `A_DEBUG_SRC`,
`A_JOBS`,
`A_GC` (`0` disables the garbage collector), `A_GC_STATS` (`1` prints
per-collection stats to stderr), `A_GC_DEBUG` (`1` enables collector
stage markers).

## Cache

Compilation caching was removed — every `a run`/`a build`/`a test`
compiles fresh and writes nothing beside your sources. `A_SPLIT_UNITS=0`
still forces the single-unit compile path; `A_DEBUG_UNITS=1` prints
per-unit emit info. `A_NO_CACHE`, `A_CACHE_DIR`, and the `run-cache`
CTest entry no longer exist.

## Editor support

- `a-lsp` — stdio LSP server: diagnostics, hover, definition, references,
  rename, completion, signature help, formatting, semantic tokens, call/type
  hierarchy, inlay hints, code actions.
- `tools/vscode-a` — VS Code extension (syntax, snippets, tasks
  Run/Build/Test/Check/Format, check-on-save).
