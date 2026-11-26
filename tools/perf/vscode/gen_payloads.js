// Generate synthetic `fpgaLsp/reports` payloads for the plugin-side perf measurements.
//
// Why synthetic: no project on this machine has flow run artifacts (`*_Runs` / parsed .rpt
// files), so a real 26 MB report cannot be produced. The shape below is taken from
//   * what the C++ server actually emits: src/reports/reports_aggregate.cpp (stage sections
//     `syn`/`phy` with files/qor/timing/area/flowStatus, top-level artifacts/files/qor/timing/
//     area/flowStatus/syn/phy/generatedAt), and
//   * exactly the fields the webview renderer reads: src/reportsHtml.js
//     (qorTab / timingTab / areaTab / flowTab / drcCdcTab / compareTab / dashboardTab).
// The report marks every number derived from these payloads as "synthetic payload" in section 1.
//
// Usage:
//   node gen_payloads.js [--target-mb 26] [--groups 12] [--paths-per-group 160]
//                        [--delay-lines 20] [--area-modules 800] [--raw-mb 18]
// Output: %TEMP%\fpgalsp-perf\vscode\reports-*.json + a manifest with the exact byte sizes.

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./lib/harness');

function parseArgs(argv) {
    // Defaults are tuned so the serialised payload lands at ~26 MB, matching the "full report
    // 26 MB" figure in audit/2026-11-19/perf/README.md section 1 (measured with a real flow).
    // The JSON contains top-level + `syn` + `phy` copies of timing/area, so each MB of raw text
    // counts three times -- this is why rawMb is small compared to the total.
    const args = { groups: 12, pathsPerGroup: 80, delayLines: 20, areaModules: 800, rawMb: 6 };
    for (let i = 0; i < argv.length; i += 1) {
        const key = argv[i];
        const value = argv[i + 1];
        const map = {
            '--groups': 'groups',
            '--paths-per-group': 'pathsPerGroup',
            '--delay-lines': 'delayLines',
            '--area-modules': 'areaModules',
            '--raw-mb': 'rawMb',
        };
        if (map[key]) {
            args[map[key]] = Number(value);
            i += 1;
        }
    }
    return args;
}

function pad(text, width) {
    return String(text).padEnd(width, ' ');
}

/** ~N MB of plausible timing report text (what `timing.raw` carries). */
function makeRawReport(megabytes, kind) {
    const line = kind === 'timing'
        ? 'Slack (VIOLATED) : -0.143ns  (arrival time: 9.812 ns  required time: 9.955 ns)  '
          + 'Path Group: clk_50M  Corner: Slow  '
        : 'LUT4   1234   LUT3  567  LUT2  89  FF  2345  BRAM  12  DSP  6   ';
    const target = Math.max(1, Math.round(megabytes * 1024 * 1024));
    const parts = [];
    let bytes = 0;
    let index = 0;
    while (bytes < target) {
        const row = `${pad(index, 8)}${line}\n`;
        parts.push(row);
        bytes += row.length;
        index += 1;
    }
    return parts.join('');
}

function metrics(count) {
    const out = {};
    const names = ['Slice LUTs', 'Slice Registers', 'Block RAM', 'DSP', 'LUT as Logic',
        'LUT as Memory', 'Bonded IOB', 'BUFG', 'Slice', 'LUT6', 'LUT5', 'LUT4',
        'MUXFX', 'CARRY', 'GND', 'VCC', 'PLL', 'DLL', 'IOBUF', 'IDDR'];
    for (let i = 0; i < count; i += 1) {
        out[names[i % names.length] + (i >= names.length ? ` #${i}` : '')] = Math.round(1234.5 * (i + 1));
    }
    return out;
}

