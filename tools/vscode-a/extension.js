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
const { settings } = require('./config');
const { isA } = require('./src/util');
const { CheckQueue } = require('./src/check-queue');
const { createFallbackController } = require('./src/fallback');
const { createServerController } = require('./src/server');
const { registerProviders } = require('./src/lsp-providers');
const { registerCommands } = require('./src/commands');
const { registerTaskProvider, generateStarterTasks } = require('./src/tasks');
const { registerRunTestCodeLens } = require('./src/codelens');
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
    server.attachDocumentSync();

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
        vscode.window.onDidChangeActiveTextEditor(() => { bars.updateRunStatus(); bars.updateCheckStatus(); }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => { bars.updateRunStatus(); bars.updateCheckStatus(); void server.decide(); })
    );

    bars.updateRunStatus(); bars.updateCheckStatus();
    void generateStarterTasks(context);
    await server.decide();
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
