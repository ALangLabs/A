'use strict';

const { execFile } = require('node:child_process');
const { argumentsFor } = require('./config');

// The diagnostics `a check --json` prints on stdout: {"diagnostics":[{severity,
// file, line, column, message, help}]}, with 1-based lines and columns.
function jsonDiagnostics(stdout) {
    const start = stdout.indexOf('{');
    const end = stdout.lastIndexOf('}');
    if (start < 0 || end <= start) return undefined;
    try {
        const parsed = JSON.parse(stdout.slice(start, end + 1));
        return Array.isArray(parsed.diagnostics) ? parsed.diagnostics : undefined;
    } catch {
        return undefined;
    }
}

function toItem(item) {
    if (!item || typeof item.file !== 'string' || !Number.isInteger(item.line)) return undefined;
    return {
        file: item.file,
        line: Math.max(0, item.line - 1),
        column: Number.isInteger(item.column) ? Math.max(0, item.column - 1) : undefined,
        severity: item.severity === 'warning' ? 'warning' : 'error',
        message: String(item.message ?? ''),
        help: typeof item.help === 'string' ? item.help : ''
    };
}

// The errors and warnings in the text `a check` prints when it has no --json:
// an `error: message`/`warning: message` line followed by a `--> file:line:col`
// line. 1-based lines and columns, as the JSON form.
function parseDiagnostics(output, cwd) {
    const lines = String(output).replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/);
    const diagnostics = [];
    let pending;
    for (const line of lines) {
        const head = /^(error|warning)(?:\[[^\]]*\])?:\s*(.*)$/.exec(line);
        if (head) {
            pending = { severity: head[1], message: head[2], related: [] };
            continue;
        }
        const at = /^\s*-->\s+(.+):(\d+):(\d+)\s*$/.exec(line);
        if (at && pending) {
            diagnostics.push({
                file: at[1], line: Math.max(0, Number(at[2]) - 1),
                column: Math.max(0, Number(at[3]) - 1),
                severity: pending.severity, message: pending.message, help: '', related: []
            });
            pending = undefined;
        }
    }
    return diagnostics;
}

// A check of config's entry file with `a check --json`. The result has the
// compiler's error, if any, the output text and the diagnostics as data.
// A compiler that prints no JSON falls back to the text diagnostics.
function runCheck(config, spawn = execFile) {
    const args = argumentsFor('check', config, { json: true });
    return new Promise(resolve => {
        spawn(config.compiler, args, {
            cwd: config.cwd, timeout: config.timeout, maxBuffer: 8 * 1024 * 1024,
            encoding: 'utf8', windowsHide: true, shell: false
        }, (error, stdout, stderr) => {
            const output = [stdout, stderr].filter(Boolean).join('\n');
            let items = jsonDiagnostics(stdout || '');
            if (items) {
                items = items.map(toItem).filter(Boolean);
            } else {
                items = parseDiagnostics(output, config.cwd);
            }
            resolve({ error, output, args, diagnostics: items });
        });
    });
}

// Own results per checked entry file: clearing one root must not erase
// another's errors in a shared import. A result stays until the next check of
// its entry replaces it, marked stale once its sources change.
class CheckQueue {
    constructor(check, changed) {
        this.check = check;
        this.changed = changed;
        this.jobs = new Map();
        this.results = new Map();
    }

    cancel(key) {
        const job = this.jobs.get(key);
        if (!job) return false;
        clearTimeout(job.timer);
        job.finish(false);
        this.jobs.delete(key);
        return true;
    }

    // The checks and results in cwd (or everywhere) no longer apply.
    invalidate(cwd) {
        for (const [key, job] of this.jobs) {
            if (!cwd || job.config.cwd === cwd) this.cancel(key);
        }
        for (const [key, result] of this.results) {
            if (!cwd || result.config.cwd === cwd) this.results.delete(key);
        }
        this.changed(this.results);
    }

    // Sources in cwd changed: stop its checks, and keep its results, marked
    // stale, for the next checks to replace.
    stale(cwd) {
        let changed = false;
        for (const [key, job] of this.jobs) {
            if (job.config.cwd === cwd) changed = this.cancel(key) || changed;
        }
        for (const result of this.results.values()) {
            if (result.config.cwd === cwd && !result.stale) result.stale = changed = true;
        }
        if (changed) this.changed(this.results);
    }

    // Drop the results of entry files that are no longer checked.
    retain(checked) {
        let changed = false;
        for (const key of this.results.keys()) {
            if (!checked(key)) changed = this.results.delete(key);
        }
        if (changed) this.changed(this.results);
    }

    schedule(config, delay = 180) {
        const key = config.file;
        this.cancel(key);
        const scheduled = new Promise(finish => {
            const job = { config, finish };
            this.jobs.set(key, job);
            job.timer = setTimeout(async () => {
                try {
                    const result = await this.check(config);
                    if (this.jobs.get(key) !== job) return;
                    this.jobs.delete(key);
                    this.results.set(key, { config, startedAt: Date.now(), ...result });
                    this.changed(this.results);
                    finish(true);
                } catch (error) {
                    if (this.jobs.get(key) !== job) return;
                    this.jobs.delete(key);
                    this.results.set(key, { config, startedAt: Date.now(), error, output: '' });
                    this.changed(this.results);
                    finish(true);
                }
            }, delay);
        });
        // Report after the job is registered, so a listener can see it is running.
        this.changed(this.results);
        return scheduled;
    }
}

module.exports = { runCheck, parseDiagnostics, CheckQueue };
