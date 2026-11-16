'use strict';

// VS Code extension host for the A programming language. It prefers the
// a-lsp language server (diagnostics as you type, hover, go to definition,
// symbols, formatting, signature help, hierarchies, lenses, code actions)
// and falls back to check-on-save plus lexical features when the server is
// missing, disabled, or the workspace is untrusted. Tasks always run the
// `a` compiler directly. No dependencies: everything lives in plain Node
// modules under src/ beside this file.
//
// Layout:
//   src/util.js           shared helpers + LSP<->VS Code converters
//   src/check-queue.js    check-on-save queue + `a check --json` runner
//   src/fallback.js       lexical providers + fallback check wiring
//   src/lsp-providers.js  every provider backed by a-lsp
//   src/server.js         a-lsp spawn/decide/document sync
//   src/commands.js       palette commands + "A" run terminal
//   src/tasks.js          `a` task provider + starter tasks.json
//   src/codelens.js       Run/Run Tests code lenses
//   src/status.js         status bar items

const vscode = require('vscode');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { settings, compilerPath, serverPath, isExecutable } = require('./config');
const { isA } = require('./src/util');
const { CheckQueue } = require('./src/check-queue');
const { createFallbackController } = require('./src/fallback');
const { createServerController } = require('./src/server');
const { registerProviders } = require('./src/lsp-providers');
const { registerCommands } = require('./src/commands');
const { registerTaskProvider, generateStarterTasks } = require('./src/tasks');
const { registerRunTestCodeLens } = require('./src/codelens');
const { registerTestController } = require('./src/tests');
const { createStatusBars } = require('./src/status');

