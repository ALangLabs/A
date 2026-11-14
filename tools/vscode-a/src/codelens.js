'use strict';

// Run/Test CodeLens providers, Python-extension style: a "Run" and a
// "Run Tests" lens at the top of every saved .astra file, plus a "Run Tests"
// lens above each detected `fn test_*` (found through whichever document
// symbol provider is active -- a-lsp or the lexical fallback). The lenses
// just invoke the a.run / a.test commands with the file URI; execution
// lives in commands.js so behavior stays identical to the command palette.

const vscode = require('vscode');
const { isA } = require('./util');

const TEST_FN = /^test_\w*$/;

function enabledFor(document) {
    if (!isA(document) || document.uri.scheme !== 'file') return false;
    return vscode.workspace.getConfiguration('a', document.uri).get('codeLens.enabled', true);
}

// extraArgs: optional command arguments appended after the file URI (the
// per-test lenses pass the test name so `a test --filter <name>` runs just
// that test).
function lens(range, title, command, uri, extraArgs) {
    return new vscode.CodeLens(range, {
        title,
        command,
        arguments: [uri.toString(), ...(extraArgs || [])],
    });
}

// Top-level `fn test_*` symbols, via the registered document symbol
// providers (works in both LSP and fallback mode).
async function testFunctions(document) {
    try {
        const symbols = await vscode.commands.executeCommand(
            'vscode.executeDocumentSymbolProvider', document.uri);
        const found = [];
        const visit = list => {
            for (const s of list || []) {
                if (s.kind === vscode.SymbolKind.Function && TEST_FN.test(s.name || '')) {
                    found.push(s);
                }
                if (Array.isArray(s.children) && s.children.length) visit(s.children);
            }
        };
        visit(symbols);
        return found;
    } catch {
        return [];
    }
}

function registerRunTestCodeLens(context) {
    const provider = {
        async provideCodeLenses(document) {
            if (!enabledFor(document)) return [];
            const lenses = [];
            const top = new vscode.Range(0, 0, 0, 0);
            lenses.push(lens(top, '$(play) Run', 'a.run', document.uri));
            const tests = await testFunctions(document);
            if (tests.length) {
                lenses.push(lens(top, '$(beaker) Run Tests', 'a.test', document.uri));
                for (const fn of tests) {
                    const line = fn.range.start.line;
                    lenses.push(lens(new vscode.Range(line, 0, line, 0),
                        `$(beaker) Run Tests (${fn.name})`, 'a.test', document.uri, [fn.name]));
                }
            }
            return lenses;
        }
    };
    context.subscriptions.push(
        vscode.languages.registerCodeLensProvider(
            [{ language: 'a', scheme: 'file' }], provider));
}

module.exports = { registerRunTestCodeLens };
