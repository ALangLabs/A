# Name resolution / module loading

`compiler/astra/frontend/name-resolution/` loads the entry file plus every
module it imports and builds the `ModuleGraph` that later stages consume.

## Files

| File | Contents |
| --- | --- |
| `module_loader.hpp` / `module_loader.cpp` | `struct Module` (id, unique `name`, `path`, `program`), `struct ModuleGraph` (`std::deque<Module> modules`, `order` in dependency order, `entry` id), `ModuleLoaderOptions` (`modulePaths` = `-I` dirs, `stdlibRoot`), `class ModuleLoader` with `load(entryPath, entrySource)`. Internally tracks `State {Loading, Loaded}` per path for cycle detection. `defaultStdlibRoot()` resolves `$A_STDLIB`, executable-relative paths, then `./stdlib`. |
| `native_modules.hpp` | `struct NativeModuleSpec` + `nativeModules()` — the table mapping native module names (`math`, `io`, `fs`, `strings`, `collections`, `unsafe`, `arena`, `core`, `time`, `json`, `binary`, `testing`, `log`, `metrics`, `trace`, `net`, `http`, `terminal`, `env`, `repl`, `py`, `kernel_ops`) to runtime builtin names. `isNativeModule`, `nativeFunction`, `nativeConstant`, `isStdlibBuiltin`. |
| `builtin_docs.hpp` + `builtin_docs_*.hpp` | Per-domain doc tables (`builtin_doc.hpp` entry type) used by Sema signatures, `a doc`-adjacent tooling, and LSP hover/completion. Domains: arena, collections, convert, core, intrinsics, io, json, kernels, log, math, metrics, net, py, terminal, testing, trace, unsafe. |

## How it fits

`ModuleLoader::load` parses each file with `Lexer`+`Parser`, resolves quoted
imports relative to the importer, then `-I` paths, `A_PATH`, and the stdlib
root; bare imports resolve through `resolveBare` against the native module
table. Import cycles and missing modules are reported through the
`DiagnosticSink`. The resulting `ModuleGraph` feeds `Sema`, ownership, IR,
codegen, and the LSP (`Compilation::graph`).

Prelude: `stdlib/prelude.a` is loaded before every program (namespaced under
`prelude.*`). `import std:prelude` / `python:<pkg>` forms are represented on
`ImportStmt`/`FromImportStmt` (`stdModule`, `pythonModule`, `nativeModule`,
`moduleId` fields).

Related: [Parser](frontend-parser.md), [Type checker](frontend-type-checker.md),
[Stdlib](stdlib.md), [overview](overview.md).
