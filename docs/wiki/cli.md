# CLI and tooling

The `a` binary (`./build/a`) is the single entry point. `a-lsp` is the
language server; the VS Code extension wraps both.

## Commands

| Command | Description |
| --- | --- |
| `a run <file.a>` | Compile and execute. `-c 'code'` runs inline source. |
| `a build <file.a> -o <bin>` | Compile to a native executable (default `a.out`). |
| `a check <file.a>` | Type-check only; `--json` for machine-readable diagnostics. |
| `a test <file.a>` | Run every `fn test_*()`. |
| `a fmt <file.a>` | Format to stdout; `-w` writes in place, `--check` exits nonzero if unformatted, `--indent <n>`. |
| `a doc <file.a>` | Generate docs (markdown; `--json`, `-o <file>`). |
| `a ir <file.a>` | Print the typed SSA IR; `--opt` or `-O*` runs optimization passes. |
| `a repl` | Interactive REPL — self-hosted A shell on a TTY, line REPL when piped. |
| `a version` / `a help` | Version and usage. |

## Options

- `-I <dir>` — module search directory (repeatable).
- `--stdlib <dir>` — override stdlib root (else `$A_STDLIB` / auto-detect).
- `-j <n>`, `--jobs <n>`, `--parallel` — parallel module checking.
- `-O0 -O1 -O2 -O3 -Os -Oz -Ofast`, `--opt-level=N`, `--optnone` — optimization.
- `--no-color` — plain diagnostics.
- `--target <arch>` — cross-compile target: `x86_64`
  (default; x86-64 assembly emitter on Linux/macOS SysV), `i386`, `arm`
  (ARMv7 hard-float), `aarch64`/`arm64`, `riscv64`, `i8086` (bare metal).
  An optional `-linux`/`-windows`/`-macos` suffix is accepted but must match
  the host OS — Astra drivers are built per-OS. x86-64 uses the dedicated
  `emitAsm` emitter; every other target uses its native per-arch emitter
  (`emitNative` in `codegen_native.cpp`, including Win64/MS-ABI for
  `x86_64-windows`), then calls the matching `astra-<arch>`
  driver; linking needs a target sysroot via
  `A_SYSROOT`/`A_SYSROOT_<ARCH>` (e.g. Debian `*-cross` packages extracted
  anywhere — note their `libc.so`/`libm.so` linker scripts use absolute
  paths that must be rewritten relative to the sysroot lib dir).
  There is no C emitter — every target compiles A → assembly → machine
  code. `A_CODEGEN=asm` forces the assembly emitter (a decline is a hard
  error); `A_CODEGEN=c` is rejected. `a run` executes natively
  (i8086 via qemu-system-i386 on Linux); `a test`/`a repl` reject non-host
  targets.

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
  `compiler/astra/runtime/i8086_runtime.s`: tagged-int (boxed-int)
  arithmetic and comparisons, decimal `print` of ints/strings/`none`/
  `true`/`false`, string literals + `+` concat via a bump-allocator heap,
  `len`/`str`/`int`/`bool`/`abs`/`range`/`repr`, collections
  (list/tuple/dict/set/range) with `[]` indexing and `in`, bitwise ops,
  `**`, and closure/call scaffolding. `print` writes to COM1 (0x3F8);
  remaining builtins (`a_getattr`/`a_method`/`a_slice`/f-string
  interpolation/`a_test_main`, …) are still stubs returning `none`.
- Known gaps: dict `d[k]` getitem, lambda/first-class-fn calls, list
  `print` repr, tuple iteration (type-checker rejects), methods/attribute
  access, slicing, and f-string interpolation are stubbed. Straight-line
  code, `while`/`if`/`for`, arithmetic, comparisons, strings, `len`,
  indexing, and collections are reliable; method/lambda/slice-heavy
  programs are not.
- Program exit writes 42 to port 0xF4 and halts. `a run --target i8086`
  wraps `qemu-system-i386 -serial stdio -device isa-debug-exit`;
  `tests/integration/i8086_image.sh` is the automated harness. Manual run:
  `qemu-system-i386 -drive format=raw,file=img -serial stdio -device isa-debug-exit,iobase=0xf4 -display none -no-reboot`
- The `astra-i8086` driver is built on Linux hosts only. The astra i8086
  backend is assembler-only, so if the native emitter declines a program
  the build fails at the astra step (there is no C fallback on any target).

Environment: `A_STDLIB`, `A_ASTRA`, `A_ASTRA_<ARCH>`, `A_RUNTIME_C`,
`A_SYSROOT`, `A_SYSROOT_<ARCH>`, `A_TARGET`, `A_CODEGEN` (`asm` to
force the assembly emitter; `c` is rejected — the C emitter was removed), `A_DEBUG_SRC`, `A_DEBUG_NATIVE`, `A_JOBS`,
`A_NO_CACHE` (disable the run cache), `A_CACHE_DIR` (run cache directory,
default `$XDG_CACHE_HOME/a/run` / `~/.cache/a/run` / `/tmp/a_cache_run`).

## Run cache

`a run` and the REPL reuse a content-addressed compiled binary across
invocations. The key hashes every module source, the runtime files,
the astra binary, target/opt level, codegen-forcing environment and
toolchain version, so any edit invalidates the entry. Repeat runs
cost ~2 ms (source validation) + binary exec instead of ~20 ms
(compile). Set `A_NO_CACHE=1` to always recompile. Covered by
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
