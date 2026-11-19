// Shared harness for the VS Code plugin-side perf measurements (2026-11-19).
//
// Why a harness and not one-off scripts: the perf report needs the *same* statistics
// (3 rounds x >=20 samples, p50/p95, machine-load snapshots, csv lines) for every table,
// otherwise the numbers get rejected (see audit/2026-11-19/perf/README.md section 6).
//
// Evidence levels used in the report: M = measured here, C = read from code, E = estimated.

'use strict';

const os = require('os');
const path = require('path');
const fs = require('fs');

const REPO = path.resolve(__dirname, '..', '..', '..', '..');
const EXT = path.join(REPO, 'vscode-extension');
const WORKDIR = path.join(process.env.TEMP || '/tmp', 'fpgalsp-perf', 'vscode');
const RESULTS = path.join(REPO, 'tools', 'perf', 'vscode', 'results');

function ensureDir(dir) {
    fs.mkdirSync(dir, { recursive: true });
    return dir;
}

// ---------------------------------------------------------------------------
// machine load (same idea as tools/audit/bench_servers.py)
// ---------------------------------------------------------------------------

function cpuSnapshot() {
    let idle = 0;
    let total = 0;
    for (const cpu of os.cpus()) {
        for (const key of ['user', 'nice', 'sys', 'idle', 'irq']) {
            total += cpu.times[key] || 0;
        }
        idle += cpu.times.idle || 0;
    }
    return { idle, total };
}

/** Busy fraction (0..1) of all logical cores between two snapshots. */
function cpuBusy(before, after) {
    const total = after.total - before.total;
    const idle = after.idle - before.idle;
    if (total <= 0) return 0;
    return Math.max(0, Math.min(1, 1 - idle / total));
}

// ---------------------------------------------------------------------------
// statistics
// ---------------------------------------------------------------------------

function percentile(sorted, fraction) {
    if (!sorted.length) return 0;
    const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(fraction * sorted.length) - 1));
    return sorted[index];
}

function summarize(samples) {
    const sorted = [...samples].sort((a, b) => a - b);
    const sum = sorted.reduce((acc, value) => acc + value, 0);
    return {
        n: sorted.length,
        min: sorted[0] || 0,
        p50: percentile(sorted, 0.5),
        p95: percentile(sorted, 0.95),
        max: sorted[sorted.length - 1] || 0,
        mean: sum / (sorted.length || 1),
    };
}

/**
 * Run `fn(index)` `iterations` times, `rounds` times; returns per-round summaries plus an
 * aggregate. `fn` must be synchronous (we measure extension-host main-thread blocking).
 */
function benchSync(fn, options) {
    const opts = options || {};
    const rounds = opts.rounds || 3;
    const iterations = opts.iterations || 21;
    const warmup = opts.warmup === undefined ? 2 : opts.warmup;
    const result = { label: opts.label || 'benchmark', rounds: [], aggregate: null, csv: [] };
    const all = [];
    for (let round = 0; round < rounds; round += 1) {
        for (let index = 0; index < warmup; index += 1) fn(index);
        const samples = [];
        for (let index = 0; index < iterations; index += 1) {
            const started = process.hrtime.bigint();
            fn(index);
            const elapsed = Number(process.hrtime.bigint() - started) / 1e6;
            samples.push(elapsed);
            all.push(elapsed);
        }
        const summary = summarize(samples);
        result.rounds.push(summary);
        result.csv.push(`${result.label},round${round + 1},${summary.n},${summary.p50.toFixed(3)},${summary.p95.toFixed(3)},${summary.mean.toFixed(3)}`);
    }
    result.aggregate = summarize(all);
    return result;
}

