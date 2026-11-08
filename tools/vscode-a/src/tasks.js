'use strict';

// Task support for the `a` task type: the TaskProvider that turns
// {type:'a', action, file?} definitions into ProcessExecutions, plus
// starter-tasks generation -- when a workspace folder contains .a files but
// no .vscode/tasks.json, we write one with Run/Build/Test/Check/Format
// tasks (like the Python extension does), gated by a.tasks.autoGenerate.

const vscode = require('vscode');
const { absolute, settings, argumentsFor } = require('../config');
const { isA, cwdOf } = require('./util');

const ACTION_TITLES = { run: 'Run Program', build: 'Build Executable', test: 'Run Tests', check: 'Check Program', fmt: 'Format File' };

// env: { isTrusted() }
function makeTask(env, definition, uri, scope, name) {
    if (!env.isTrusted() || uri.scheme !== 'file') return undefined;
    const config = settings(vscode, uri, definition, typeof scope === 'object' ? scope : undefined);
    if (!ACTION_TITLES[definition.action]) return undefined;
    const task = new vscode.Task({ ...definition, type: 'a', file: config.file }, scope || config.workspaceFolder || vscode.TaskScope.Workspace, name || ACTION_TITLES[definition.action], 'A', new vscode.ProcessExecution(config.compiler, argumentsFor(definition.action, config, { filter: definition.filter }), { cwd: config.cwd }), '$a');
    if (definition.action === 'run') task.group = vscode.TaskGroup.Build;
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: true };
    return task;
}

function registerTaskProvider(context, env) {
    context.subscriptions.push(vscode.tasks.registerTaskProvider('a', {
        provideTasks() {
            if (!env.isTrusted()) return [];
            const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
            if (doc && isA(doc)) return Object.keys(ACTION_TITLES).map(action => makeTask(env, { type: 'a', action }, doc.uri));
            return (vscode.workspace.workspaceFolders || []).flatMap(folder => {
                const entry = vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', ''); if (!entry) return [];
                const uri = vscode.Uri.file(absolute(entry, folder.uri.fsPath, ''));
                return Object.keys(ACTION_TITLES).map(action => makeTask(env, { type: 'a', action }, uri, folder));
            });
        },
        resolveTask(task) {
            if (!env.isTrusted()) return undefined;
            const folder = typeof task.scope === 'object' ? task.scope : undefined;
            const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
            const base = folder && folder.uri.fsPath; const file = task.definition.file;
            const configuredEntry = folder && vscode.workspace.getConfiguration('a', folder.uri).get('entryFile', '');
            const uri = file && base && !file.includes('${file}') ? vscode.Uri.file(absolute(file, base, '')) : active && isA(active) && (!base || cwdOf(active) === base) ? active.uri : configuredEntry ? vscode.Uri.file(absolute(configuredEntry, base || '', '')) : undefined;
            if (!uri) return undefined;
            const resolved = makeTask(env, task.definition, uri, task.scope, task.name);
            if (resolved) { resolved.definition = task.definition; resolved.presentationOptions = task.presentationOptions; if (task.group) resolved.group = task.group; resolved.problemMatchers = task.problemMatchers.length ? task.problemMatchers : ['$a']; }
            return resolved;
        }
    }));
}

// Starter tasks.json, written once per workspace folder that has .a files
// and no tasks.json yet. Uses the contributed `a` task type so the tasks
// resolve through the same provider as everything else.
const STARTER_TASKS = {
    version: '2.0.0',
    tasks: [
        { label: 'A: Run Program', type: 'a', action: 'run', file: '${file}', group: 'build', presentation: { reveal: 'always', panel: 'dedicated', clear: true } },
        { label: 'A: Build Executable', type: 'a', action: 'build', file: '${file}', presentation: { reveal: 'always', panel: 'dedicated', clear: true } },
        { label: 'A: Run Tests', type: 'a', action: 'test', file: '${file}', presentation: { reveal: 'always', panel: 'dedicated', clear: true } },
        { label: 'A: Check Program', type: 'a', action: 'check', file: '${file}', presentation: { reveal: 'always', panel: 'silent', clear: true } },
        { label: 'A: Format File', type: 'a', action: 'fmt', file: '${file}', presentation: { reveal: 'always', panel: 'dedicated', clear: true } },
    ],
};

async function folderHasAFile(folder) {
    try {
        const files = await vscode.workspace.findFiles(
            new vscode.RelativePattern(folder, '**/*.a'),
            '{**/node_modules/**,**/.git/**,**/build/**,**/build-*/**}', 1);
        return files.length > 0;
    } catch {
        return false;
    }
}

async function generateStarterTasks(context) {
    if (!vscode.workspace.isTrusted) return;
    for (const folder of vscode.workspace.workspaceFolders || []) {
        try {
            const config = vscode.workspace.getConfiguration('a', folder.uri);
            if (!config.get('tasks.autoGenerate', true)) continue;
            const vscodeDir = vscode.Uri.joinPath(folder.uri, '.vscode');
            const tasksUri = vscode.Uri.joinPath(vscodeDir, 'tasks.json');
            try {
                await vscode.workspace.fs.stat(tasksUri);
                continue; // user already has tasks
            } catch { /* missing: generate */ }
            if (!await folderHasAFile(folder)) continue;
            await vscode.workspace.fs.createDirectory(vscodeDir);
            await vscode.workspace.fs.writeFile(
                tasksUri, Buffer.from(JSON.stringify(STARTER_TASKS, null, 2) + '\n', 'utf8'));
        } catch { /* best effort: never break activation */ }
    }
}

module.exports = { ACTION_TITLES, makeTask, registerTaskProvider, generateStarterTasks };
