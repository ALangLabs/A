'use strict';

// VS Code extension host for the A programming language. It prefers the a-lsp
// language server (diagnostics as you type, hover, go to definition, symbols,
// formatting, signature help) and falls back to check-on-save plus lexical
// features when the server is missing, disabled, or the workspace is untrusted.
// Tasks always run the `a` compiler directly. No dependencies: the LSP client
// and analysis live in plain Node modules beside this file.

const vscode = require('vscode');
const path = require('node:path');
const { LanguageClient } = require('./lsp-client');
const { declarations, imports } = require('./language');
const { STDLIB, KEYWORDS, BUILTINS, BUILTIN_SIGS } = require('./stdlib-data');
const {
    absolute, compilerPath, serverPath, isExecutable, settings, argumentsFor,
    quotedImportCandidates, bareImportCandidates
} = require('./config');

const ACTION_TITLES = { run: 'Run Program', build: 'Build Executable', test: 'Run Tests', check: 'Check Program', fmt: 'Format File' };
const A_DOC = { language: 'a', scheme: 'file' };
const A_SELECTOR = [A_DOC, { language: 'a', scheme: 'untitled' }];

function isA(doc) { return doc && doc.languageId === 'a'; }
function cwdOf(doc) { return path.dirname(doc.uri.fsPath); }

function completionKindFor(name, moduleName) {
    if (!moduleName) return vscode.CompletionItemKind.Module;
    if (/^[A-Z]/.test(name)) return vscode.CompletionItemKind.Class;
    if (moduleName && typeof name === 'string' && name === name.toUpperCase() && name.length > 2) return vscode.CompletionItemKind.Constant;
    return vscode.CompletionItemKind.Function;
}

function stdlibCompletionItems() {
    const items = [];
    for (const kw of KEYWORDS) {
        const it = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
        it.detail = 'A keyword';
        items.push(it);
    }
    for (const b of BUILTINS) {
        const it = new vscode.CompletionItem(b, vscode.CompletionItemKind.Function);
        it.detail = 'A builtin';
        items.push(it);
    }
    for (const [name, mod] of Object.entries(STDLIB)) {
        const it = new vscode.CompletionItem(name, vscode.CompletionItemKind.Module);
        it.detail = mod.detail || 'A stdlib module';
        it.documentation = new vscode.MarkdownString(mod.doc || '');
        items.push(it);
    }
    return items;
}

function memberCompletions(moduleName) {
    const mod = STDLIB[moduleName];
    if (!mod) return [];
    return (mod.members || []).map(name => {
        const it = new vscode.CompletionItem(name, completionKindFor(name, moduleName));
        it.detail = `${moduleName}.${name} — ${mod.detail || 'stdlib'}`;
        it.documentation = new vscode.MarkdownString((mod.doc || '') + `\n\nMember of \`std.${moduleName}\`.`);
        return it;
    });
}

function fallbackCompletionProvider() {
    return {
        provideCompletionItems(document, position) {
            const line = document.lineAt(position).text.slice(0, position.character);
            // import <module>
            const imp = /\b(?:from|import)\s+([A-Za-z_][\w/]*)?$/.exec(line);
            if (imp && !line.includes('.')) {
                return Object.keys(STDLIB).map(name => {
                    const mod = STDLIB[name];
                    const it = new vscode.CompletionItem(name, vscode.CompletionItemKind.Module);
                    it.detail = mod.detail || 'A stdlib module';
                    it.documentation = new vscode.MarkdownString(mod.doc || '');
                    return it;
                });
            }
            // <module>.
            const dot = /([A-Za-z_]\w*)\.$/.exec(line);
            if (dot) return memberCompletions(dot[1]);
            // local declarations + curated stdlib/global completions
            const declItems = declarations(document.getText()).map(d => {
                const it = new vscode.CompletionItem(d.name, symbolKind(d.kind));
                it.detail = `local ${d.detail}`;
                return it;
            });
            const seen = new Set(declItems.map(i => i.label));
            const items = [...declItems];
            for (const it of stdlibCompletionItems()) {
                if (!seen.has(it.label)) { seen.add(it.label); items.push(it); }
            }
            return items;
        }
    };
}

// Pyright-style layout: 3+ params or >48 chars render multi-line with one
// indented parameter per line. Mirrors the a-lsp hover renderer.
function splitTopLevelParams(inside) {
    const params = [];
    let current = '', depth = 0;
    for (const c of inside) {
        if (c === '(' || c === '[' || c === '{') depth++;
        if (c === ')' || c === ']' || c === '}') depth--;
        if (c === ',' && depth === 0) { params.push(current); current = ''; } else current += c;
    }
    if (current.trim()) params.push(current);
    return params.map(p => p.trim());
}

function formatSignature(signature) {
    const open = signature.indexOf('(');
    if (open < 0) return signature;
    let depth = 0, close = -1;
    for (let i = open; i < signature.length; i++) {
        const c = signature[i];
        if (c === '(' || c === '[' || c === '{') depth++;
        if (c === ')' || c === ']' || c === '}') { if (--depth === 0) { close = i; break; } }
    }
    if (close < 0) return signature;
    const head = signature.slice(0, open), tail = signature.slice(close + 1);
    const params = splitTopLevelParams(signature.slice(open + 1, close));
    if (!params.length) return signature;
    if (params.length < 3 && signature.length <= 48) return signature;
    let out = `${head}(\n`;
    params.forEach((p, i) => { out += `    ${p}${i + 1 < params.length ? ',' : ''}\n`; });
    return out + `)${tail}`;
}

