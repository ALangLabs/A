'use strict';

// Fallback language features used when a-lsp is missing, disabled, or the
// workspace is untrusted: lexical document symbols, import document links,
// workspace-wide definition search, curated stdlib completions and hover,
// plus the check-on-save wiring that drives the CheckQueue. Everything here
// is best-effort; a-lsp remains the authoritative intelligence.

const vscode = require('vscode');
const path = require('node:path');
const { declarations, imports } = require('../language');
const { STDLIB, KEYWORDS, BUILTINS, BUILTIN_SIGS } = require('../stdlib-data');
const {
    settings, quotedImportCandidates, bareImportCandidates
} = require('../config');
const { A_SELECTOR, A_DOC, isA, cwdOf, declSymbolKind } = require('./util');

// ------------------------------------------------------- completion/hover

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
                const it = new vscode.CompletionItem(d.name, declSymbolKind(d.kind));
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
                return buildRichHover(sig, docLines.join('\n'), null, null, range);
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

// ------------------------------------------------------- lifecycle wiring

// env: { output, queue, checkDiagnostics, recheck, updateCheckStatus,
//        isDisposed() }
function createFallbackController(env) {
    let fallback = undefined;
    const { output, queue, checkDiagnostics } = env;

    function start() {
        if (fallback || env.isDisposed()) return;
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
            if (env.isDisposed() || !fallback) return;
            const existing = [...queue.results.values(), ...queue.jobs.values()].filter(item => item.config.cwd === cwd && !item.stale);
            if (existing.length && existing.every(item => item.startedAt === undefined || item.startedAt > modified)) return;
            declCache.clear(); env.recheck();
        };
        fallback = vscode.Disposable.from(
            vscode.languages.registerDocumentSymbolProvider(A_SELECTOR, {
                provideDocumentSymbols(doc) {
                    return declarations(doc.getText()).map(item => new vscode.DocumentSymbol(item.name, item.detail, declSymbolKind(item.kind), new vscode.Range(doc.positionAt(item.start), doc.positionAt(item.end)), new vscode.Range(doc.positionAt(item.nameStart), doc.positionAt(item.nameEnd))));
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
            vscode.workspace.onDidCloseTextDocument(doc => { if (isA(doc)) env.updateCheckStatus(); }),
            watcher, watcher.onDidCreate(changedOnDisk), watcher.onDidChange(changedOnDisk), watcher.onDidDelete(changedOnDisk)
        );
        env.recheck(); env.updateCheckStatus();
    }

    function stop() {
        if (!fallback) return;
        fallback.dispose(); fallback = undefined;
        queue.invalidate(); checkDiagnostics.clear();
    }

    return {
        start,
        stop,
        get active() { return Boolean(fallback); }
    };
}

module.exports = { createFallbackController, fallbackCompletionProvider, fallbackHoverProvider };
