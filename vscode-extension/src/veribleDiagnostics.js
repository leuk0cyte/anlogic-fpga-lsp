// Minimal, diagnostics-only LSP client for `verible-verilog-ls`.
//
// Why not vscode-languageclient? Registering Verible's LSP as a normal language client also
// exposes its formatting/rename/definition providers, which collide with fpga-lsp's own providers
// (two identically labelled formatters, rename returning nothing, ...). We only want Verible's lint
// diagnostics, so this client speaks just enough LSP for that: initialize, didOpen/didChange/
// didClose and textDocument/publishDiagnostics.

'use strict';

const { spawn } = require('child_process');
const vscode = require('vscode');

const SEVERITY = {
    1: vscode.DiagnosticSeverity.Error,
    2: vscode.DiagnosticSeverity.Warning,
    3: vscode.DiagnosticSeverity.Information,
    4: vscode.DiagnosticSeverity.Hint,
};

/**
 * One-pass comment index for a document.
 *
 * F6/R4: `_publish()` used to call `isInsideComment(text, line, character)` once **per
 * diagnostic**, and that function began with `text.split('\n')`. A lint run with N findings on an
 * M-byte file therefore cost N x O(M) (50 no-tabs findings on 1 MB measured 405 ms of
 * extension-host time, see audit/2026-11-19/perf/VSCODE.md §P3). Building the index once per
 * publish reduces that to O(M) + O(N * k) with k = number of comments on the line.
 *
 * The predicate is the same one the old per-diagnostic scan implemented:
 *   inside(line, character)
 *     = a comment was *opened* earlier on this line   (an opener is only recognised with one
 *                                                      character of lookahead, hence `start < column - 1`)
 *       OR the position itself is inside a comment     (a comment that starts on this line also
 *                                                      needs the lookahead: `column >= start + 2`)
 * Strings are tracked so `//` or "/*" inside a string literal is not treated as a comment start.
 *
 * @param {string} text
 * @returns {{lines: Array<{length:number, starts:number[], segments:Array<[number,number,boolean]>}>}}
 *   `segments` entries are `[fromColumn, toColumnExclusive, opensOnThisLine]`.
 */
function buildCommentIndex(text) {
    const lines = [];
    let line = { length: 0, starts: [], segments: [] };
    lines.push(line);
    let column = 0;
    let state = 'code';      // 'code' | 'line' | 'block' | 'string'
    let escaped = false;
    let segmentStart = -1;   // column where the comment open on this line starts
    let segmentOpensHere = false;

    const openSegment = (start, opensHere) => {
        segmentStart = start;
        segmentOpensHere = opensHere;
    };
    const closeSegment = (end) => {
        if (segmentStart >= 0) {
            line.segments.push([segmentStart, end, segmentOpensHere]);
            segmentStart = -1;
            segmentOpensHere = false;
        }
    };

    for (let i = 0; i < text.length; i += 1) {
        const ch = text[i];
        const next = i + 1 < text.length ? text[i + 1] : '';
        if (ch === '\n') {
            // A line comment ends here; a block comment carries over, so on this line it covers
            // every column up to (and including) the end of the line.
            closeSegment(state === 'block' ? Infinity : column);
            if (state === 'line') state = 'code';
            line.length = column;
            if (state === 'block') openSegment(0, false);
            line = { length: 0, starts: [], segments: [] };
            lines.push(line);
            column = 0;
            continue;
        }
        if (state === 'block') {
            if (ch === '*' && next === '/') {
                closeSegment(column + 2);
                state = 'code';
                column += 2;
                i += 1;
                continue;
            }
            column += 1;
            continue;
        }
        if (state === 'string') {
            if (escaped) escaped = false;
            else if (ch === '\\') escaped = true;
            else if (ch === '"') state = 'code';
            column += 1;
            continue;
        }
        if (state === 'line') {
            column += 1;
            continue;
        }
        // state === 'code'
        if (ch === '"') {
            state = 'string';
            column += 1;
            continue;
        }
        if (ch === '/' && next === '*') {
            line.starts.push(column);
            openSegment(column, true);
            state = 'block';
            column += 2;
            i += 1;
            continue;
        }
        if (ch === '/' && next === '/') {
            line.starts.push(column);
            openSegment(column, true);
            state = 'line';
            column += 2;
            i += 1;
            continue;
        }
        column += 1;
    }
    // A block comment that is still open at EOF covers the rest of the last line, which has no
    // finite end column (the old scanner reported `inBlock` at any column there).
    closeSegment(state === 'block' ? Infinity : column);
    line.length = column;
    return { lines };
}

