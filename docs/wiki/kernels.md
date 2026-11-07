# Kernels

`kernels/` is a library of numeric compute kernels **written in A itself**
(self-hosted; the header comments note there is "no native `kernel_ops`
dependency" in the sources, though `kernel_ops` still exists in the native
module table for other callers).

## Layout

- `kernels/__init__.a` — re-exports everything from `src/__init__.a`.
- `kernels/src/__init__.a` — package entry; imports each module:
  - `fused.a` — fused arithmetic: `fused_add/sub/mul/div/mul_add/scale/clamp/pow/min/max/sqrt/exp/log/reciprocal`.
  - `reductions.a` — `reduce_sum/max/min/mean/prod/var/std/norm`, `argmax`, `argmin`, `dot`.
  - `matmul.a` — `matvec`, `matmul`, `transpose`, `outer`, `eye`, `zeros`, `ones`, `full`, `trace`, `diag`, `reshape` (pure-A triple-loop implementations, e.g. `dot` accumulates `a[i]*b[i]`).
  - `activations.a` — `relu`, `sigmoid`, `tanh`, `gelu`, `silu`, `elu`, `leaky_relu`, `softplus`, `mish`, `softmax`, `softmax_row(s)`, `bias_relu`.
  - `normalization.a` — `normalize`, `rms_norm`, `layer_norm`, `layer_norm_rows`.
  - `attention.a` — `attention_scores`, `attention`.
  - `conv.a` — `conv1d`, `max_pool1d`, `avg_pool1d`.
  - `quantize.a` — `quantize_int8`, `dequantize_int8`.

## How it fits

Imported like any module (`import kernels` / `from kernels import matmul`).
Sema has a dedicated builtin domain `sema_builtins_kernels.cpp` and the
`kernel_ops` native module maps `kernels_*` names to `A_BI_*` runtime entries
(`native_modules.hpp`), but the shipped `kernels/src/*.a` files implement the
same API in portable A so they run on any backend. Related:
[Stdlib](stdlib.md), [examples](examples.md), [overview](overview.md).
