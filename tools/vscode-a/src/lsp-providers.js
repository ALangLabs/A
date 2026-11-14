'use strict';

// VS Code providers backed by the a-lsp language server. Registered once at
// activation; each provider no-ops until getClient() returns a running
// client. Covers every capability a-lsp advertises: location features,
// hover/completion/signature help, symbols, formatting (document + range),
// rename, highlights, folding, selection range, semantic tokens, inlay
// hints, workspace symbols, document links, code lenses, code actions, and
// call/type hierarchies.

const vscode = require('vscode');
const {
    toRange, locationsFrom, convertDocumentSymbol, toTextEdit,
    workspaceEditFromLsp, hoverContents, documentationToMarkdown,
    signatureHelpFrom, toSymbolKind, toCompletionKind,
    hierarchyItemFrom, typeHierarchyItemFrom,
    callHierarchyIncomingFrom, callHierarchyOutgoingFrom,
    codeLensFrom, selectionRangeFrom, diagnosticSeverityToLsp,
} = require('./util');

const A_LEGEND = new vscode.SemanticTokensLegend(
    ['namespace', 'type', 'class', 'enum', 'function', 'method', 'parameter', 'variable', 'property', 'keyword', 'comment', 'string', 'number', 'operator'],
    ['declaration', 'readonly', 'static']);

function semanticTokensFrom(data) {
    const b = new vscode.SemanticTokensBuilder(A_LEGEND);
    let line = -1, ch = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const dl = data[i], dc = data[i + 1], len = data[i + 2], type = data[i + 3], mods = data[i + 4];
        if (dl > 0) { line += dl; ch = dc; } else ch += dc;
        while (line < 0) line = 0;
        b.push(new vscode.Range(line, ch, line, ch + len), type,
            mods & 1 ? ['declaration'] : mods & 2 ? ['readonly'] : mods & 4 ? ['static'] : []);
    }
    return b.build();
}

// Newer VS Code builds replace registerSemanticTokensProvider with
// registerDocumentSemanticTokensProvider + registerDocumentRangeSemanticTokensProvider;
// older builds expose the single combined API. Probe so the extension
// activates on both -- calling a missing API used to crash activate(),
// leaving the extension entirely dead (no LSP, no hover, empty output).
function registerSemanticTokens(langs, selector, getClient) {
    const fullProvider = {
        async provideDocumentSemanticTokens(document, token) {
            const a = getClient(); if (!a) return null;
            const r = await a.request('textDocument/semanticTokens/full', { textDocument: { uri: document.uri.toString() } }, token);
            if (!r || !Array.isArray(r.data)) return null;
            return semanticTokensFrom(r.data);
        }
    };
    if (typeof langs.registerSemanticTokensProvider === 'function') {
        return [langs.registerSemanticTokensProvider(selector, fullProvider, A_LEGEND)];
    }
    const subs = [];
    if (typeof langs.registerDocumentSemanticTokensProvider === 'function') {
        subs.push(langs.registerDocumentSemanticTokensProvider(selector, fullProvider, A_LEGEND));
    }
    if (typeof langs.registerDocumentRangeSemanticTokensProvider === 'function') {
        const rangeProvider = {
            async provideDocumentRangeSemanticTokens(document, range, token) {
                const a = getClient(); if (!a) return null;
                const r = await a.request('textDocument/semanticTokens/range', {
                    textDocument: { uri: document.uri.toString() },
                    range: { start: { line: range.start.line, character: range.start.character }, end: { line: range.end.line, character: range.end.character } }
                }, token);
                if (!r || !Array.isArray(r.data)) return null;
                return semanticTokensFrom(r.data);
            }
        };
        subs.push(langs.registerDocumentRangeSemanticTokensProvider(selector, rangeProvider, A_LEGEND));
    }
    return subs;
}

function lspPosition(position) {
    return { line: position.line, character: position.character };
}

function lspRange(range) {
    return {
        start: { line: range.start.line, character: range.start.character },
        end: { line: range.end.line, character: range.end.character },
    };
}

