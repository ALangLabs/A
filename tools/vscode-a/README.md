# A Language — VS Code extension

Syntax highlighting, snippets, tasks (Run/Build/Test/Check/Format),
check-on-save, format-on-save, the Test Explorer, a REPL, run-selection,
and the `a-lsp` language server client for the
[A programming language](../../README.md).

The extension uses only the standard VS Code Extension API (`vscode`
module) and spawns local processes (`a`, `a-lsp`) — there are no
Microsoft-Marketplace-only APIs or dependencies. It therefore runs in
VS Code and in forks/editors that consume [Open VSX](https://open-vsx.org)
(Cursor, Windsurf, VSCodium, code-server, Theia-based IDEs, …).

## Features

- **Editing**: TextMate grammar, snippets, semantic tokens (via `a-lsp`),
  folding, bracket matching, document outline/breadcrumbs.
- **IntelliSense**: `a-lsp` provides diagnostics as you type, hover, go to
  definition/references, completion + resolve, signature help, rename,
  document highlight, selection ranges, inlay hints (opt-in), workspace
  symbols, document links, code lenses, code actions, and call/type
  hierarchies. Without `a-lsp` (or in untrusted workspaces) the extension
  falls back to `a check` on save plus lexical/stdlib completion and hover.
- **Run**: `A: Run Program` (editor title button, `Ctrl+Shift+B`, `F5`,
  `Ctrl+F5`) runs in a dedicated "A" terminal; `A: Run Selection/Line`
  (`Shift+Enter`) evaluates the selection or current line via `a -c`;
  `A: Start REPL` opens an interactive `a repl` terminal.
- **Tests**: every `fn test_*()` appears in the **Testing** view, grouped
  by workspace folder and file. Run all, one file, or a single test
  (`a test --filter`); per-test results map back to pass/fail in the view.
  Code lenses (`Run`, `Run Tests`) sit above each file and test function.
- **Tasks**: `A: Run/Build/Test/Check/Format` tasks (`Ctrl+Shift+B` builds
  by default). A starter `.vscode/tasks.json` is generated for workspaces
  that contain `.astra` files.
- **Format**: `a fmt` on demand, range formatting, and optional
  `a.formatOnSave` (works with `a-lsp` or the `a fmt` fallback).
- **Setup hints**: one-time warnings when the `a` compiler or `a-lsp`
  cannot be found, with shortcuts to `a.compilerPath` / `a.server.path`.

## Configuration

| Setting | Default | Purpose |
| --- | --- | --- |
| `a.compilerPath` | `a` on PATH / `build*/a` | Compiler executable |
| `a.entryFile` | active file | Entry point for check/run/test when editing modules |
| `a.stdlibPath`, `a.modulePaths` | — | `--stdlib` / `-I` search paths |
| `a.useLanguageServer` | `true` | Enable `a-lsp` |
| `a.server.path`, `a.server.args`, `a.server.modulePaths` | — | `a-lsp` executable and args |
| `a.checkOnSave` | `true` | Fallback `a check` on open/save |
| `a.formatOnSave` | `false` | Format on save |
| `a.inlayHints.enabled` (+ `.types`, `.parameters`) | `false` | Inferred-type and parameter-name hints |
| `a.jobs` | `0` (all cores) | Parallel checking (`--jobs` / `--parallel`) |
| `a.codeLens.enabled` | `true` | Run / Run Tests lenses |
| `a.tasks.autoGenerate` | `true` | Write starter `tasks.json` |
| `a.useRunKeybindings` | `true` | `Ctrl+Shift+B`/`F5`/`Ctrl+F5`/`Shift+Enter` run bindings |
| `a.checkTimeout` | `30000` | Background check timeout (ms) |

## Install

### From a VSIX (any editor, no registry needed)

```sh
npm run package          # produces a-language-<version>.vsix
```

Then in the editor: **Extensions view → `…` → Install from VSIX…** and
pick the `.vsix` file.

### From Open VSX (recommended for forks)

Publishing to Open VSX makes the extension searchable/installable in
the built-in Extensions view of forks and VS Code-compatible editors.

```sh
npm i -g ovsx            # or: npx ovsx
ovsx login               # one-time; creates/uses a namespace token
npm run publish:ovsx     # or: ovsx publish
```

Notes:

- The Open VSX namespace is the `publisher` field in `package.json`
  (`a-lang`). On first publish that namespace is claimed for your
  Open VSX account; if it is already taken, pick an unused `publisher`
  name (remember to bump the version for each publish — the registry
  rejects re-publishing an existing version).
- `ovsx publish` packages the extension itself (equivalent to
  `vsce package` + upload); `npm run package` still produces a local
  `.vsix` via `vsce`.
- Keep `version` in `package.json` in sync with the other version
  sources (`CMakeLists.txt`, `src/astra/frontend/driver/version.hpp`,
  `tools/lsp/server.cpp`) — CI enforces this.

## Develop

Press **F5** in this folder (`.vscode/launch.json`) to launch an
Extension Development Host with the extension active. There are no
npm dependencies to install.
