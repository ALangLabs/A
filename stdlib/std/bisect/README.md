# std.bisect

Pure-A port of CPython 3.14 `Lib/bisect.py` (`/usr/lib/python3.14/bisect.py`).
The C accelerator (`_bisect`) only changes speed, not semantics, so the
pure-Python fallback algorithms are mirrored exactly.

Import with `import bisect` or `from bisect import bisect_left, ...`.

## Coverage vs Python

| CPython API | Status | A equivalent |
|---|---|---|
| `bisect_left(a, x, lo=0, hi=len(a), *, key=None)` | Ported | `bisect_left(a, x)` and `bisect_left_range(a, x, lo, hi)` |
| `bisect_right(a, x, lo=0, hi=len(a), *, key=None)` | Ported | `bisect_right(a, x)` and `bisect_right_range(a, x, lo, hi)` |
| `insort_left(a, x, lo=0, hi=len(a), *, key=None)` | Ported | `insort_left(a, x)` and `insort_left_range(a, x, lo, hi)` (mutates `a`) |
| `insort_right(a, x, lo=0, hi=len(a), *, key=None)` | Ported | `insort_right(a, x)` and `insort_right_range(a, x, lo, hi)` (mutates `a`) |
| `bisect = bisect_right`, `insort = insort_right` | Ported | `bisect(a, x)`, `bisect_range(a, x, lo, hi)`, `insort(a, x)`, `insort_range(a, x, lo, hi)` |
| `key=` callable | Missing | A has no keyword arguments; callers pre-key their data |
| `lo < 0` -> `ValueError` | Mapped | `core.panic` (runtime error), matching stdlib contract-violation style |

## Semantic deviations

- **No default parameters.** A does not support `lo=0, hi=len(a)` defaults or
  keyword-only `key=`. Each function therefore comes in a 2-argument form
  (whole list) and a `*_range` form (explicit `lo`/`hi` bounds).
- **Lists are `[T]`.** `a.insert(pos, x)` has Python list semantics including
  negative/out-of-range clamping; `insort_*` mutate the caller's array
  (arrays have reference semantics).
- **`hi` is not clamped.** Like CPython, an out-of-range `hi` surfaces as an
  index error at the midpoint probe rather than being silently clamped.
- Works on any ordered comparable element type the language supports
  (numbers, strings), same as CPython's duck-typed `<` comparisons.
