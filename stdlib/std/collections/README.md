# std.collections

Collection helpers plus ports of CPython's pure-Python `collections`
classes. Sources: `/usr/lib/python3.14/collections/__init__.py`
(see `docs/standard-library/python-porting.md`).

## Importing

Bare `import collections` resolves to the **native** collections module
(`len`/`map`/`filter`/`reduce`/...), not this package — native modules win
name resolution. Reach this package with a quoted path:

```a
from "std/collections/__init__.a" import Counter, namedtuple, deque_of
import "std/collections/__init__.a" as col   # functions only: col.map_list
```

Prefer `from ... import` for the ported **types**: struct constructors
re-exported through `__init__.a` do not resolve via module-alias syntax
(`col.Counter({})` yields an error-typed value — a name-resolution
limitation; `from "std/collections/__init__.a" import Counter` works).
Module-alias access is fine for plain functions (`col.user_list_of`,
`col.namedtuple`, ...).

Pre-existing helpers (`map_list`, `dict_get`, ...) are unchanged.

## Ported classes

| Python | A | File |
|---|---|---|
| `namedtuple` | `namedtuple(typename, fields) -> NamedTupleSpec` | `namedtuple.a` |
| `Counter` | `Counter` struct (`counts` map) | `counter.a` |
| `OrderedDict` | `OrderedDict` struct (`data` map) | `ordereddict.a` |
| `defaultdict` | `defaultdict(factory) -> DefaultDict<T>` | `defaultdict.a` |
| `ChainMap` | `ChainMap` struct (`maps` list) | `chainmap.a` |
| `UserDict` | `UserDict` struct (`data` map) | `userdict.a` |
| `UserList` | `UserList<T>` struct (`data` list) | `userlist.a` |
| `deque` | `Deque<T>` struct + `deque`/`deque_of`/`deque_bounded` | `deque.a` |

## API coverage vs Python

- **Counter**: `update` (array or `{K: int}` map), `subtract`,
  `update_counter`/`subtract_counter`, `elements()` (list),
  `most_common(n)`/`most_common_all()` (list of `(k, count)` tuples),
  `total()`, `get` (0 for missing, like `c[k]`), `set`, `inc`, `dec`,
  `add`, `contains`, `keys`/`values`/`items`, `size`, `clear`, `copy`,
  and the operator equivalents `plus`/`minus`/`intersect`/`union`
  (`+ - & |`). Missing: `fromkeys`, `__missing__` hook, non-int counts.
- **OrderedDict**: full mutable-mapping surface (`at`, `get`, `get_or`,
  `set`, `pop`, `pop_or`, `popitem(last)`, `move_to_end(k, last)` plus
  `move_to_front(k)` convenience, `update`, `clear`, `copy`, `keys`/
  `values`/`items`, `reversed()`, order-sensitive `eq`,
  `ordered_dict_fromkeys`). A maps already preserve insertion order, so
  `data` is a plain map. Missing: `__or__`/`__ior__` merge operators,
  `reversed()` as iterator (returns a list).
- **defaultdict**: `at(k)` is the `dd[k]` auto-vivify analogue,
  `get`/`get_or`/`setdefault`/`set`/`remove`/`pop`/`update`/`clear`/
  `copy`/`keys`/`values`/`items`/`size`. Missing: `default_factory =
  None` mode (factory is required), `__missing__` override.
- **ChainMap**: `get`/`get_or`/`at`/`set` (first map)/`contains`/
  `new_child`/`parents`/`maps_list`/`keys`/`items`/`size`/`pop`/
  `popitem`/`clear`/`update`/`copy`, plus `chain_map()`/`chain_map_of`.
  Missing: `__iter__`/`__len__` dunders (use `keys()`/`size()`),
  `__getitem__` on the ChainMap itself (`at` instead).
- **UserDict**: MutableMapping surface — `at`/`get`/`get_or`/`set`/
  `setdefault`/`remove`/`pop`/`pop_or`/`popitem`/`contains`/`keys`/
  `values`/`items`/`update`/`clear`/`copy`/`size`/`eq`.
- **UserList**: MutableSequence surface — `at`/`set` (indexing
  analogue), `append`/`extend`/`insert`/`pop`/`pop_at`/`remove`/`clear`/
  `copy`/`count`/`index`/`index_from`/`reverse`/`sort`/`contains`/
  `size`/`to_list`/`first`/`last`. Missing: slicing (`l[1:3]`),
  in-place `l[i] += 1` style ops.
- **deque**: `append`/`appendleft`/`extend`/`extendleft`/`pop`/
  `popleft`/`rotate`/`clear`/`copy`/`reverse`/`count`/`index`/
  `index_from`/`remove`/`contains`/`at`/`first`/`last`/`size`/
  `maxlen_value`/`to_list`, `maxlen` bound enforced on both ends.
  Missing: `index(x, start, stop)` third arg, `__getitem__` slicing,
  iteration (`to_list()` instead).

## Semantic deviations (per python-porting.md)

- **Exceptions -> `core.panic`**: KeyError/IndexError/ValueError
  conditions (missing key, bad index, `popitem` on empty, `index` of an
  absent value) are runtime errors via `core.panic`, matching stdlib
  contract-violation style. No `try`/`except` exists.
- **Iterators/generators -> lists**: `elements()`, `keys()`, `items()`,
  `reversed()`, `to_list()` return materialized `[T]`.
- **Operators -> methods**: A has no class operator overloading, so
  Counter's `+ - & |` are `plus`/`minus`/`intersect`/`union`, and
  `d[k]`/`del d[k]`/`k in d` become `at`/`remove`/`contains`.
- **Dynamic typing limits**: A arrays are homogeneous, and passing a
  value through `json.*` helpers types it as the opaque `Json` kind, so
  `Counter.update`/`subtract` accept an array *or* a map via
  `json.kind` dispatch; map values must be integers (`json.as_int`).
  `Counter` keys may be any value but a single array argument must be
  homogeneous (A rule).
- **namedtuple cannot synthesize types**: `namedtuple("Point", ["x","y"])`
  returns a `NamedTupleSpec` "class" object; `Point.make([1,2])` builds
  the record (the `_make` analogue) and `Point.make_map({...})` is the
  kwargs analogue. There is no `p.x` attribute syntax — use
  `p.field("x")`, `p.at(0)`, `p.asdict()`, `p.fields()`. `p.replace`
  replaces one named field (no `**kwargs`). `p.eq(other)` is the `==`
  analogue (struct `==` is identity). Python's identifier/keyword
  validation of field names is not enforced.
- **deque complexity**: append/appendleft/pop/popleft are O(1)
  (amortized; `rotate`/`reverse`/`to_list`/`index`/`remove` are O(n)).
  `maxlen` is an `i64` where `-1` means unbounded (Python `None`).
- **OrderedDict/dicts**: A `{K:V}` maps are insertion-ordered already
  (verified: new keys append, updates keep position), so `OrderedDict`
  is a thin wrapper adding `popitem`/`move_to_end`/order-sensitive `eq`.
- **Dynamically-typed returns**: methods on these wrappers return
  `Json`-typed values. `==`, `print`, indexing, and iteration all work
  normally, but the test harness's `assert_eq` is type-strict and can
  report Json-vs-native container mismatches — compare containers with
  `assert(a == b)` in tests (scalar `assert_eq` is fine).
