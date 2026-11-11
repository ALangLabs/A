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
| `a run <file.a>` | Compile and execute. `-c 'code'` runs inline source. |
| `a build <file.a> -o <bin>` | Compile to a native executable (default `a.out`). |
| `a check <file.a>` | Type-check only; `--json` for machine-readable diagnostics. |
| `a test [file\|dir]` | Run `fn test_*()` in a file, or recursively discover test files in a directory/codebase (default: `.`). `--filter <substr>` selects matching test names (in scan mode, files with no matches are skipped). |
| `a fmt <file.a>` | Format to stdout; `-w` writes in place, `--check` exits nonzero if unformatted, `--indent <n>`. |
| `a doc <file.a>` | Generate docs (markdown; `--json`, `-o <file>`). |
| `a ir <file.a>` | Print the typed SSA IR; `--opt` or `-O*` runs optimization passes. |
| `a repl` | Interactive REPL — self-hosted A shell on a TTY, line REPL when piped. |
| `a version` / `a help` | Version and usage. |

## Options

- `-I <dir>` — module search directory (repeatable).
- `--stdlib <dir>` — override stdlib root (else `$A_STDLIB` / auto-detect).
- `-j <n>`, `--jobs <n>`, `--parallel` — parallel module checking.
- `-O0 -O1 -O2 -O3 -Os -Oz -Ofast`, `--opt-level=N` (0..3; out-of-range
  values like `9` or `-1` are rejected), `--optnone` — optimization.
- `--no-color` — plain diagnostics.
- `--filter <substr>` — `a test` only: run just the `test_*` functions whose
  name contains the substring (e.g. `a test foo.a --filter dict`, or
  `a test --filter dict` to select across discovered files). For a single
  file, a filter that matches no tests is an error; in directory scan mode,
  files with no matching tests are skipped and an error is reported only if
  no tests match anywhere. The filter is part of the run-cache key, so
  different filters never share a cached binary.
- `--target <arch>` — cross-compile target: `x86_64`
  (default; x86-64 assembly emitter on Linux/macOS SysV, Win64 MS-ABI
  native emitter on Windows), `i386`, `arm`
  (ARMv7 hard-float), `aarch64`/`arm64`, `riscv64`, `i8086` (bare metal).
  An optional `-linux`/`-windows`/`-macos` suffix is accepted but must match
  the host OS — Astra drivers are built per-OS. Host OSes: Linux, macOS,
  and Windows (x86-64/arm64). The astra driver set differs per host:
  Linux builds `x86_64 i386 arm arm64 riscv64 i8086`, macOS `x86_64 arm64`,
  Windows `x86_64 i386 arm64`. x86-64 on Linux/macOS uses the dedicated
  `emitAsm` emitter; every other target/OS uses the native per-arch
  emitter (`emitNative` in `codegen_native.cpp`, including Win64/MS-ABI
  for `x86_64-windows`), then calls the matching `astra-<arch>`
  driver; linking needs a target sysroot via
  `A_SYSROOT`/`A_SYSROOT_<ARCH>` (e.g. Debian `*-cross` packages extracted
  anywhere — note their `libc.so`/`libm.so` linker scripts use absolute
  paths that must be rewritten relative to the sysroot lib dir).
  There is no C emitter — every target compiles A → assembly → machine
  code, and an emitter decline is a hard error. `A_CODEGEN=asm` forces the
  assembly emitter (a decline is a hard error); `A_CODEGEN=c` is rejected.
  `a run` executes natively
  (i8086 via qemu-system-i386 on Linux); `a test`/`a repl` reject non-host
  targets.

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
  `tests/integration/i8086_image.sh <path-to-a> <source.a> [expected.out]`
  builds an image and diffs the serial output against `expected.out`. Manual run:
  `qemu-system-i386 -drive format=raw,file=img -serial stdio -device isa-debug-exit,iobase=0xf4 -display none -no-reboot`
- The `astra-i8086` driver is built on Linux hosts only. The astra i8086
  backend is assembler-only, so if the native emitter declines a program
  the build fails at the astra step (there is no C fallback on any target).

Environment: `A_STDLIB`, `A_ASTRA`, `A_ASTRA_<ARCH>`, `A_RUNTIME_C`,
`A_SYSROOT`, `A_SYSROOT_<ARCH>`, `A_TARGET`, `A_CODEGEN` (`asm` to
force the assembly emitter; `c` is rejected — the C emitter was removed), `A_DEBUG_SRC`, `A_DEBUG_NATIVE`, `A_JOBS`,
`A_NO_CACHE` (disable caches), `A_CACHE_DIR` (override cache root for CI/tests;
by default caches live in `_acache_/` next to source files, like Python's
`__pycache__`),
`A_GC` (`0` disables the garbage collector), `A_GC_STATS` (`1` prints
per-collection stats to stderr), `A_GC_DEBUG` (`1` enables collector
stage markers).

## Cache (`_acache_`, `__pycache__`-like)

A stores compilation caches in `_acache_/` directories next to source files
(like Python's `__pycache__`): per-module compiled object/symbol artifacts for
incremental builds, plus content-addressed run/precheck binaries for `a run`,
`a build`, and `a test`. `A_CACHE_DIR` overrides the cache root for isolated
runs (CI/tests). `A_NO_CACHE=1` disables caching; `A_SPLIT_UNITS=0` forces the
single-unit compile path; `A_DEBUG_UNITS=1` prints per-unit emit/reuse.

### Run cache

`a run` and the REPL reuse a content-addressed compiled binary across
invocations. Two keys are involved:

- the **pre-parse key** lets a hit skip the frontend entirely. It follows
  the entry's imports textually through the same candidate tables the module
  loader uses (entry dir, `-I` dirs, `A_PATH`, the stdlib package layouts),
  hashing the content of every module reached and marking every candidate
  that was checked and absent, so creating a shadowing file changes the key.
  Only the real dependency graph is touched: a directory holding thousands of
  unrelated `.a` files, or `/tmp`, neither slows the check nor disables it.
  A module is re-hashed only while it is reachable, and imports the scanner
  cannot classify fall back to the frontend rather than trusting a partial key.
- the **binary key** hashes every module source, the runtime files, the astra
  binary, target/opt level, codegen-forcing environment and toolchain version.

A warm run costs one dependency check plus an `execve` of the cached binary.
On the development box that is ~10 ms end to end (`python3 -c pass` is ~20 ms);
`a build` produces a binary that starts in ~1 ms, and the difference is the
dependency stat/hash pass, which is what keeps `a run` honest after an edit.
Set `A_NO_CACHE=1` to always recompile. Covered by
`tests/integration/cache_test.sh` (the `run-cache` CTest entry).

`a run <file.a>` additionally skips the frontend on repeat runs via
a depfile-style precheck: a bounded stat snapshot of every `.a`
file under the entry dir, module search dirs, and the stdlib
(plus the per-string deps recorded on the first compile). On a hit
it execs the cached binary directly and replays the recorded
compile-time diagnostics (sidecar `.diag`), so warnings like unused
variables fire exactly as on a cold run. Any source, module, or
search-path change yields a fresh key.

## Editor support

- `a-lsp` — stdio LSP server: diagnostics, hover, definition, references,
  rename, completion, signature help, formatting, semantic tokens, call/type
  hierarchy, inlay hints, code actions.
- `tools/vscode-a` — VS Code extension (syntax, snippets, tasks
  Run/Build/Test/Check/Format, check-on-save).
