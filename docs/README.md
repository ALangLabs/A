# Documentation

- [wiki/](wiki/index.md) — technical wiki covering the whole toolchain:
  architecture, compiler frontend, LLVM backend, runtime, stdlib, kernels,
  editor tooling, tests, examples, and benchmarks.

Design documents:

- [mlir-architecture.md](mlir-architecture.md) — Mojo/KGEN-style in-process
  MLIR architecture (`src/astra/mlir/`), `a` dialect, lowering passes,
  `.ambc` bytecode packages, and implementation status.
- [mlir-typed-values-design.md](mlir-typed-values-design.md) — typed-values
  (`!a.int`/`!a.float`/…) design for the `a` dialect.
- [mlir-emitter-inventory.md](mlir-emitter-inventory.md) — inventory of what
  the current MLIR emitter (`frontend/mlir/`) produces.
- [runtime-typed-abi.md](runtime-typed-abi.md) — typed runtime ABI notes.
- [mojo-kgen-notes.md](mojo-kgen-notes.md) / [mojo-gap-analysis.md](mojo-gap-analysis.md)
  — Mojo/KGEN reference notes and gap analysis motivating the MLIR work.
