// Plugin-side cost of the reports payload: JSON.parse + structure conversion + section renders.
//
// Measures the REAL renderer (vscode-extension/src/reportsHtml.js) on the synthetic payloads
// produced by gen_payloads.js. No VS Code needed: reportsHtml is pure string building.
//
//   node --expose-gc bench_reports.js
//
// Output: csv lines on stdout + tools/perf/vscode/results/reports.json

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./lib/harness');

const reportsHtml = require(path.join(harness.EXT, 'src', 'reportsHtml.js'));

function readPayloads() {
    const dir = harness.WORKDIR;
    const manifestPath = path.join(dir, 'payload-manifest.json');
    if (!fs.existsSync(manifestPath)) {
        throw new Error(`missing ${manifestPath}; run "node gen_payloads.js" first`);
    }
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    const load = (key) => fs.readFileSync(manifest.files[key].path, 'utf8');
    return { manifest, full: load('full'), fullNoRaw: load('fullNoRaw'), summary: load('summary') };
}

function main() {
    const payloads = readPayloads();
    const before = harness.cpuSnapshot();
    const results = { generatedAt: new Date().toISOString(), payloads: payloads.manifest, benches: {}, memory: {} };
    const csv = [];

    // ---- 1. JSON.parse (what the JSON-RPC client does when the response arrives) ----------
    const parseFull = harness.benchSync(() => { JSON.parse(payloads.full); },
        { label: 'parse.full-26mb', rounds: 3, iterations: 21, warmup: 1 });
    results.benches.parseFull = parseFull;
    csv.push(...parseFull.csv);

    const parseNoRaw = harness.benchSync(() => { JSON.parse(payloads.fullNoRaw); },
        { label: 'parse.full-noraw-8mb', rounds: 3, iterations: 21 });
    results.benches.parseFullNoRaw = parseNoRaw;
    csv.push(...parseNoRaw.csv);

    const parseSummary = harness.benchSync(() => { JSON.parse(payloads.summary); },
        { label: 'parse.summary-559b', rounds: 3, iterations: 21 });
    results.benches.parseSummary = parseSummary;
    csv.push(...parseSummary.csv);

    // ---- memory of one parse -------------------------------------------------------------
    for (const [key, text] of Object.entries({ full: payloads.full, fullNoRaw: payloads.fullNoRaw, summary: payloads.summary })) {
        const { result } = harness.measureParse(text);
        results.memory[key] = result;
    }

    // Parse once; the render benches below work on the parsed object (that is what the panel
    // holds in memory after the response arrived).
    const full = JSON.parse(payloads.full);
    const noRaw = JSON.parse(payloads.fullNoRaw);
    const summary = JSON.parse(payloads.summary);

    // ---- 2. structure conversion: buildReportView + reportSectionIds ----------------------
    for (const [key, value] of Object.entries({ full, fullNoRaw: noRaw, summary })) {
        const bench = harness.benchSync(() => {
            const view = reportsHtml.buildReportView(value, 'syn');
            reportsHtml.reportSectionIds(view, 'syn');
        }, { label: `view.build.${key}`, rounds: 3, iterations: 21 });
        results.benches[`viewBuild_${key}`] = bench;
        csv.push(...bench.csv);
    }

    // ---- 3. the panel's patch path: renderReportSection() per section id ------------------
    const ids = reportsHtml.reportSectionIds(reportsHtml.buildReportView(full, 'syn'), 'syn');
    const perSection = {};
    for (const id of ids) {
        const bench = harness.benchSync(() => {
            reportsHtml.renderReportSection({ reports: full, stage: 'syn' }, id);
        }, { label: `patch.section.${id}`, rounds: 3, iterations: 15 });
        perSection[id] = bench;
        csv.push(...bench.csv);
    }
    results.benches.sections = perSection;

    const patchTotal = harness.benchSync(() => {
        for (const id of ids) reportsHtml.renderReportSection({ reports: full, stage: 'syn' }, id);
    }, { label: 'patch.total(full-26mb)', rounds: 3, iterations: 15 });
    results.benches.patchTotal = patchTotal;
    csv.push(...patchTotal.csv);

    // Same patch path for the payload the extension actually gets today (`section:summary`).
    const summaryIds = reportsHtml.reportSectionIds(reportsHtml.buildReportView(summary, 'syn'), 'syn');
    const patchSummary = harness.benchSync(() => {
        for (const id of summaryIds) reportsHtml.renderReportSection({ reports: summary, stage: 'syn' }, id);
    }, { label: `patch.total(summary-${summaryIds.length}sections)`, rounds: 3, iterations: 21 });
    results.benches.patchSummary = patchSummary;
    csv.push(...patchSummary.csv);

    // Why does *every* section cost ~9 ms on the big payload? `reportSectionHtml()` computes
    // `areaTab(view.area)` unconditionally (reportsHtml.js:627), and areaTab() escapes the
    // 2.7 MB `area.raw` (rawBlock splits the whole string, then escapes 6000 lines).
    // Cross-check: the same section render without any `raw` text should collapse to ~0.
    const noRawIds = ['compare', 'flow', 'qor'];
    for (const id of noRawIds) {
        const bench = harness.benchSync(() => {
            reportsHtml.renderReportSection({ reports: noRaw, stage: 'syn' }, id);
        }, { label: `patch.section.${id}.noraw`, rounds: 3, iterations: 15 });
        results.benches[`sectionNoRaw_${id}`] = bench;
        csv.push(...bench.csv);
    }

    // ---- 4. first paint: full page render -------------------------------------------------
    const paintFull = harness.benchSync(() => {
        reportsHtml.renderReportsHtml({ reports: full, nonce: 'a'.repeat(32), stage: 'syn' });
    }, { label: 'paint.full-26mb', rounds: 3, iterations: 15 });
    results.benches.paintFull = paintFull;
    csv.push(...paintFull.csv);

    const paintSummary = harness.benchSync(() => {
        reportsHtml.renderReportsHtml({ reports: summary, nonce: 'a'.repeat(32), stage: 'syn' });
    }, { label: 'paint.summary', rounds: 3, iterations: 21 });
    results.benches.paintSummary = paintSummary;
    csv.push(...paintSummary.csv);

    // ---- 5. HTML size produced (the webview has to parse/layout it, in a separate process) --
    const sampleIds = ids;
    const htmlSizes = {};
    for (const id of sampleIds) {
        htmlSizes[id] = reportsHtml.renderReportSection({ reports: full, stage: 'syn' }, id).length;
    }
    const fullHtml = reportsHtml.renderReportsHtml({ reports: full, nonce: 'a'.repeat(32), stage: 'syn' });
    results.html = {
        fullPageBytes: fullHtml.length,
        sectionBytes: htmlSizes,
        sectionTotalBytes: Object.values(htmlSizes).reduce((acc, value) => acc + value, 0),
        summaryPageBytes: reportsHtml.renderReportsHtml({ reports: summary, nonce: 'a'.repeat(32), stage: 'syn' }).length,
    };
    csv.push(`html,fullpage,bytes,${results.html.fullPageBytes}`);
    csv.push(`html,sections,bytes,${results.html.sectionTotalBytes}`);
    csv.push(`html,summarypage,bytes,${results.html.summaryPageBytes}`);

    const after = harness.cpuSnapshot();
    results.cpuBusy = harness.cpuBusy(before, after);

    // ---- 6. functional cross-check: what does the summary-only payload actually render? -----
    // The extension asks for `{section:'summary'}` (reportsPanel.refresh) and then renders the
    // stage tabs from *that* payload. On a server that honours `section` the payload has no
    // qor/timing/area tables at all -> the panel must show the empty states. This is recorded
    // here because it decides whether the big-payload costs above are even reachable.
    const summaryView = reportsHtml.buildReportView(summary, 'syn');
    const summaryHtml = reportsHtml.renderReportsHtml({ reports: summary, nonce: 'a'.repeat(32), stage: 'syn' });
    results.summaryRender = {
        viewOk: summaryView.ok === true,
        sections: reportsHtml.reportSectionIds(summaryView, 'syn'),
        // The section headings are always rendered, so look for the *empty-state* strings instead.
        emptyQor: summaryHtml.includes('No QoR metrics found'),
        emptyTimingPaths: summaryHtml.includes('No path groups.'),
        emptyArea: summaryHtml.includes('No area report found.'),
        areaModulesZero: summaryHtml.includes('Per-module utilization (0)'),
        hasFlowSteps: summaryHtml.includes('read_hdl'),
        hasQorMetricTable: /<table class="metrics"><thead><tr><th>Metric<\/th>/.test(summaryHtml),
        bytes: summaryHtml.length,
    };
    csv.push(`summary,viewOk,${results.summaryRender.viewOk ? 1 : 0}`);
    csv.push(`summary,emptyQor,${results.summaryRender.emptyQor ? 1 : 0}`);
    csv.push(`summary,emptyTimingPaths,${results.summaryRender.emptyTimingPaths ? 1 : 0}`);
    csv.push(`summary,emptyArea,${results.summaryRender.emptyArea ? 1 : 0}`);

    results.csv = csv;
    const target = harness.writeJson('reports.json', results);
    harness.printTable('reports payload (ms)', csv);
    console.log(`\nmachine busy during run: ${(results.cpuBusy * 100).toFixed(1)}%`);
    console.log(`wrote ${target}`);
}

if (require.main === module) main();