const DOC_SECTIONS = { args: 'Args', arguments: 'Args', parameters: 'Parameters', params: 'Parameters', returns: 'Returns', return: 'Returns', example: 'Example', examples: 'Example', 'see also': 'See Also', note: 'Note' };

function renderDocSections(documentation) {
    if (!documentation) return '';
    const lines = String(documentation).split(/\r?\n/);
    const headingOf = line => {
        const m = /^([A-Za-z][A-Za-z ]*?):\s*(.*)$/.exec(line.trim());
        if (!m) return null;
        const label = DOC_SECTIONS[m[1].trim().toLowerCase()];
        return label ? { label, rest: m[2] } : null;
    };
    if (!lines.some(l => headingOf(l))) return documentation;
    let section = null, exampleMode = false, out = '';
    for (const raw of lines) {
        const line = raw.trim();
        const head = headingOf(line);
        if (head) {
            if (exampleMode) { out += '```\n'; exampleMode = false; }
            if (out && !out.endsWith('\n\n')) out += '\n';
            out += `**${head.label}:**${head.rest ? ' ' + head.rest : ''}\n`;
            section = head.label;
            continue;
        }
        if (!line) { out += '\n'; continue; }
        if (section === 'Example') {
            if (!exampleMode) { out += '```a\n'; exampleMode = true; }
            out += line + '\n';
        } else if ((section === 'Args' || section === 'Parameters') && !line.startsWith('- ') && !line.startsWith('* ')) {
            out += `- ${line}\n`;
        } else {
            out += line + '\n';
        }
    }
    if (exampleMode) out += '```\n';
    return out.replace(/\n+$/, '');
}

function richHover(markdown, range) {
    const md = new vscode.MarkdownString();
    md.appendMarkdown(markdown);
    md.supportThemeIcons = false;
    return new vscode.Hover(md, range);
}

function buildRichHover(signature, documentation, example, seeAlso, range) {
    let text = '```a\n' + formatSignature(signature) + '\n```';
    const body = renderDocSections(documentation);
    if (body) text += '\n\n' + body;
    if (example) text += '\n\n**Example:**\n\n```a\n' + example + '\n```';
    if (seeAlso) text += '\n\n**See Also:** ' + seeAlso.split(',').map(s => '`' + s.trim() + '`').filter(s => s !== '``').join(', ');
    return richHover(text, range);
}

function fallbackHoverProvider() {
    return {
        provideHover(document, position) {
            const range = document.getWordRangeAtPosition(position, /[A-Za-z_]\w*/);
            if (!range) return null;
            const name = document.getText(range);
            const line = document.lineAt(position).text;
            // Local declarations first: signature line + ## doc comment above.
            const decl = declarations(document.getText()).find(d => d.name === name);
            if (decl) {
                const declLine = document.lineAt(document.positionAt(decl.start).line).text.trim();
                // Reconstruct a Pyright-style signature from the decl line.
                const header = /^fn\s+\w+\s*\((.*)\)\s*(->\s*\S+)?/.exec(declLine);
                const sig = header
                    ? `fn ${name}(${header[1]})${header[2] ? ' ' + header[2] : ''}`
                    : declLine.replace(/:\s*$/, '');
                const lines = document.getText().split('\n');
                const docLines = [];
                for (let i = document.positionAt(decl.start).line - 1; i >= 0; i--) {
                    const m = /^\s*##\s?(.*)$/.exec(lines[i] || '');
                    if (!m) break;
                    docLines.unshift(m[1].replace(/\s+$/, ''));
                }
                const hover = buildRichHover(sig, docLines.join('\n'), null, null, range);
                return hover;
            }
            // module.member
            const mm = new RegExp(`\\b([A-Za-z_]\\w*)\\.${name}\\b`).exec(line);
            if (mm && STDLIB[mm[1]]) {
                const mod = STDLIB[mm[1]];
                if ((mod.members || []).includes(name)) {
                    const sig = BUILTIN_SIGS[name] ? BUILTIN_SIGS[name].signature.replace(/^(\w+)/, `${mm[1]}.$1`) : `${mm[1]}.${name}`;
                    const info = BUILTIN_SIGS[name];
                    return buildRichHover(sig, info ? info.documentation : (mod.doc || ''), info && info.example, info && info.seeAlso, range);
                }
            }
            if (STDLIB[name]) {
                const mod = STDLIB[name];
                let text = '```a\nimport ' + name + '\n```\n\n' + (mod.doc || '');
                text += '\n\nMembers: ' + (mod.members || []).slice(0, 16).map(m => '`' + m + '`').join(', ') + ((mod.members || []).length > 16 ? ', ...' : '');
                return richHover(text, range);
            }
            if (BUILTIN_SIGS[name] || BUILTINS.includes(name)) {
                const info = BUILTIN_SIGS[name];
                if (info) return buildRichHover(info.signature, info.documentation, info.example, info.seeAlso, range);
                return richHover('```a\n' + name + '\n```\n\nA builtin function/value (fallback hover; use `a-lsp` for precise signatures).', range);
            }
            return null;
        }
    };
}

// ---------------- diagnostics / check-on-save queue (kept compatible) ----------------