/** `true` when (line, character) sits inside a Verilog comment according to `index`. */
function isInsideComment(index, line, character) {
    const row = index && index.lines[line];
    if (!row) return false;
    const column = Math.max(0, Math.min(row.length, character));
    for (const start of row.starts) {
        // An opener needs one character of lookahead to be seen by the old scanner.
        if (start < column - 1) return true;
    }
    for (const [from, to, opensHere] of row.segments) {
        if (column >= to) continue;
        if (opensHere ? column >= from + 2 : column >= from) return true;
    }
    return false;
}

class VeribleDiagnosticsClient {
    constructor(options) {
        this.exe = options.exe;
        this.args = options.args || [];
        this.cwd = options.cwd;
        this.env = options.env;
        this.log = options.log || (() => undefined);
        this.collection = vscode.languages.createDiagnosticCollection('verible');
        this.proc = undefined;
        this.buffer = Buffer.alloc(0);
        this.nextId = 1;
        this.pending = new Map();
        this.ready = false;
        this.timers = new Map();
        this.texts = new Map();
    }

    async start() {
        this.proc = spawn(this.exe, this.args, {
            cwd: this.cwd,
            env: this.env,
            stdio: ['pipe', 'pipe', 'pipe'],
            windowsHide: true,
        });
        this.proc.stdout.on('data', (chunk) => this._onData(chunk));
        this.proc.stderr.on('data', (chunk) => this.log('verible ls stderr: ' + chunk.toString().slice(0, 400)));
        this.proc.on('exit', (code) => {
            this.log(`verible ls exited (${code})`);
            this.ready = false;
            for (const resolver of this.pending.values()) resolver(undefined);
            this.pending.clear();
        });
        this.proc.on('error', (err) => this.log('verible ls spawn error: ' + err.message));

        const root = (vscode.workspace.workspaceFolders || [])[0];
        const result = await this._request('initialize', {
            processId: process.pid,
            clientInfo: { name: 'fpga-lsp', version: '1' },
            rootUri: root ? root.uri.toString() : null,
            workspaceFolders: root ? [{ uri: root.uri.toString(), name: root.name }] : null,
            capabilities: {
                textDocument: {
                    publishDiagnostics: { relatedInformation: true, versionSupport: true },
                },
                workspace: { configuration: true },
            },
            initializationOptions: null,
            trace: 'off',
        });
        this.ready = Boolean(result);
        if (!this.ready) {
            this.log('verible ls initialize did not return a result');
            return false;
        }
        this._notify('initialized', {});
        this.log('verible diagnostics client ready: ' + this.exe);
        return true;
    }

    _send(message) {
        if (!this.proc || !this.proc.stdin.writable) return;
        const payload = Buffer.from(JSON.stringify(message), 'utf8');
        this.proc.stdin.write(`Content-Length: ${payload.length}\r\n\r\n`);
        this.proc.stdin.write(payload);
    }

    _notify(method, params) {
        this._send({ jsonrpc: '2.0', method, params });
    }

    _request(method, params) {
        return new Promise((resolve) => {
            const id = this.nextId++;
            this.pending.set(id, resolve);
            this._send({ jsonrpc: '2.0', id, method, params });
            setTimeout(() => {
                if (this.pending.has(id)) {
                    this.pending.delete(id);
                    resolve(undefined);
                }
            }, 10000);
        });
    }

