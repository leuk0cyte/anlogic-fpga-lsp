// D1 evidence: drive the real ReportsPanel against a section-aware fake server and show that
// QoR / Timing / Area are actually non-empty (and how big each section response/HTML is).
//
//   node vscode-extension/test/reports-sections-check.js
//
// This is the "stub benchmark" half of the D1 acceptance: no VS Code needed, but the panel,
// ReportsService and reportsHtml are the real implementations. The fake server mimics the C++
// server's section contract (src/server/commands.cpp: a `section` string is parsed into a mask,
// echoed back, and `raw` report text is dropped for section requests).
//
// Output: one csv line per section + a PASS/FAIL line. Exit code != 0 on failure.

'use strict';

const Module = require('module');
const path = require('path');

const mock = require('./mock-vscode');
const originalLoad = Module._load;
Module._load = function patched(request, parent, isMain) {
    if (request === 'vscode') return mock;
    return originalLoad.call(this, request, parent, isMain);
};

const { ReportsService } = require('../src/reportsService');
const { ReportsPanel } = require('../src/reportsPanel');

// ---------------------------------------------------------------------------
// synthetic report, split the way the server would answer per section
// ---------------------------------------------------------------------------
function makeReports() {
    const timing = {
        ok: true,
        summary: { SWNS: -0.143, STNS: -12.5, HWNS: -0.021, HTNS: -1.2, status: 'Not Met' },
        metadata: { Device: 'EF2L45LG144B', Corner: 'Slow' },
        check_status: [{ corner: 'Slow', max: 'Not Met', min: 'Met' }],
        clocks: Array.from({ length: 12 }, (u, i) => ({ name: `clk_${i}`, freq_mhz: 25 + i, fanout: 120 + i })),
        groups: Array.from({ length: 8 }, (u, g) => ({
            name: `clk_${g}`,
            type: 'Max',
            statistics: { max: { swns_ns: -0.1 - g * 0.01, stns_ns: -12 - g, viol_endpoints: 2 + g, total_endpoints: 3400 } },
            paths: Array.from({ length: 40 }, (v, p) => ({
                slack: -0.14 + p * 0.01,
                begin: `u_pwm/bit_${p % 32}/reg`,
                end: `u_ad7924/shift_${p % 16}/q`,
                check: 'Setup',
                process: 'Slow',
                details: { Budget: '9.955ns', 'Logic Level': '4' },
                delay: ['0.001 net_1 SLICE_LUTX 0.001', '0.002 net_2 SLICE_FX 0.002'],
            })),
        })),
        // what the server drops for section requests (`include_raw` is false by default)
        raw: 'TIMING REPORT\n'.repeat(2000),
    };
    const area = {
        ok: true,
        rows: [
            { resource: 'Slice LUTs', used: 1200, total: 44000, utilization: '2.7%' },
            { resource: 'Slice Registers', used: 2345, total: 45000, utilization: '5.2%' },
        ],
        counts: { LUT4: 2411, FF: 2345 },
        modules: Array.from({ length: 120 }, (u, i) => ({
            path: `u_pwm/gen_${i % 16}/u_cmp_${i}`,
            module: 'comparator',
            resources: { lut: 12 + (i % 40), seq: 8 + (i % 16) },
            utilization: { lut: 0.4, seq: 0.3 },
        })),
        raw: 'AREA REPORT\n'.repeat(2000),
    };
    const qor = {
        ok: true,
        step: 'route',
        metrics: { 'Setup WNS': '4388', 'Slice Utilization': '12.3%', 'Block RAM': '12' },
        suggestions: [{ id: 'QOR-1302', priority: 'MEDIUM', text: 'Route HWNS -2649ps smaller than 0.' }],
    };
    const files = {
        qor: '%WORKSPACE%/CorpusAlpha/.fpgalsp/run/syn/qor.rpt',
        timing: '%WORKSPACE%/CorpusAlpha/.fpgalsp/run/syn/timing.rpt',
        area: '%WORKSPACE%/CorpusAlpha/.fpgalsp/run/syn/area.rpt',
        flow_status: '%WORKSPACE%/CorpusAlpha/.fpgalsp/run/syn/status.txt',
    };
    const base = {
        project: 'demo_project',
        al: '%WORKSPACE%/CorpusAlpha/demo_project.al',
        generatedAt: 1761800000,
        files,
        artifacts: { bit: '%WORKSPACE%/CorpusAlpha/demo_project.bit', syn_db: null, route_db: null, runs: ['run_1'] },
        flow: { running: false, stage: 'idle', elapsed: 0, lastMessage: 'flow finished', ok: true },
    };
    return {
        // `{section:'summary'}` -> files + artifacts only (src/reports/reports_aggregate.cpp:34)
        summary: Object.assign({}, base, { ok: true, section: 'summary' }),
        qor: Object.assign({}, base, { ok: true, section: 'qor', qor }),
        timing: Object.assign({}, base, { ok: true, section: 'timing', timing: stripRaw(timing) }),
        area: Object.assign({}, base, { ok: true, section: 'area', area: stripRaw(area) }),
        full: Object.assign({}, base, { ok: true, qor, timing, area }),
    };
}

