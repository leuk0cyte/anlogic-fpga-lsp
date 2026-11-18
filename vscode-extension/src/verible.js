// Optional Verible integration for the VS Code client.
//
// Two independent pieces:
//   * formatting  — handled by the Python server (`textDocument/formatting` -> verible-verilog-format),
//                   so any LSP client benefits; the extension only forwards the path settings;
//   * language server — `verible-verilog-ls` can additionally be started as a *second* client for
//                   lint/quick-fixes. It is off by default; diagnostics are tagged by source.
//
// Kept dependency-free (no `vscode` import) so it can be unit-tested with plain node.

'use strict';

const fs = require('fs');
const path = require('path');

const FORMATTER = 'verible-verilog-format';
const LANGUAGE_SERVER = 'verible-verilog-ls';

function isFile(candidate) {
    try {
        return fs.statSync(candidate).isFile();
    } catch (_) {
        return false;
    }
}

/** Resolve a Verible binary: explicit dir/exe first, then PATH. */
function findBinary(name, configured) {
    const exe = process.platform === 'win32' ? `${name}.exe` : name;
    const candidates = [];
    if (configured) {
        const clean = String(configured).replace(/\\/g, '/').replace(/\/+$/, '');
        if (/\.exe$/i.test(clean)) candidates.push(path.resolve(clean));
        candidates.push(path.join(clean, exe), path.join(clean, name));
    }
    // PATH next (an entry there is an explicit user choice), home/common installs last
    for (const dir of String(process.env.PATH || '').split(path.delimiter)) {
        if (!dir) continue;
        candidates.push(path.join(dir, exe), path.join(dir, name));
    }
    const home = process.env.USERPROFILE || process.env.HOME || '';
    const common = ['verible', 'verible/bin', 'tools/verible', 'tools/verible/bin']
        .map((relative) => (home ? path.join(home, relative) : ''))
        .concat(['C:/verible/bin', 'C:/tools/verible/bin', 'C:/Program Files/verible/bin']);
    for (const dir of common) {
        if (!dir) continue;
        candidates.push(path.join(dir, exe), path.join(dir, name));
    }
    return candidates.find(isFile) || '';
}

/**
 * @param {{veriblePath?: string}} options
 * @returns {{formatter: string, languageServer: string, configured: string}}
 */
function resolveVerible(options) {
    const configured = (options && options.veriblePath) || '';
    return {
        configured,
        formatter: findBinary(FORMATTER, configured),
        languageServer: findBinary(LANGUAGE_SERVER, configured),
    };
}

module.exports = { findBinary, resolveVerible, FORMATTER, LANGUAGE_SERVER };