function makeTiming(options) {
    const groups = [];
    for (let g = 0; g < options.groups; g += 1) {
        const paths = [];
        for (let p = 0; p < options.pathsPerGroup; p += 1) {
            const delay = [];
            for (let d = 0; d < options.delayLines; d += 1) {
                delay.push(`${pad(12.345 + d * 0.001, 8)}${pad('net_' + d, 40)}${pad('cell_' + d, 40)}${pad('SLICE_LUTX', 16)}${0.001}`);
            }
            paths.push({
                slack: -0.143 + p * 0.01,
                begin: `u_pwm/u_carr/u_phase_acc/bit_${p % 32}/reg[${p % 8}]`,
                end: `u_ad7924/u_spi/shift_reg_${p % 16}/q[${p % 8}]`,
                check: 'Setup',
                process: 'Slow',
                details: {
                    'Begin Point': `u_pwm/bit_${p % 32}/reg`,
                    'End Point': `u_ad7924/shift_${p % 16}/q`,
                    'Check Type': 'Setup',
                    Process: 'Slow',
                    Budget: '9.955ns',
                    'Data Path Delay': `${1.2 + p * 0.001}ns`,
                    'Clock Skew': '0.031ns',
                    'Logic Level': String(3 + (p % 8)),
                    'Max Fanout': String(12 + (p % 9)),
                    'Clock Uncertainty': '0.120ns',
                    Corner: 'Slow',
                },
                delay,
            });
        }
        groups.push({
            name: `clk_${g}`,
            type: 'Max',
            from_clock: `clk_${g}`,
            to_clock: `clk_${g}`,
            min_period: `${40 + g}.000`,
            fmax: `${100 - g}.5`,
            statistics: {
                max: {
                    swns_ns: -0.143 - g * 0.01,
                    stns_ns: -12.5 - g,
                    viol_endpoints: 12 + g,
                    total_endpoints: 3400 + g,
                },
                min: { swns_ns: 0.31, stns_ns: 0, viol_endpoints: 0, total_endpoints: 3400 + g },
            },
            paths,
        });
    }
    const clocks = [];
    for (let c = 0; c < 40; c += 1) {
        clocks.push({
            name: `clk_${c}`,
            freq_mhz: 25 + c,
            fanout: 120 + c * 7,
            source: `pll_gen/clk${c % 4}_out`,
        });
    }
    return {
        ok: true,
        summary: { SWNS: -0.143, STNS: -12.5, HWNS: -0.021, HTNS: -1.2, status: 'Not Met' },
        metadata: {
            'Report kind': 'Timing',
            Generator: 'TangDynasty',
            Version: '6.2.3.210850',
            Device: 'EF2L45LG144B',
            Corner: 'Slow',
        },
        check_status: [
            { corner: 'Slow', max: 'Not Met', min: 'Met' },
            { corner: 'Fast', max: 'Met', min: 'Met' },
        ],
        clocks,
        clock_detail: clocks.slice(0, 8).map((clock) => ({ clock: clock.name, period: '40.000', freq: clock.freq_mhz })),
        groups,
        paths: [],
        raw: makeRawReport(options.rawMb * 0.55, 'timing'),
    };
}

function makeArea(options) {
    const rows = [];
    const resources = ['Slice LUTs', 'Slice Registers', 'Block RAM', 'DSP', 'Bonded IOB', 'BUFG'];
    for (let i = 0; i < resources.length; i += 1) {
        rows.push({
            resource: resources[i],
            used: 1200 + i * 111,
            total: 44000 + i * 1000,
            utilization: `${(2.7 + i).toFixed(1)}%`,
        });
    }
    const modules = [];
    for (let i = 0; i < options.areaModules; i += 1) {
        modules.push({
            path: `u_pwm/u_carr/gen_block_${i % 64}/u_cmp_${i}`,
            name: `u_cmp_${i}`,
            module: 'comparator',
            resources: { lut: 12 + (i % 40), ripple: 3, seq: 8 + (i % 16), bram: i % 2, dsp: i % 3 },
            utilization: { lut: 0.4 + (i % 10) / 100, seq: 0.2 + (i % 7) / 100 },
        });
    }
    return {
        ok: true,
        rows,
        counts: { LUT1: 231, LUT2: 988, LUT3: 1024, LUT4: 2411, LUT5: 873, LUT6: 145, FF: 2345, MUXFX: 44 },
        modules,
        raw: makeRawReport(options.rawMb * 0.45, 'area'),
    };
}

