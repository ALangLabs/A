# A Language for VS Code

Syntax highlighting, language intelligence, and compiler tasks for the
[A programming language](../../README.md).

## Features

- TextMate syntax highlighting for `.a` files (Python-grade, then the same
  on top of it with LSP semantic tokens): keywords (`fn`/`struct`/`trait`/
  `impl`/`comptime`/control/import), declarations with parameters and generic
  type arguments (`entity.name.*`, `variable.parameter`, `support.type.*`),
  primitive + generic types (`Option<T>`, `Result<T, E>`, `Async<T>`, …),
  both call sites and builtins (`support.function.*`, `meta.function-call`),
  numbers (hex/binary/octal/float, underscores), strings (normal, raw
  `"""…"""` docstrings, `{expr}` interpolation), chars, `#`/`##` doc
  comments (with TODO markers), operators (`??`, `?.`, `..`, `|>`, `->`,
  `=>`), members, and punctuation. ` ```a ` fenced blocks in Markdown get
  highlighting too. LSP semantic tokens and inlay hints refine this further
  (types, members, inferred `: i32` on `x = 1`), like Pyright/Pyrefly.
- Snippets for functions, structs, classes, enums, traits, imports, control
  flow, match, comprehensions, defer, tests, and asserts.
- Two analysis modes, chosen automatically:
  - **Language server** — when `a-lsp` is found and the workspace is trusted,
    the server reports diagnostics as you type and serves hover, go to
    definition, find references, document/workspace symbols, completion,
    signature help, formatting, rename, document highlights, and folding.
  - **Check on save** — otherwise (no `a-lsp`, `a.useLanguageServer` off, or
    an untrusted workspace) saved files are checked with `a check --json` and
    the results appear in Problems, plus a lexical outline and clickable
    import links.
- Status bar items: a check indicator (errors, stale results, failures) and a
  `$(play) Run` button.
- Tasks for every compiler verb: **Run Program** (the default build task),
  **Build Executable**, **Run Tests**, **Check Program**, and **Format File**,
  with problem matching for `error:`/`warning:` + `--> file:line:col` output.
- Commands and keybindings: `Ctrl+Shift+B` / `Cmd+Shift+B`, `F5`, and
  `Ctrl+F5` run the program (toggle with `a.useRunKeybindings`); the editor
  title gets a play button; **A: Show Compiler Output** opens the `A` output
  channel.
- Debug picker integration: **A: Run Program** appears in the Run/Debug
  configuration picker and launches the run task (no debugger).
- `a.entryFile` support: point commands and tasks at an entry file while
  editing imported modules.

The extension is dependency-free (no `npm install` required): the LSP client
is hand-rolled over stdio and tasks spawn the `a` compiler directly.

## Quick start

1. Build the toolchain from the repository root:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ```

2. Package and install the extension:

   ```sh
   cd tools/vscode-a
   npx @vscode/vsce package --no-dependencies
   code --install-extension a-language-0.0.1.vsix
   ```

   Or open this folder in VS Code and press `F5` for an Extension Development
   Host.

3. Open any `.a` file. Syntax highlighting works immediately; `build/a` and
   `build/a-lsp` are found automatically.

## Finding the tools

**Compiler** (`a.compilerPath`, used by tasks and check-on-save):

1. `a.compilerPath` (settings).
2. `a` / `build/Release/a` / `build/Debug/a` / `build/a` inside the workspace
   folder.
3. `a` on `PATH`.

**Language server** (`a.server.path`):

1. `a.server.path` (settings) or `A_LSP_PATH` in the environment.
2. `build/a-lsp` and `build*/a-lsp` inside each workspace folder.
3. `build/a-lsp` at the root of this development checkout.
4. `a-lsp` on `PATH`.

If the server is missing or fails to start, the extension falls back to
check-on-save automatically and says so in the output channel.

## Settings

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `a.compilerPath` | `""` | `a` executable or command on PATH. |
| `a.entryFile` | `""` | Entry file for commands/tasks; empty uses the active file. |
| `a.stdlibPath` | `""` | Standard-library directory (`--stdlib` / `stdlibRoot`). |
| `a.modulePaths` | `[]` | Import search directories (`-I` for the compiler, module paths for `a-lsp`). |
| `a.useLanguageServer` | `true` | Start `a-lsp` for live analysis. |
| `a.checkOnSave` | `true` | Check saved files when the server is off. |
| `a.checkTimeout` | `30000` | Milliseconds before a background check is killed. |
| `a.useRunKeybindings` | `true` | Run on `Ctrl+Shift+B`, `F5`, `Ctrl+F5`. |
| `a.server.path` | `""` | Path to the `a-lsp` executable. |
| `a.server.args` | `[]` | Extra arguments for `a-lsp`. |
| `a.server.modulePaths` | `[]` | Extra import directories for `a-lsp` (merged with `a.modulePaths`). |

## Commands

- **A: Run Program** — compile and run (also `Ctrl+Shift+B`, `F5`, editor
  title play button, Tasks: Run Task).
- **A: Build Executable** — `a build`, output beside the source.
- **A: Run Tests** — `a test`, runs every `fn test_*()`.
- **A: Check Program** — `a check`; with the server running it shows the full
  compiler output in a terminal, otherwise it queues a background check.
- **A: Format File** — `a fmt -w`.
- **A: Show Compiler Output** — reveals the `A` output channel.
- **A: Restart Language Server** — re-probes `a-lsp` and restarts or falls
  back to check-on-save.

## tasks.json

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
      "action": "build",
      "label": "Build Executable",
      "problemMatcher": "$a"
    }
  ]
}
```

`action` is one of `run`, `build`, `test`, `check`, `fmt`; `file` overrides the
source (default: `a.entryFile` or the active file) and `output` overrides the
build destination.

## Notes

- The server uses full-document synchronization. The extension debounces
  edits by 250 ms before sending them.
- In check-on-save mode, results are keyed by entry file: editing an imported
  module marks its entry's results stale until the next save.
- Untrusted workspaces get highlighting, snippets, and the outline only;
  compiler tasks require trust.
