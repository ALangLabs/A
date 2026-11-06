# `std.statistics`

> Descriptive statistics for numeric data, inspired by CPython's [`statistics`](https://docs.python.org/3/library/statistics.html) module.

`std.statistics` provides a **pure-A subset** of common averages and dispersion measures for numeric arrays/lists. It is useful for quick analysis in scripts, CLI tools, and test helpers.

> [!NOTE]
> This is **not a drop-in CPython module**. Inputs are expected to be numeric A values; invalid inputs result in runtime `panic` messages (A does not use CPython's exception types here).

## Status

| Field | Value |
|---|---|
| Kind | Pure A stdlib package |
| Source | `stdlib/std/statistics/__init__.a` |
| Import names | `statistics` (explicit path import recommended for source layouts) |
| API style | Python-inspired subset |
| Error model | Runtime `panic` for invalid inputs/insufficient data |

## Import

```a
from "std/statistics/__init__.a" import mean, median, stdev, variance
```

```a
import statistics
print(statistics.mean([1, 2, 3, 4]))
```

## API reference

| Function | Description | Notes |
|---|---|---|
| `mean(data)` | Arithmetic mean. | Requires at least 1 data point. |
| `fmean(data)` | Arithmetic mean as float. | Weights not supported in this subset. |
| `median(data)` | Median (middle value). | Returns average of middle two for even-sized data. |
| `median_low(data)` | Low median. | Useful for even-sized samples. |
| `median_high(data)` | High median. | Useful for even-sized samples. |
| `mode(data)` | Most common value. | First value with highest count among ties (by first occurrence). |
| `pvariance(data)` | Population variance. | Requires at least 1 data point. |
| `variance(data)` | Sample variance (`ddof=1`). | Requires at least 2 data points. |
| `pstdev(data)` | Population standard deviation. | `sqrt(pvariance(data))`. |
| `stdev(data)` | Sample standard deviation. | `sqrt(variance(data))`. |
| `geometric_mean(data)` | Geometric mean. | Requires positive data points. |
| `harmonic_mean(data)` | Harmonic mean. | Requires positive data points. |

## Examples

### Averages and spread

```a
from "std/statistics/__init__.a" import mean, fmean, median, median_low, median_high, mode, variance, stdev

data = [1, 2, 3, 4, 4]
print(mean(data))          # 2.8
print(fmean(data))         # 2.8
print(median([1, 3, 5, 7]))    # 4.0
print(median_low([1, 3, 5, 7]))  # 3.0
print(median_high([1, 3, 5, 7])) # 5.0
print(mode([1, 2, 2, 3, 3, 3]))  # 3
print(variance([1, 2, 3, 4, 5])) # sample variance
print(stdev([1, 2, 3, 4, 5]))    # sample stdev
```

### Positive-data means

```a
from "std/statistics/__init__.a" import geometric_mean, harmonic_mean

print(geometric_mean([1, 2, 4]))   # 2.0
print(harmonic_mean([1, 2, 4]))    # 1.714285...
```

## Differences from CPython

- No `StatisticsError` exceptions; invalid cases `panic` with descriptive messages.
- `fmean` does not accept weights in this subset.
- Not implemented: `median_grouped`, `multimode`, `quantiles`, covariance/correlation, linear regression, `NormalDist`, and full `Decimal`/`Fraction` coercions.

## Validation

```bash
build/a run tests/integration/cases/stdlib/pystatistics_basic.a
```

## See also

- [`docs/standard-library/README.md`](../../../docs/standard-library/README.md)
- [`stdlib/std/README.md`](../README.md)
- [CPython `statistics` docs](https://docs.python.org/3/library/statistics.html)
