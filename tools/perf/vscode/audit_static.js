// Static audit of the plugin-side "per event does O(N)" suspects.
//
// This is the C-evidence half of the report: every hit carries file:line so the numbers in
// VSCODE.md can be traced to code. It does not measure anything (bench_*.js do that).
//
//   node audit_static.js
//
// Output: results/static-audit.json + a readable summary on stdout.

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./lib/harness');

const ROOT = harness.EXT;

function listSources(dir, out) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) {
            if (entry.name === 'node_modules' || entry.name === '.git') continue;
            if (entry.name === 'test') continue;  // tests are not shipped
            listSources(full, out);
        } else if (entry.name.endsWith('.js')) {
            out.push(full);
        }
    }
    return out;
}

const PATTERNS = [
    { key: 'timer', re: /\bsetInterval\s*\(/, note: 'polling timer' },
    { key: 'timeout', re: /\bsetTimeout\s*\(/, note: 'one-shot timer / debounce' },
    { key: 'clearTimer', re: /\bclear(Interval|Timeout)\s*\(/, note: 'timer cleanup' },
    { key: 'eventSub', re: /\.onDid[A-Za-z]+\s*\(/, note: 'vscode event subscription' },
    { key: 'notification', re: /onNotification\s*\(/, note: 'server notification handler' },
    { key: 'jsonParse', re: /JSON\.parse\s*\(/, note: 'JSON parse (main thread)' },
    { key: 'jsonStringify', re: /JSON\.stringify\s*\(/, note: 'JSON stringify (main thread)' },
    { key: 'syncIo', re: /\b(fs\.)?(appendFileSync|writeFileSync|readFileSync|mkdirSync|existsSync)\s*\(/, note: 'synchronous fs' },
    { key: 'fireTree', re: /\.fire\s*\(\s*undefined\s*\)/, note: 'tree invalidation (full re-query)' },
    { key: 'refresh', re: /\b(refresh|requestRefresh|scheduleModuleRefresh)\s*\(/, note: 'refresh entry point' },
];

function scan() {
    const files = listSources(ROOT, []).sort();
    const hits = [];
    for (const file of files) {
        const rel = path.relative(harness.REPO, file).replace(/\\/g, '/');
        const lines = fs.readFileSync(file, 'utf8').split('\n');
        lines.forEach((line, index) => {
            for (const pattern of PATTERNS) {
                if (pattern.re.test(line)) {
                    hits.push({ file: rel, line: index + 1, key: pattern.key, note: pattern.note, text: line.trim().slice(0, 200) });
                }
            }
        });
    }
    return { files, hits };
}

function summarize(hits) {
    const counts = {};
    for (const hit of hits) counts[hit.key] = (counts[hit.key] || 0) + 1;
    return counts;
}

function registrations() {
    // What `activate()` subscribes / registers: counted from the source because the mock run
    // (bench_activate.js) already gives the authoritative 57 subscriptions.
    const extension = path.join(ROOT, 'extension.js');
    const lines = fs.readFileSync(extension, 'utf8').split('\n');
    const rows = [];
    lines.forEach((line, index) => {
        if (/context\.subscriptions\.push\s*\(/.test(line)) {
            rows.push({ file: 'vscode-extension/extension.js', line: index + 1, text: line.trim().slice(0, 160) });
        }
    });
    return rows;
}

function duplicateNotifications() {
    // Same notification method registered twice in activate() would double the per-event work.
    const extension = path.join(ROOT, 'extension.js');
    const text = fs.readFileSync(extension, 'utf8');
    const methods = {};
    for (const match of text.matchAll(/onNotification\(\s*METHODS\.([A-Za-z]+)/g)) {
        methods[match[1]] = (methods[match[1]] || 0) + 1;
    }
    return methods;
}

function pollingTimers(hits) {
    // Every setInterval in the shipped code, with its period (from the same line or the next few).
    const rows = [];
    for (const hit of hits.filter((entry) => entry.key === 'timer')) {
        const file = path.join(harness.REPO, hit.file);
        const lines = fs.readFileSync(file, 'utf8').split('\n');
        const window = lines.slice(hit.line - 1, hit.line + 2).join(' ');
        const period = /(\b\d{3,6}\b)/.exec(window);
        rows.push({ ...hit, window: window.replace(/\s+/g, ' ').slice(0, 240), periodMs: period ? Number(period[1]) : null });
    }
    return rows;
}

function main() {
    const scanResult = scan();
    const result = {
        generatedAt: new Date().toISOString(),
        scannedFiles: scanResult.files.map((file) => path.relative(harness.REPO, file).replace(/\\/g, '/')),
        counts: summarize(scanResult.hits),
        hits: scanResult.hits,
        intervals: pollingTimers(scanResult.hits),
        subscriptions: registrations(),
        notificationRegistrations: duplicateNotifications(),
    };
    const target = harness.writeJson('static-audit.json', result);
    console.log(JSON.stringify({ counts: result.counts, intervals: result.intervals, notifications: result.notificationRegistrations }, null, 1));
    console.log('csv,static,eventSubs,count,' + (result.counts.eventSub || 0));
    console.log('csv,static,setTimeout,count,' + (result.counts.timeout || 0));
    console.log('csv,static,setInterval,count,' + (result.counts.timer || 0));
    console.log('csv,static,jsonParse,count,' + (result.counts.jsonParse || 0));
    console.log('csv,static,syncFs,count,' + (result.counts.syncIo || 0));
    console.log('csv,static,subscriptions,count,' + result.subscriptions.length);
    console.log(`wrote ${target}`);
}

if (require.main === module) main();