function makeQor() {
    return {
        ok: true,
        step: 'Synthesis',
        metrics: metrics(48),
        suggestions: Array.from({ length: 24 }, (unused, index) => ({
            id: `QOR-${100 + index}`,
            priority: index % 2 ? 'Medium' : 'High',
            text: `Reduce logic depth in u_pwm/gen_block_${index} (fanout ${12 + index}, level ${3 + (index % 9)}).`,
        })),
    };
}

function makeFlowStatus() {
    const steps = [];
    for (let i = 0; i < 12; i += 1) {
        steps.push({
            stage: i < 6 ? 'syn' : 'phy',
            name: ['read_hdl', 'elaborate', 'synthesize', 'optimize', 'map', 'place', 'route', 'route_opt', 'timing', 'bitgen', 'report', 'finish'][i],
            status: 'done',
            wall: `${10 + i}s`,
            elapsed: 10 + i,
        });
    }
    return { ok: true, steps, stages: { syn: { ok: true, steps: steps.slice(0, 6) }, phy: { ok: true, steps: steps.slice(6) } } };
}

function makeStage(label, timing, area, qor, flowStatus) {
    return {
        ok: true,
        label,
        files: {
            qor: `C:/work/CorpusAlpha/.fpgalsp/run/${label.toLowerCase()}/qor.rpt`,
            timing: `C:/work/CorpusAlpha/.fpgalsp/run/${label.toLowerCase()}/timing.rpt`,
            area: `C:/work/CorpusAlpha/.fpgalsp/run/${label.toLowerCase()}/area.rpt`,
            flow_status: `C:/work/CorpusAlpha/.fpgalsp/run/${label.toLowerCase()}/status.txt`,
        },
        qor,
        timing,
        area,
        flowStatus,
    };
}

