'use strict';

// A Language extension. With a built a-lsp, the language server analyzes the
// programs as they are edited and answers the editor's queries; otherwise,
// and in an untrusted workspace, saved files are checked with `a check
// --json` (the check-on-save mode). Either way the `a` compiler drives the
// Run/Build/Test/Check/Format tasks, keybindings and the debug picker.

const vscode = require('vscode');
const path = require('node:path');
const {
    settings, argumentsFor, serverPath, isExecutable, modulePaths, absolute,
    quotedImportCandidates, bareImportCandidates
} = require('./config');
const { runCheck, CheckQueue } = require('./diagnostics');
const { declarations, imports } = require('./language');
const { LanguageClient, toRange } = require('./lsp-client');

const isA = doc => doc.languageId === 'a' && doc.uri.scheme === 'file';

const ACTION_TITLES = {
    run: 'Run Program',
    build: 'Build Executable',
    test: 'Run Tests',
    check: 'Check Program',
    fmt: 'Format File'
};

// What deactivation stops: the language server of the window, if one runs.
let stopOnDeactivate = async () => {};

async function activate(context) {
    const checkDiagnostics = vscode.languages.createDiagnosticCollection('a-check');
    const output = vscode.window.createOutputChannel('A');
    let disposed = false;
    let publication = 0;
    let reportedFailure = '';

    // The extension runs in one of two modes. With a compiler that serves an
    // editor (a-lsp), the language server analyzes the programs as they are
    // edited and answers the editor's queries; otherwise, and in an untrusted
    // workspace, the saved files are checked with `a check` (the check-on-save
    // mode: the queue, the outline and the import links below). `server` is
    // the language server's state: off, starting or running, with the setup
    // it was started for.
    let server = { status: 'off' };
    // The check-on-save mode's subscriptions while it runs.
    let fallback;

    // What the last check of each entry file found: a failure with no source
    // location, or the number of errors, and whether sources changed since.
    const checked = new Map();
    // The status of the active file's entry, in the bottom status bar. With
    // the server: the errors it reports, over every file. Without: the last
    // check, marked stale once sources change, with a spinner while a new
    // check runs.
    const checkStatus = vscode.window.createStatusBarItem('a.check', vscode.StatusBarAlignment.Left, 4);
    checkStatus.name = 'A: Check';
    function updateCheckStatus() {
        const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        if (!vscode.workspace.isTrusted || !doc || doc.languageId !== 'a') { checkStatus.hide(); return; }
        checkStatus.command = 'a.showOutput';
        checkStatus.backgroundColor = undefined;
        if (server.status !== 'off') {
            if (server.status === 'starting') {
                checkStatus.text = '$(sync~spin) A: Starting';
                checkStatus.tooltip = 'Starting the A language server.';
            } else {
                let errors = 0;
                for (const [, items] of vscode.languages.getDiagnostics()) {
                    errors += items.filter(item => item.severity === vscode.DiagnosticSeverity.Error).length;
                }
                if (errors) {
                    checkStatus.text = `$(error) A: ${errors} error${errors === 1 ? '' : 's'}`;
                    checkStatus.tooltip = 'The A language server reports errors. Click to open Problems.';
                    checkStatus.command = 'workbench.actions.view.problems';
                } else {
                    checkStatus.text = '$(check) A';
                    checkStatus.tooltip = 'The A language server reports no errors. Click to open the A output.';
                }
            }
            checkStatus.show();
            return;
        }
        const config = isA(doc) ? settings(vscode, doc.uri) : undefined;
        const running = config && queue.jobs.has(config.file);
        const last = config && checked.get(config.file);
        if (running) {
            checkStatus.text = '$(sync~spin) A: Checking';
            checkStatus.tooltip = last && !last.stale ? 'Checking again.' : 'Checking the saved files.';
        } else if (!last) {
            checkStatus.text = '$(circle-outline) A';
            checkStatus.tooltip = config && config.checkOnSave ? 'Not checked yet. Save to check.' : 'Not checked yet. Run A: Check Program to check.';
        } else if (last.problem) {
            checkStatus.text = `$(warning) A: ${last.problem.title}`;
            checkStatus.tooltip = `${last.problem.message.split('\n').slice(0, 6).join('\n')}\n\nClick to open the A output.`;
            checkStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
        } else if (last.errors > 0) {
            checkStatus.text = `$(error) A: ${last.errors} error${last.errors === 1 ? '' : 's'}`;
            checkStatus.tooltip = 'The last A check found errors. Click to open Problems.';
            checkStatus.command = 'workbench.actions.view.problems';
        } else {
            checkStatus.text = '$(check) A';
            checkStatus.tooltip = 'The last A check found no errors. Click to open the A output.';
        }
        if (last && last.stale && !running) {
            checkStatus.text += ' (stale)';
            checkStatus.tooltip = `Edited since this check; save to check again.\n\n${checkStatus.tooltip}`;
        }
        checkStatus.show();
    }

    // A result's diagnostics, built once when it lands, against the sources it
    // checked: a stale result keeps them as they were while files are edited.
    async function build(result) {
        const { config, error } = result;
        const lines = new Map();
        async function sourceLine(file, line) {
            if (!lines.has(file)) {
                const open = vscode.workspace.textDocuments.find(doc => doc.uri.fsPath === file);
                try {
                    const source = open ? open.getText() : Buffer.from(await vscode.workspace.fs.readFile(vscode.Uri.file(file))).toString('utf8');
                    lines.set(file, source.split(/\r?\n/));
                } catch { lines.set(file, []); }
            }
            return lines.get(file)[line];
        }
        const outputText = result.output || '';
        output.appendLine(`> ${config.compiler} ${(result.args || argumentsFor('check', config, { json: true })).map(arg => JSON.stringify(arg)).join(' ')}`);
        if (outputText.trim()) output.appendLine(outputText.trimEnd());
        // A failure with no source location is not an error on any line. It is
        // listed in Problems against the checked file at an empty range, so it
        // marks no text. It also goes to the status bar and the output channel.
        let problem;
        const items = Array.isArray(result.diagnostics) ? result.diagnostics : [];
        if (error && !items.some(item => item.severity === 'error')) {
            const missing = error.code === 'ENOENT';
            const message = missing
                ? `A compiler not found: ${config.compiler}. Set a.compilerPath to the executable.`
                : error.killed ? `A check exceeded ${config.timeout} ms or was terminated.`
                : failureMessage(outputText) || error.message;
            problem = { message, title: missing ? 'Compiler Not Found' : error.killed ? 'Check Timed Out' : 'Check Failed' };
            items.push({ file: config.file, line: 0, column: 0, empty: true, severity: 'warning', message: `A check failed: ${message}`, help: '' });
            if (missing && reportedFailure !== message) {
                reportedFailure = message;
                void vscode.window.showWarningMessage(message);
            }
        } else if (!error) reportedFailure = '';
        const files = new Map();
        let errors = 0;
        for (const item of items) {
            // The compiler reports paths as given or relative to its cwd.
            const filePath = path.isAbsolute(item.file) ? item.file : path.resolve(config.cwd, item.file);
            const line = (await sourceLine(filePath, item.line)) ?? '';
            const start = item.column === undefined ? Math.max(0, line.search(/\S/)) : Math.min(item.column, line.length);
            const from = new vscode.Position(item.line, start);
            let range;
            if (item.empty) {
                range = new vscode.Range(from, from);
            } else {
                // Underline the identifier at the column, or one character.
                const end = item.column === undefined
                    ? line.length
                    : Math.min(line.length, start + (line.slice(start).match(/^[A-Za-z_]\w*/) || [''])[0].length || 1);
                range = new vscode.Range(item.line, start, item.line, Math.max(start, end));
            }
            const diagnostic = new vscode.Diagnostic(range,
                item.help ? `${item.message}\n\n${item.help}` : item.message,
                item.severity === 'warning' ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error);
            diagnostic.source = 'a';
            const entry = files.get(filePath) || [];
            if (entry.some(other => other.message === diagnostic.message && other.range.isEqual(range))) continue;
            entry.push(diagnostic);
            files.set(filePath, entry);
            if (diagnostic.severity === vscode.DiagnosticSeverity.Error) errors++;
        }
        return { files, problem, errors };
    }
    const built = new WeakMap();

    async function publish(results) {
        if (disposed) return;
        const generation = ++publication;
        const current = [...results.values()];
        for (const result of current) if (!built.has(result)) built.set(result, build(result));
        const found = await Promise.all(current.map(result => built.get(result)));
        if (disposed || generation !== publication) return;
        const grouped = new Map();
        checked.clear();
        current.forEach((result, i) => {
            const { files, problem, errors } = found[i];
            checked.set(result.config.file, { problem, errors, stale: !!result.stale });
            for (const [file, items] of files) {
                const entry = grouped.get(file) || [];
                for (const diagnostic of items) {
                    if (!entry.some(other => other.message === diagnostic.message && other.range.isEqual(diagnostic.range))) entry.push(diagnostic);
                }
                grouped.set(file, entry);
            }
        });
        checkDiagnostics.clear();
        checkDiagnostics.set([...grouped].map(([file, items]) => [vscode.Uri.file(file), items]));
        updateCheckStatus();
    }

    const queue = new CheckQueue(runCheck, results => {
        void publish(results).catch(error => output.appendLine(`Diagnostics: ${error.message}`));
    });
    const cwdOf = doc => settings(vscode, doc.uri).cwd;
    const dirtyIn = cwd => vscode.workspace.textDocuments.some(doc => isA(doc) && doc.isDirty && cwdOf(doc) === cwd);

    function check(doc) {
        if (disposed || !fallback || !vscode.workspace.isTrusted || !isA(doc) || doc.isDirty) return;
        const config = settings(vscode, doc.uri);
        if (config.checkOnSave && !dirtyIn(config.cwd)) void queue.schedule(config);
    }

    // Check the entry files of open documents in cwd (or everywhere), and drop
    // the results of entries no open document checks any more.
    function recheck(cwd) {
        if (!fallback) return;
        const entries = new Map();
        for (const doc of vscode.workspace.textDocuments) {
            if (!isA(doc)) continue;
            const config = settings(vscode, doc.uri);
            if (config.checkOnSave && !entries.has(config.file)) entries.set(config.file, { doc, cwd: config.cwd });
        }
        queue.retain(file => entries.has(file));
        for (const entry of entries.values()) if (!cwd || entry.cwd === cwd) check(entry.doc);
    }

    // Sources in cwd were saved, closed or changed on disk: what was found
    // stays, stale, until rechecked.
    function refresh(cwd) {
        queue.stale(cwd);
        recheck(cwd);
    }

    // The check-on-save mode: the checks of saved files, the outline from a
    // lexical scan and the import links, which the server provides otherwise.
    function startFallback() {
        if (fallback || disposed) return;
        const watcher = vscode.workspace.createFileSystemWatcher('**/*.a');
        const changedOnDisk = async uri => {
            const cwd = settings(vscode, uri).cwd;
            // Filesystem notifications can arrive after a manual check of a just-
            // saved file. Do not cancel that check or erase its current results.
            let modified = Infinity;
            try { modified = (await vscode.workspace.fs.stat(uri)).mtime; } catch { /* Deleted file. */ }
            if (disposed || !fallback) return;
            // A stale result is already due to be replaced, so it does not count.
            const existing = [...queue.results.values(), ...queue.jobs.values()].filter(item => item.config.cwd === cwd && !item.stale);
            if (existing.length && existing.every(item => item.startedAt === undefined || item.startedAt > modified)) return;
            refresh(cwd);
        };
        fallback = vscode.Disposable.from(
            vscode.languages.registerDocumentSymbolProvider([{ language: 'a', scheme: 'file' }, { language: 'a', scheme: 'untitled' }], {
                provideDocumentSymbols(doc) {
                    return declarations(doc.getText()).map(item => new vscode.DocumentSymbol(item.name, item.detail,
                        symbolKind(item.kind),
                        new vscode.Range(doc.positionAt(item.start), doc.positionAt(item.end)),
                        new vscode.Range(doc.positionAt(item.nameStart), doc.positionAt(item.nameEnd))));
                }
            }),
            vscode.languages.registerDocumentLinkProvider({ language: 'a', scheme: 'file' }, {
                async provideDocumentLinks(doc, token) {
                    const config = settings(vscode, doc.uri);
                    const links = [];
                    for (const item of imports(doc.getText())) {
                        const candidates = item.quoted
                            ? quotedImportCandidates(item.spec, doc.uri.fsPath, config)
                            : bareImportCandidates(item.spec, config);
                        for (const candidate of candidates) {
                            if (token.isCancellationRequested) return [];
                            try {
                                const uri = vscode.Uri.file(candidate);
                                const stat = await vscode.workspace.fs.stat(uri);
                                if (!(stat.type & vscode.FileType.File)) continue;
                                const range = new vscode.Range(
                                    new vscode.Position(item.line, item.startChar),
                                    new vscode.Position(item.line, item.endChar));
                                const link = new vscode.DocumentLink(range, uri);
                                link.tooltip = 'Open A module';
                                links.push(link);
                                break;
                            } catch { /* Try the next location, as the compiler does. */ }
                        }
                    }
                    return links;
                }
            }),
            vscode.workspace.onDidOpenTextDocument(check),
            vscode.workspace.onDidSaveTextDocument(doc => {
                if (isA(doc)) refresh(cwdOf(doc));
            }),
            vscode.workspace.onDidChangeTextDocument(event => {
                if (isA(event.document) && event.contentChanges.length) queue.stale(cwdOf(event.document));
            }),
            vscode.workspace.onDidCloseTextDocument(doc => {
                if (isA(doc)) refresh(cwdOf(doc));
            }),
            watcher, watcher.onDidCreate(changedOnDisk), watcher.onDidChange(changedOnDisk), watcher.onDidDelete(changedOnDisk)
        );
        recheck();
        updateCheckStatus();
    }

    function stopFallback() {
        if (!fallback) return;
        fallback.dispose();
        fallback = undefined;
        queue.invalidate();
        checkDiagnostics.clear();
        checked.clear();
    }

    // --- the language server ---------------------------------------------

    async function stopServer() {
        const client = server.client;
        server = { status: 'off' };
        openedDocuments.clear();
        for (const timer of changeTimers.values()) clearTimeout(timer);
        changeTimers.clear();
        if (client) {
            try { await client.stop(); } catch { /* It may be gone already. */ }
        }
        updateCheckStatus();
    }
    stopOnDeactivate = stopServer;

    async function startServer(setup) {
        await stopServer();
        if (disposed) return;
        const client = new LanguageClient(setup.path, setup.args,
            { modulePaths: setup.modulePaths, stdlibRoot: setup.stdlib }, output);
        server = { status: 'starting', client, setup };
        updateCheckStatus();
        output.appendLine(`> ${setup.path} ${(setup.args || []).map(arg => JSON.stringify(arg)).join(' ')}  (language server)`);
        try {
            await client.start();
            if (server.client !== client) return;
            server.status = 'running';
            for (const document of vscode.workspace.textDocuments) {
                if (isA(document)) client.openDocument(document);
            }
        } catch (error) {
            if (server.client !== client) return;
            output.appendLine(`The A language server did not start: ${error.message}`);
            server = { status: 'off' };
            try { client.stop(); } catch { /* It may be gone already. */ }
            void vscode.window.showWarningMessage(`The A language server did not start (${error.message}); checking saved files instead. See the A output.`);
            startFallback();
        }
        updateCheckStatus();
    }

    // Which mode runs: the server where the settings ask for one, the
    // workspace is trusted and a-lsp is found, else the checks of saved files.
    // Decided again when that may have changed: the settings, the folders,
    // the workspace's trust, a restart. A server already running for the same
    // setup stays.
    let deciding = Promise.resolve();
    function decide(restart = false) {
        deciding = deciding.then(async () => {
            if (disposed) return;
            const setup = serverSetup();
            // A bare command name (no path separators) is resolved on PATH by
            // the spawn; anything path-like must exist and be executable.
            const runnable = setup.path && (isExecutable(setup.path) || !/[\\/]/.test(setup.path));
            const wanted = vscode.workspace.isTrusted && setup.enabled && runnable;
            if (!wanted) {
                await stopServer();
                startFallback();
                return;
            }
            if (!restart && server.client && JSON.stringify(server.setup) === JSON.stringify(setup)) return;
            stopFallback();
            await startServer(setup);
        }).catch(error => output.appendLine(`Language server: ${error.message}`));
        return deciding;
    }

    function serverSetup() {
        const folders = vscode.workspace.workspaceFolders || [];
        const first = folders[0];
        const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        const doc = active && isA(active) ? active : vscode.workspace.textDocuments.find(isA);
        const cwd = first ? first.uri.fsPath : doc ? path.dirname(doc.uri.fsPath) : process.cwd();
        const config = vscode.workspace.getConfiguration('a', first && first.uri ? first.uri : doc && doc.uri);
        const configured = config.get('server.path', '') || process.env.A_LSP_PATH || '';
        const stdlibRaw = config.get('stdlibPath', '');
        return {
            enabled: config.get('useLanguageServer', true),
            path: serverPath(configured, cwd, context.extensionPath),
            args: Array.isArray(config.get('server.args', [])) ? config.get('server.args', []) : [],
            modulePaths: modulePaths(config, cwd, ''),
            stdlib: stdlibRaw ? absolute(stdlibRaw, cwd, '') : ''
        };
    }

    // The document a command acts on, the active editor's or the one at
    // `uri`, saved with the open A files of its folder.
    async function savedDocument(uri) {
        const pick = async () => uri
            ? vscode.workspace.openTextDocument(uri).then(doc => doc, () => undefined)
            : vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        const doc = await pick();
        if (!doc || !isA(doc)) return undefined;
        if (!vscode.workspace.isTrusted) {
            void vscode.window.showWarningMessage('Trust this workspace to run the A compiler.');
            return undefined;
        }
        if (doc.isUntitled && !await doc.save()) return undefined;
        const active = await pick();
        if (!active || !isA(active)) return undefined;
        const cwd = cwdOf(active);
        for (const open of vscode.workspace.textDocuments) {
            if (isA(open) && open.isDirty && cwdOf(open) === cwd && !await open.save()) return undefined;
        }
        return active;
    }

    function makeTask(definition, uri, scope, name) {
        if (!vscode.workspace.isTrusted || uri.scheme !== 'file') return undefined;
        const config = settings(vscode, uri, definition, typeof scope === 'object' ? scope : undefined);
        if (!ACTION_TITLES[definition.action]) return undefined;
        const task = new vscode.Task({ ...definition, type: 'a', file: config.file },
            scope || config.workspaceFolder || vscode.TaskScope.Workspace,
            name || ACTION_TITLES[definition.action], 'A',
            new vscode.ProcessExecution(config.compiler, argumentsFor(definition.action, config), { cwd: config.cwd }), '$a');
        // VS Code reserves isDefault for tasks.json. Contribute Run as our only
        // build task; explicit Build/Test/Check/Format tasks remain in
        // Tasks: Run Task.
        if (definition.action === 'run') task.group = vscode.TaskGroup.Build;
        task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: true };
        return task;
    }

    const runStatus = vscode.window.createStatusBarItem('a.run', vscode.StatusBarAlignment.Left, 5);
    runStatus.name = 'A: Run Program';
    runStatus.text = '$(play) Run';
    runStatus.tooltip = 'Compile and run the A program';
    runStatus.command = 'a.run';
    function updateRunStatus() {
        if (vscode.workspace.isTrusted && vscode.window.activeTextEditor && isA(vscode.window.activeTextEditor.document)) runStatus.show();
        else runStatus.hide();
    }

    const runConfigurations = () => [{ type: 'a', request: 'launch', name: 'A: Run Program', noDebug: true }];
    // Participate in the native Run configuration picker without claiming a
    // debugger. Launch the same terminal task, then stop debug resolution.
    const launchProvider = {
        provideDebugConfigurations: runConfigurations,
        resolveDebugConfiguration(_folder, config) {
            return { ...runConfigurations()[0], ...config, noDebug: true };
        },
        async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
            if (!vscode.workspace.isTrusted) return undefined;
            if (config.request !== 'launch') {
                void vscode.window.showErrorMessage('A supports run-only launch; attaching a debugger is not available.');
                return undefined;
            }
            const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
            const entry = (folder || active) && vscode.workspace.getConfiguration('a', folder && folder.uri || active && active.uri).get('entryFile', '');
            let uri;
            if (config.program || entry) {
                const file = config.program || entry;
                const base = folder && folder.uri.fsPath || (active && active.uri.scheme === 'file' ? path.dirname(active.uri.fsPath) : undefined);
                if (!base && !path.isAbsolute(file)) {
                    void vscode.window.showErrorMessage('Open a workspace folder or use an absolute A program path.');
                    return undefined;
                }
                uri = vscode.Uri.file(path.isAbsolute(file) ? file : path.resolve(base || path.dirname(file), file));
            } else if (!folder || active && vscode.workspace.getWorkspaceFolder(active.uri) && vscode.workspace.getWorkspaceFolder(active.uri).uri.toString() === folder.uri.toString()) {
                const saved = await savedDocument();
                uri = saved && saved.uri;
            }
            if (!uri) {
                void vscode.window.showErrorMessage('Open an A file or set a.entryFile to run the program.');
                return undefined;
            }
            const definition = { type: 'a', action: 'run', file: uri.fsPath };
            const cwd = settings(vscode, uri, definition, folder).cwd;
            for (const doc of vscode.workspace.textDocuments) {
                if (isA(doc) && doc.isDirty && cwdOf(doc) === cwd && !await doc.save()) return undefined;
            }
            await vscode.tasks.executeTask(makeTask(definition, uri, folder));
            return undefined;
        }
    };

    // --- LSP providers -----------------------------------------------------

    function registerProviders(getClient) {
        const selector = { language: 'a', scheme: 'file' };

        context.subscriptions.push(
            vscode.languages.registerDefinitionProvider(selector, {
                async provideDefinition(document, position) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/definition', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                    });
                    return locationsFrom(result);
                },
            }),

            vscode.languages.registerReferencesProvider(selector, {
                async provideReferences(document, position, refContext) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/references', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                        context: { includeDeclaration: refContext.includeDeclaration },
                    });
                    return locationsFrom(result);
                },
            }),

            vscode.languages.registerHoverProvider(selector, {
                async provideHover(document, position) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/hover', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                    });
                    if (!result || !result.contents) return null;
                    const range = result.range ? toRange(result.range) : undefined;
                    return new vscode.Hover(hoverContents(result.contents), range);
                },
            }),

            vscode.languages.registerCompletionItemProvider(
                selector,
                {
                    async provideCompletionItems(document, position) {
                        const active = getClient();
                        if (!active) return null;
                        const result = await active.request('textDocument/completion', {
                            textDocument: { uri: document.uri.toString() },
                            position: { line: position.line, character: position.character },
                        });
                        if (!result) return null;
                        const entries = Array.isArray(result) ? result : result.items || [];
                        const items = entries.map((entry) => {
                            const item = new vscode.CompletionItem(entry.label, toCompletionKind(entry.kind));
                            if (entry.detail) item.detail = entry.detail;
                            if (entry.documentation) item.documentation = documentationToMarkdown(entry.documentation);
                            item.insertText = typeof entry.insertText === 'string' ? entry.insertText : entry.label;
                            if (entry.filterText) item.filterText = entry.filterText;
                            if (entry.sortText) item.sortText = entry.sortText;
                            if (entry.data) item.data = entry.data;
                            return item;
                        });
                        return new vscode.CompletionList(items, Boolean(result.isIncomplete));
                    },
                    async resolveCompletionItem(item) {
                        const active = getClient();
                        if (!active || !item.data) return item;
                        const result = await active.request('completionItem/resolve', {
                            label: item.label,
                            data: item.data,
                        });
                        if (result && result.documentation) {
                            item.documentation = documentationToMarkdown(result.documentation);
                        }
                        return item;
                    },
                },
                '.',
                '"',
                '(',
                ','
            ),

            vscode.languages.registerSignatureHelpProvider(selector, {
                async provideSignatureHelp(document, position) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/signatureHelp', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                    });
                    return signatureHelpFrom(result);
                },
            }),

            vscode.languages.registerDocumentSymbolProvider(selector, {
                async provideDocumentSymbols(document) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/documentSymbol', {
                        textDocument: { uri: document.uri.toString() },
                    });
                    if (!Array.isArray(result)) return null;
                    return result.map(convertDocumentSymbol);
                },
            }),

            vscode.languages.registerDocumentFormattingEditProvider(selector, {
                async provideDocumentFormattingEdits(document, options) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/formatting', {
                        textDocument: { uri: document.uri.toString() },
                        options: {
                            tabSize: options.tabSize,
                            insertSpaces: options.insertSpaces,
                        },
                    });
                    if (!Array.isArray(result)) return null;
                    return result.map(toTextEdit);
                },
            }),

            vscode.languages.registerRenameProvider(selector, {
                async provideRenameEdits(document, position, newName) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/rename', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                        newName,
                    });
                    if (!result) return null;
                    return workspaceEditFromLsp(result);
                },
            }),

            vscode.languages.registerDocumentHighlightProvider(selector, {
                async provideDocumentHighlights(document, position) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/documentHighlight', {
                        textDocument: { uri: document.uri.toString() },
                        position: { line: position.line, character: position.character },
                    });
                    if (!Array.isArray(result)) return null;
                    return result.map((entry) => {
                        const kind = Math.max(0, Math.min(2, (entry.kind || 1) - 1));
                        return new vscode.DocumentHighlight(toRange(entry.range), kind);
                    });
                },
            }),

            vscode.languages.registerFoldingRangeProvider(selector, {
                async provideFoldingRanges(document) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('textDocument/foldingRange', {
                        textDocument: { uri: document.uri.toString() },
                    });
                    if (!Array.isArray(result)) return null;
                    return result.map(
                        (entry) =>
                            new vscode.FoldingRange(
                                entry.startLine,
                                entry.endLine,
                                entry.kind === 'region'
                                    ? vscode.FoldingRangeKind.Region
                                    : entry.kind === 'comment'
                                        ? vscode.FoldingRangeKind.Comment
                                        : undefined
                            )
                    );
                },
            }),

            vscode.languages.registerWorkspaceSymbolProvider({
                async provideWorkspaceSymbols(query) {
                    const active = getClient();
                    if (!active) return null;
                    const result = await active.request('workspace/symbol', { query });
                    if (!Array.isArray(result)) return null;
                    return result
                        .filter((entry) => entry && entry.location)
                        .map(
                            (entry) =>
                                new vscode.SymbolInformation(
                                    entry.name,
                                    toSymbolKind(entry.kind),
                                    entry.containerName || 'A',
                                    new vscode.Location(
                                        vscode.Uri.parse(entry.location.uri),
                                        toRange(entry.location.range)
                                    )
                                )
                        );
                },
            })
        );
    }

    // --- document sync -----------------------------------------------------

    const openedDocuments = new Set();
    const changeTimers = new Map();

    function debounceChange(document) {
        const key = document.uri.toString();
        const existing = changeTimers.get(key);
        if (existing) clearTimeout(existing);
        changeTimers.set(
            key,
            setTimeout(() => {
                changeTimers.delete(key);
                const active = getClient();
                if (active && openedDocuments.has(key)) active.changeDocument(document);
            }, 250)
        );
    }

    // --- wiring ------------------------------------------------------------

    const getClient = () => server.status === 'running' ? server.client : undefined;
    registerProviders(getClient);

    context.subscriptions.push(
        checkDiagnostics, output, runStatus, checkStatus,
        { dispose() { disposed = true; stopFallback(); queue.invalidate(); void stopServer(); } },
        vscode.window.onDidChangeActiveTextEditor(() => { updateRunStatus(); updateCheckStatus(); }),
        vscode.languages.onDidChangeDiagnostics(() => { if (server.status !== 'off') updateCheckStatus(); }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => { updateRunStatus(); updateCheckStatus(); void decide(); }),
        vscode.debug.registerDebugConfigurationProvider('a', launchProvider),
        vscode.debug.registerDebugConfigurationProvider('a', { provideDebugConfigurations: runConfigurations }, vscode.DebugConfigurationProviderTriggerKind.Dynamic),
        vscode.commands.registerCommand('a.showOutput', () => output.show()),
        vscode.commands.registerCommand('a.restartServer', async () => {
            await decide(true);
            vscode.window.showInformationMessage('A language server restarted.');
        }),
        vscode.commands.registerCommand('a.check', async () => {
            const doc = await savedDocument();
            if (!doc) return;
            // With the server checking the program as it is edited, an explicit
            // check shows the compiler's whole output in a terminal.
            if (server.status !== 'off') {
                await vscode.tasks.executeTask(makeTask({ type: 'a', action: 'check' }, doc.uri));
                return;
            }
            const completed = await queue.schedule(settings(vscode, doc.uri), 0);
            if (completed) output.show(true);
        }),
        // The editor title's play button runs the active file; a string
        // argument (a URI, as a code lens might pass) runs that file instead.
        vscode.commands.registerCommand('a.run', async target => {
            const uri = typeof target === 'string' ? vscode.Uri.parse(target) : target instanceof vscode.Uri ? target : undefined;
            const doc = await savedDocument(uri);
            if (!doc) return;
            const definition = { type: 'a', action: 'run' };
            if (uri) definition.file = doc.uri.fsPath;
            await vscode.tasks.executeTask(makeTask(definition, doc.uri));
        }),
        ...['build', 'test', 'fmt'].map(action => vscode.commands.registerCommand(`a.${action}`, async () => {
            const doc = await savedDocument();
            if (doc) await vscode.tasks.executeTask(makeTask({ type: 'a', action }, doc.uri));
        })),
        vscode.tasks.registerTaskProvider('a', {
            provideTasks() {
                if (!vscode.workspace.isTrusted) return [];
                const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
                if (doc && isA(doc)) return Object.keys(ACTION_TITLES).map(action => makeTask({ type: 'a', action }, doc.uri));
                // A configured program is runnable even when another editor (or
                // just the Run view) has focus.
                return (vscode.workspace.workspaceFolders || []).flatMap(folder => {
                    const entry = vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', '');
                    if (!entry) return [];
                    const uri = vscode.Uri.file(absolute(entry, folder.uri.fsPath, ''));
                    return Object.keys(ACTION_TITLES).map(action => makeTask({ type: 'a', action }, uri, folder));
                });
            },
            resolveTask(task) {
                if (!vscode.workspace.isTrusted) return undefined;
                const folder = typeof task.scope === 'object' ? task.scope : undefined;
                const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
                const base = folder && folder.uri.fsPath;
                const file = task.definition.file;
                const configuredEntry = folder && vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', '');
                const uri = file && base && !file.includes('${file}')
                    ? vscode.Uri.file(absolute(file, base, ''))
                    : active && isA(active) && (!base || cwdOf(active) === base) ? active.uri
                    : configuredEntry ? vscode.Uri.file(absolute(configuredEntry, base || '', '')) : undefined;
                if (!uri) return undefined;
                const resolved = makeTask(task.definition, uri, task.scope, task.name);
                if (resolved) {
                    resolved.definition = task.definition;
                    resolved.presentationOptions = task.presentationOptions;
                    if (task.group) resolved.group = task.group;
                    resolved.problemMatchers = task.problemMatchers.length ? task.problemMatchers : ['$a'];
                    resolved.runOptions = task.runOptions;
                }
                return resolved;
            }
        }),
        // Results checked with other settings or workspace folders no longer
        // apply, and the server may be wanted, or wanted otherwise.
        vscode.workspace.onDidChangeConfiguration(event => {
            if (!event.affectsConfiguration('a')) return;
            if (fallback) { queue.invalidate(); recheck(); }
            void decide();
        }),
        vscode.workspace.onDidChangeWorkspaceFolders(() => {
            if (fallback) { queue.invalidate(); recheck(); }
            void decide();
        }),
        // An A file opened without a folder decides the server's working
        // directory: the first one starts the server.
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (isA(doc) && !(vscode.workspace.workspaceFolders || []).length && server.status === 'off') void decide();
            const active = getClient();
            if (!active || !isA(doc)) return;
            openedDocuments.add(doc.uri.toString());
            active.openDocument(doc);
        }),

        vscode.workspace.onDidChangeTextDocument(event => {
            if (!getClient() || !isA(event.document)) return;
            if (!openedDocuments.has(event.document.uri.toString())) return;
            if (!event.contentChanges.length) return;
            debounceChange(event.document);
        }),

        vscode.workspace.onDidSaveTextDocument(document => {
            const active = getClient();
            if (!active || !isA(document)) return;
            const key = document.uri.toString();
            const timer = changeTimers.get(key);
            if (timer) {
                clearTimeout(timer);
                changeTimers.delete(key);
            }
            if (openedDocuments.has(key)) active.saveDocument(document);
        }),

        vscode.workspace.onDidCloseTextDocument(document => {
            const active = getClient();
            if (!isA(document)) return;
            const key = document.uri.toString();
            const timer = changeTimers.get(key);
            if (timer) clearTimeout(timer);
            changeTimers.delete(key);
            if (active && openedDocuments.delete(key)) active.closeDocument(document);
        })
    );
    updateRunStatus();
    updateCheckStatus();
    await decide();
}

