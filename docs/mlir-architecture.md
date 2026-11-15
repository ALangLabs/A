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
- Keep `emitMLIR` (text path) and the `llvm_emit.cpp` fallback intact; new
  paths are additive, gated by env `A_MLIR_NATIVE=1` until integration.
- Namespace `a`, C++20, no warnings (CI = Werror).
- MLIR 18 API: use `mlir::` dialect registration, `PassManager`,
  `BytecodeWriter`/`BytecodeReader`, `translateModuleToLLVMIR`,
  `llvm::PassBuilder`, `llvm::TargetMachine`. Study headers under
  `/usr/lib/llvm-18/include/mlir/` before writing code.
