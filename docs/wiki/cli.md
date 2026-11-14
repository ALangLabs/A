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
- `--native` / `--backend llvm|native|asm` — execution backend for
  `run`/`test`/`repl`. Native is the only backend: A lowers to LLVM IR and
  compiles with `opt`/`llc`/`clang` (no interpreter).
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
  degrade. `i8086` is Linux-hosts-only.
- **Linux / macOS**: full feature set, including `i8086` (Linux only),
  Python interop, TLS/zlib, and the parallel runtime/test paths.

### `i8086` bare-metal target

`--target i8086` (aliases `i86`, `8086`, `8088`) emits a flat real-mode
boot image instead of an executable — no libc, no sysroot, no `a test`;
`a run` executes the image under qemu-system-i386. Details:

- The output is a BIOS boot sector at 0x7C00 with an `int 13h` CHS loader
  (AH=08h drive geometry + AH=02h reads) that pulls the remaining sectors
  (image padded to 512-byte sectors, 0x55AA signature, sector count at
  file offset 0x1F8) to 0x7E00+, then far-jumps to `_entry` at CS=0x1000.
- The emitted code uses 386+ operand-size prefixes in real mode. Real-mode
  CS:IP is 16-bit, so `.text` must fit a single 64 KiB segment at
  0x10000; the emitter dead-strips unreachable functions to stay under it.
- Runtime is the hand-written subset in
  `compiler/astra/runtime/i8086_runtime.s`. It handles tagged-int
  (boxed-int) arithmetic and comparisons, decimal `print` of
  ints/strings/`none`/`true`/`false`, string literals + `+` concat,
  `len`/`str`/`int`/`bool`/`abs`/`range`/`repr`, `[]` indexing (including
  range subscripts like `xs[1..4]`), bitwise ops, `**`, `while`/`if`/`for`
  over ints, and `fn(x) => …` lambdas called directly. `print` writes to
  COM1 (0x3F8); remaining builtins (`a_getattr`/`a_method`/`a_slice`/
  string interpolation/`a_test_main`, …) are stubs returning `none`.
- Known gaps: method/attribute access (`s.upper()`, `d.get(…)`) and dict
  `d[k]` read-back print nothing (stubs returning `none`); string
  interpolation `"{expr}"` and programs that mix heap-allocated strings
  with further loops wedge the image (kill the QEMU run, or use a shorter
  `timeout`); tuple iteration is rejected at code generation. The heap is a
  fixed 60 KiB bump arena with no collection, so keep images small.
  These were checked by actually running programs under
  `qemu-system-i386`, not only by compiling them.
- Program exit writes 42 to port 0xF4 and halts. `a run --target i8086`
  wraps `qemu-system-i386 -serial stdio -device isa-debug-exit`;
  `tests/integration/i8086_image.sh <path-to-a> <source.astra> [expected.out]`
  builds an image and diffs the serial output against `expected.out`. Manual run:
  `qemu-system-i386 -drive format=raw,file=img -serial stdio -device isa-debug-exit,iobase=0xf4 -display none -no-reboot`
- The `astra-i8086` driver is built on Linux hosts only. The astra i8086
  backend is assembler-only, so if the native emitter declines a program
  the build fails at the astra step (there is no C fallback on any target).

Environment: `A_STDLIB`, `A_ASTRA`, `A_ASTRA_<ARCH>`, `A_RUNTIME_C`,
`A_RUNTIME_CC_<ARCH>` (cross C compiler used at cmake time to build the
runtime archives; e.g. `A_RUNTIME_CC_ARM64=aarch64-linux-gnu-gcc`),
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
