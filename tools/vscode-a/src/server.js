'use strict';

// a-lsp lifecycle: decides whether the language server should run (trusted
// workspace + a.useLanguageServer + a runnable binary), spawns it through
// LanguageClient, mirrors document open/change/save/close events, and
// exposes getClient() used by every LSP-backed provider. Falls back to the
// lexical/check-on-save controller when the server cannot start.

const vscode = require('vscode');
const path = require('node:path');
const { LanguageClient } = require('../lsp-client');
const {
    absolute, compilerPath, serverPath, isExecutable, modulePaths,
} = require('../config');
const { isA } = require('./util');

// env: { output, context, fallback: {start,stop,active}, updateCheckStatus(), isDisposed() }
function createServerController(env) {
    const { output, context } = env;
    const server = { status: 'off', client: undefined, setup: undefined };
    const openedDocuments = new Set();
    const changeTimers = new Map();

    const getClient = () => server.status === 'running' ? server.client : undefined;

    function serverSetup() {
        const folders = vscode.workspace.workspaceFolders || [];
        const first = folders[0];
        const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        const doc = active && isA(active) ? active : vscode.workspace.textDocuments.find(isA);
        const cwd = first ? first.uri.fsPath : doc ? path.dirname(doc.uri.fsPath) : process.cwd();
        const config = vscode.workspace.getConfiguration('a', first && first.uri ? first.uri : doc && doc.uri);
        const configured = config.get('server.path', '') || process.env.A_LSP_PATH || '';
        const stdlibRaw = config.get('stdlibPath', '');
        const compiler = compilerPath(config.get('compilerPath', ''), cwd);
        const beside = compiler && /[\\/]/.test(compiler) ? path.join(path.dirname(compiler), process.platform === 'win32' ? 'a-lsp.exe' : 'a-lsp') : '';
        const jobsRaw = Number(config.get('jobs', 0));
        return {
            enabled: config.get('useLanguageServer', true),
            path: serverPath(configured, cwd, context.extensionPath) || (beside && isExecutable(beside) ? beside : ''),
            args: Array.isArray(config.get('server.args', [])) ? config.get('server.args', []) : [],
            modulePaths: modulePaths(config, cwd, ''),
            stdlib: stdlibRaw ? absolute(stdlibRaw, cwd, '') : '',
            jobs: Number.isFinite(jobsRaw) && jobsRaw >= 0 ? Math.trunc(jobsRaw) : 0,
        };
    }

    async function stopServer() {
        const client = server.client;
        server.status = 'off'; server.client = undefined;
        openedDocuments.clear();
        for (const timer of changeTimers.values()) clearTimeout(timer);
        changeTimers.clear();
        if (client) { try { await client.stop(); } catch {} }
        env.updateCheckStatus();
    }

    async function startServer(setup) {
        await stopServer(); if (env.isDisposed()) return;
        const client = new LanguageClient(setup.path, setup.args, { modulePaths: setup.modulePaths, stdlibRoot: setup.stdlib, jobs: setup.jobs }, output);
        client.onCheckingChanged = () => env.updateCheckStatus();
        server.status = 'starting'; server.client = client; server.setup = setup; env.updateCheckStatus();
        output.appendLine(`> ${setup.path} ${(setup.args || []).map(arg => JSON.stringify(arg)).join(' ')}  (language server)`);
        try {
            await client.start();
            if (server.client !== client) return;
            server.status = 'running';
            for (const document of vscode.workspace.textDocuments) {
                if (isA(document)) { const key = document.uri.toString(); openedDocuments.add(key); client.openDocument(document); }
            }
        } catch (error) {
            if (server.client !== client) return;
            output.appendLine(`The A language server did not start: ${error.message}`);
            server.status = 'off'; server.client = undefined;
            try { client.stop(); } catch {}
            void vscode.window.showWarningMessage(`The A language server did not start (${error.message}); using fallback analysis. See the A output.`);
            env.fallback.start();
        }
        env.updateCheckStatus();
    }

    let deciding = Promise.resolve();
    function decide(restart = false) {
        deciding = deciding.then(async () => {
            if (env.isDisposed()) return;
            const setup = serverSetup();
            const runnable = setup.path && (isExecutable(setup.path) || !/[\\/]/.test(setup.path));
            const wanted = vscode.workspace.isTrusted && setup.enabled && runnable;
            if (!wanted) { await stopServer(); env.fallback.start(); return; }
            if (!restart && server.client && JSON.stringify(server.setup) === JSON.stringify(setup)) return;
            env.fallback.stop(); await startServer(setup);
        }).catch(error => output.appendLine(`Language server: ${error.message}`));
        return deciding;
    }

    // Mirror document lifecycle into the server; incremental changes are
    // debounced 250 ms so a-lsp's own 200 ms compile debounce sees settled text.
    function attachDocumentSync() {
        context.subscriptions.push(
            vscode.workspace.onDidOpenTextDocument(doc => {
                if (isA(doc) && !(vscode.workspace.workspaceFolders || []).length && server.status === 'off') void decide();
                const active = getClient(); if (!active || !isA(doc)) return;
                openedDocuments.add(doc.uri.toString()); active.openDocument(doc);
            }),
            vscode.workspace.onDidChangeTextDocument(event => {
                const active = getClient(); if (!active || !isA(event.document) || !event.contentChanges.length) return;
                const key = event.document.uri.toString();
                if (!openedDocuments.has(key)) { openedDocuments.add(key); active.openDocument(event.document); return; }
                const existing = changeTimers.get(key); if (existing) clearTimeout(existing);
                const captured = event.contentChanges.length === 1 ? { range: event.contentChanges[0].range, text: event.contentChanges[0].text } : undefined;
                changeTimers.set(key, setTimeout(() => {
                    changeTimers.delete(key);
                    const c = getClient();
                    if (c && openedDocuments.has(key)) {
                        if (captured && captured.range) c.changeDocument(event.document, captured.range, captured.text);
                        else c.changeDocument(event.document);
                    }
                }, 250));
            }),
            vscode.workspace.onDidSaveTextDocument(document => {
                const active = getClient(); if (!active || !isA(document)) return;
                const key = document.uri.toString(); const timer = changeTimers.get(key);
                if (timer) { clearTimeout(timer); changeTimers.delete(key); }
                if (!openedDocuments.has(key)) { openedDocuments.add(key); active.openDocument(document); }
                active.saveDocument(document);
            }),
            vscode.workspace.onDidCloseTextDocument(document => {
                const active = getClient(); if (!isA(document)) return;
                const key = document.uri.toString(); const timer = changeTimers.get(key);
                if (timer) { clearTimeout(timer); changeTimers.delete(key); }
                if (active && openedDocuments.delete(key)) active.closeDocument(document);
            })
        );
    }

    return { server, decide, getClient, attachDocumentSync, stopServer };
}

module.exports = { createServerController };
