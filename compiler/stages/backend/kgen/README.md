# KGEN backend (A, KGEN-only)

Architecture adapted from Mojo `Compiler/{Pipeline,KGENCompiler,ObjectCompiler}` and `KGENToLLVM`:
- `kgen_backend.*` / `kgen_compiler.hpp`: KGEN compiler entry points.
- `object_compiler.*`: object compilation façade (LLVM IR emission, build, run).
- `codegen/llvm_emitter.*`: A IR/AST -> LLVM IR emission and module build/run target.
- `mlir/*`: LLVM-18/MLIR-aware toolchain path (`llc`/`clang` object+link); uses installed LLVM/MLIR 18 when found.
- `execution_engine*` + `runtime/builtins.cpp`: internal execution engine/runtime support for full dynamic A `run`/`test` semantics while native KGEN lowering coverage is incomplete.

Full Mojo parity requires A-specific MLIR dialects (LIT/HLCF/POP/KGEN analogs) and `KGENToLLVM`-style lowering plus LLVM pass/object emission for all A semantics.
