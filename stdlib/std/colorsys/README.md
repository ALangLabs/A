# colorsys

Port of CPython 3.14 `Lib/colorsys.py` — conversion functions between RGB
and the YIQ, HLS, and HSV color systems.

```python
import colorsys

y, i, q = colorsys.rgb_to_yiq(0.5, 0.2, 0.8)
r, g, b = colorsys.yiq_to_rgb(y, i, q)
h, l, s = colorsys.rgb_to_hls(r, g, b)
r, g, b = colorsys.hls_to_rgb(h, l, s)
h, s, v = colorsys.rgb_to_hsv(r, g, b)
r, g, b = colorsys.hsv_to_rgb(h, s, v)
```

## API coverage

| CPython | A | Notes |
| --- | --- | --- |
| `rgb_to_yiq` / `yiq_to_rgb` | same | FCC/NTSC constants, verbatim |
| `rgb_to_hls` / `hls_to_rgb` | same | incl. gh-106498 `2.0-maxc-minc` fix |
| `rgb_to_hsv` / `hsv_to_rgb` | same | |
| `ONE_THIRD` / `ONE_SIXTH` / `TWO_THIRD` | same | module-level float vars |
| `rgb_to_ycbcr` / `ycbcr_to_rgb` | **A-only extension** | CPython `colorsys` has no YCbCr; added per port request using full-range BT.601 formulas normalized to [0,1] |

All functions take/return 3-tuples of floats in `[0.0, 1.0]` (I/Q may
exceed the range, as in CPython).

## Semantic deviations

- **Float `%`**: CPython's `%` on floats is floor-modulo (sign of the
  divisor); A's is C `fmod` (sign of the dividend). Internal helper
  `_posmod` reproduces Python semantics for the positive moduli used
  (`1.0`, `6`), so results are bit-identical to CPython.
- **`max`/`min`**: A builtins take exactly 2 arguments, so the 3-way
  reductions are nested (`max(r, max(g, b))`).
- **Variables**: A has no branch-local first assignment, so `s`/`h`/`m2`
  are pre-initialized before the `if`/`elif` chains (same values).
- `hsv_to_rgb`'s unreachable tail returns `(0.0, 0.0, 0.0)` instead of
  CPython's implicit `None`.
- No `__all__`; no exceptions (CPython never raises here either).
