'use strict';

// Test Explorer integration via the VS Code Testing API: discovers top-level
// `fn test_*()` declarations in workspace .astra files and runs them through
// `a test` -- the whole suite, one file, or a single test (--filter). The
// harness prints `ok <name>` / `FAILED <name>: <msg>` per test, which is
// parsed back into per-item pass/fail results in the Testing view.

const vscode = require('vscode');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { settings, argumentsFor } = require('../config');
const { isA, failureMessage } = require('./util');

const TEST_ID = 'a-test:';
const FILE_ID = 'a-file:';
const FOLDER_ID = 'a-folder:';
const EXCLUDES = '{**/node_modules/**,**/.git/**,**/build/**,**/build-*/**,**/.apm/**,**/third_party/**}';
const TEST_FN = /^[ \t]*fn[ \t]+(test_\w+)/gm;

function testNamesIn(text) {
    const names = [];
    for (const m of String(text).matchAll(TEST_FN)) names.push(m[1]);
    return names;
}

// env: { output } -- the "A" output channel, used for the spawned commands.
function registerTestController(context, env) {
    const testsApi = vscode.tests;
    if (!testsApi || typeof testsApi.createTestController !== 'function') return;
    const controller = testsApi.createTestController('a', 'A');
    context.subscriptions.push(controller);

    const folderItems = new Map(); // folder uri string -> TestItem
    const fileItems = new Map();   // file uri string -> { item, parent, folder, names }
    const syncTimers = new Map();

    function folderItem(folder) {
        const key = folder.uri.toString();
        let item = folderItems.get(key);
        if (!item) {
            item = controller.createTestItem(FOLDER_ID + key, folder.name, folder.uri);
            folderItems.set(key, item);
        }
        return item;
    }

    async function textOf(uri) {
        const key = uri.toString();
        const open = vscode.workspace.textDocuments.find(d => d.uri.toString() === key);
        if (open) return open.getText();
        return Buffer.from(await vscode.workspace.fs.readFile(uri)).toString('utf8');
    }

    function removeFile(key) {
        const rec = fileItems.get(key);
        if (!rec) return;
        try { rec.parent.children.delete(rec.item.id); } catch {}
        fileItems.delete(key);
    }

    // Create/update the file suite item and its per-test children.
    async function syncFile(uri, folder) {
        const key = uri.toString();
        let names = [];
        try { names = testNamesIn(await textOf(uri)); } catch { /* unreadable: drop */ }
        if (!names.length) { removeFile(key); return; }
        let rec = fileItems.get(key);
        if (!rec) {
            const parent = folderItem(folder);
            const item = controller.createTestItem(FILE_ID + key, path.basename(uri.fsPath), uri);
            parent.children.add(item);
            rec = { item, parent, folder: folder.uri.toString(), names: new Map() };
            fileItems.set(key, rec);
        }
        const seen = new Set();
        for (const name of names) {
            seen.add(name);
            if (!rec.names.has(name)) {
                const ti = controller.createTestItem(TEST_ID + key + '::' + name, name, uri);
                rec.item.children.add(ti);
                rec.names.set(name, ti);
            }
        }
        for (const [name, ti] of rec.names) {
            if (!seen.has(name)) {
                try { rec.item.children.delete(ti.id); } catch {}
                rec.names.delete(name);
            }
        }
    }

    async function discover(token) {
        const cancelled = () => token && token.isCancellationRequested;
        for (const folder of vscode.workspace.workspaceFolders || []) {
            if (cancelled()) return;
            let files = [];
            try {
                files = await vscode.workspace.findFiles(
                    new vscode.RelativePattern(folder, '**/*.astra'), EXCLUDES);
            } catch { continue; }
            const found = new Set(files.map(f => f.toString()));
            for (const uri of files) {
                if (cancelled()) return;
                await syncFile(uri, folder);
            }
            // Drop file items for .astra files that no longer exist.
            for (const [key, rec] of [...fileItems]) {
                if (rec.folder === folder.uri.toString() && !found.has(key)) removeFile(key);
            }
        }
    }

    // Debounced per-file rescan when an open document changes.
    function scheduleSync(uri) {
        const key = uri.toString();
        const existing = syncTimers.get(key);
        if (existing) clearTimeout(existing);
        syncTimers.set(key, setTimeout(() => {
            syncTimers.delete(key);
            const folder = vscode.workspace.getWorkspaceFolder(uri);
            if (folder) void syncFile(uri, folder);
        }, 300));
    }

    function collectTargets(items, out) {
        for (const item of items || []) {
            if (!item) continue;
            if (typeof item.id === 'string' && item.id.startsWith(TEST_ID)) { out.push(item); continue; }
            if (item.children && item.children.size) collectTargets([...item.children], out);
        }
    }

    function allTestItems() {
        const out = [];
        for (const rec of fileItems.values()) for (const ti of rec.names.values()) out.push(ti);
        return out;
    }

    async function runFile(group, run, token, setChild) {
        const uri = group.uri;
        const tests = group.tests; // Map<name, TestItem>
        for (const item of tests.values()) run.enqueued(item);
        const config = settings(vscode, uri);
        const names = [...tests.keys()];
        // A single requested test maps to `a test --filter <name>`; several
        // tests in one file run the whole file (the filter is a substring
        // match, so it cannot select exactly one of several anyway).
        const filter = names.length === 1 ? names[0] : '';
        const args = argumentsFor('test', { ...config, file: uri.fsPath }, filter ? { filter } : {});
        env.output.appendLine(`> ${config.compiler} ${args.map(a => JSON.stringify(a)).join(' ')}`);
        const result = await new Promise(resolve => {
            const cp = spawn(config.compiler, args, { cwd: config.cwd, windowsHide: true });
            setChild(cp);
            let out = '', err = '';
            cp.stdout.on('data', d => { const s = d.toString('utf8'); out += s; run.appendOutput(s); });
            cp.stderr.on('data', d => { const s = d.toString('utf8'); err += s; run.appendOutput(s); });
            const timer = setTimeout(() => { try { cp.kill(); } catch {} }, 10 * 60 * 1000);
            const onCancel = token.onCancellationRequested(() => { try { cp.kill(); } catch {} });
            cp.on('error', e => {
                clearTimeout(timer); onCancel.dispose(); setChild(null);
                resolve({ code: -1, out, err, error: e.message });
            });
            cp.on('close', code => {
                clearTimeout(timer); onCancel.dispose(); setChild(null);
                resolve({ code, out, err });
            });
        });
        const byName = new Map();
        for (const line of result.out.split(/\r?\n/)) {
            let m = /^ok\s+(\S+)\s*$/.exec(line);
            if (m) { byName.set(m[1], { ok: true }); continue; }
            m = /^FAILED\s+(\S+?)\s*:\s*(.*)$/.exec(line);
            if (m) byName.set(m[1], { ok: false, message: m[2] });
        }
        for (const [name, item] of tests) {
            const r = byName.get(name);
            if (r && r.ok) { run.passed(item); continue; }
            if (r) { run.failed(item, undefined, r.message || 'test failed'); continue; }
            if (token.isCancellationRequested) { run.skipped(item, 'cancelled'); continue; }
            if (result.code === 0) { run.skipped(item, 'no result reported'); continue; }
            run.failed(item, undefined,
                failureMessage(result.err || result.out || result.error || `exit code ${result.code}`));
        }
    }

    async function runRequest(request, token) {
        if (!vscode.workspace.isTrusted) {
            void vscode.window.showWarningMessage('Trust this workspace to run A tests.');
            return;
        }
        const run = controller.createTestRun(request);
        const cancel = new vscode.CancellationTokenSource();
        const linked = token.onCancellationRequested(() => cancel.cancel());
        let child = null;
        try {
            let targets = [];
            if (request && Array.isArray(request.include) && request.include.length) {
                collectTargets(request.include, targets);
            } else {
                targets = allTestItems();
            }
            const seen = new Set();
            targets = targets.filter(t => t && !seen.has(t.id) && seen.add(t.id));
            // Group requested tests by their file so each file compiles once.
            const byFile = new Map();
            for (const item of targets) {
                const rest = item.id.slice(TEST_ID.length);
                const at = rest.lastIndexOf('::');
                const key = rest.slice(0, at);
                let group = byFile.get(key);
                if (!group) {
                    group = { uri: item.uri, tests: new Map() };
                    byFile.set(key, group);
                }
                group.tests.set(rest.slice(at + 2), item);
            }
            for (const group of byFile.values()) {
                if (cancel.token.isCancellationRequested) break;
                if (!group.uri) continue;
                await runFile(group, run, cancel.token, c => { child = c; });
            }
        } catch (error) {
            env.output.appendLine(`A tests: ${error && error.message ? error.message : error}`);
        } finally {
            linked.dispose();
            cancel.dispose();
            try { if (child) child.kill(); } catch {}
            run.end();
        }
    }

    const kindRun = vscode.TestRunProfileKind && vscode.TestRunProfileKind.Run;
    controller.createRunProfile('Run Tests', kindRun, runRequest, true);
    controller.refreshHandler = token => discover(token);

    const watcher = vscode.workspace.createFileSystemWatcher('**/*.astra');
    context.subscriptions.push(
        watcher,
        watcher.onDidCreate(uri => {
            const folder = vscode.workspace.getWorkspaceFolder(uri);
            if (folder && uri.scheme === 'file') void syncFile(uri, folder);
        }),
        watcher.onDidChange(uri => {
            const folder = vscode.workspace.getWorkspaceFolder(uri);
            if (folder && uri.scheme === 'file') void syncFile(uri, folder);
        }),
        watcher.onDidDelete(uri => removeFile(uri.toString())),
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (isA(doc) && doc.uri.scheme === 'file') scheduleSync(doc.uri);
        }),
        vscode.workspace.onDidChangeTextDocument(event => {
            if (isA(event.document) && event.contentChanges.length) scheduleSync(event.document.uri);
        }),
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (isA(doc) && doc.uri.scheme === 'file') scheduleSync(doc.uri);
        })
    );

    // Initial discovery is cheap (one findFiles per folder) and keeps the
    // Testing view populated before the user presses refresh.
    void discover();
}

module.exports = { registerTestController, testNamesIn };
