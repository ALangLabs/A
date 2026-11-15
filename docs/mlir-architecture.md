# A toolchain: Mojo/KGEN-style MLIR architecture

Goal: move `a` from "AST -> hand-printed llvm-dialect text -> mlir-translate/opt/llc/clang
subprocesses" to a Mojo/KGEN-style architecture:

  source -> lexer/parser/AST -> comptime -> Sema -> ownership
        -> emit `a` MLIR dialect (in-process MLIR objects)
        -> MLIR pass pipeline (canonicalizer, mem2reg/SROA, custom lowering)
        -> `a` -> `llvm` dialect lowering
        -> in-process LLVM IR -> LLVM opt pipeline -> TargetMachine -> object
        -> link with libaruntime.a

plus Mojo-`.mojoc`-style bytecode packaging: checked modules serialize to MLIR
bytecode (`.ambc`) so imports/precompiled packages skip re-parse+sema.

## Status (as of this doc)

Implemented and exercised by `tests/mlir/` (`a mlir-opt` golden suite):

- **`a` dialect + text round-trip**: DONE. Ops/types in
  `src/astra/mlir/dialect/` (`ADialect`, `AOps`, `ATypes`); `a mlir-opt`
  parses and prints MLIR text round-trip (`a_ops_roundtrip`,
  `a_ops_struct`, `typed_ops`, `typed_unbox` cases).
- **`a` -> `llvm` lowering**: DONE — all 23 ops lowered
  (`a-lower-to-llvm` pass in `src/astra/mlir/passes/ALowerToLLVM.cpp`),
  including `a.struct.getfield`/`a.struct.setfield` via the per-op `field`
  StrAttr (lowered to `a_getattr`/`a_setattr` runtime calls with
  materialized `__a_cstr<N>` name globals) and i1/`!a.bool` ABI values
  (tagged-aval encoding). `a.for`/`a.while`/`a.condition` region
  rebuilding is heap-safe (in-place successor retargeting).
- **`a-canonicalize` pass**: DONE — folds `to_aval`/`from_aval` round-trips,
  `from_aval(a.const int)` chains, identity `to_aval`, and `a.gep` with a
  constant-zero index; wired before the lowering passes.
- **Typed values M1+M2: DONE and DEFAULT.** `a.to_aval`/`a.from_aval`
  boxing plus unboxed `i64`/`f64`/`i1` scalar emission for Sema-proven
  Int/Float/Bool bindings (`A_MLIR_TYPED=0` selects the boxed
  llvm-dialect emitter). Only exact-semantics ops stay unboxed: i64
  bitwise/comparisons, all f64 arith, i1 logic; `+ - * << >>` and unary
  `-` on ints keep the boxed aval path (runtime promotes int64 overflow
  to f64 and panics on out-of-range shifts). See
  `docs/mlir-typed-values-design.md`.
- **Pass pipeline**: canonicalize/CSE/mem2reg/SROA + custom lowering passes
  wired in `src/astra/mlir/pipeline.cpp`; `a mlir-opt` accepts mlir-opt-style
  pass lists.
- **Per-module `.ambc` packages**: DONE. `a precompile <pkg> -o <dir>`
  writes `<dir>/<mod>.ambc` + `ambc.meta`; consumed by default on
  non-Windows (`A_AMBC=0` disables; `ambc_modules.cpp`). Single-file
  `foo.ambc` sibling packages also work; `a test` uses the per-module
  layout (`ambcModulesCompileToObject` synthesizes the `a_test_main`
  harness and applies the compile-time `--filter` substring). Packages
  record source mtimes (`src` records in `ambc.meta`,
  `<name>.ambc.srcs` for whole-graph packages): a changed or missing
  source falls through to the normal pipeline. Packages written before
  staleness tracking (no `src` records) are still trusted
  unconditionally. Module units and entry.mlir are emitted with the
  `a`-dialect typed emitter when `A_MLIR_TYPED` is on (the default) and
  the boxed llvm-dialect emitter when it is off, matching `a run`/
  `a build`.
- **In-process LLVM bridge**: implemented in
  `src/astra/mlir/codegen.cpp`/`native.cpp` (parse llvm-dialect text ->
  LLVM IR -> `llvm::PassBuilder` -> `TargetMachine` -> `.o`) and is the
  DEFAULT on non-Windows hosts.
- **Defaults**: the in-process MLIR path is the default for
  `a build`/`a run`; the subprocess pipeline
  (`mlir-translate`/`opt`/`llc`/`clang`) remains as the escape hatch via
  `A_MLIR_NATIVE=0` or `A_BACKEND=subprocess` (aliases
  `mlir-subprocess`/`llvm-subprocess`/`tools`). `A_BACKEND=mlir-native`
  (or `native`) selects it explicitly. Scalar emission is typed by
  default (`A_MLIR_TYPED=0` selects the boxed llvm-dialect emitter).
  Windows and no-MLIR builds always
  use the subprocess path. `a check`/`a fmt`/`a doc`/LSP never touch
  either backend.

