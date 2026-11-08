'use strict';

// Status bar items: "$(play) Run" (left, runs a.run on the active A file)
// and the check-status indicator driven by the CheckQueue / a-lsp state.

const vscode = require('vscode');
const { isA } = require('./util');

// env: { getServerStatus() -> 'off'|'starting'|'running', queue }
function createStatusBars(context, env) {
    const checkStatus = vscode.window.createStatusBarItem('a.check', vscode.StatusBarAlignment.Left, 4);
    checkStatus.name = 'A: Check Status';
    checkStatus.command = 'a.showOutput';
    const runStatus = vscode.window.createStatusBarItem('a.run', vscode.StatusBarAlignment.Left, 5);
    runStatus.name = 'A: Run Program';
    runStatus.text = '$(play) Run';
    runStatus.tooltip = 'Compile and run the A program';
    runStatus.command = 'a.run';

    const activeA = () => vscode.window.activeTextEditor && isA(vscode.window.activeTextEditor.document)
        ? vscode.window.activeTextEditor.document : undefined;

    function updateCheckStatus() {
        const active = activeA();
        if (!active || !vscode.workspace.isTrusted) { checkStatus.hide(); return; }
        const status = env.getServerStatus();
        if (status === 'starting') { checkStatus.text = '$(sync~spin) A'; checkStatus.tooltip = 'Starting a-lsp'; checkStatus.show(); return; }
        if (status === 'running') { checkStatus.text = '$(check) A'; checkStatus.tooltip = 'A language server active (a-lsp). Fallback completion/hover enabled only when server is off.'; checkStatus.show(); return; }
        checkStatus.text = '$(search) A'; checkStatus.tooltip = 'Using fallback check-on-save + lexical/stdlib completions'; checkStatus.show();
    }

    function updateRunStatus() {
        if (vscode.workspace.isTrusted && activeA()) runStatus.show(); else runStatus.hide();
    }

    context.subscriptions.push(checkStatus, runStatus);
    return { updateCheckStatus, updateRunStatus, checkStatus, runStatus };
}

module.exports = { createStatusBars };