class CheckQueue {
    constructor(output, diagnostics) {
        this.output = output;
        this.diagnostics = diagnostics;
        this.jobs = new Map();
        this.results = new Map();
        this.waiting = new Map();
        this.checked = new Set();
        this.onStateChanged = () => {};
    }
    stale(cwd) {
        for (const map of [this.jobs, this.results]) {
            for (const item of map.values()) if (item.config.cwd === cwd) item.stale = true;
        }
        this.onStateChanged();
    }
    invalidate() {
        for (const job of this.jobs.values()) { if (job.timer) clearTimeout(job.timer); if (job.kill) job.kill(); }
        this.jobs.clear();
        this.waiting.clear();
        this.results.clear();
        this.checked.clear();
        this.diagnostics.clear();
        this.onStateChanged();
    }
    async schedule(config, delay = 250) {
        if (!config.checkOnSave || !vscode.workspace.isTrusted) return undefined;
        const key = config.cwd;
        if (this.jobs.has(key)) {
            const existing = this.jobs.get(key);
            if (existing.timer) clearTimeout(existing.timer);
            existing.config = config;
            return new Promise(resolve => {
                existing.timer = setTimeout(async () => resolve(await this.run(key)), delay);
            });
        }
        return new Promise(resolve => {
            this.jobs.set(key, { config, resolve, timer: setTimeout(async () => resolve(await this.run(key)), delay) });
        });
    }
    async run(key) {
        const job = this.jobs.get(key);
        if (!job) return undefined;
        job.timer = undefined;
        job.startedAt = Date.now();
        this.onStateChanged();
        const config = job.config;
        const result = await runCheck(config, this.output, (kill) => { if (this.jobs.get(key) === job) job.kill = kill; });
        this.jobs.delete(key);
        if (!result.cancelled) {
            this.results.set(key, { config, result, stale: false, startedAt: job.startedAt });
            this.checked.add(config.file);
            applyDiagnostics(this.diagnostics, result, config);
        }
        this.onStateChanged();
        if (job.resolve) job.resolve(result);
        return result;
    }
}

async function runCheck(config, output, setKill) {
    const cp = require('node:child_process');
    return await new Promise(resolve => {
        const args = argumentsFor('check', config, { json: true });
        output.appendLine(`> ${config.compiler} ${args.map(a => JSON.stringify(a)).join(' ')}`);
        const child = cp.spawn(config.compiler, args, { cwd: config.cwd, windowsHide: true });
        if (setKill) setKill(() => { try { child.kill(); } catch {} });
        let out = '', err = '', done = false;
        const finish = (payload) => { if (done) return; done = true; resolve(payload); };
        const timer = setTimeout(() => { try { child.kill(); } catch {} finish({ ok: false, cancelled: false, timeout: true, diagnostics: [], error: `Timed out after ${config.timeout} ms.` }); }, config.timeout);
        child.stdout.on('data', d => out += d.toString('utf8'));
        child.stderr.on('data', d => err += d.toString('utf8'));
        child.on('error', error => { clearTimeout(timer); finish({ ok: false, cancelled: false, diagnostics: [], error: error.message }); });
        child.on('close', code => {
            clearTimeout(timer);
            let diagnostics = [];
            try { diagnostics = JSON.parse(out || '[]'); } catch { diagnostics = parseTextDiagnostics(out + '\n' + err, config.cwd); }
            if (err.trim()) output.appendLine(err.trim());
            finish({ ok: code === 0, cancelled: false, diagnostics, code, error: code === 0 ? '' : failureMessage(err || out) });
        });
    });
}

function parseTextDiagnostics(text, cwd) {
    const diagnostics = [];
    const lines = String(text || '').split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
        const m = /^(error|warning)(?:\[[^\]]*\])?:\s*(.*)$/.exec(lines[i]);
        if (!m) continue;
        const loc = /^\s*-->+\s+(.+?):(\d+):(\d+)/.exec(lines[i + 1] || '');
        diagnostics.push({ severity: m[1], message: m[2], file: loc ? path.resolve(cwd, loc[1]) : '', line: loc ? Number(loc[2]) : 1, column: loc ? Number(loc[3]) : 1 });
    }
    return diagnostics;
}

function applyDiagnostics(collection, result, config) {
    const byFile = new Map();
    for (const item of result.diagnostics || []) {
        const file = item.file ? (path.isAbsolute(item.file) ? item.file : path.resolve(config.cwd, item.file)) : config.file;
        if (!byFile.has(file)) byFile.set(file, []);
        const line = Math.max(0, (item.line || 1) - 1);
        const col = Math.max(0, (item.column || 1) - 1);
        const d = new vscode.Diagnostic(new vscode.Range(line, col, line, col + 1), item.message || 'A check failed', /warn/i.test(item.severity || '') ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error);
        d.source = 'a';
        byFile.get(file).push(d);
    }
    collection.clear();
    for (const [file, list] of byFile) collection.set(vscode.Uri.file(file), list);
    if (!byFile.size && result.ok) collection.clear();
}