## Environment facts (verified)

- MLIR 18 dev is installed: headers `/usr/lib/llvm-18/include/mlir`,
  libs `/usr/lib/llvm-18/lib/libMLIR*.a` (333 libs), CMake package at
  `/usr/lib/llvm-18/lib/cmake/mlir/MLIRConfig.cmake` (`find_package(MLIR)`).
- Tools on disk (not PATH): `/usr/lib/llvm-18/bin/{mlir-opt,mlir-translate,
  mlir-tblgen,mlir-cat,mlir-reduce}`.
- Compiler is C++20, `namespace a`, warnings-as-errors in CI.
- Existing paths: frontend at `src/astra/frontend/`, runtime at
  `src/astra/runtime/`, stdlib `stdlib/`, tests `tests/integration/cases/`.

## Layout (new code under src/astra/mlir/)

- `src/astra/mlir/CMakeLists.txt` — `a_mlir` static lib + tools.
- `src/astra/mlir/dialect/` — `a` dialect: TableGen `.td` under
  `src/astra/mlir/include/A/` (ops/types/attrs), C++ impl next to it.
- `src/astra/mlir/passes/` — MLIR passes (lowering, canonicalization helpers).
- `src/astra/mlir/codegen.cpp` — in-process backend: MLIR module -> LLVM IR
  (`mlir::translateModuleToLLVMIR`) -> `llvm::PassBuilder` opt ->
  `llvm::TargetMachine` -> `.o`; link step still uses `clang`/`ld` via
  libaruntime.a (runtime is C, keep as-is).
- `src/astra/mlir/bytecode.cpp` — BytecodeWriter/Reader helpers for `.ambc`.
- `src/astra/mlir/driver.cpp` — optional `a mlir-opt`-style hidden subcommand
  for pass debugging (like `kgen-opt`).

## Dialect sketch (`a` dialect, phase 1)

Phase 1 keeps the current aval/boxed semantics so behavior is identical:

- Types: `!a.aval` (= i64 payload), `!a.ptr`, `!a.struct<name>`, `!a.array<N>`,
  `!a.enum<name>`.
- Ops: `a.func`, `a.call`, `a.struct.getfield`, `a.struct.setfield`,
  `a.array.get`, `a.array.set`, `a.const` (int/float/bool/str/none),
  `a.if`, `a.for`, `a.return`, `a.alloca`, `a.load`, `a.store`,
  `a.runtime.call` (escape hatch to `a_*` runtime fns), `a.fib.call`.
- Lowering `a` -> `llvm` dialect reproduces exactly what
  `frontend/mlir/mlir_emit.cpp` emits today (boxed i64, runtime calls).

Phase 2 (later, not this pass): typed values (`!a.int`, `!a.float`, `!a.simd`),
SROA/Mem2Reg on aggregates, real optimization. Structure the dialect so types
can be added without breaking ops.

## Bytecode format (`.ambc`)

Mojo `.mojoc` analog: serialize the checked per-module MLIR module to MLIR
bytecode (`mlir::BytecodeWriter`). Import resolution: `ModuleLoader` consults
`.ambc` next to `.astra` sources; on hit, `BytecodeReader` materializes the
module's ops lazily (or eagerly v1). Version-stamp with compiler version;
mismatch = fall back to source. `a precompile <dir>` writes `.ambc` for a
package (parse + sema only, like `mojo precompile`).

## Rules for all workstreams

- Do NOT edit files outside your assigned ownership list.
- Do NOT run full `cmake --build build` (other agents share it). Verify with
  `g++ -std=c++20 -fsyntax-only` against MLIR headers
  (`-I/usr/lib/llvm-18/include -I/usr/lib/llvm-18/include/mlir/IR` etc.) or a
  private build dir `build-<name>` if you must configure.
- Keep `emitMLIR` (text path) and the `llvm_emit.cpp` fallback intact; the
  in-process paths are now the default on non-Windows — `A_MLIR_NATIVE=0`
  or `A_BACKEND=subprocess` and `A_AMBC=0` are the escape hatches.
- Namespace `a`, C++20, no warnings (CI = Werror).
- MLIR 18 API: use `mlir::` dialect registration, `PassManager`,
  `BytecodeWriter`/`BytecodeReader`, `translateModuleToLLVMIR`,
  `llvm::PassBuilder`, `llvm::TargetMachine`. Study headers under
  `/usr/lib/llvm-18/include/mlir/` before writing code.
