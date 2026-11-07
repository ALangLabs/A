# IR

`compiler/astra/frontend/ir/` lowers the Sema-checked AST into a typed SSA IR
used by `a ir` and the optimizer. It is **not** on the `a run`/`a build` path —
the asm backend lowers the AST directly.

## Files

| File | Role |
| --- | --- |
| `ir.hpp` | Public IR definition: `enum class Op` (Const*, Param, Phi, Copy, Binary, Unary, Select, Call/CallValue, Index, Slice, Field, Array/Tuple/Map/Set, Range, StringConcat, Iter*, TupleGet, StoreIndex/StoreField, Global, FuncRef, Lambda, and LLVM-style Cast/GetElementPtr/ExtractValue/InsertValue/Freeze/Opaque), `struct Inst` (SSA `id`, `type`, `text`, value fields, `operands`, `incomingBlocks` for Phi, `tailCall`), `Terminator` (Jump/Branch/Return/Unreachable), `Block`, `Parameter`, `Function`, `Program`. API: `lowerProgram(graph, sema)`, `printFunction`/`printProgram`, `defaultType` (applies i64/f64 defaults to unconstrained vars), `opName`. |
| `ir.cpp` | `opName`, `lowerProgram` driver (lowers every top-level `FnDecl` plus entry-module top-level statements as `main`), `defaultType`. |
| `ir_internal.hpp` | `namespace a::ir::detail::Builder` — per-function lowering state: scope envs (`StringMap<int>`), def/use maps, loop contexts, phi materialization at joins (`recordEdge`, `pushPhi`, `materializeJoin`), and `lower*` methods for statements/expressions. Helpers `collectAssigned*`, `castKindFor`, `isConversionName`. |
| `ir_builder_core.cpp` | Builder skeleton: blocks, env snapshots, `emit`, value numbering helpers. |
| `ir_builder_stmt.cpp` | Statement lowering: let/assign/destructure/if/while/do-while/for (range fast path `lowerForRange` + generic iterable `lowerForIterable`)/loop/return/defer. |
| `ir_builder_expr.cpp` | Expression lowering: literals, idents, unary/binary, calls, indexing, aggregates. |
| `ir_builder_flow.cpp` | Control-flow helpers: branches, loops, break/continue, phi joins. |
| `ir_analysis.cpp` | Shared analysis helpers (`intType`, `lookupEnv`, `collectAssigned*`). |
| `ir_print.cpp` | Textual IR printer (`printProgram`) used by `a ir`. |

## Constructs not yet represented

Closures, pattern matching, and comprehensions lower to `Op::Opaque` where the
SSA subset cannot express them; the comment in `ir.hpp` documents this.

Related: [Type checker](frontend-type-checker.md),
[Optimizer](frontend-optimization.md),
[Driver/CLI](frontend-driver.md), [overview](overview.md).
