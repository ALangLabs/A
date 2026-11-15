# Typed values in the `a` MLIR dialect (phase 2 design)

Status: design spec, implementation-ready. Phase 1 (boxed `!a.aval` semantics,
see `docs/mlir-architecture.md`) is in place: the `a` dialect exists under
`src/astra/mlir/` (`include/A/{ADialect,ATypes,AOps}.td`, `dialect/*.cpp`,
`passes/ALowerToLLVM.cpp`, `pipeline.cpp`). This document specifies how Sema's
static types flow into `a`-dialect ops so the MLIR pass pipeline can optimize
real arithmetic instead of `i64`-tagged payloads.

## 0. What exists today (facts the design builds on)

- `Expr::inferredType` (`src/astra/frontend/ast/exprs.hpp:15`) is filled by
  Sema for every expression via `resolveType(type)` (`sema_expr.cpp:8`),
  which collapses `TypeKind::Var` inference links. `SemaResult.exprTypes`
  (`sema.hpp:84`) mirrors it for LSP. The emitters already receive `SemaResult`
  (`MlirEmitter` ctor, `mlir_emit.cpp:64`; `(void)sema_` at line 70 shows it
  is currently ignored).
- `Type` (`type-checker/types.hpp:63`) is a tagged union-ish struct:
  `kind` + `bits`/`isSigned` (ints/floats), `elem`/`errorType`/`params`
  (containers), `structName` (Struct/Enum nominal), `moduleId` (Module),
  `ret` (Function), `length` (Array/Vector), `varId`/`link` (inference vars).
- Aval ABI today (`mlir_emit.cpp:31-38`, `698-760`): every value is a tagged
  `i64` (`kAvalTag = -281474976710656`, `kInlineLim = 1<<46`); int payload is
  `(v<<1)|1` in low 46 bits else heap-boxed via `a_i64n`; all operators go
  through `a_*_slow` runtime calls (`emitBinOp`, `mlir_emit.cpp:2627`) because
  nothing is statically known to be an int. Every `a_*` runtime function in
  `libaruntime.a` keeps this ABI: `i64`/`!llvm.ptr` in, `i64` out.
- Dialect ops already accept `AnyType` where phase 2 needs it: `a.call`
  operands/results (`AOps.td:171-172`), `a.load`/`a.store` (`383-406`),
  `a.yield`, `a.while` inits/results, `a.runtime.call` (`477-479`). Hardcoded
  `AVAL` remains on: `a.const` result, `a.return` value, `a.fib.*`,
  `a.struct.*`, `a.array.*`. `a.func` uses `function.type` (`FunctionType`),
  so typed signatures slot in without an op change.
- `buildAOptPipeline` (`src/astra/mlir/pipeline.cpp:61-82`) already runs
  canonicalizer + CSE always and `createMem2Reg`/`createSROA` at `optLevel>0`,
  but they are no-ops on boxed `i64` payloads.
- Comptime runs **before** Sema (`pipeline/comptime.cpp`, header of
  `comptime.hpp`): `fn f(comptime N: int, ...)` calls clone the `FnDecl` to
  `f__ct<k>`, substitute literals for `N`, and strip the comptime params;
  `comptime if`/`comptime for` splice/unroll at the AST level. Sema and all
  backends only ever see specialized functions.

## 1. Type mapping: `Type` -> `a`-dialect MLIR type

