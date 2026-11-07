# A runtime

`compiler/astra/runtime/` is the C runtime linked into every compiled A
program. It implements the dynamic value model and all builtins/native modules.
Compiled by astra (not the host C compiler) into `libaruntime.a`
(`cmake/libastra.cmake`).

## Value model (`a_runtime.h`)

Every A value is a 64-bit NaN-boxed word `aval` (`uint64_t`):

- Untagged doubles: any bit pattern whose top 16 bits are **not** `0xFFFF`.
- Tagged: top 16 bits == `A_TAG` (`0xFFFF000000000000`); 48-bit payload is
  inline int (bit0=1, 47-bit signed), pointer to heap object (low nibble 0,
  16-byte aligned), or small constants `A_NONE`/`A_FALSE`/`A_TRUE`.
- Heap objects share `struct AObj { uint32_t type; uint32_t len; uint64_t cap; }`.
- Functions are `aval f(aval *argv)` with `argv[0]` = closure env; builtins are
  `FNON` objects dispatched through `a_call` to `aval (*)(uint64_t, aval*)`.

Public API highlights: `a_mkint`/`a_int_val`/`a_mkdouble`/`a_is_*` inline
helpers, `A_INT_LIM` (`±2^46` inline range), `a_add`/`a_sub`/... fast-path
inline wrappers over `*_slow` runtime functions, object constructors
(`a_str_new`, `a_list_new`, `a_dict_new`, `a_range_new`, `a_fnon_new`,
`a_closure_env_new`), indexing/slicing (`a_getitem`, `a_setitem`, `a_slice`),
iteration protocol (`a_iter_new`/`a_iter_next`), calls (`a_call`, `a_method`,
`a_getattr`, `a_unwrap`, `a_some_new`), `a_builtin`/`a_bi_call`, the `a test`
harness `a_test_main`, `a_runtime_init`, and the `A_BI_*` builtin-id enum
(order is ABI — must match `codegen_internal.hpp::builtinRuntimeId`).

## Modules

`a_runtime_internal.h` defines the object model (`T_STR`…`T_PYBUF`, `AStr`,
`AArr` with packed-int/nomix flags, `ADict` open-addressed index, `ARange`,
`AFnon`, `AEnv`, `AResult`, `AInt64`, `AView`, `AIter`, `AArena`, `APyObj`,
`APyBuf` zero-copy Python buffer view) plus shared helpers (`panicf`,
`xmalloc`, `mkobj`, `str_*`, `arr_*`, `dict_*`, `format_into`, `bi_*`
prototypes, `a_py_*` bridge).

| File(s) | Domain |
| --- | --- |
| `a_runtime.c` | Dispatch/printing/init: `a_call`, `a_method`, `a_getattr`, builtin table, `a_printv`/`a_eprintv`, `a_runtime_init`, `a_test_main`. |
| `core_str.c`, `core_arr.c`, `core_dict.c`, `core_buf.c`, `core_fmt.c`, `core_util.c` | String/array/dict/buffer/format/util object implementations. |
| `value_arith.c`, `value_bits.c`, `value_closure.c`, `value_collect.c`, `value_compare.c`, `value_convert.c`, `value_index.c` | Arithmetic/bitwise/closures/iteration/comparison/conversion/indexing semantics behind the `*_slow` paths. |
| `methods.c`, `methods_str.c`, `methods_seq.c`, `methods_dict.c`, `methods_set.c`, `methods_view.c`, `methods_helpers.c`, `methods_internal.h` | Method dispatch (`a_method`) per receiver kind. |
| `builtins_*.c` | `bi_*` implementations: check/assert, collect (map/filter/reduce/any/all/some), convert (int/float/bool/str/repr/type/isinstance), math, misc, string builtins. |
| `fs.c` | File I/O builtins (read/write/append/list_dir/remove/rename/current_dir). |
| `json.c` | `json_*` builtins: parse/stringify/pretty/is_valid/get/at/len/has/keys/type/as_*. |
| `binary.c` | `binary_encode`/`binary_decode` tagged-value codec. |
| `log.c`, `metrics.c`, `trace.c` | Leveled logging, counters/gauges/histograms, in-process spans. |
| `net.c` | Name resolution + TCP/UDP/TLS probing builtins. |
| `arena.c`, `unsafe.c` | Bump/pool allocators; raw container/memory access builtins. |
| `python.c` | `py_*` builtins — libpython `dlopen`'d at runtime (no link-time dep); opaque `T_PYOBJ` values with attribute/item/call/iteration bridging; `T_PYBUF` views. Backend side: `compiler/astra/backend/python/python.c`. |
| `terminal.c` | POSIX terminal builtins (isatty, raw mode, read char, size, home; Windows → unsupported stubs via `a_runtime_internal.h`). |
| `time.c`, `intrinsics.c` | `time_now`/`time_sleep`; LLVM-style intrinsics (ctz/clz/popcount/bswap/rot, fma/fabs/copysign, min/max, overflowing arithmetic, expect/likely/unlikely). |
| `strings/utf8.cpp/.hpp`, `collections/transparent_map.hpp` | C++ helpers used by frontend code that links runtime headers. |

## How it fits

`pipeline.cpp::findRuntime`/`astraBuild` locate these sources (or the staged
`libaruntime.a`) and hand them to `astra`, which compiles them together with
the emitted `.s`. `A_BI_*` ids, `aval` layout, and the `aval f(aval *argv)`
function ABI are the contract between [codegen](frontend-codegen.md) and this
runtime. Related: [Astra backend](backend-astra.md),
[Stdlib](stdlib.md), [overview](overview.md).
