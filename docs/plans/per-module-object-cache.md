# Plan: per-module compilation units + object cache ("recompile only what changed")

Status: **implemented** (Phases 1–3 landed; see deltas below). Owner: compiler/codegen + pipeline. Reversible at every phase.

## Implementation status (as built)

The design below shipped with these deltas from the original proposal —
`pipeline.cpp` (`objcache`/`run_cache` namespaces) and `codegen_decls.cpp` are the source
of truth:

- Per-module split units are **on by default**; `A_SPLIT_UNITS=0` (or
  `off`/`no`/`false`) reverts to the single-unit `emitAsm`/`emitNative` path.
- Unit keys live in `unitKeyWithCtx`/`KeyContext`: besides `source(M)`,
  `sighash(M)`, and transitive `sighash(D)` deps, every unit key also mixes a
  **whole-graph scope-name hash** (`scopeNamesHash` over every module's
  top-level `let`/`assign`/`fn`/`struct`/`enum` names, in `g.modules` order),
  because `buildGlobalScope` resolves name collisions across the flat global
  namespace. The entry unit additionally keys on `g.order` (module-init call
  order) and the `a test` filter.
- `mtag` is `stableTag()` of the module's **weakly-canonicalized path**
  (`AsmEmitter::moduleTag`), matching §1.
- **Cache layout (now `_acache_`, `__pycache__`-like):** by default, artifacts
  live in `_acache_/` next to source modules (per-module `<key>.o`/`.sym`, and
  run/precheck binaries near the entry, e.g. `run.<key>`, `pre.<key>`).
  `A_CACHE_DIR` overrides with a central root for CI/tests (run entries in
  `$A_CACHE_DIR`, objects in `dirname($A_CACHE_DIR)/obj`, preserving legacy
  override layout for compatibility).
- `A_NO_CACHE=1` disables caching; `A_DEBUG_UNITS=1` prints per-unit
  emit/reuse counts; exercised by `tests/integration/cache_test.sh`.

## Goal

Editing one A module should recompile (emit + assemble) **only that module**, the
way CPython recompiles only the changed `.pyc`. Today `a run`/`a build` emits one
`.s` for the whole program and assembles it whole.

### Measured baseline (x86-64, this box, `A_NO_CACHE=1`)

| stage | hello (2 modules) | `std:http` program (7 modules) |
|---|---:|---:|
| read + lex/parse/resolve/check (`a check`) | 10 ms | 30 ms |
| emit x86-64 asm | ~20 ms | ~100 ms (106,263 lines) |
| assemble (astra) | ~8 ms | 120 ms |
| link + write | ~2 ms | 20 ms |
| **total** | 40 ms | **270 ms** |

89% of the cost is emit + assemble of the imported modules' code. Frontend is not
the problem, so the win must come from splitting the *backend* unit, not from
caching the frontend.

### Acceptance criteria

1. Cold build unchanged or faster (`std:http`: <= 270 ms).
2. Edit one leaf module (`stdlib/std/bytes/bytes.a`): full rebuild <= 60 ms
   (target ~35 ms), emitting only that module.
3. Edit the entry only: <= 60 ms.
4. Edit a module's *interface* (add/remove a decl): dependents rebuild
   (correctness), so no silent stale output.
5. All existing gates green: `ctest` 6/6, integration 116/116, 57 stdlib
   selftest groups, both benchmark suites within noise, `a fmt --check`.

## Why the obvious alternatives do not work (checked)

| alternative | result |
|---|---|
| dead-code pruning of unused prelude/stdlib functions | **impossible**: all 72 functions are address-taken (`lea __a_fN` for their `fnon` closure), so nothing is provably dead |
| GNU `as` instead of astra's assembler | **slower**: 250 ms vs 120 ms on the same `.s` |
| `--gc-sections` for smaller/faster links | **unavailable**: astra links with its internal ELF linker (`backend/astraelf.c`), no section GC |
| caching the frontend (`.pyc` analogue) | **wrong layer**: 30 ms of 270 ms |
| whole-program object cache (current run cache) | already exists; a one-module edit still recompiles everything |

Prerequisites that **are** supported (verified):

- astra links multiple objects: `astra main.o mod.o … libaruntime.a -o out` works.
- astra accepts `.L` labels and keeps them object-local, so per-unit label
  namespaces need no linker changes.
- global symbol creation is centralised in 3 places (`collectGlobals`,
  `newLabel`, `emitStrData` / `emitMain`), so the renaming surface is small.

## Design

### 1. Symbol scheme: names derived from (module, declaration), never from program order

