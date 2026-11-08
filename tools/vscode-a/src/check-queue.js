'use strict';

// Check-on-save queue used when a-lsp is unavailable: debounced `a check
// --json` runs per workspace folder, results cached and applied to the
// 'a-check' diagnostic collection. (diagnostics.js holds an alternate,
// entry-keyed implementation; this one is what the extension activates.)

const vscode = require('vscode');
const path = require('node:path');
const { argumentsFor } = require('../config');
const { failureMessage } = require('./util');

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

function runCheck(config, output, setKill) {
    const cp = require('node:child_process');
    return new Promise(resolve => {
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

module.exports = { CheckQueue, runCheck, parseTextDiagnostics, applyDiagnostics };
