// Parse TD flow output into Problems entries.
//
// Pure functions only (no `vscode` import) so the parser can be unit-tested; FlowController turns
// the result into a DiagnosticCollection.

'use strict';

const SEVERITY_WORDS = /\b(CRITICAL-WARNING|WARNING|ERROR)\b/i;
const CODE = /\b([A-Z][A-Z0-9]{1,5}-\d{3,5})\b/;
const IN_FILE = /(?:^|\s(?:in|at)\s+|\s)([A-Za-z]:[\\/][^()\n]+?|[A-Za-z_][\w./\\-]*\.(?:v|sv|vh|svh|vhd|vhdl|adc|sdc|al))\((\d+)\)/i;
const NOISE = /(?:start command|analyze (?:verilog|vhdl|entity|architecture|file)|launch_runs|reset_run|save_best_bits|query_current_package|open_project|commit_param|import_device|wait_run|read_hdl|read_adc)\b/i;

/**
 * @param {string} message
 * @param {{forceError?: boolean}} [options]
 * @returns {null|{severity:'error'|'warning', code:string, message:string, file:string, line:number}}
 */
function parseFlowIssue(message, options) {
    let text = String(message || '').replace(/^\s*\[run\.log\]\s*/i, '').trim();
    if (!text) return null;
    let severity = '';
    const sev = SEVERITY_WORDS.exec(text);
    if (sev) severity = sev[1].toUpperCase();
    const codeMatch = CODE.exec(text);
    const code = codeMatch ? codeMatch[1] : '';
    if (!severity && code) {
        // Many TD diagnostics have a code but no severity token. Do not turn progress chatter
        // (RUN-1002 start command, ...) into Problems entries.
        if (NOISE.test(text)) return null;
        if (/\b(?:warning|error|critical|ignored|undeclared|syntax|failed|failure)\b/i.test(text)) {
            severity = 'WARNING';
        }
    }
    if (!severity && options && options.forceError) severity = 'ERROR';
    if (!severity) return null;
    const fileMatch = IN_FILE.exec(text);
    let file = '';
    let line = 1;
    if (fileMatch) {
        file = fileMatch[1];
        line = Math.max(1, parseInt(fileMatch[2], 10) || 1);
    }
    let severityName = severity === 'ERROR' ? 'error' : 'warning';
    let body = text.replace(/\s+/g, ' ').slice(0, 1200);
    // Strip a leading "[run.log]" was already done; keep the original TD code in the message unless
    // it would be redundant with the Diagnostic.code value.
    return { severity: severityName, code: code || 'FLOW', message: body, file, line };
}

/**
 * Resolve a path from a flow message against the project directory.
 * @param {string} file
 * @param {{dir?: string}} [project]
 * @returns {string}
 */
function resolveFlowFile(file, project) {
    if (!file) return '';
    const path = require('path');
    if (path.isAbsolute(file) || /^[A-Za-z]:[\\/]/.test(file)) return file;
    if (project && project.dir) return path.join(project.dir, file);
    return file;
}

module.exports = { parseFlowIssue, resolveFlowFile };
