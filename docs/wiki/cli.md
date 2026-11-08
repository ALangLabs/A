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
  `emitAsm` emitter; every other target tries its native per-arch emitter
  (`emitNative` in `codegen_native.cpp`) first and falls back to the
  portable C emitter (`emitC`), then calls the matching `astra-<arch>`
  driver; linking needs a target sysroot via
  `A_SYSROOT`/`A_SYSROOT_<ARCH>` (e.g. Debian `*-cross` packages extracted
  anywhere — note their `libc.so`/`libm.so` linker scripts use absolute
  paths that must be rewritten relative to the sysroot lib dir).
  `A_CODEGEN=asm`/`c` forces the emitter choice. `a run` executes natively
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
  arithmetic and comparisons plus decimal `print` of ints — `none`,
  `true`/`false` values work too. `print` writes to COM1
  (0x3F8); every other builtin is a stub returning `none` (non-int
  `print` args show `?`).
- Known gaps: **string literals are currently broken** — they read the
  wrong memory pending a linker address fix, so `print("hi")` yields
  `none`; do not rely on strings in i8086 programs. `while` loops,
  methods, and lambdas are also known-broken (the runtime stubs behind
  them return `none`/misbehave). Only straight-line tagged-int code is
  reliable today.
- Program exit writes 42 to port 0xF4 and halts. `a run --target i8086`
  wraps `qemu-system-i386 -serial stdio -device isa-debug-exit`;
  `tests/integration/i8086_image.sh` is the automated harness. Manual run:
  `qemu-system-i386 -drive format=raw,file=img -serial stdio -device isa-debug-exit,iobase=0xf4 -display none -no-reboot`
- The `astra-i8086` driver is built on Linux hosts only. The astra i8086
  backend is assembler-only, so if the native emitter declines a program
  the portable-C fallback cannot rescue it — the build fails at the astra
  step.

Environment: `A_STDLIB`, `A_ASTRA`, `A_ASTRA_<ARCH>`, `A_RUNTIME_C`,
`A_SYSROOT`, `A_SYSROOT_<ARCH>`, `A_TARGET`, `A_CODEGEN` (`asm`/`c` to
force an emitter), `A_DEBUG_SRC`, `A_DEBUG_NATIVE`, `A_JOBS`.

## Editor support

- `a-lsp` — stdio LSP server: diagnostics, hover, definition, references,
  rename, completion, signature help, formatting, semantic tokens, call/type
  hierarchy, inlay hints, code actions.
- `tools/vscode-a` — VS Code extension (syntax, snippets, tasks
  Run/Build/Test/Check/Format, check-on-save).