async function activate(context) {
    let disposed = false;
    let fallback = undefined;
    let stopOnDeactivate = async () => {};
    const output = vscode.window.createOutputChannel('A');
    const checkDiagnostics = vscode.languages.createDiagnosticCollection('a-check');
    const queue = new CheckQueue(output, checkDiagnostics);
    const server = { status: 'off' };
    const openedDocuments = new Set();
    const changeTimers = new Map();
    const checkStatus = vscode.window.createStatusBarItem('a.check', vscode.StatusBarAlignment.Left, 4);
    checkStatus.name = 'A: Check Status';
    checkStatus.command = 'a.showOutput';

    function updateCheckStatus() {
        const active = vscode.window.activeTextEditor && isA(vscode.window.activeTextEditor.document) ? vscode.window.activeTextEditor.document : undefined;
        if (!active || !vscode.workspace.isTrusted) { checkStatus.hide(); return; }
        if (server.status === 'starting') { checkStatus.text = '$(sync~spin) A'; checkStatus.tooltip = 'Starting a-lsp'; checkStatus.show(); return; }
        if (server.status === 'running') { checkStatus.text = '$(check) A'; checkStatus.tooltip = 'A language server active (a-lsp). Fallback completion/hover enabled only when server is off.'; checkStatus.show(); return; }
        checkStatus.text = '$(search) A'; checkStatus.tooltip = 'Using fallback check-on-save + lexical/stdlib completions'; checkStatus.show();
    }
    queue.onStateChanged = updateCheckStatus;

    function recheck() {
        for (const doc of vscode.workspace.textDocuments) if (isA(doc) && !doc.isDirty) queue.schedule(settings(vscode, doc.uri), 0);
    }

    function startFallback() {
        if (fallback || disposed) return;
        output.appendLine('> a-lsp is not in use (missing, disabled by a.useLanguageServer, or workspace untrusted).');
        output.appendLine('> Using compiler check-on-save plus lexical/curated stdlib completion and hover fallback.');
        const watcher = vscode.workspace.createFileSystemWatcher('**/*.a');
        const declCache = new Map();
        async function fileInfo(uri) {
            const key = uri.toString();
            if (declCache.has(key)) return declCache.get(key);
            let info = { text: '', decls: [] };
            try {
                const open = vscode.workspace.textDocuments.find(doc => doc.uri.toString() === key);
                const text = open ? open.getText() : Buffer.from(await vscode.workspace.fs.readFile(uri)).toString('utf8');
                info = { text, decls: declarations(text) };
            } catch {}
            declCache.set(key, info); return info;
        }
        function nameLocation(document, decl) {
            return new vscode.Location(document.uri, new vscode.Range(document.positionAt(decl.nameStart), document.positionAt(decl.nameEnd)));
        }
        function offsetRange(text, start, end) {
            const pos = offset => { const before = text.slice(0, offset); const parts = before.split('\n'); return { line: parts.length - 1, character: parts[parts.length - 1].length }; };
            const s = pos(start), e = pos(end); return new vscode.Range(s.line, s.character, e.line, e.character);
        }
        const changedOnDisk = async uri => {
            const cwd = settings(vscode, uri).cwd;
            let modified = Infinity;
            try { modified = (await vscode.workspace.fs.stat(uri)).mtime; } catch {}
            if (disposed || !fallback) return;
            const existing = [...queue.results.values(), ...queue.jobs.values()].filter(item => item.config.cwd === cwd && !item.stale);
            if (existing.length && existing.every(item => item.startedAt === undefined || item.startedAt > modified)) return;
            declCache.clear(); recheck();
        };
        fallback = vscode.Disposable.from(
            vscode.languages.registerDocumentSymbolProvider(A_SELECTOR, {
                provideDocumentSymbols(doc) {
                    return declarations(doc.getText()).map(item => new vscode.DocumentSymbol(item.name, item.detail, symbolKind(item.kind), new vscode.Range(doc.positionAt(item.start), doc.positionAt(item.end)), new vscode.Range(doc.positionAt(item.nameStart), doc.positionAt(item.nameEnd))));
                }
            }),
            vscode.languages.registerDocumentLinkProvider(A_DOC, {
                async provideDocumentLinks(doc, token) {
                    const config = settings(vscode, doc.uri); const links = [];
                    for (const item of imports(doc.getText())) {
                        const candidates = item.quoted ? quotedImportCandidates(item.spec, doc.uri.fsPath, config) : bareImportCandidates(item.spec, config);
                        for (const candidate of candidates) {
                            if (token.isCancellationRequested) return [];
                            try {
                                const uri = vscode.Uri.file(candidate); const stat = await vscode.workspace.fs.stat(uri);
                                if (!(stat.type & vscode.FileType.File)) continue;
                                const range = new vscode.Range(new vscode.Position(item.line, item.startChar), new vscode.Position(item.line, item.endChar));
                                const link = new vscode.DocumentLink(range, uri); link.tooltip = 'Open A module'; links.push(link); break;
                            } catch {}
                        }
                    }
                    return links;
                }
            }),
            vscode.languages.registerDefinitionProvider(A_DOC, {
                async provideDefinition(document, position, token) {
                    const word = document.getWordRangeAtPosition(position, /[A-Za-z_]\w*/); if (!word) return null;
                    const name = document.getText(word); const config = settings(vscode, document.uri);
                    for (const item of imports(document.getText())) {
                        if (position.line !== item.line || position.character < item.startChar || position.character > item.endChar) continue;
                        const candidates = item.quoted ? quotedImportCandidates(item.spec, document.uri.fsPath, config) : bareImportCandidates(item.spec, config);
                        for (const candidate of candidates) {
                            if (token.isCancellationRequested) return null;
                            try { const uri = vscode.Uri.file(candidate); const stat = await vscode.workspace.fs.stat(uri); if (stat.type & vscode.FileType.File) return new vscode.Location(uri, new vscode.Range(0,0,0,0)); } catch {}
                        }
                    }
                    const local = declarations(document.getText()).filter(decl => decl.name === name);
                    if (local.length) return local.map(decl => nameLocation(document, decl));
                    const files = await vscode.workspace.findFiles('**/*.a', '{**/node_modules/**,**/.git/**,**/build/**,**/build-*/**}');
                    const locations = [];
                    for (const uri of files) {
                        if (token.isCancellationRequested || locations.length >= 50) return locations.length ? locations : null;
                        if (uri.toString() === document.uri.toString()) continue;
                        const { text, decls } = await fileInfo(uri);
                        for (const decl of decls) if (decl.name === name) locations.push(new vscode.Location(uri, offsetRange(text, decl.nameStart, decl.nameEnd)));
                    }
                    return locations.length ? locations : null;
                }
            }),
            vscode.languages.registerCompletionItemProvider(A_SELECTOR, fallbackCompletionProvider(), '.', '"', '(', ','),
            vscode.languages.registerHoverProvider(A_SELECTOR, fallbackHoverProvider()),
            vscode.workspace.onDidOpenTextDocument(doc => { if (isA(doc)) queue.schedule(settings(vscode, doc.uri), 0); }),
            vscode.workspace.onDidSaveTextDocument(doc => { if (isA(doc)) { declCache.clear(); queue.schedule(settings(vscode, doc.uri), 0); } }),
            vscode.workspace.onDidChangeTextDocument(event => { if (isA(event.document) && event.contentChanges.length) { declCache.clear(); queue.stale(cwdOf(event.document)); } }),
            vscode.workspace.onDidCloseTextDocument(doc => { if (isA(doc)) updateCheckStatus(); }),
            watcher, watcher.onDidCreate(changedOnDisk), watcher.onDidChange(changedOnDisk), watcher.onDidDelete(changedOnDisk)
        );
        recheck(); updateCheckStatus();
    }

    function stopFallback() {
        if (!fallback) return;
        fallback.dispose(); fallback = undefined;
        queue.invalidate(); checkDiagnostics.clear();
    }

    async function stopServer() {
        const client = server.client;
        server.status = 'off'; server.client = undefined;
        openedDocuments.clear();
        for (const timer of changeTimers.values()) clearTimeout(timer);
        changeTimers.clear();
        if (client) { try { await client.stop(); } catch {} }
        updateCheckStatus();
    }
    stopOnDeactivate = stopServer;

    async function startServer(setup) {
        await stopServer(); if (disposed) return;
        const client = new LanguageClient(setup.path, setup.args, { modulePaths: setup.modulePaths, stdlibRoot: setup.stdlib, jobs: setup.jobs }, output);
        client.onCheckingChanged = () => updateCheckStatus();
        server.status = 'starting'; server.client = client; server.setup = setup; updateCheckStatus();
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
            startFallback();
        }
        updateCheckStatus();
    }

    let deciding = Promise.resolve();
    function decide(restart = false) {
        deciding = deciding.then(async () => {
            if (disposed) return;
            const setup = serverSetup();
            const runnable = setup.path && (isExecutable(setup.path) || !/[\\/]/.test(setup.path));
            const wanted = vscode.workspace.isTrusted && setup.enabled && runnable;
            if (!wanted) { await stopServer(); startFallback(); return; }
            if (!restart && server.client && JSON.stringify(server.setup) === JSON.stringify(setup)) return;
            stopFallback(); await startServer(setup);
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
        const compiler = compilerPath(config.get('compilerPath', ''), cwd);
        const beside = compiler && /[\\/]/.test(compiler) ? path.join(path.dirname(compiler), process.platform === 'win32' ? 'a-lsp.exe' : 'a-lsp') : '';
        const jobsRaw = Number(config.get('jobs', 0));
        const mod = require('./config');
        return { enabled: config.get('useLanguageServer', true), path: serverPath(configured, cwd, context.extensionPath) || (beside && isExecutable(beside) ? beside : ''), args: Array.isArray(config.get('server.args', [])) ? config.get('server.args', []) : [], modulePaths: mod.modulePaths(config, cwd, ''), stdlib: stdlibRaw ? absolute(stdlibRaw, cwd, '') : '', jobs: Number.isFinite(jobsRaw) && jobsRaw >= 0 ? Math.trunc(jobsRaw) : 0 };
    }

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

    function makeTask(definition, uri, scope, name) {
        if (!vscode.workspace.isTrusted || uri.scheme !== 'file') return undefined;
        const config = settings(vscode, uri, definition, typeof scope === 'object' ? scope : undefined);
        if (!ACTION_TITLES[definition.action]) return undefined;
        const task = new vscode.Task({ ...definition, type: 'a', file: config.file }, scope || config.workspaceFolder || vscode.TaskScope.Workspace, name || ACTION_TITLES[definition.action], 'A', new vscode.ProcessExecution(config.compiler, argumentsFor(definition.action, config), { cwd: config.cwd }), '$a');
        if (definition.action === 'run') task.group = vscode.TaskGroup.Build;
        task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: true };
        return task;
    }

    const runStatus = vscode.window.createStatusBarItem('a.run', vscode.StatusBarAlignment.Left, 5);
    runStatus.name = 'A: Run Program'; runStatus.text = '$(play) Run'; runStatus.tooltip = 'Compile and run the A program'; runStatus.command = 'a.run';
    function updateRunStatus() { if (vscode.workspace.isTrusted && vscode.window.activeTextEditor && isA(vscode.window.activeTextEditor.document)) runStatus.show(); else runStatus.hide(); }

    // register LSP providers (only active when server running)
    function getClient() { return server.status === 'running' ? server.client : undefined; }
    registerProviders(getClient);

    context.subscriptions.push(
        checkDiagnostics, output, runStatus, checkStatus,
        { dispose() { disposed = true; stopFallback(); queue.invalidate(); void stopServer(); } },
        vscode.window.onDidChangeActiveTextEditor(() => { updateRunStatus(); updateCheckStatus(); }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => { updateRunStatus(); updateCheckStatus(); void decide(); }),
        vscode.commands.registerCommand('a.showOutput', () => output.show()),
        vscode.commands.registerCommand('a.restartServer', async () => { await decide(true); vscode.window.showInformationMessage('A language server restarted.'); }),
        vscode.commands.registerCommand('a.check', async () => {
            const doc = await savedDocument(); if (!doc) return;
            if (server.status !== 'off') { await vscode.tasks.executeTask(makeTask({ type: 'a', action: 'check' }, doc.uri)); return; }
            const completed = await queue.schedule(settings(vscode, doc.uri), 0); if (completed) output.show(true);
        }),
        vscode.commands.registerCommand('a.run', async target => {
            const uri = typeof target === 'string' ? vscode.Uri.parse(target) : target instanceof vscode.Uri ? target : undefined;
            const doc = await savedDocument(uri); if (!doc) return;
            const definition = { type: 'a', action: 'run' }; if (uri) definition.file = doc.uri.fsPath;
            await vscode.tasks.executeTask(makeTask(definition, doc.uri));
        }),
        ...['build', 'test', 'fmt'].map(action => vscode.commands.registerCommand(`a.${action}`, async () => { const doc = await savedDocument(); if (doc) await vscode.tasks.executeTask(makeTask({ type: 'a', action }, doc.uri)); })),
        vscode.tasks.registerTaskProvider('a', {
            provideTasks() {
                if (!vscode.workspace.isTrusted) return [];
                const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
                if (doc && isA(doc)) return Object.keys(ACTION_TITLES).map(action => makeTask({ type: 'a', action }, doc.uri));
                return (vscode.workspace.workspaceFolders || []).flatMap(folder => {
                    const entry = vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', ''); if (!entry) return [];
                    const uri = vscode.Uri.file(absolute(entry, folder.uri.fsPath, ''));
                    return Object.keys(ACTION_TITLES).map(action => makeTask({ type: 'a', action }, uri, folder));
                });
            },
            resolveTask(task) {
                if (!vscode.workspace.isTrusted) return undefined;
                const folder = typeof task.scope === 'object' ? task.scope : undefined;
                const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
                const base = folder && folder.uri.fsPath; const file = task.definition.file;
                const configuredEntry = folder && vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', '');
                const uri = file && base && !file.includes('${file}') ? vscode.Uri.file(absolute(file, base, '')) : active && isA(active) && (!base || cwdOf(active) === base) ? active.uri : configuredEntry ? vscode.Uri.file(absolute(configuredEntry, base || '', '')) : undefined;
                if (!uri) return undefined;
                const resolved = makeTask(task.definition, uri, task.scope, task.name);
                if (resolved) { resolved.definition = task.definition; resolved.presentationOptions = task.presentationOptions; if (task.group) resolved.group = task.group; resolved.problemMatchers = task.problemMatchers.length ? task.problemMatchers : ['$a']; }
                return resolved;
            }
        }),
        vscode.workspace.onDidChangeConfiguration(event => { if (!event.affectsConfiguration('a')) return; if (fallback) { queue.invalidate(); recheck(); } void decide(); }),
        vscode.workspace.onDidChangeWorkspaceFolders(() => { if (fallback) { queue.invalidate(); recheck(); } void decide(); }),
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
            changeTimers.set(key, setTimeout(() => { changeTimers.delete(key); const c = getClient(); if (c && openedDocuments.has(key)) { if (captured && captured.range) c.changeDocument(event.document, captured.range, captured.text); else c.changeDocument(event.document); } }, 250));
        }),
        vscode.workspace.onDidSaveTextDocument(document => {
            const active = getClient(); if (!active || !isA(document)) return;
            const key = document.uri.toString(); const timer = changeTimers.get(key); if (timer) { clearTimeout(timer); changeTimers.delete(key); }
            if (!openedDocuments.has(key)) { openedDocuments.add(key); active.openDocument(document); }
            active.saveDocument(document);
        }),
        vscode.workspace.onDidCloseTextDocument(document => {
            const active = getClient(); if (!isA(document)) return;
            const key = document.uri.toString(); const timer = changeTimers.get(key); if (timer) clearTimeout(timer); changeTimers.delete(key);
            if (active && openedDocuments.delete(key)) active.closeDocument(document);
        })
    );
    updateRunStatus(); updateCheckStatus(); await decide();
}

