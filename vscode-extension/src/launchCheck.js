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

// ---------------------------------------------------------------- logging
//
// F6/R8: `appendLog()` used to do `mkdirSync` + `appendFileSync` on every call. The extension
// logs from *event handlers* (`fpgaLsp/indexChanged` fires once per save), and on a project
// volume that append measured 6-7 ms of blocked extension-host time per event
// (audit/2026-11-19/perf/VSCODE.md §P2). Writes are now buffered per file and performed
// asynchronously on the next tick; ordering inside a batch is preserved.
//
// Callers that must not lose their message (shutdown, fatal errors) can call `flushLogSync()`.

const pendingLines = new Map();   // filePath -> string[]
const MAX_PENDING_LINES = 4096;   // a log that cannot keep up must not grow without bound
let flushScheduled = false;

function scheduleFlush() {
    if (flushScheduled) return;
    flushScheduled = true;
    // No timer/polling: one flush attempt per macrotask that produced log lines.
    setImmediate(() => {
        flushScheduled = false;
        flushPending();
    });
}

function flushPending() {
    for (const [filePath, lines] of pendingLines) {
        if (!lines.length) continue;
        const chunk = lines.join('');
        lines.length = 0;
        fs.mkdir(path.dirname(filePath), { recursive: true }, (dirError) => {
            if (dirError) return;
            fs.appendFile(filePath, chunk, () => { /* best effort: logging never throws */ });
        });
    }
}

/** Append a line to a log file (buffered, asynchronous), creating parent dirs; never throws. */
function appendLog(filePath, message) {
    if (!filePath) return;
    let lines = pendingLines.get(filePath);
    if (!lines) {
        lines = [];
        pendingLines.set(filePath, lines);
    }
    lines.push(`[${new Date().toISOString()}] ${message}\n`);
    if (lines.length > MAX_PENDING_LINES) lines.splice(0, lines.length - MAX_PENDING_LINES);
    scheduleFlush();
    try { console.log(message); } catch (_) { /* ignore */ }
}

/**
 * Write everything still buffered, synchronously. Only for shutdown / fatal paths -- the normal
 * path must stay asynchronous (see the note above).
 */
function flushLogSync() {
    for (const [filePath, lines] of pendingLines) {
        if (!lines.length) continue;
        const chunk = lines.join('');
        lines.length = 0;
        try {
            fs.mkdirSync(path.dirname(filePath), { recursive: true });
            fs.appendFileSync(filePath, chunk, 'utf8');
        } catch (_) { /* read-only workspace etc. */ }
    }
}

module.exports = { checkPython, appendLog, flushLogSync, run };
