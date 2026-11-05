# Typing Model (Explicit, Current + Target)

## Current semantics (HEAD)
A is currently **Python-style dynamic** in runtime behavior:
- Plain assignment creates/binds mutable variables in scope.
- `var`/`let`/`mut`/`const`/`pvt` are not required by current semantics.
- Type annotations are optional and not a complete static enforcement boundary.
- Arithmetic, comparisons, and many operations are resolved dynamically by the interpreter/backends according to implemented runtime behavior.
- Strings, arrays, maps, sets, structs/classes, and optional-like values follow the language/runtime behavior documented by tests and implementation, not stale prose.

**Source of truth:** implementation and passing tests (`tests/integration/cases/`), then `docs/language-spec/`, `docs/type-system/`, and `AGENTS.md`. `README.md` marketing may lag; do not treat it as authoritative where it conflicts.

## Design decision (rewrite/redesign target)
Target direction is **Mojo-inspired ergonomics with an explicit static core + dynamic boundary** (incremental, compatibility-aware):
- **Static core (target):** explicit type annotations on public APIs/stdlib boundaries where practical, trait/Result/compile-time parameter checking in the type checker, and specialization/monomorphization for codegen.
- **Dynamic boundary (compat):** preserve current dynamic behavior for existing scripts where annotations/checked generics are absent; do not silently break passing programs.
- **Mutability (target clarity):** make mutability explicit in docs and future syntax decisions (`var`-style mutable intent analog) while keeping current plain-assignment compatibility during migration.
- **Collections/numerics (target):** move toward predictable element typing and sized numeric intent where specified; runtime-dynamic containers remain a compatibility mode until migrated.
- **Safety boundary:** `unsafe` operations remain explicit; ownership/borrowing checks apply where modeled.

## Non-goals / constraints
- No claim of full static soundness until the checker + all backends enforce it and tests prove it.
- Compiler remains C++/CMake for the bootstrap/toolchain unless a separately justified migration plan is executed; Mojo is reference inspiration, not a required compiler implementation language.
- MLIR-dialect backend, real SIMD vector codegen, and real GPU codegen are architectural TODOs requiring backend/runtime work and toolchain dependencies (see `TODO.md`).
