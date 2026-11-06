# std.functools

Pure-A port of a feasible subset of CPython 3.14 `Lib/functools.py`
(`/usr/lib/python3.14/functools.py`, 1165 lines; the C accelerator
`_functools` only speeds up `reduce`/`partial`/`lru_cache`/`cmp_to_key`, so
the pure-Python fallback semantics are what is mirrored here).

Import with `import functools` or `from functools import partial, ...`.

## Coverage vs CPython

| CPython API | Status | A equivalent |
|---|---|---|
| `partial(func, /, *args, **kw)` | Partial | `partial(func, bound_array)` + `partial1`..`partial4`; result is a `Partial` record called via `.call(args)` |
| `reduce(function, iterable[, init])` | Ported | `reduce(function, xs)` (first element seeds; panics on empty — CPython raises `TypeError`), `reduce_init(function, xs, init)` |
| `cache` | Ported | `cache(func)` -> `LRUCache`, invoked via `.call(args)` |
| `lru_cache(maxsize=None, typed=False)` | Partial | `lru_cache(func, maxsize)` (`maxsize <= 0` = unbounded); `typed=` ignored (A has no per-type key separation) |
| `LRUCache.cache_info()` | Ported | `.cache_info()` -> `CacheInfo` struct (hits, misses, maxsize, currsize) instead of a namedtuple |
| `LRUCache.cache_clear()` | Ported | `.cache_clear()` |
| `LRUCache.cache_parameters()` | Missing | — |
| `cmp_to_key(cmp)` | Adapted | `sort_by(xs, cmp)` — stable merge sort returning a NEW array (CPython `sorted(..., key=cmp_to_key(cmp))` analogue). `cmp(a,b)` returns negative/0/positive |
| `singledispatch(func)` | Simplified | `singledispatch(func)` -> `Dispatcher`; `.register(tag, fn)`, `.dispatch(tag)`, `.dispatch_value(x)`, `.call(args)`. Flat registry keyed by runtime tag strings (`"int"`, `"float"`, `"string"`, `"bool"`, "array", `"object"`, `"null"`) derived from `json.kind` |
| `total_ordering` | Skipped | Class decorator; A has no decorators and no dunder-method dispatch |
| `wraps` / `update_wrapper` / `WRAPPER_ASSIGNMENTS` / `WRAPPER_UPDATES` / `assigned` / `updated` | Skipped | Requires `__name__`/`__dict__`/`__module__` attributes on function objects, which A functions do not have |
| `cached_property` | Skipped | Requires the descriptor protocol (`__get__`/`__set_name__`), which A classes do not implement |
| `singledispatchmethod` | Skipped | Depends on `singledispatch` + descriptor protocol |
| `cache`/`lru_cache` `.cache_parameters` | Missing | — |
| `_CacheInfo`, `_HashedSeq`, `_make_key`, `RLock`, `partialmethod`, `singledispatch` internals | Skipped/N.A. | Implementation details of CPython; see deviations |

## Semantic deviations (language-level, apply to all APIs)

- **No decorators.** `cache`/`lru_cache`/`singledispatch` are called as
  ordinary functions returning wrapper records, not applied with `@`.
- **No `*args`/`**kwargs`/splat.** Call arguments are passed as arrays:
  `p.call([x, y])`, `c.call([x])`, `d.call([x, ...])`. Arities above 8 call
  arguments are a runtime error (`_apply.a`/`_key` trampolines cover 0..8).
- **No callable objects.** `Partial`, `LRUCache`, and `Dispatcher` are not
  themselves callable (`__call__` is not dispatched); use `.call(args)`.
  To pass one to `map`/`filter`/`reduce`/`sort_by`, adapt with a lambda:
  `map(xs, fn(n) => p.call([n]))`.
- **No named arguments on A calls.** Named args only work for
  `import python:` interop objects, so `partial` keyword binding is absent.
- **No exceptions.** Contract violations are runtime `panic`s
  (`reduce` on empty input, arity > 8) matching stdlib style.
- **No generators/iterators.** Everything is eager arrays/records.
- **sema arity note.** Untyped parameters are inferred per function across
  call sites: reusing one wrapper name (`partial`, `cache`, `lru_cache`,
  `singledispatch`) with callables of *different arities* in the same module
  can fail type-checking ("expected function type takes N"). Workarounds:
  use the `partialN` constructors (separate names), wrap the callable in a
  lambda, or split across modules. Same-arity callables are fine.

## Cache-key semantics

`lru_cache`/`cache` build the key from positional arguments only, encoded as
`(arity, args-tuple)` so different arities cannot collide. Keys must be
hashable A map keys: scalars, strings, and tuples of hashables work. Arrays,
maps, and other composite values key by identity, so calls with them simply
never hit (CPython raises `TypeError` for unhashable args). `typed=True`
semantics (distinguishing `1` from `1.0`/`True`) are not implemented.

Eviction is a true LRU order maintained in an array (O(n) per operation —
fine for typical cache sizes; CPython uses a linked list).
