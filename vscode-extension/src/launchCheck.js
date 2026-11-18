// Preflight checks for the language server launch. These run *inside the extension host* and their
// result is written to <workspace>/.fpgalsp/logs/extension.log, so a launch failure is diagnosable
// without guessing (VS Code's "Server process exited with code N" alone is not actionable).

'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

/** Run a command, capture stdout/stderr, never throw. */
function run(command, args, options, timeoutMs) {
    return new Promise((resolve) => {
        let proc;
        try {
            proc = spawn(command, args, Object.assign({ windowsHide: true }, options || {}));
        } catch (err) {
            resolve({ ok: false, code: null, signal: null, stdout: '', stderr: String(err && err.message) });
            return;
        }
        let stdout = '';
        let stderr = '';
        let done = false;
        const finish = (result) => {
            if (done) return;
            done = true;
            resolve(result);
        };
        const timer = setTimeout(() => {
            try { proc.kill(); } catch (_) { /* ignore */ }
            finish({ ok: false, code: null, signal: 'timeout', stdout, stderr: stderr + `\n[timeout after ${timeoutMs} ms]` });
        }, timeoutMs || 15000);
        proc.stdout.on('data', (d) => { stdout += d.toString('utf8'); });
        proc.stderr.on('data', (d) => { stderr += d.toString('utf8'); });
        proc.on('error', (err) => {
            clearTimeout(timer);
            finish({ ok: false, code: null, signal: null, stdout, stderr: stderr + String(err && err.message) });
        });
        proc.on('exit', (code, signal) => {
            clearTimeout(timer);
            finish({ ok: code === 0, code, signal, stdout: stdout.trim(), stderr: stderr.trim() });
        });
    });
}

/**
 * Check that the interpreter exists and can import the bundled `fpgalsp` package with the same
 * environment/cwd the real server will get.
 * @returns {Promise<{ok: boolean, python: string, executable?: string, version?: string, error?: string, detail?: string}>}
 */
async function checkPython(options) {
    const python = options.python || 'python';
    const result = await run(python, ['-c', 'import sys, fpgalsp; print(sys.executable); print(getattr(fpgalsp, "__version__", "?"))'],
        { cwd: options.cwd, env: options.env, stdio: ['ignore', 'pipe', 'pipe'] }, options.timeoutMs || 20000);
    if (!result.ok) {
        return {
            ok: false,
            python,
            error: result.stderr || `exit ${result.code}${result.signal ? ' signal ' + result.signal : ''}`,
            detail: `cwd=${options.cwd} PYTHONPATH=${(options.env && options.env.PYTHONPATH) || ''}`,
        };
    }
    const lines = result.stdout.split('\n');
    return { ok: true, python, executable: lines[0], version: lines[1] };
}

/** Append a line to a log file, creating parent dirs; silently tolerate failures. */
function appendLog(filePath, message) {
    try {
        fs.mkdirSync(path.dirname(filePath), { recursive: true });
        fs.appendFileSync(filePath, `[${new Date().toISOString()}] ${message}\n`, 'utf8');
    } catch (_) { /* read-only workspace etc. */ }
    try { console.log(message); } catch (_) { /* ignore */ }
}

module.exports = { checkPython, appendLog, run };