function makeFullPayload(options) {
    const timing = makeTiming(options);
    const area = makeArea(options);
    const qor = makeQor();
    const flowStatus = makeFlowStatus();
    makeStage('Synthesis', timing, area, qor, flowStatus);
    return {
        ok: true,
        project: 'demo_project',
        al: 'C:/work/CorpusAlpha/demo_project.al',
        generatedAt: Math.floor(Date.now() / 1000),
        artifacts: { bit: 'C:/work/CorpusAlpha/demo_project.bit', syn_db: null, route_db: null, runs: ['run_1'] },
        files: {
            qor: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/qor.rpt',
            timing: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/timing.rpt',
            area: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/area.rpt',
            flow_status: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/status.txt',
        },
        flow: { running: false, stage: 'idle', elapsed: 0, lastMessage: 'flow finished', ok: true },
        flowStatus,
        qor,
        timing,
        area,
        analysis: {
            drc: {
                available: true,
                violations: 3,
                rules: [
                    { rule: 'USR-8026', severity: 'Warning', count: 2 },
                    { rule: 'USR-8027', severity: 'Warning', count: 1 },
                ],
                details: Array.from({ length: 60 }, (unused, index) => `pin P${80 + index} is not assigned a PULLTYPE`),
                files: { drc: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/drc.rpt' },
            },
            cdc: {
                available: true,
                counts: { Unsafe: 0, Safe: 12 },
                rows: Array.from({ length: 40 }, (unused, index) => [`clk_${index % 4}`, `clk_${(index + 1) % 4}`, 'async', 'safe']),
                files: { cdc: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/cdc.rpt' },
            },
        },
        compare: {
            runs: Array.from({ length: 5 }, (unused, index) => ({
                run: `run_${index + 1}`,
                strategy: ['AreaOptimize', 'CongestionHigh', 'LogicLevelOptimize', 'RuntimeOptimize', 'TimingHigh'][index],
                setup_wns_ps: -143 - index * 10,
                setup_tns_ps: -1250 - index * 100,
                luts: 2411 - index * 20,
                slices: 640 - index * 5,
                slice_pct: 2.7 + index * 0.1,
            })),
            best: { run: 'run_5' },
            available: ['AreaOptimize', 'CongestionHigh', 'LogicLevelOptimize', 'RuntimeOptimize', 'TimingHigh'],
            work: 'C:/work/CorpusAlpha/.fpgalsp/compare',
            elapsed: 58,
        },
        syn: makeStage('Synthesis', timing, area, qor, flowStatus),
        phy: makeStage('Place & Route', timing, area, qor, flowStatus),
        sections: ['qor', 'timing', 'area', 'flow'],
    };
}

/** What the panel actually receives today: `{section:'summary'}` -> files + artifacts only. */
function makeSummaryPayload() {
    return {
        ok: true,
        section: 'summary',
        project: 'demo_project',
        al: 'C:/work/CorpusAlpha/demo_project.al',
        generatedAt: Math.floor(Date.now() / 1000),
        artifacts: { bit: 'C:/work/CorpusAlpha/demo_project.bit', syn_db: null, route_db: null, runs: ['run_1'] },
        files: {
            qor: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/qor.rpt',
            timing: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/timing.rpt',
            area: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/area.rpt',
            flow_status: 'C:/work/CorpusAlpha/.fpgalsp/run/syn/status.txt',
        },
        flow: { running: false, stage: 'idle', elapsed: 0, lastMessage: 'flow finished', ok: true },
        sections: ['qor', 'timing', 'area'],
    };
}

function main() {
    const options = parseArgs(process.argv.slice(2));
    harness.ensureDir(harness.WORKDIR);

    const full = makeFullPayload(options);
    const fullText = JSON.stringify(full);
    const fullPath = path.join(harness.WORKDIR, 'reports-full.json');
    fs.writeFileSync(fullPath, fullText);

    // Same parsed tables without the huge `raw` text: separates "parse a big string" from
    // "parse many small objects" (both matter, they are fixed by different changes).
    const noRaw = JSON.parse(fullText);
    for (const node of [noRaw, noRaw.syn, noRaw.phy]) {
        if (!node) continue;
        for (const key of ['timing', 'area', 'qor']) {
            if (node[key] && typeof node[key] === 'object') delete node[key].raw;
        }
    }
    const noRawText = JSON.stringify(noRaw);
    const noRawPath = path.join(harness.WORKDIR, 'reports-full-noraw.json');
    fs.writeFileSync(noRawPath, noRawText);

    const summaryText = JSON.stringify(makeSummaryPayload());
    const summaryPath = path.join(harness.WORKDIR, 'reports-summary.json');
    fs.writeFileSync(summaryPath, summaryText);

    const manifest = {
        generatedAt: new Date().toISOString(),
        options,
        files: {
            full: { path: fullPath, bytes: Buffer.byteLength(fullText) },
            fullNoRaw: { path: noRawPath, bytes: Buffer.byteLength(noRawText) },
            summary: { path: summaryPath, bytes: Buffer.byteLength(summaryText) },
        },
        shape: {
            timingGroups: full.timing.groups.length,
            timingPaths: full.timing.groups.reduce((acc, group) => acc + group.paths.length, 0),
            timingDelayLines: options.delayLines,
            areaRows: full.area.rows.length,
            areaModules: full.area.modules.length,
            qorMetrics: Object.keys(full.qor.metrics).length,
            qorSuggestions: full.qor.suggestions.length,
        },
    };
    fs.writeFileSync(path.join(harness.WORKDIR, 'payload-manifest.json'), JSON.stringify(manifest, null, 1));
    console.log(JSON.stringify(manifest, null, 1));
    console.log('csv,payload,full,bytes,' + manifest.files.full.bytes);
    console.log('csv,payload,full-noraw,bytes,' + manifest.files.fullNoRaw.bytes);
    console.log('csv,payload,summary,bytes,' + manifest.files.summary.bytes);
}

if (require.main === module) main();

module.exports = { makeFullPayload, makeSummaryPayload, makeTiming, makeArea, parseArgs };