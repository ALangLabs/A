# Mojo gap analysis and roadmap

Goal: identify what Mojo does (from the OSS mirror at `/tmp/mojo-ref`), map each
capability onto A's current implementation, and prioritize the work needed for
parity. Sources: `compiler/astra/frontend/**` (A), `/tmp/mojo-ref/Mojo/**`
(Mojo stdlib, docs, proposals; the Mojo compiler itself is closed-source —
only design docs under `Mojo/docs/compiler/` are public).

## 1. What the Mojo OSS repo actually contains

The compiler is **not** in the repo (MLIR-based, closed). Public surface:

- `Mojo/stdlib/std/` — the full stdlib *in Mojo source* (~60 modules):
  `builtin` (traits like `Copyable`/`Moveable`/`ImplicitlyCopyable`,
  `AnyType`, literals, `SIMDLength`), `memory` (`Pointer`, `UnsafePointer`,
  `MaybeUninit`, `alloc`), `origin` (`ImmOrigin`/`MutOrigin`/`AnyOrigin`),
  `collections`, `math`, **`simd.mojo`** (~4,500 lines: `Simd`/`Int`/`Float`
  vector types, element-wise ops, shuffles, reductions, intrinsics via
  `std.sys.llvm_intrinsic`), `algorithm/backend/{cpu,tile,unswitch,
  vectorize}` (autovectorization hints the compiler consumes),
  `python` (`PythonObject`, `ConvertibleToPython`), `ffi`, `traits`,
  `reflection` (`reflect[T]` comptime reflection), `compile`
  (`compile_info` → asm/LLVM-IR/object inspection), `runtime`, `sys`
  (`is_gpu`, `dtype_of`, `size_of`, `align_of`, `bit_width_of`,
  `_RegisterPackType`), `_plugin/{cuda,hip,metal}` (GPU plugins),
  `_gpu/primitives`, `testing/prop` (property-based testing), `subprocess`,
  `tempfile`, `logger`, `benchmark`, `itertools`, `complex`, `stat`.
- `Mojo/docs/site/` — full manual + language reference:
  parameterization (`docs/site/manual/parameters/`), metaprogramming
  (`metaprogramming/{comptime-evaluation,constraints,materialization,
  reflection}.mdx`), generics, traits, lifecycle/ownership
  (`lifecycle/{life,death,destruction-transfer,explicit-destroy,
  initialization,trivial}`), errors (typed/variant/parametric-raises/
  context managers), `c-ffi`, operators; reference pages for
  `comptime if/for/match` (`reference/compound-statements.mdx`), decorators,
  cheat-sheets.
- `Mojo/docs/compiler/` — internal design docs: `DesignOverview.md`,
  `arcana/{Generics,WitnessElaboration,Thunks,Conformance}.md`,
  `overviews/{Interpreter,MojoREPL,ParallelElaboration,Variadics,
  LifeOfMojoRegPassableArgs}.md`, `manual/{ParameterSugar,PassesAndIR,
  MojoNotes}.md`. Useful for understanding *how* Mojo compiles even though
  the sources aren't public.
- `Mojo/proposals/` — RFCs: `async-design.md` (**async is still a draft**;
  Mojo has an internal unfinished stackless-coroutine implementation, not a
  stable feature), `value-ownership.md`, `origin-design.md`,
  `unsafe-pointer-v2.md`, `lifetimes-and-provenance.md`, `trait_composition`,
  `where_clauses`, `variadics-design`, `enums`, `pattern-matching`,
  `parametric_alias`, `project-manifest-and-build-tool`.
- `max/` — GPU kernel library (`max.kernels` matmul etc.) showing how Mojo is
  actually used for accelerator programming.

## 2. Mojo headline features vs A's state

### 2.1 Parametric types / generics

- **Mojo**: `fn f[T: Trait](x: T)`, `struct Tensor[rank: Int, dtype: DType]`;
  parameters can be types, values, or bools; automatic parameterization,
  aliases, partial binding, `rebind` (`docs/site/manual/parameters/`,
  `proposals/parameter-binding-syntax.md`, `parametric_alias.md`).
- **A**: `fn id<T>(x: T) -> T`, `<T: Trait>` bounds, `class`/`enum`/`trait`/
  `impl`/`type` all exist (`docs/wiki/declarations.md`,
  `compiler/astra/frontend/type-checker/sema.hpp` `typeParams`/`typeParamVars`
  on `FnSig`/`StructSym`/`EnumInfo`). `where` constraints **work**
  (`sema` + `tests/integration/cases/*where*`); traits support inheritance.
  **Gap**: no comptime *value* parameters (see 2.2), no aliases-as-parameters,
  no automatic parameterization, trait bounds are checked but there is no
  witness/constraint elaboration system like Mojo's `arcana/WitnessElaboration`.

