'use strict';

const fs = require('node:fs');
const path = require('node:path');

function expand(value, folder, file) {
    return String(value || '')
        .replace(/\$\{workspaceFolder\}/g, () => folder || '')
        .replace(/\$\{file\}/g, () => file || '');
}

function absolute(value, folder, file) {
    const expanded = expand(value, folder, file);
    if (!expanded) return '';
    return path.isAbsolute(expanded) ? expanded : path.resolve(folder || process.cwd(), expanded);
}

// The `a` compiler executable: the configured path or command, a build/a (or
// build/Release|Debug/a) next to the workspace folder, then `a` on PATH.
function compilerPath(configured, folder) {
    if (configured) {
        const expanded = expand(configured, folder, '');
        return /[\\/]/.test(expanded) ? path.resolve(folder || process.cwd(), expanded) : expanded;
    }
    const name = process.platform === 'win32' ? 'a.exe' : 'a';
    for (const relative of [name, `build/Release/${name}`, `build/Debug/${name}`, `build/${name}`]) {
        const candidate = path.join(folder || process.cwd(), relative);
        try {
            if (fs.statSync(candidate).isFile()) return candidate;
        } catch { /* keep looking */ }
    }
    return name;
}

// The `a-lsp` language server executable, searched like the compiler but
// under build*/ directories and on PATH.
function serverPath(configured, folder, extensionPath) {
    if (configured) {
        const expanded = expand(configured, folder, '');
        return /[\\/]/.test(expanded) ? path.resolve(folder || process.cwd(), expanded) : expanded;
    }
    const names = process.platform === 'win32' ? ['a-lsp.exe', 'a-lsp'] : ['a-lsp'];
    const roots = [];
    if (folder) roots.push(folder);
    if (extensionPath) roots.push(path.resolve(extensionPath, '..', '..'));
    for (const root of roots) {
        for (const name of names) {
            for (const relative of [`build/${name}`, `build/Release/${name}`, `build/Debug/${name}`, name]) {
                const candidate = path.join(root, relative);
                try {
                    if (fs.statSync(candidate).isFile()) return candidate;
                } catch { /* keep looking */ }
            }
        }
        try {
            for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
                if (!entry.isDirectory() || !entry.name.startsWith('build')) continue;
                for (const name of names) {
                    const candidate = path.join(root, entry.name, name);
                    try {
                        if (fs.statSync(candidate).isFile()) return candidate;
                    } catch { /* keep looking */ }
                }
            }
        } catch { /* not a readable directory */ }
    }
    const onPath = whichOnPath(names[0]);
    return onPath || '';
}

function whichOnPath(name) {
    const { spawnSync } = require('node:child_process');
    const command = process.platform === 'win32' ? 'where' : 'which';
    const result = spawnSync(command, [name], { encoding: 'utf8' });
    if (result.status !== 0 || !result.stdout) return '';
    return result.stdout.split(/\r?\n/).map(line => line.trim()).find(Boolean) || '';
}

function isExecutable(pathname) {
    try {
        fs.accessSync(pathname, fs.constants.X_OK);
        return true;
    } catch {
        return false;
    }
}

// apm packages: the workspace folder (or nearest ancestor) that holds an
// apm.json gets its .apm/packages appended ("" when it is not an apm
// project). Mirrors the CLI's findProjectDir, so bare `import <pkg>` and
// its candidates resolve to installed packages exactly like `a run`.
function apmPackagesDir(folder) {
    if (!folder) return '';
    let dir = path.resolve(folder);
    for (;;) {
        try {
            if (fs.existsSync(path.join(dir, 'apm.json'))) {
                const pkgDir = path.join(dir, '.apm', 'packages');
                if (fs.statSync(pkgDir).isDirectory()) return pkgDir;
                return '';
            }
        } catch { /* not a readable directory */ }
        const parent = path.dirname(dir);
        if (parent === dir) return '';
        dir = parent;
    }
}

function modulePaths(config, cwd, file) {
    const merged = [
        ...(Array.isArray(config.get('modulePaths', [])) ? config.get('modulePaths', []) : []),
        ...(Array.isArray(config.get('server.modulePaths', [])) ? config.get('server.modulePaths', []) : [])
    ];
    const apmDir = apmPackagesDir(cwd);
    if (apmDir) merged.push(apmDir);
    const seen = new Set();
    const dirs = [];
    for (const entry of merged) {
        const dir = absolute(entry, cwd, file);
        if (dir && !seen.has(dir)) { seen.add(dir); dirs.push(dir); }
    }
    return dirs;
}

