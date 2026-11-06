# Compiler Internals (A v2)

This document summarizes the current implementation architecture. For exact behavior, consult source under `compiler/stages/` and `CMakeLists.txt`.

## High-level pipeline

1. **Diagnostics / Source management** — source files, spans, diagnostic sink.
2. **Lexer** — indentation-aware tokenization, literals (int/float/hex/binary/string/char), keywords/operators, comments/doc comments.
3. **Parser** — builds AST for expressions, statements, declarations, patterns, modules, and type expressions.
4. **Name resolution / module loading** — resolves imports (`import`, `from ... import`), native modules, stdlib/prelude auto-load, module graph.
5. **Type checking / semantic analysis** — type inference/checking, builtin/native module typing, method signatures, trait/impl checks, symbol/module export tables.
6. **Ownership / static analyses** — ownership/borrow-related analysis hooks (implementation-specific; runtime also enforces/tracks in places).
7. **IR / optimization** — typed SSA IR emission and optional optimization passes.
8. **Backend / execution** — Astra (`compiler/astra/`, the builtin TinyCC fork) compiles and links `.a` sources directly; `a run`/`a build`/`a test` delegate to the `astra` driver.

## Astra backend

- The backend is **Astra** (`compiler/astra/`): a forked TinyCC that also contains a first-party A→x86-64/arm64 compiler (`a/`).
- `a run`, `a build`, and `a test` invoke the `astra` driver (found via `A_ASTRA`, `<exe>/tcclib/astra`, or `PATH`); `astra link` links objects into an executable.
- Python interop sources live in `compiler/astra/python/` (built as `a_python_interop`).

## Key directories

```text
compiler/
  stages/
    cli/              # `a` command-line entrypoint
    driver/           # pipeline/driver API
    diagnostics/      # spans, sources, diagnostics
    lexer/            # tokenizer
    parser/           # parser
    ast/              # AST node definitions
    name-resolution/  # module loader, native modules, builtin docs
    type-checker/     # sema, types, method signatures
    ownership/        # ownership analysis
    ir/               # IR structures/lowering support
    optimization/     # optimization passes
    pipeline/         # frontend/pipeline orchestration
    tooling/          # formatter, docgen

runtime/              # runtime support libraries/data structures
stdlib/               # A standard library (prelude + std packages)
tests/                # integration, IR, tooling/CLI tests
tools/lsp/            # a-lsp server
```

## Native modules and builtins

- Native modules are declared in `name-resolution/native_modules.hpp` and mapped to builtin implementations.
- Builtin documentation used by tooling/LSP is defined in `name-resolution/builtin_docs.hpp`.
- The type checker recognizes known builtins, constants, conversions, and native module members.
- Native-backed stdlib families (by prefix/name mapping): `json_*`, `binary_*`, `testing_*`, `log_*`, `metrics_*`, `trace_*`, `net_*`, `http_*`.

## Module loading

- The loader can auto-load `stdlib/prelude.a` (or `std/prelude` variants) before the entry module.
- Stdlib discovery prefers repository `stdlib/` (walk-up/CWD heuristics) and supports `A_STDLIB` / `--stdlib`.
- `A_PATH` and module search paths (`-I`) influence module resolution.

## Testing layout

- `tests/integration/cases/*.a` + `.out/.err/.exit/.in/.flags`: golden language tests.
- `tests/ir/ir_tests.cpp`: IR tests.
- `tests/tooling/tooling_tests.cpp`, `tests/tooling/run_cli_tests.sh`: tooling/CLI tests.
- `tools/lsp/smoke_test.py`: LSP smoke test.

## Source of truth

When docs and implementation disagree, prefer:

1. integration tests (`tests/integration/cases/`)
2. parser/lexer/AST/type checker/module loader source
3. generated docs/tooling tables in `name-resolution/*` and `type-checker/*`
