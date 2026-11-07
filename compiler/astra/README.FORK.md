# Astra backend fork notes (`compiler/astra/backend/`)

`backend/` is the **Astra** C compiler — the sole native backend for A.
It is a fork of upstream TinyCC (Fabrice Bellard), rebranded and evolved
in-tree; there is no external gcc/clang/llc dependency.

## Role in A

- `A -> x86-64 asm (emitAsm) -> astra (+ a_runtime.c) -> native EXE` for `a build`
- `A -> x86-64 asm (emitAsm) -> astra (+ a_runtime.c) -> temp EXE -> run` for `a run`/`a test`
- Built once by `cmake/libastra.cmake` into the `astra` driver / `libastra` and the staged runtime/support archive (`build/astra/`, `build/astralib/`: headers, `libastra1.a`, `runmain.o`, staged `a_runtime.*`).

## Layout

- `astra.c` / `astra.h` — driver entry point and core state header.
- `libastra.c` / `libastra.h` — library API (`astra_*` functions, ONE_SOURCE
  unity build that `#include`s the other `.c` files).
- `astrapp.c`, `astragen.c`, `astradbg.c`, `astraasm.c`, `astraelf.c`,
  `astrarun.c`, `astracoff.c`, `astrape.c`, `astramacho.c`,
  `astratok.h`, `astratools.c`, `astralib.h` — preprocessor, code
generator, debugger hooks, assembler, and the ELF/COFF/PE/Mach-O
object backends.
- `<arch>-gen.c`, `<arch>-asm.c`, `<arch>-link.c`, `<arch>-tok.h` —
  per-target ports (x86-64, arm64, plus legacy i386/arm/riscv64/c67).
- `include/` — bundled C library headers staged for compiled programs.
- `lib/` — compiler support routines (`libastra1.a`) and helpers.
- `win32/` — Windows headers and support sources.
- `VERSION`, `COPYING`, `RELICENSING` — upstream version stamp and
  licenses (LGPL 2.1).

## Provenance

Derived from TinyCC (`release_0_9_26-2610` era). Upstream copyright and
the relicensing grant are preserved in `COPYING` / `RELICENSING` and in
the per-file headers. The public API surface is `libastra.h`
(`LIBASTRAAPI`, `ASTRAState`); internal identifiers use the `astra_` /
`ASTRA_` prefixes and `CONFIG_ASTRA_*` / `ASTRA_TARGET_*` defines.

## Local changes

- `libastra.c`: `astra_set_output_type` honors `nostdlib_paths` for Windows
  system directories and macOS SDK discovery, as it does for configured
  library paths. The embedded JIT supplies its runtime symbols and support
  archive itself.
- `arm64-gen.c`: use Apple's natural stack size and alignment for fixed
  floating-point arguments, including homogeneous float aggregates. This
  fixes a trailing float being passed at offset 16 instead of 12 after a
  spilled three-float struct. Variadic arguments retain eight-byte slots.
- `lib/libastra1.c`, `astragen.c`: round an unsigned 64-bit integer of 2^63
  or more to float or double once, at run time (`__floatundisf`,
  `__floatundidf`), and a 64-bit integer constant to float once when
  folding it. Both went through `long double`, which is no wider than
  `double` on Windows (and, for the folding, on Apple ARM64), so they
  rounded twice and could be an ulp off.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
```
