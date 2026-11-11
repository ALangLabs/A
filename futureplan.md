# Future Plan: Dual Execution Backends (Native AOT + Bytecode/VM)

Date: 2026-11-09  
Status: **Planning only (no implementation in this change).**

## Goal

Support **both execution systems** with explicit flags:

- **`a build`** keeps the current behavior: compile A to **native code** and produce **`.o`/executable** artifacts via the existing AOT path (frontend → codegen emit asm/native → Astra assemble/link + C runtime).
- **`a run`** (and where appropriate `a test`/`repl`) can run via a **bytecode/VM interpreter path** for fast iteration, while still allowing the **native AOT** path when requested.
- Users select backend explicitly with flags (both available), e.g. `--vm` / `--native` (final naming decided during implementation; keep legacy `--backend` compatibility in mind).

This is a **major** addition: the repo currently has **no interpreter/VM/bytecode execution backend**. Execution is frontend → asm/native codegen → Astra → native binary (cached in `_acache_`).

## Current Architecture (facts to preserve)

| Layer | Key paths | Notes |
|---|---|---|
| CLI | `compiler/astra/frontend/cli/main.cpp` | Parses commands/flags; `--backend` is currently a compatibility no-op; dispatches to driver. |
| Driver | `compiler/astra/frontend/driver/{driver.hpp,driver.cpp}` | `runFile/runSource/buildFile/testFile/emitIRFile/repl...`; maps `DriverConfig` → `PipelineConfig`. |
| Pipeline | `compiler/astra/frontend/pipeline/{pipeline.hpp,pipeline.cpp}` | Frontend orchestration; target resolution; native emit (`emitAsm`/`emitNative`); Astra compile/link/run; run/precheck cache; split-unit per-module object cache in `_acache_`; i8086/QEMU path. |
| Frontend | `compiler/astra/frontend/{lexer,parser,ast,name-resolution,type-checker,ownership,ir,optimization,codegen,tooling,...}` | Shared semantic analysis produces `CompiledModule{ModuleGraph,SemaResult}`. |
| IR | `compiler/astra/frontend/ir/*`, `optimization/*` | Typed SSA-ish IR + optimizer; used by `a ir` (`emitIRPipeline` lowers/optimizes/prints). **AOT execution codegen bypasses IR** and consumes AST+Sema. IR lowering currently returns `Opaque` for constructs it cannot represent. |
| Native codegen | `compiler/astra/frontend/codegen/*` | `emitAsm`/`emitAsmPerUnit` (x86-64 SysV AT&T), `emitNative`/`emitNativePerUnit` (other arches/ABIs, incl. Win64 x86-64). Emits runtime calls (`a_*`) directly. |
| Astra backend | `compiler/astra/backend/`, `cmake/libastra.cmake`, `build/astra/`, `build/astralib*/` | Assembles/links staged `.s` + C runtime into native binaries/boot images. |
| Runtime | `compiler/astra/runtime/{a_runtime.h,a_runtime_internal.h,*.c,i8086_runtime.s}` | NaN-boxed 64-bit `aval` value model; A function convention `aval f(aval *argv)` (`argv[0]` = env); builtin table/IDs (`A_BI_*`, `g_builtins`); collections/strings/bytes/fs/json/os/net/time/python/linalg/etc. Linked into native programs. |
| Module loading | `compiler/astra/frontend/name-resolution/*` | Prelude auto-load, quoted/bare/`std:`/native/python resolution, module graph/order. |
| Caching | `pipeline.cpp` (`run_cache`, `objcache`), `tests/integration/cache_test.sh` | `_acache_` next to sources by default; `A_CACHE_DIR` override for CI/tests; run/precheck binary cache + per-module object/sym cache; `A_NO_CACHE`, `A_SPLIT_UNITS`, `A_DEBUG_UNITS`. |
| Tests | `tests/CMakeLists.txt`, `tests/integration/{run_tests.sh,cache_test.sh,cases/**}` | Golden integration cases (`.a/.out/.in/.err/.exit/.flags`), run-cache test, IR/tooling tests, LSP smokes.

## Proposed Design

### 1. Backend selection (flags for both)

Add an explicit execution backend enum/config (do **not** overload the legacy `--backend` compat no-op ambiguously):

- Config plumbing: `cli/main.cpp` (`Options`) → `compiler/astra/frontend/driver/driver.hpp` (`DriverConfig`) → `compiler/astra/frontend/pipeline/pipeline.hpp` (`PipelineConfig`).
- Candidate flags: `--vm`, `--native` (mutually exclusive unless last wins; define clearly). Possibly `--run-backend=vm|native` for scripting; keep `--backend` accepted as compat where currently accepted, but document behavior.
- **Defaults (decide explicitly, test-gated):**
  - `a build`: **native only** (produces `.o`/exe). Reject `--vm` for `build` unless/until bytecode artifact output is designed.
  - `a run`: default to **VM** *or* keep native default initially behind a transition flag — choose based on parity/stability. Plan for both flags being available either way.
  - `a test`: support both; default must preserve golden-test observable behavior (stdout/stderr/exit, filter, entry/test discovery).
  - `a repl`: support both; preserve current fresh-eval semantics by default or make persistence explicit. TTY self-hosted REPL (`stdlib/std/repl/main.a`) is an A program — define how backend flag applies.
  - `a check`/`fmt`/`doc`/`ir`: unaffected (frontend/tooling), except `ir` may later gain bytecode dump if useful.

### 2. Pipeline integration seam

After `runFrontendPipeline` produces a valid `CompiledModule{ModuleGraph,SemaResult}` (reusing module loading, prelude, native/python/`std:` resolution, sema, ownership):

