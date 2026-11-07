# Compiler frontend

`compiler/astra/frontend/` is the C++20 implementation of the A language
toolchain (`namespace a`). It is compiled into two binaries:

- **`a`** — the CLI driver (`frontend/cli/main.cpp` → `driver/driver.cpp` →
  `pipeline/pipeline.cpp`).
- **`a-lsp`** — the language server, which links the frontend libraries and
  adds its own semantic index in `tools/lsp/`.

The umbrella header `frontend/a.hpp` re-exports diagnostics, lexer sources,
spans, tokens, the driver, the AST, and sema/types for embedders.

## Stage overview

```mermaid
flowchart LR
    src["source text"] --> lex["lexer/"] --> tok["tokens + comments"]
    tok --> parse["parser/"] --> ast["ast/"]
    ast --> load["name-resolution/<br/>ModuleLoader"] --> graph["ModuleGraph"]
    graph --> sema["type-checker/<br/>Sema"] --> sres["SemaResult"]
    sres --> own["ownership/<br/>analyzeOwnership"]
    own -->|"check only"| done["diagnostics"]
    own -->|"ir command"| ir["ir/ + optimization/"]
    own -->|"run/build/test"| cg["codegen/<br/>emitAsm"] --> asm[".s assembly"]
```

`runFrontendPipeline` in `pipeline/pipeline.cpp` performs module loading →
`Sema::analyze()` (or `analyzeParallel`) → `analyzeOwnership`, flushing
diagnostics between stages. `a check` stops there; `a fmt`/`a doc` go through
`tooling/`; `a ir` additionally runs `ir::lowerProgram`; `a run`/`build`/`test`
add `emitAsm` and the Astra invocation.

## Subsystem pages

| Page | Directory | Role |
| --- | --- | --- |
| [Lexer](frontend-lexer.md) | `lexer/` | Source files, spans, tokens, `Lexer::tokenize()` |
| [Parser](frontend-parser.md) | `parser/` | Recursive-descent parser producing the AST |
| [AST](frontend-ast.md) | `ast/` | Node hierarchy, `ExprKind`/`StmtKind`/`BinOp`, `BuiltinId` |
| [Diagnostics](frontend-diagnostics.md) | `diagnostics/` | `DiagnosticSink`, severities, suppression comments |
| [Name resolution](frontend-name-resolution.md) | `name-resolution/` | `ModuleLoader`/`ModuleGraph`, native module table, builtin docs |
| [Type checker](frontend-type-checker.md) | `type-checker/` | `Sema`, Hindley-Milner-style unification, builtin domains |
| [Ownership](frontend-ownership.md) | `ownership/` | Flow-sensitive move/borrow/view analysis |
| [IR](frontend-ir.md) | `ir/` | Typed SSA lowering (`lowerProgram`) |
| [Optimizer](frontend-optimization.md) | `optimization/` | LLVM-style pass pipeline over the IR |
| [Codegen](frontend-codegen.md) | `codegen/` | `AsmEmitter`: AST → x86-64 AT&T assembly |
| [Tooling](frontend-tooling.md) | `tooling/` | `a fmt` formatter, `a doc` doc generator, shared printers |
| [Driver, CLI, pipeline](frontend-driver.md) | `driver/`, `cli/`, `pipeline/` | Commands, options, REPL, astra invocation |

## Conventions

- C++20, `namespace a`; headers sit next to implementations; each stage is its
  own CMake target (`a_lexer`, `a_parser`, `a_diagnostics`, ... see the
  per-directory `CMakeLists.txt`).
- Warnings are errors in CI (`-DA_WARNINGS_AS_ERRORS=ON`).
- Diagnostics flow through a shared `DiagnosticSink`; every stage flushes only
  newly added diagnostics.
- Spans are 1-based line/column pairs with a `fileId` into the sink's file
  registry, so multi-module programs report locations correctly.
