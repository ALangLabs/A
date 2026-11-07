# std.enum — named constant sets and bit flags

Runtime-constructed named constants and bitmask flag sets, created through
a functional API. For enums that participate in `match` patterns, prefer
the native `enum` keyword — it is strictly more powerful. This package
covers sets built dynamically at runtime.

## Import

```a
import std:enum                                    # module form
import std:enum as en                             # en.Enum(...)
from std:enum import Enum, IntEnum, StrEnum, Flag, IntFlag, auto
```

`enum` is an A keyword; `std:`/`from std:` module paths accept keywords,
so this package is importable under its own name.

## Factories

All factories take `(typename, spec)` and return a spec object
(`EnumSpec` or `FlagSpec`) — the "class" analogue, same role as
`NamedTupleSpec` in `std.collections.namedtuple`.

`spec` accepts three forms:

- **string** — whitespace/comma-separated names, auto-numbered from 1
- **list** — names in order, auto-numbered from 1
- **map** — `{name: value}` explicit values; `auto()` values continue
  numbering from the previous integer + 1

```a
Color = Enum("Color", ["RED", "GREEN", "BLUE"])   # values 1, 2, 3
Color = Enum("Color", "RED GREEN BLUE")           # same result
Color = Enum("Color", {"RED": 1, "GREEN": 5})     # explicit values
Num   = IntEnum("Num", ["ONE", "TWO"])            # int-style text()
Word  = StrEnum("Word", ["HI", "BYE"])            # str-style text()
Perm  = Flag("Perm", {"R": 1, "W": 2, "X": 4})
IPerm = IntFlag("IPerm", ["READ", "WRITE", "EXEC"])
```

### `auto()`

```a
mix = {}
mix["LIGHT"] = 10
mix["DARK"] = auto()          # → 11 (prev int + 1)
mix["AUTO2"] = auto()         # → 12
Shade = Enum("Shade", mix)    # values [10, 11, 12]
```

In name-list/string form values number 1, 2, 3, … automatically.
In `Flag` specs `auto()` yields powers of two (1, 2, 4, 8, …).

Map literals require homogeneous values in A, so mixing `auto()`
(strings internally) with ints must be built procedurally as shown.

### Aliases

Duplicate values share one member — the second name is an alias:

```a
Alias = Enum("Alias", {"A": 1, "B": 1, "C": 2})
Alias.size()                  # 2  (canonical members only)
Alias.names()                 # ["A", "C"]
Alias.at("B").label()         # "Alias.A"  (B resolves to the A member)
Alias.members_map().keys()    # ["A", "B", "C"]  (includes aliases)
```

## Members — `EnumMember`

```a
r = Color.at("RED")
r.name                        # "RED"        (field)
r.value                       # 1            (field)
r.label()                     # "Color.RED"  ("Perm.R|W" for composites)
r.text()                      # "Color.RED"; "2" for IntEnum, "1" for StrEnum
r.repr()                      # "<Color.RED: 1>"
r.typename()                  # "Color"
r.eq(other)                   # true for the same member of the same spec
```

## Spec lookups — `EnumSpec` / `FlagSpec`

```a
Color.typename()              # "Color"
Color.size()                  # 3            canonical (non-alias) members
Color.members()               # [member, ...] in definition order
Color.names()                 # ["RED", "GREEN", "BLUE"]
Color.values()                # [1, 2, 3]
Color.members_map()           # {"RED": member, ...} including aliases

Color.at("RED")               # member; runtime error when absent
Color.get("RED")              # -> EnumMember?  (None when absent)
Color.has("RED")              # bool
Color.value_of(2)             # member with value 2; runtime error when absent
Color.find_value(2)           # -> EnumMember?  (None when absent)
```

`at`/`value_of` panic on missing input (the `KeyError`/`ValueError`
analogues); `get`/`find_value` return `None` instead.

## Flag operations — `FlagSpec`

A has no operator overloading on class instances, so flag bitwise
operations are methods. All take/return members (or bitmask ints via
`value_of`):

```a
Perm = Flag("Perm", {"R": 1, "W": 2, "X": 4})
R = Perm.at("R"); W = Perm.at("W"); X = Perm.at("X")

rw  = Perm.union(R, W)                    # R | W    → Perm.R|W (value 3)
rwx = Perm.union(rw, X)                   # Perm.R|W|X
Perm.intersection(rwx, Perm.union(R, X))  # → Perm.R|X
Perm.symmetric_difference(rwx, R)         # R ^ W    → Perm.W|X
Perm.difference(rwx, W)                   # R & ~W   → Perm.R|X
Perm.invert(R)                            # ~R       → Perm.W|X
Perm.contains_flag(rwx, W)                # W in rwx → true
Perm.contains_flag(R, rw)                 # → false
Perm.value_of(3)                          # Perm(3)  → Perm.R|W
Perm.find_value(0)                        # → Perm.0
```

Composite members get canonical names in definition order (`R|W`, never
`W|R`), unknown bits are kept as a decimal suffix (`Perm.8`), and
composites are cached — repeated lookups of the same bitmask return the
same member object.

## Coverage

Implemented: `Enum`, `IntEnum`, `StrEnum`, `Flag`, `IntFlag`, `auto()`;
spec forms string/list/map; aliases; `name`/`value`/`label`/`text`/`repr`/
`eq`/`typename` on members; `at`/`get`/`has`/`value_of`/`find_value`/
`members`/`names`/`values`/`members_map`/`size`/`typename` on specs;
flag composites and bitwise ops.

Not included:

- **Class-syntax enums** — A class bodies only hold field declarations
  and `fn` methods; use the `enum` keyword or this functional API.
- **`Color.RED` attribute access** — members are looked up by name string.
- **Iteration over the spec** — call `.members()` and iterate the list.
- **Decorator/marker hooks** (`unique`, `verify`, `property`, `member`,
  `nonmember`, `EnumType`, `FlagBoundary`) — no A equivalent.
- **`_generate_next_value_`/`__new__`/`_missing_` customization, sunder/
  dunder name rules** — names must be non-empty strings; duplicates panic.
- **Strict flag boundary** — unknown bits are always kept
  (`Perm.value_of(8)` on `{R:1,W:2,X:4}` yields `Perm.8`).
- **Multi-bit canonical flag values** — flag values must be zero or
  powers of two (validated at construction).
- **`int(member)` coercion, `==` across different specs** — use `.value`,
  `.eq()`, `.text()`.

## Semantic notes

- Contract violations become `core.panic` runtime errors: missing member,
  duplicate name, non-integer flag value, non-power-of-two flag value.
- Member values are stored as concrete A values (int/float/bool/String/
  None); array/object values remain `Json` — keep values hashable scalars.
- `auto()` is a string sentinel internally; a member value equal to that
  exact string would be misread (not a realistic concern).
- `EnumSpec`/`FlagSpec`/`EnumMember` are plain A classes; `==` on members
  is structural and includes a per-spec id, so members of two different
  `Enum("Color", ...)` calls never compare equal.
- Module-level variables read as `0.0` inside `a test` test functions
  (toolchain quirk); the `auto()` sentinel is inlined as a literal to
  stay correct under `a test`.

## Testing

`tests/integration/cases/stdlib/enum.a` exercises the full API
(`.flags` = `test`, `fn test_*` functions).
