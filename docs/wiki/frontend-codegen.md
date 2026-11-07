# Codegen (assembly backend)

`compiler/astra/frontend/codegen/` emits **x86-64 AT&T assembly** — the only
codegen backend. Generated code works on `aval` words (the runtime's NaN-boxed
value type) and calls `a_*` helpers from `compiler/astra/runtime/a_runtime.h`;
the `*_slow` externs implement the general cases that header fast paths don't
cover.

## Files

| File | Role |
| --- | --- |
| `codegen.hpp` | Public API: `bool emitAsm(const ModuleGraph&, const SemaResult&, std::string& out, std::string& error, bool testMode = false)`. `testMode` makes `main` discover/run top-level `fn test_*()` via the runtime `a_test_main` harness (`a test`). |
| `codegen.cpp` | `AsmEmitter::run` driver: `collectGlobals`, `buildGlobalScope`, `emitStructCtor`, `emitFunction`, `emitMain`, `emitLambdaFn`; `emitAsm` entry point. |
| `codegen_emitter.hpp` | `class AsmEmitter` (~1000 lines of emitter state/helpers): frame slots, scopes/bindings (`ABinding` kinds Local/Global/Param/Capture/FnValue/Builtin/StructCtor/EnumValue/Module/PyModule/PyFunc/NativeConst), `FnInfo`/`StructInfo`/`EnumVariantInfo` tables, label/slot counters, rodata/bss/text streams, `Argv` argv-block helpers, value materialization (`mkintA`, `emitIntLitA`, `emitBoxInt64`, `emitUnboxInt`, `emitAvalToI64`), SSE2 double fast paths (`emitDoublePairCheck`, `emitDoubleArith`, NaN canonicalization `kCanonNaN`), defer/drop scope frames, lambda queue (`PendingLambda`), and performance machinery: scalar promotion (`promoteNames_`, `specNames_` speculative raw int64 with deopt), `ARange` int64 interval analysis (`computeNameRanges`), raw register cache (`rawRegOf_`, `RawRegSuspend`), xmm register residency in loops (`loopXmm_`), small-function inlining (`InlineInfo`, `kMaxInlineCost=40`, `kMaxInlineSites=16`, `rawDepth_` bounds). |
| `codegen_analysis.cpp` | The analysis half of `AsmEmitter`: promotion set computation, range analysis fixpoint, inline candidacy scanning, raw-op safety proofs (`rawBinFastOk`, `rawCmpOk`, ...). |
| `codegen_expr.cpp` / `codegen_stmt.cpp` / `codegen_calls.cpp` / `codegen_decls.cpp` / `codegen_emit.cpp` | Expression/statement/call/declaration emission and low-level emit helpers (rodata strings, calls, builtins via `builtinRuntimeId`). |
| `codegen_internal.hpp` | Shared internals: `builtinRuntimeId(name)` (name → `A_BI_*` index matching `a_runtime.h`), `builtinIdForBuiltin`, `cEscape`, `utf8Encode`, `floatText`, `stemOf`, `FnInfo`/`StructInfo`/`EnumVariantInfo`, `BKind`. |

## ABI notes

- Functions are emitted with the uniform signature `aval f(aval *argv)` where
  `argv[0]` is the closure env (`A_NONE` for plain functions); `kMissing`
  (`A_TAG` payload 0) marks unsupplied defaulted parameters.
- Tagged-value constants mirror `a_runtime.h`: `kTag = 0xFFFF000000000000`,
  `kNone/kFalse/kTrue`, inline ints cover 47 bits (`[-2^46, 2^46)`).
- `builtinRuntimeId` must stay in sync with the `A_BI_*` enum order in
  `a_runtime.h` — both are ABI.

Emission failure returns `false` with an error string; there is no fallback
backend. Related: [AST](frontend-ast.md), [Type checker](frontend-type-checker.md),
[Runtime](runtime.md), [Astra backend](backend-astra.md),
[overview](overview.md).
