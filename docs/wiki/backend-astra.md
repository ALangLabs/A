# Astra backend

`compiler/astra/backend/` is **Astra**, a fork of TinyCC (Fabrice Bellard,
LGPL — see file headers) adapted as the A toolchain's C backend. It compiles
and assembles the `.s` emitted by the frontend together with the C runtime
modules into native executables. Built by `cmake/libastra.cmake`.

## Entry points

- `astra.c` — the `astra` driver executable. Parses args via
  `astra_parse_args` (astratools.c), accepts `link`/`cc` subcommand aliases,
  locates the staged runtime via `astra_libpath()` (probes for
  `libastra1.a` next to the binary, `../astralib`, etc.), then loops
  `astra_add_file`/`astra_output_file`/`astra_run`. Tool modes: `-ar`
  (`astra_tool_ar`), `-impdef`, `-print-search-dirs`, `-m32/-m64` cross
  (`astra_tool_cross`), `-run`, `-dt` test mode.
- `libastra.c` / `libastra.h` — the compiler-as-a-library API
  (`ASTRAState`, `astra_new/delete`, `astra_set_options`,
  `astra_add_include_path`, `astra_define_symbol`, `astra_compile_string`,
  `astra_set_output_type` with `ASTRA_OUTPUT_MEMORY/EXE/DLL/OBJ/PREPROCESS`,
  `astra_output_file`, `astra_run`, `astra_relocate`, `astra_get_symbol`,
  `astra_list_symbols`, `astra_setjmp`, `astra_set_backtrace_func`,
  `astra_set_realloc`). Built as `libastra` static lib with `ONE_SOURCE=1`.
- `astra.h` — shared internal header: target selection macros
  (`ASTRA_TARGET_X86_64`, `ARM`, `ARM64`, `C67`, `RISCV64`, `I386`, plus
  `ASTRA_TARGET_PE`/`ASTRA_TARGET_MACHO`), `ASTRA_IS_NATIVE`, config flags
  (`CONFIG_ASTRA_BACKTRACE`, `CONFIG_ASTRA_BCHECK`, `CONFIG_NEW_MACHO`),
  platform shims.

## Pipeline modules

| Group | Files | Role |
| --- | --- | --- |
| Core | `astragen.c` (~9k lines) | C parser + IR + code generator core. |
| Preprocessor | `astrapp.c`, `astrape.c`(PE output? no—see note) | Actually `astrapp.c` is the preprocessor; `astrape.c` is PE file output. |
| Assembler | `astraasm.c` | Inline `.s` assembly (`CONFIG_ASTRA_ASM`). |
| Per-target codegen | `x86_64-gen.c`, `arm-gen.c`, `arm64-gen.c`, `i386-gen.c`, `riscv64-gen.c`, `c67-gen.c` | Architecture backends (register allocation, instruction selection). |
| Per-target asm/link | `x86_64-asm.c`/`x86_64-link.c`, `arm-*`, `arm64-*`, `riscv64-*`, `i386-*`, `c67-*` (+ `*-tok.h`) | Target-specific assembly emission and linking/relocations. |
| Object formats | `astraelf.c` (ELF), `astramacho.c` (Mach-O), `astracoff.c` (COFF/PE) | Executable/object writers; headers `elf.h`, `coff.h`, `dwarf.h`, `stab.h`. |
| Tools/misc | `astratools.c` (arg parsing, `-ar`, `-impdef`, cross), `astrarun.c` (`-run` in-memory execution), `astradbg.c` (stab/dwarf debug), `python/python.c` (libpython bridge compiled into A programs needing `py.*`), `lib/` (runtime support sources). |
| Windows | `win32/include/**`, `win32/lib/*.c` | Bundled MinGW headers + CRT startup objects for PE targets. |

## `lib/` — support code compiled into outputs

`libastra1.c` (ELF) / `lib-arm64.c` + `armflush.c` (AArch64) / `armeabi.c`
(ARM), `runmain.c` (entry: runs ctors, `main`, dtors — kept out of the archive
for `astra_run`), `bcheck.c` (optional bounds checker, `-b`),
`bt-exe.c`/`bt-dll.c`/`bt-log.c` (`-bt` backtraces), `builtin.c`
(`__builtin_*`/`__astra_builtin_*`), `tcov.c` (test coverage), `dsohandle.c`,
`stdatomic.c`, `va_list.c`, `alloca.S`, `atomic.S`.

`cmake/libastra.cmake` builds `libastra1.a` and `runmain.o` **using the astra
driver itself**, stages headers into `build/astralib/include`, and prebuilds
`libaruntime.a` from `compiler/astra/runtime/*.c` (the A runtime — see
[runtime.md](runtime.md)). Generated `config.h` sets `ASTRA_TARGET_*`,
`CONFIG_ASTRA_PREDEFS 0`, `CONFIG_ASTRA_LIBPATHS` (staged dir + system lib
dirs), `ASTRA_LIBGCC`, optional `CONFIG_SELINUX`.

## How it fits

`a build`/`a run`/`a test` invoke the staged `astra` binary
(`pipeline.cpp::astraBuild`) with the emitted `.s` plus either
`libaruntime.a` (default `-O2`) or the runtime `.c` sources. Related:
[Codegen](frontend-codegen.md), [Runtime](runtime.md),
[Driver/pipeline](frontend-driver.md), [overview](overview.md).
