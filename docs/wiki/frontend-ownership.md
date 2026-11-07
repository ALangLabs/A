# Ownership analysis

`compiler/astra/frontend/ownership/` is a flow-sensitive pass layered on top of
Sema that checks move semantics and borrowing rules. Entry point:
`void analyzeOwnership(const ModuleGraph&, DiagnosticSink&)`.

## Files

| File | Role |
| --- | --- |
| `ownership.hpp` | Public API + documented rules: `move(x)` transfers a value (use/move again before reassignment is an error); unit-step slices (`items[1..4]`) create views that forbid changing the lender's length or moving it, while element writes stay allowed. |
| `ownership.cpp` | `analyzeOwnership` driver. |
| `ownership_internal.hpp` | `namespace a::detail`: `OwnershipState` (`moved`, `movedAt`), `BorrowKind {View, Shared, Mutable}`, `isBorrowExpression` (`borrow(x)`/`borrow_mut(x)`), `rootIdentName`, `Scope`, and `class Analyzer` with snapshot/merge machinery. |
| `ownership_state.cpp` / `ownership_snapshot.cpp` | Scope/binding state, branch+loop state merging (`Snapshot`, `mergeState`, `unionBorrows`). |
| `ownership_stmt.cpp` / `ownership_expr.cpp` / `ownership_borrow.cpp` | Statement/expression walkers, move detection, borrow registration and conflict checks (`requireNotBorrowed`, `isStructuralMutator` for `append`/`extend`/`insert`/`pop`/`remove`/`clear`/`remove_at`). |

## How it fits

Runs once per program after `Sema` inside `runFrontendPipeline`; diagnostics go
to the shared sink and abort compilation when errors exist. It is purely a
checker — it does not transform the AST. Related:
[Type checker](frontend-type-checker.md), [Codegen](frontend-codegen.md),
[overview](overview.md).