function deactivate() {
    return stopOnDeactivate();
}

// ------------------------------------------------------------- utilities

function failureMessage(output) {
    return String(output || '').replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/).join('\n').trim();
}

function symbolKind(name) {
    switch (name) {
        case 'Function': return vscode.SymbolKind.Function;
        case 'Struct': return vscode.SymbolKind.Struct;
        case 'Class': return vscode.SymbolKind.Class;
        case 'Enum': return vscode.SymbolKind.Enum;
        case 'Interface': return vscode.SymbolKind.Interface;
        case 'TypeParameter': return vscode.SymbolKind.TypeParameter;
        default: return vscode.SymbolKind.Variable;
    }
}

function toSymbolKind(kind) {
    const value = typeof kind === 'number' ? kind - 1 : 0;
    return Math.max(0, Math.min(25, value));
}

function toCompletionKind(kind) {
    const value = typeof kind === 'number' ? kind - 1 : 0;
    return Math.max(0, Math.min(24, value));
}

function hoverContents(contents) {
    if (!contents) return new vscode.MarkdownString('');
    if (Array.isArray(contents)) {
        const markdown = new vscode.MarkdownString();
        for (const part of contents) {
            const value = typeof part === 'string' ? part : part && part.value;
            if (value) markdown.appendMarkdown(value);
            markdown.appendMarkdown('\n\n');
        }
        return markdown;
    }
    if (typeof contents === 'string') return new vscode.MarkdownString(contents);
    return new vscode.MarkdownString(contents.value || '');
}

