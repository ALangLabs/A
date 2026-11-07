# Frontend tooling (fmt / doc)

`compiler/astra/frontend/tooling/` implements the source-level tools behind
`a fmt` and `a doc`, plus shared pretty-printers used by the LSP.

## Files

| File | Contents |
| --- | --- |
| `formatter.hpp` / `formatter.cpp` | `a::tooling::FormatOptions` (`indentWidth` default 4, `maxLineWidth` 100), `FormatResult`, `formatSource(source, sink, options)` → canonical formatting or `ok=false` on parse errors (never overwrites broken code). |
| `formatter_internal.hpp` / `formatter_comments.cpp` / `formatter_expr.cpp` / `formatter_stmt.cpp` / `formatter_output.cpp` | Comment attachment (leading/trailing/`##` doc), expression and statement layout, final output assembly. |
| `docgen.hpp` / `docgen_extract.cpp` / `docgen_render.cpp` / `docgen_internal.hpp` | `a doc`: `DocOptions`, `DocResult`, `DocModule`/`DocItem`/`DocMember`/`DocParameter` model (kinds Function/Class/Struct/Metaclass/Enum/Trait/TypeAlias/Constant/Variable), `extractDocs` (from `##` comments and docstrings), `renderMarkdown`, `renderJson`. |
| `ast_text.hpp` / `ast_text.cpp` | Shared signature printers: `typeText`, `paramText`, `paramsText`, `typeParamsText`, `signatureText`, `declarationKeyword` (recovers `var`/`class`/`struct`/`metaclass` spelling the AST normalizes). Used by formatter, docgen, and LSP. |
| `source_text.hpp` / `source_text.cpp` | `class SourceText` — byte-oriented source view mapping 1-based line/col spans back to substrings (`offsetOf`, `slice`, `between`, `firstChar`), `trimCopy`. |

## How it fits

`driver::formatFile`/`documentFile` (`driver/driver.cpp`) read the file, run
`formatSource`/`extractDocs`, and write stdout, `-w` in-place, `-o` output, or
`--json`. `a fmt --check` exits nonzero when the file is not canonical;
`a fmt` is idempotent (covered by `tests/tooling/tooling_tests.cpp`).
Related: [Parser](frontend-parser.md), [AST](frontend-ast.md),
[LSP](tools-lsp.md), [overview](overview.md).