New TypeDefs go in `ATypes.td`; scalar unboxed types map to native MLIR
scalar types directly (like Mojo's `!llvm.int`/`!llvm.float` lowering), no
`!a.*` wrapper needed for primitives. `!a.*` is reserved for A-semantics
types that lower differently from their MLIR spelling.

| A `TypeKind` | MLIR type | Notes |
|---|---|---|
| `Int` iN signed | `iN` (native) | i8/i16/i32/i64/u8..u64 all map to `iN`; sign is a Sema-level fact, MLIR ints are signless. `int` default = i64. |
| `Float` fN | `fN` (native) | f32/f64; `float` default = f64. |
| `Bool` | `i1` | MLIR has no bool; `!a.bool` exists but is redundant - prefer `i1` and delete/repurpose `!a.bool` in phase 2.2. |
| `Char` | `i32` | Unicode scalar value; boxing to aval only at runtime boundary. |
| `Vector` `vector[T,N]` / `simd[T,N]` | `vector<NxT'>` (native `vector` dialect type) | types.hpp:43-46 already promises "lowered to a native LLVM `<N x ty>` vector inside expressions". `T'` = mapped elem type; only numeric T allowed (Sema enforces). |
| `String` | `!a.str` (new, = `!llvm.ptr` + i64 len pair is NOT expressible; keep `!a.str` opaque -> `!llvm.ptr` to a `{ptr,len}` header or aval) | **Decision: boxed** `!a.aval` initially. Strings are heap objects mutated by the runtime; a dedicated `!a.str` can come later. Map to `!a.aval` in step M1. |
| `Bytes` | `!a.aval` | Growable heap buffer; runtime-owned. |
| `Array` `list[T]` / `[T]` | `!a.list` (new opaque) -> `!a.aval` payload | Heap container with runtime invariants (GC, iteration protocol). Mojo keeps heap types as pointers too. |
| `Map` `dict[K,V]` | `!a.aval` | Same. |
| `Set` | `!a.aval` | Same. |
| `Range` | `!a.aval` | Runtime object (`a_range_new`). |
| `Tuple` `(T1,..,Tn)` fixed | `!a.tuple<T1',..,Tn'>` (new parameterized) in M3; `!a.aval` in M1 | Fixed tuples are SROA candidates; variadic `tuple[T,...]` stays `!a.aval`. |
| `Optional` `T?` | `!a.optional<T'>` (new) in M3; `!a.aval` in M1 | Payload `T'` + tag; lowers to `{i1, T'}` struct or stays aval. |
| `Result` `Result<T,E>` | `!a.result<T',E'>` (new) in M3; `!a.aval` in M1 | Same shape as Optional. |
| `Union` `T|U` | `!a.aval` | Needs runtime tag dispatch; keep boxed permanently (like Mojo's `Any`). |
| `Any` (`any`/`Any`) | `!a.aval` | Dynamic top type; always boxed. This is the gradual-typing escape hatch. |
| `Struct` `S` | `!a.struct<"Name">` exists; M1 result still `!a.aval`, M3 unboxed `{...}` layout | Phase 2.3 attaches field layout to the existing TypeDef (ATypes.td:79 already anticipates this). |
| `Enum` `E` | `!a.enum<"Name">` exists; M1 `!a.aval`, M3 `{i64 tag, payload union}` | Plain enums are i64-tagged; algebraic enums need payload union. |
| `Function` `fn(T)->R` | `!a.fn` (new opaque) -> `!a.aval` | First-class fn values are `{fnptr, env}` pairs managed by runtime (`a_fnon_new`). Signature is metadata only. |
| `Module` | n/a | Modules are compile-time namespaces; never a runtime value type. Values imported from modules resolve to their member types. |
| `Allocator` | `!llvm.ptr` | Raw allocation handle. |
| `Pointer` `Pointer<T>` | `!llvm.ptr` (opaque) | Unsafe escape hatch; no pointee tracking in MLIR 18 without ptr attrs. |
| `Json`, `IO` | `!a.aval` | Opaque runtime handles. |
| `Void` | no result / `!a.none` | `void` functions return nothing; `none` literal is `!a.none` (exists, lowers to tagged i64 in phase 1). |
| `Var` (unresolved at emit time) | `!a.aval` | Post-Sema `resolveType` should have collapsed these; anything still `Var`/`Error` is a bug - emit `!a.aval` and warn once. |

Rule: **only Int/Float/Bool/Char/Vector get native unboxed MLIR types in M1**;
everything else is `!a.aval` (or an existing `!a.*` nominal marker). This
matches Mojo: scalars unbox, heap/reference types stay pointers.

## 2. Where types attach: typed results, not attributes

**Decision: typed results (Mojo style).** Ops already declare result types;
`a.call`/`a.load`/`a.yield` use `AnyType`/`Variadic<AnyType>` precisely so
`i64`, `f64`, `!a.aval` can coexist. Concretely:

- `a.func`: `function.type` attribute becomes `(i64, f64) -> i1` etc. derived
  from `FnDecl` param/return `TypePtr`s. Args/results are the mapped MLIR
  types directly.
- `a.call`: `functional-type($operands, results)` already prints the real
  types; verifier (`AOps.cpp`) checks against callee `function.type` - extend
  it to compare mapped `a` types, not just `!a.aval`.
- No per-value `a.type` attribute. Rationale: MLIR SSA values already carry
  types; duplicating them as attrs invites divergence and blocks canonicalizer
  folding. The one place a *source* type is needed beyond the MLIR type is
  `TypeKind`-level info MLIR can't express (signedness, nominal identity,
  `Any` vs concrete). That goes in **op attrs where it already exists**
  (`struct_type` on `a.struct.getfield`, `array_type` on `a.array.get`) -
  extend these from `AStruct`/`AArray` TypeAttrs to carry the full mapped
  field/element type, not a parallel attr system.

**Gradual typing caveat:** A has `any`, `Union`, unresolved `Var`s, and
Python-style inference (unannotated containers keep permissive element
types - `Type::fromAnnotation`, types.hpp:89). Policy:

- Concrete scalar types -> unboxed MLIR scalars, always.
- `Any`/`Union`/unresolved `Var`/`Error` -> `!a.aval`. A value typed `Any`
  that flows into an `i64` context gets an explicit `a.to_aval`/`a.from_aval`
  pair (see §3); Sema guarantees `from_aval` sites are dominated by a
  runtime type check or an explicit `cast`/annotation, exactly as the LLVM
  backend already requires.
- `fromAnnotation=false` inferred container element types that are still
  `Var` -> treat as `Any` -> `!a.aval` element ops. This preserves the
  "untyped stays permissive" rule without blocking unboxing of annotated
  code, which is where the wins are.

## 3. Unboxing strategy and the aval ABI boundary

Runtime functions (`a_add_slow`, `a_getattr`, `a_str_new`, `a_bi_call`, ...)
are C functions in `libaruntime.a` with fixed `i64`/pointer ABI. They do not
change in phase 2. The boundary is explicit ops:

- `a.to_aval` (new): operand `AnyType` (any mapped type) -> result `!a.aval`.
  For `iN`/`fN`/`i1`: emits the tag/payload encode inline
  (`avalIntEncode`, `mlir_emit.cpp:34`) or `llvm.call @a_i64n` for big ints;
  for `!a.aval` input: no-op (folded by canonicalizer).
- `a.from_aval` (new): operand `!a.aval` -> result `AnyType`. Emits the
  inline decode for scalars (shift/mask + sign-extend per `isSigned`), or
  `llvm.call @a_unwrap`-style runtime check for heap->scalar narrowing.
  **Verifier rule:** `a.from_aval -> iN` is only legal when the producer is
  an `a.call`/`a.load` whose *declared* result type is `iN` - i.e. it is the
  ABI unpack at function/runtime boundaries, never a mid-expression cast.
  Mid-expression narrowing uses `a.unbox<T>` (checked, may call runtime
  `a_type_error`) - keep the two distinct so unchecked ABI unpack stays free.
- `a.box`/`a.unbox` naming alternative rejected: `to_aval`/`from_aval` names
  the actual representation; `box`/`unbox` implies heap allocation which
  scalars don't do.

Signature changes:

- `a.call` on user `a.func`: operands/results are the *mapped* types. A call
  `f(x)` where `f: fn(i64)->f64` and `x: !a.aval` (e.g. `x: Any`) inserts
  `a.from_aval -> i64` on the arg and `a.to_aval` on the result at the call
  site - the emitter inserts these during AST->ops lowering using
  `Expr::inferredType` vs callee `fnParamTypes` (`SemaResult.fnParamTypes`,
  sema.hpp:77). `a.call` itself stays type-agnostic; the *boundary* ops make
  mismatches explicit and canonicalizable.
- `a.runtime.call`: unchanged shape (`StrAttr callee` + `signature` attr).
  Phase-2 emitter always wraps operands in `a.to_aval` and results in
  `a.from_aval` when the `signature` says i64, preserving the C ABI. The
  `signature` attr stays the LLVM-level truth (`"(i64,i64)->i64"`).
- `a.func` ABI: two lowering modes. (a) **Boxed boundary (default M1):**
  `function.type` carries mapped types internally, but `ALowerToLLVM` still
  lowers `a.func` to `llvm.func` taking `!llvm.ptr argv` and unboxing -
  identical to today, so `a_call`/fib/runtime dispatch keep working. (b)
  **Typed boundary (M4, opt-in per fn):** lower `a.func` to
  `llvm.func` with native signature `(i64,f64)->i1`; callers in the same
  module call it directly with mapped types, no argv pack. Extern fns,
  fibers, `a_bi_call` targets, and anything address-taken stay boxed.
  Gate (b) on `fn` not being extern/variadic/generic-instantiated-and-
  address-taken; measure before making default.

## 4. Comptime / generics mapping

**Recommendation: keep comptime at the AST level; do not model it in MLIR.**

Rationale:

- `runComptimePass` already specializes before Sema (`comptime.hpp` header):
  `comptime N` params become literal substitutions in cloned `f__ct<k>`
  decls; `comptime if`/`for` are fully resolved or degraded to runtime
  control flow. By the time the `a` dialect is built, comptime is gone -
  there is nothing left to map. This is *stronger* than Mojo's
  `!kgen.param`: specialization happens once, monomorphized, and downstream
  Sema/MLIR see only concrete types (better for unboxing: `comptime N`
  feeding `vector[T,N]` yields a concrete lane count).
- A `kgen.param` analog (`a.param` attribute + `a.generator` op) would only
  pay off for *lazy* specialization or code-size control. A's pass already
  caps specializations (`kMaxSpecs`, comptime.cpp:1665) and errors on
  unbounded ones, so the AST approach is sufficient.
- Generics (`typeParams`, `FnDecl::typeParams`) are handled by Sema
  instantiation (`instantiateType`, sema.hpp:220): each call site gets a
  concrete `TypePtr`. The MLIR emitter sees post-instantiation types and
  emits one `a.func` per specialization - same as today.

Optional cheap addition (not required): stamp `a.func` with a
`comptime_params = ["N=4", ...]` string attr for debugging/`.ambc`
cache keys, reusing the specialization key comptime.cpp already computes.
Skip `a.generator`/`a.param` ops entirely in phase 2.

## 5. Pass enablement and estimated payoff

Once scalars are unboxed (`i64`/`f64`/`i1`/`vector` instead of tagged aval):

| Pass | Legal on | Payoff |
|---|---|---|
| `arith` canonicalization / `arith.addi` folding | unboxed `iN`/`fN` binops emitted as `arith.*` instead of `a_*_slow` calls | **Highest.** Today *every* `+`/`-`/`*`/compare is an opaque C call (`emitBinOp` -> `a_add_slow`...). Integer loop counters, index arithmetic, flags become foldable/vectorizable. Estimated: removes ~all runtime-call overhead in numeric kernels; enables LICM/CSE on loop bodies. |
| `mem2reg` / SROA (`createMem2Reg`, `createSROA` already in pipeline.cpp:70-71) | `a.alloca`+`a.load`/`a.store` of scalar `!a.ptr`-free slots; M3 `!a.struct`/`!a.tuple` layouts | Medium-high for locals: today every `let` is an alloca+boxed store. With typed allocas (`a.alloca` element_type already a TypeAttr) mem2reg promotes scalar locals to SSA; SROA splits fixed structs/tuples. |
| `scf`/`cf` canonicalization, `loop-invariant-code-motion` | `a.for` bounds/iter_args already `index`/`AnyType` | Medium: loop bounds are already `index`; typed iter_args let LICM hoist invariant boxed->unboxed conversions out of loops. |
| `vector` dialect lowering | `vector<T,N>` -> `vector<NxT>` | High for `simd`/`vector` code (stdlib kernels use it); native `<N x ty>` was already promised by types.hpp:45. |
| `ub` (undefined-behavior) checks opt-out | unboxed `a.from_aval` is unchecked | N/A - correctness relies on Sema, same as today. |

Emitter inventory driving the estimate (`mlir_emit.cpp`): `emitBinOp`
(~50 lines, all ops -> `a_*_slow`), `evalExpr` literal paths already compute
`avalIntEncode` inline for small ints, `a_getitem`/`a_setitem`/
`a_getattr`/`a_setattr` for all indexing/fields, `a_bi_call` for builtins,
`a_call` for user fns. Numeric `+ - * / %` and comparisons are the dominant
op count in real code; struct field access is second.

## 6. Migration path (each step independently shippable)

Gate every step on `A_MLIR_TYPED=1` (alongside existing `A_MLIR_NATIVE=1`)
until integration; `emitMLIR` text path and llvm fallback stay intact.

- **M1 - Scalar unboxing at expr level.** Add `mlirTypeOf(TypePtr)` mapper
  (Int/Float/Bool/Char/Vector -> native; rest -> `!a.aval`). In the `a`-dialect
  emitter (new, in-process - the `src/astra/mlir` dialect, not the text
  `mlir_emit.cpp`), emit `iN`/`fN`/`i1` values for `Expr::inferredType`
  scalars; binary/unary ops on two unboxed scalars emit `arith.*` ops
  directly; any operand/result crossing an aval boundary gets `a.to_aval` /
  `a.from_aval`. `a.func` `function.type` uses mapped types; `ALowerToLLVM`
  keeps boxed argv ABI (mode a, §3). Ships: faster arithmetic, zero ABI
  risk. ~the bulk of the win.
- **M2 - `a.alloca`/`a.load`/`a.store` typed slots.** Locals whose
  `VarTypeInfo.type` maps to a scalar get `a.alloca` with scalar
  `element_type` and typed load/store results (`ALoadOp`/`AStoreOp` already
  `AnyType`). Enables mem2reg on scalar locals. Verify `createMem2Reg`
  actually fires (it targets memref/LLVM-style allocas; may need a small
  `a.alloca`->`llvm.alloca` pre-pass or memref lowering first - check
  `ALowerToLLVM.cpp:620` element-type handling).
- **M3 - Aggregate types.** Parameterize `!a.struct<Name>` with field types,
  add `!a.tuple<...>`, `!a.optional<T>`, `!a.result<T,E>`; lower fixed
  structs/tuples to LLVM struct values `{T1,..,Tn}` behind SROA; `a.struct.
  getfield`/`setfield` become `llvm.extractvalue`/`insertvalue` when the
  object is unboxed, `a_getattr`/`a_setattr` when boxed. Optional/Result get
  `{i1, payload}` layout. Ships: SROA payoff, no calls for field access on
  unboxed aggregates.
- **M4 - Typed `a.func` ABI (opt-in).** Direct `llvm.func` signatures for
  non-extern, non-variadic, non-address-taken fns; `a.call` skips argv pack.
  Requires interprocedural analysis or a conservative syntactic check
  (address-taken = appears as value, passed to `a_bi_call`, fib, extern).
  Measure call overhead reduction before defaulting.
- **M5 - Heap types stay boxed forever.** `str`/`list`/`dict`/`set`/`bytes`/
  `fn`/`Any`/`Union` remain `!a.aval`; `a.array.get`/`a.struct.getfield`
  keep runtime calls when receiver is `!a.aval`, direct memory ops only
  under M3 unboxed aggregates. This is the steady state - matches Mojo.
- **M6 - `.ambc` bytecode.** Serialize post-M1 modules; type info is in the
  ops already, no format change needed beyond versioning.

Ordering rationale: M1 is self-contained (expr-level, no ABI change) and
delivers most of the arithmetic win; M2-M3 need the alloca/mem2reg path
verified; M4 is the riskiest (ABI) and last among optimizations; M5/M6 are
independent.

## Open questions

- Does `createMem2Reg` (MLIR 18) accept `a.alloca`? It is written for
  memref/`llvm.alloca`; verify in M2 and if needed lower `a.alloca` to
  `llvm.alloca` *before* the opt pipeline for promotable slots only.
- Sign extension on `a.from_aval -> iN`: the aval int encoding is 46-bit
  payload; i64 needs `a_i64n` heap path today - unboxed i64 must keep the
  same overflow-to-heap semantics or widen the inline limit. Decide whether
  unboxed i64 uses full 64-bit (breaking aval interop) or stays 46-bit
  (limiting). Recommend: unboxed i64 is a *different* representation; boxing
  boundary (`a.to_aval`) does the range check and falls back to `a_i64n`,
  exactly mirroring `avalInt` (`mlir_emit.cpp:234`).
- `Char`: confirm Sema never mixes `char` into `int` arithmetic without a
  cast; if it does, emit `a.from_aval`-equivalent zext at boundaries.
