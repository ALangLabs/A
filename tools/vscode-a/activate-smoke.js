'use strict';

// Activation smoke test for the A extension.
//
// Regression guard for the crash where registerProviders() called
// vscode.languages.registerSemanticTokensProvider, which does not exist in
// newer VS Code builds (replaced by registerDocumentSemanticTokensProvider /
// registerDocumentRangeSemanticTokensProvider). The thrown TypeError killed
// activate() entirely: no LSP, no hover, empty output channel.
//
// Modes (default "new"):
//   node activate-smoke.js            # stub without registerSemanticTokensProvider
//   node activate-smoke.js old        # stub with the legacy combined API
//   node activate-smoke.js both       # exercise both shapes
// Exits non-zero when activate() fails under either API shape.

const Module = require('module');
const origLoad = Module._load;
const disp = () => ({ dispose() {} });

function makeVscodeStub(mode) {
    const langImpl = {
        createDiagnosticCollection: () => ({ dispose() {}, clear() {}, set() {}, delete() {} }),
        registerDocumentSymbolProvider: disp, registerDocumentLinkProvider: disp,
        registerDefinitionProvider: disp, registerCompletionItemProvider: disp,
        registerHoverProvider: disp, registerDeclarationProvider: disp,
        registerTypeDefinitionProvider: disp, registerImplementationProvider: disp,
        registerReferenceProvider: disp, registerSignatureHelpProvider: disp,
        registerDocumentFormattingEditProvider: disp, registerRenameProvider: disp,
        registerDocumentHighlightProvider: disp, registerFoldingRangeProvider: disp,
        registerWorkspaceSymbolProvider: disp,
        registerInlayHintsProvider: disp,
    };
    if (mode === 'old') {
        langImpl.registerSemanticTokensProvider = disp;
    } else {
        langImpl.registerDocumentSemanticTokensProvider = disp;
        langImpl.registerDocumentRangeSemanticTokensProvider = disp;
    }
    return {
        languages: langImpl,
        workspace: {
            isTrusted: true, textDocuments: [], workspaceFolders: [],
            getConfiguration: () => ({ get: (k, d) => d }),
            createFileSystemWatcher: () => ({ onDidCreate: () => ({ dispose() {} }), onDidChange: () => ({ dispose() {} }), onDidDelete: () => ({ dispose() {} }), dispose() {} }),
            onDidOpenTextDocument: disp, onDidSaveTextDocument: disp, onDidChangeTextDocument: disp,
            onDidCloseTextDocument: disp, onDidChangeConfiguration: disp, onDidChangeWorkspaceFolders: disp,
            onDidGrantWorkspaceTrust: disp,
            fs: { readFile: async () => Buffer.from(''), stat: async () => { throw new Error('no'); } },
            findFiles: async () => [], openTextDocument: async () => { throw new Error('no'); },
        },
        window: {
            createOutputChannel: () => ({ appendLine() {}, show() {} }),
            createStatusBarItem: () => ({ show() {}, hide() {} }),
            activeTextEditor: undefined, onDidChangeActiveTextEditor: disp,
            showWarningMessage: async () => {}, showInformationMessage: async () => {},
        },
        commands: { registerCommand: () => ({ dispose() {} }) },
        tasks: { registerTaskProvider: () => ({ dispose() {} }) },
        MarkdownString: class { constructor(v) { this.value = v; } appendMarkdown(s) { this.value += s; } },
        Hover: class { constructor(c, r) { this.contents = c; this.range = r; } },
        Range: class { constructor(a, b, c, d) { this.start = { line: a, character: b }; this.end = { line: c, character: d }; } },
        Position: class { constructor(a, b) { this.line = a; this.character = b; } },
        Diagnostic: class {},
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        DiagnosticTag: { Unnecessary: 1, Deprecated: 2 },
        DiagnosticRelatedInformation: class {},
        Location: class {}, DocumentLink: class {}, DocumentSymbol: class {},
        SymbolKind: { Function: 11, Struct: 22, Class: 4, Enum: 9, Interface: 10, TypeParameter: 25, Variable: 12 },
        CompletionItem: class { constructor(l, k) { this.label = l; this.kind = k; } },
        CompletionItemKind: { Module: 8, Class: 6, Constant: 20, Function: 2, Keyword: 13 },
        CompletionList: class {}, SnippetString: class {}, SignatureInformation: class {},
        ParameterInformation: class {}, SignatureHelp: class {}, FoldingRange: class {},
        FoldingRangeKind: { Comment: 1, Imports: 2, Region: 3 }, SymbolInformation: class {},
        SemanticTokensLegend: class {}, SemanticTokensBuilder: class { push() {} build() { return {}; } },
        InlayHint: class {}, InlayHintKind: { Type: 1, Parameter: 2 }, WorkspaceEdit: class { set() {} },
        DocumentHighlight: class {},
        Uri: { file: (p) => ({ fsPath: p, toString: () => 'file://' + p }), parse: (s) => ({ toString: () => s, fsPath: '' }) },
        Task: class {}, TaskScope: { Workspace: 1 }, TaskGroup: { Build: 1 },
        TaskRevealKind: { Always: 1 }, TaskPanelKind: { Dedicated: 1 }, ProcessExecution: class {},
        Disposable: { from: (...d) => ({ dispose() { d.flat().forEach((x) => x && x.dispose && x.dispose()); } }) },
        StatusBarAlignment: { Left: 1, Right: 2 }, FileType: { File: 1 },
    };
}

// Spawns a fresh Node process per shape so module caches don't leak between.
// The child snippet lives in this same file (see bottom); we just re-exec.
const { spawnSync } = require('node:child_process');

function runChild(mode) {
    const result = spawnSync(process.execPath, [__filename, '--run', mode], {
        encoding: 'utf8',
        timeout: 45000,
    });
    if (result.status !== 0) {
        console.error(`FAIL (${mode}):\n${result.stdout || ''}\n${result.stderr || ''}`);
        return false;
    }
    console.log(result.stdout.trim());
    return true;
}

async function activateUnder(mode) {
    const vscodeStub = makeVscodeStub(mode);
    Module._load = function (request, parent, isMain) {
        if (request === 'vscode') return vscodeStub;
        return origLoad.apply(this, arguments);
    };
    const ext = require('./extension.js');
    const context = { subscriptions: [], extensionPath: __dirname };
    await ext.activate(context);
    return context;
}

if (process.argv[2] === '--run') {
    activateUnder(process.argv[3])
        .then(() => { console.log('OK (' + process.argv[3] + ')'); process.exit(0); })
        .catch((error) => { console.error(error.stack); process.exit(1); });
    setTimeout(() => process.exit(2), 30000);
} else {
    const arg = process.argv[2] || 'new';
    const modes = arg === 'both' ? ['new', 'old'] : [arg];
    let ok = true;
    for (const mode of modes) ok = runChild(mode) && ok;
    process.exit(ok ? 0 : 1);
}
