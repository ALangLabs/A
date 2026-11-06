# A Language for VS Code

Editor support for the [A programming language](../../README.md): syntax highlighting, language intelligence via `a-lsp`, compiler tasks, and fallback analysis when the language server is unavailable.

## What you get

- **Syntax highlighting** for `.a` files (keywords, declarations, types, builtins, numbers, strings/interpolation, comments, operators) and fenced ` ```a ` blocks in Markdown.
- **Language server mode (`a-lsp`, preferred)**
  - diagnostics as you type
  - hover, go to definition/declaration/type definition/implementation
  - references, document/workspace symbols
  - completion and signature help
  - formatting, rename, highlights, folding
  - semantic tokens and inlay hints (when provided by `a-lsp`)
- **Fallback mode (no `a-lsp`, disabled, or untrusted workspace)**
  - check-on-save with `a check --json`
  - lexical outline and import navigation
  - curated stdlib/module completion and hover (including common A stdlib modules like `string`, `textwrap`, `statistics`, `copy`, `json`, `fs`, `math`)
  - Python-like snippets and stdlib-oriented completion hints for a smoother editing experience
- **Tasks & commands**
  - Run / Build / Test / Check / Format
  - status bar run/check indicators
  - restart language server command

> **Note:** `a-lsp` is the authoritative source for semantic completions/signatures. The extension’s fallback completions are intentionally lightweight and curated so editing still feels productive when the server is unavailable.

## Quick start

1. **Build A and `a-lsp`** from the repository root:
   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
   cmake --build build -j
   ```
2. **Open an `.a` file** in VS Code with this extension loaded.
3. The extension will auto-discover:
   - compiler: `build/a` / workspace build outputs / `a` on PATH
   - server: `build/a-lsp` / workspace build outputs / `a-lsp` on PATH

### Install/package locally
```sh
cd tools/vscode-a
npx @vscode/vsce package --no-dependencies
code --install-extension a-language-*.vsix
```

Or open `tools/vscode-a` in VS Code and press `F5` to launch an Extension Development Host.

## Python-like editing experience (A-adapted)

This extension aims for a Python-like feel (familiar snippets, module completion, hover hints, and stdlib discoverability) while staying faithful to A’s syntax and semantics.

Examples of helpful completions/snippets:
- module imports: `import math`, `import json`, `import string`, `import textwrap`, `import statistics`, `import copy`
- stdlib calls: `string.capwords(...)`, `textwrap.fill(...)`, `statistics.mean(...)`, `json.parse(...)`
- common constructs: functions, structs, classes, enums, traits, loops, match, comprehensions, tests

## Settings

| Setting | Default | Description |
|---|---|---|
| `a.compilerPath` | `""` | Path/command for the `a` compiler |
| `a.entryFile` | `""` | Entry `.a` file for commands/tasks (empty = active file) |
| `a.stdlibPath` | `""` | Stdlib root passed to compiler / `a-lsp` |
| `a.modulePaths` | `[]` | Extra module search paths (`-I`) |
| `a.useLanguageServer` | `true` | Prefer `a-lsp` for live analysis |
| `a.checkOnSave` | `true` | Use compiler checks when fallback mode is active |
| `a.checkTimeout` | `30000` | Background check timeout (ms) |
| `a.jobs` | `0` | Parallel checking jobs (`0` = all cores, `1` = serial) |
| `a.useRunKeybindings` | `true` | Enable run keybindings for A files |
| `a.server.path` | `""` | Path to `a-lsp` (or `A_LSP_PATH`) |
| `a.server.args` | `[]` | Extra args for `a-lsp` |
| `a.server.modulePaths` | `[]` | Extra module paths for `a-lsp` |

## Commands

- **A: Run Program** (`a.run`)
- **A: Build Executable** (`a.build`)
- **A: Run Tests** (`a.test`)
- **A: Check Program** (`a.check`)
- **A: Format File** (`a.fmt`)
- **A: Show Compiler Output** (`a.showOutput`)
- **A: Restart Language Server** (`a.restartServer`)

## Tasks (`tasks.json`)

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "type": "a",
      "action": "run",
      "label": "Run Program",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": "$a"
    },
    {
      "type": "a",
      "action": "check",
      "label": "Check Program",
      "problemMatcher": "$a"
    }
  ]
}
```

Supported actions: `run`, `build`, `test`, `check`, `fmt`.

## Workspace trust

Untrusted workspaces allow safe editor features (highlighting/snippets/outline) but compiler execution requires trust.

## Maintaining better autocomplete

The extension has two layers:
1. **`a-lsp` semantic intelligence** (preferred) — improve completions/signatures in the compiler/LSP for best results.
2. **Extension fallback data/snippets** — lightweight curated support in:
   - `tools/vscode-a/stdlib-data.js` (module/member hints and hover summaries)
   - `tools/vscode-a/snippets/a.json` (snippets)
   - `tools/vscode-a/language.js` (lexical declarations/import scanning)

When new stdlib modules are added, update `stdlib-data.js` and snippets so fallback completions stay useful.
