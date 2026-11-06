# reprlib

Port of CPython 3.14 `Lib/reprlib.py` (230 lines) to A.

## Coverage vs Python

| Python | Status |
| ------ | ------ |
| `Repr` class | `Repr(maxlevel, maxtuple, maxlist, maxarray, maxdict, maxset, maxfrozenset, maxdeque, maxstring, maxlong, maxother, fillvalue, indent)` — same positional field order; `.repr(x)` / `.repr1(x, level)` methods. |
| `repr` | `reprlib.repr(x)` module function backed by shared `aRepr` (CPython defaults). |
| `aRepr` | `reprlib.aRepr` shared default `Repr` instance. |
| `recursive_repr` | `recursive_repr(fillvalue)` returns a decorator producing a `RecursiveRepr` record; invoke via `.call([x])` (A has no decorators/callable objects — same `.call(args)` convention as `functools.LRUCache`). |
| `Repr._join` / `_repr_iterable` / `repr_tuple` / `repr_list` / `repr_set` / `repr_dict` / `repr_str` / `repr_int` / `repr_instance` | Ported as methods. |
| `repr_array` (array.array), `repr_frozenset`, `repr_deque` | **Not ported** — A has no `array.array`, `frozenset`, or `deque` equivalents. The `maxarray`/`maxfrozenset`/`maxdeque` fields exist for signature parity. |
| `_possibly_sorted` | **Not ported** — A cannot safely sort heterogeneous keys; dict/set order is insertion/storage order (CPython falls back to unsorted when sorting fails anyway). |

## Semantic deviations

- **Dispatch** is by runtime tag (`json.kind` + `json.as_array`/`as_map`
  probes + `str` prefix for sets/tuples) instead of `type(x)` +
  `getattr(self, 'repr_' + typename)`.
- **Sets and tuples** are decomposed through the `binary` codec
  (`encode -> decode -> Json array`) because A tuples cannot be iterated
  and sets have no uniform element access; elements surface as `Json`.
  Nested tuples/sets inside them degrade to lists.
- **Dict keys** print in map order, not sorted order.
- **`indent`** accepts a non-negative int (spaces per level) or `-1` for
  CPython's `None`; string indents unsupported.
- **Strings**: escapes cover `\\`, the quote char, `\n`, `\t`, `\r`;
  other control/non-ASCII characters pass through literally.
- **`recursive_repr`** requires the fillvalue argument (A has no default
  parameters — pass `"..."` for the CPython default) and guards on the
  argument *value*; containers hash by identity like CPython's `id()`,
  scalars by value (harmless — scalars cannot recurse).
- Exceptions have no A equivalent; `recursive_repr` with != 1 argument
  raises a runtime error via `core.panic`.
