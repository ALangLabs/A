# Diagnostics

`compiler/astra/frontend/diagnostics/` is the shared error/warning channel for
every frontend stage and for the language server.

## Files

- `diagnostics.hpp` / `diagnostics.cpp`

## Key types

- `using FileId = int` — index into the sink's source-file registry.
- `enum class Severity { Error, Warning }`.
- `struct Diagnostic` — `severity`, stable `code` (e.g. `"E0001"`–`"E0007"`
  errors, `"W0001"` warnings), `Span`, `message`, `help`.
- `class DiagnosticSink` — thread-safe (mutex-guarded; ty/pyrefly-style
  parallel checking emits from workers). API:
  - `addFile(std::shared_ptr<const SourceFile>)` → `FileId`; `file(id)`.
  - `error(span, msg, help)`, `errorWithCode(...)`, `warning(span, code, msg, help)`.
  - `isSuppressed(span, code?)` — implements `# a: ignore`, `# type: ignore`,
    `# ty: ignore`, `# pyrefly: ignore` trailing comments with optional
    `[CODE]` filter.
  - `hasErrors()`, `errorCount()`, `diagnostics()` (live vector — only when no
    writer is active), `diagnosticsCopy()` (thread-safe snapshot).
  - `flush(ostream, useColor)` — prints only diagnostics added since the last
    flush, so each pipeline stage can emit incrementally; `writeJson(ostream)`
    emits `{"diagnostics":[...]}` for `a check --json` and editor tooling.

## How it fits

Every stage (`Lexer`, `Parser`, `ModuleLoader`, `Sema`, `Analyzer`, tooling)
holds a `DiagnosticSink&` and reports spans rather than printing. The pipeline
calls `flush`/`writeJson` between stages; the LSP reads `diagnosticsCopy()` and
translates them into LSP diagnostics. Related: [Lexer](frontend-lexer.md),
[overview](overview.md).
