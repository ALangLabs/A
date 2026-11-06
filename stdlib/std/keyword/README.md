# keyword

Port of CPython 3.14 `Lib/keyword.py` — a data module of language
keywords and membership predicates, adapted so the primary lists describe
**A's** keywords.

```python
import keyword

keyword.iskeyword("fn")        # true  — A keyword
keyword.iskeyword("def")       # false — not an A keyword
keyword.ispykeyword("def")     # true  — Python keyword
keyword.issoftkeyword("match") # true  — Python soft keyword
```

## API coverage

| CPython | A | Notes |
| --- | --- | --- |
| `kwlist` | `kwlist` | **A's** keyword list (36 entries, from the lexer keyword table — includes `Class`/`Metaclass` alias forms and `true`/`false`/`None` literals) |
| `softkwlist` | `softkwlist` | Python's soft keywords (`_`, `case`, `match`, `type`); A has no soft keywords |
| `iskeyword(s)` | `iskeyword(s)` | membership in `kwlist` (A keywords) |
| `issoftkeyword(s)` | `issoftkeyword(s)` | membership in `softkwlist` |
| — | `python_kwlist` | CPython `kwlist`, verbatim (35 entries) |
| — | `python_softkwlist` | CPython `softkwlist`, verbatim |
| — | `ispykeyword(s)` / `ispysoftkeyword(s)` | predicates over the Python lists |

## Semantic deviations

- `kwlist`/`iskeyword` describe **A**, not Python — this is A's `keyword`
  module. Use `ispykeyword`/`python_kwlist` when you need CPython's set
  (e.g. porting Python source-analysis tools).
- `iskeyword` is a plain function, not a `frozenset.__contains__`
  callable; call syntax `keyword.iskeyword("fn")` is identical.
- Case-sensitive membership, matching both CPython and A's lexer
  (`Class`/`Metaclass` are distinct keyword spellings, `class`/`metaclass`
  are separate entries).
- No `__all__`.
