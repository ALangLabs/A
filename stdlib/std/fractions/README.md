# std.fractions

Port of CPython 3.14 `Lib/fractions.py` — the `Fraction` rational type.

- `Fraction_new(n, d)` / `Fraction_from_int(v)` / `Fraction_from_float(f)`
  constructors (normalised: denominator > 0, gcd-reduced).
- Arithmetic/comparison via methods (`add sub mul truediv neg abs eq lt le
  gt ge`) — A has no operator overloading on classes.
- `floor ceil trunc round` (banker's rounding), `limit_denominator(max)`,
  `to_float`, `to_string` — all verified against CPython in
  `pyfractions_basic`.

Deviations: string parsing ("3/4", "1.5") not implemented (no split-based
parser yet); float->Fraction is the exact binary ratio (matches CPython);
exceptions -> `core.panic`. Generic `Fraction(v)` dispatch was omitted —
use the typed constructors.