function stripRaw(section) {
    const copy = JSON.parse(JSON.stringify(section));
    delete copy.raw;
    return copy;
}

function bytes(value) {
    return Buffer.byteLength(JSON.stringify(value), 'utf8');
}

async function main() {
    const reports = makeReports();
    const requests = [];
    const service = new ReportsService({
        request: async (method, params) => {
            const section = (params && params.section) || 'full';
            requests.push(section);
            return JSON.parse(JSON.stringify(reports[section] || reports.full));
        },
    });
    const panelsBefore = mock.__state.webviewPanels.length;
    const messagesBefore = mock.__state.webviewMessages.length;
    const panel = new ReportsPanel({ rpc: { request: async () => reports.summary }, reportsService: service });

    await panel.show();
    const created = mock.__state.webviewPanels[panelsBefore];
    const patch = mock.__state.webviewMessages.slice(messagesBefore)
        .filter((message) => message.type === 'patch').pop();
    const skeleton = created.webview.html;

    created.webview.__receive({ type: 'tab', id: 'timing' });
    await new Promise((resolve) => setTimeout(resolve, 30));
    const timingPatch = mock.__state.webviewMessages.slice(messagesBefore)
        .filter((message) => message.type === 'patch' && message.sections && message.sections.timing).pop();

    const rows = [
        ['summary', bytes(reports.summary), skeleton.length, skeleton.includes('No area report found.') ? 0 : 1],
        ['qor', bytes(reports.qor), (patch.sections.qor || '').length, (patch.sections.qor || '').includes('Setup WNS') ? 1 : 0],
        ['area', bytes(reports.area), (patch.sections.area || '').length, (patch.sections.area || '').includes('Slice LUTs') ? 1 : 0],
        ['timing', bytes(reports.timing), (timingPatch.sections.timing || '').length, (timingPatch.sections.timing || '').includes('u_pwm/bit_') ? 1 : 0],
        ['full(no section)', bytes(reports.full), 0, 0],
    ];
    for (const [section, payloadBytes, htmlBytes, nonEmpty] of rows) {
        console.log(`csv,D1,${section},${payloadBytes},${htmlBytes},${nonEmpty}`);
    }
    console.log(`csv,D1,requests,${requests.join('|')},,`);
    console.log(`csv,D1,requestCount,${requests.length},,`);

    const failures = [];
    if (rows.find((row) => row[0] === 'qor')[3] !== 1) failures.push('qor empty');
    if (rows.find((row) => row[0] === 'area')[3] !== 1) failures.push('area empty');
    if (rows.find((row) => row[0] === 'timing')[3] !== 1) failures.push('timing empty');
    if (requests.filter((name) => name === 'timing').length !== 1) failures.push('timing fetched != 1');
    if (requests.filter((name) => name === 'qor').length !== 1) failures.push('qor fetched != 1');
    if (requests.filter((name) => name === 'area').length !== 1) failures.push('area fetched != 1');
    if (requests[0] !== 'summary') failures.push('summary is not the first request');

    console.log(failures.length ? `FAIL ${failures.join(', ')}` : 'PASS three sections non-empty, each fetched once');
    panel.dispose();
    process.exit(failures.length ? 1 : 0);
}

main().catch((err) => {
    console.error(err && err.stack ? err.stack : err);
    process.exit(1);
});