    _onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        for (;;) {
            const headerEnd = this.buffer.indexOf('\r\n\r\n');
            if (headerEnd < 0) return;
            const header = this.buffer.slice(0, headerEnd).toString('ascii');
            const match = /Content-Length:\s*(\d+)/i.exec(header);
            if (!match) {
                this.buffer = this.buffer.slice(headerEnd + 4);
                continue;
            }
            const length = Number(match[1]);
            if (this.buffer.length < headerEnd + 4 + length) return;
            const body = this.buffer.slice(headerEnd + 4, headerEnd + 4 + length).toString('utf8');
            this.buffer = this.buffer.slice(headerEnd + 4 + length);
            try {
                this._handle(JSON.parse(body));
            } catch (err) {
                this.log('verible ls message parse error: ' + ((err && err.message) || err));
            }
        }
    }

    _handle(message) {
        if (message.id !== undefined && message.method === undefined) {
            const resolver = this.pending.get(message.id);
            if (resolver) {
                this.pending.delete(message.id);
                resolver(message.result);
            }
            return;
        }
        if (message.method === 'textDocument/publishDiagnostics') {
            this._publish(message.params || {});
            return;
        }
        if (message.method === 'workspace/configuration' && message.id !== undefined) {
            const items = (message.params && message.params.items) || [];
            this._send({ jsonrpc: '2.0', id: message.id, result: items.map(() => ({})) });
            return;
        }
        if (message.method === 'window/showMessage') {
            this.log('verible ls: ' + ((message.params && message.params.message) || ''));
            return;
        }
        if (message.id !== undefined) {
            // answer unknown server requests so it never blocks waiting for us
            this._send({ jsonrpc: '2.0', id: message.id, result: null });
        }
    }

    _publish(params) {
        const uri = params.uri;
        if (!uri) return;
        const text = this.texts.get(uri) || '';
        const diagnostics = [];
        // F6/R4: build the comment index lazily, at most once per publish (it used to be rebuilt
        // -- including a full `split('\n')` -- for every single diagnostic).
        let commentIndex;
        for (const item of params.diagnostics || []) {
            const range = item.range || { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } };
            const message = item.message || '';
            const code = String(item.code || '');
            // Verible flags tabs in comments too, but the formatter leaves comments untouched.
            // Keep no-tabs for real code while suppressing that comment-only false positive.
            const tabsIssue = /no-tabs|tabs?/i.test(message) || /no-tabs/i.test(code);
            if (tabsIssue) {
                if (!commentIndex) commentIndex = buildCommentIndex(text);
                if (isInsideComment(commentIndex, range.start.line, range.start.character)) continue;
            }
            const diagnostic = new vscode.Diagnostic(
                new vscode.Range(range.start.line, range.start.character, range.end.line, range.end.character),
                message,
                SEVERITY[item.severity] || vscode.DiagnosticSeverity.Warning
            );
            diagnostic.source = item.source || 'verible';
            if (item.code !== undefined) diagnostic.code = item.code;
            diagnostics.push(diagnostic);
        }
        this.collection.set(vscode.Uri.parse(uri), diagnostics);
    }

    didOpen(document) {
        if (!this.ready) return;
        this.texts.set(document.uri.toString(), document.getText());
        this._notify('textDocument/didOpen', {
            textDocument: {
                uri: document.uri.toString(),
                languageId: document.languageId,
                version: document.version,
                text: document.getText(),
            },
        });
    }

    didChange(document) {
        if (!this.ready) return;
        const key = document.uri.toString();
        if (this.timers.has(key)) clearTimeout(this.timers.get(key));
        this.texts.set(key, document.getText());
        this.timers.set(key, setTimeout(() => {
            this.timers.delete(key);
            this._notify('textDocument/didChange', {
                textDocument: { uri: key, version: document.version },
                contentChanges: [{ text: document.getText() }],
            });
        }, 400));
    }

    didClose(document) {
        if (!this.ready) return;
        const key = document.uri.toString();
        if (this.timers.has(key)) {
            clearTimeout(this.timers.get(key));
            this.timers.delete(key);
        }
        this.texts.delete(key);
        this._notify('textDocument/didClose', { textDocument: { uri: key } });
        this.collection.delete(document.uri);
    }

    dispose() {
        for (const timer of this.timers.values()) clearTimeout(timer);
        this.timers.clear();
        this.collection.dispose();
        if (!this.proc) return;
        try {
            this._notify('shutdown', {});
            this._notify('exit', {});
        } catch (_) { /* ignore */ }
        const proc = this.proc;
        this.proc = undefined;
        setTimeout(() => {
            try { proc.kill(); } catch (_) { /* ignore */ }
        }, 500);
    }
}

// F6/R4: the two pure helpers are exported so a differential check can compare them against the
// previous per-diagnostic scan (they are not part of the extension's public surface otherwise).
module.exports = { VeribleDiagnosticsClient, buildCommentIndex, isInsideComment };
