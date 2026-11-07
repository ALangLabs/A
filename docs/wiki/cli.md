# CLI and tooling

The `a` binary (`./build/a`) is the single entry point. `a-lsp` is the
language server; the VS Code extension wraps both.

## Commands

| Command | Description |
| --- | --- |
| `a run <file.a>` | Compile and execute. `-c 'code'` runs inline source. |
| `a build <file.a> -o <bin>` | Compile to a native executable (default `a.out`). |
| `a check <file.a>` | Type-check only; `--json` for machine-readable diagnostics. |
| `a test <file.a>` | Run every `fn test_*()`. |
| `a fmt <file.a>` | Format to stdout; `-w` writes in place, `--check` exits nonzero if unformatted, `--indent <n>`. |
| `a doc <file.a>` | Generate docs (markdown; `--json`, `-o <file>`). |
| `a ir <file.a>` | Print the typed SSA IR; `--opt` or `-O*` runs optimization passes. |
| `a repl` | Interactive REPL — self-hosted A shell on a TTY, line REPL when piped. |
| `a version` / `a help` | Version and usage. |

## Options

- `-I <dir>` — module search directory (repeatable).
- `--stdlib <dir>` — override stdlib root (else `$A_STDLIB` / auto-detect).
- `-j <n>`, `--jobs <n>`, `--parallel` — parallel module checking.
- `-O0 -O1 -O2 -O3 -Os -Oz -Ofast`, `--opt-level=N`, `--optnone` — optimization.
- `--no-color` — plain diagnostics.
- `--target <arch>` (`a build` only) — cross-compile target: `x86_64`
  (default; native assembly emitter), `i386`, `arm` (ARMv7 hard-float),
  `aarch64`/`arm64`, `riscv64`. An optional `-linux`/`-windows`/`-macos`
  suffix is accepted but must match the host OS — Astra drivers are built
  per-OS. Non-x86-64 targets go through the portable C emitter and the
  matching `astra-<arch>` driver; linking needs a target sysroot via
  `A_SYSROOT`/`A_SYSROOT_<ARCH>` (e.g. Debian `*-cross` packages extracted
  anywhere — note their `libc.so`/`libm.so` linker scripts use absolute
  paths that must be rewritten relative to the sysroot lib dir).
  `a run`/`a test`/`a repl` always execute natively and reject other targets.

Environment: `A_STDLIB`, `A_ASTRA`, `A_ASTRA_<ARCH>`, `A_RUNTIME_C`,
`A_SYSROOT`, `A_SYSROOT_<ARCH>`, `A_DEBUG_SRC`, `A_JOBS`.

## Editor support

- `a-lsp` — stdio LSP server: diagnostics, hover, definition, references,
  rename, completion, signature help, formatting, semantic tokens, call/type
  hierarchy, inlay hints, code actions.
- `tools/vscode-a` — VS Code extension (syntax, snippets, tasks
  Run/Build/Test/Check/Format, check-on-save).
