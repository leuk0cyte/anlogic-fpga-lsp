// Differential check for F6/R4: the new one-pass comment index must answer exactly like the old
// per-diagnostic scan (`isInsideComment` before the optimisation, copied verbatim below).
//
//   node vscode-extension/test/comment-index-diff.js
//
// Generates random Verilog-ish snippets (comments, block comments, strings, escaped quotes, tabs,
// CRLF, unterminated constructs) and compares both implementations at every (line, column).
// Exits non-zero on the first mismatch.

'use strict';

const path = require('path');
const Module = require('module');
const EXT = path.resolve(__dirname, '..');
// veribleDiagnostics.js requires('vscode'); the extension host provides it, plain node does not.
const mock = require(path.join(EXT, 'test', 'mock-vscode.js'));
const originalLoad = Module._load;
Module._load = function patched(request, parent, isMain) {
    if (request === 'vscode') return mock;
    return originalLoad.call(this, request, parent, isMain);
};
const { buildCommentIndex, isInsideComment } = require(path.join(EXT, 'src', 'veribleDiagnostics.js'));

// ---------------------------------------------------------------------------
// the previous implementation, verbatim (git show HEAD:vscode-extension/src/veribleDiagnostics.js)
// ---------------------------------------------------------------------------
function legacyIsInsideComment(text, line, character) {
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

// ---------------------------------------------------------------------------
// random snippets
// ---------------------------------------------------------------------------
const PIECES = [
    'wire a;', '\twire b;', '    // comment\twith tab', '    /* block\tcomment */',
    'assign x = "a//b";', 'assign y = "/*not a comment*/";', '/* multi', 'line */',
    '    wire c;\t', '\\escaped', '`ifdef X', '`endif', '//', '/*', '*/', '"', '"\\""',
    'module m;', 'endmodule', '\t\t', '    ', 'a // tail', 'b /* mid */ c', '\r',
];

function randomText(rng) {
    const lines = [];
    const count = 1 + Math.floor(rng() * 8);
    for (let i = 0; i < count; i += 1) {
        let row = '';
        const pieces = 1 + Math.floor(rng() * 3);
        for (let j = 0; j < pieces; j += 1) row += PIECES[Math.floor(rng() * PIECES.length)];
        lines.push(row);
    }
    const eol = rng() < 0.3 ? '\r\n' : '\n';
    return lines.join(eol) + (rng() < 0.5 ? eol : '');
}

function mulberry32(seed) {
    let a = seed;
    return function next() {
        a |= 0;
        a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function main() {
    const rng = mulberry32(0xC0FFEE);
    const index = buildCommentIndex;  // alias for readability
    let checked = 0;
    let mismatches = 0;
    const examples = [];
    for (let iteration = 0; iteration < 4000; iteration += 1) {
        const text = randomText(rng);
        const rows = text.split('\n');
        const built = index(text);
        for (let line = -1; line <= rows.length; line += 1) {
            const width = line >= 0 && line < rows.length ? rows[line].length : 0;
            for (let character = -1; character <= width + 2; character += 1) {
                const before = legacyIsInsideComment(text, line, character);
                const after = isInsideComment(built, line, character);
                checked += 1;
                if (before !== after) {
                    mismatches += 1;
                    if (examples.length < 5) {
                        examples.push({ text: JSON.stringify(text), line, character, before, after });
                    }
                }
            }
        }
    }
    console.log(JSON.stringify({ checked, mismatches, examples }, null, 1));
    dumpOptimistic: {
        // Extra: exhaustive over a small alphabet for short texts (catches single-char edge cases).
        const alphabet = ['a', '\t', '/', '*', '"', '\\', '\n', ' ', '\r'];
        let exhaustive = 0;
        let bad = 0;
        const walk = (prefix, depth) => {
            if (depth === 0) {
                const rows = prefix.split('\n');
                const built = index(prefix);
                for (let line = 0; line < rows.length; line += 1) {
                    for (let character = 0; character <= rows[line].length + 1; character += 1) {
                        exhaustive += 1;
                        if (legacyIsInsideComment(prefix, line, character) !== isInsideComment(built, line, character)) {
                            bad += 1;
                        }
                    }
                }
                return;
            }
            for (const ch of alphabet) walk(prefix + ch, depth - 1);
        };
        walk('', 4);
        console.log(JSON.stringify({ exhaustive, bad }));
    }
    process.exit(mismatches === 0 ? 0 : 1);
}

main();