async function activate(context) {
    let disposed = false;
    const output = vscode.window.createOutputChannel('A');
    const checkDiagnostics = vscode.languages.createDiagnosticCollection('a-check');
    const queue = new CheckQueue(output, checkDiagnostics);

    function recheck() {
        for (const doc of vscode.workspace.textDocuments) {
            if (isA(doc) && !doc.isDirty) queue.schedule(settings(vscode, doc.uri), 0);
        }
    }

    // Status bars first: queue/server state changes drive them.
    const bars = createStatusBars(context, {
        getServerStatus: () => (server ? server.server.status : 'off'),
        queue,
    });
    queue.onStateChanged = bars.updateCheckStatus;

    const env = {
        output,
        queue,
        context,
        isDisposed: () => disposed,
        isTrusted: () => vscode.workspace.isTrusted,
        updateCheckStatus: bars.updateCheckStatus,
        updateRunStatus: bars.updateRunStatus,
        decide: (restart) => server ? server.decide(restart) : Promise.resolve(),
        fallback: undefined, // filled below
        server: undefined,   // filled below
    };

    const fallback = createFallbackController({
        output, queue, checkDiagnostics,
        recheck,
        updateCheckStatus: bars.updateCheckStatus,
        isDisposed: () => disposed,
    });
    env.fallback = fallback;

    const server = createServerController({
        output, context, fallback,
        updateCheckStatus: bars.updateCheckStatus,
        isDisposed: () => disposed,
    });
    env.server = server.server;

    context.subscriptions.push(
        checkDiagnostics, output,
        { dispose() { disposed = true; fallback.stop(); queue.invalidate(); return server.stopServer(); } },
        // LSP-backed providers (no-op until a-lsp is running).
        ...registerProviders(server.getClient),
    );
    registerRunTestCodeLens(context);

    registerTaskProvider(context, env);
    registerCommands(context, env);
    registerTestController(context, env);
    server.attachDocumentSync();

    // a.formatOnSave: run whichever formatting provider is active (a-lsp, or
    // the `a fmt` fallback) before the document is written to disk.
    context.subscriptions.push(
        vscode.workspace.onWillSaveTextDocument(async event => {
            if (!isA(event.document)) return;
            try {
                const config = vscode.workspace.getConfiguration('a', event.document.uri);
                if (!config.get('formatOnSave', false)) return;
                const edits = await vscode.commands.executeCommand(
                    'vscode.executeDocumentFormattingProvider', event.document.uri);
                if (Array.isArray(edits) && edits.length) {
                    const we = new vscode.WorkspaceEdit();
                    we.set(event.document.uri, edits);
                    await vscode.workspace.applyEdit(we);
                }
            } catch { /* formatting is best-effort; never block a save */ }
        })
    );

    // One-time toolchain hints (Python-extension style): only when the
    // workspace actually has A files, warn if `a` cannot run or `a-lsp`
    // cannot be found, with shortcuts to the relevant settings.
    let toolchainChecked = false;
    async function checkToolchain() {
        if (toolchainChecked || disposed) return;
        try {
            const hasA = vscode.workspace.textDocuments.some(isA) ||
                (await vscode.workspace.findFiles('**/*.astra',
                    '{**/node_modules/**,**/.git/**,**/build/**,**/build-*/**}', 1)).length > 0;
            if (!hasA || disposed) return;
            toolchainChecked = true;
            const folder = (vscode.workspace.workspaceFolders || [])[0];
            const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
            const uri = folder ? folder.uri : doc && doc.uri;
            const config = vscode.workspace.getConfiguration('a', uri);
            const cwd = folder ? folder.uri.fsPath
                : uri && uri.fsPath ? path.dirname(uri.fsPath) : process.cwd();
            const openSettings = key => vscode.commands.executeCommand(
                'workbench.action.openSettings', `a.${key}`);

            const compiler = compilerPath(config.get('compilerPath', ''), cwd);
            const ver = spawnSync(compiler, ['version'],
                { timeout: 10000, windowsHide: true, encoding: 'utf8' });
            if (ver.error || ver.status !== 0) {
                const pick = await vscode.window.showWarningMessage(
                    `The A compiler '${compiler}' did not run` +
                    `${ver.error ? ` (${ver.error.message})` : ` (exit ${ver.status})`}. ` +
                    'Install the A toolchain or set "a.compilerPath".',
                    'Open Settings', 'Show Output');
                if (pick === 'Open Settings') void openSettings('compilerPath');
                else if (pick === 'Show Output') output.show(true);
                return;
            }

            if (vscode.workspace.isTrusted && config.get('useLanguageServer', true)
                && server.server.status === 'off') {
                const configured = config.get('server.path', '') || process.env.A_LSP_PATH || '';
                const lsp = serverPath(configured, cwd, context.extensionPath);
                const beside = /[\\/]/.test(compiler)
                    ? path.join(path.dirname(compiler), process.platform === 'win32' ? 'a-lsp.exe' : 'a-lsp')
                    : '';
                if (!lsp && !(beside && isExecutable(beside))) {
                    const pick = await vscode.window.showInformationMessage(
                        'a-lsp was not found; using check-on-save with fallback completions. ' +
                        'Build a-lsp or set "a.server.path".',
                        'Open Settings', 'Show Output');
                    if (pick === 'Open Settings') void openSettings('server.path');
                    else if (pick === 'Show Output') output.show(true);
                }
            }
        } catch { /* hints are best-effort */ }
    }

    // Workspace/config changes re-decide server vs fallback and re-check.
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(event => {
            if (!event.affectsConfiguration('a')) return;
            if (fallback.active) { queue.invalidate(); recheck(); }
            void server.decide();
        }),
        vscode.workspace.onDidChangeWorkspaceFolders(() => {
            if (fallback.active) { queue.invalidate(); recheck(); }
            void server.decide();
            void generateStarterTasks(context);
        }),
        vscode.window.onDidChangeActiveTextEditor(() => { bars.updateRunStatus(); bars.updateCheckStatus(); void checkToolchain(); }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => { bars.updateRunStatus(); bars.updateCheckStatus(); void server.decide(); })
    );

    bars.updateRunStatus(); bars.updateCheckStatus();
    void generateStarterTasks(context);
    await server.decide();
    void checkToolchain();
}

let stopOnDeactivate = async () => {};

function deactivate() { return stopOnDeactivate ? stopOnDeactivate() : Promise.resolve(); }

// assign after activate defines the real teardown path via context dispose;
// kept for symmetry with the previous single-file layout.
const _activate = activate;
module.exports = {
    activate: async function (context) {
        const subs = context.subscriptions;
        await _activate(context);
        // The dispose subscription pushed inside activate performs teardown.
        stopOnDeactivate = async () => {
            for (const sub of [...subs].reverse()) {
                try { const r = sub.dispose && sub.dispose(); if (r && r.then) await r; } catch {}
            }
        };
    },
    deactivate,
};