function codeActionFrom(action) {
    if (!action || typeof action.title !== 'string') return null;
    const ca = new vscode.CodeAction(action.title, action.kind ? new vscode.CodeActionKind(action.kind) : undefined);
    if (action.edit) ca.edit = workspaceEditFromLsp(action.edit);
    if (Array.isArray(action.diagnostics)) {
        ca.diagnostics = action.diagnostics.map(d => {
            const diag = new vscode.Diagnostic(toRange(d.range), d.message || '', (d.severity || 1) === 2 ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error);
            diag.source = 'a';
            return diag;
        });
    }
    if (action.isPreferred) ca.isPreferred = true;
    if (action.disabled) ca.disabled = { reason: action.disabled.reason || 'Unavailable' };
    if (action.command) {
        ca.command = {
            title: action.command.title || action.title,
            command: action.command.command,
            arguments: Array.isArray(action.command.arguments) ? action.command.arguments : [],
        };
    }
    return ca;
}

// Registers every LSP-backed provider. getClient() must return the running
// LanguageClient or undefined. Returns disposables for context.subscriptions.
function registerProviders(getClient) {
    // Include untitled buffers: otherwise hover/completion/etc. are silently
    // dead for untitled A files while fallback (which also covers them) is
    // unregistered in server mode.
    const selector = [{ language: 'a', scheme: 'file' }, { language: 'a', scheme: 'untitled' }];
    const docUri = document => ({ textDocument: { uri: document.uri.toString() } });
    const pos = (document, position) => ({
        textDocument: { uri: document.uri.toString() },
        position: { line: position.line, character: position.character },
    });
    const client = () => getClient();

    const subs = [
        vscode.languages.registerDefinitionProvider(selector, {
            async provideDefinition(document, position, token) {
                const a = client(); if (!a) return null;
                return locationsFrom(await a.request('textDocument/definition', pos(document, position), token));
            }
        }),
        vscode.languages.registerDeclarationProvider(selector, {
            async provideDeclaration(document, position, token) {
                const a = client(); if (!a) return null;
                return locationsFrom(await a.request('textDocument/declaration', pos(document, position), token));
            }
        }),
        vscode.languages.registerTypeDefinitionProvider(selector, {
            async provideTypeDefinition(document, position, token) {
                const a = client(); if (!a) return null;
                return locationsFrom(await a.request('textDocument/typeDefinition', pos(document, position), token));
            }
        }),
        vscode.languages.registerImplementationProvider(selector, {
            async provideImplementation(document, position, token) {
                const a = client(); if (!a) return null;
                return locationsFrom(await a.request('textDocument/implementation', pos(document, position), token));
            }
        }),
        vscode.languages.registerReferenceProvider(selector, {
            async provideReferences(document, position, refContext) {
                const a = client(); if (!a) return null;
                return locationsFrom(await a.request('textDocument/references', {
                    ...pos(document, position),
                    context: { includeDeclaration: refContext.includeDeclaration },
                }));
            }
        }),
        vscode.languages.registerHoverProvider(selector, {
            async provideHover(document, position, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/hover', pos(document, position), token);
                if (!r || !r.contents) return null;
                return new vscode.Hover(hoverContents(r.contents), r.range ? toRange(r.range) : undefined);
            }
        }),
        vscode.languages.registerCompletionItemProvider(selector, {
            async provideCompletionItems(document, position, token) {
                const a = client(); if (!a) return null;
                const result = await a.request('textDocument/completion', {
                    ...pos(document, position),
                    context: { triggerKind: 1 },
                }, token);
                if (!result) return null;
                const entries = Array.isArray(result) ? result : (result.items || []);
                const items = entries.map(entry => {
                    const it = new vscode.CompletionItem(entry.label, toCompletionKind(entry.kind));
                    if (entry.detail) it.detail = entry.detail;
                    if (entry.documentation) it.documentation = documentationToMarkdown(entry.documentation);
                    if (entry.insertTextFormat === 2 && typeof entry.insertText === 'string') {
                        it.insertText = new vscode.SnippetString(entry.insertText);
                    } else {
                        it.insertText = typeof entry.insertText === 'string' ? entry.insertText : entry.label;
                    }
                    if (entry.sortText) it.sortText = entry.sortText;
                    if (entry.filterText) it.filterText = entry.filterText;
                    if (entry.data) it.data = entry.data;
                    if (entry.preselect) it.preselect = entry.preselect;
                    return it;
                });
                return new vscode.CompletionList(items, Boolean(result.isIncomplete));
            },
            async resolveCompletionItem(item, token) {
                const a = client(); if (!a || !item.data) return item;
                const r = await a.request('completionItem/resolve', { label: item.label, data: item.data }, token);
                if (r && r.documentation) item.documentation = documentationToMarkdown(r.documentation);
                return item;
            }
        }, '.', '"', '(', ',', ':', '<'),
        vscode.languages.registerSignatureHelpProvider(selector, {
            async provideSignatureHelp(document, position) {
                const a = client(); if (!a) return null;
                return signatureHelpFrom(await a.request('textDocument/signatureHelp', pos(document, position)));
            }
        }),
        vscode.languages.registerDocumentSymbolProvider(selector, {
            async provideDocumentSymbols(document) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/documentSymbol', docUri(document));
                if (!Array.isArray(r)) return null;
                return r.map(convertDocumentSymbol);
            }
        }),
        vscode.languages.registerDocumentFormattingEditProvider(selector, {
            async provideDocumentFormattingEdits(document, options) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/formatting', {
                    textDocument: { uri: document.uri.toString() },
                    options: { tabSize: options.tabSize, insertSpaces: options.insertSpaces },
                });
                if (!Array.isArray(r)) return null;
                return r.map(toTextEdit);
            }
        }),
        vscode.languages.registerDocumentRangeFormattingEditProvider(selector, {
            async provideDocumentRangeFormattingEdits(document, range, options) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/rangeFormatting', {
                    textDocument: { uri: document.uri.toString() },
                    range: lspRange(range),
                    options: { tabSize: options.tabSize, insertSpaces: options.insertSpaces },
                });
                if (!Array.isArray(r)) return null;
                return r.map(toTextEdit);
            }
        }),
        vscode.languages.registerRenameProvider(selector, {
            async provideRenameEdits(document, position, newName, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/rename', { ...pos(document, position), newName }, token);
                if (!r) return null;
                return workspaceEditFromLsp(r);
            }
        }),
        vscode.languages.registerDocumentHighlightProvider(selector, {
            async provideDocumentHighlights(document, position) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/documentHighlight', pos(document, position));
                if (!Array.isArray(r)) return null;
                return r.map(e => new vscode.DocumentHighlight(toRange(e.range), Math.max(0, Math.min(2, (e.kind || 1) - 1))));
            }
        }),
        vscode.languages.registerFoldingRangeProvider(selector, {
            async provideFoldingRanges(document) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/foldingRange', docUri(document));
                if (!Array.isArray(r)) return null;
                return r.map(e => new vscode.FoldingRange(e.startLine, e.endLine,
                    e.kind === 'comment' ? vscode.FoldingRangeKind.Comment
                        : e.kind === 'imports' ? vscode.FoldingRangeKind.Imports
                        : vscode.FoldingRangeKind.Region));
            }
        }),
        vscode.languages.registerSelectionRangeProvider(selector, {
            async provideSelectionRanges(document, positions) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/selectionRange', {
                    textDocument: { uri: document.uri.toString() },
                    positions: positions.map(lspPosition),
                });
                if (!Array.isArray(r)) return null;
                return r.map(selectionRangeFrom).filter(Boolean);
            }
        }),
        vscode.languages.registerWorkspaceSymbolProvider({
            async provideWorkspaceSymbols(query) {
                const a = client(); if (!a) return null;
                const r = await a.request('workspace/symbol', { query });
                if (!Array.isArray(r)) return null;
                return r.filter(e => e && e.location).map(e => new vscode.SymbolInformation(
                    e.name, toSymbolKind(e.kind), e.containerName || 'A',
                    new vscode.Location(vscode.Uri.parse(e.location.uri), toRange(e.location.range))));
            }
        }),
        // http(s) links inside comments, served by a-lsp.
        vscode.languages.registerDocumentLinkProvider(selector, {
            async provideDocumentLinks(document) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/documentLink', docUri(document));
                if (!Array.isArray(r)) return null;
                return r.filter(l => l && l.range && l.target).map(l => {
                    const link = new vscode.DocumentLink(toRange(l.range), vscode.Uri.parse(l.target));
                    if (l.tooltip) link.tooltip = l.tooltip;
                    return link;
                });
            }
        }),
        // "N references" lenses on declarations (resolveProvider: false).
        vscode.languages.registerCodeLensProvider(selector, {
            async provideCodeLenses(document) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/codeLens', docUri(document));
                if (!Array.isArray(r)) return null;
                return r.map(codeLensFrom).filter(Boolean);
            }
        }),
        vscode.languages.registerCodeActionsProvider(selector, {
            async provideCodeActions(document, range, context, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/codeAction', {
                    textDocument: { uri: document.uri.toString() },
                    range: lspRange(range),
                    context: {
                        diagnostics: (context.diagnostics || []).map(d => ({
                            range: lspRange(d.range),
                            message: d.message,
                            severity: diagnosticSeverityToLsp(d.severity),
                            source: d.source,
                            code: typeof d.code === 'object' && d.code ? d.code.value : d.code,
                        })),
                        only: context.only ? [context.only.value] : undefined,
                    },
                }, token);
                if (!Array.isArray(r)) return null;
                return r.map(codeActionFrom).filter(Boolean);
            }
        }, { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix, vscode.CodeActionKind.Refactor, new vscode.CodeActionKind('source.fixAll')] }),
        vscode.languages.registerCallHierarchyProvider(selector, {
            async prepareCallHierarchy(document, position, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/prepareCallHierarchy', pos(document, position), token);
                if (!r) return null;
                const entries = Array.isArray(r) ? r : [r];
                const items = entries.map(hierarchyItemFrom).filter(Boolean);
                return items.length === 1 ? items[0] : (items.length ? items : null);
            },
            async provideCallHierarchyIncomingCalls(item, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('callHierarchy/incomingCalls', { item: { ...item, uri: item.uri.toString() } }, token);
                if (!Array.isArray(r)) return null;
                return r.map(callHierarchyIncomingFrom).filter(Boolean);
            },
            async provideCallHierarchyOutgoingCalls(item, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('callHierarchy/outgoingCalls', { item: { ...item, uri: item.uri.toString() } }, token);
                if (!Array.isArray(r)) return null;
                return r.map(callHierarchyOutgoingFrom).filter(Boolean);
            }
        }),
        vscode.languages.registerTypeHierarchyProvider(selector, {
            async prepareTypeHierarchy(document, position, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('textDocument/prepareTypeHierarchy', pos(document, position), token);
                if (!r) return null;
                const entries = Array.isArray(r) ? r : [r];
                const items = entries.map(typeHierarchyItemFrom).filter(Boolean);
                return items.length === 1 ? items[0] : (items.length ? items : null);
            },
            async provideTypeHierarchySupertypes(item, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('typeHierarchy/supertypes', { item: { ...item, uri: item.uri.toString() } }, token);
                if (!Array.isArray(r)) return null;
                return r.map(typeHierarchyItemFrom).filter(Boolean);
            },
            async provideTypeHierarchySubtypes(item, token) {
                const a = client(); if (!a) return null;
                const r = await a.request('typeHierarchy/subtypes', { item: { ...item, uri: item.uri.toString() } }, token);
                if (!Array.isArray(r)) return null;
                return r.map(typeHierarchyItemFrom).filter(Boolean);
            }
        }),
        vscode.languages.registerInlayHintsProvider(selector, {
            async provideInlayHints(document, range, token) {
                const a = client(); if (!a) return null;
                const config = vscode.workspace.getConfiguration('a', document.uri);
                if (!config.get('inlayHints.enabled', false)) return [];
                const wantTypes = config.get('inlayHints.types', false);
                const wantParams = config.get('inlayHints.parameters', false);
                if (!wantTypes && !wantParams) return [];
                const r = await a.request('textDocument/inlayHint', {
                    textDocument: { uri: document.uri.toString() },
                    range: lspRange(range),
                }, token);
                if (!Array.isArray(r)) return null;
                const hints = [];
                for (const e of r) {
                    // LSP InlayHintKind: 1 = Type, 2 = Parameter.
                    if (e.kind === 1 && !wantTypes) continue;
                    if (e.kind === 2 && !wantParams) continue;
                    const h = new vscode.InlayHint(
                        new vscode.Position(e.position.line, e.position.character),
                        e.label,
                        e.kind === 1 ? vscode.InlayHintKind.Type : e.kind === 2 ? vscode.InlayHintKind.Parameter : undefined);
                    h.paddingLeft = true;
                    h.paddingRight = Boolean(e.paddingRight);
                    if (e.tooltip) h.tooltip = typeof e.tooltip === 'string' ? e.tooltip : e.tooltip.value;
                    hints.push(h);
                }
                return hints;
            }
        }),
        ...registerSemanticTokens(vscode.languages, selector, getClient),
    ];
    return subs;
}

module.exports = { registerProviders };
