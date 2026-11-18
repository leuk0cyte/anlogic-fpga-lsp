// Unit tests for src/locateServer.js (no VS Code, no server needed).
//   node test/locate-server.test.js

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { resolveServerRoot } = require('../src/locateServer');

let failures = 0;
function check(name, ok, detail) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures += 1;
}

function makeServerRoot(dir) {
    const root = path.join(dir, 'root');
    fs.mkdirSync(path.join(root, 'fpgalsp'), { recursive: true });
    fs.writeFileSync(path.join(root, 'fpgalsp', '__main__.py'), 'print("hi")\n');
    return root;
}

function main() {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'fpgal-locate-'));
    try {
        const siblingRoot = makeServerRoot(path.join(tmp, 'dev'));
        const extInDev = path.join(siblingRoot, 'vscode-extension');
        fs.mkdirSync(extInDev, { recursive: true });

        const r1 = resolveServerRoot({ extensionPath: extInDev });
        check('dev layout: extension inside <root>/vscode-extension finds sibling fpgalsp',
            r1.source === 'sibling' && r1.root === siblingRoot, `${r1.source}:${r1.root}`);

        const extPath = path.join(tmp, 'installed', 'fpga-lsp-anlogic-0.2.1');
        fs.mkdirSync(path.join(extPath, 'server', 'fpgalsp'), { recursive: true });
        fs.writeFileSync(path.join(extPath, 'server', 'fpgalsp', '__main__.py'), 'print("hi")\n');
        const r2 = resolveServerRoot({ extensionPath: extPath });
        check('installed .vsix layout: uses the bundled <ext>/server',
            r2.source === 'bundled' && r2.root === path.join(extPath, 'server'), `${r2.source}:${r2.root}`);

        const r3 = resolveServerRoot({ extensionPath: extPath, configured: siblingRoot });
        check('explicit fpgaLsp.serverRoot wins over the bundled copy',
            r3.source === 'setting' && r3.root === siblingRoot, `${r3.source}:${r3.root}`);

        // dev checkout with a stale bundled copy: the live sibling must win
        const devWithBundle = path.join(siblingRoot, 'vscode-extension');
        fs.mkdirSync(path.join(devWithBundle, 'server', 'fpgalsp'), { recursive: true });
        fs.writeFileSync(path.join(devWithBundle, 'server', 'fpgalsp', '__main__.py'), 'stale\n');
        const r6 = resolveServerRoot({ extensionPath: devWithBundle });
        check('dev checkout prefers the live sibling over a bundled snapshot',
            r6.source === 'sibling' && r6.root === siblingRoot, `${r6.source}:${r6.root}`);

        const r4 = resolveServerRoot({ extensionPath: path.join(tmp, 'nowhere') });
        check('nothing found -> source=missing (caller shows a clear error)',
            r4.source === 'missing' && r4.root === '', `${r4.source}`);

        const r5 = resolveServerRoot({ extensionPath: path.join(tmp, 'nowhere'), configured: path.join(tmp, 'bogus') });
        check('invalid setting is reported instead of silently used',
            r5.source === 'setting-invalid' && r5.root === path.join(tmp, 'bogus'), `${r5.source}`);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILED`);
    process.exit(failures === 0 ? 0 : 1);
}

main();
