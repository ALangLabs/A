# Compiler/Stdlib Redesign Status (Mojo-inspired)

This document tracks the incremental rewrite/redesign from the current C++20 toolchain toward a Mojo-inspired structure.

## Implemented
- C++ compiler pipeline remains staged under `compiler/stages/` (`lexer -> parser -> ast -> name-resolution -> type-checker -> ownership -> ir -> optimization -> backend/tooling/driver/cli`).
- Backends: interpreter (most complete runtime semantics), native C++ emitter, LLVM emitter/path as implemented; `a ir` exposes typed SSA IR.
- Stdlib reorganized to Mojo-style `stdlib/std/<pkg>/` with package `__init__.a` files, `std/prelude`, and loader support for `std/` package resolution.
- Legacy compat copies removed where source imports were migrated; `stdlib/std_layout` integration smoke verifies `std/` package imports.
- Native modules remain C++-implemented (`name-resolution/native_modules.hpp`, `backend/stdlib/builtins.cpp`); `std/<pkg>/` stubs provide canonical package locations.

## Honest gaps / blockers
- **Full rewrite from scratch:** not completed. A safe full rewrite of lexer/parser/sema/IR/backends/runtime/tooling while preserving all behavior is a large staged effort; current work is incremental and test-preserving.
- **Full native-stdlib self-hosting in `.a`:** blocked where functionality requires runtime/FFI/OS/compiler hooks or backend lowering (I/O, fs, net, http, json, trace, metrics, sockets/TLS, backend-specific intrinsics). Many packages are native-backed by design today.
- **Ergonomic native stub re-export wrappers without namespace-method regressions:** partially constrained by module namespace/type/method dispatch semantics. Re-export-only shims caused regressions; compat required behavior-preserving copies. Needs a loader/type-checker/module-object design that preserves struct method dispatch and namespace exports.
- **MLIR-dialect backend (LIT/KGEN/POP/HLCF-style):** not implemented. Blockers: no in-tree MLIR/LLVM dialect build integration for those custom dialects, significant compiler-backend architecture work, and dependency/toolchain complexity. Current tree has LLVM-oriented/native paths; no custom MLIR dialect implementation.
- **Real SIMD vector codegen/specialization:** not implemented. Current SIMD is scalar/list emulation in `.a` (`std/simd`, prelude `Vec4` helpers); real fixed-width vector lowering requires typed vector IR/codegen and backend support.
- **Real GPU codegen/specialization:** not implemented. `std/gpu` is host/interpreter-oriented abstractions/stubs; real GPU execution requires device runtime, kernel codegen, buffer lifetime/memory model, and target toolchain integration.
- **Typing-model redesign:** explicit current/target model documented in `docs/typing-model.md`; enforcing static-core semantics across checker + all backends requires staged migration and test updates.

## Next incremental steps
1. Preserve compat by improving module re-export semantics (no method-dispatch regressions), then remove remaining legacy path dependencies.
2. Add typed vector/GPU IR hooks and backend capability checks; keep scalar fallbacks.
3. Introduce explicit static-boundary checking behind opt-in/migration flags or module conventions, with golden tests.
4. Prototype MLIR backend only behind a CMake option with clear dependencies; do not replace working backends until parity is proven.
