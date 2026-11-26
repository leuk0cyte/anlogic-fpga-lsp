// Prepare a %TEMP% workspace copy for the extension-side benchmarks.
//
// The extension itself writes `<workspace>/.fpgalsp/logs/extension.log` on every `diag()` call
// (sync fs append) and the highlight-report path writes `.fpgalsp/logs/highlight.txt`. The perf
// rules forbid writing to C:/work/CorpusAlpha, so the benchmarks run against this copy:
//   %TEMP%\fpgalsp-perf\vscode\project   <- tests/gate-corpus/corpus_alpha (byte-identical corpus)
//
//   node prep_workspace.js

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./lib/harness');

function main() {
    const source = path.join(harness.REPO, 'tests', 'gate-corpus', 'corpus_alpha');
    const target = path.join(harness.WORKDIR, 'project');
    if (!fs.existsSync(source)) throw new Error(`corpus missing: ${source}`);
    fs.rmSync(target, { recursive: true, force: true });
    fs.cpSync(source, target, { recursive: true });
    let files = 0;
    let bytes = 0;
    const walk = (dir) => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) walk(full);
            else { files += 1; bytes += fs.statSync(full).size; }
        }
    };
    walk(target);
    console.log(JSON.stringify({ source, target, files, bytes }, null, 1));
}

if (require.main === module && require.main.filename === __filename) main();
module.exports = { main };