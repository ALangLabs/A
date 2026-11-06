# `std.statistics`

Descriptive statistics for numeric data, inspired by Python's `statistics` module.

Implements common averages and spread measures over A lists/arrays of numbers. Inputs should be numeric values; behavior is adapted to A's runtime semantics.

## Status

- **Kind:** Pure A stdlib package (`stdlib/std/statistics`)
- **Style:** Python-inspired API subset adapted to A semantics (not a drop-in CPython module)
- **Source path:** `stdlib/std/statistics/`

## Import

```a
import statistics
```

```a
# Explicit stdlib path import (works in source layouts that load stdlib modules by path)
from "std/statistics/__init__.a" import *
```

## What It Provides

- **Functions/exports (detected):** `mean`, `fmean`, `median`, `median_low`, `median_high`, `mode`, `pvariance`, `variance`, `pstdev`, `stdev`, `geometric_mean`, `harmonic_mean`

## API Reference (Practical)

- `mean(data)`, `fmean(data)` — arithmetic mean (`fmean` does not accept weights in this subset).
- `median(data)`, `median_low(data)`, `median_high(data)` — median variants.
- `mode(data)` — most common value (first among ties for highest count).
- `pvariance(data)`, `variance(data)` — population/sample variance.
- `pstdev(data)`, `stdev(data)` — population/sample standard deviation.
- `geometric_mean(data)`, `harmonic_mean(data)` — geometric/harmonic means for positive data.

## Examples

```a
import statistics

data = [1, 2, 3, 4, 4]
print(statistics.mean(data))
print(statistics.fmean(data))
print(statistics.median([1, 3, 5, 7]))
print(statistics.mode([1, 2, 2, 3, 3, 3]))
print(statistics.variance([1, 2, 3, 4, 5]))
print(statistics.stdev([1, 2, 3, 4, 5]))
```

## Usage Notes

- Invalid input (e.g. empty data, insufficient points, non-positive values where required) results in a runtime `panic` with a descriptive message.
- This is a focused subset; advanced Python features like `NormalDist`, `quantiles`, `multimode`, and full type coercion (`Decimal`/`Fraction`) are not implemented.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
