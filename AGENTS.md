# Agent instructions for the A toolchain

This repo contains the **A programming language** compiler/toolchain (`a`), LSP (`a-lsp`), runtime, stdlib, docs, examples, and tests. Prefer reading existing docs and nearby code before changing behavior.

## Read first
- [`README.md`](README.md) — overview, CLI quickstart, language/stdlib highlights.
- [`docs/README.md`](docs/README.md) — documentation index.
- [`docs/wiki/stdlib.md`](docs/wiki/stdlib.md) and [`docs/wiki/builtins.md`](docs/wiki/builtins.md) — stdlib API and native builtin reference (current implementation).
- `examples/` — runnable examples by category.

## Build and test (SUPERFAST)
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON -DA_ENABLE_LTO=OFF \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```
Or one-shot: `tooling/build/build.sh` (add `--lto` for faster binaries / slower builds). Targeted: `cmake --build build -j$(nproc) --target a` (or `a-lsp`). Build outputs live at the build root; LTO off by default (fastest build). Do NOT use `-DCMAKE_UNITY_BUILD=ON` — it breaks the `run-cache` test.

Useful targeted checks:
- Integration/golden suite only: `tests/integration/run_tests.sh build/a` (cases in `tests/integration/cases/`: `*.a` plus optional `.out/.in/.err/.exit/.flags`; `.flags` with `check` runs `a check`).
- CTest targets defined in `tests/CMakeLists.txt`: `language-suite`, `tooling-tests`, `ir-tests`, `tooling-cli`, and `language-server-smoke` when Python 3 is available.
- LSP smoke test directly: `python3 tools/lsp/smoke_test.py build/a-lsp`.
- Examples sanity: `for f in examples/*/*.a; do ./build/a run "$f" >/dev/null || echo "FAIL $f"; done` (pick specific categories when faster).

CI (`.github/workflows/ci.yml`) builds with `-DA_WARNINGS_AS_ERRORS=ON`, runs ctest, checks version consistency across `CMakeLists.txt`, `compiler/astra/frontend/driver/version.hpp`, `tools/lsp/server.cpp` (`kServerVersion`), and `tools/vscode-a/package.json`, and runs an ASan/UBSan integration pass.


## Project layout (high level)
- `apm/` — the A package manager (A modules for manifests, registry/resolution, install, CLI).
- `install/` — user-facing installers: `install.sh` (curl|sh, Linux/macOS) and `install.ps1` (irm|iex, Windows). They pull versioned assets (`a-<platform>.tar.gz|.zip`, `a-language.vsix`) from GitHub Releases; `.github/workflows/release.yml` builds those assets on `v*` tags via `tooling/release/package.sh` (which stages `bin/` + `astralib*` + `stdlib` — everything `a` discovers relative to its exe).
- `compiler/astra/frontend/` — C++ frontend modules (`a.hpp` umbrella; `diagnostics`, `lexer`, `ast`, `parser`, `name-resolution`, `type-checker`, `ownership`, `llvm`, `tooling`, `pipeline`, `driver`, `cli`). The C Astra backend lives in `compiler/astra/backend/`, with the A runtime in `compiler/astra/runtime/`.
- `compiler/astra/runtime/` — the A runtime: the C modules linked into compiled programs (`a_runtime.h`/`a_runtime.c` + domain modules such as `core_*.c`, `value_*.c`, `methods*.c`, `builtins_*.c`, `bytes_*.c`, `fs.c`, `json.c`, `os.c`, `net.c`, `time.c`, …; see `compiler/astra/runtime/` for the current set) plus the C++ frontend-support library in `collections/` and `strings/`.
- `stdlib/` — stdlib docs/notes and A-file modules (native execution is delegated to the Astra backend (`compiler/astra/`); the A-subset it supports is still growing — confirm via docs/tests when changing behavior).
- `tools/lsp/` — `a-lsp` (stdio LSP) and smoke tests (`smoke_test.py`, `apm_smoke_test.py`); `tools/vscode-a` — VS Code extension.
- `tests/` — integration, IR, and tooling tests; `examples/` — runnable programs; `docs/` — spec/CLI/compiler/stdlib docs.
- `compiler/astra/` — Astra toolchain: the assembler/linker backend in `backend/` (the `astra` driver + `libastra`, built by `cmake/libastra.cmake` into `build/astra/` + staged `build/astralib/`; it assembles `.s`/`.S` and links objects — C input is rejected and the TCC-derived C statement/call codegen was stripped; what remains is the assembler/linker plus the expression/symbol-table machinery the assembler shares for operands), the A runtime in `runtime/` (`a_runtime.h` public API + `a_runtime.c` dispatch/printing + modular domain C modules sharing `a_runtime_internal.h`, prebuilt once at cmake time by the system C compiler for the host and by a detected `<arch>-gcc`/`A_RUNTIME_CC_<ARCH>` cross toolchain into `libaruntime-<arch>.a`), the C++ frontend in `frontend/` (lex/parse/sema/codegen/IR for `a check`/`a run`/`a build`/`a fmt`/`a doc`/`a ir`, LSP). The frontend lowers the checked AST directly to LLVM IR (`frontend/llvm/llvm_emit.cpp`); the pipeline runs `opt -O<n>` → `llc` → `clang` (located via `A_LLVM_OPT`/`A_LLVM_LLC`/`A_LLVM_CC` or PATH) and links the object with the staged `libaruntime.astra` — astra is now an LLVM wrapper, host targets only (cross `--target` is rejected). `A_CODEGEN=c`/`asm` are rejected. RISC-V32 is unsupported. Supported host OSes are Linux, macOS, and Windows.

## Coding and change conventions
- C++20 in `namespace a`; keep headers next to implementations; follow the existing stage/module structure. Keep warnings clean — CI treats warnings as errors.
- Use Conventional-style commit subjects scoped by area when committing (e.g., `fix(sema): ...`, `feat(lsp): ...`). Keep PRs focused on one behavior.
- **Version bumps:** update all version sources together when changing the project version (see CI check above).
- **Stdlib/native member changes:** update `docs/wiki/stdlib.md` / `docs/wiki/builtins.md`, the LSP builtin docs table (`builtin_docs*.hpp` under `compiler/astra/frontend/name-resolution/`), and add/adjust a runnable example or test coverage when appropriate.
- **Tooling/LSP behavior changes:** extend golden integration cases and/or `tools/lsp/smoke_test.py` as relevant.
- `a build`/`a run`/`a test` lower A to LLVM IR and use `opt`/`llc`/`clang` to produce and (for `run`/`test`) execute a native binary linked with `libaruntime.astra` (built from `compiler/astra/runtime/`). Python interop is discovered at runtime (not linked at build time); some stdlib modules behave differently across interpreter/native backends — validate with the right command path (`a run`, `a test`, `a check`, `a build`).

Do not edit generated/local build artifacts under `build/` unless explicitly requested.
