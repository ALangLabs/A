# A toolchain wiki

Technical documentation for the A language toolchain in this repository:
the `a` compiler driver, the C++ frontend, the Astra (TinyCC-fork) backend,
the C runtime, the standard library, kernels, editor tooling, tests, and
examples.

Start with [overview.md](overview.md) for the architecture and the
source → assembly → native-binary pipeline diagram.

## Pages

### Architecture

- [Overview](overview.md) — system architecture, pipeline diagram, repo layout, build/test.

### Compiler frontend (`compiler/astra/frontend/`)

- [Frontend](frontend.md) — stage overview and index of the frontend modules.
- [Lexer](frontend-lexer.md) — `lexer/`: `SourceFile`, `Span`, `Token`, `Lexer::tokenize()`.
- [Parser](frontend-parser.md) — `parser/`: recursive-descent parser, `Parser::parseProgram()`.
- [AST](frontend-ast.md) — `ast/`: node hierarchy, `ExprKind`/`StmtKind`/`BinOp`, `BuiltinId`.
- [Diagnostics](frontend-diagnostics.md) — `diagnostics/`: `DiagnosticSink`, severities, `# a: ignore` suppression.
- [Name resolution](frontend-name-resolution.md) — `name-resolution/`: `ModuleLoader`, `ModuleGraph`, native modules, builtin docs.
- [Type checker](frontend-type-checker.md) — `type-checker/`: `Sema`, unification, builtin domains, `SemaResult`.
- [Ownership](frontend-ownership.md) — `ownership/`: `analyzeOwnership`, move/borrow/view checks.
- [IR](frontend-ir.md) — `ir/`: typed SSA lowering, `lowerProgram`, `a ir`.
- [Optimizer](frontend-optimization.md) — `optimization/`: LLVM-style passes and `OptLevel` pipeline.
- [Codegen](frontend-codegen.md) — `codegen/`: `AsmEmitter`, AST → x86-64 AT&T assembly.
- [Tooling](frontend-tooling.md) — `tooling/`: `a fmt` formatter, `a doc` generator, shared printers.
- [Driver, CLI, pipeline](frontend-driver.md) — `driver/`, `cli/`, `pipeline/`: commands, options, REPL, astra invocation.

### Backend and runtime

- [Astra backend](backend-astra.md) — `compiler/astra/backend/`: TinyCC fork, `astra` driver, `libastra` API, per-target codegen, object writers, `lib/` support.
- [A runtime](runtime.md) — `compiler/astra/runtime/`: `aval` NaN-boxed value model, `a_runtime.h` API, domain modules, python interop.

### Libraries and language ecosystem

- [Standard library](stdlib.md) — `stdlib/`: `prelude.a`, `std/` packages, import forms.
- [Kernels](kernels.md) — `kernels/`: self-hosted numeric kernels in A.

### Tooling

- [a-lsp](tools-lsp.md) — `tools/lsp/`: stdio LSP server, semantic index, handlers, smoke test.
- [vscode-a](tools-vscode-a.md) — `tools/vscode-a/`: VS Code extension.
- [Supporting tools](tools-scripts.md) — `tooling/`, `tools/gen-docs`, `tools/gen_docs.py`, `tools/benchmark-runner`, `websearchcli.py`, CI.

### Quality and examples

- [Tests](tests.md) — `tests/`: CTest targets, integration golden suite conventions.
- [Examples](examples.md) — `examples/`: runnable programs by category.
- [Benchmarks](benchmarks.md) — `benchmarks/`: timed workloads and cross-language equivalents.
