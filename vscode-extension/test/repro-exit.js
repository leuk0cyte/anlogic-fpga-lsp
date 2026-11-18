// Reproduce *exactly* how the extension launches the server, without VS Code:
// same resolveServerRoot() decision, same args/env/cwd, then the LSP handshake.
// Prints the exit code and the server's own log tail when it dies early.
//
//   node test/repro-exit.js [project-dir] [extensionPath]
//
// extensionPath defaults to the repo's vscode-extension dir (dev layout).

'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const { resolveServerRoot } = require('../src/locateServer');
const { METHODS, COMMANDS } = require('../src/protocol');

const PROJECT = process.argv[2] || 'Y:\\CorpusAlpha';
const EXT_PATH = process.argv[3] || path.resolve(__dirname, '..');
const logPath = path.join(PROJECT, '.fpgalsp', 'logs', 'repro-server.log');

function line(s) { console.log(s); }

function main() {
    const resolved = resolveServerRoot({ extensionPath: EXT_PATH, configured: '' });
    line(`extensionPath : ${EXT_PATH}`);
    line(`server root   : ${resolved.root}  (source=${resolved.source})`);
    if (!resolved.root) {
        line('FATAL: server root not found (this is what a mis-packaged vsix would hit)');
        process.exit(2);
    }
    const serverRoot = resolved.root;
    const env = Object.assign({}, process.env);
    env.PYTHONPATH = [serverRoot, env.PYTHONPATH].filter(Boolean).join(path.delimiter);
    env.PYTHONIOENCODING = 'utf-8';
    env.PYTHONUNBUFFERED = '1';
    const args = ['-m', 'fpgalsp', '--stdio', '--log', logPath];
    line(`spawn         : python ${args.join(' ')}`);
    line(`cwd           : ${PROJECT}`);
    line(`PYTHONPATH    : ${env.PYTHONPATH}`);

    const proc = spawn('python', args, {
        cwd: PROJECT,
        env,
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    let buffer = Buffer.alloc(0);
    let exited = null;
    const pending = new Map();
    const notices = [];

    proc.on('exit', (code, signal) => {
        exited = { code, signal };
        line(`EXIT          : code=${code} signal=${signal}`);
        const tail = readLogTail(5);
        if (tail.length) {
            line('server log tail:');
            tail.forEach((l) => line('  ' + l));
        }
    });
    proc.on('error', (err) => line(`SPAWN ERROR   : ${err.message}`));
    proc.stderr.on('data', (d) => process.stderr.write('[stderr] ' + d));
    proc.stdout.on('data', (chunk) => {
        buffer = Buffer.concat([buffer, chunk]);
        for (;;) {
            const sep = buffer.indexOf('\r\n\r\n');
            if (sep < 0) return;
            const header = buffer.slice(0, sep).toString('ascii');
            const m = /Content-Length:\s*(\d+)/i.exec(header);
            if (!m) { buffer = buffer.slice(sep + 4); continue; }
            const len = parseInt(m[1], 10);
            if (buffer.length < sep + 4 + len) return;
            const body = buffer.slice(sep + 4, sep + 4 + len).toString('utf8');
            buffer = buffer.slice(sep + 4 + len);
            let msg;
            try { msg = JSON.parse(body); } catch { continue; }
            if (msg.method && msg.id === undefined) {
                notices.push(msg.method);
                if (msg.method === 'textDocument/publishDiagnostics') {
                    const ds = (msg.params && msg.params.diagnostics) || [];
                    line(`  <- publishDiagnostics ${path.basename((msg.params && msg.params.uri) || '')} (${ds.length})`);
                }
            } else if (msg.id !== undefined && pending.has(msg.id)) {
                const resolve = pending.get(msg.id);
                pending.delete(msg.id);
                resolve(msg);
            }
        }
    });

    function send(method, params, id) {
        const payload = JSON.stringify(Object.assign({ jsonrpc: '2.0' }, id === undefined ? {} : { id }, { method, params }));
        proc.stdin.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
    }
    function request(method, params, timeoutMs = 15000) {
        return new Promise((resolve) => {
            const id = ++global.__id;
            pending.set(id, resolve);
            send(method, params, id);
            setTimeout(() => { if (pending.delete(id)) resolve(null); }, timeoutMs);
        });
    }
    global.__id = 0;

    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

    (async () => {
        await sleep(1200);
        line(`alive after 1.2s: ${exited ? 'NO' : 'yes'}`);
        if (exited) return finish(false);

        const init = await request('initialize', {
            processId: process.pid,
            clientInfo: { name: 'repro', version: '1' },
            rootUri: 'file:///' + PROJECT.replace(/\\/g, '/'),
            capabilities: { workspace: { workspaceFolders: true } },
            workspaceFolders: [{ uri: 'file:///' + PROJECT.replace(/\\/g, '/'), name: path.basename(PROJECT) }],
            initializationOptions: { fpgaLsp: { tdCheckOnSave: false } },
        });
        line(`initialize    : ${init && init.result ? 'ok' : 'FAILED ' + JSON.stringify(init && init.error)}`);
        if (!init || !init.result) return finish(false);
        send('initialized', {});

        await sleep(300);
        line(`alive after init: ${exited ? 'NO' : 'yes'}`);
        if (exited) return finish(false);

        const tree = await request(METHODS.moduleTree, {});
        line(`moduleTree    : ${tree && tree.result && tree.result.ok ? 'ok root=' + tree.result.root.module : 'FAILED'}`);

        // open a Verilog file like an editor would
        const vfile = findVerilog(PROJECT);
        if (vfile) {
            const text = fs.readFileSync(vfile, 'utf8');
            send('textDocument/didOpen', { textDocument: { uri: 'file:///' + vfile.replace(/\\/g, '/'), languageId: 'verilog', version: 1, text } });
            await sleep(1500);
            line(`alive after didOpen: ${exited ? 'NO' : 'yes'}`);
        }
        await sleep(500);
        if (exited) return finish(false);

        await request('shutdown', null, 5000).catch(() => undefined);
        send('exit', {});
        await sleep(800);
        line('done');
        return finish(!exited || exited.code === 0);
    })().catch((err) => { line('harness error: ' + err.stack); finish(false); });

    function finish(ok) {
        try { proc.kill(); } catch (_) { /* already gone */ }
        line(ok ? '\nRESULT: PASS' : '\nRESULT: FAIL (server did not stay up)');
        process.exit(ok ? 0 : 1);
    }

    function readLogTail(n) {
        try {
            const lines = fs.readFileSync(logPath, 'utf8').trim().split('\n');
            return lines.slice(-n);
        } catch (_) {
            return [];
        }
    }
}

function findVerilog(dir) {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const e of entries) {
        const p = path.join(dir, e.name);
        if (e.isDirectory() && e.name !== '.fpgalsp') {
            const found = findVerilog(p);
            if (found) return found;
        } else if (e.isFile() && e.name.endsWith('.v')) {
            return p;
        }
    }
    return null;
}

main();
