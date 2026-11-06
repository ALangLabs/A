# std.heapq

Port of CPython 3.14 `Lib/heapq.py` (the pure-Python implementation; the C
accelerator only speeds it up, so nothing is lost semantically).

## Usage

```a
import heapq

h = []
heapq.heappush(h, 3)
heapq.heappush(h, 1)
heapq.heappop(h)              # 1
heapq.heapify([9, 1, 5])      # in-place
heapq.heappushpop(h, 4)
heapq.heapreplace(h, 2)
heapq.nlargest(3, data)
heapq.nsmallest_key(3, words, fn(s) => len(s))
heapq.merge([[1, 3], [2, 4]]) # -> [1, 2, 3, 4]
```

## API coverage vs Python

| Python | A | Notes |
| --- | --- | --- |
| `heappush`, `heappop`, `heapify`, `heappushpop`, `heapreplace` | same names | identical semantics, in-place on `[T]` |
| `heappush_max`, `heappop_max`, `heapify_max`, `heappushpop_max`, `heapreplace_max` | same names | max-heap variants |
| `_siftdown`, `_siftup`, `_siftdown_max`, `_siftup_max` | `_siftdown(heap, startpos, pos, lt)`, `_siftup(heap, pos, lt)` | take an extra `lt` comparator `fn(a,b)->bool` ("a pops before b") |
| `_heapq`, `_heappush_max`, `_heappop_max`, `_heapreplace_max`, `_heappushpop_max`, `_heapify_max`, `_heapify` | `_heappush_max`, `_heappop_max`, `_heapreplace_max`, `_heappushpop_max`, `_heapify_max`, `_heapify` | underscore aliases kept for compatibility |
| `merge(*iterables, key=None, reverse=False)` | `merge(iterables)`, `merge_key(iterables, key)`, `merge_reverse(iterables)`, `merge_key_reverse(iterables, key)` | `iterables` is an array of sorted arrays (no `*args`, no kwargs/defaults in A) |
| `nlargest(n, iterable, key=None)`, `nsmallest(n, iterable, key=None)` | `nlargest`, `nsmallest`, `nlargest_key`, `nsmallest_key` | key passed as a lambda `fn(x) => ...` |

## Semantic deviations

- **Generators -> lists.** `merge()` materializes and returns `[T]`.
- **Exceptions.** `heappop`/`heapreplace` on an empty heap are runtime
  errors (index/pop on an empty array), matching CPython's `IndexError`
  in spirit but without a catchable exception type.
- **No `key=`/`reverse=` kwargs, no `*iterables`.** Keyed and reverse
  variants are separate functions (see table). `merge` takes one array
  of arrays instead of variadic iterables.
- **Decorated tuples.** CPython decorates elements as `(key, order, elem)`
  tuples; A tuples are not indexable and arrays must be homogeneous, so
  keyed paths use small internal classes (`_KEntry`, `_MEntry`). Equal-key
  ordering still follows CPython's `order` tiebreaker via a sequence
  counter, so results match `sorted(..., key=...)`.
- Elements must be mutually comparable with `<`/`>` (ints, floats,
  strings). Mixed-type heaps are not supported, as in CPython they would
  raise `TypeError`.
