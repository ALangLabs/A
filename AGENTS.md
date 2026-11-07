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

CI (`.github/workflows/ci.yml`) builds with `-DA_WARNINGS_AS_ERRORS=ON`, runs ctest, checks version consistency across `CMakeLists.txt`, `compiler/astra/frontend/driver/version.hpp`, `tools/lsp/server.cpp` (`kServerVersion`), and `tools/vscode-a/package.json`, and runs an ASan/UBSan integration pass.

## Web search (agent CLI)
Use the repo-root [`websearchcli.py`](websearchcli.py) for web searching instead of guessing or a built-in web tool whenever you need current/external information (docs, upstream APIs, error messages, versioning). It queries the Exa MCP endpoint (`https://mcp.exa.ai/mcp`, `web_search_exa`, no API key needed) and prints ranked titles, URLs, and snippets; it requires internet access.
```sh
python3 websearchcli.py "<query>" [-n 1-20] [--type auto|neural|keyword] [--livecrawl fallback|always|never] [--json] [--timeout SECS]
# e.g. python3 websearchcli.py "Astra compiler backend" -n 5 --type keyword
#      python3 websearchcli.py "latest news about X" --livecrawl always --json
```
- Defaults mirror the old vtx `WebTool`: `-n 8`, `--type auto` (hybrid), `--livecrawl fallback`; use `--type neural` for concept matching, `--type keyword` for exact text, `--livecrawl always` for a fresh crawl, `--json` when you need to parse the result (`{"query": ..., "result": ...}`).
- Exit `0` prints results to stdout; exit `1` means the search failed (message on stderr — retry once with different flags, then report it as unavailable; do not fabricate results), exit `2` means bad arguments (`--help` lists them).
- The module is also importable (`from websearchcli import search`) and is stdlib-only, so it runs from any checkout without `vtx`/`pydantic`/`httpx`.

## Project layout (high level)
- `websearchcli.py` — standalone agent web-search CLI (Exa MCP; see "Web search" above).
- `compiler/astra/frontend/` — C++ frontend modules (`a.hpp` umbrella; `diagnostics`, `lexer`, `ast`, `parser`, `name-resolution`, `type-checker`, `ownership`, `codegen`, `ir`, `optimization`, `tooling`, `pipeline`, `driver`, `cli`). The C Astra backend lives in `compiler/astra/backend/`, with the A runtime in `compiler/astra/runtime/`.
- `compiler/astra/runtime/` — the A runtime: the C modules linked into compiled programs (`a_runtime.h`/`a_runtime.c` + domain modules like `core.c`, `value.c`, `methods.c`, `builtins.c`, `json.c`, `python.c`, …) plus the C++ frontend-support library in `collections/`, `errors/`, `io/`, `memory/`, `strings/`.
- `stdlib/` — stdlib docs/notes and A-file modules (native execution is delegated to the Astra backend (`compiler/astra/`); the A-subset it supports is still growing — confirm via docs/tests when changing behavior).
- `kernels/` — A compute kernels.
- `tools/lsp/` — `a-lsp` (stdio LSP) and smoke test; `tools/vscode-a/` — VS Code extension.
- `tests/` — integration, IR, and tooling tests; `examples/` — runnable programs; `docs/` — spec/CLI/compiler/stdlib docs.
- `compiler/astra/` — Astra toolchain: the C backend in `backend/` (the `astra` driver + `libastra`, built by `cmake/libastra.cmake` into `build/astra/` + staged `build/astralib/`), the A runtime in `runtime/` (`a_runtime.h` public API + `a_runtime.c` dispatch/printing + modular domain modules (`core.c`, `value.c`, `methods.c`, `builtins.c`, `fs.c`, `json.c`, `binary.c`, `log.c`, `metrics.c`, `trace.c`, `net.c`, `arena.c`, `unsafe.c`, `python.c`, `terminal.c`, `time.c`, `intrinsics.c`) sharing `a_runtime_internal.h`, all compiled and linked with the generated code), the C++ frontend in `frontend/` (lex/parse/sema/codegen/IR for `a check`/`a run`/`a build`/`a fmt`/`a doc`/`a emit-ir`, LSP). The frontend emits x86-64 AT&T assembly (`frontend/codegen/`, `emitAsm`); the pipeline stages a temp `.s` and `astra` compiles/assembles it together with the `compiler/astra/runtime/` C modules into a native machine-code executable.

## Coding and change conventions
- C++20 in `namespace a`; keep headers next to implementations; follow the existing stage/module structure. Keep warnings clean — CI treats warnings as errors.
- Use Conventional-style commit subjects scoped by area when committing (e.g., `fix(sema): ...`, `feat(lsp): ...`). Keep PRs focused on one behavior.
- **Version bumps:** update all version sources together when changing the project version (see CI check above).
- **Stdlib/native member changes:** update [`docs/standard-library/README.md`](docs/standard-library/README.md), the LSP builtin docs table (`builtin_docs.hpp` under compiler/name-resolution/tooling as used by the project), and add/adjust a runnable example or test coverage when appropriate.
- **Tooling/LSP behavior changes:** extend golden integration cases and/or `tools/lsp/smoke_test.py` as relevant.
- `a build`/`a run`/`a test` lower A to x86-64 assembly and use the staged `astra` driver to produce and (for `run`/`test`) execute a native binary linked with the `compiler/astra/runtime/` modules (`a_runtime.c` + `rt_*.c`) (no `g++` build step). Python interop is discovered at runtime (not linked at build time); some stdlib modules behave differently across interpreter/native backends — validate with the right command path (`a run`, `a test`, `a check`, `a build`).

Do not edit generated/local build artifacts under `build/` unless explicitly requested.
