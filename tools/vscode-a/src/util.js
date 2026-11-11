'use strict';

// Shared helpers for the A extension: document predicates, LSP<->VS Code
// conversions, and small utilities used by several modules. Plain Node +
// the vscode API only; nothing here may assume a running server.

const vscode = require('vscode');
const path = require('node:path');

const A_DOC = { language: 'a', scheme: 'file' };
const A_SELECTOR = [A_DOC, { language: 'a', scheme: 'untitled' }];

function isA(doc) { return doc && doc.languageId === 'a'; }
function cwdOf(doc) { return path.dirname(doc.uri.fsPath); }

function shellQuote(arg) {
    const s = String(arg);
    if (!s) return "''";
    if (!/[ \t$"'`\\;|&<>(){}\[\]*?!#]/.test(s)) return s;
    return '"' + s.replace(/(["\\$`])/g, '\\$1') + '"';
}

// ------------------------------------------------------------- ranges etc.

function toRange(range) {
    if (!range) return new vscode.Range(0, 0, 0, 0);
    // Tolerate sparse server payloads (e.g. selectionRange end:{}).
    const n = v => (Number.isFinite(v) ? v : 0);
    const s = range.start || {}, e = range.end || {};
    return new vscode.Range(n(s.line), n(s.character), n(e.line), n(e.character));
}

function toPosition(position) {
    if (!position) return new vscode.Position(0, 0);
    return new vscode.Position(position.line, position.character);
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

// ------------------------------------------------------------- kind maps

// VS Code SymbolKind is LSP SymbolKind + 1; clamp defensively.
function toSymbolKind(kind) {
    const value = typeof kind === 'number' ? kind - 1 : 0;
    return Math.max(0, Math.min(25, value));
}

function toCompletionKind(kind) {
    return toSymbolKind(kind);
}

// declaration() kinds (language.js) -> vscode.SymbolKind
function declSymbolKind(name) {
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

function convertDocumentSymbol(symbol) {
    const c = new vscode.DocumentSymbol(
        symbol.name, symbol.detail || '', toSymbolKind(symbol.kind),
        toRange(symbol.range), toRange(symbol.selectionRange));
    if (Array.isArray(symbol.children) && symbol.children.length) {
        c.children = symbol.children.map(convertDocumentSymbol);
    }
    return c;
}

// ------------------------------------------------------------- edits/hover

function toTextEdit(edit) { return new vscode.TextEdit(toRange(edit.range), edit.newText || ''); }

function workspaceEditFromLsp(edit) {
    const we = new vscode.WorkspaceEdit();
    if (!edit) return we;
    if (edit.changes) {
        for (const [uri, edits] of Object.entries(edit.changes)) {
            we.set(vscode.Uri.parse(uri), (edits || []).map(toTextEdit));
        }
    }
    if (Array.isArray(edit.documentChanges)) {
        for (const c of edit.documentChanges) {
            if (c && c.textDocument && Array.isArray(c.edits)) {
                we.set(vscode.Uri.parse(c.textDocument.uri), c.edits.map(toTextEdit));
            }
        }
    }
    return we;
}

function hoverContents(contents) {
    if (!contents) return new vscode.MarkdownString('');
    if (Array.isArray(contents)) {
        const md = new vscode.MarkdownString();
        for (const p of contents) {
            const v = typeof p === 'string' ? p : p && p.value;
            if (v) md.appendMarkdown(v + '\n\n');
        }
        return md;
    }
    if (typeof contents === 'string') return new vscode.MarkdownString(contents);
    return new vscode.MarkdownString(contents.value || '');
}

function documentationToMarkdown(documentation) {
    if (!documentation) return undefined;
    if (typeof documentation === 'string') return new vscode.MarkdownString(documentation);
    return new vscode.MarkdownString(documentation.value || '');
}

function signatureHelpFrom(result) {
    if (!result || !Array.isArray(result.signatures) || !result.signatures.length) return null;
    const signatures = result.signatures.map(entry => {
        const s = new vscode.SignatureInformation(entry.label);
        if (entry.documentation) s.documentation = typeof entry.documentation === 'string' ? new vscode.MarkdownString(entry.documentation) : new vscode.MarkdownString(entry.documentation.value || '');
        if (Array.isArray(entry.parameters)) {
            s.parameters = entry.parameters.map(p => {
                const pi = new vscode.ParameterInformation(p.label);
                if (p.documentation) pi.documentation = typeof p.documentation === 'string' ? new vscode.MarkdownString(p.documentation) : new vscode.MarkdownString(p.documentation.value || '');
                return pi;
            });
        }
        return s;
    });
    const help = new vscode.SignatureHelp(signatures);
    help.activeSignature = Math.max(0, Math.min(result.activeSignature || 0, signatures.length - 1));
    const active = signatures[help.activeSignature];
    if (active && typeof result.activeParameter === 'number') {
        help.activeParameter = Math.max(0, Math.min(result.activeParameter, (active.parameters || []).length - 1));
    }
    return help;
}

// ------------------------------------------------------------- hierarchies

function hierarchyItemFrom(item) {
    if (!item || !item.uri || !item.range) return null;
    const hi = new vscode.CallHierarchyItem(
        toSymbolKind(item.kind), item.name || '', item.detail || '',
        vscode.Uri.parse(item.uri), toRange(item.range), toRange(item.selectionRange || item.range));
    // Preserve the server's opaque `data` blob so follow-up requests work.
    hi.data = item.data;
    return hi;
}

function typeHierarchyItemFrom(item) {
    if (!item || !item.uri || !item.range) return null;
    const ti = new vscode.TypeHierarchyItem(
        toSymbolKind(item.kind), item.name || '', item.detail || '',
        vscode.Uri.parse(item.uri), toRange(item.range), toRange(item.selectionRange || item.range));
    ti.data = item.data;
    return ti;
}

// a-lsp returns {from, fromRanges, to} for both directions; map onto the
// VS Code shapes (incoming -> from is the caller, outgoing -> to is callee).
function callHierarchyIncomingFrom(entry) {
    const from = hierarchyItemFrom(entry && entry.from);
    if (!from) return null;
    return new vscode.CallHierarchyIncomingCall(from, rangesFrom(entry.fromRanges));
}

function callHierarchyOutgoingFrom(entry) {
    const to = hierarchyItemFrom(entry && entry.to);
    if (!to) return null;
    return new vscode.CallHierarchyOutgoingCall(to, rangesFrom(entry.fromRanges));
}

function rangesFrom(ranges) {
    return (Array.isArray(ranges) ? ranges : []).map(toRange);
}

// ------------------------------------------------------------- lenses etc.

function codeLensFrom(item) {
    if (!item || !item.range) return null;
    const cmd = item.command;
    if (!cmd || typeof cmd.command !== 'string') return null;
    let args = Array.isArray(cmd.arguments) ? cmd.arguments : [];
    if (cmd.command === 'editor.action.showReferences') {
        // The server sends plain JSON; the built-in command validates its
        // arguments and requires real Uri/Position instances.
        args = args.map((arg, i) => {
            if (i === 0 && typeof arg === 'string') {
                try { return vscode.Uri.parse(arg); } catch { return arg; }
            }
            if (i === 1 && arg && typeof arg === 'object' && Number.isFinite(arg.line)) {
                return new vscode.Position(arg.line, Number.isFinite(arg.character) ? arg.character : 0);
            }
            return arg;
        });
    }
    return new vscode.CodeLens(toRange(item.range), {
        title: cmd.title || cmd.command,
        command: cmd.command,
        arguments: args,
    });
}

// The a-lsp selectionRange reply is non-standard ({ranges:[...]} per
// position); accept that plus the spec shape ({range, parent}).
function selectionRangeFrom(entry) {
    if (!entry) return null;
    const node = Array.isArray(entry.ranges) && entry.ranges.length ? entry.ranges[0] : entry;
    if (!node || !node.range) return null;
    let parent;
    const p = node.parent;
    const parentNode = Array.isArray(p) ? p[0] : p;
    if (parentNode && parentNode.range) {
        parent = selectionRangeFrom(parentNode);
    }
    return new vscode.SelectionRange(toRange(node.range), parent);
}

// ------------------------------------------------------------- diagnostics

// vscode.DiagnosticSeverity -> LSP severity (1=Error .. 4=Hint).
function diagnosticSeverityToLsp(severity) {
    switch (severity) {
        case vscode.DiagnosticSeverity.Error: return 1;
        case vscode.DiagnosticSeverity.Warning: return 2;
        case vscode.DiagnosticSeverity.Information: return 3;
        case vscode.DiagnosticSeverity.Hint: return 4;
        default: return 1;
    }
}

function lspDiagnosticsToVscode(items, uri) {
    return (Array.isArray(items) ? items : []).map(item => {
        const d = new vscode.Diagnostic(toRange(item.range), item.message || '', toSeverity(item.severity));
        d.source = item.source || 'a';
        if (item.code !== undefined && item.code !== null) {
            const target = item.codeDescription && item.codeDescription.href;
            d.code = target ? { value: String(item.code), target: vscode.Uri.parse(target) } : String(item.code);
        }
        return d;
    });
}

function toSeverity(severity) {
    switch (severity) {
        case 1: return vscode.DiagnosticSeverity.Error;
        case 2: return vscode.DiagnosticSeverity.Warning;
        case 3: return vscode.DiagnosticSeverity.Information;
        case 4: return vscode.DiagnosticSeverity.Hint;
        default: return vscode.DiagnosticSeverity.Error;
    }
}

function failureMessage(output) {
    return String(output || '').replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/).join('\n').trim();
}

module.exports = {
    A_DOC, A_SELECTOR, isA, cwdOf, shellQuote,
    toRange, toPosition, locationsFrom,
    toSymbolKind, toCompletionKind, declSymbolKind, convertDocumentSymbol,
    toTextEdit, workspaceEditFromLsp,
    hoverContents, documentationToMarkdown, signatureHelpFrom,
    hierarchyItemFrom, typeHierarchyItemFrom,
    callHierarchyIncomingFrom, callHierarchyOutgoingFrom,
    codeLensFrom, selectionRangeFrom, rangesFrom,
    diagnosticSeverityToLsp, lspDiagnosticsToVscode, toSeverity, failureMessage,
};
