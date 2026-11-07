# Type checker (Sema)

`compiler/astra/frontend/type-checker/` performs name resolution within
modules, type inference, and semantic checks. It is the largest frontend stage.

## Files

| File | Role |
| --- | --- |
| `sema.hpp` | `class Sema` — `analyze()`, `analyzeParallel(jobs)`, `analyzeAsync(jobs)` (background `std::future` for the LSP). Result type `SemaResult` collects qualified `functions`/`structs`/`enums`, per-module `ModuleExports`, `fnModule`/`structModule` maps, `fnParamTypes`/`fnReturnTypes`, `variables` (`VarTypeInfo`) and `exprTypes` for editor hover/inlay hints. |
| `sema_internal.hpp` | Shared internals: `gVarMutex` (serializes type-var unification across parallel workers), `terminatesFlow`, builtin-name classifiers. |
| `types.hpp` | `enum class TypeKind` (Error, Void, Bool, Int, Float, Char, String, Array, Range, Optional, Result, Tuple, Struct, Enum, Function, Module, Map, Set, Allocator, Pointer, Json, IO, Var) and `struct Type` (bits/sign, `elem`, `errorType`, `structName`, `moduleId`, `params`, `ret`, `length`, Var fields: `varId`, `constraint`, `allowDefault`, `link`). Constructors `typeInt/typeFloat/typeArray/...`, predicates `isIntegerType`/`isNumericType`, `builtinTypeByName` (`int`, `i8`…`u64`, `f32`/`f64`, `String`, `Allocator`, `Json`, `IO`, `Pointer`), `typeName()`/`shortName()`. |
| `method_signatures.hpp` | Primitive-type method signatures shared with docs/LSP. |
| `debug/magic_identifiers.hpp` | `__file__`, `__line__`, `__func__`, `__repr__`, `__debug_time__`, `__debug_context__`, `__ic_format__` handling. |
| `sema_module.cpp` | Per-module driver: `analyzeModule`, `registerModule`, `checkModule`, `checkModuleBodies`, `registerImports`, `buildModuleExports`, module aliases/`from` bindings. |
| `sema_core.cpp` | Type-variable machinery: `freshVar`, `unify`, `bindVar`, `occursIn`, `constrainVar`, `resolveTypeExpr`, `instantiateType`, `makeSignatureTypes`; scope stack (`pushScope`/`popScope`/`lookupVar`/`declareVar`, unused-local reporting). |
| `sema_decls.cpp` / `sema_constructors.cpp` / `sema_pattern.cpp` | Declaration registration (structs/enums/functions/method signatures), struct construction & variant construction, pattern checking + match exhaustiveness (`collectPatternCoverage`). |
| `sema_expr.cpp` / `sema_calls.cpp` / `sema_literals.cpp` / `sema_stmt.cpp` / `sema_checks.cpp` | Expression inference (`inferExpr` and per-node `infer*`), call checking (`checkFunctionCall`, `checkMethodCall`, `checkStructConstruction`, `checkVariantConstruction`, `checkLambdaAgainst`), literal typing, statement checks (`checkLet`, `checkAssign`, `checkIf`, loops, `checkReturn`, `checkDefer`), and whole-program checks (`checkTypeVariables`, `checkDeclaredVariables`, `checkReturnPaths`). |
| `sema_builtins*.cpp` | Builtin call domains dispatched from `checkBuiltinCall`: values, io, memory, numeric, system, collections, json, testing, log, metrics, trace, net, kernels. |

## Type system notes

- Hindley-Milner-style unification over `TypeKind::Var` nodes with
  `Constraint::{None,Integer,Floating,Numeric}` and `allowDefault` (unconstrained
  literals default to i64/f64 — also applied by `ir::defaultType`).
- Qualified names use `::`; `qualified()`/`qualifiedFor()` build them per
  module. `ModuleExports` snapshots per-module symbols for `import` resolution.
- Flow narrowing (ty/pyrefly style): `if x != None:` refines `T?` to `T` in the
  then-branch via `narrowingFacts`/`applyNarrowing`/`restoreNarrowing`.
- Parallel checking: declarations register serially, then per-module body
  checking runs on workers (`analyzeParallel`); unification is serialized by
  `gVarMutex` so results match `analyze()`.
- `BuiltinId` on `CallExpr` is set here for unshadowed builtins so codegen can
  dispatch without name lookups (`ast/builtins.hpp`).

Related: [AST](frontend-ast.md), [Ownership](frontend-ownership.md),
[Name resolution](frontend-name-resolution.md),
[overview](overview.md).
