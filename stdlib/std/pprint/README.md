# pprint

Port of CPython 3.14 `Lib/pprint.py` (675 lines) to A.

## Coverage vs Python

| Python | Status |
| ------ | ------ |
| `PrettyPrinter` | `PrettyPrinter(indent, width, depth, compact, sort_dicts, underscore_numbers)` struct with `.pprint(object)` (stdout), `.pformat(object)` (string), `.isrecursive(object)`, `.isreadable(object)`. `depth <= 0` means unlimited (CPython `depth=None`). |
| `pprint` / `pformat` | Module functions with CPython defaults (indent=1, width=80, depth=None, compact=false, sort_dicts=true, underscore_numbers=false). |
| `saferepr` / `isreadable` / `isrecursive` | Module functions via `_safe_repr` (unlimited depth). |
| `pp` | `pp(object)` — pprint with sort_dicts=false, like CPython. |
| `_safe_repr` / `_format` / `_format_items` / `_format_dict_items` / `_repr` / `_pprint_dict` / `_pprint_list` / `_pprint_set` / `_pprint_str` | Ported as `PrettyPrinter` methods. `_pprint_tuple` is not provided (tuples can't be decomposed — see deviations). |
| `_safe_key` / `_safe_tuple` sorting | **Approximation** — keys are sorted only when every key is a string; otherwise insertion order (CPython's `_safe_key` fallback is also unsorted for unorderable types). |
| `_pprint_str` word wrapping | **Approximation** — over-width strings are parenthesized and chunked on character boundaries (no regex engine in A for CPython's `\S*\s*` splitting). |
| `_pprint_tuple`, `_wrap_bytes_repr`, `_builtin_scalars`, `sort_dicts` attribute | **Not ported** — no bytes/bytearray in A; tuples cannot be decomposed (see deviations). `sort_dicts` (the 3.14 dict-sorting helper) does not exist in the 3.14 source; the `sort_dicts` *parameter* is honored. |

## Semantic deviations

- **No `stream=` parameter** — `pprint` writes to stdout, `pprint.pprint`
  adds the trailing newline like CPython.
- **No `*args`/`**kwargs`** — optional parameters are only reachable through
  the `PrettyPrinter` constructor (positional fields, documented order).
- **Structs format as dicts** (`{'x': 1, 'y': 2}`), not `Name(x=1, ...)` —
  `json.as_map` treats structs and maps uniformly and there is no cheap,
  cycle-safe way to recover the struct name.
- **Sets** decompose via the `binary` codec to `Json` arrays.
  **Tuples cannot be decomposed** (un-iterable, un-indexable, and
  `binary.decode` round-trips them to an opaque `TupleValue`), so they
  render via their one-line `str()` repr — `(1, "x")` shape preserved,
  elements not recursed into.
- **Recursion markers** are `<Recursion on list|set|tuple|dict>` —
  CPython's `id=...` suffix is omitted (A has no stable object id).
- **Context keys** are the objects themselves; A hashes containers by
  identity (matching CPython's `id()` semantics), so cyclic
  lists/dicts/sets/structs are handled — verified for self-referential
  lists (`[<Recursion on list>]`). Deeply cyclic `Json`-wrapped values
  are not supported.
- **Strings**: repr escapes cover `\\`, quote char, `\n`, `\t`, `\r`;
  other characters pass through literally.
- Exceptions have no A equivalent; contract violations raise runtime
  errors via `core.panic`.