// Settings for the document (or task) at `uri`: the entry file commands act
// on, the compiler, module paths, stdlib override, timeouts and switches.
function jobsOf(config) {
    const jobs = Number(config.get('jobs', 0));
    return Number.isFinite(jobs) && jobs >= 0 ? Math.trunc(jobs) : 0;
}

function settings(vscode, uri, definition = {}, taskFolder) {
    const workspaceFolder = taskFolder || vscode.workspace.getWorkspaceFolder(uri);
    const cwd = workspaceFolder ? workspaceFolder.uri.fsPath : path.dirname(uri.fsPath);
    const config = vscode.workspace.getConfiguration('a', taskFolder && taskFolder.uri ? taskFolder.uri : uri);
    const file = absolute(definition.file || config.get('entryFile', '') || uri.fsPath, cwd, uri.fsPath || '');
    const output = definition.output
        ? absolute(definition.output, cwd, file)
        : file.replace(/\.(astra|a)$/, '') || `${file}.out`;
    return {
        workspaceFolder, cwd, file,
        compiler: compilerPath(config.get('compilerPath', ''), cwd),
        stdlib: config.get('stdlibPath', '') ? absolute(config.get('stdlibPath', ''), cwd, uri.fsPath || '') : '',
        modulePaths: modulePaths(config, cwd, uri.fsPath || ''),
        timeout: Math.min(300000, Math.max(1000, config.get('checkTimeout', 30000))),
        checkOnSave: config.get('checkOnSave', true),
        useLanguageServer: config.get('useLanguageServer', true),
        jobs: jobsOf(config),
        output
    };
}

// The `a` command line for an action: run, build, test or check. Module
// paths become -I flags, stdlibPath becomes --stdlib, and build gets -o.
function argumentsFor(action, config, options = {}) {
    if (!['run', 'build', 'test', 'check', 'fmt'].includes(action)) {
        throw new Error(`Unknown A task action: ${action}`);
    }
    const args = [action];
    if (options.json) args.push('--json');
    if (config.stdlib) args.push('--stdlib', config.stdlib);
    for (const dir of config.modulePaths) args.push('-I', dir);
    // Parallel/async checking: same diagnostics, faster on multi-module code.
    if (config.jobs !== undefined && config.jobs > 1) {
        args.push(`--jobs=${config.jobs}`);
    } else if (config.jobs === 0) {
        args.push('--parallel');
    }
    if (action === 'build') args.push('-o', config.output);
    if (action === 'fmt') args.push('-w');
    // `a test --filter <name>`: per-test CodeLens runs a single test_* fn.
    if (action === 'test' && options.filter) args.push('--filter', options.filter);
    args.push(config.file);
    return args;
}

// Where a quoted import may live, mirroring ModuleLoader::resolveQuoted:
// relative to the importing file, then -I/module paths, A_PATH, stdlib.
function quotedImportCandidates(rawPath, currentFile, config, env = process.env) {
    const requested = path.isAbsolute(rawPath) ? rawPath : null;
    const roots = requested ? [path.parse(requested).root] : [];
    if (!requested) {
        roots.push(path.dirname(currentFile));
        for (const dir of config.modulePaths) roots.push(dir);
        for (const dir of (env.A_PATH || '').split(path.delimiter).filter(Boolean)) roots.push(dir);
        if (config.stdlib) roots.push(config.stdlib);
    }
    return requested ? [requested] : roots.map(root => path.join(root, rawPath));
}

// Where a bare `import name` may live, mirroring ModuleLoader::resolveBare:
// <root>/name.astra, <root>/name/name.astra, <root>/name/__init__.astra under
// the stdlib root and each module path.
function bareImportCandidates(name, config) {
    const roots = [];
    if (config.stdlib) roots.push(config.stdlib);
    for (const dir of config.modulePaths) roots.push(dir);
    const candidates = [];
    for (const root of roots) {
        candidates.push(path.join(root, `${name}.astra`));
        candidates.push(path.join(root, name, `${name}.astra`));
        candidates.push(path.join(root, name, '__init__.astra'));
        candidates.push(path.join(root, 'std', `${name}.astra`));
        candidates.push(path.join(root, 'std', name, `${name}.astra`));
        candidates.push(path.join(root, 'std', name, '__init__.astra'));
    }
    return candidates;
}

module.exports = {
    expand, absolute, compilerPath, serverPath, isExecutable, modulePaths,
    apmPackagesDir, settings, argumentsFor, quotedImportCandidates, bareImportCandidates
};
