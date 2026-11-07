# std.enum — Python `enum` module port

Source: CPython 3.14 `Lib/enum.py` (functional API subset).

`enum` is an A keyword, but `std:`/`from std:` module paths now accept keywords, so the package is named `enum` (bare
`import enum` cannot parse — see `docs/standard-library/python-porting.md`).
Import it as:

```a
import enum
from enum import Enum, IntEnum, StrEnum, Flag, IntFlag, auto
```

## Why not `class Color(Enum)`?

Python's Enum is built on class syntax plus the `EnumMeta` metaclass:

```python
class Color(Enum):
    RED = 1
    GREEN = auto()
```

A cannot express this:

- Class bodies only hold field declarations and `fn` methods —
  `RED = 1` inside a class body is a parse error.
- There is no metaclass machinery to synthesize attributes at runtime.
- `match` variant patterns require real `enum` variants, which a
  stdlib class cannot provide.

What A *can* express is enum.py's **functional API**, which is what this
module ports. For native enums with `match` support, use A's `enum`
keyword instead — it is strictly more powerful.

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

| Python | A |
|---|---|
| `Color.RED` | `Color.at("RED")` |
| `Color['RED']` | `Color.at("RED")` |
| `Color(1)` | `Color.value_of(1)` |
| `list(Color)` / `for m in Color` | `Color.members()` (returns a list) |
| `Color.__members__` | `Color.members_map()` |
| `len(Color)` | `Color.size()` |
| `member.name` / `member.value` | `member.name` / `member.value` (fields) |
| `str(member)` | `member.text()` |
| `repr(member)` | `member.repr()` |
| `Color.RED.name`-style iteration | `Color.names()`, `Color.values()` |

Non-throwing lookups (A additions, no Python equivalent):
`Color.get(name) -> EnumMember?`, `Color.find_value(v) -> EnumMember?`,
`Color.has(name) -> bool`.

`at`/`value_of` panic (runtime error) on missing names/values, matching
Python's `KeyError`/`ValueError`/`AttributeError`.

## Flag operations

Python's `|` `&` `^` `~` operators and `in` have no class-operator
analogue in A, so they are methods on FlagSpec:

| Python | A |
|---|---|
| `Perm.R \| Perm.W` | `Perm.union(R, W)` |
| `Perm.R & Perm.W` | `Perm.intersection(R, W)` |
| `Perm.R ^ Perm.W` | `Perm.symmetric_difference(R, W)` |
| `Perm.R & ~Perm.W` | `Perm.difference(R, W)` |
| `~Perm.R` | `Perm.invert(R)` |
| `Perm.W in rw` | `Perm.contains_flag(rw, W)` |
| `Perm(3)` | `Perm.value_of(3)` |

Composite members get canonical names like `Perm.R|W` (definition order)
and are cached, so repeated lookups of the same bitmask return the same
member object.

## Coverage vs Python

Implemented: `Enum`, `IntEnum`, `StrEnum`, `Flag`, `IntFlag`, `auto()`;
spec forms string/list/map; aliases (duplicate values share one member);
`name`/`value`/`label`/`text`/`repr`/`eq`/`typename` on members;
`at`/`get`/`has`/`value_of`/`find_value`/`members`/`names`/`values`/
`members_map`/`size`/`typename` on specs; flag composites and bitwise ops.

Not ported (no A analogue or out of scope):

- **Class syntax** (`class Color(Enum): RED = 1`) — impossible, see above.
- **`Color.RED` attribute access** — members are looked up by name string.
- **Iteration over the spec** (`for m in Color`) — call `.members()`.
- **`unique`, `verify`, `global_enum`, `property`, `member`, `nonmember`,
  `EnumType`, `FlagBoundary`, `enum.member`/`nonmember` sentinels** —
  decorators/markers have no A equivalent.
- **`_generate_next_value_` hooks, `__new__`/`__init__`/`_missing_`
  customization, sunder/dunder name rules, `_order_`** — Python's
  member-validation machinery is not reproduced; names must be non-empty
  strings and duplicate names panic.
- **Flag boundary modes** — unknown bits are always *kept* (Python
  `boundary=KEEP`): `Perm.value_of(8)` on `{R:1,W:2,X:4}` yields a member
  named `Perm.8`. Python's strict-Flag `ValueError` is not reproduced.
- **Multi-bit canonical flag values** — flag member values must be zero
  or powers of two (validated at construction).
- **`int(member)`/`str` coercion, comparison operators between members** —
  use `.value`, `.eq()`, `.text()`.

## Semantic deviations

- Python exceptions become `core.panic` runtime errors (missing member,
  duplicate name, non-integer flag value, non-power-of-two flag value).
- Member values are stored as concrete A values (int/float/bool/String/
  None); array/object values are kept as `Json` and work for storage but
  not for `value_of` lookup edge cases — keep values hashable scalars.
- `auto()` is a string sentinel internally; a member value equal to that
  exact control-character string would be misread (not a realistic
  concern).
- `EnumSpec`/`FlagSpec`/`EnumMember` are plain A classes; `==` on members
  is structural (includes a per-spec id, so members of two different
  `Enum("Color", ...)` calls never compare equal — Python semantics).
- Map literals require homogeneous values, so
  `Enum("X", {"A": auto(), "B": 1})` must be built procedurally:
  `m = {}; m["A"] = auto(); m["B"] = 1`.

## Testing

`tests/integration/cases/stdlib/enum.a` exercises the main API
(`.flags` = `test`, `fn test_*` functions).
