<div align="center">

# A

**A fast, batteries-included programming language and toolchain** — lexer, parser,
type checker, ownership analysis, SSA optimizer, and a TinyCC-derived backend that
compiles A straight to native machine code.

[![CI](https://github.com/ALangLabs/A/actions/workflows/ci.yml/badge.svg)](https://github.com/ALangLabs/A/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.0.1-informational.svg)](#)
[![Platform](https://img.shields.io/badge/platform-linux%20%7C%20macOS%20%7C%20windows-lightgrey.svg)](#)

[Features](#-features) • [Quick start](#-quick-start) • [The language](#-the-language) • [Toolchain](#-toolchain) • [Architecture](#-architecture) • [Documentation](#-documentation) • [Development](#-development)

</div>

---

## ✨ Features

- **Native compilation, no runtime VM** — `a build` lowers A to machine code
  and links a real executable through the bundled **Astra** backend (a TinyCC
  fork). `a run` does the same and executes it. x86-64 SysV (Linux, macOS)
  uses a dedicated x86-64 assembly emitter; x86-64 Windows uses a Win64
  MS-ABI native emitter; every other target tries its own native assembly
  emitter first (a declined construct is a hard error — there is no C
  fallback).
- **Multi-target builds** — `a build --target <arch>` cross-compiles for
  i386, ARM (ARMv7), AArch64 (`aarch64`/`arm64`) and RISC-V64 on the host OS,
  given a matching target sysroot (see *Cross-compilation* below), plus the
  bare-metal 16-bit `i8086` target.
- **Static type checking with inference** — Hindley-Milner-style unification,
  generics, traits, algebraic enums, optional/result types, and flow-sensitive
  narrowing (`if x != None`).
- **Ownership & borrowing** — move semantics, `borrow`/`borrow_mut`, and
  compile-time view (slice) tracking, checked after type inference.
- **Rich standard library** — collections, math, JSON, binary codec, filesystem,
  HTTP/net, logging, metrics, tracing, terminal, time, testing, and more —
  A packages under [`stdlib/`](stdlib/) plus native runtime modules, reached
  through `import std:<pkg>`.
- **Python interop** — `import python:<pkg>` calls into CPython at run time via
  a `dlopen`'d libpython bridge (NumPy, PyTorch, …), including zero-copy
  buffer-protocol views.
- **Native ML kernels** — Mojo-inspired kernels (`matmul` block-tiled, fused
  `attention`/`softmax`, FMA hot loops, and an IR `fuseFma` contraction pass).
- **First-class tooling** — `a fmt`, `a doc`, `a ir`, a REPL, a full **LSP**
  server (`a-lsp`), and a VS Code extension.
- **Package manager** — `apm` (`apm/apm`), `pip` for A: install, update,
  publish, and run third-party packages written in A itself, with a
  registry index, `apm.json` manifests, and `apm.lock.json` pinning.
  See [`apm/`](apm/README.md).
- **LLVM-style optimization pipeline** — constant folding, DCE, inlining, LICM,
  CSE, instcombine, SCCP, and `-O0`…`-Ofast` levels over a typed SSA IR.

## 🚀 Quick start

```sh
# Build the toolchain (produces build/a and build/a-lsp)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j

# Compile and run a program
./build/a run examples/basics/hello.a

# Type-check without running
./build/a check path/to/file.a

# Produce a native binary
./build/a build path/to/file.a -o myapp
```

`cmake --build build -j` builds everything in `all`, including `a-lsp`
(use `--target a` or `--target a-lsp` to build just one binary). Link-time
optimization is **off by default** so dev builds stay fast; for release
builds add `-DA_ENABLE_LTO=ON` to the configure step.

Or use the helper script: `tooling/build/build.sh`.

### The `a` CLI

| Command | Description |
| --- | --- |
| `a run <file.a>` | Compile and execute (`-c 'code'` for inline source) |
| `a build <file.a> -o <bin>` | Compile to a native executable |
| `a check <file.a>` | Type-check only (`--json` for machine-readable diagnostics) |
| `a test <file.a>` | Discover and run every `fn test_*()` |
| `a fmt <file.a>` | Format source (`-w` in place, `--check` for CI) |
| `a doc <file.a>` | Generate documentation (`--json` supported) |
| `a ir <file.a>` | Print the typed SSA IR (`--opt` / `-O*` to optimize) |
| `a repl` | Interactive REPL (self-hosted shell on a TTY) |

Common flags: `-I <dir>` module search path, `--stdlib <dir>`, `-j/--jobs`,
`-O0/-O1/-O2/-O3/-Os/-Oz/-Ofast`, `--optnone`, `--no-color`.

## 📝 The language

A is a statically typed, indentation-sensitive language with Python-like
syntax and native performance. A quick tour:

```python
import std:math

class Point:
    x: float
    y: float

    fn length(self) -> float:
        return math.sqrt(self.x * self.x + self.y * self.y)

fn fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

fn main():
    p = Point(3.0, 4.0)
    print("length:", p.length())          # 5.0
    print("fib(20):", fib(20))            # 6765
    print("squares:", [x * x for x in 0..5])
```

`fn main()` is invoked automatically once the entry file's top-level statements
finish — no explicit `main()` call needed. It runs only for the file you pass
to `a run`/`a build`, never for imported modules, and is skipped by `a test`
(which discovers `fn test_*()` instead). Top-level statements still run on
import, so keep library modules declaration-only; put program logic in
`fn main()`.

Highlights: `class` types with fields, methods, and inheritance, `enum` (plain and
algebraic), `trait` + `impl` conformance, generics and `comptime` params,
closures/lambdas, comprehensions, `match` expressions, `?` optionals and
`Result`, slices/views, `defer`, destructuring, string interpolation, and
`#` comments and `"""..."""` docstrings.

## 🧰 Toolchain

```mermaid
flowchart LR
    src[".a source"] --> fe["C++ frontend<br/>lex · parse · resolve · sema<br/>ownership · IR · codegen"]
    fe --> s["x86-64 / per-arch asm"]
    s --> astra["Astra backend<br/>(TinyCC fork)"]
    rt["A runtime (C)<br/>aval values · collections · builtins"] --> astra
    astra --> bin["native executable"]
    bin --> out["stdout / exit code"]
```

- **`compiler/astra/frontend/`** — the C++20 frontend (`namespace a`): lexer,
  parser, AST, diagnostics, module loader, `Sema` type checker, ownership
  analysis, typed SSA IR + optimizer, and the native code emitters
  (`emitAsm` for x86-64 SysV, `emitNative` per-arch incl. the Win64 MS ABI
  for `x86_64-windows`).
- **`compiler/astra/backend/`** — **Astra**, a TinyCC fork: the `astra` driver,
  the `libastra` library API, per-target code generators (x86-64, ARM, AArch64,
  RISC-V, i386, C67x), ELF/Mach-O/COFF writers, and support libraries.
- **`compiler/astra/runtime/`** — the C runtime every A program links:
  NaN-boxed `aval` values, collections, builtins, and native modules
  (json, fs, net, arena, unsafe, python interop, terminal, time, intrinsics).
- **`tools/lsp/`** — `a-lsp`, a full stdio language server (diagnostics,
  hover, go-to-definition, completion, formatting, call/type hierarchy, …).
- **`apm/`** — `apm`, the package manager: registry index, dependency
  resolver, installer (local/git/http sources), lockfile, and the
  `apm` launcher script.
- **`tools/vscode-a/`** — VS Code extension with syntax highlighting,
  snippets, tasks, and the LSP client.

## 🎯 Targets & cross-compilation

`a build --target <arch>` selects the output architecture (the OS always
matches the host — Astra drivers are built per-OS and cannot cross
operating systems):

| Target | Backend | Status |
|---|---|---|
| `x86_64` (default) | x86-64 asm emitter → `astra` | native, fully tested |
| `i386` | native i386 asm emitter → `astra-i386` | verified (runs natively on x86-64) |
| `arm` (ARMv7 hard-float) | native ARM asm emitter → `astra-arm` | verified under qemu-armhf |
| `aarch64` / `arm64` | native AArch64 asm emitter → `astra-arm64` | needs AArch64 sysroot |
| `riscv64` | native RISC-V64 asm emitter → `astra-riscv64` | needs RISC-V64 sysroot |
| `i8086` / `i86` / `i8088` | native i8086 asm emitter, bare metal → flat boot image | Linux hosts only; no libc/sysroot; `a run` via qemu-system-i386, `a test` unsupported |

`a run`, `a test` and `a repl` execute natively and reject non-host
targets — the one exception is `a run --target i8086` on Linux, which runs
the boot image under QEMU (see below). Linking a cross binary requires the
target C library: install the matching cross toolchain
(e.g. `gcc-aarch64-linux-gnu`, `libc6-dev-i386`)
or point `A_SYSROOT` / `A_SYSROOT_<ARCH>` at a sysroot root. Per-arch Astra
drivers and staged lib dirs are produced by the build (`build/astra/astra-<arch>`,
`build/astralib-<arch>/`); `A_ASTRA_<ARCH>` overrides the driver lookup.

Not supported: RISC-V32 (no astra backend; the runtime value model is
64-bit). 16-bit is `i8086` only — a bare-metal real-mode target (the
emitted code uses 386+ operand-size prefixes despite the name) with no
libc and no sysroot, built by the per-arch `astra-i8086` assembler/linker
(asm-only; there is no i8086 C backend). `a build --target i8086 -o img`
produces a flat boot image (`-oformat=binary`, entry at 0x7C00): sector 0
contains an `int 13h` CHS loader (AH=08h drive geometry + AH=02h reads) that
pulls the remaining 512-byte sectors to 0x7E00+, records the sector count at
file offset 0x1F8, and stamps the 0x55AA signature; it then far-jumps to
`_entry` at CS=0x1000. `print` writes to COM1 (0x3F8), program exit writes
42 to port 0xF4 and halts. The hand-written runtime
(`compiler/astra/runtime/i8086_runtime.s`) is a partial subset. It handles
tagged-int arithmetic/comparisons, `print` of ints/strings/`none`/`true`/
`false`, string literals and `+` concatenation, `len`/`str`/`int`/`bool`/
`abs`/`range`/`repr`, `[]` indexing (including ranges like `xs[1..4]`),
`if`/`while`/`for` over ints, and `fn(x) => …` lambdas called directly;
other builtins are stubs returning `none`. Real-mode CS:IP is 16-bit, so
`.text` must fit in a single 64 KiB code segment at 0x10000 — the emitter
dead-strips unreachable functions to stay under it. The astra i8086 driver
is built on Linux hosts only. Known gaps: method/attribute access
(`s.upper()`, `d.get(…)`) and dict `d[k]` read-back are stubs that print
nothing, string interpolation `"{expr}"` and programs that mix
heap-allocated strings with further loops wedge the image until QEMU is
killed, and tuple iteration is rejected at code generation. The runtime
heap is a fixed 60 KiB bump arena with no collection, so keep programs
small. Those claims were checked by running programs under
`qemu-system-i386`.
`a run --target i8086` builds
the image and executes it under `qemu-system-i386` (COM1 on stdio,
isa-debug-exit on port 0xF4); `a test`/`a repl` still reject it.
`tests/integration/i8086_image.sh` is the automated harness — it builds an
image and diffs serial output against an expected `.out` file. To run an
image manually:
`qemu-system-i386 -drive format=raw,file=img -serial stdio -device isa-debug-exit,iobase=0xf4 -display none -no-reboot`
Windows is a supported host OS (x86-64 and arm64): the driver pipeline has
`_WIN32` ports (`CreateProcess`, `GetTempPath`, `GetModuleFileName` in
`pipeline.cpp`), the Win64 MS-ABI native emitter covers `x86_64`, and the
Windows astra drivers (`x86_64`, `i386`, `arm64`) link PE32+ executables —
`a run`/`a test`/`a build` work, with `a test` taking the serial path (no
fork-based batching) and the runtime's parallel pool disabled. Runtime
caveats on Windows: TCP sockets work via Winsock2, but TLS/zlib stay
unavailable (no `dlopen`), `import python:<pkg>` interop is stubbed out,
and POSIX-only pieces like `terminal` raw mode degrade gracefully. The
`i8086` target remains Linux-hosts-only, and Windows builds only the
`x86_64`/`i386`/`arm64` astra drivers — no `arm`, `riscv64`, or `i8086`.
Debian `*-cross`
sysroots ship `libc.so`/`libm.so` linker scripts with absolute paths
astra cannot resolve — rewrite the `GROUP` entries relative to the sysroot
lib dir when setting one up.

`tests/integration/cross_suite.sh <arch> <sysroot-env> <sysroot> [runner]`
cross-compiles every runnable integration case for a target and diffs
program output — verified 70/70 on i386 (native), arm, aarch64 and riscv64
(under qemu-user).

## 📚 Documentation

- **[Technical wiki](docs/wiki/index.md)** — deep-dive pages for every
  subsystem: frontend stages, backend, runtime, stdlib, tooling,
  tests, and examples.
- **[`examples/`](examples/)** — runnable programs by category (basics,
  control flow, classes, enums, modules, stdlib, Python interop, …).
- **[`stdlib/std/`](stdlib/std/)** — standard library packages.
- **[`tests/integration/cases/`](tests/integration/cases/)** — golden test
  suite doubles as feature documentation.

## 🧪 Development

```sh
# Full build + test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# Release/perf build: LTO is off by default (faster dev builds) — opt in with
#   -DA_ENABLE_LTO=ON on the configure step.

# Targeted checks
tests/integration/run_tests.sh build/a          # golden suite only
python3 tools/lsp/smoke_test.py build/a-lsp     # LSP smoke test
```

CI (`.github/workflows/ci.yml`) builds with warnings-as-errors, verifies
version consistency across all components, runs the full test suite, and runs
an ASan/UBSan pass.

Conventions: C++20 in `namespace a`, headers next to implementations,
conventional-commit style (`fix(sema): …`, `feat(lsp): …`), and version bumps
must update `CMakeLists.txt`, `frontend/driver/version.hpp`,
`tools/lsp/server.cpp`, and `tools/vscode-a/package.json` together. See
[`AGENTS.md`](AGENTS.md) for the full contributor guide.

## 📄 License

[MIT](LICENSE) © 2026 OEvortex / ALangLabs.
