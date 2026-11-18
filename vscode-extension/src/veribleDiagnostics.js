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

function isInsideComment(text, line, character) {
    if (!text) return false;
    const rows = text.split('\n');
    if (line < 0 || line >= rows.length) return false;
    let inBlock = false;
    let inString = false;
    let escaped = false;
    for (let index = 0; index < rows.length; index += 1) {
        const row = rows[index];
        const limit = index === line ? Math.max(0, Math.min(row.length, character)) : row.length;
        for (let i = 0; i < limit; i += 1) {
            const ch = row[i];
            const next = i + 1 < limit ? row[i + 1] : '';
            if (inBlock) {
                if (ch === '*' && next === '/') {
                    inBlock = false;
                    i += 1;
                }
                continue;
            }
            if (inString) {
                if (escaped) escaped = false;
                else if (ch === '\\') escaped = true;
                else if (ch === '"') inString = false;
                continue;
            }
            if (ch === '"') { inString = true; continue; }
            if (ch === '/' && next === '*') {
                if (index === line) return true;
                inBlock = true;
                i += 1;
                continue;
            }
            if (ch === '/' && next === '/') {
                if (index === line) return true;
                i = limit;
            }
        }
        if (index === line && inBlock) return true;
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
        for (const item of params.diagnostics || []) {
            const range = item.range || { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } };
            const message = item.message || '';
            const code = String(item.code || '');
            // Verible flags tabs in comments too, but the formatter leaves comments untouched.
            // Keep no-tabs for real code while suppressing that comment-only false positive.
            const tabsIssue = /no-tabs|tabs?/i.test(message) || /no-tabs/i.test(code);
            if (tabsIssue && isInsideComment(text, range.start.line, range.start.character)) {
                continue;
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

module.exports = { VeribleDiagnosticsClient };
