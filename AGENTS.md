# Agent instructions for the A toolchain

This repo contains the **A programming language** compiler/toolchain (`a`), LSP (`a-lsp`), runtime, stdlib, docs, examples, and tests. Prefer reading existing docs and nearby code before changing behavior.

## Read first
- [`README.md`](README.md) — overview, CLI quickstart, language/stdlib highlights.
- [`docs/README.md`](docs/README.md) — documentation index.
- [`docs/wiki/stdlib.md`](docs/wiki/stdlib.md) and [`docs/wiki/builtins.md`](docs/wiki/builtins.md) — stdlib API and native builtin reference (current implementation).
- `examples/` — runnable examples by category.

## Build and test
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```
`tooling/build/build.sh` performs the same Release build + ctest. Build outputs (`a`, `a-lsp`, tests) live at the build root; `a` and `a-lsp` are both in the default `all` target (`--target a` builds only `a`, `--target a-lsp` only the LSP). Link-time optimization is **off by default** for fast dev builds — pass `-DA_ENABLE_LTO=ON` at configure time for release/perf builds.

Useful targeted checks:
- Integration/golden suite only: `tests/integration/run_tests.sh build/a` (cases in `tests/integration/cases/`: `*.a` plus optional `.out/.in/.err/.exit/.flags`; `.flags` with `check` runs `a check`).
- CTest targets defined in `tests/CMakeLists.txt`: `language-suite`, `tooling-tests`, `ir-tests`, `tooling-cli`, and `language-server-smoke` when Python 3 is available.
- LSP smoke test directly: `python3 tools/lsp/smoke_test.py build/a-lsp`.
- Examples sanity: `for f in examples/*/*.a; do ./build/a run "$f" >/dev/null || echo "FAIL $f"; done` (pick specific categories when faster).

CI (`.github/workflows/ci.yml`) builds with `-DA_WARNINGS_AS_ERRORS=ON`, runs ctest, checks version consistency across `CMakeLists.txt`, `compiler/astra/frontend/driver/version.hpp`, `tools/lsp/server.cpp` (`kServerVersion`), and `tools/vscode-a/package.json`, and runs an ASan/UBSan integration pass.


## Project layout (high level)
- `websearchcli.py` — standalone agent web-search CLI (Exa MCP; see "Web search" above).
- `compiler/astra/frontend/` — C++ frontend modules (`a.hpp` umbrella; `diagnostics`, `lexer`, `ast`, `parser`, `name-resolution`, `type-checker`, `ownership`, `codegen`, `ir`, `optimization`, `tooling`, `pipeline`, `driver`, `cli`). The C Astra backend lives in `compiler/astra/backend/`, with the A runtime in `compiler/astra/runtime/`.
- `compiler/astra/runtime/` — the A runtime: the C modules linked into compiled programs (`a_runtime.h`/`a_runtime.c` + domain modules like `core.c`, `value.c`, `methods.c`, `builtins.c`, `fs.c`, `json.c`, `python.c`, …) plus the C++ frontend-support library in `collections/`, `errors/`, `io/`, `memory/`, `strings/`.
- `stdlib/` — stdlib docs/notes and A-file modules (native execution is delegated to the Astra backend (`compiler/astra/`); the A-subset it supports is still growing — confirm via docs/tests when changing behavior).
- `tools/lsp/` — `a-lsp` (stdio LSP) and smoke test; `tools/vscode-a/` — VS Code extension.
- `tests/` — integration, IR, and tooling tests; `examples/` — runnable programs; `docs/` — spec/CLI/compiler/stdlib docs.
- `compiler/astra/` — Astra toolchain: the C backend in `backend/` (the `astra` driver + `libastra`, built by `cmake/libastra.cmake` into `build/astra/` + staged `build/astralib/`), the A runtime in `runtime/` (`a_runtime.h` public API + `a_runtime.c` dispatch/printing + modular domain modules (`core.c`, `value.c`, `methods.c`, `builtins.c`, `fs.c`, `json.c`, `binary.c`, `log.c`, `metrics.c`, `trace.c`, `net.c`, `arena.c`, `unsafe.c`, `python.c`, `terminal.c`, `time.c`, `intrinsics.c`) sharing `a_runtime_internal.h`, all compiled and linked with the generated code), the C++ frontend in `frontend/` (lex/parse/sema/codegen/IR for `a check`/`a run`/`a build`/`a fmt`/`a doc`/`a emit-ir`, LSP). The frontend emits x86-64 AT&T assembly for x86-64 SysV targets (`frontend/codegen/`, `emitAsm`); every other `--target`/OS uses the per-arch native emitter `emitNative` in `codegen_native.cpp` (arm64, riscv64, i386, arm, i8086, and Win64 MS-ABI for `x86_64` on Windows — including `python:` interop via the runtime `py_call` builtin where supported). There is no C emitter: an emitter decline is a hard error. The pipeline stages a temp `.s` and the per-arch `astra`/`astra-<arch>` driver (staged in `build/astra/`, lib dirs in `build/astralib*/`) compiles it together with the `compiler/astra/runtime/` C modules into a machine-code executable. `a build --target i386|arm|aarch64|arm64|riscv64` cross-compiles on the host OS and needs a matching target sysroot (`A_SYSROOT`/`A_SYSROOT_<ARCH>`); the astra driver set is host-OS dependent (Linux: `x86_64 i386 arm arm64 riscv64 i8086`, macOS: `x86_64 arm64`, Windows: `x86_64 i386 arm64`). `A_CODEGEN=asm` forces the assembly emitter; `A_CODEGEN=c` is rejected. `--target i8086` is the exception: a bare-metal real-mode boot image (asm-only, no libc/sysroot, Linux hosts, `a run` via qemu-system-i386, `a test` unsupported; runtime subset is tagged-int arithmetic/comparisons + int `print` — string literals and `while`/methods/lambdas are known-broken). RISC-V32 is unsupported. Supported host OSes are Linux, macOS, and Windows; on Windows TLS/zlib and `python:` interop are unavailable and only the `x86_64`/`i386`/`arm64` astra drivers are built.

## Coding and change conventions
- C++20 in `namespace a`; keep headers next to implementations; follow the existing stage/module structure. Keep warnings clean — CI treats warnings as errors.
- Use Conventional-style commit subjects scoped by area when committing (e.g., `fix(sema): ...`, `feat(lsp): ...`). Keep PRs focused on one behavior.
- **Version bumps:** update all version sources together when changing the project version (see CI check above).
- **Stdlib/native member changes:** update `docs/wiki/stdlib.md` / `docs/wiki/builtins.md`, the LSP builtin docs table (`builtin_docs*.hpp` under `compiler/astra/frontend/name-resolution/`), and add/adjust a runnable example or test coverage when appropriate.
- **Tooling/LSP behavior changes:** extend golden integration cases and/or `tools/lsp/smoke_test.py` as relevant.
- `a build`/`a run`/`a test` lower A to x86-64 assembly and use the staged `astra` driver to produce and (for `run`/`test`) execute a native binary linked with the `compiler/astra/runtime/` modules (`a_runtime.c` + `rt_*.c`) (no `g++` build step). Python interop is discovered at runtime (not linked at build time); some stdlib modules behave differently across interpreter/native backends — validate with the right command path (`a run`, `a test`, `a check`, `a build`).

Do not edit generated/local build artifacts under `build/` unless explicitly requested.
