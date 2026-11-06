# std.operator

Pure-A port of CPython 3.14 `Lib/operator.py`
(`/usr/lib/python3.14/operator.py`, the pure-Python implementation; the C
accelerator `_operator` only changes speed).

Import with `import operator` or `from operator import add, ...`.

## Coverage vs Python

| CPython API | Status | A equivalent / notes |
|---|---|---|
| `lt le eq ne ge gt` | Ported | identical wrappers over `< <= == != >= >` |
| `not_` | Ported | `not truth(a)` (see truth caveat) |
| `truth` | Approximation | repr-based truthiness; see deviations |
| `is_`, `is_not` | Approximation | `==` / `!=` (A has no identity operator) |
| `is_none`, `is_not_none` | Approximation | `str(x) == "None"`; the string `"None"` itself also reports true |
| `abs` | Ported | delegates to `math.abs` |
| `add sub mul truediv floordiv mod neg pos inv/invert lshift rshift and_ or_ xor pow` | Ported | thin wrappers; `floordiv`/`mod`/`truediv` correct A's truncating `/`/`%` to Python floor/true-division semantics |
| `index` | Approximation | integral numbers only (`int()` + range check); panics otherwise |
| `concat` | Ported | strings via `+`, other iterables copied element-wise into a fresh array |
| `contains` | Ported | `b in a` (reversed operands, like CPython) |
| `countOf` | Ported | `==`-based count; strings use `.count` (substring, not per-char) |
| `delitem` | Ported | `a.pop(b)` — index for arrays, key for maps |
| `getitem`, `setitem` | Ported | `a[b]`, `a[b] = c` |
| `indexOf` | Ported | first `==` index; strings use `.index` (substring); panics like `ValueError` |
| `length_hint` | Partial | `len(obj)`; no `default=` argument (A has none); non-sized objects error instead of returning the default |
| `itemgetter(item)` | Ported | returns `fn(obj) => obj[item]` |
| `itemgetter(*items)` | Adapted | `itemgetter_all(items)` returns a callable producing an **array** of items (CPython returns a tuple) |
| `attrgetter`, `methodcaller`, `call` | Missing | require `getattr`/dynamic dispatch/`*args` — none exist in A |
| `matmul` (`@`) | Missing | no `@` operator in A |
| `iadd iand iconcat ifloordiv ilshift imod imul imatmul ior ipow irshift isub itruediv ixor` | Missing | A has no in-place rebinding operators; use `a += b` etc. directly |
| `__add__ = add` etc. dunder aliases | Missing | A has no special-method protocol; the plain names are the API |
| `repeat` | A addition | `repeat(a, n)`/`repeat_seq(a, n)` — sequence repetition (`a * n` in CPython is spelled `mul` there); not part of CPython's `operator` |

## Semantic deviations (language-level)

- **No exceptions.** Contract violations (`concat` on non-sequences,
  `indexOf` miss, `index` on non-integers) raise runtime errors via
  `core.panic`, matching stdlib style.
- **No `is` identity.** `is_`/`is_not` use `==`/`!=`; for `None` checks this
  coincides with Python semantics for the supported types.
- **`truth`/`not_` are approximations.** A has no general truthiness test and
  `json.kind`/`comptime.type_tag` would pin parameter types to `Json`
  (breaking every other operation on the value), so `truth` compares
  `str(x)` against falsy representations (`""`, `"None"`, `"false"`, `"0"`,
  `"0.0"`, `"-0.0"`, `"[]`, `"{}"`). Strings that literally equal one of those
  reprs (e.g. `"0"`, `"None"`, `"[]"`) are misreported as falsy; structs and
  function values are unsupported.
- **`== None` inside function bodies miscompiles** under the native backend,
  so `None` detection goes through `str(x) == "None"`.
- **Type-checker limitation:** passing an untyped parameter through another
  function and then using it in typed operations can hang semantic analysis;
  every function here is therefore self-contained (probes inlined).
- **`floordiv`/`mod`** implement Python semantics on top of A's truncating
  `/` and C-style `%`: `floordiv` floors toward `-inf`, `mod` takes the
  divisor's sign, `truediv` always returns `float`.
- **`pow`** covers only the two-argument form.
- **Callable objects** (`itemgetter` results) are A lambdas/closures; they
  cannot be inspected or re-bound like Python callables.
