// Integration check: the *real* launch path the extension uses (Node child_process <-> Python LSP),
// plus the v2 custom requests. No VS Code needed — this catches framing/encoding/launch bugs.
//
//   node test/integration-server.js [project-dir] [server-root]
//
// Exit 0 = every assertion passed.

const { spawn } = require('child_process');
const path = require('path');

const PROJECT = process.argv[2] || 'Y:\\CorpusAlpha';
const SERVER_ROOT = process.argv[3] || path.resolve(__dirname, '..', '..');
const PROTO = require('../src/protocol');

let failures = 0;
function check(name, ok, detail) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures += 1;
}

function uri(p) {
    return 'file:///' + p.replace(/\\/g, '/');
}

class Server {
    constructor() {
        this.counter = 0;
        this.pending = new Map();
        this.buffer = Buffer.alloc(0);
        this.proc = spawn('python', ['-m', 'fpgalsp', '--stdio'], {
            cwd: PROJECT,
            env: { ...process.env, PYTHONPATH: SERVER_ROOT, PYTHONIOENCODING: 'utf-8', PYTHONUNBUFFERED: '1' },
            stdio: ['pipe', 'pipe', 'pipe'],
        });
        this.proc.stdout.on('data', (chunk) => this._onData(chunk));
        this.proc.stderr.on('data', (chunk) => process.stderr.write(chunk));
        this.proc.on('exit', (code) => {
            for (const [, resolve] of this.pending) resolve(null);
            this.pending.clear();
            if (code !== 0 && code !== null) process.stderr.write(`server exited ${code}\n`);
        });
    }

    _onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        for (;;) {
            const sep = this.buffer.indexOf('\r\n\r\n');
            if (sep < 0) return;
            const header = this.buffer.slice(0, sep).toString('ascii');
            const m = /Content-Length:\s*(\d+)/i.exec(header);
            if (!m) { this.buffer = this.buffer.slice(sep + 4); continue; }
            const length = parseInt(m[1], 10);
            if (this.buffer.length < sep + 4 + length) return;
            const body = this.buffer.slice(sep + 4, sep + 4 + length).toString('utf8');
            this.buffer = this.buffer.slice(sep + 4 + length);
            let msg;
            try { msg = JSON.parse(body); } catch { continue; }
            if (msg.id !== undefined && this.pending.has(msg.id)) {
                const resolve = this.pending.get(msg.id);
                this.pending.delete(msg.id);
                resolve(msg);
            }
        }
    }

    request(method, params, timeoutMs = 60000) {
        const id = ++this.counter;
        const payload = JSON.stringify({ jsonrpc: '2.0', id, method, params });
        this.proc.stdin.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
        return new Promise((resolve) => {
            const timer = setTimeout(() => { this.pending.delete(id); resolve(null); }, timeoutMs);
            this.pending.set(id, (msg) => { clearTimeout(timer); resolve(msg); });
        });
    }

    notify(method, params) {
        const payload = JSON.stringify({ jsonrpc: '2.0', method, params });
        this.proc.stdin.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
    }

    stop() {
        this.notify('exit');
        setTimeout(() => this.proc.kill(), 500);
    }
}

async function main() {
    const server = new Server();
    try {
        const init = await server.request('initialize', {
            processId: process.pid,
            rootUri: uri(PROJECT),
            capabilities: {},
            initializationOptions: { fpgaLsp: { tdCheckOnSave: false } },
        });
        const caps = (init && init.result && init.result.capabilities) || {};
        check('initialize over Node pipe', !!caps.textDocumentSync);
        server.notify('initialized', {});

        const treeMsg = await server.request(PROTO.METHODS.moduleTree, {});
        const tree = (treeMsg && treeMsg.result) || {};
        check('fpgaLsp/moduleTree', tree.ok === true && !!tree.root,
            `root=${tree.root && tree.root.module} modules=${tree.modulesSeen}`);
        const flat = [];
        (function walk(node) { if (!node) return; flat.push(node); (node.children || []).forEach(walk); })(tree.root);
        check('tree nodes carry file+range', flat.length > 3 && flat.every((n) => n.file && n.range));

        const reportsMsg = await server.request(PROTO.METHODS.reports, {});
        const reports = (reportsMsg && reportsMsg.result) || {};
        check('fpgaLsp/reports answers (ok or a friendly error)', typeof reports.ok === 'boolean', reports.ok ? 'has data' : reports.error);

        const statusMsg = await server.request(PROTO.METHODS.flowStatus, {});
        const status = (statusMsg && statusMsg.result) || {};
        check('fpgaLsp/flowStatus', !!(status.flow && status.flow.running === false));

        const infoMsg = await server.request('workspace/executeCommand', { command: 'fpgalsp.info', arguments: [] });
        const info = (infoMsg && infoMsg.result) || {};
        check('fpgalsp.info', info.ok === true, `${info.project} ${info.device} top=${info.top}`);

        const shutdown = await server.request('shutdown', null, 5000);
        check('shutdown', shutdown !== null);
    } finally {
        server.stop();
    }
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILED`);
    process.exit(failures === 0 ? 0 : 1);
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});
