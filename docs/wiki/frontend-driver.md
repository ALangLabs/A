# Driver, CLI, and pipeline

Three small directories glue the frontend stages into the `a` binary.

## `cli/` — `a` executable entry point

`cli/main.cpp` parses argv into `struct Options` and dispatches to driver
functions. Commands: `repl`, `run`, `build`, `check`, `test`, `fmt`, `doc`,
`ir`, `version`, `help`. Notable options: `-o`, `-c <code>` (inline source,
implies `run`), `-w/--write`, `--check`, `--indent`, `--json`,
`--parallel`/`-j/--jobs`, `-O0..-O3/-Os/-Oz/-Ofast` and `--opt-level=N`,
`--optnone`, `--opt` (IR only), `-I <dir>`, `--stdlib <dir>`, `--no-color`.
Exit codes follow sysexits style (`kExitUsage = 64`; pipeline uses 0/1/66).
On startup it exports `A_A_BIN` (its own exe path) so compiled programs can
locate the toolchain (e.g. `repl.eval`).

## `driver/` — command implementations

`driver/driver.hpp` declares `DriverConfig` (checkOnly, jsonDiagnostics,
useColor, jobs, optimizationLevel, backend, modulePaths, stdlibRoot) and entry
points `runFile`, `runSource`, `testFile`, `repl`, `buildFile`,
`emitIRFile`, `formatFile`, `documentFile`. `driver.cpp` converts to
`PipelineConfig`, reads files, and implements:

- `repl` — on a TTY, locates and runs the self-hosted A REPL
  (`stdlib/std/repl/main.a`, via `findReplMain`); otherwise a line-based REPL
  using `replInputComplete` (bracket/colon continuation heuristic) +
  `replEval`.
- `formatFile`/`documentFile` — thin wrappers over `tooling::`.

`driver/version.hpp` holds `kVersion`/`kVersionLine` (kept in sync with
`CMakeLists.txt`, `tools/lsp/server.cpp`, `tools/vscode-a/package.json`).

## `pipeline/` — stage orchestration

`pipeline/pipeline.hpp`/`pipeline.cpp`:

- `struct PipelineConfig` — checkOnly, jsonDiagnostics, useColor, optimize,
  optimizationLevel, jobs, modulePaths, stdlibRoot.
- `struct CompiledModule` — `ModuleGraph` + `SemaResult` + `ok`.
- `runFrontendPipeline(path, source, config, sink, diagOut, diagErr)` —
  `ModuleLoader::load` → `Sema::analyze()`/`analyzeParallel(jobs)` →
  `analyzeOwnership`, flushing diagnostics between stages.
- `executePipeline` (`a run`/`check`), `testPipeline` (`a test`),
  `buildPipeline` (`a build`), `emitIRPipeline` (`a ir`), `replEval`/`ReplSession`.
- Astra handoff: `findAstra()` (`A_ASTRA` env → staged `build/astra/astra` or
  `build/astralib/astra` → PATH), `findRuntime()` (`A_RUNTIME_C`, staged
  `astralib`, source tree), `stageSrc()` (temp `.s`, `A_DEBUG_SRC` copies the
  staged source), `astraBuild()` invokes `astra -O<n> -I<runtime> ... <src.s>
  [libaruntime.a | runtime *.c] -o <bin>` — the archive is used at the default
  `-O2` when present.
- `compileAndRun` forks/execs the produced binary (`runBinary`), propagating
  its exit code; `a test` passes `testMode=true` to `emitAsm`.

Related: [Codegen](frontend-codegen.md), [Astra backend](backend-astra.md),
[Runtime](runtime.md), [overview](overview.md).
