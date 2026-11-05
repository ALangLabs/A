# Mojo compiler/backend reference (vendored for A)

This directory contains a **read-only reference copy** of Mojo compiler internals from `modular/modular` (`/tmp/mojo-ref/Mojo/lib`, depth-1 clone at time of copy) to inform A's backend redesign.

## Contents
- `Mojo_lib/` — copy of `Mojo/lib/` (MojoParser/Elaborator, LIT/KGEN/POP/HLCF/CO dialect code, `KGENToLLVM/`, `LowerLIT/`, transforms, compiler/tooling support).
- `LICENSE.upstream` — upstream license file from the source repository.

## License/attribution
Upstream files retain their original headers (Apache-2.0 with LLVM Exceptions in the copied Mojo compiler sources where present). **Do not treat this as a blanket license grant for all Modular/MAX code**; MAX components may be under different terms. Keep attribution and comply with upstream licenses. This copy is for study/integration reference; modifications for A must preserve required notices.

## Integration intent (A)
Goal: adapt Mojo's MLIR/KGEN-inspired backend architecture to **A syntax/semantics** (simpler syntax than Mojo) rather than adopting Mojo syntax.

Mapping guidance (design, not yet implemented):
- `MojoParser/` concepts -> A frontend (`compiler/stages/{lexer,parser,ast,name-resolution,type-checker}`) keeps A syntax; only semantic/lowering ideas are reused.
- `LITDialect/` (frontend IR) / elaboration ideas -> A typed AST + `compiler/stages/ir` SSA IR.
- `HLCFDialect/`/CF lowering ideas -> A control-flow lowering in `ir`/`optimization`/backend.
- `POPDialect/` (SIMD/arithmetic) -> future real SIMD/vector lowering for A.
- `KGENDialect/` + `KGENToLLVM/` -> future MLIR-based backend target for A (requires MLIR/LLVM toolchain + CMake integration + A-specific op/type mapping).
- `CompilerRT`/runtime bridges -> compare with `runtime/` and C++ backends; adapt only where compatible with A runtime model.

## Current blocker status
**Not wired into A's build.** A currently builds via CMake (`CMakeLists.txt`, C++20, `tooling/build/build.sh`) and does not depend on MLIR/LLVM or Modular's Bazel/toolchain. Compiling/using `Mojo_lib` as A's backend requires:
1. MLIR/LLVM development packages and compatible toolchain configuration.
2. A-specific frontend/type/ownership/IR mapping (A has different syntax/semantics than Mojo).
3. New CMake targets/integration and parity tests against existing `a run/check/test/build/ir` behavior.
4. Careful handling of API/dependency differences from Modular's internal build environment.

Until that integration is implemented and verified, this directory is reference-only. See `docs/compiler-redesign.md` and `TODO.md`.
