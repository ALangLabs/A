'use strict';

// Minimal Language Server Protocol client over stdio, dependency-free so the
// extension loads without npm install. Everything the editor needs
// (diagnostics, hover, completion, ...) is served by the a-lsp process.

const vscode = require('vscode');
const cp = require('child_process');

class LanguageClient {
    constructor(serverPath, args, initOptions, output) {
        this.serverPath = serverPath;
        this.args = args;
        this.initOptions = initOptions || {};
        this.output = output;
        this.process = undefined;
        this.buffer = Buffer.alloc(0);
        this.nextId = 1;
        this.pending = new Map();
        this.ready = false;
        this.disposables = [];
        this.diagnostics = vscode.languages.createDiagnosticCollection('a');
        this.disposables.push(this.diagnostics);
    }

    async start() {
        const root = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length
            ? vscode.workspace.workspaceFolders[0].uri.toString()
            : null;

        this.process = cp.spawn(this.serverPath, this.args, {
            cwd: vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length
                ? vscode.workspace.workspaceFolders[0].uri.fsPath
                : undefined,
            stdio: ['pipe', 'pipe', 'pipe'],
            env: process.env,
            windowsHide: true,
        });

        this.process.stdout.on('data', (chunk) => this.onData(chunk));
        this.process.stderr.on('data', (chunk) => {
            const text = chunk.toString('utf8').trim();
            if (text) this.output.appendLine(text);
        });
        this.process.on('error', (error) => {
            this.output.appendLine(`A language server failed to start: ${error.message}`);
            for (const pending of this.pending.values()) {
                pending.reject(error);
            }
            this.pending.clear();
            if (this.onInitializeError) {
                const reject = this.onInitializeError;
                this.onInitializeError = undefined;
                reject(error);
            }
        });
        this.process.on('exit', (code, signal) => {
            this.ready = false;
            for (const pending of this.pending.values()) {
                pending.reject(new Error('The A language server exited.'));
            }
            this.pending.clear();
            if (this.onInitializeError) {
                const reject = this.onInitializeError;
                this.onInitializeError = undefined;
                reject(new Error('The A language server exited during initialization.'));
            }
            if (code !== 0 && code !== null) {
                this.output.appendLine(`A language server exited with code ${code}${signal ? ` (${signal})` : ''}.`);
            }
        });

        const initialized = new Promise((resolve, reject) => {
            this.onInitialized = resolve;
            this.onInitializeError = reject;
        });

        this.request('initialize', {
            processId: process.pid,
            clientInfo: { name: 'a-vscode', version: require('./package.json').version },
            rootUri: root,
            workspaceFolders: vscode.workspace.workspaceFolders
                ? vscode.workspace.workspaceFolders.map((folder) => ({
                    uri: folder.uri.toString(),
                    name: folder.name,
                }))
                : null,
            capabilities: {
                workspace: { workspaceFolders: true },
                textDocument: {
                    synchronization: { didSave: true, dynamicRegistration: false },
                    completion: {
                        completionItem: {
                            snippetSupport: false,
                            resolveSupport: { properties: ['documentation'] },
                        },
                    },
                    signatureHelp: {},
                    hover: { contentFormat: ['markdown', 'plaintext'] },
                    documentSymbol: { hierarchicalDocumentSymbolSupport: true },
                    definition: { linkSupport: true },
                    references: {},
                    documentHighlight: {},
                    formatting: {},
                    rename: {},
                    foldingRange: {},
                },
            },
            initializationOptions: {
                modulePaths: this.initOptions.modulePaths || [],
                stdlibRoot: this.initOptions.stdlibRoot || '',
            },
        }).catch((error) => {
            if (this.onInitializeError) this.onInitializeError(error);
        });

        await Promise.race([
            initialized,
            new Promise((_, reject) =>
                setTimeout(() => reject(new Error('Timed out waiting for a-lsp to initialize.')), 10000)
            ),
        ]);
        this.ready = true;
        this.notify('initialized', {});
        this.pushConfiguration();
    }

    // The server also accepts workspace/didChangeConfiguration with
    // settings.a.{modulePaths,stdlibRoot}; send the current values so edits to
    // a.modulePaths or a.stdlibPath reach it without a restart.
    pushConfiguration() {
        this.notify('workspace/didChangeConfiguration', {
            settings: {
                a: {
                    modulePaths: this.initOptions.modulePaths || [],
                    stdlibRoot: this.initOptions.stdlibRoot || '',
                },
            },
        });
    }

