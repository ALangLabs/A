# Tests

`tests/` contains the compiler's test suites, wired into CTest via
`tests/CMakeLists.txt`.

## CTest targets

| Test | Command | What it runs |
| --- | --- | --- |
| `language-suite` | `tests/integration/run_tests.sh $<TARGET_FILE:a>` | Golden integration cases. |
| `tooling-tests` | `a-tooling-tests` (`tooling/tooling_tests.cpp`) | Unit tests for `tooling::formatSource` and `tooling::extractDocs`/renderers (formatter idempotence, doc extraction). |
| `ir-tests` | `a-ir-tests` (`ir/ir_tests.cpp`) | IR lowering + optimizer checks: builds `ModuleGraph`→`Sema`→`ir::lowerProgram` in-process and asserts on `printFunction`/`printProgram` output and pass effects. |
| `tooling-cli` | `tooling/run_cli_tests.sh $<TARGET_FILE:a>` | CLI-level checks of `a` commands. |
| `language-server-smoke` | `python3 tools/lsp/smoke_test.py $<TARGET_FILE:a-lsp>` | LSP smoke test (only when Python 3 found). |

## Integration golden suite

`tests/integration/run_tests.sh` discovers every `cases/**/*.a` (excluding
`*/lib/*` fixtures) and compares against sibling files:

- `<name>.out` — expected stdout (exact diff)
- `<name>.err` — expected stderr substrings (one per line, `grep -F`)
- `<name>.exit` — expected exit code (default 0)
- `<name>.in` — stdin for the program
- `<name>.flags` — command override (`run` default, or `check`/`test`/...)

It exports `A_STDLIB` to the repo `stdlib/` so fixtures resolve correctly, and
runs each case from its own directory. Case coverage spans arithmetic,
arrays/views, ownership errors (`borrow_*`, `ownership_*`), C-breadth features
(enums, const, do-while, ranges, ternary), comprehensions, comptime params,
default/variadic params, docstrings, destructuring, deterministic cleanup,
error cases (`error_*.a` expecting nonzero exit + stderr), file I/O, fixed
arrays, generics, modules (incl. `modules/lib/` fixtures and cycle/missing
cases), optional narrowing, optionals/result, pipelines, sets/maps, slices,
`std_imports`, `stdlib/` cases (getopt, pycollections, pyenum), SIMD, strings,
structs, suppression diagnostics, the `test` runner, traits, tuples, type
aliases, UTF-8, and warnings (`warn_unreachable`, `warn_unused`).

Related: [tools-scripts](tools-scripts.md), [overview](overview.md).