function convertDocumentSymbol(symbol) {
    const converted = new vscode.DocumentSymbol(
        symbol.name,
        symbol.detail || '',
        toSymbolKind(symbol.kind),
        toRange(symbol.range),
        toRange(symbol.selectionRange)
    );
    if (Array.isArray(symbol.children) && symbol.children.length) {
        converted.children = symbol.children.map(convertDocumentSymbol);
    }
    return converted;
}

function toTextEdit(edit) {
    return new vscode.TextEdit(toRange(edit.range), edit.newText || '');
}

function workspaceEditFromLsp(edit) {
    const workspaceEdit = new vscode.WorkspaceEdit();
    if (!edit) return workspaceEdit;
    if (edit.changes) {
        for (const [uri, edits] of Object.entries(edit.changes)) {
            workspaceEdit.set(vscode.Uri.parse(uri), (edits || []).map(toTextEdit));
        }
    }
    if (Array.isArray(edit.documentChanges)) {
        for (const change of edit.documentChanges) {
            if (!change || !change.textDocument || !Array.isArray(change.edits)) continue;
            workspaceEdit.set(
                vscode.Uri.parse(change.textDocument.uri),
                change.edits.map(toTextEdit)
            );
        }
    }
    return workspaceEdit;
}

function locationsFrom(result) {
    if (!result) return null;
    const entries = Array.isArray(result) ? result : [result];
    const locations = [];
    for (const entry of entries) {
        if (!entry) continue;
        const uri = entry.uri || entry.targetUri;
        const range = entry.range || entry.targetSelectionRange || entry.targetRange;
        if (!uri || !range) continue;
        locations.push(new vscode.Location(vscode.Uri.parse(uri), toRange(range)));
    }
    return locations.length ? locations : null;
}

