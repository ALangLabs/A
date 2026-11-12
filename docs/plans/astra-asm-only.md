# Plan: make astra an asm/link-only backend (no C compiler)

> **Status (implemented):** Phase 1 done; Phase 2 done *functionally* —
> `astra_compile` rejects all non-asm input, so no entry point can compile
> C. The TCC-derived C frontend files (`astragen.c`, `*-gen.c`) were kept
> because they also contain infrastructure the assembler/linker shares
> (symbol table, `expr_const`, `greloca`, per-arch emission helpers);
> physical removal is a follow-up extraction, not required for correctness.
> Phase 3 docs done; CI needs no change (host-only builds).

Goal: astra stops being a TinyCC fork that compiles C. It becomes the
assembler + linker + runtime-packaging driver for A's emitted `.s`/`.S`
files. `A → asm` stays in the C++ frontend; `asm → machine code` stays in
astra. Nothing in the product path compiles C anymore.

## Why the C frontend exists today (must be replaced first)

1. `compiler/astra/runtime/*.c` (~40 files) — the A runtime is C.
   - Host: prebuilt to `libaruntime.a` by **system cc**
     (`A_RUNTIME_SYSTEM_CC=ON`, `cmake/libastra.cmake:259`).
   - Cross: **no archive is built** (`libastra.cmake:576`); the `a`
     pipeline compiles every runtime `.c` with `astra-<arch>` per program
     (`pipeline.cpp:1752-1787`), caching `.o`s in the staged dir.
2. `backend/lib/*.c`, `lib/runmain.c`, `win32/lib/*.c` — compiled into
   `libastra1-<arch>.a` / `runmain.o` by `astra-<arch> -c`
   (`libastra.cmake:545-573`).
3. `A_RUNTIME_SYSTEM_CC=OFF` builds and the pipeline's last-resort
   `rtSrcs` path also use astra's C gen.

## Phase 1 — runtime objects via a real C compiler (cmake + pipeline)

- `cmake/libastra.cmake`
  - Host (Linux/macOS/Windows): keep system-cc path (already default).
  - Cross archs: detect a C compiler for the target, in order:
    `<triplet>-gcc` (`arm-linux-gnueabihf-gcc`, `aarch64-linux-gnu-gcc`,
    `riscv64-linux-gnu-gcc`, `i686-linux-gnu-gcc`), `clang`/`clang-cl`
    with `--target`, `x86_64-w64-mingw32-gcc` for PE targets, or
    `A_RUNTIME_CC_<ARCH>` env override. Build `libaruntime-<arch>.a`
    (runtime/*.c, `-O2`, sysroot includes) and the `libastra1-<arch>.a`
    objects + `runmain.o` with it, staged in `astralib-<arch>/`.
  - If no cross cc is found: `message(WARNING)` at configure time and
    skip the archives; `a build --target` then fails with a clear
    "install <triplet>-gcc" error (see Phase 2).
- `compiler/astra/frontend/pipeline/pipeline.cpp`
  - Delete `buildRuntimeArchiveCC`, the `rtSrcs` directory scan, and the
    per-program `.c` compile/`-ar` fallback in `astraBuild` and
    `prepareAstraPlan`. Always link the staged
    `libaruntime-<arch>.a`; missing archive → hard error naming the
    required toolchain.
  - Drop now-unused `A_RUNTIME_C`/`A_RUNTIME_CC`/`A_RUNTIME_CFLAGS`
    handling (or keep parsing for cache-key stability — decide in review).
  - Net effect: ~120 lines removed, cross builds get *faster* (archive
    link instead of ~40 C compiles per program).

## Phase 2 — excise the C frontend from astra

Delete (dead once `astragen.c` is gone — verified: only `astragen.c`
calls into `*-gen.c`):

- `astragen.c` (~8.8k lines: C parser/sema/codegen)
- `x86_64-gen.c`, `i386-gen.c`, `arm-gen.c`, `arm64-gen.c`,
  `riscv64-gen.c`, `i8086-gen.c` (~11k lines: C-AST→machine emitters)

Keep (the actual product):

- `astraasm.c`, `*-asm.c`, `*-asm.h`, `*-tok.h` — assembler
- `astraelf.c`, `*-link.c`, `astrape.c` (PE), `astramacho.c` — linker
- `astrapp.c` + `astratok.h` — tokenizer/preprocessor; **still required**
  (`.S` files and `do_preprocess` path use `astra_preprocess`,
  `begin_macro`, `astrapp_putfile`). C-only bits (`parse_define`,
  `expr_preprocess`, C `#` semantics) can be trimmed in a follow-up —
  not required for correctness.
- `astrarun.c` (`-run`), `astradbg.c`, `astratools.c` (`ar`/`impdef`),
  `libastra.c/h`, `astra.h`, `elf.h`, `dwarf.h`, `stab.h`, `lib/`,
  `include/`, `win32/`

Edit:

- `libastra.c`: remove `#include "astragen.c"` and the
  `astragen_init/compile/finish` calls in the compile loop
  (`libastra.c:825-838`); `.c` input → clean error
  ("astra assembles .s/.S and links objects; C compilation removed").
  Keep `.s`/`.S`/`.o`/`.a` paths.
- `astra.c`: help text — drop `-std=`, `-E` (or make `-E` an error),
  C-warning flags; keep asm/link/run/ar options.
- `astra.h`: prune `astragen_*` prototypes and gen-only decls. Leave
  `CType`/token machinery for now (dead types are harmless; a second
  pass can slim it).
- `cmake/libastra.cmake` + host `libastra` target: drop `*-gen.c` from
  sources/ONE_SOURCE includes.

## Phase 3 — docs + cleanup

- `AGENTS.md`: astra description → "assembler+linker driver; no C
  frontend"; runtime objects built by system/cross cc at build time.
- `docs/wiki/cli.md` / compiler docs: `A_RUNTIME_*` env changes.
- `.github/workflows/ci.yml`: ensure a cross cc is installed for the
  cross-target test legs (or mark them conditional).
- `tooling/release/package.sh`: ship `libaruntime-<arch>.a` per target.

## Verification

- `cmake --build build` + `ctest` + `tests/integration/cache_test.sh`
- `for f in examples/*/*.a; do ./build/a run "$f"; done`
- `a build --target x86_64` smoke; cross legs need the detected cc.
- `astra file.c` → clean error; `astra -c file.s` still works.

## Risks / open questions

- **Cross toolchain requirement is new.** Today astra self-hosts runtime
  objects for any arch; after this, `--target arm64` needs
  `aarch64-linux-gnu-gcc` (+ sysroot, which was already required).
  CI must install it or skip cross legs.
- **Windows/PE targets**: `win32/lib/*.c` currently compiled by astra
  with bundled mingw headers. Needs `x86_64-w64-mingw32-gcc` (or
  clang-cl on Windows hosts). Biggest portability gap — may need a
  staged prebuilt-object fallback.
- **macOS cross** (e.g. Linux→arm64-apple): needs osxcross; rare,
  document as unsupported without it.
- `astrapp.c` keeps ~4k lines of C-preprocessor machinery because the
  `.S` path shares it. Trimming C-only directives is optional polish.
