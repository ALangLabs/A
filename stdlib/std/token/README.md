# token

Port of CPython 3.14 `Lib/token.py` — the token-type constants module.

> **This is a Python-compatibility vocabulary layer, not A's tokenizer.**
> A's real lexer lives in the compiler
> (`compiler/stages/lexer/lexer.cpp`, `Tok` in `token.hpp`) and produces
> language tokens (keywords, identifiers, literals, punctuation), not the
> CPython token-type numbering exported here. Use this module when writing
> tools that consume CPython `tokenize`/`token` output or port Python
> parser code.

```python
import token

token.NAME            # 1
token.tok_name[1]     # "NAME"
token.exact_token_types["%="]   # 40
token.isterminal(token.NAME)    # true
token.isnonterminal(token.NT_OFFSET)  # true
token.iseof(token.ENDMARKER)    # true
```

## API coverage

| CPython | A | Notes |
| --- | --- | --- |
| `ENDMARKER`..`N_TOKENS` (70 constants) | same names/values | verbatim, incl. `NT_OFFSET = 256` |
| `tok_name` | `tok_name` | `{i64: String}` map, values 0..69 plus 256 — same entries CPython derives from `globals()` |
| `EXACT_TOKEN_TYPES` | `exact_token_types` + `EXACT_TOKEN_TYPES` alias | `{String: i64}` map of punct → token code |
| `ISTERMINAL(x)` | `isterminal(x)` + `ISTERMINAL(x)` | |
| `ISNONTERMINAL(x)` | `isnonterminal(x)` + `ISNONTERMINAL(x)` | |
| `ISEOF(x)` | `iseof(x)` + `ISEOF(x)` | |

## Semantic deviations

- `tok_name` is built explicitly from `_token_names` (the contiguous
  0..69 range) plus `NT_OFFSET`, rather than by scanning `globals()` —
  identical contents.
- Map key misses are A map errors (CPython raises `KeyError` — same
  failure mode).
- Predicate names are provided in both snake_case (stdlib style) and
  CPython's UPPERCASE spelling.
- No `tokenize`/`untokenize`/`generate_tokens` — CPython implements those
  in `Lib/tokenize.py` on top of the tokenizer; porting them is out of
  scope for this constants module.
- No `__all__`.