function documentationToMarkdown(documentation) {
    if (!documentation) return undefined;
    if (typeof documentation === 'string') return new vscode.MarkdownString(documentation);
    return new vscode.MarkdownString(documentation.value || '');
}

function signatureHelpFrom(result) {
    if (!result || !Array.isArray(result.signatures) || !result.signatures.length) return null;
    const signatures = result.signatures.map((entry) => {
        const signature = new vscode.SignatureInformation(entry.label);
        if (entry.documentation) signature.documentation = documentationToMarkdown(entry.documentation);
        if (Array.isArray(entry.parameters)) {
            signature.parameters = entry.parameters.map((parameter) => {
                const info = new vscode.ParameterInformation(parameter.label);
                if (parameter.documentation) info.documentation = documentationToMarkdown(parameter.documentation);
                return info;
            });
        }
        return signature;
    });
    const help = new vscode.SignatureHelp(signatures);
    help.activeSignature = Math.max(
        0,
        Math.min(result.activeSignature || 0, signatures.length - 1)
    );
    const active = signatures[help.activeSignature];
    if (active && typeof result.activeParameter === 'number') {
        help.activeParameter = Math.max(
            0,
            Math.min(result.activeParameter, (active.parameters || []).length - 1)
        );
    }
    return help;
}

module.exports = { activate, deactivate };
