# std.enum — named constant sets and bit flags

`enum` is an A keyword, but `std:`/`from std:` module paths accept keywords,
so this package is imported as:

```a
import std:enum
from std:enum import Enum, IntEnum, StrEnum, Flag, IntFlag, auto
```

**Note**: For enums that participate in `match` patterns, prefer the native
`enum` keyword — it is strictly more powerful. This package covers dynamic,
runtime-constructed constant sets (the functional-API style).

## API

```a
Color = Enum("Color", ["RED", "GREEN", "BLUE"])   # values auto-number 1,2,3
Color = Enum("Color", "RED GREEN BLUE")           # string form also works
Color = Enum("Color", {"RED": 1, "GREEN": 5})     # explicit values
Color = Enum("Color", mix)                        # mix built procedurally
Num   = IntEnum("Num", ["ONE", "TWO"])
Word  = StrEnum("Word", ["HI", "BYE"])
Perm  = Flag("Perm", {"R": 1, "W": 2, "X": 4})
IPerm = IntFlag("IPerm", ["READ", "WRITE", "EXEC"])
```

`auto()` may appear as a map value; it continues numbering from the
previous integer value + 1 (Flag specs use powers of two instead).

The result is an **EnumSpec** (or **FlagSpec**) — the "class" analogue,
same role as `NamedTupleSpec` in `std.collections.namedtuple`:

| Concept | A |
|---|---|
| `Color.RED` attribute | `Color.at("RED")` |
| `Color(1)` value lookup | `Color.value_of(1)` |
| `list(Color)` / iteration | `Color.members()` (returns a list) |
| `Color.__members__` map | `Color.members_map()` |
| `len(Color)` | `Color.size()` |
| `member.name` / `member.value` | `member.name` / `member.value` (fields) |
| `str(member)` | `member.text()` |
| `repr(member)` | `member.repr()` |
| member names | `Color.names()`, `Color.values()` |

Non-throwing lookups: `Color.get(name) -> EnumMember?`,
`Color.find_value(v) -> EnumMember?`, `Color.has(name) -> bool`.

`at`/`value_of` panic (runtime error) on missing names/values.

## Flag operations

A has no operator overloading on class instances, so the bitwise flag
operators are methods on FlagSpec:

| Operation | A |
|---|---|
| `R \| W` | `Perm.union(R, W)` |
| `R & W` | `Perm.intersection(R, W)` |
| `R ^ W` | `Perm.symmetric_difference(R, W)` |
| `R & ~W` | `Perm.difference(R, W)` |
| `~R` | `Perm.invert(R)` |
| `W in rw` | `Perm.contains_flag(rw, W)` |
| `Perm(3)` | `Perm.value_of(3)` |

Composite members get canonical names like `Perm.R|W` (definition order)
and are cached, so repeated lookups of the same bitmask return the same
member object.

## Coverage

Implemented: `Enum`, `IntEnum`, `StrEnum`, `Flag`, `IntFlag`, `auto()`;
spec forms string/list/map; aliases (duplicate values share one member);
`name`/`value`/`label`/`text`/`repr`/`eq`/`typename` on members;
`at`/`get`/`has`/`value_of`/`find_value`/`members`/`names`/`values`/
`members_map`/`size`/`typename` on specs; flag composites and bitwise ops.

Not included:

- **Class-syntax enums** (`class Color(Enum): RED = 1`) — A class bodies
  only hold field declarations and `fn` methods; use the `enum` keyword
  or the functional API here.
- **`Color.RED` attribute access** — members are looked up by name string.
- **Iteration over the spec** (`for m in Color`) — call `.members()`.
- **Decorator/marker hooks** (`unique`, `verify`, `property`, `member`,
  `nonmember`, `EnumType`, `FlagBoundary`) — no A equivalent.
- **`_generate_next_value_`/`__new__`/`_missing_` customization, sunder/
  dunder name rules** — names must be non-empty strings; duplicate names
  panic.
- **Strict flag boundary** — unknown bits are always *kept*
  (boundary=KEEP): `Perm.value_of(8)` on `{R:1,W:2,X:4}` yields a member
  named `Perm.8`.
- **Multi-bit canonical flag values** — flag member values must be zero
  or powers of two (validated at construction).
- **`int(member)`/`str` coercion, `==` between members of different
  specs** — use `.value`, `.eq()`, `.text()`.

## Semantic notes

- Contract violations become `core.panic` runtime errors (missing member,
  duplicate name, non-integer flag value, non-power-of-two flag value).
- Member values are stored as concrete A values (int/float/bool/String/
  None); array/object values stay `Json` — keep values hashable scalars.
- `auto()` is a string sentinel internally; a member value equal to that
  exact string would be misread (not a realistic concern).
- `EnumSpec`/`FlagSpec`/`EnumMember` are plain A classes; `==` on members
  is structural (includes a per-spec id, so members of two different
  `Enum("Color", ...)` calls never compare equal).
- Map literals require homogeneous values, so
  `Enum("X", {"A": auto(), "B": 1})` must be built procedurally:
  `m = {}; m["A"] = auto(); m["B"] = 1`.

## Testing

`tests/integration/cases/stdlib/enum.a` exercises the main API
(`.flags` = `test`, `fn test_*` functions).