- **Native path (existing):** `resolveTarget`/native emit/Astra/link/run caches/split-unit object cache as today. `--target`, `A_CODEGEN`, `A_SPLIT_UNITS`, cross/i8086 remain native-specific.
- **VM path (new):** bypass Astra assemble/link and native run/object caches. Execute in-process via VM/bytecode engine. Reuse diagnostics/exit-code conventions expected by integration tests.
- **Target handling:** VM is host-interpreted; reject or clearly diagnose incompatible cross-target execution (`--target` != host semantics) unless a future design says otherwise. `a build` remains target-aware/native.

### 3. Bytecode/VM implementation strategy

Prefer **runtime ABI parity** over inventing a separate object model.

- **Value model:** use the existing NaN-boxed `aval` representation and runtime helpers (`compiler/astra/runtime/a_runtime.h` + internal/runtime impl) wherever feasible so strings/arrays/maps/bytes/closures/builtins behave the same as native.
- **Function/builtin bridge:** VM must call builtins and runtime helpers through the same ABI concepts (`A_BI_*` ids/table, `aval`/`argv` conventions, function env/call path). Audit native codegen (`codegen_emit.cpp`, `codegen_calls.cpp`, `codegen_native.cpp`) and runtime exports for the exact required surface.
- **Bytecode source of truth (key decision):**
  - *Option A (recommended for parity-first):* lower bytecode from **AST + `SemaResult` + `ModuleGraph`**, mirroring how `emitAsm`/`emitNative` work. More implementation work, but aligns with current execution semantics and avoids IR `Opaque` gaps.
  - *Option B:* lower from **IR** (`ir::lowerProgram`) for a cleaner VM ISA, but only after closing IR coverage (closures, pattern matching, comprehensions, and any other `Opaque` cases) and proving parity. Can leverage `optimization` passes.
  - *Decision gate:* implement a small spike (constants/arithmetic/calls/control flow/functions) before committing.
- **Bytecode cache:** store VM artifacts in `_acache_` (next to source, `__pycache__`-like naming) but **namespaced separately** from native `.o`/run binaries (e.g. `*.abc`/`*.vm*` or a `vm/` sub-layout) and keyed by backend kind + bytecode/VM version + source/module graph/deps/flags that affect semantics. Never reuse a native binary/object cache entry for VM execution.

### 4. Module/native/stdlib/Python parity

- Reuse `ModuleLoader` graph, prelude auto-load, `-I`/`A_PATH`/`A_STDLIB`/apm package paths, `std:` and native module resolution (`native_modules.hpp`).
- Honor module init order and entry/top-level behavior (`a run` executes top-level; entry-point semantics use the current `entry ...():`/`isEntry` model — do not reintroduce legacy assumptions).
- Support `test_*` discovery, `--filter`, parameter rejection, and exit/output behavior expected by `tests/integration/run_tests.sh`.
- Python/native/OS/net/fs/json/bytes features must route through the same runtime capabilities where parity is required; document any VM-only limitations explicitly if temporarily unavoidable (but avoid for golden-tested behavior).

### 5. REPL strategy

- **Phase 1 (low risk):** VM-backed REPL keeps the current “fresh compile/run per eval” model (no persistent bindings), matching existing behavior.
- **Phase 2 (optional):** persistent in-process VM state with explicit module/global/reset semantics and tests.

### 6. Observability and safety

- Clear diagnostics when backend/target/flag combinations are invalid.
- `A_NO_CACHE` disables VM bytecode/run caches too (or has VM-specific equivalent).
- Debug env vars (e.g. `A_DEBUG_VM`, `A_DEBUG_BYTECODE`, `A_TRACE_VM`) — names TBD.
- Keep `cache_test.sh` valid; add VM cache parity/invalidation tests separately.

## Risks

- **Semantic parity:** the biggest risk. Native codegen and runtime have subtle behavior (NaN-boxing, ints, strings, closures, ownership hooks, test harness, module init, builtins).
- **IR gaps:** IR is not execution-complete today (`Opaque` for some constructs). AST-lowering VM avoids this but duplicates lowering effort.
- **Runtime coupling:** codegen emits direct `a_*` calls; VM must map cleanly to runtime/public internals without destabilizing AOT.
- **Cache poisoning/cross-backend reuse:** VM and native artifacts must be isolated by backend and versioned keys.
- **Performance expectations:** VM `a run` should improve iteration/startup in common cases, but CPU-bound runtime will not match native AOT without JIT/AOT tiers.
- **Target/i8086/cross complexity:** keep VM out of bare-metal/cross-native execution paths initially.

## Milestones

1. **Design & plumbing:** backend flags/config/docs; no behavior change by default (or carefully chosen default).
2. **VM spike:** execute a tiny subset (literals, arithmetic, `print`, `if/while`, simple `fn`/calls, `entry`) using runtime `aval` helpers.
3. **Core parity:** control flow, functions/closures, collections/strings, modules/imports, exceptions/panics, test harness basics.
4. **Stdlib/native parity:** builtin/native module/runtime-backed features; run integration golden subsets under `--vm`.
5. **Caching & polish:** `_acache_` bytecode cache, diagnostics, REPL mode decision, docs, CI coverage.
6. **Default flip (optional):** only after parity and performance criteria are met.

## Non-goals (initially)

- Replacing the native AOT backend or removing `.o`/exe output from `a build`.
- Full cross-target VM execution semantics for all native targets/i8086.
- JIT/native tiering (future work after a correct VM exists).
- Claiming Python-identical bytecode internals; the goal is Python-like *workflow* (fast `run`, cached bytecode/artifacts) with A semantics.
