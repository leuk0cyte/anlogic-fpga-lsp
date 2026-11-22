// Preflight checks for the language server launch. These run *inside the extension host* and their
// result is appended to the extension log, so a launch failure is diagnosable without guessing
// (VS Code's "Server process exited with code N" alone is not actionable).
//
// The log location is not hard-coded here: it is `<state>/logs/extension.log`, where `<state>` is
// resolved by `src/stateDir.js` (system temp, same directory the server gets via
// FPGALSP_STATE_DIR -- the workspace no longer contains `.fpgalsp`). Every byte is written through
// `src/writeService.js` (state root policy + never-throw), never through raw `fs`.

'use strict';

const { spawn } = require('child_process');
const { appendText } = require('./writeService');

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
// (audit/2026-11-19/perf/VSCODE.md §P2). Writes are now buffered per log target and performed
// on the next tick; ordering inside a batch is preserved.
//
// The buffered batch is flushed through `writeService.appendText()` -- that module owns the path
// policy (`<state>/logs/<name>.log`) and all raw file I/O (see src/writeService.js).
//
// Callers that must not lose their message (shutdown, fatal errors) can call `flushLogSync()`.

const pendingLines = new Map();   // "<workspaceRoot>\0<name>" -> { workspaceRoot, name, lines }
const MAX_PENDING_LINES = 4096;   // a log that cannot keep up must not grow without bound
let flushScheduled = false;

function targetOf(workspaceRoot, name) {
    const key = `${workspaceRoot}\0${name}`;
    let bucket = pendingLines.get(key);
    if (!bucket) {
        bucket = { workspaceRoot, name, lines: [] };
        pendingLines.set(key, bucket);
    }
    return bucket;
}

function scheduleFlush() {
    if (flushScheduled) return;
    flushScheduled = true;
    // No timer/polling: one flush attempt per macrotask that produced log lines.
    setImmediate(() => {
        flushScheduled = false;
        flushPending();
    });
}

function flushBucket(bucket) {
    if (!bucket.lines.length) return;
    const chunk = bucket.lines.join('');
    bucket.lines.length = 0;
    // Best effort: logging never throws and a read-only state dir must not break the extension.
    appendText(bucket.workspaceRoot, `logs/${bucket.name}.log`, chunk);
}

function flushPending() {
    for (const bucket of pendingLines.values()) flushBucket(bucket);
}

/** Append a line to `<state>/logs/<name>.log` (buffered, asynchronous); never throws. */
function appendLog(workspaceRoot, name, message) {
    if (!workspaceRoot || !name) return;
    const bucket = targetOf(workspaceRoot, name);
    bucket.lines.push(`[${new Date().toISOString()}] ${message}\n`);
    if (bucket.lines.length > MAX_PENDING_LINES) bucket.lines.splice(0, bucket.lines.length - MAX_PENDING_LINES);
    scheduleFlush();
    try { console.log(message); } catch (_) { /* ignore */ }
}

/**
 * Write everything still buffered, synchronously. Only for shutdown / fatal paths -- the normal
 * path must stay asynchronous (see the note above).
 */
function flushLogSync() {
    for (const bucket of pendingLines.values()) flushBucket(bucket);
}

module.exports = { checkPython, appendLog, flushLogSync, run };
