'use strict';

// Palette commands (a.run/a.build/a.test/a.check/a.fmt/a.showOutput/
// a.restart*), the saved-document gate they share, the "A" terminal used
// for `a run`, and task execution for the non-run actions.

const vscode = require('vscode');
const { settings, argumentsFor, compilerPath } = require('../config');
const { isA, cwdOf, shellQuote } = require('./util');
const { makeTask } = require('./tasks');

// Shared compiler flags that apply to every subcommand: stdlib override and
// module search paths. argumentsFor() inlines the same logic for tasks.
function optionArgs(config) {
    const args = [];
    if (config.stdlib) args.push('--stdlib', config.stdlib);
    for (const dir of config.modulePaths) args.push('-I', dir);
    return args;
}

// Config for documents without a file path (untitled buffers): `a -c` and
// `a repl` need no entry file, only the compiler and search paths.
function looseConfig() {
    const folder = (vscode.workspace.workspaceFolders || [])[0];
    const cwd = folder ? folder.uri.fsPath : process.cwd();
    return { compiler: compilerPath('', cwd), stdlib: '', modulePaths: [], cwd };
}

function configFor(doc) {
    return doc.uri.scheme === 'file' ? settings(vscode, doc.uri) : looseConfig();
}

// env: { output, queue, server, decide, isTrusted() }
//   server: { status, client } -- status 'off'|'starting'|'running'
function registerCommands(context, env) {
    const { output, queue } = env;

    async function savedDocument(uri) {
        const pick = async () => uri ? vscode.workspace.openTextDocument(uri).then(doc => doc, () => undefined) : vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        const doc = await pick(); if (!doc || !isA(doc)) return undefined;
        if (!vscode.workspace.isTrusted) { void vscode.window.showWarningMessage('Trust this workspace to run the A compiler.'); return undefined; }
        if (doc.isUntitled && !await doc.save()) return undefined;
        const active = await pick(); if (!active || !isA(active)) return undefined;
        const cwd = cwdOf(active);
        for (const open of vscode.workspace.textDocuments) { if (isA(open) && open.isDirty && cwdOf(open) === cwd && !await open.save()) return undefined; }
        return active;
    }

    // Terminal-based run (Python-style): the "Run" button opens/reuses a
    // terminal named "A" and sends "<compiler> run <file>" to it, so program
    // output appears in the terminal instead of a task panel.
    let runTerminal = undefined;
    function runInTerminal(doc) {
        const config = settings(vscode, doc.uri);
        const command = [config.compiler, ...argumentsFor('run', config)].map(shellQuote).join(' ');
        if (!runTerminal || runTerminal.exitStatus !== undefined) {
            runTerminal = vscode.window.createTerminal({ name: 'A', cwd: config.cwd });
        }
        runTerminal.show();
        runTerminal.sendText(command + '\n');
    }

    // "A REPL" terminal running `a repl` (Python-extension style REPL).
    let replTerminal = undefined;
    function replInTerminal(doc) {
        const config = configFor(doc);
        const command = [config.compiler, 'repl', ...optionArgs(config)].map(shellQuote).join(' ');
        if (!replTerminal || replTerminal.exitStatus !== undefined) {
            replTerminal = vscode.window.createTerminal({ name: 'A REPL', cwd: config.cwd });
        }
        replTerminal.show();
        replTerminal.sendText(command + '\n');
    }

    // `a -c <code>` runs inline source without touching a file, so unsaved
    // selections work. Empty selection falls back to the cursor's line.
    function runSelectionInTerminal(editor) {
        const sel = editor.selection;
        const text = sel.isEmpty
            ? editor.document.lineAt(sel.active.line).text
            : editor.document.getText(sel);
        if (!text.trim()) {
            void vscode.window.showWarningMessage('Select some A code or place the cursor on a line to run.');
            return;
        }
        const config = configFor(editor.document);
        const command = [config.compiler, ...optionArgs(config), '-c', text].map(shellQuote).join(' ');
        if (!runTerminal || runTerminal.exitStatus !== undefined) {
            runTerminal = vscode.window.createTerminal({ name: 'A', cwd: config.cwd });
        }
        runTerminal.show();
        runTerminal.sendText(command + '\n');
    }

    // uri may be a string (CodeLens passes document.uri.toString()) or a
    // vscode.Uri; commands without one act on the active A document.
    function targetUri(target) {
        if (typeof target === 'string') { try { return vscode.Uri.parse(target); } catch { return undefined; } }
        return target instanceof vscode.Uri ? target : undefined;
    }

    context.subscriptions.push(
        vscode.commands.registerCommand('a.showOutput', () => output.show()),
        vscode.commands.registerCommand('a.restartServer', async () => { await env.decide(true); vscode.window.showInformationMessage('A language server restarted.'); }),
        vscode.commands.registerCommand('a.restart', async () => { await env.decide(true); vscode.window.showInformationMessage('A language server restarted.'); }),
        vscode.commands.registerCommand('a.check', async target => {
            const doc = await savedDocument(targetUri(target)); if (!doc) return;
            if (env.server.status !== 'off') { await vscode.tasks.executeTask(makeTask(env, { type: 'a', action: 'check' }, doc.uri)); return; }
            const completed = await queue.schedule(settings(vscode, doc.uri), 0); if (completed) output.show(true);
        }),
        vscode.commands.registerCommand('a.run', async target => {
            const doc = await savedDocument(targetUri(target)); if (!doc) return;
            runInTerminal(doc);
        }),
        vscode.commands.registerCommand('a.repl', async () => {
            const editor = vscode.window.activeTextEditor;
            const doc = editor && isA(editor.document) ? editor.document
                : vscode.workspace.textDocuments.find(d => isA(d));
            if (!doc) { void vscode.window.showWarningMessage('Open an A file to start the REPL.'); return; }
            if (!vscode.workspace.isTrusted) { void vscode.window.showWarningMessage('Trust this workspace to run the A compiler.'); return; }
            replInTerminal(doc);
        }),
        vscode.commands.registerCommand('a.runSelection', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || !isA(editor.document)) return;
            if (!vscode.workspace.isTrusted) { void vscode.window.showWarningMessage('Trust this workspace to run the A compiler.'); return; }
            runSelectionInTerminal(editor);
        }),
        // Optional second arg: a test-name filter (`a test --filter`), used
        // by the per-test CodeLens lenses; ignored for other actions.
        ...['build', 'test', 'fmt'].map(action => vscode.commands.registerCommand(`a.${action}`, async (target, filter) => {
            const doc = await savedDocument(targetUri(target)); if (!doc) return;
            await vscode.tasks.executeTask(makeTask(env, { type: 'a', action, filter }, doc.uri));
        })),
        { dispose() { try { if (runTerminal) runTerminal.dispose(); } catch {} try { if (replTerminal) replTerminal.dispose(); } catch {} } }
    );
}

module.exports = { registerCommands };
