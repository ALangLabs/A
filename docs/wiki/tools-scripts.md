# Supporting tools and scripts

Miscellaneous tooling outside the compiler and LSP.

## `tooling/`

- `build/build.sh` — configure + build + test: `cmake -S . -B build
  -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON`, `cmake --build -j`,
  `ctest --test-dir build --output-on-failure`.
- `release/release.sh` — release packaging: Release+LTO build, ctest, then a
  `dist/a-<version>-<platform>.tar.gz` containing `bin/a`, `stdlib/`, `README.md`.

## `tools/gen-docs/` — builtin/method reference generator

`generate.py` produces `docs/builtins.md` and `docs/methods.md` from the
compiler's own doc tables so docs can't drift: it compiles
`dump_builtin_docs.cpp` (a tiny C++ tool printing the `builtin_docs.hpp` table
as JSON) and merges with `type-checker/method_signatures.hpp`. `--check` mode
fails on stale output (CI guard). Outputs carry an AUTO-GENERATED marker.

## `tools/gen_docs.py` — stdlib doc generator

Walks `stdlib/std/**/*.a`, extracts `##` doc comments before
`fn`/`class`/`enum`/`trait` declarations (regexes `FN_RE`, `STRUCT_RE`,
`METHOD_SIG_RE`), and emits `compiler/stages/name-resolution/builtin_docs.hpp`
(note: path in the script predates the current `compiler/astra/` layout),
`docs/standard-library/auto/*.md`, and `docs/standard-library/auto.json`.
`--check` for CI.

## `tools/benchmark-runner/`

- `run.sh` — times every `benchmarks/*/*.a` with `a run`, `RUNS` repetitions
  (default 3), best-of reporting.
- `compare.py` — compares benchmark results across runs/toolchains.

## `websearchcli.py`

Repo-root standalone agent CLI for web search via the Exa MCP endpoint
(`https://mcp.exa.ai/mcp`, `web_search_exa`, no API key). Stdlib-only Python;
see AGENTS.md for usage. Unrelated to the compiler itself.

## `.github/workflows/ci.yml`

CI: Release build with warnings-as-errors, version consistency check across
`CMakeLists.txt` / `version.hpp` / `kServerVersion` / `package.json`, ctest,
and a Debug ASan+UBSan build running the integration suite.

Related: [Tests](tests.md), [Examples](examples.md),
[Benchmarks](benchmarks.md), [overview](overview.md).
