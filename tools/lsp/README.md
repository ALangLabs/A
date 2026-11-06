# A language server

`a-lsp` is the Language Server Protocol server for the A programming language.
It reuses the compiler front end (`lexer`, `parser`, `module loader`, and
`Sema`), so editor feedback matches `a check`.

## Features

| LSP method | Behaviour |
| ---------- | --------- |
| `textDocument/didOpen`, `didChange`, `didSave`, `didClose` | incremental sync (kind 2) + **async type checking**: edits only snapshot text and enqueue a background job (200 ms debounce, latest-wins, stale jobs cancelled). Diagnostics publish from a worker thread without blocking requests; `didClose` cancels pending work |
| `textDocument/publishDiagnostics` | lex, parse, and type-check diagnostics with stable codes (`E0001`…), `codeDescription`, tags, and `relatedInformation` (help text), delivered asynchronously. Suppressed by trailing `# a: ignore` / `# type: ignore` / `# ty: ignore` / `# pyrefly: ignore`, optionally with `[CODE]` |
| `textDocument/definition`, `declaration`, `typeDefinition`, `implementation` | variables, parameters, functions, structs, fields, methods, enum variants, imported modules |
| `textDocument/references` | all uses of the symbol under the cursor, including imported modules |
| `textDocument/hover` | declaration signature/type, inferred type (`x: i32  (inferred)`) for unannotated variables, definition link, and attached `##` documentation |
| `textDocument/completion` | keywords (with snippets for `if`/`while`/`for`/`fn`), builtins (with docs), visible locals (with inferred types), module/struct/enum members |
| `textDocument/documentSymbol` | functions, structs/classes, fields, methods, enums, variants, and top-level bindings |
| `textDocument/formatting` | full-document formatting through the same AST formatter as `a fmt` |
| `textDocument/rename`, `textDocument/prepareRename` | symbol rename across declaration and all references; prepare returns the identifier range |
| `textDocument/documentHighlight` | every occurrence of the symbol under the cursor |
| `textDocument/foldingRange` | foldable function, class, loop, match, and conditional bodies |
| `textDocument/selectionRange` | identifier → enclosing indent block expansion |
| `textDocument/semanticTokens/full`, `textDocument/semanticTokens/range` | semantic tokens (namespace/type/class/enum/function/method/parameter/variable/property/keyword/comment/string/number/operator) with declaration modifiers |
| `textDocument/inlayHint` | inferred types (`: i32`) after unannotated variables, parameter names at call sites (`a:`, `b:`) |
| `textDocument/codeAction` | quick fixes: prefix unused variables with `_`, append `# a: ignore[CODE]` suppression |
| `textDocument/signatureHelp` | declared signatures for functions, builtins, native modules, and primitive methods, with active parameter |
| `textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`, `callHierarchy/outgoingCalls` | call hierarchy for functions and methods (AST-based, works across modules) |
| `textDocument/prepareTypeHierarchy`, `typeHierarchy/supertypes`, `typeHierarchy/subtypes` | type hierarchy for classes (`class(Base)`) and traits (`trait C(A)`) |
| `textDocument/codeLens` | "N references" lens above functions, methods, classes, and enums (jump via `editor.action.showReferences`) |
| `textDocument/rangeFormatting` | formats the requested range only (whole-document format + line diff, edits clipped to the range) |
| `textDocument/diagnostic` | LSP 3.17 pull diagnostics (`kind: "full"`), same items as `publishDiagnostics` |
| `textDocument/moniker` | document monikers (`scheme: "a"`, unique = canonical path) |
| `textDocument/documentLink` | clickable `http(s)://` URLs inside comments |
| `workspace/symbol` | top-level symbols in open A documents |

Protocol handling runs on the main thread (JSON-RPC over stdin/stdout) while
type checking runs on a background worker (async, debounced), with stdout
guarded so replies and `publishDiagnostics` never interleave. Requests that
need the symbol index call `ensureCompiled()` — they reuse the latest
background result or compile on demand when a fresh edit is still
debouncing. Multi-module documents are checked in parallel (`Sema::analyzeParallel`,
worker pool; unify serialised so results match the serial checker).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target a-lsp -j
```

The executable is `build/a-lsp`. The editor extension finds it automatically
when it lives in `build/` (or another `build-*` directory) at the workspace
root. Otherwise set `a.server.path` to its absolute path, or put `a-lsp` on
`PATH`.

Run it directly to check that it starts:

```sh
build/a-lsp --version
```

Editors normally start the server themselves. For manual debugging:

```sh
build/a-lsp --stdio
```

Set `A_LSP_LOG=1` to write lifecycle messages to stderr.

## Layout

- `analysis.hpp` / `analysis.cpp` — symbol index, scopes, definition/reference
  resolution, and compilation wrapper.
- `json.hpp` / `json.cpp` — minimal JSON parser/serializer for LSP messages.
- `server.cpp` — stdio JSON-RPC loop and LSP request handlers.
- `smoke_test.py` — end-to-end protocol smoke test.

## CLI parallel checking

`a check --parallel` (or `-j N` / `--jobs=N`, `A_JOBS=N`) checks modules in
parallel on a worker pool. Diagnostics and exit codes are identical to the
serial checker; ordering of messages may differ for multi-module programs.

## Limitations

- Documents are reanalysed in the background (incremental sync is used, but
  analysis is still whole-document per edit). The server debounces edits for
  200 ms so typing does not thrash the checker.
- Type narrowing is limited to `Optional` None checks (`x != None` refines
  `x: T?` to `x: T` in the then-branch); full control-flow reachability and
  speculative-type materialisation from ty/pyrefly are not implemented.
- `textDocument/references` honours `context.includeDeclaration`; rename still
  operates on resolved identifiers only.
- Call hierarchy covers named functions and methods; calls through function
  values, lambdas assigned to variables, and builtin/native callees are not
  linked. Top-level (module scope) calls have no enclosing callable and are
  skipped.
- Type hierarchy follows `class(Base)` inheritance and `trait` parents; it does
  not yet model `impl Trait for Type` conformance as a subtype edge.
- Range formatting diffs whole-document formatter output against the source
  and emits only hunks fully inside the requested range; partially overlapping
  hunks are skipped.
- Code lens counts resolved references across all modules in the compilation;
  it is computed per request (no caching).
- Rename operates on resolved identifiers; module aliases and names that the
  front end cannot resolve are not renamed.
- Member completion falls back to all known struct members when the receiver
  type cannot be inferred.
