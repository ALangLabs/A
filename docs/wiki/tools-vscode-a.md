# vscode-a extension

`tools/vscode-a/` is the VS Code extension for A (`a-language`, publisher
`a-lang`, version kept in sync with the toolchain by CI).

## Files

- `package.json` — extension manifest: activation events (`onLanguage:a`,
  `a.run`/`a.build`/`a.test`/`a.check`/`a.fmt` commands, tasks, debug),
  configuration (`a.compilerPath`, `a.entryFile`, `a.stdlibPath`,
  `a.modulePaths`, `a.server.path`/`args`/`modulePaths`), restricted-mode
  capabilities, snippets/syntax contributions.
- `extension.js` — main activation: registers commands, tasks
  (Run/Build/Test/Check/Format), debug configuration, check-on-save fallback,
  output channel.
- `lsp-client.js` — spawns/connects `a-lsp` over stdio and bridges LSP.
- `language.js`, `config.js`, `diagnostics.js`, `stdlib-data.js` — language
  basics, settings, diagnostic plumbing, stdlib symbol data.
- `syntaxes/a.tmLanguage.json`, `syntaxes/a.markdown.json`,
  `language-configuration.json`, `snippets/a.json` — TextMate grammar,
  bracket/comment config, snippets.
- `activate-smoke.js` — extension-host smoke check; `a-language-0.0.1.vsix` —
  packaged artifact (build output, regenerated on release).

## How it fits

The extension locates the `a` binary (`a.compilerPath`) and `a-lsp`
(`a.server.path`), forwards `modulePaths`/`stdlibPath` to both, and provides
graceful degradation (syntax highlighting/snippets/outline work in Restricted
Mode; compiling requires trust). Related: [a-lsp](tools-lsp.md),
[overview](overview.md).