### 2.2 Comptime metaprogramming

- **Mojo**: `comptime if/for/match`, `Comptime[T]` wrapper, comptime aliases,
  `reflect[T]` field/method reflection, materialization of comptime values,
  constraint implication for generic calls (`docs/site/manual/
  metaprogramming/**`, `docs/compiler/arcana/Generics.md`).
- **A**: `comptime` params are **parsed but not implemented** —
  `docs/wiki/declarations.md:80-85`: "call-site specialization is not
  implemented … comptime functions are effectively unusable today."
  `stdlib/std/comptime/comptime.a` is a stub (`comptime_version() -> "0.0.1"`,
  runtime `type_tag` helpers). AST already has `ComptimeParam`
  (`ast/stmts.hpp`, `parser/parser_decl.cpp:53-122`); `llvm_emit.cpp:330`
  reads them into `FnInfo::paramNames`.
- **Priority: P0** — this is Mojo's single biggest differentiator and A's
  largest known hole. Implementation path: specialize at the module-graph
  level — clone `FnDecl`/AST per comptime binding before `Sema::analyze`
  (in `pipeline.cpp` `runFrontendPipeline`), so the emitter gets
  specialization for free; then add `comptime if/for` as sugar that
  evaluates a pure-A interpreter subset at parse/sema time.

### 2.3 SIMD / vectorization

- **Mojo**: `std.simd` fixed-width SIMD types lowered to LLVM vectors +
  intrinsics; `std.algorithm.vectorize` backend asks the compiler to
  autovectorize `map`-style loops; `SimdLength`/`_RegisterPackType` for
  register packing; GPU `DevicePassable` types.
- **A**: `stdlib/std/simd/simd.a` is explicitly "scalar-emulated fixed-width
  lanes" (`docs/wiki/stdlib.md:21`); real perf comes from hand-written C
  kernels (`linalg_*` builtins in `compiler/astra/runtime/`). The LLVM
  backend `llvm/llvm_emit.cpp` emits **only i64 `aval` words** and routes every
  operation through `a_*` runtime calls — there is **no `llvm.vector`, no
  vector arithmetic, no autovectorization possible**; `clang -O` can only
  clean up allocas.
- **Implementation path (P1)**: introduce a `vector[T, N]`/`simd` type in
  `types.hpp` + parser, represent values as LLVM `<N x ty>` in
  `llvm_emit.cpp`, lower element-wise ops to native vector IR so LLVM's loop
  vectorizer/SLP can fire; keep the aval ABI for boxing at boundaries.
  Autovectorization of `for` loops needs real LLVM arithmetic instead of
  `a_add` calls — i.e. the `*_fast` runtime helpers + unboxed i64/f64 fast
  paths in `llvm_emit.cpp`.

### 2.4 Async / parallelism

- **Mojo**: `async def` stackless coroutines exist internally but are
  **unstable/draft** (`proposals/async-design.md`); `parallelize`/task
  runtime lives in `max.runtime`; `std.algorithm` CPU/GPU backends.
- **A**: no async syntax. Has **implicit parallelism**: runtime thread pool
  (`compiler/astra/runtime/rt_pool.c`) auto-parallelizes `sum`/`map`/`filter`
  over large lists and `linalg_*` kernels; `A_PAR=0`/`A_PAR_THREADS=N` knobs.
- **Priority: P2** — Mojo itself hasn't shipped stable async; A is arguably at
  parity for user-facing features.

### 2.5 Ownership / memory model

- **Mojo**: deterministic value semantics — argument conventions
  (`owned`/`borrowed`/`inout`/`read`), `deinit`, `__moveinit__`, `Copyable`/
  `Moveable` traits, `Origin` provenance on `Pointer`, `MaybeUninit`, no GC.
- **A**: flow-sensitive `move`/`borrow`/`borrow_mut` + slice-view tracking
  (`frontend/ownership/`) — good borrow-checker parity — but memory is
  reclaimed by a **conservative mark-sweep GC** (`runtime/gc.c`, `A_GC=0` to
  disable). `Pointer<T>` + `unsafe` exist. No `deinit`, no provenance.
- **Priority: P1** — path: `deinit`/drop elaboration in `ownership/` (insert
  destructor calls at scope exit), keep GC as cycle fallback.

### 2.6 Python interop

- **Mojo**: `PythonObject`, `ConvertibleToPython`/`FromPython`, export Mojo
  modules as Python extension modules.