/** Async variant: awaits `fn` per sample (IPC-like paths), still reports per-round p50/p95. */
async function benchAsync(fn, options) {
    const opts = options || {};
    const rounds = opts.rounds || 3;
    const iterations = opts.iterations || 21;
    const warmup = opts.warmup === undefined ? 2 : opts.warmup;
    const result = { label: opts.label || 'benchmark', rounds: [], aggregate: null, csv: [] };
    const all = [];
    for (let round = 0; round < rounds; round += 1) {
        for (let index = 0; index < warmup; index += 1) await fn(index);
        const samples = [];
        for (let index = 0; index < iterations; index += 1) {
            const started = process.hrtime.bigint();
            await fn(index);
            const elapsed = Number(process.hrtime.bigint() - started) / 1e6;
            samples.push(elapsed);
            all.push(elapsed);
        }
        const summary = summarize(samples);
        result.rounds.push(summary);
        result.csv.push(`${result.label},round${round + 1},${summary.n},${summary.p50.toFixed(3)},${summary.p95.toFixed(3)},${summary.mean.toFixed(3)}`);
    }
    result.aggregate = summarize(all);
    return result;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

function heapUsedMb() {
    return process.memoryUsage().heapUsed / (1024 * 1024);
}

function rssMb() {
    return process.memoryUsage().rss / (1024 * 1024);
}

/**
 * Parse `text` once and report heap/peak deltas. Run with --expose-gc for stable numbers;
 * without it the deltas are upper bounds (documented in the report).
 */
function measureParse(text, options) {
    const opts = options || {};
    const gc = typeof global.gc === 'function';
    if (gc) global.gc();
    const before = process.memoryUsage();
    const started = process.hrtime.bigint();
    const value = JSON.parse(text);
    const elapsed = Number(process.hrtime.bigint() - started) / 1e6;
    const after = process.memoryUsage();
    const result = {
        bytes: Buffer.byteLength(text, 'utf8'),
        ms: elapsed,
        heapUsedDeltaMb: (after.heapUsed - before.heapUsed) / (1024 * 1024),
        rssDeltaMb: (after.rss - before.rss) / (1024 * 1024),
        gcForced: gc,
        nodes: opts.countNodes === false ? undefined : countNodes(value),
    };
    return { result, value };
}

function countNodes(value) {
    let nodes = 0;
    let strings = 0;
    let stringBytes = 0;
    const stack = [value];
    while (stack.length) {
        const item = stack.pop();
        nodes += 1;
        if (typeof item === 'string') {
            strings += 1;
            stringBytes += item.length;
        } else if (Array.isArray(item)) {
            for (const child of item) stack.push(child);
        } else if (item && typeof item === 'object') {
            for (const key of Object.keys(item)) stack.push(item[key]);
        }
    }
    return { nodes, strings, stringChars: stringBytes };
}

// ---------------------------------------------------------------------------
// mock vscode loader (same trick as vscode-extension/test/run.js)
// ---------------------------------------------------------------------------

function installVscodeMock() {
    const Module = require('module');
    const mock = require(path.join(EXT, 'test', 'mock-vscode.js'));
    const original = Module._load;
    if (!Module._installedForPerf) {
        Module._load = function patched(request, parent, isMain) {
            if (request === 'vscode') return mock;
            if (request === 'vscode-languageclient/node') {
                class FakeLanguageClient {
                    constructor(id, name, serverOptions, clientOptions) {
                        this.id = id;
                        this.name = name;
                        this.serverOptions = serverOptions;
                        this.clientOptions = clientOptions;
                        this.notificationHandlers = {};
                    }
                    async start() { this.running = true; }
                    isRunning() { return this.running; }
                    onNotification(method, handler) { this.notificationHandlers[method] = handler; }
                    async sendRequest() { return { ok: true }; }
                    async stop() { this.running = false; }
                }
                return { LanguageClient: FakeLanguageClient, TransportKind: { stdio: 0 } };
            }
            return original.call(this, request, parent, isMain);
        };
        Module._installedForPerf = true;
    }
    return mock;
}

function requireExtension(relative) {
    installVscodeMock();
    return require(path.join(EXT, relative));
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

function writeJson(name, value) {
    ensureDir(RESULTS);
    const target = path.join(RESULTS, name);
    fs.writeFileSync(target, JSON.stringify(value, null, 1));
    return target;
}

function printTable(title, rows) {
    console.log(`\n== ${title} ==`);
    for (const row of rows) console.log('csv,' + row);
}

module.exports = {
    REPO,
    EXT,
    WORKDIR,
    RESULTS,
    ensureDir,
    cpuSnapshot,
    cpuBusy,
    summarize,
    benchSync,
    benchAsync,
    heapUsedMb,
    rssMb,
    measureParse,
    countNodes,
    installVscodeMock,
    requireExtension,
    writeJson,
    printTable,
};