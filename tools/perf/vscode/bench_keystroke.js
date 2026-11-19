// Per-keystroke cost of the extension-host side (the VS Code side of "typing feels laggy").
//
// Two paths matter:
//   1. `veribleDiagnostics.didChange(document)` is wired to `workspace.onDidChangeTextDocument`
//      (extension.js:1037) -> it runs on EVERY keystroke: document.getText() (a full-text copy),
//      texts.set(), clearTimeout+setTimeout(400 ms).
//   2. 400 ms later one `textDocument/didChange` is sent to verible-verilog-ls: JSON.stringify of
//      the whole document + two stdin writes.
// The language client itself also sends incremental didChange to fpga-lsp on every keystroke
// (VS Code API, not our code) -- not measured here, see VSCODE.md §6.
//
//   node --expose-gc bench_keystroke.js [--kb 200]
//
// Output: csv lines + results/keystroke.json

'use strict';

const path = require('path');
const harness = require('./lib/harness');

const mock = harness.installVscodeMock();
const { VeribleDiagnosticsClient } = require(path.join(harness.EXT, 'src', 'veribleDiagnostics.js'));

function parseArgs(argv) {
    let kb = 200;
    for (let i = 0; i < argv.length; i += 1) {
        if (argv[i] === '--kb') { kb = Number(argv[i + 1]); i += 1; }
    }
    return { kb };
}

/** A source file of roughly `kb` KB (real top.v is 13 KB; large SoC files are 200 KB - 1 MB). */
function makeText(kb) {
    const rows = [];
    let bytes = 0;
    let index = 0;
    while (bytes < kb * 1024) {
        const row = `    assign wire_${index} = bus_${index % 64}[${index % 8}] & mask_${index % 32}; // row ${index}\n`;
        rows.push(row);
        bytes += row.length;
        index += 1;
    }
    return rows.join('');
}

function main() {
    const args = parseArgs(process.argv.slice(2));
    const results = { generatedAt: new Date().toISOString(), kb: args.kb, benches: {}, csv: [] };
    const csv = [];
    const before = harness.cpuSnapshot();
    const text = makeText(args.kb);
    results.textBytes = Buffer.byteLength(text);

    // Fake document: getText() is what VS Code does on every call, i.e. materialise the buffer.
    let version = 1;
    const document = {
        uri: mock.Uri.file('C:/perf/src/big.v'),
        languageId: 'verilog',
        get version() { return version; },
        getText: () => text.slice(0),
    };

    // Fake verible child process: absorbs writes, never responds.
    const writes = { frames: 0, bytes: 0 };
    const fakeStdin = {
        writable: true,
        write(chunk) { writes.frames += 1; writes.bytes += chunk.length; return true; },
    };
    const client = new VeribleDiagnosticsClient({
        exe: 'verible-verilog-ls', args: [], cwd: harness.WORKDIR, env: {},
        log: () => undefined,
    });
    client.proc = { stdin: fakeStdin };
    client.ready = true;

    // ---- 1. immediate part of didChange (runs on every keystroke) ------------------------
    const immediate = harness.benchSync(() => {
        version += 1;
        client.didChange(document);
    }, { label: `didChange.immediate(${args.kb}kb)`, rounds: 3, iterations: 21 });
    results.benches.didChangeImmediate = immediate;
    csv.push(...immediate.csv);

    // ---- 2. the 400 ms-later flush: JSON.stringify(whole doc) + 2 stdin writes ------------
    const notify = harness.benchSync(() => {
        client._notify('textDocument/didChange', {
            textDocument: { uri: document.uri.toString(), version },
            contentChanges: [{ text: text }],
        });
    }, { label: `didChange.flush.notify(${args.kb}kb)`, rounds: 3, iterations: 21 });
    results.benches.didChangeFlush = notify;
    csv.push(...notify.csv);
    results.writes = writes;

    // ---- 3. the getText copy alone (so the reader can see what belongs to VS Code) --------
    const copy = harness.benchSync(() => {
        text.slice(0);
    }, { label: `document.getText.copy(${args.kb}kb)`, rounds: 3, iterations: 21 });
    results.benches.getTextCopy = copy;
    csv.push(...copy.csv);

    // ---- 4. diagnostic publish path: per tab diagnostic it scans the file for comments ---
    // (`isInsideComment` is only called for no-tabs diagnostics, but each call walks up to the
    //  reported position across the whole file.)
    const publish = harness.benchSync(() => {
        client._publish({
            uri: document.uri.toString(),
            diagnostics: [{
                range: { start: { line: 10, character: 4 }, end: { line: 10, character: 8 } },
                message: 'Use spaces, not tabs.', severity: 2, code: 'no-tabs',
            }],
        });
    }, { label: `diagnostics.publish.tabs(${args.kb}kb)`, rounds: 3, iterations: 21 });
    results.benches.publishTabs = publish;
    csv.push(...publish.csv);

    const after = harness.cpuSnapshot();
    results.cpuBusy = harness.cpuBusy(before, after);

    // ---- 5. N tab diagnostics in one publish: isInsideComment() re-splits the whole file per
    // diagnostic, so a lint run with many no-tabs findings is O(diagnostics x file size).
    for (const count of [10, 50]) {
        const bench = harness.benchSync(() => {
            const diagnostics = [];
            for (let i = 0; i < count; i += 1) {
                diagnostics.push({
                    range: { start: { line: 5 + i, character: 4 }, end: { line: 5 + i, character: 8 } },
                    message: 'Use spaces, not tabs.', severity: 2, code: 'no-tabs',
                });
            }
            client._publish({ uri: document.uri.toString(), diagnostics });
        }, { label: `diagnostics.publish.${count}tabs(${args.kb}kb)`, rounds: 3, iterations: 11 });
        results.benches[`publish${count}Tabs`] = bench;
        csv.push(...bench.csv);
    }

    results.csv = csv;
    const target = harness.writeJson('keystroke-' + args.kb + 'kb.json', results);
    harness.printTable('keystroke paths (ms)', csv);
    console.log(`\nmachine busy during run: ${(results.cpuBusy * 100).toFixed(1)}%`);
    console.log(`wrote ${target}`);
}

if (require.main === module) main();