function deactivate() { return stopOnDeactivate ? stopOnDeactivate() : Promise.resolve(); }

// ---- minimal provider registration for LSP mode (kept from previous extension) ----
function registerProviders(getClient) {
    const selector = { language: 'a', scheme: 'file' };
    function toRange(range) { if (!range) return new vscode.Range(0,0,0,0); return new vscode.Range(range.start.line, range.start.character, range.end.line, range.end.character); }
    function locationsFrom(result) {
        if (!result) return null; const entries = Array.isArray(result) ? result : [result]; const locations = [];
        for (const entry of entries) { if (!entry) continue; const uri = entry.uri || entry.targetUri; const range = entry.range || entry.targetSelectionRange || entry.targetRange; if (!uri || !range) continue; locations.push(new vscode.Location(vscode.Uri.parse(uri), toRange(range))); }
        return locations.length ? locations : null;
    }
    function hoverContents(contents) {
        if (!contents) return new vscode.MarkdownString(''); if (Array.isArray(contents)) { const md = new vscode.MarkdownString(); for (const p of contents) { const v = typeof p === 'string' ? p : p && p.value; if (v) md.appendMarkdown(v + '\n\n'); } return md; } if (typeof contents === 'string') return new vscode.MarkdownString(contents); return new vscode.MarkdownString(contents.value || '');
    }
    function documentationToMarkdown(documentation) { if (!documentation) return undefined; if (typeof documentation === 'string') return new vscode.MarkdownString(documentation); return new vscode.MarkdownString(documentation.value || ''); }
    function toCompletionKind(kind) { const value = typeof kind === 'number' ? kind - 1 : 0; return Math.max(0, Math.min(24, value)); }
    function toSymbolKind(kind) { const value = typeof kind === 'number' ? kind - 1 : 0; return Math.max(0, Math.min(25, value)); }
    function convertDocumentSymbol(symbol) { const c = new vscode.DocumentSymbol(symbol.name, symbol.detail || '', toSymbolKind(symbol.kind), toRange(symbol.range), toRange(symbol.selectionRange)); if (Array.isArray(symbol.children) && symbol.children.length) c.children = symbol.children.map(convertDocumentSymbol); return c; }
    function toTextEdit(edit) { return new vscode.TextEdit(toRange(edit.range), edit.newText || ''); }
    function workspaceEditFromLsp(edit) { const we = new vscode.WorkspaceEdit(); if (!edit) return we; if (edit.changes) for (const [uri, edits] of Object.entries(edit.changes)) we.set(vscode.Uri.parse(uri), (edits||[]).map(toTextEdit)); if (Array.isArray(edit.documentChanges)) for (const c of edit.documentChanges) if (c && c.textDocument && Array.isArray(c.edits)) we.set(vscode.Uri.parse(c.textDocument.uri), c.edits.map(toTextEdit)); return we; }
    const A_LEGEND = new vscode.SemanticTokensLegend(['namespace','type','class','enum','function','method','parameter','variable','property','keyword','comment','string','number','operator'], ['declaration','readonly','static']);
    function semanticTokensFrom(data) { const b = new vscode.SemanticTokensBuilder(A_LEGEND); let line=-1, ch=0; for (let i=0;i+4<data.length;i+=5){ const dl=data[i],dc=data[i+1],len=data[i+2],type=data[i+3],mods=data[i+4]; if (dl>0){line+=dl;ch=dc;} else ch+=dc; while(line<0)line=0; b.push(new vscode.Range(line,ch,line,ch+len), type, mods&1?['declaration']:mods&2?['readonly']:mods&4?['static']:[]);} return b.build(); }

    const subs = [
        vscode.languages.registerDefinitionProvider(selector, { async provideDefinition(document, position, token) { const a=getClient(); if(!a) return null; return locationsFrom(await a.request('textDocument/definition',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}},token)); } }),
        vscode.languages.registerDeclarationProvider(selector, { async provideDeclaration(document, position, token) { const a=getClient(); if(!a) return null; return locationsFrom(await a.request('textDocument/declaration',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}},token)); } }),
        vscode.languages.registerTypeDefinitionProvider(selector, { async provideTypeDefinition(document, position, token) { const a=getClient(); if(!a) return null; return locationsFrom(await a.request('textDocument/typeDefinition',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}},token)); } }),
        vscode.languages.registerImplementationProvider(selector, { async provideImplementation(document, position, token) { const a=getClient(); if(!a) return null; return locationsFrom(await a.request('textDocument/implementation',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}},token)); } }),
        vscode.languages.registerReferenceProvider(selector, { async provideReferences(document, position, refContext) { const a=getClient(); if(!a) return null; return locationsFrom(await a.request('textDocument/references',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character},context:{includeDeclaration:refContext.includeDeclaration}})); } }),
        vscode.languages.registerHoverProvider(selector, { async provideHover(document, position, token) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/hover',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}},token); if(!r||!r.contents) return null; return new vscode.Hover(hoverContents(r.contents), r.range?toRange(r.range):undefined); } }),
        vscode.languages.registerCompletionItemProvider(selector, {
            async provideCompletionItems(document, position, token) {
                const a=getClient(); if(!a) return null;
                const result = await a.request('textDocument/completion',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character},context:{triggerKind:1}},token);
                if(!result) return null; const entries = Array.isArray(result)?result:(result.items||[]);
                const items = entries.map(entry=>{ const it=new vscode.CompletionItem(entry.label,toCompletionKind(entry.kind)); if(entry.detail)it.detail=entry.detail; if(entry.documentation)it.documentation=documentationToMarkdown(entry.documentation); if(entry.insertTextFormat===2&&typeof entry.insertText==='string'){it.insertText=new vscode.SnippetString(entry.insertText);} else it.insertText=typeof entry.insertText==='string'?entry.insertText:entry.label; if(entry.sortText)it.sortText=entry.sortText; if(entry.filterText)it.filterText=entry.filterText; if(entry.data)it.data=entry.data; if(entry.preselect)it.preselect=entry.preselect; return it; });
                return new vscode.CompletionList(items, Boolean(result.isIncomplete));
            },
            async resolveCompletionItem(item, token) { const a=getClient(); if(!a||!item.data) return item; const r=await a.request('completionItem/resolve',{label:item.label,data:item.data},token); if(r&&r.documentation) item.documentation=documentationToMarkdown(r.documentation); return item; }
        }, '.', '"', '(', ',', ':', '<'),
        vscode.languages.registerSignatureHelpProvider(selector, { async provideSignatureHelp(document, position) { const a=getClient(); if(!a) return null; return signatureHelpFrom(await a.request('textDocument/signatureHelp',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}})); } }),
        vscode.languages.registerDocumentSymbolProvider(selector, { async provideDocumentSymbols(document) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/documentSymbol',{textDocument:{uri:document.uri.toString()}}); if(!Array.isArray(r)) return null; return r.map(convertDocumentSymbol); } }),
        vscode.languages.registerDocumentFormattingEditProvider(selector, { async provideDocumentFormattingEdits(document, options) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/formatting',{textDocument:{uri:document.uri.toString()},options:{tabSize:options.tabSize,insertSpaces:options.insertSpaces}}); if(!Array.isArray(r)) return null; return r.map(toTextEdit); } }),
        vscode.languages.registerRenameProvider(selector, { async provideRenameEdits(document, position, newName, token) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/rename',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character},newName},token); if(!r) return null; return workspaceEditFromLsp(r); } }),
        vscode.languages.registerDocumentHighlightProvider(selector, { async provideDocumentHighlights(document, position) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/documentHighlight',{textDocument:{uri:document.uri.toString()},position:{line:position.line,character:position.character}}); if(!Array.isArray(r)) return null; return r.map(e=>new vscode.DocumentHighlight(toRange(e.range), Math.max(0,Math.min(2,(e.kind||1)-1)))); } }),
        vscode.languages.registerFoldingRangeProvider(selector, { async provideFoldingRanges(document) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/foldingRange',{textDocument:{uri:document.uri.toString()}}); if(!Array.isArray(r)) return null; return r.map(e=>new vscode.FoldingRange(e.startLine,e.endLine,e.kind==='comment'?vscode.FoldingRangeKind.Comment:e.kind==='imports'?vscode.FoldingRangeKind.Imports:vscode.FoldingRangeKind.Region)); } }),
        vscode.languages.registerWorkspaceSymbolProvider({ async provideWorkspaceSymbols(query) { const a=getClient(); if(!a) return null; const r=await a.request('workspace/symbol',{query}); if(!Array.isArray(r)) return null; return r.filter(e=>e&&e.location).map(e=>new vscode.SymbolInformation(e.name,toSymbolKind(e.kind),e.containerName||'A',new vscode.Location(vscode.Uri.parse(e.location.uri),toRange(e.location.range)))); } }),
        vscode.languages.registerSemanticTokensProvider(selector, { async provideDocumentSemanticTokens(document, token) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/semanticTokens/full',{textDocument:{uri:document.uri.toString()}},token); if(!r||!Array.isArray(r.data)) return null; return semanticTokensFrom(r.data); } }, A_LEGEND),
        vscode.languages.registerInlayHintsProvider(selector, { async provideInlayHints(document, range, token) { const a=getClient(); if(!a) return null; const r=await a.request('textDocument/inlayHint',{textDocument:{uri:document.uri.toString()},range:{start:{line:range.start.line,character:range.start.character},end:{line:range.end.line,character:range.end.character}}},token); if(!Array.isArray(r)) return null; return r.map(e=>{ const h=new vscode.InlayHint(new vscode.Position(e.position.line,e.position.character),e.label,e.kind===1?vscode.InlayHintKind.Type:e.kind===2?vscode.InlayHintKind.Parameter:undefined); h.paddingLeft=Boolean(e.paddingLeft); h.paddingRight=Boolean(e.paddingRight); if(e.tooltip)h.tooltip=typeof e.tooltip==='string'?e.tooltip:e.tooltip.value; return h; }); } }),
    ];
    // attach to global context via side-effect in activate? easier: push to module not available; activate pushes nothing here but providers live while extension active in VS Code; we'll store globally is not necessary — VS Code disposes on deactivate? not directly. Keep simple: rely on extension host lifecycle.
    global.__aProviderSubs = subs;
}

function signatureHelpFrom(result) {
    if (!result || !Array.isArray(result.signatures) || !result.signatures.length) return null;
    const signatures = result.signatures.map(entry => { const s=new vscode.SignatureInformation(entry.label); if(entry.documentation) s.documentation = typeof entry.documentation==='string'? new vscode.MarkdownString(entry.documentation): new vscode.MarkdownString(entry.documentation.value||''); if(Array.isArray(entry.parameters)) s.parameters=entry.parameters.map(p=>{ const pi=new vscode.ParameterInformation(p.label); if(p.documentation) pi.documentation=typeof p.documentation==='string'?new vscode.MarkdownString(p.documentation):new vscode.MarkdownString(p.documentation.value||''); return pi; }); return s; });
    const help = new vscode.SignatureHelp(signatures); help.activeSignature=Math.max(0,Math.min(result.activeSignature||0,signatures.length-1)); const active=signatures[help.activeSignature]; if(active&&typeof result.activeParameter==='number') help.activeParameter=Math.max(0,Math.min(result.activeParameter,(active.parameters||[]).length-1)); return help;
}

function symbolKind(name) {
    switch (name) { case 'Function': return vscode.SymbolKind.Function; case 'Struct': return vscode.SymbolKind.Struct; case 'Class': return vscode.SymbolKind.Class; case 'Enum': return vscode.SymbolKind.Enum; case 'Interface': return vscode.SymbolKind.Interface; case 'TypeParameter': return vscode.SymbolKind.TypeParameter; default: return vscode.SymbolKind.Variable; }
}
function failureMessage(output) { return String(output || '').replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/).join('\n').trim(); }

module.exports = { activate, deactivate };
