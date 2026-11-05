# AGENTS.md

## What this is
- C++20 toolchain for the **A** language (`CMakeLists.txt`: `project(Alang VERSION 0.0.1)`), producing `build/a` (CLI) and `build/a-lsp` (LSP server). Language source files use `.a`.
- **Current semantics (HEAD, commit `7894e30`) are Python-style dynamic, not the static-typed marketing in `README.md`:** no `pvt`/`var`/`const`/`let`/`mut`; plain assignment creates a mutable variable; type annotations are optional; interpreter does dynamic arithmetic/promotion. Trust code and tests over README/docs prose where they conflict.

## Commands
```sh
tooling/build/build.sh                              # configure Release + Werror, build, ctest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
tests/integration/run_tests.sh build/a              # golden language suite (also a ctest)
python3 tools/lsp/smoke_test.py build/a-lsp
tests/tooling/run_cli_tests.sh build/a              # fmt/doc/check/test CLI (also a ctest)
for f in examples/*/*.a; do build/a run "$f" >/dev/null || echo "FAIL $f"; done
./build/a run|check|test|build|fmt|doc|ir <file.a>  # --help for flags; `--backend native|llvm` (code default is `llvm`, help text is stale)
./ac prog.a                                         # wrapper: finds `build/a` or `a` on PATH, runs with `--backend=llvm`
```
- Single focused checks: `ctest --test-dir build -R 'ir-tests|tooling-tests|tooling-cli' --output-on-failure`, or run one case directly with `build/a run tests/integration/cases/<name>.a`.
- Sanitizers (CI job): `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DA_WARNINGS_AS_ERRORS=ON -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && cmake --build build-asan -j && tests/integration/run_tests.sh build-asan/a`
- Prerequisites: CMake 3.20+, C++20 compiler, `g++` on PATH (`a build` shells out to `g++ -O3 -std=c++20`).

## Architecture
- One CMake build, layered static libs in `CMakeLists.txt` (order matters): `a_diagnostics` (incl. `lexer/source.cpp`, to avoid a diagnostics/lexer cycle) -> `a_lexer` -> `a_parser` -> `a_name_resolution` -> `a_type_checker` -> `a_ownership`; `a_ir` -> `a_optimization`; `a_runtime`, `a_backend` (interpreter, native C++ codegen, LLVM), `a_tooling` (formatter/docgen), `a_driver` -> executables `a`, `a-lsp`.
- Entrypoints: `compiler/stages/cli/main.cpp` (`a` subcommands), `compiler/stages/driver/driver.cpp` (pipeline/glue), `tools/lsp/server.cpp` (`a-lsp`, stdio LSP). Pipeline stages live under `compiler/stages/{diagnostics,lexer,parser,ast,name-resolution,type-checker,ownership,ir,optimization,backend,tooling,driver,cli}/` (one CMake module per stage, plus `cmake/AOptions.cmake` and `compiler/a.hpp` umbrella header); runtime support under `runtime/`.
- Lang/stdlib additions usually touch 3 places: implementation (`name-resolution/native_modules.hpp`, `backend/stdlib/builtins.cpp`, `type-checker/method_signatures.hpp`), LSP docs (`name-resolution/builtin_docs.hpp`), and `docs/standard-library/README.md` plus a runnable example. LSP completion/hover is data-driven from those tables; formatter/docgen or LSP changes should extend golden cases or `smoke_test.py`.
- **Self-hosted layout (Mojo-analog):** `stdlib/std/` = A-written stdlib (`builtin/`, `collections/`, `math/`, `memory/`, `simd/`, `traits/`) + `stdlib/prelude.a` auto-vocabulary (Mojo `std/prelude` analog); `kernels/` = A-written MAX-analog compute library (`kernels/src/` fused/matmul/activations); `stdlib/simd/`, `stdlib/gpu/`, `stdlib/comptime/`, `stdlib/traits/` = top-level feature modules. Type-checker features (traits, Result, compile-time params): interpreter-run in `.a` today; codegen specialization + real GPU backend are TODOs. Compiler stays C++ per Modular KGEN precedent.

## Tests & conventions
- Golden tests: `tests/integration/cases/**/*.a`, excluding `lib/` fixtures. Companions: `.out` stdout, `.in` stdin, `.err` stderr substrings, `.exit`, `.flags` (`check`/`test` instead of `run`). Other suites: `tests/tooling/tooling_tests.cpp`, `tests/ir/ir_tests.cpp`, `tests/tooling/run_cli_tests.sh`.
- Version must stay in sync in 4 places or CI fails: `CMakeLists.txt` (`project(Alang VERSION ...)`), `compiler/stages/driver/version.hpp` (`kVersion`), `tools/lsp/server.cpp` (`kServerVersion`), `tools/vscode-a/package.json`.
- Style: C++20, `namespace a`, headers next to implementations, keep warnings clean (CI builds with `-Werror`). `.editorconfig`: 4 spaces for `*.a`/`*.md`, 2 for C++/JSON/YAML/shell.
- Commits: Conventional-style, area-scoped (`feat(lsp): ...`, `fix(sema): ...`), imperative, one logical change per commit. PRs: one behavior per PR, explain *why*, follow `.github/PULL_REQUEST_TEMPLATE.md`.
- Do not commit `build*/`, `*.a` build artifacts, `dist/`, `*.vsix`, `compile_commands.json` (`.gitignore` already handles these; only source `*.a` under `examples/ tests/ stdlib/ benchmarks/` is tracked).
- Optional dependency: OpenSSL enables real TLS (`net.tls_connect`/`https`); without it, it reports unavailable - do not treat that as a failure.
- Reference docs: `CONTRIBUTING.md` (layout, testing, versioning), `docs/` (`language-spec`, `type-system`, `memory-model`, `compiler-design`, `standard-library`), `TODO.md` for the implemented/missing feature audit.
