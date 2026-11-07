# Lexer

`compiler/astra/frontend/lexer/` turns source text into a token stream plus
collected comments. It is Python-flavored (significant indentation) with `#`
line comments.

## Files

| File | Contents |
| --- | --- |
| `lexer.hpp` / `lexer.cpp` | `class Lexer` — constructor takes text, `DiagnosticSink&`, `fileId`, optional `baseLine`/`baseCol`, and `inlineMode`; `tokenize()` returns `std::vector<Token>`; `comments()`/`takeComments()` expose captured comments. |
| `lexer_scan.cpp` | `scanIdentifier()`, `scanNumber()` (decimal/hex `0x`/binary `0b`/octal `0o`, `_` separators, float detection), `scanString()` (escapes, interpolation, triple-quoted raw strings), `scanChar()`. |
| `lexer_internal.hpp` | Shared internals incl. the keyword table. |
| `token.hpp` | `enum class Tok` (keywords `KwFn`…`KwImpl`, literals, operators), `struct Token` (kind, lexeme, `Span`, `strValue`, `intValue`, `floatValue`, `charValue`, `rawString`), `struct Comment` (`span`, `text`, `trailing`, `doc` for `##`), `tokName()`, `describeToken()`. |
| `source.hpp` / `source.cpp` | `class SourceFile` — immutable path+text with cached `lines_`; `lineText(int)`, `lineCount()`. Registered in the `DiagnosticSink` file table. |
| `span.hpp` | `struct Span` — half-open 1-based line/col region plus `fileId`; `Span::join`, `valid()`. |

## How it works

`Lexer` keeps `pos_`, `line_`, `col_`, `atLineStart_`, `bracketDepth_`, and an
`indentStack_` seeded with `0`. `handleLineStart()`/`emitNewline()`/`finalize()`
emit `Newline`/`Indent`/`Dedent` tokens; bracket depth suppresses newlines
inside `()[]{}`. `inlineMode` disables NEWLINE/INDENT/DEDENT for fragments such
as string-interpolation contents (the parser uses `parseInlineExpression()` for
those).

Comments are kept out of the token stream but recorded as `Comment`s so the
formatter, doc generator, and LSP can use them without re-scanning.

## Consumers

- `parser/` consumes `tokenize()` output.
- `tooling/` and `tools/lsp/` use `SourceText`/`SourceFile` and the comment list.
- Related: [Parser](frontend-parser.md), [Diagnostics](frontend-diagnostics.md),
  [overview](overview.md).
