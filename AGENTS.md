# Agent instructions for the A toolchain

This repo contains the **A programming language** compiler/toolchain (`a`), LSP (`a-lsp`), runtime, stdlib, docs, examples, and tests. Prefer reading existing docs and nearby code before changing behavior.

## Read first
- [`README.md`](README.md) — overview, CLI quickstart, language/stdlib highlights.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — layout, conventions, versioning, docs/stdlib/LSP expectations.
- [`docs/README.md`](docs/README.md) and [`docs/standard-library/README.md`](docs/standard-library/README.md) — language/spec and stdlib API reference (current implementation).
- [`examples/README.md`](examples/README.md) — runnable examples by category.

## Build and test
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```
`tooling/build/build.sh` performs the same Release build + ctest. Build outputs (`a`, `a-lsp`, tests) live at the build root.

Useful targeted checks:
- Integration/golden suite only: `tests/integration/run_tests.sh build/a` (cases in `tests/integration/cases/`: `*.a` plus optional `.out/.in/.err/.exit/.flags`; `.flags` with `check` runs `a check`).
- CTest targets defined in [`tests/CMakeLists.txt`](tests/CMakeLists.txt): `language-suite`, `tooling-tests`, `ir-tests`, `tooling-cli`, and `language-server-smoke` when Python 3 is available.
- LSP smoke test directly: `python3 tools/lsp/smoke_test.py build/a-lsp`.
- Examples sanity: `for f in examples/*/*.a; do ./build/a run "$f" >/dev/null || echo "FAIL $f"; done` (pick specific categories when faster).

CI (`.github/workflows/ci.yml`) builds with `-DA_WARNINGS_AS_ERRORS=ON`, runs ctest, checks version consistency across `CMakeLists.txt`, `compiler/stages/driver/version.hpp`, `tools/lsp/server.cpp` (`kServerVersion`), and `tools/vscode-a/package.json`, and runs an ASan/UBSan integration pass.

## Project layout (high level)
- `compiler/` — toolchain sources (`a.hpp` umbrella; `stages/*` modules such as lexer, parser, ast, diagnostics, name-resolution, type-checker, ownership, ir, optimization, backend, tooling, pipeline, object, driver, cli).
- `runtime/` — runtime values, memory, strings, collections, IO.
- `stdlib/` — stdlib docs/notes and A-file modules (many capabilities are native-backed; some modules are interpreter/KGEN-path oriented — confirm via docs/tests when changing behavior).
- `kernels/` — A compute kernels.
- `tools/lsp/` — `a-lsp` (stdio LSP) and smoke test; `tools/vscode-a/` — VS Code extension.
- `tests/` — integration, IR, and tooling tests; `examples/` — runnable programs; `docs/` — spec/CLI/compiler/stdlib docs.
- `third_party/tinycc/` — vendored TinyCC used by the in-process JIT backend.

## Coding and change conventions
- C++20 in `namespace a`; keep headers next to implementations; follow the existing stage/module structure. Keep warnings clean — CI treats warnings as errors.
- Use Conventional-style commit subjects scoped by area when committing (e.g., `fix(sema): ...`, `feat(lsp): ...`). Keep PRs focused on one behavior.
- **Version bumps:** update all version sources together when changing the project version (see CI check above).
- **Stdlib/native member changes:** update [`docs/standard-library/README.md`](docs/standard-library/README.md), the LSP builtin docs table (`builtin_docs.hpp` under compiler/name-resolution/tooling as used by the project), and add/adjust a runnable example or test coverage when appropriate.
- **Tooling/LSP behavior changes:** extend golden integration cases and/or `tools/lsp/smoke_test.py` as relevant.
- `a build` compiles emitted C++ via `g++ -O3 -std=c++20` on `PATH`. Python interop is discovered at runtime (not linked at build time); some stdlib modules behave differently across interpreter/native backends — validate with the right command path (`a run`, `a test`, `a check`, `a build`).

Do not edit generated/local build artifacts under `build/` unless explicitly requested.
