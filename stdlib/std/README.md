
This directory mirrors the layout of [Mojo's `stdlib/std/`](https://github.com/modular/modular/tree/main/mojo/stdlib/std), adapted for the A language.

## Layout

| Package | Kind | Description |
| --- | --- | --- |
| `prelude/` | A (auto-loaded) | Auto-imported vocabulary (`Vec4`, `Result`/`Ok`/`Err`, helpers). Loaded before every program by the module loader. |
| `builtin/` | A | Conversions and predicates (`Int`, `Float`, `Bool`, `String`, `len_of`, `range_list`, assertions). |
| `collections/` | A | List/dict helpers (`map_list`, `filter_list`, `reduce_list`, `sum_list`, etc.). |
| `math/` | A | Math wrappers over the native `math` module (`sqrt`, `abs_val`, `clamp`, constants). |
| `memory/` | A | Arena allocator wrappers over the native `arena` module. |
| `simd/` | A | Fixed-width vector helpers (`Vec4`/list lane ops, dot/add/scale). Scalar emulation today. |
| `traits/` | A | Trait-like helpers (`Equatable`, `Hashable`, `Comparable`, `eq`, `is_copyable`). |
| `iter/` | A | Iterator/combinator helpers (`range_step`, `repeat`, `take`, `zip_apply`, `enumerate_pairs`). |
| `format/` | A | Text formatting (`csv_row`, `table`). |
| `test/` | A | Assertion harness (`check`, `equal`, `require`, `report`). |
| `comptime/` | A | Compile-time/runtime helpers (`type_tag`, `repeat_const`, `const_range`). |
| `gpu/` | A | GPU stubs (Device/Buffer/Kernel abstractions; host-interpreted, no real GPU backend yet). |
| `binary/`, `core/`, `fs/`, `http/`, `json/`, `log/`, `metrics/`, `net/`, `time/`, `trace/`, `testing/` | native-backed | Package stubs for toolchain-native modules. `import <name>` resolves to the native implementation in `compiler/stages/backend/stdlib/builtins.cpp`. |


- **Compiler stays C++** (Modular KGEN precedent). Stdlib and kernels are written in A where practical.
- **Prelude** is auto-loaded (`stdlib/prelude.a` / `stdlib/std/prelude/prelude.a`) so core vocabulary is available without explicit imports, mirroring Mojo's `std/prelude`.
- **Native modules** remain the performance/capability foundation (I/O, net, JSON, tracing, etc.). Self-hosted `.a` packages wrap and compose them.
- **A semantics are currently Python-style dynamic** (plain mutable assignment, optional annotations, dynamic arithmetic). The layout is Mojo-inspired, but typing/ownership semantics follow current A behavior unless explicitly evolved.
