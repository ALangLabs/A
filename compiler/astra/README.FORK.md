# First-party native compiler (compiler/astra/)

This directory is now the **builtin, Astra** for A.
It previously lived under `third_party/tinycc` as a vendored dependency;
it has been moved here so it can be forked, rebranded, and evolved as
the **sole** production compiler for A (no external gcc/clang/llc).

## Role in A

- `A -> emitCSource() (C99 aval ABI) -> compiler/astra -> EXE` for `a build`
- `A -> emitCSource() -> compiler/astra (memory mode)` for `a run` JIT
- Built once by `cmake/libtcc.cmake` into `libtcc` + `tcclib/` staged
  support archive.

## Fork status

Upstream TinyCC (Fabrice Bellard), see `README`, `COPYING`, `VERSION`.
A-side integration now references this directory as
`compiler/astra/` and the public API (`libtcc.h`) is kept stable while
the fork is reworked. Follow-up fork work (rebranding, optimizer/ABI
tuning for A) should happen here, not in `third_party/`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
```