- **A**: `python:` imports, `py` native module, `py.buffer` zero-copy buffer
  protocol; works on all emitters. **Gap**: one-directional; no
  "export A module to Python" packaging.
- **Priority: P0-ish** — parity is mostly there; gap is `a build
  --python-ext` producing a `.so` (small effort reusing the py bridge).

### 2.7 C FFI / native interop

- **Mojo**: `external_call`, `stdlib/std/ffi`, shared-library loading,
  `@export`.
- **A**: no general FFI — native modules are hardcoded tables in
  `name-resolution/native_modules.hpp` + runtime C modules.
- **Priority: P1** — path: `extern "C" fn` declarations lowered in
  `llvm_emit.cpp` to plain LLVM `declare`+call (trivial there); runtime
  `ffi_call`/`dlopen` helpers for completeness.

### 2.8 Package manager / build tooling

- **Mojo**: `mojo build/run/test/doc/format/repl`, `.mojopkg` format, Jupyter.
- **A**: `apm/` package manager (registry/resolver/installer/lockfile),
  `a run/build/check/test/fmt/doc/repl`, self-hosted REPL, LSP + VS Code.
  **Gap**: no Jupyter kernel; no compiled-package format.
- **Priority: P2** — `apm` arguably exceeds Mojo's package UX already.

### 2.9 Compile times / architecture

- **Mojo**: MLIR pipeline, parallel elaboration, fast incremental compile.
- **A**: single-pass C++ frontend; `Sema::analyzeParallel`; LLVM backend via
  opt→llc→clang; per-module object cache planned
  (`docs/plans/per-module-object-cache.md`). No
  incremental compilation.
- **Priority: P2** — finish object cache.

### 2.10 Errors / exceptions

- **Mojo**: `raises`, `Error` union, typed/variant errors, `try`/`except`,
  context managers.
- **A**: `Result<T,E>` + `expr?` propagation, `defer`, `panic`. **Gap**: no
  `try`/`except` catch, no error variants, no context managers.
- **Priority: P1** — path: parser+sema sugar lowering to Result discriminant
  checks.

### 2.11 Other smaller gaps

- **Variadics**: Mojo full variadic support; A has `*args`→list — partial.
- **Pattern matching**: both have `match`; Mojo's richer (guards, nesting).
- **Reflection**: Mojo `reflect[T]`; A has runtime-only `struct_name`/
  `struct_methods`/`fn_signature` helpers — no comptime reflection (needs 2.2).
- **GPU**: Mojo CUDA/HIP/Metal plugins — A has nothing (P3).
- **Property-based testing**: Mojo `testing.prop`; A has `a test` — P2.

## 3. Prioritized roadmap

### P0 — correctness/interop foundations
1. **Comptime specialization** (2.2): AST-clone specialization in
   `pipeline.cpp` before sema; `comptime if/for` via a parse-time evaluator.
2. **`try`/`except` on `Result`** (2.10): parser + sema + emitters.
3. **LLVM-emit typed arithmetic fast path** (2.3 prerequisite): emit real
   LLVM `add`/`fadd`/`load`/`store` for provably-unboxed `i64`/`f64` locals
   — prerequisite for vector IR and autovectorization.
4. **Python-module export** (2.6): `a build --python-ext`.

### P1 — performance / systems parity
5. **`vector[T,N]` SIMD type** in LLVM backend (`<N x ty>` IR,
   `llvm.vector` intrinsics); port `stdlib/std/simd` to it.
6. **Deterministic `deinit`/drop** in `ownership/`; demote GC to cycle-fallback.
7. **C FFI**: `extern` declarations → LLVM `declare`; runtime `ffi_call`.
8. **Witness/constraint elaboration** for `where` (Mojo `arcana` parity).

### P2 — ecosystem
9. Finish **per-module object cache**.
10. **Jupyter kernel**; richer `match` patterns; property-testing
    stdlib module.

### P3 — explicitly out of near-term scope
- GPU/CUDA/HIP/Metal backends — needs a device abstraction A doesn't have.
- Stable `async`/`await` — Mojo hasn't shipped it either.

## 4. Bottom line

A already matches or exceeds Mojo on: Python-syntax ergonomics, module
system, package manager (`apm`), LSP/tooling, runtime breadth, Python
interop direction, and has a *working* borrow checker plus a native LLVM
backend.
A is decisively behind on: **comptime metaprogramming** (parsed but dead —
highest-leverage fix), **typed SIMD/vector IR**, **deterministic memory
management** (GC vs ownership), **general C FFI**, and **structured error
handling**. Items 1-4 of P0 are prerequisites for most of P1.