Current names are assigned in program order (`__a_f<fnId++>`), so a cached object
from an earlier build would silently reference a different function today. New
scheme:

| symbol | new form | notes |
|---|---|---|
| function | `__a_f_<mtag>_<dh>` | `mtag` = 64-bit hash of the module's canonical path (hex), `dh` = hash of decl name + arity |
| method | `__a_m_<mtag>_<sh>_<dh>` | `sh` = hash of the struct name |
| function fnon global | `__a_gf_<mtag>_<dh>` | |
| struct ctor / its fnon | `__a_ctor_<mtag>_<sh>` / `__a_gs_<mtag>_<sh>` | |
| enum constant | `__a_ev_<mtag>_<eh>_<vh>` | enables duplicate enum names across modules |
| module global (`let`/`assign` at top level) | `__a_top_<mtag>_<nh>` | `nh` = hash of the variable name |
| local control-flow labels | `.L__a_<pfx><n>` | `t`-local in the object; no cross-unit collisions, smaller symbol tables |
| string/literal data | `.L__a_d<n>` | same |

Stability requirement: a name must not change when *other* declarations are
added to or removed from the module (that is why `dh` hashes the declaration
rather than using a per-module index), and must not depend on module ordering in
the graph (that is why `mtag` hashes the path).

Collision handling: hash collision or duplicate decl names get a deterministic
index suffix, ordered by source position, and are recorded in the unit's
interface hash so dependents rebuild.

### 2. Unit decomposition

Each module becomes one assembly unit:

- in: its functions, methods, struct ctors, enum data, its `__a_top_*` globals,
  its literals, and a new `__a_modinit_<mtag>` function holding its top-level
  statements (today those are inlined into `main` by `emitMain`, so `emitMain`
  must be split into "emit my module init" + "emit main").
- entry unit additionally holds `main`: `a_runtime_init`, `a_frame_push`, the
  `a_fnon_new_ex` setup table for all structs/functions/enums, the `a test` table
  when applicable, and calls to every `__a_modinit_<mtag>` in topological order
  (prelude first, then modules in load order — the order `ModuleGraph::order`
  already provides).

Cross-unit references are ordinary extern symbols; nothing else needs to move.

### 3. Interface hash and cache key

Per module M compute `sighash(M)` = hash of its *exported declarations*:
kind, name, arity, variadic flag, comptime params, struct field names/order/base,
method names, enum variants/values, top-level `let` names, plus its native/python
import set (they change emitted code).

Unit key:

```
key(M) = H( source(M), opt level, target, codegen version,
            sighash(D) for every D in transitive-imports(M) )
```

Transitive closure (not just direct imports) is required because a struct's
layout/field order defined in D is baked into the code of any module that
touches it, possibly through a re-export chain.

