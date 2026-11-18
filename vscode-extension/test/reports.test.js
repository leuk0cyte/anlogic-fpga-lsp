// Unit test for the split reports webview (Dashboard + Synthesis + Place & Route tabs).
//   node test/reports.test.js

'use strict';

const { renderReportsHtml } = require('../src/reportsHtml');

let failures = 0;
function check(name, ok, detail) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures += 1;
}

const reports = {
    ok: true,
    generatedAt: 1700000000,
    flow: { stage: 'route', running: false },
    qor: {
        step: 'route',
        metrics: [['Slice LUT', 1234], ['Registers', 567], ['Timing WNS', -0.25]],
        suggestions: [{ id: 'QOR-2001', priority: 'high', text: 'Reduce LUT pressure' }],
    },
    timing: {
        summary: { SWNS: 0.12, STNS: 0, HWNS: -0.05, HTNS: -0.2, status: 'Met' },
        metadata: { Device: 'EF2L45LG144B', Confidence: 'Synthesized' },
        check_status: [{ corner: 'Slow', max: 'yes', min: 'yes' }],
        clocks: [{ name: 'clk0', freq_mhz: 64, fanout: 733, source: 'pll_gen/clk' }],
        clock_detail: [{ name: 'clk0', type: 'local', swns_ns: '3.740' }],
        groups: [{ name: 'clk0 -> clk0', type: 'Self', from_clock: 'clk0', to_clock: 'clk0',
                   min_period: '8.145ns', fmax: '122.775MHz',
                   statistics: { max: { swns_ns: 3.74, stns_ns: 0, viol_endpoints: 0, total_endpoints: 1467 } },
                   paths: [{ slack: 3.74, begin: 'a/x_reg.clk', end: 'b/y_reg.d', check: 'setup',
                             details: { 'Data Path Delay': '3.957ns' },
                             delay: ['port P87 0.000 0.000 pin: clk'] }] }],
        paths: [{ slack: 3.74, begin: 'a/x_reg.clk', end: 'b/y_reg.d', check: 'setup' }],
        raw: 'Timing Status\nSWNS: 3.740ns',
    },
    area: {
        io: { le: 1234 },
        rows: [{ resource: '#lut', used: 1033, total: 4480, utilization: '23.06%' }],
        counts: { Total_luts: 657 },
        totals: { lut: 4480, reg: 4480 },
        modules: [{ name: 'top', module: 'top', path: 'top', level: 0,
                    resources: { lut: 845, ripple: 188, seq: 733, bram: 0, dsp: 0 },
                    utilization: { lut: 18.86, ripple: 4.2, seq: 16.36, bram: 0, dsp: 0 } }],
        raw: 'Report Hierarchy Area:\n|top|top|845|188|733|0|0|',
    },
    analysis: { drc: { violations: 0, clean: true }, cdc: { counts: { Unsafe: 1, Safe: 2 } } },
    files: { qor: 'Y:/p/route.qor', timing: 'Y:/p/final_timing.rpt' },
    syn: {
        ok: true,
        label: 'Synthesis',
        qor: { step: 'gate', metrics: [['LUT', 1000]] },
        timing: { summary: { SWNS: 1.0, STNS: 0 } },
        area: { rows: [{ resource: '#LUT', used: 900, total: 4000, utilization: '22%' }] },
        files: { qor: 'Y:/p/syn/gate.qor' },
    },
    phy: {
        ok: true,
        label: 'Place & Route',
        qor: { step: 'route', metrics: [['LUT', 1234]] },
        timing: { summary: { SWNS: 0.12, HWNS: -0.05 } },
        area: { rows: [{ resource: '#LUT', used: 100, total: 400, utilization: '25%' }] },
        files: { qor: 'Y:/p/phy/route.qor' },
    },
};

const html = renderReportsHtml({ reports, nonce: 'abc123' });
check('reports html renders', html.includes('FPGA Reports'));
check('dashboard tab exists', html.includes('data-tab="dashboard"') && html.includes('id="tab-dashboard"'));
check('synthesis tab exists', html.includes('data-tab="synthesis"') && html.includes('id="tab-synthesis"'));
check('place & route tab exists', html.includes('data-tab="place-route"') && html.includes('id="tab-place-route"'));
check('dashboard has timing summary', html.includes('SWNS') && html.includes('HTNS'));
check('dashboard has DRC/CDC', html.includes('DRC:') && html.includes('CDC:'));
check('suggestions rendered', html.includes('QOR-2001'));
check('stage tabs render their QoR', html.includes('Y:/p/syn/gate.qor') && html.includes('Y:/p/phy/route.qor'));
check('only dashboard starts active',
    (html.match(/tab-panel active/g) || []).length === 1 && html.includes('id="tab-dashboard" class="tab-panel active"'));
check('report files are clickable', html.includes('command:fpgaLsp.openReportFile'));

// split pages: one page per stage, each with dashboard + timing/area/search
const synHtml = renderReportsHtml({ reports, nonce: 'n1', stage: 'syn' });
const phyHtml = renderReportsHtml({ reports, nonce: 'n2', stage: 'phy' });
check('synthesis page has its own title', synHtml.includes('<title>Synthesis Reports</title>') && synHtml.includes('Synthesis Reports'));
check('place&route page has its own title', phyHtml.includes('<title>Place &amp; Route Reports</title>') || phyHtml.includes('Place &amp; Route Reports'));
check('stage page still has a dashboard', synHtml.includes('id="tab-dashboard"') && phyHtml.includes('id="tab-dashboard"'));
check('stage page has no cross-stage tabs', !synHtml.includes('data-tab="place-route"') && !phyHtml.includes('data-tab="synthesis"'));
check('search box is available', synHtml.includes('id="report-search"') && phyHtml.includes('id="report-search"'));
check('timing page lists every path group', synHtml.includes('clk0 -&gt; clk0') && synHtml.includes('a/x_reg.clk'));
check('timing page has raw report lines', synHtml.includes('Raw timing report') && synHtml.includes('raw-line'));
check('area page has per-module utilization', synHtml.includes('Per-module utilization') && synHtml.includes('18.86%') && synHtml.includes('top'));
check('area page has raw report lines', synHtml.includes('Raw area report'));

console.log(failures ? `\n${failures} FAILED` : '\nALL PASS');
process.exit(failures === 0 ? 0 : 1);
