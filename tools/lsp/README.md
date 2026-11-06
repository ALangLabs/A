# A language server

`a-lsp` is the Language Server Protocol server for the A programming language.
It reuses the compiler front end (`lexer`, `parser`, `module loader`, and
`Sema`), so editor feedback matches `a check`.

## Features

| LSP method | Behaviour |
| ---------- | --------- |
| `textDocument/didOpen`, `didChange`, `didSave`, `didClose` | incremental document sync (kind 2), with full-document fallback |
| `textDocument/publishDiagnostics` | lex, parse, and type-check diagnostics with stable codes (`E0001`…), `codeDescription`, tags, and `relatedInformation` (help text). Suppressed by trailing `# a: ignore` / `# type: ignore` / `# ty: ignore` / `# pyrefly: ignore`, optionally with `[CODE]` |
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
| `textDocument/inlayHint` | inferred types (`: i32`) after unannotated variables |
| `textDocument/codeAction` | quick fixes: prefix unused variables with `_`, append `# a: ignore[CODE]` suppression |
| `textDocument/signatureHelp` | declared signatures for functions, builtins, native modules, and primitive methods, with active parameter |
| `workspace/symbol` | top-level symbols in open A documents |

The server is single-threaded, speaks JSON-RPC over stdin/stdout, and has no
third-party dependencies.

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

## Limitations

- Documents are reanalysed on every change (incremental sync is used, but
  analysis is still whole-document). This is fast for normal A files, but
  very large files may benefit from delay/debounce in the editor; the VS
  Code extension debounces changes for 250 ms.
- Type narrowing is limited to `Optional` None checks (`x != None` refines
  `x: T?` to `x: T` in the then-branch); full control-flow reachability and
  speculative-type materialisation from ty/pyrefly are not implemented.
- Range formatting, call/type hierarchies, and code lens are not implemented.
- Rename operates on resolved identifiers; module aliases and names that the
  front end cannot resolve are not renamed.
- Member completion falls back to all known struct members when the receiver
  type cannot be inferred.
