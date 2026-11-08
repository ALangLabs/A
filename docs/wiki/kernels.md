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
  - `matmul.a` — `matvec`, `matmul`, `matmul_blocked`, `transpose`, `outer`, `eye`, `zeros`, `ones`, `full`, `trace`, `diag`, `reshape` (pure-A triple-loop implementations, e.g. `dot` accumulates `a[i]*b[i]`; `matmul_blocked` uses cache-tiled ikj loop order, the same blocking the Mojo/CBLAS matmul kernels use).
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

## Native ML kernels and the FMA pass (Mojo-inspired)

For hot loops the portable A kernels above are backed by native runtime
kernels (`compiler/astra/runtime/linalg.c`), borrowing three ideas from
Mojo/MAX kernels (modular's `max/examples/custom_ops/` block-tiled,
vectorized matmul; Mojo's kernel fusion):

- **Block-tiled matmul** — `matmul(a, b, m, k, n)` runs ikj loops in
  32-wide column tiles with `fma()` accumulate, keeping an output row
  segment in L1 while B rows stream sequentially.
- **Kernel fusion** — `attention(q, k, v, nk, dk, dv)` computes scores,
  stable softmax, and the weighted sum in one allocation instead of
  three intermediate lists; `softmax(x)` is the stable single-buffer path.
- **Vectorized reductions** — `dot`/`matvec`/`matvec_t` unroll 4 ways with
  `fma()` (single rounding per multiply-add).

The compiler mirrors this at the IR level: the `fuseFma` optimizer pass
rewrites single-use `a*b + c` floating-point pairs into an `fma` IR
instruction (LLVM-style contraction), visible via `a ir --opt`. Related:
[Builtins](builtins.md), [benchmarks](benchmarks.md).