Invalidation semantics (stronger than CPython's mtime+size):

| edit | units re-emitted |
|---|---|
| body of M changes | M only |
| interface of M changes | M + every module that transitively imports M |
| M's transitive import added/removed | M |
| file appears that shadows an import | affected module (content-hash based precheck already handles the diagnostic side) |
| runtime `.c`/`.h`, astra, compiler | everything (version/env in the key) |

### 4. Cache layout and pipeline

```
$XDG_CACHE_HOME/a/obj/<key>.o        # one object per module unit
$XDG_CACHE_HOME/a/obj/<key>.sym      # optional: interface hash for debugging
$XDG_CACHE_HOME/a/run/<binhash>      # existing whole-program binary cache (unchanged)
```

Pipeline (`pipeline.cpp`, `compileAndRun`/`build` paths):

1. frontend as today (30 ms; `a check` unchanged).
2. for each module in `graph.order`: compute `key(M)`; if `<key>.o` exists, skip;
   else emit that unit's `.s` (temp, in cache dir) and assemble to `<key>.o`.
3. link: `astra <entry>.o <mod>.o … <libaruntime.a> libastra1.a -o out`.
4. keep the existing binary cache on top so an unchanged program is still a
   single `execve`.

Assembly of independent units runs in the existing job pool (`A_JOBS`), which is
a bonus: cold builds gain parallelism even with a empty cache.

### 5. Expected numbers

| scenario | today | after |
|---|---:|---:|
| cold, empty cache (`std:http`) | 270 ms | ~270 ms (or ~180 ms with parallel assembly) |
| warm, nothing changed | 1–10 ms | 1–10 ms (binary cache, unchanged) |
| edit entry only | 270 ms | **~50 ms** (entry unit + link) |
| edit `bytes.a` body | 270 ms | **~35 ms** (one unit + link) |
| edit a struct's fields in `bytes.a` | 270 ms | ~120 ms (bytes + dependents) |

## Phases

Each phase is independently landable and keeps the whole-program path as a
fallback (`A_SPLIT_UNITS=0` default until phase 3 lands).

### Phase 1 — stable symbols (no behaviour change)

- `codegen_decls.cpp`: replace `fnId++`/`stId++`/`counter_++` naming in
  `collectGlobals` with the scheme in §1; add `moduleTag(mod)` helper.
- `codegen_emit.cpp`: `newLabel()`/`emitStrData()` emit `.L`-prefixed labels.
- Keep single-unit output.

Gate: identical runtime behaviour; `ctest` 6/6; integration 116/116; stdlib
selftests unchanged (same pass/fail set as today, including the 3 known-bad
groups); benchmarks within noise; `a-lsp` smoke (symbol names appear in
diagnostics? check `tools/lsp/smoke_test.py`).

### Phase 2 — per-module emission behind a flag

- `AsmEmitter`: add `emitUnit(moduleId, out)` that emits only that module's
  globals/functions/ctors/enums/literals plus `__a_modinit_<mtag>`; split
  `emitMain` into `emitModuleInitBody(moduleId)` + `emitMain()`.
- `codegen.cpp`: `emitAsmPerUnit(graph) -> vector<{moduleId, asmText}>`.
- `pipeline.cpp`: emit N `.s`, assemble each (`astra -c`), link all; `A_JOBS`
  parallelism. Gate the old single-file path with `A_SPLIT_UNITS=0`.

Gate: full suite with the flag on; binary size within 5% of today; both
benchmark suites within noise (asm layout changes may move numbers ±3%);
explicit check that module-init topological order matches today (test with
globals that depend on imported module state — add a golden case
`tests/integration/cases/module_init_order.a`).

### Phase 3 — object cache + interface hashing

- new `run_cache::unitKey(...)` and `unitCachePath(...)` (reuse `mixBytes`
  hashing helpers from `pipeline.cpp`).
- emit/assemble only on a key miss; reuse `<key>.o` otherwise.
- GC of the object cache (bounded size/age) mirroring whatever policy the binary
  cache gets.
- `A_NO_CACHE=1` bypasses phase 3 as well.

Gate: `tests/integration/cache_test.sh` extended with (a) edit body -> exactly
one unit re-emitted (assert via a debug counter/`A_DEBUG_UNITS=1`), (b) edit
interface -> dependent units re-emitted, (c) shadowing-file invalidation,
(d) cache-off parity: `A_NO_CACHE=1` output byte-equal to cached output for a
set of programs.

## Risks and mitigations

| risk | mitigation |
|---|---|
| stale cached unit (silent miscompile) — the one failure mode that matters | names are content-derived (§1); keys include transitive interface closure (§3); `A_NO_CACHE=1` parity test in phase 3 gate; cache key includes compiler/runtime identity |
| label collision between units | all local labels become `.L`-local (verified astra keeps them object-local); exported names are hash-scoped |
| module init order regression | inits stay in one place (`main`) and call per-module init functions in `graph.order`; dedicated golden case |
| `a test` table / test filter | test table is built in the entry unit and keys on the filter (existing cache key already includes it) |
| cross-compilation (`--target arm64 …`) | same split; units are per target (target in the key); staged `astra-<arch>` driver unchanged |
| concurrent edits by another process in `compiler/astra/frontend/codegen/` | phase 1 touches `codegen_decls.cpp`, `codegen_emit.cpp`; those are currently clean while `codegen_native.cpp`, `codegen_internal.hpp`, `ir*`, `sema*` are dirty — coordinate before starting, or land phase 1 first while the others are frozen |
| rollback | `A_SPLIT_UNITS=0` returns to the single-unit path; each phase is one commit, revertible independently |

## Effort

| phase | size | risk |
|---|---|---|
| 1 stable symbols | ~120 lines changed in 3 files | low (renaming; loud link errors if wrong) |
| 2 per-unit emission + link | ~350 lines across emitter + pipeline | medium (unit boundaries, init order) |
| 3 object cache | ~250 lines (key, reuse, GC, tests) | medium (invalidation correctness) |
| parallel assembly (independent bonus) | ~80 lines, split the single `.s` at function boundaries | low |

## Open questions for the owner

1. Go/no-go on phase 2 touching `codegen_emitter.hpp`/`emitMain` while another
   process has codegen work in flight.
2. Object-cache eviction policy: size-capped LRU vs "delete on `a clean`" only.
3. Should phase 3 also cache *frontend* module summaries later (an
   `.aobj` format with serialised decls) to cut the remaining 30 ms?
