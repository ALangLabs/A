# A Language — VS Code extension

Syntax highlighting, snippets, tasks (Run/Build/Test/Check/Format),
check-on-save, and the `a-lsp` language server client for the
[A programming language](../../README.md).

The extension uses only the standard VS Code Extension API (`vscode`
module) and spawns local processes (`a`, `a-lsp`) — there are no
Microsoft-Marketplace-only APIs or dependencies. It therefore runs in
VS Code and in forks/editors that consume [Open VSX](https://open-vsx.org)
(Cursor, Windsurf, VSCodium, code-server, Theia-based IDEs, …).

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
  sources (`CMakeLists.txt`, `compiler/astra/frontend/driver/version.hpp`,
  `tools/lsp/server.cpp`) — CI enforces this.

## Develop

Press **F5** in this folder (`.vscode/launch.json`) to launch an
Extension Development Host with the extension active. There are no
npm dependencies to install.
