# Parser

`compiler/astra/frontend/parser/` is a hand-written recursive-descent parser
that consumes `std::vector<Token>` and produces a `Program` AST. Split across
focused translation units.

## Files

| File | Role |
| --- | --- |
| `parser.hpp` | `class Parser` public interface: `parseProgram()`, `parseInlineExpression()` (for string interpolation fragments). Private helpers cover statements, declarations, expressions, patterns, call args, string parts, destructuring. |
| `parser_internal.hpp` | Internal helpers shared across the .cpp files. |
| `parser_core.cpp` | Token cursor primitives: `cur/peek/check/match/advance/expect`, `fail()` → throws `ParseError`, `synchronize()` for error recovery, `previousSpan()`. |
| `parser_decl.cpp` | Declaration parsing: `parseFnDecl`, `parseStructDecl` (`class`/`struct`/`metaclass`), `parseEnumDecl`, `parseTraitDecl`, `parseImplDecl`, `parseTypeAliasDecl`, params, docstring statements. |
| `parser_stmt.cpp` | Statement dispatch: `if`/`elif`/`else`, `while`, `do..while`, `for..in`, `loop`, `return`, `break`/`continue`, `defer`, imports, let/typed declarations, destructuring, simple statements. |
| `parser_expr.cpp` | Pratt expression parser: `parseExpression(minPrec)`, `parsePrefix`, `parsePostfix`, `parseLambda`, `parseMatch`, `parseCallOrIndexPostfix`, `parseCallArgs`, `binaryPrecedence`/`isRightAssociative`/`binOp`. |
| `parser_expr_primary.cpp` | `parsePrimary`: literals, identifiers, parenthesized exprs/tuples, array/map/set literals, ranges. |
| `parser_pattern.cpp` | `parsePattern`/`parsePrimaryPattern`: literal, range, wildcard, variant, binding, tuple, or-patterns for `match`. |
| `parser_string.cpp` | `parseStringParts`: splits string literals into literal `StringPart`s and interpolated expression parts; `decodeEscapes`. |
| `parser_type.cpp` | `parseTypeExpr`: named/qualified types, arrays `[T]`/`[T; N]`, `T?`, tuples, `map<K,V>`, `set<T>`, function types. |

## Design notes

- Errors are reported through `DiagnosticSink& sink_`; `fail()` throws
  `ParseError` and `synchronize()` skips to the next statement boundary.
- Docstrings: a bare string-literal statement inside a type/impl body is stored
  via `parseDocstringStatement` / `docstringText` helpers (see
  `ast/stmts.hpp`).
- `parseInlineExpression()` re-enters expression parsing on a sub-token stream
  for `${...}`/interpolation fragments.
- Output is `std::unique_ptr<Program>` (`ast/stmts.hpp`): a flat list of
  `StmtPtr`s; declaration kinds keep structured children (`FnDecl::body`,
  `StructDecl::fields/methods`, ...).

## Consumers

`ModuleLoader` parses every module; `tooling::formatSource` and
`tooling::extractDocs` parse single files for `a fmt`/`a doc`; the LSP parses
open documents. Related: [AST](frontend-ast.md),
[Name resolution](frontend-name-resolution.md),
[overview](overview.md).
