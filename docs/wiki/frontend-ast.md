# AST

`compiler/astra/frontend/ast/` defines the abstract syntax tree: polymorphic
node structs tagged by `ExprKind`/`StmtKind` enums so backends dispatch with a
single compare instead of RTTI chains.

## Files

| File | Contents |
| --- | --- |
| `ast.hpp` | Umbrella header re-exporting all sub-headers. |
| `fwd.hpp` | Forward declarations + aliases `ExprPtr`/`StmtPtr`/`TypeExprPtr` (`unique_ptr`) and `TypePtr` (`shared_ptr<Type>`). |
| `kinds.hpp` | `ExprKind` (IntLit…SetLit), `StmtKind` (Let…ImplDecl), `BinOp` (Add…NotIn) — the operator identity used by backends. |
| `exprs.hpp` | `struct Expr` base (`span`, `kind`, `inferredType` filled by Sema) and all expression nodes: literals, `IdentExpr`, `UnaryExpr` (incl. `++`/`--`), `BinaryExpr` (`op` spelling + `binop`), `CallExpr` (with lazy callee cache, `BuiltinId builtinId`, `ic` context fields), `IndexExpr`, `SliceExpr`, `FieldExpr`, `SafeNavExpr`, `ArrayLitExpr`, `RangeExpr`, `TernaryExpr`, `TupleExpr`, `LambdaExpr`, `ComprehensionExpr`, `MatchExpr`/`MatchArm`/`Pattern`, `TryExpr`, `MapLitExpr`, `SetLitExpr`, `StringLitExpr`/`StringPart` (interpolation). Also `icExprIsLiteralLike`/`icExprIsSimple`/`icExprIsBare` helpers for `ic()` call-site classification. |
| `stmts.hpp` | `struct Stmt` base and statement/declaration nodes: `LetStmt`, `AssignStmt`, `ExprStmt`, `DestructureStmt`, `ReturnStmt`, `BreakStmt`, `ContinueStmt`, `DeferStmt`, `ImportStmt`/`FromImportStmt` (quoted path, bare name, `python:`/`std:` prefixes, `moduleId`), `IfStmt`, `WhileStmt`, `DoWhileStmt`, `ForStmt`, `LoopStmt`, `FnDecl` (type params, comptime params, where clause, doc), `StructDecl` (fields, methods, base class, metaclass), `EnumDecl`/`EnumVariant` (algebraic payloads), `TraitDecl`/`TraitSig`, `ImplDecl`, `TypeAliasDecl`, `Program`. Helpers `plainStringText`, `docstringText`. |
| `type_exprs.hpp` | `TypeExpr` hierarchy: `NamedTypeExpr` (qualifiers + type args), `ArrayTypeExpr`, `OptionalTypeExpr`, `TupleTypeExpr`, `MapTypeExpr`, `SetTypeExpr`, `FunctionTypeExpr`, `ParamDecl` (default values, `variadic`). |
| `builtins.hpp` | `enum class BuiltinId` (Print…IcFormat) and `builtinIdForName()` — statically resolved builtin identity stamped on `CallExpr` by Sema so backends don't re-scan names per call. |

## Consumers

Parser produces these nodes; Sema annotates `Expr::inferredType`, `CallExpr::builtinId`,
`FnDecl::moduleId`, docs; ownership, IR lowering, asm codegen, formatter, doc
generator, and the LSP all walk the same tree. Related:
[Parser](frontend-parser.md), [Type checker](frontend-type-checker.md),
[Codegen](frontend-codegen.md).
