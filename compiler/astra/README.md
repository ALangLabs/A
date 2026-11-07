# Astra toolchain (`compiler/astra/`)

This directory holds the **Astra** toolchain: the TinyCC-derived compiler/assembler/linker backend used to turn the A frontend's x86-64 assembly into native machine code, plus the A runtime linked into compiled programs.

## Layout

- `frontend/` — C++ frontend (lex/parse/sema/ownership/IR/optimizer), pipeline, and the `a` / `a-lsp` drivers. It emits x86-64 AT&T assembly (`emitAsm`) and uses the `astra` driver for `a run` / `a build` / `a test`.
- `backend/` / Astra C toolchain sources — the Astra compiler/assembler/linker codebase (TinyCC-derived), per-target backends (x86-64/arm64 and legacy targets), bundled headers/support sources, and build integration (`cmake/libastra.cmake` stages `build/astra/astra` and `build/astralib/`).
- `runtime/` — the A runtime that generated assembly and the compiled executable link against: `a_runtime.h` (public API), `a_runtime_internal.h` (shared object model + helper prototypes), `a_runtime.c` (builtin dispatch, printing, init), and modular C files (`core.c`, `value.c`, `methods.c`, `builtins.c`, `fs.c`, `intrinsics.c`, `time.c`, `terminal.c`, `json.c`, `binary.c`, `log.c`, `metrics.c`, `trace.c`, `net.c`, `arena.c`, `unsafe.c`, `python.c`). The C++ frontend-support library (`collections/`, `errors/`, `io/`, `memory/`, `strings/`) lives in subdirectories of the same folder.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
```

`cmake/libastra.cmake` produces `build/astra/astra` (the driver) and
stages the support archive + headers into `build/astralib/`. See
`README.FORK.md` for backend provenance and local changes.

License: the backend is LGPL 2.1 (`backend/COPYING`,
`backend/RELICENSING`).
