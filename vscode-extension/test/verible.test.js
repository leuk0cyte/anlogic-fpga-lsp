// Unit tests for src/verible.js (no VS Code, no real Verible needed).
//   node test/verible.test.js

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { findBinary, resolveVerible } = require('../src/verible');

let failures = 0;
function check(name, ok, detail) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures += 1;
}

function main() {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'fpgal-verible-'));
    const originalPath = process.env.PATH;
    try {
        const exe = process.platform === 'win32' ? 'verible-verilog-format.exe' : 'verible-verilog-format';
        const lsExe = process.platform === 'win32' ? 'verible-verilog-ls.exe' : 'verible-verilog-ls';
        fs.writeFileSync(path.join(tmp, exe), '');
        fs.writeFileSync(path.join(tmp, lsExe), '');

        const found = findBinary('verible-verilog-format', tmp);
        check('findBinary resolves from an explicit directory',
            path.resolve(found || '') === path.resolve(path.join(tmp, exe)), found || '(not found)');

        const resolved = resolveVerible({ veriblePath: tmp });
        check('resolveVerible finds formatter + language server',
            path.resolve(resolved.formatter || '') === path.resolve(path.join(tmp, exe))
                && path.resolve(resolved.languageServer || '') === path.resolve(path.join(tmp, lsExe)),
            `${path.basename(resolved.formatter || '-')}, ${path.basename(resolved.languageServer || '-')}`);

        // full .exe path configured directly
        const direct = resolveVerible({ veriblePath: path.join(tmp, exe) });
        check('configured full .exe path is accepted',
            path.resolve(direct.formatter || '') === path.resolve(path.join(tmp, exe)),
            direct.formatter || '(none)');

        // PATH fallback
        process.env.PATH = `${tmp}${path.delimiter}${originalPath || ''}`;
        const fromPath = findBinary('verible-verilog-format', '');
        check('findBinary falls back to PATH', fromPath === path.join(tmp, exe), fromPath || '(none)');

        process.env.PATH = originalPath;
        // absent binary -> empty string (graceful degradation, no throw)
        check('missing binary returns empty string', findBinary('verible-does-not-exist-xyz', '') === '');
        // isolate home/common installs so a real Verible on this machine cannot satisfy the lookup
        const originalHome = process.env.USERPROFILE;
        const originalPosixHome = process.env.HOME;
        process.env.USERPROFILE = path.join(tmp, 'no-home');
        process.env.HOME = path.join(tmp, 'no-home');
        try {
            check('resolveVerible without verible does not throw',
                resolveVerible({ veriblePath: path.join(tmp, 'nope') }).formatter === '');
        } finally {
            process.env.USERPROFILE = originalHome;
            process.env.HOME = originalPosixHome;
        }
    } finally {
        process.env.PATH = originalPath;
        fs.rmSync(tmp, { recursive: true, force: true });
    }
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILED`);
    process.exit(failures === 0 ? 0 : 1);
}

main();
