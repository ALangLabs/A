# Builtins and native modules

Core builtin functions are available without imports (verified against
`compiler/astra/frontend/ast/builtins.hpp` and the runtime `A_BI_*` table).

## Core builtins

| Category | Functions |
| --- | --- |
| I/O | `print(...)`, `eprint(...)` (stderr), `input()`, `flush()` |
| Values | `len(x)`, `str(x)`, `int(x)`, `float(x)`, `bool(x)`, `repr(x)`, `type(x)`, `isinstance(x, T)` |
| Math | `abs`, `min`, `max`, `sum`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`, `random`, `random_int` |
| Linear algebra | `dot(a, b)`, `matvec(x, w, bias, nin, nout)`, `matvec_t(x, w, bias, nout, nin)`; canonical names `linalg_dot`, `linalg_matvec`, `linalg_matvec_t` also work |
| Collections | `map`, `filter`, `reduce`, `any`, `all`, `some`, `join`, `sorted`, `reversed`, `enumerate`, `zip`, `set`, `tuple`, `list`, `dict`, `range`-style iteration via `a..b` |
| Option/Result | `Some(v)`, `Ok(v)`, `Err(e)` constructors; `None` literal |
| Assertions | `assert(cond)`, `assert_eq(a, b)`, `panic(msg)` |
| Debug | `ic(*values)` (prelude), `repr`, `__repr__`, `__debug_repr__`, `__ic_format__` |
| Files | `read_file`, `write_file`, `append_file`, `file_exists`, `list_dir`, `remove_file`, `create_dir`, `remove_dir`, `rename`, `current_dir` |
| Conversions | `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64` |
| Intrinsics | `ctz clz popcount bswap rotl rotr fma fabs copysign smin smax umin umax` + overflow variants and `expect/likely/unlikely` |

Note: `range(5)` is **not** a function — write `0..5` range literals instead.

## Native modules

`import <name>` binds a namespace of builtins (table in
`compiler/astra/frontend/name-resolution/native_modules.hpp`):

`math` (sqrt/abs/min/max/pow/trig/log/floor/ceil/round/trunc/random + `PI`,`E`),
`io` (print/eprint/input/flush), `fs` (file ops), `strings`, `collections`,
`core` (panic/assert/assert_eq), `time` (now/sleep), `json`, `binary`,
`testing`, `log`, `metrics`, `trace`, `net` (resolve/tcp/udp/tls), `http`
(get/post/request/serve), `terminal`, `env`, `repl` (eval/quiet), `py`
(Python bridge), `unsafe`, `arena`, `kernel_ops`.

Prefer `import std:<pkg>` for the ergonomic stdlib wrappers — see
[stdlib](stdlib.md).