    stop() {
        this.ready = false;
        this.diagnostics.clear();
        for (const disposable of this.disposables) disposable.dispose();
        this.disposables = [];
        if (this.process) {
            try {
                this.notify('exit', {});
            } catch {
                // The pipe may already be closed.
            }
            try {
                this.process.kill();
            } catch {
                // Ignore.
            }
            this.process = undefined;
        }
    }

    send(message) {
        if (!this.process || !this.process.stdin.writable) return;
        const body = Buffer.from(JSON.stringify(message), 'utf8');
        this.process.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
        this.process.stdin.write(body);
    }

    notify(method, params) {
        this.send({ jsonrpc: '2.0', method, params });
    }

    request(method, params) {
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.send({ jsonrpc: '2.0', id, method, params });
        });
    }

    respond(id, result) {
        this.send({ jsonrpc: '2.0', id, result });
    }

    respondError(id, code, message) {
        this.send({ jsonrpc: '2.0', id, error: { code, message } });
    }

    onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (true) {
            const headerEnd = this.buffer.indexOf('\r\n\r\n');
            if (headerEnd < 0) return;
            const header = this.buffer.slice(0, headerEnd).toString('ascii');
            const match = /content-length:\s*(\d+)/i.exec(header);
            if (!match) {
                this.buffer = this.buffer.slice(headerEnd + 4);
                continue;
            }
            const length = Number.parseInt(match[1], 10);
            const start = headerEnd + 4;
            if (this.buffer.length < start + length) return;
            const body = this.buffer.slice(start, start + length).toString('utf8');
            this.buffer = this.buffer.slice(start + length);
            let message;
            try {
                message = JSON.parse(body);
            } catch (error) {
                this.output.appendLine(`Invalid message from a-lsp: ${error.message}`);
                continue;
            }
            this.handleMessage(message);
        }
    }

    handleMessage(message) {
        if (message.id !== undefined && !message.method) {
            const pending = this.pending.get(message.id);
            if (!pending) return;
            this.pending.delete(message.id);
            if (message.error) {
                pending.reject(new Error(message.error.message || 'A language server error.'));
            } else {
                const result = message.result;
                pending.resolve(result);
                if (this.onInitialized && result && result.capabilities) {
                    const resolve = this.onInitialized;
                    this.onInitialized = undefined;
                    resolve();
                }
            }
            return;
        }

        if (message.method === 'textDocument/publishDiagnostics') {
            this.publishDiagnostics(message.params);
            return;
        }

        if (message.id !== undefined) {
            if (message.method === 'workspace/configuration') {
                this.respond(message.id, []);
            } else if (message.method === 'client/registerCapability' || message.method === 'window/workDoneProgress/create') {
                this.respond(message.id, null);
            } else {
                this.respondError(message.id, -32601, `unsupported request: ${message.method}`);
            }
        }
    }

    publishDiagnostics(params) {
        if (!params || !params.uri) return;
        const uri = vscode.Uri.parse(params.uri);
        const diagnostics = (params.diagnostics || []).map((item) => {
            const diagnostic = new vscode.Diagnostic(
                toRange(item.range),
                item.message || '',
                toSeverity(item.severity)
            );
            diagnostic.source = item.source || 'a';
            if (item.code !== undefined) diagnostic.code = item.code;
            return diagnostic;
        });
        this.diagnostics.set(uri, diagnostics);
    }

    openDocument(document) {
        this.notify('textDocument/didOpen', {
            textDocument: {
                uri: document.uri.toString(),
                languageId: 'a',
                version: document.version,
                text: document.getText(),
            },
        });
    }

    changeDocument(document) {
        this.notify('textDocument/didChange', {
            textDocument: { uri: document.uri.toString(), version: document.version },
            contentChanges: [{ text: document.getText() }],
        });
    }

    saveDocument(document) {
        this.notify('textDocument/didSave', {
            textDocument: { uri: document.uri.toString() },
            text: document.getText(),
        });
    }

    closeDocument(document) {
        this.notify('textDocument/didClose', {
            textDocument: { uri: document.uri.toString() },
        });
        this.diagnostics.delete(document.uri);
    }
}

// ------------------------------------------------------------- utilities

function toRange(range) {
    if (!range) return new vscode.Range(0, 0, 0, 0);
    return new vscode.Range(
        range.start.line,
        range.start.character,
        range.end.line,
        range.end.character
    );
}

function toSeverity(severity) {
    switch (severity) {
        case 1:
            return vscode.DiagnosticSeverity.Error;
        case 2:
            return vscode.DiagnosticSeverity.Warning;
        case 3:
            return vscode.DiagnosticSeverity.Information;
        case 4:
            return vscode.DiagnosticSeverity.Hint;
        default:
            return vscode.DiagnosticSeverity.Error;
    }
}

module.exports = { LanguageClient, toRange, toSeverity };
