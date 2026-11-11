# TODO: Dual Native AOT + Bytecode/VM Backends

Status: **Planning checklist (no compiler/runtime implementation done yet).**  
Tracks the dual-backend work: keep `a build` native (`.o`/exe), add VM/interpreted path for `a run` (and selected commands), with **flags for both**.

## 0. Decisions (lock before coding)

- [ ] Choose flag UX and precedence: `--vm` / `--native` (and/or `--run-backend=vm|native`); define interaction with legacy `--backend` compat no-op.
- [ ] Decide per-command defaults: `build` (native-only), `run`, `test`, `repl`, `-c`/`runSource`. Preserve golden-test behavior.
- [ ] Decide bytecode lowering source: **AST+Sema+ModuleGraph** (parity-first) vs **IR** (cleaner, requires closing `Opaque`/coverage gaps). Record spike results.
- [ ] Decide VM value/runtime strategy: reuse NaN-boxed `aval` + `compiler/astra/runtime` helpers/ABI vs isolated VM object model (prefer reuse for parity).
- [ ] Decide REPL semantics under VM: fresh-eval-per-input (current behavior) vs persistent state (phase 2).
- [ ] Decide bytecode cache naming/namespace in `_acache_` and versioning/key components (must not collide with native `.o`/run/pre caches).

## 1. Config / CLI / Driver / Pipeline plumbing

- [ ] Add explicit backend enum to `compiler/astra/frontend/driver/driver.hpp` (`DriverConfig`) and `compiler/astra/frontend/pipeline/pipeline.hpp` (`PipelineConfig`).
- [ ] Parse `--vm`/`--native` (final names) in `compiler/astra/frontend/cli/main.cpp`; update `printUsage`/help text.
- [ ] Validate flag compatibility (e.g. `a build --vm` rejected until bytecode output exists; VM + incompatible `--target`/cross/i8086 execution diagnosed clearly).
- [ ] Thread backend selection through driver entry points (`runFile/runSource/testFile/buildFile/repl*`) into pipeline calls.
- [ ] Add/adjust unit or integration coverage for flag parsing/help where the repo expects it.

## 2. Pipeline dispatch and native isolation

- [ ] In `executePipeline`/`testPipeline`/`replEval` (`pipeline.cpp`), branch after successful `runFrontendPipeline`: native path unchanged, VM path to new executor.
- [ ] Keep `buildPipeline` on the native AOT path (`.o`/exe via `buildSplitUnits` or single-unit emit + Astra).
- [ ] Ensure VM path bypasses native-only steps: Astra assemble/link, split-unit `.o` cache reuse as native artifacts, run-binary cache exec (unless a separate VM run-cache is explicitly designed).
- [ ] Preserve diagnostic formatting, `.diag` behavior where applicable, and exit-code semantics used by `tests/integration/run_tests.sh`.

## 3. Bytecode format and compiler (VM backend)

- [ ] Create VM/bytecode module(s) under `compiler/astra/frontend/` (or chosen location) with clear CMake integration.
- [ ] Define minimal bytecode/ISA: constants, locals/temps, arithmetic/comparisons, jumps/branches, calls/returns, globals/module init hooks, basic containers as needed.
- [ ] Implement bytecode lowering for the spike subset: literals, `let`/assign, arithmetic, `if/while`, `fn`/calls, `print`/core builtins, `entry`.
- [ ] Map A function calling convention to VM frames with `aval`/env compatibility (closure env in `argv[0]` model parity where required by runtime).
- [ ] Integrate module graph/import/init order and entry-module behavior.
- [ ] Extend coverage to: strings/arrays/maps/tuples/sets/ranges, methods/closures, pattern constructs, error/panic behavior, ownership/runtime-observable semantics as needed for parity.

## 4. VM runtime bridge

- [ ] Audit required runtime surface from `compiler/astra/runtime/a_runtime.h`, `a_runtime_internal.h`, `a_runtime.c`, and native codegen emission sites (`codegen_emit.cpp`, `codegen_calls.cpp`, `codegen_native.cpp`).
- [ ] Implement builtin dispatch and runtime helper calls for `A_BI_*` / `g_builtins` and core `a_*` operations.
- [ ] Support native-module and `std:`/python/FS/net/json/bytes behaviors through the same runtime capabilities where required.
- [ ] Verify NaN-boxing/int/string/container invariants match native behavior.

## 5. `a test` and REPL parity

- [ ] Support `a test` discovery of top-level `test_*`, `--filter`, and reject parameterized `test_*` with the same diagnostics/behavior as native path.
- [ ] Match test harness output/exit behavior for golden cases.
- [ ] Implement VM path for REPL: start with fresh-eval-per-input parity; document/flag any persistent mode added later.
- [ ] Confirm TTY self-hosted REPL behavior (`stdlib/std/repl/main.a`) under selected backend flags.

## 6. Caching (`_acache_`)

- [ ] Add VM bytecode artifact cache (separate namespace from native `.o`/`.sym`/`run.*`/`pre.*` artifacts).
- [ ] Key VM cache by backend, bytecode/VM version, source/module graph/deps, relevant flags/target constraints, and runtime/tool inputs as needed.
- [ ] Ensure `A_NO_CACHE=1` disables VM caching; `A_CACHE_DIR` behaves predictably for CI.
- [ ] Add/extend cache tests (model after `tests/integration/cache_test.sh`) without breaking existing native cache tests.

## 7. Tests and docs

- [ ] Add focused integration cases for `--vm`/`--native` (small `.a` programs; golden `.out/.err/.exit` as needed).
- [ ] Run native-path regression checks: existing integration suite behavior, `cache_test.sh`, IR/tooling/LSP smokes as relevant.
- [ ] Run VM-path golden subsets; expand to full parity targets incrementally.
- [ ] Update `docs/wiki/cli.md`, `docs/wiki/compiler.md`/`overview.md` as needed, and mention backend flags, defaults, and `_acache_` VM artifacts.
- [ ] Update `AGENTS.md` only if project conventions/commands for this workflow materially change (keep accurate).

## 8. Performance & follow-ups (after correctness)

- [ ] Benchmark `a run` startup/iteration (VM vs native, cold/hot cache) and CPU-bound runtime.
- [ ] Profile VM dispatch hotspots; consider threaded dispatch/superinstructions only after correctness.
- [ ] Evaluate optional JIT/AOT tier or bytecode-to-native fast path later (separate plan).

## Commands to use during implementation (reference)

```sh
cmake --build build -j$(nproc) --target a
ctest --test-dir build --output-on-failure
tests/integration/run_tests.sh build/a
tests/integration/cache_test.sh build/a
```
