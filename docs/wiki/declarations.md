# Declarations

Top-level and member declarations, verified against
`src/astra/frontend/parser/parser_decl.cpp`, `ast/stmts.hpp`, and
`examples/`.

## Functions

```python
fn add(a, b):
    return a + b

fn greet(name: String) -> String:
    return "Hello, {name}"
```

- Optional parameter/return type annotations; types may be omitted.
- Default values on trailing params: `fn f(x = 1)`; variadic `*args` collects
  extras into a list.
- Generic type params: `fn id<T>(x: T) -> T`; generic bounds use
  `<T: Trait>`. `comptime` params:
  `fn f(comptime N: int)`; constraints via `where`:
  `fn same(a: Point, b: Point) -> bool where Equatable:`.
- Lambdas: `double = fn(n) => n * 2`; `(fn(x, y) => x + y)(20, 22)`.

## Extern functions (C FFI)

`extern fn` declares a C function with no body; calls lower to a direct C
call (LLVM `declare` + `call`), bypassing the A runtime ABI:

```python
extern fn strlen(s: str) -> int
extern "C" fn abs(x: int) -> int
extern fn malloc(size: int) -> Pointer
extern fn free(p: Pointer)
extern fn my_pow(x: float, y: float) -> float = "pow"   # rename the C symbol
```

- `extern "C"` is accepted (only `"C"` linkage exists); `= "name"` renames
  the symbol when the A name differs from the C name.
- Every parameter needs a type annotation; no generics, `where`, `comptime`,
  variadic `*args`, or default values.
- Type mapping: `int`/`i8`–`i64`/`u8`–`u64`/`char` ↔ C integers (`i64`
  args, width-matched returns), `float`/`f32`/`f64` ↔ `float`/`double`,
  `bool` ↔ C `_Bool`, `str` ↔ `char *` (NUL-terminated; `-> str` boxes the
  result via `strlen`+`a_str_new`), `Pointer` ↔ raw address as `i64`,
  `Bytes` ↔ `unsigned char *` (params only). `-> void` or no return
  annotation means void.
- Symbols resolve at link time: libc functions (`strlen`, `malloc`,
  `getenv`, …) work with zero extra flags. Functions in user libraries need
  the library linked into the final binary (e.g. via a wrapper object);
  there is no `-l` flag plumbing yet.
- Not supported: C callbacks (function pointers), C variadic functions,
  structs-by-value, and `Bytes` return values. Using an extern fn as a
  first-class value is not supported either — call it directly.

## Classes

```python
class Point:
    x: i64
    y: i64

    fn length(self) -> float:
        ...

p = Point(3, 4)          # positional or named: Point(x=1, y=2)
```

- Fields are bare names or `name: Type`; methods take `self`.
- Inheritance: `class Dog(Animal):`; metaclass: `class C(metaclass=M):`
  (also `class C(Base, metaclass=M)`).
- Docstrings: a `"""..."""` string-literal statement documents a declaration —
  the first statement inside a body, or directly above the declaration
  (triple-quoted, raw — no escapes or interpolation). `#`/`##` are plain
  comments, not documentation.

## Enums

```python
enum Color:
    Red
    Green = 5      # explicit values allowed; later variants auto-increment
    Blue
```

Access: `Color.Red`. Algebraic variants carry fields (payload enums); `match`
handles them.

## Traits and impls

```python
trait Equatable:
    fn eq(self, other) -> bool

class Point: ...
impl Equatable for Point:
    pass
```

Traits support inheritance (`trait Hashable(Equatable)`) and `where` lists.
`impl` verifies the type provides every signature.

## Type aliases and comptime

```python
type UserId = i64
id: UserId = 7
```

`comptime` params (`fn f(comptime N: int, x: int)`) are specialized at each
call site: `f(4, x)` clones the function, removes the comptime parameter from
the signature, and substitutes `N` with the literal `4` throughout the body
before type checking, so every specialization is zero-cost ordinary code. The
argument must be a compile-time constant (literals, constant arithmetic,
`len`/`range`/`str`/`int`/`float`/`bool`/`abs` on constants). Comptime params
may sit anywhere in the parameter list; all call arguments must be
positional. Inside a body, `comptime if COND:` / `comptime for i in xs:` are
evaluated at compile time — only the taken branch is kept and `comptime for`
unrolls — when the condition/iterable is a compile-time constant (including
substituted comptime params); otherwise they degrade to ordinary runtime
`if`/`for`. Comptime params on methods, comptime types, and `reflect` are not
implemented yet. Constraints via `where` do work (see above).

## Kernel / device functions (GPU offload)

`kernel fn` and `device fn` mark functions for GPU compilation (Mojo-style
`compile_offload`): the typed MLIR backend slices them into a device module
compiled by LLVM's NVPTX (PTX text, loaded via the CUDA driver) or AMDGPU
(code object, loaded via HIP) targets, and embeds the blob in the binary.
No CUDA/ROCm toolkit is needed — only the GPU driver at runtime.

```python
kernel fn square(x: int) -> int:
    return x * x

device fn add1(x: int) -> int:
    return x + 1
```

- `kernel` fns are launchable entry points; `device` fns are only callable
  from other kernel/device functions. Host code cannot call either directly
  (Sema rejects it); kernels are launched via `std.gpu` (`gpu.launch_i` /
  `gpu.launch_f`, `gpu.alloc`/`htod`/`dtoh`/`synchronize`).
- Params and return type must be explicitly annotated scalars
  (`int`/`float`/`bool`); no generics, `comptime`, `extern`, or `entry`.
- Bodies must be free of the host runtime: scalar arithmetic and calls to
  other kernel/device fns only — no builtins (`print`, `len`, ...), no
  allocation, no `for x in <non-range>` iteration. Prefer `while` loops.
- Device target: `A_GPU_TARGET` (default `nvptx64-nvidia-cuda`;
  `amdgcn-amd-amdhsa` for AMD), `A_GPU_CPU` (default `sm_60`/`gfx900`),
  `A_GPU_ATTRS` (LLVM feature string), `A_GPU_OFFLOAD=0` disables.
- Requires the in-process MLIR backend (default); the subprocess
  `mlir-translate`/`llc` path does not extract offloads.
