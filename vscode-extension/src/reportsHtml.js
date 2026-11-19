// Pure HTML renderer for the "FPGA Reports" webview.
//
// Kept free of the `vscode` module so it can be unit tested directly. Every value coming from
// the reports JSON is escaped before being interpolated into HTML (the report files are
// external data); dynamic widths are emitted in a nonce-protected <style> block so the strict
// CSP does not need `unsafe-inline`.

'use strict';

const REPORT_TABS = ['Dashboard', 'Synthesis', 'Place & Route', 'QoR', 'Timing', 'Area', "DRC/CDC", 'Compare'];
const STAGE_TABS = ['Dashboard', 'QoR', 'Timing', 'Area', 'Flow', "DRC/CDC", 'Compare'];

function esc(value) {
    return String(value === undefined || value === null ? '' : value)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function fmtNum(value) {
    if (value === undefined || value === null || value === '') return '—';
    const n = Number(value);
    if (!Number.isFinite(n)) return String(value);
    return String(Math.round(n * 1000) / 1000);
}

function fmtElapsed(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return value === undefined || value === null ? '' : String(value);
    return n < 100 ? `${n.toFixed(1)}s` : `${Math.round(n)}s`;
}

function fmtSize(bytes) {
    const n = Number(bytes);
    if (!Number.isFinite(n) || n < 0) return '';
    if (n < 1024) return `${n} B`;
    const units = ['KB', 'MB', 'GB', 'TB'];
    let value = n / 1024;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit += 1;
    }
    return `${value >= 10 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

function fmtDate(unixSeconds) {
    const n = Number(unixSeconds);
    if (!Number.isFinite(n) || n <= 0) return '';
    return new Date(n * 1000).toLocaleString();
}

function pad2(value) {
    return value < 10 ? `0${value}` : String(value);
}

function fmtMtime(unixSeconds) {
    const n = Number(unixSeconds);
    if (!Number.isFinite(n) || n <= 0) return '';
    const date = new Date(n * 1000);
    return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())} `
        + `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
}

function sanitizeNonce(nonce) {
    const clean = String(nonce || '').replace(/[^A-Za-z0-9_-]/g, '');
    return clean || 'fpga-nonce';
}

/** Build a VS Code command URI (the panel has `enableCommandUris: true`). */
function commandHref(command, argument) {
    const encoded = encodeURIComponent(JSON.stringify([argument]));
    return `command:${command}?${encoded}`;
}

function fileLink(command, filePath, label) {
    const text = label === undefined ? filePath : label;
    return `<a href="${esc(commandHref(command, filePath))}" title="${esc(filePath)}">${esc(text)}</a>`;
}

/** DOM id for a tab label ('DRC/CDC' -> 'drc-cdc'). */
function tabId(label) {
    return String(label).toLowerCase().replace(/[^a-z0-9]+/g, '-');
}

function basename(filePath) {
    const parts = String(filePath || '').split(/[\\/]/);
    return parts[parts.length - 1] || String(filePath || '');
}

/** A timing Begin/End point: clickable when reports.py mapped it back to RTL. */
function timingPointCell(point, rawText) {
    if (point && point.resolved && point.file) {
        const line = Number(point.line) || 0;
        const href = commandHref('fpgaLsp.openTimingPoint',
            { file: point.file, line, signal: point.signal || '' });
        return `<a href="${esc(href)}" title="${esc(point.file + ':' + (line + 1))}">`
            + `${esc(basename(point.file))}:${line + 1}</a> <span class="muted">${esc(point.signal || '')}</span>`;
    }
    return esc(rawText);
}

/** Highlight QOR-xxxx issue ids inside already-escaped text. */
function highlightQor(escapedText) {
    return String(escapedText).replace(/\b(QOR-\d+)\b/g, '<code class="qor-id">$1</code>');
}

function metricPairs(metrics) {
    if (!metrics) return [];
    if (Array.isArray(metrics)) {
        return metrics
            .map((entry) => {
                if (!entry || typeof entry !== 'object') return undefined;
                const name = entry.name || entry.key || entry.metric;
                if (name === undefined) return undefined;
                const group = entry.group ? `${entry.group} / ` : '';
                return [`${group}${name}`, entry.value];
            })
            .filter(Boolean);
    }
    if (typeof metrics === 'object') {
        return Object.keys(metrics).map((key) => [key, metrics[key]]);
    }
    return [];
}

function utilizationPercent(row) {
    if (!row || typeof row !== 'object') return 0;
    const used = Number(row.used);
    const total = Number(row.total);
    if (Number.isFinite(used) && Number.isFinite(total) && total > 0) {
        return Math.max(0, Math.min(100, (used / total) * 100));
    }
    const parsed = parseFloat(String(row.utilization || '').replace('%', ''));
    if (Number.isFinite(parsed)) return Math.max(0, Math.min(100, parsed));
    return 0;
}

function barLevel(percent) {
    if (percent >= 90) return 'bar-red';
    if (percent >= 75) return 'bar-orange';
    return 'bar-green';
}

function emptyStateHtml(reports) {
    const error = reports && reports.error ? String(reports.error) : '';
    return `
<div class="empty">
  <h2>No reports yet</h2>
  <p>Run a flow first — use <b>Run Synthesis</b> or <b>Run Full Flow</b> above,
     or the flow buttons in the FPGA view title.</p>
  ${error ? `<p class="muted">${esc(error)}</p>` : ''}
</div>`;
}

function qorTab(qor) {
    const data = qor && typeof qor === 'object' ? qor : {};
    const pairs = metricPairs(data.metrics);
    const suggestions = Array.isArray(data.suggestions) ? data.suggestions : [];
    const rows = pairs.map(([name, value]) => (
        `<tr><th>${esc(name)}</th><td>${esc(fmtNum(value))}</td></tr>`
    )).join('');
    const suggestionHtml = suggestions.map((entry) => {
        if (entry === undefined || entry === null) return '';
        if (typeof entry === 'string') {
            return `<li>${highlightQor(esc(entry))}</li>`;
        }
        const id = entry.id ? `<code class="qor-id">${esc(entry.id)}</code> ` : '';
        const priority = entry.priority
            ? `<span class="prio prio-${esc(String(entry.priority).toLowerCase())}">${esc(entry.priority)}</span> `
            : '';
        const text = entry.text !== undefined ? entry.text : (entry.message !== undefined ? entry.message : '');
        return `<li>${priority}${id}${highlightQor(esc(text))}</li>`;
    }).filter(Boolean).join('');

    return `
<section id="tab-qor" class="tab-panel active">
  <h2>Quality of Results ${data.step ? `<span class="badge">${esc(data.step)}</span>` : ''}</h2>
  ${rows
        ? `<table class="metrics"><thead><tr><th>Metric</th><th>Value</th></tr></thead><tbody>${rows}</tbody></table>`
        : '<p class="muted">No QoR metrics found in the parsed report files.</p>'}
  <h3>Suggestions</h3>
  ${suggestionHtml ? `<ul class="suggestions">${suggestionHtml}</ul>` : '<p class="muted">No suggestions.</p>'}
</section>`;
}

function rawBlock(text, label) {
    const content = String(text || '');
    if (!content.trim()) return '';
    const lines = content.split('\n');
    const shown = lines.slice(0, 6000);
    const rows = shown.map((line) => `<div class="raw-line searchable">${esc(line) || '&nbsp;'}</div>`).join('');
    const note = lines.length > shown.length
        ? `<p class="muted">Showing ${shown.length} of ${lines.length} lines.</p>` : '';
    return `<details class="raw-block"><summary>${esc(label || 'Raw report')} (${lines.length} lines)</summary>${note}<div class="raw">${rows}</div></details>`;
}

function searchableTable(headers, rows) {
    if (!rows.length) return '<p class="muted">No rows.</p>';
    const head = headers.map(([key, label]) => `<th>${esc(label)}</th>`).join('');
    const body = rows.map((row) => `<tr class="searchable">${headers.map(([key]) => `<td>${esc(row[key] === undefined || row[key] === null ? '' : row[key])}</td>`).join('')}</tr>`).join('');
    return `<table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
}

function fmtPercent(value) {
    const n = Number(value);
    return Number.isFinite(n) ? `${n.toFixed(2)}%` : '';
}

function timingTab(timing) {
    const data = timing && typeof timing === 'object' ? timing : {};
    const summary = data.summary && typeof data.summary === 'object'
        ? data.summary
        : (['SWNS', 'STNS', 'HWNS', 'HTNS'].some((key) => data[key] !== undefined) ? data : {});
    const keys = ['SWNS', 'STNS', 'HWNS', 'HTNS'];
    const cards = keys.map((key) => `
    <div class="card">
      <div class="label">${esc(key)}</div>
      <div class="value">${esc(fmtNum(summary[key]))}${summary[key] === undefined ? '' : '<span class="unit">ns</span>'}</div>
    </div>`).join('');
    const status = summary.status
        ? `<div class="card"><div class="label">Status</div><div class="value status-${esc(String(summary.status).toLowerCase())}">${esc(summary.status)}</div></div>`
        : '';

    const metadata = data.metadata && typeof data.metadata === 'object' ? data.metadata : {};
    const metaRows = Object.entries(metadata).map(([key, value]) => ({ key, value }));
    const checkStatus = Array.isArray(data.check_status) ? data.check_status : [];
    const clocks = Array.isArray(data.clocks) ? data.clocks : [];
    const clockRows = clocks.map((clock) => ({
        name: clock.name, freq_mhz: clock.freq_mhz, fanout: clock.fanout, source: clock.source,
    }));
    const clockDetail = Array.isArray(data.clock_detail) ? data.clock_detail : [];
    const groups = Array.isArray(data.groups) ? data.groups : [];
    const paths = Array.isArray(data.paths) ? data.paths : [];

    const groupHtml = groups.map((group, groupIndex) => {
        const stats = group.statistics || {};
        const statRows = Object.entries(stats).map(([kind, values]) => ({
            kind: kind.toUpperCase(), swns_ns: values.swns_ns, stns_ns: values.stns_ns,
            viol_endpoints: values.viol_endpoints, total_endpoints: values.total_endpoints,
        }));
        const pathHtml = (group.paths || []).map((path, pathIndex) => {
            const details = path.details || {};
            // M3c：时序端点已由后端解析到源码（begin_at/end_at 带 file/line），
            // 这里渲染成可点击链接；未解析时退回原始文本（esc 过）。
            const beginHtml = timingPointCell(path.begin_at, path.begin || '');
            const endHtml = timingPointCell(path.end_at, path.end || '');
            const detailRows = Object.entries(details).map(([key, value]) => ({ key, value }));
            const delay = Array.isArray(path.delay) ? path.delay : [];
            const delayHtml = delay.length
                ? `<div class="raw delay searchable">${delay.map((line) => `<div class="raw-line searchable">${esc(line) || '&nbsp;'}</div>`).join('')}</div>`
                : '';
            return `<details class="path-card searchable">
              <summary><b>Slack ${esc(fmtNum(path.slack))} ns</b> · ${beginHtml} → ${endHtml}</summary>
              <p class="muted">Group: ${esc(group.name || '')} · Check: ${esc(path.check || '')} · Process: ${esc(path.process || '')}</p>
              ${delayHtml}
            </details>`;
        }).join('');
        return `<section class="timing-group">
          <h3>${esc(group.name || 'Path group')}</h3>
          <p class="muted">Type ${esc(group.type || '')} · From ${esc(group.from_clock || '')} · To ${esc(group.to_clock || '')} · Min period ${esc(group.min_period || '')} · Fmax ${esc(group.fmax || '')}</p>
          ${searchableTable([['kind', 'Statistic'], ['swns_ns', 'SWNS (ns)'], ['stns_ns', 'STNS (ns)'],
            ['viol_endpoints', 'Viol endpoints'], ['total_endpoints', 'Total endpoints']], statRows)}
          ${pathHtml || '<p class="muted">No paths in this group.</p>'}
        </section>`;
    }).join('');

    const fallbackRows = paths.map((path) => ({
        slack: path.slack,
        from: path.from !== undefined ? path.from : (path.begin || ''),
        to: path.to !== undefined ? path.to : (path.end || ''),
        check: path.check || '',
    }));
    const fallbackPaths = fallbackRows.length && !groups.length ? `<h3>Paths (${fallbackRows.length})</h3>
      ${searchableTable([['slack', 'Slack (ns)'], ['from', 'From'], ['to', 'To'], ['check', 'Check']], fallbackRows)}` : '';

    return `
<section id="tab-timing" class="tab-panel">
  <h2>Timing</h2>
  <div class="cards">${cards}${status}</div>
  <h3>Report metadata</h3>
  ${metaRows.length ? `<table><thead><tr><th>Field</th><th>Value</th></tr></thead><tbody>${metaRows.map((row) => `<tr class="searchable"><th>${esc(row.key)}</th><td>${esc(row.value)}</td></tr>`).join('')}</tbody></table>` : '<p class="muted">No metadata.</p>'}
  <h3>Check status</h3>
  ${searchableTable([['corner', 'Corner'], ['max', 'Max'], ['min', 'Min']], checkStatus)}
  <h3>Clocks</h3>
  ${clockRows.length ? searchableTable([['name', 'Clock'], ['freq_mhz', 'Freq (MHz)'], ['fanout', 'Fanout'], ['source', 'Source']], clockRows) : '<p class="muted">No clock table.</p>'}
  ${clockDetail.length ? `<h3>Clock summary</h3>${searchableTable(Object.keys(clockDetail[0]).map((key) => [key, key]), clockDetail)}` : ''}
  <h3>Path groups</h3>
  ${groupHtml || '<p class="muted">No path groups.</p>'}
  ${fallbackPaths}
  ${rawBlock(data.raw, 'Raw timing report')}
</section>`;
}
function areaTab(area) {
    const data = area && typeof area === 'object' ? area : {};
    const rows = Array.isArray(data.rows) ? data.rows : [];
    const barCss = [];
    const bodyRows = rows.map((row, index) => {
        if (!row || typeof row !== 'object') return '';
        const percent = utilizationPercent(row);
        barCss.push(`.bar-${index} { width: ${percent.toFixed(1)}%; }`);
        return `<tr class="searchable">
      <td>${esc(row.resource !== undefined ? row.resource : row.name)}</td>
      <td>${esc(fmtNum(row.used))}</td>
      <td>${esc(fmtNum(row.total))}</td>
      <td>${esc(row.utilization !== undefined ? row.utilization : `${percent.toFixed(1)}%`)}</td>
      <td class="bar-cell"><div class="bar"><div class="bar-fill bar-${index} ${barLevel(percent)}"></div></div></td>
    </tr>`;
    }).join('');
    const counts = data.counts && typeof data.counts === 'object' ? data.counts : {};
    const countRows = Object.entries(counts).map(([key, value]) => ({ key, value }));
    const modules = Array.isArray(data.modules) ? data.modules : [];
    const moduleRows = modules.map((module) => {
        const resources = module.resources || {};
        const utilization = module.utilization || {};
        return {
            instance: module.path || module.name || '',
            module: module.module || '',
            lut: resources.lut, lut_util: fmtPercent(utilization.lut),
            ripple: resources.ripple, seq: resources.seq, seq_util: fmtPercent(utilization.seq),
            bram: resources.bram, dsp: resources.dsp,
        };
    });
    return {
        css: barCss.join('\n'),
        html: `
<section id="tab-area" class="tab-panel">
  <h2>Area</h2>
  ${rows.length
        ? `<h3>Device utilization</h3><table><thead><tr><th>Resource</th><th>Used</th><th>Total</th><th>Utilization</th><th></th></tr></thead><tbody>${bodyRows}</tbody></table>`
        : '<p class="muted">No area report found.</p>'}
  ${countRows.length ? `<h3>LUT statistics</h3>${searchableTable([['key', 'Metric'], ['value', 'Value']], countRows)}` : ''}
  <h3>Per-module utilization (${modules.length})</h3>
  ${moduleRows.length ? searchableTable([
        ['instance', 'Instance'], ['module', 'Module'],
        ['lut', 'LUT'], ['lut_util', 'LUT %'], ['ripple', 'Ripple'],
        ['seq', 'SEQ'], ['seq_util', 'SEQ %'], ['bram', 'BRAM'], ['dsp', 'DSP'],
    ], moduleRows) : '<p class="muted">No hierarchy area table.</p>'}
  ${rawBlock(data.raw, 'Raw area report')}
</section>`,
    };
}
function flowTab(reports) {
    const flow = (reports && reports.flow) || {};
    const flowRows = [
        ['Running', flow.running ? 'yes' : 'no'],
        ['Stage', flow.stage || 'idle'],
        ['Elapsed', fmtElapsed(flow.elapsed)],
        ['Last message', flow.lastMessage || '—'],
    ].map(([label, value]) => `<tr><th>${esc(label)}</th><td>${esc(value)}</td></tr>`).join('');

    const rawStatus = reports && reports.flowStatus;
    const statuses = Array.isArray(rawStatus)
        ? rawStatus
        : (rawStatus && Array.isArray(rawStatus.steps) ? rawStatus.steps : []);
    const statusRows = statuses.map((row) => {
        if (!row || typeof row !== 'object') return '';
        const step = row.step || row.name || row.stage || '';
        const status = row.status || row.stage || '';
        const wall = row.elapsed || row.wall || '';
        const cpu = row.cpu || '';
        const mem = row.mem_mb !== undefined
            ? `${row.mem_mb} MB`
            : (row.peak_mb !== undefined ? `peak ${row.peak_mb} MB` : '');
        return `<tr><td>${esc(step)}</td><td>${esc(status)}</td><td>${esc(wall)}</td><td>${esc(cpu)}</td><td>${esc(mem)}</td></tr>`;
    }).join('');

    // reports.py additionally returns setup/hold timing counters from the flow status file.
    const rawFlowStatus = rawStatus && !Array.isArray(rawStatus) ? rawStatus : {};
    const flowSummary = [];
    if (rawFlowStatus.setup) {
        flowSummary.push(`Setup: WNS ${esc(fmtNum(rawFlowStatus.setup.wns_ps))} ps · TNS ${esc(fmtNum(rawFlowStatus.setup.tns_ps))} ps · FEPS ${esc(fmtNum(rawFlowStatus.setup.feps))}`);
    }
    if (rawFlowStatus.hold) {
        flowSummary.push(`Hold: WNS ${esc(fmtNum(rawFlowStatus.hold.wns_ps))} ps · TNS ${esc(fmtNum(rawFlowStatus.hold.tns_ps))} ps · FEPS ${esc(fmtNum(rawFlowStatus.hold.feps))}`);
    }
    if (rawFlowStatus.mode) flowSummary.push(`Timing mode: ${esc(rawFlowStatus.mode)}`);

    const files = (reports && reports.files) || {};
    const fileRows = [
        ['qor', 'QoR report'],
        ['timing', 'Timing report'],
        ['area', 'Area report'],
    ].map(([key, label]) => {
        const file = files[key];
        if (typeof file === 'string' && file) {
            return `<li>${esc(label)}: ${fileLink('fpgaLsp.openReportFile', file)}</li>`;
        }
        return `<li class="muted">${esc(label)}: not parsed</li>`;
    }).join('');

    const artifacts = (reports && reports.artifacts) || {};
    const artifactRows = [
        ['bit', 'Bitstream'],
        ['syn_db', 'Synthesis DB'],
        ['route_db', 'Route DB'],
    ].map(([key, label]) => {
        const artifact = artifacts[key];
        if (!artifact || typeof artifact !== 'object' || !artifact.path) {
            return `<li class="muted">${esc(label)}: not built</li>`;
        }
        const meta = [
            artifact.exists === false ? 'missing' : 'exists',
            artifact.size !== undefined ? fmtSize(artifact.size) : '',
            artifact.mtime !== undefined ? fmtMtime(artifact.mtime) : '',
        ].filter(Boolean).join(' · ');
        return `<li>${esc(label)}: ${fileLink('fpgaLsp.openArtifact', String(artifact.path))} <span class="muted">${esc(meta)}</span></li>`;
    }).join('');

    return `
<section id="tab-flow" class="tab-panel">
  <h2>Flow</h2>
  <table class="flow"><tbody>${flowRows}</tbody></table>
  ${flowSummary.length ? `<p class="muted">${flowSummary.join('<br>')}</p>` : ''}
  <h3>Stage status</h3>
  ${statusRows
        ? `<table><thead><tr><th>Step</th><th>Status</th><th>Wall</th><th>CPU</th><th>Mem</th></tr></thead><tbody>${statusRows}</tbody></table>`
        : '<p class="muted">No stage status reported.</p>'}
  <h3>Parsed report files</h3>
  <ul class="files">${fileRows}</ul>
  <h3>Artifacts</h3>
  <ul class="files">${artifactRows}</ul>
</section>`;
}

function drcCdcTab(reports) {
    const analysis = (reports && reports.analysis) || undefined;
    const head = '<h2>DRC / CDC</h2>';
    if (!analysis) {
        return `<section id="tab-drc-cdc" class="tab-panel">${head}
  <p class="muted">No analysis results yet. Click <b>Run DRC/CDC</b> above (reads the placed/routed database, ~3 s).</p>
</section>`;
    }
    const drc = analysis.drc || {};
    const cdc = analysis.cdc || {};
    const drcBadge = drc.supported === false
        ? '<span class="badge">unsupported</span>'
        : `<span class="badge ${drc.violations ? 'status-notmet' : 'status-met'}">${esc(drc.violations || 0)} violations</span>`;
    const rules = (drc.rules || []).map((rule) => `<tr><td>${esc(rule.rule)}</td><td>${esc(rule.severity)}</td><td>${esc(rule.count)}</td></tr>`).join('');
    const details = (drc.details || []).slice(0, 60).map((line) => `<li>${esc(line)}</li>`).join('');
    const cdcBadge = cdc.supported === false
        ? '<span class="badge">unsupported</span>'
        : `<span class="badge ${(cdc.counts && cdc.counts.unsafe) ? 'status-notmet' : 'status-met'}">${esc((cdc.counts && cdc.counts.unsafe) || 0)} unsafe</span>`;
    const cdcRows = (cdc.rows || []).slice(0, 40).map((row) => `<tr>${(Array.isArray(row) ? row : [row]).map((cell) => `<td>${esc(cell)}</td>`).join('')}</tr>`).join('');
    const files = (analysis.files || {});
    return `<section id="tab-drc-cdc" class="tab-panel">${head}
  <h3>DRC ${drcBadge}</h3>
  ${drc.supported === false
        ? `<p class="muted">DRC unavailable: ${esc(drc.reason || '')}</p>`
        : (rules
            ? `<table><thead><tr><th>Rule</th><th>Severity</th><th>Violations</th></tr></thead><tbody>${rules}</tbody></table>`
            : '<p class="muted">No DRC violations (clean designs do not produce a report file).</p>')}
  ${details ? `<h3>DRC details</h3><ul class="suggestions">${details}</ul>` : ''}
  ${files.drc ? `<p class="muted">Report file: ${fileLink('fpgaLsp.openReportFile', files.drc)}</p>` : ''}
  <h3>CDC ${cdcBadge}</h3>
  ${cdc.supported === false
        ? `<p class="muted">${esc(cdc.reason || 'CDC analysis is not supported for this device')}</p>`
        : (cdcRows
            ? `<table><thead><tr><th colspan="8">Clock pairs</th></tr></thead><tbody>${cdcRows}</tbody></table>`
            : '<p class="muted">No CDC data.</p>')}
  ${files.cdc ? `<p class="muted">Report file: ${fileLink('fpgaLsp.openReportFile', files.cdc)}</p>` : ''}
</section>`;
}

function compareTab(reports) {
    const compare = (reports && reports.compare) || undefined;
    const head = '<h2>Strategy comparison</h2>';
    if (!compare) {
        return `<section id="tab-compare" class="tab-panel">${head}
  <p class="muted">No comparison results yet. Click <b>Compare</b> above (runs synthesis + P&amp;R per strategy on a project copy, ~20-60 s).</p>
</section>`;
    }
    const runs = compare.runs || [];
    const rows = runs.map((run) => {
        const best = compare.best && compare.best.run === run.run ? ' <span class="badge">best</span>' : '';
        const cells = [run.strategy || run.run, run.setup_wns_ps, run.setup_tns_ps, run.luts, run.slices, run.slice_pct];
        return `<tr>${cells.map((cell) => `<td>${cell === undefined || cell === null ? '—' : esc(cell)}</td>`).join('')}<td>${best}</td></tr>`;
    }).join('');
    return `<section id="tab-compare" class="tab-panel">${head}
  <p class="muted">Runs on a project copy (${esc(compare.work || '')}); your .al is untouched.</p>
  ${rows
        ? `<table><thead><tr><th>Strategy</th><th>Setup WNS (ps)</th><th>Setup TNS (ps)</th><th>LUTs</th><th>Slices</th><th>Slice %</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
        : '<p class="muted">No comparable run results.</p>'}
  ${compare.elapsed ? `<p class="muted">Total ${esc(compare.elapsed)} s; available strategies: ${esc((compare.available || []).join(', '))}</p>` : ''}
</section>`;
}

/**
 * Render the whole webview document.
 * @param {{reports: any, nonce: string, cspSource?: string}} options
 * @returns {string} complete HTML
 */
/** Compact metric table used by the dashboard and the per-stage tabs. */
function metricsTable(metrics, limit) {
    const pairs = metricPairs(metrics).slice(0, limit || 14);
    if (!pairs.length) return '<p class="muted">No metrics.</p>';
    return '<table class="metrics"><thead><tr><th>Metric</th><th>Value</th></tr></thead><tbody>'
        + pairs.map(([name, value]) => `<tr><th>${esc(name)}</th><td>${esc(fmtNum(value))}</td></tr>`).join('')
        + '</tbody></table>';
}

function timingSummaryCards(timing) {
    const data = timing && typeof timing === 'object' ? timing : {};
    const summary = data.summary && typeof data.summary === 'object'
        ? data.summary
        : (['SWNS', 'STNS', 'HWNS', 'HTNS'].some((key) => data[key] !== undefined) ? data : {});
    return ['SWNS', 'STNS', 'HWNS', 'HTNS'].map((key) => `
    <div class="card">
      <div class="label">${esc(key)}</div>
      <div class="value">${esc(fmtNum(summary[key]))}${summary[key] === undefined ? '' : '<span class="unit">ns</span>'}</div>
    </div>`).join('');
}

function areaRows(area) {
    const rows = (area && Array.isArray(area.rows)) ? area.rows : [];
    if (!rows.length) return '<p class="muted">No area report.</p>';
    return '<table class="metrics"><thead><tr><th>Resource</th><th>Used</th><th>Total</th><th>Util.</th></tr></thead><tbody>'
        + rows.map((row) => `<tr><th>${esc(row.resource)}</th><td>${esc(row.used)}</td><td>${esc(row.total)}</td><td>${esc(row.utilization)}</td></tr>`).join('')
        + '</tbody></table>';
}

function stageTab(section, id, title) {
    const data = section && typeof section === 'object' ? section : {};
    const files = data.files || {};
    return `
<section id="${esc(id)}" class="tab-panel">
  <h2>${esc(title)} ${data.ok === false ? '<span class="badge">no artifacts</span>' : ''}</h2>
  <h3>QoR</h3>
  ${metricsTable((data.qor || {}).metrics)}
  <h3>Timing</h3>
  <div class="cards">${timingSummaryCards(data.timing)}</div>
  <h3>Area</h3>
  ${areaRows(data.area)}
  <h3>Files</h3>
  <ul class="files">${Object.entries(files).filter(([, value]) => value)
        .map(([key, value]) => `<li>${esc(key)}: ${fileLink('fpgaLsp.openReportFile', value, basename(value))}</li>`).join('')
        || '<li class="muted">none</li>'}</ul>
</section>`;
}

function dashboardTab(reports) {
    const qor = reports.qor || {};
    const timing = reports.timing || {};
    const area = reports.area || {};
    const analysis = reports.analysis || {};
    const suggestions = Array.isArray(qor.suggestions) ? qor.suggestions : [];
    const drc = analysis.drc || {};
    const cdc = analysis.cdc || {};
    const flow = reports.flowStatus || reports.flow || {};
    const files = reports.files || {};
    const drcText = drc.available === false || drc.error
        ? `not available (${esc(drc.error || '')})`
        : `${esc(drc.violations === undefined ? 0 : drc.violations)} violation(s)`;
    const cdcText = cdc.available === false || cdc.error
        ? `not available (${esc(cdc.error || '')})`
        : `${esc((cdc.counts && (cdc.counts.Unsafe || cdc.counts.unsafe)) || 0)} unsafe / ${esc((cdc.counts && (cdc.counts.Safe || cdc.counts.safe)) || 0)} safe`;
    const flowRows = Array.isArray(flow.steps) ? flow.steps : [];
    return `
<section id="tab-dashboard" class="tab-panel active">
  <h2>Dashboard — full analysis</h2>
  <div class="cards">${timingSummaryCards(timing)}</div>
  <h3>QoR metrics</h3>
  ${metricsTable(qor.metrics, 200)}
  <h3>Area utilization</h3>
  ${areaRows(area)}
  <h3>Flow</h3>
  ${flowRows.length
        ? `<table class="metrics"><thead><tr><th>Stage</th><th>Step</th><th>Status</th><th>Elapsed</th></tr></thead><tbody>${flowRows.map((step) => `<tr><th>${esc(step.stage || '')}</th><td>${esc(step.name || step.step || '')}</td><td>${esc(step.status || step.stage || '')}</td><td>${esc(step.wall || fmtElapsed(step.elapsed || 0))}</td></tr>`).join('')}</tbody></table>`
        : '<p class="muted">No flow status yet.</p>'}
  <h3>DRC / CDC</h3>
  <p>DRC: ${drcText} · CDC: ${cdcText}</p>
  ${Array.isArray(drc.rules) && drc.rules.length
        ? `<table class="metrics"><thead><tr><th>DRC rule</th><th>Severity</th><th>Count</th></tr></thead><tbody>${drc.rules.map((rule) => `<tr><th>${esc(rule.rule)}</th><td>${esc(rule.severity)}</td><td>${esc(rule.count)}</td></tr>`).join('')}</tbody></table>`
        : ''}
  ${cdc.counts && typeof cdc.counts === 'object'
        ? `<table class="metrics"><thead><tr><th>CDC class</th><th>Count</th></tr></thead><tbody>${Object.entries(cdc.counts).map(([key, value]) => `<tr><th>${esc(key)}</th><td>${esc(value)}</td></tr>`).join('')}</tbody></table>`
        : ''}
  <h3>Suggestions</h3>
  ${suggestions.length
        ? `<ul class="suggestions">${suggestions.map((entry) => `<li>${highlightQor(esc(typeof entry === 'string' ? entry : (entry.text || entry.message || entry.id || '')))}</li>`).join('')}</ul>`
        : '<p class="muted">No suggestions.</p>'}
  <h3>Report files</h3>
  <ul class="files">${Object.entries(files).filter(([, value]) => value)
        .map(([key, value]) => `<li>${esc(key)}: ${fileLink('fpgaLsp.openReportFile', value, basename(value))}</li>`).join('')
        || '<li class="muted">none</li>'}</ul>
</section>`;
}

/**
 * Normalise `reports` + `stage` into the single `view` object every section renderer uses.
 * (Extracted in Phase 2 so the panel can re-render **one section** without rebuilding the page.)
 */
function buildReportView(reports, stage) {
    const stageData = stage ? (reports[stage] || {}) : undefined;
    const stageLabel = stage === 'syn' ? 'Synthesis' : (stage === 'phy' ? 'Place & Route' : '');
    if (!stage) return reports;
    return Object.assign({}, reports, stageData || {}, {
        ok: reports.ok === true && !!stageData && stageData.ok !== false,
        label: stageLabel,
        stage,
        // stage data wins, but keep the full-report fields the stage file does not carry
        qor: Object.assign({}, reports.qor || {}, (stageData && stageData.qor) || {}),
        timing: Object.assign({}, reports.timing || {}, (stageData && stageData.timing) || {}),
        area: Object.assign({}, reports.area || {}, (stageData && stageData.area) || {}),
        flowStatus: (stageData && stageData.flowStatus) || reports.flowStatus,
        files: Object.assign({}, reports.files || {}, (stageData && stageData.files) || {}),
    });
}

/** Ordered section ids for one `view` (empty when the report has no data). */
function reportSectionIds(view, stage) {
    if (!view || view.ok !== true) return [];
    const ids = ['dashboard'];
    if (!stage) ids.push('synthesis', 'place-route');
    ids.push('qor', 'timing', 'area', 'flow', 'drc-cdc', 'compare');
    return ids;
}

/**
 * HTML of **one** `<section id="tab-…">` block. Used both by the full-page renderer and by the
 * webview's incremental `patch` messages.
 * @param {object} view   result of `buildReportView`
 * @param {object} reports full reports payload (needed for the per-stage tabs)
 * @param {string} id     'dashboard' | 'qor' | 'timing' | 'area' | 'flow' | 'drc-cdc' |
 *                        'compare' | 'synthesis' | 'place-route'
 * @param {{html:string, css:string}} [area] cached `areaTab(view.area)` result
 */
function reportSectionHtml(view, reports, id, area) {
    const areaBlock = area || areaTab(view.area);
    let html;
    switch (id) {
        case 'dashboard':
            html = dashboardTab(view);
            break;
        case 'qor':
            html = qorTab(view.qor);
            break;
        case 'timing':
            html = timingTab(view.timing);
            break;
        case 'area':
            html = areaBlock.html;
            break;
        case 'flow':
            html = flowTab(view);
            break;
        case 'drc-cdc':
            html = drcCdcTab(view);
            break;
        case 'compare':
            html = compareTab(view);
            break;
        case 'synthesis':
            html = stageTab(reports ? reports.syn : undefined, 'tab-synthesis', 'Synthesis');
            break;
        case 'place-route':
            html = stageTab(reports ? reports.phy : undefined, 'tab-place-route', 'Place & Route');
            break;
        default:
            return '';
    }
    if (id !== 'dashboard') {
        // only the dashboard may start "active" (the others are legacy tabs that mark themselves)
        html = html.replace(/class="tab-panel active"/g, 'class="tab-panel"');
    }
    return html;
}

/** One-line header meta (`Synthesis stage · Generated … · stage phy · flow running`). */
function reportMeta(reports, stage) {
    const safe = reports && typeof reports === 'object' ? reports : { ok: false };
    const stageKey = stage === 'syn' || stage === 'phy' ? stage : '';
    const view = buildReportView(safe, stageKey);
    const stageLabel = stageKey === 'syn' ? 'Synthesis' : (stageKey === 'phy' ? 'Place & Route' : '');
    const generated = view.generatedAt ? fmtDate(view.generatedAt) : '';
    const flow = view.flow || {};
    return [
        stageKey ? `${stageLabel} stage` : '',
        generated ? `Generated ${generated}` : '',
        flow.stage ? `stage ${flow.stage}` : '',
        flow.running ? 'flow running' : '',
    ].filter(Boolean).join(' · ');
}

function renderReportsHtml(options) {
    const opts = options || {};
    const reports = opts.reports && typeof opts.reports === 'object' ? opts.reports : { ok: false };
    const nonce = sanitizeNonce(opts.nonce);
    const stage = opts.stage === 'syn' || opts.stage === 'phy' ? opts.stage : '';
    const view = buildReportView(reports, stage);
    const ok = view.ok === true;
    const stageLabel = stage === 'syn' ? 'Synthesis' : (stage === 'phy' ? 'Place & Route' : '');
    const title = stage ? `${stageLabel} Reports` : 'FPGA Reports';

    const meta = reportMeta(reports, stage);

    const area = areaTab(view.area);
    let body;
    if (ok) {
        body = reportSectionIds(view, stage)
            .map((id) => reportSectionHtml(view, reports, id, area))
            .join('');
    } else {
        body = emptyStateHtml(view);
    }

    const tabs = ok
        ? `<nav class="tabs">${(stage ? STAGE_TABS : REPORT_TABS).map((tab, index) => (
            `<button data-tab="${tabId(tab)}" class="${index === 0 ? 'active' : ''}">${esc(tab)}</button>`
        )).join('')}</nav>`
        : '';

    const style = `
:root { color-scheme: light dark; }
* { box-sizing: border-box; }
body {
  font-family: var(--vscode-font-family, -apple-system, "Segoe UI", sans-serif);
  font-size: 13px;
  color: var(--vscode-foreground, #ccc);
  background: var(--vscode-editor-background, #1e1e1e);
  margin: 0;
  padding: 0 20px 40px;
}
header {
  position: sticky; top: 0; z-index: 2;
  display: flex; align-items: center; justify-content: space-between; gap: 16px;
  padding: 14px 0 10px;
  background: var(--vscode-editor-background, #1e1e1e);
  border-bottom: 1px solid var(--vscode-panel-border, #444);
}
h1 { font-size: 19px; margin: 0; font-weight: 600; }
.meta { opacity: .65; margin: 8px 0 0; font-size: 12px; }
.actions { display: flex; gap: 6px; flex-wrap: wrap; }
.actions button {
  font-family: inherit; font-size: 12px; cursor: pointer;
  color: var(--vscode-button-foreground, #fff);
  background: var(--vscode-button-background, #0e639c);
  border: none; border-radius: 3px; padding: 5px 10px;
}
.actions button.secondary {
  color: var(--vscode-button-secondaryForeground, #fff);
  background: var(--vscode-button-secondaryBackground, #3a3d41);
}
.search { display: flex; align-items: center; gap: 8px; flex: 1; justify-content: flex-end; }
.search input {
  font-family: inherit; font-size: 12px; width: 280px; padding: 5px 8px;
  color: var(--vscode-input-foreground, #ccc);
  background: var(--vscode-input-background, #3c3c3c);
  border: 1px solid var(--vscode-input-border, #555); border-radius: 3px;
}
.raw { max-height: 420px; overflow: auto; border: 1px solid var(--vscode-panel-border, #444); padding: 6px; font-family: var(--vscode-editor-font-family, monospace); font-size: 12px; }
.raw-line { white-space: pre-wrap; word-break: break-all; }
.path-card { border: 1px solid var(--vscode-panel-border, #444); border-radius: 4px; padding: 6px 10px; margin: 8px 0; }
.timing-group { border-top: 1px solid var(--vscode-panel-border, #444); margin-top: 16px; padding-top: 6px; }
.actions button:hover { filter: brightness(1.1); }
.tabs { display: flex; gap: 2px; margin-top: 10px; border-bottom: 1px solid var(--vscode-panel-border, #444); }
.tabs button {
  font-family: inherit; font-size: 13px; cursor: pointer;
  background: none; border: none; border-bottom: 2px solid transparent;
  color: var(--vscode-foreground, #ccc); opacity: .75; padding: 8px 14px;
}
.tabs button:hover { opacity: 1; }
.tabs button.active { opacity: 1; border-bottom-color: var(--vscode-focusBorder, #0a84ff); }
.tab-panel { display: none; }
.tab-panel.active { display: block; }
h2 { font-size: 15px; margin: 20px 0 8px; font-weight: 600; }
h3 { font-size: 11px; margin: 18px 0 6px; text-transform: uppercase; letter-spacing: .08em; opacity: .7; font-weight: 600; }
.badge { font-size: 11px; font-weight: 400; border: 1px solid currentColor; border-radius: 9px; padding: 1px 7px; opacity: .75; margin-left: 6px; }
table { border-collapse: collapse; width: 100%; margin: 4px 0 12px; }
th, td { text-align: left; padding: 5px 8px; border-bottom: 1px solid var(--vscode-panel-border, #3a3a3a); vertical-align: middle; }
th { font-weight: 600; opacity: .75; }
td.bar-cell { width: 30%; }
.cards { display: flex; flex-wrap: wrap; gap: 10px; margin: 8px 0 12px; }
.card { border: 1px solid var(--vscode-panel-border, #444); border-radius: 6px; padding: 10px 14px; min-width: 130px; }
.card .label { font-size: 11px; text-transform: uppercase; letter-spacing: .07em; opacity: .65; }
.card .value { font-size: 21px; font-weight: 600; margin-top: 2px; }
.card .unit { font-size: 11px; opacity: .65; margin-left: 3px; }
.status-met { color: var(--vscode-charts-green, #3fb950); }
.status-notmet, .status-failed { color: var(--vscode-charts-red, #f85149); }
.bar { height: 8px; border-radius: 4px; overflow: hidden; background: var(--vscode-input-background, rgba(127,127,127,.25)); }
.bar-fill { height: 100%; border-radius: 4px; }
.bar-green { background: var(--vscode-charts-green, #3fb950); }
.bar-orange { background: var(--vscode-charts-orange, #d29922); }
.bar-red { background: var(--vscode-charts-red, #f85149); }
code.qor-id { font-family: var(--vscode-editor-font-family, monospace); font-size: 12px; padding: 1px 5px; border-radius: 3px; background: var(--vscode-textCodeBlock-background, rgba(127,127,127,.18)); }
.prio { font-size: 10px; text-transform: uppercase; letter-spacing: .06em; padding: 1px 6px; border-radius: 8px; border: 1px solid currentColor; opacity: .85; }
.suggestions li { margin: 5px 0; line-height: 1.45; }
.files { padding-left: 18px; margin: 4px 0 12px; line-height: 1.7; }
.files a { color: var(--vscode-textLink-foreground, #3794ff); text-decoration: none; }
.files a:hover { text-decoration: underline; }
.muted { opacity: .6; }
.empty { max-width: 560px; margin: 60px auto; text-align: center; }
.empty h2 { font-size: 18px; margin-bottom: 8px; }
.empty p { line-height: 1.6; }
${area.css}`;

    const script = `
(function () {
  const api = acquireVsCodeApi();
  const search = document.getElementById('report-search');
  if (search) {
    const count = document.getElementById('search-count');
    search.addEventListener('input', function () {
      const query = search.value.trim().toLowerCase();
      let shown = 0;
      let total = 0;
      document.querySelectorAll('.searchable').forEach(function (element) {
        const text = (element.textContent || '').toLowerCase();
        const hit = !query || text.indexOf(query) >= 0;
        element.style.display = hit ? '' : 'none';
        if (hit) shown += 1;
        total += 1;
      });
      if (count) count.textContent = query ? shown + '/' + total : '';
    });
  }
  document.querySelectorAll('[data-tab]').forEach(function (button) {
    button.addEventListener('click', function () {
      document.querySelectorAll('[data-tab]').forEach(function (other) {
        other.classList.toggle('active', other === button);
      });
      document.querySelectorAll('.tab-panel').forEach(function (panel) {
        panel.classList.toggle('active', panel.id === 'tab-' + button.dataset.tab);
      });
      // D1: tell the host which tab became visible so it can fetch that report section on
      // demand (QoR / Timing / Area are no longer part of the lightweight "summary" payload).
      api.postMessage({ type: 'tab', id: button.dataset.tab });
    });
  });
  document.querySelectorAll('[data-action]').forEach(function (button) {
    button.addEventListener('click', function () {
      const action = button.dataset.action;
      if (action === 'refresh') api.postMessage({ type: 'refresh' });
      if (action === 'syn') api.postMessage({ type: 'run', stages: ['syn'] });
      if (action === 'all') api.postMessage({ type: 'run', stages: ['all'] });
      if (action === 'analysis') api.postMessage({ type: 'analysis' });
      if (action === 'compare') api.postMessage({ type: 'compare' });
    });
  });
  // Phase 2: incremental section patch (the host replaces one <section> instead of the whole
  // document, so scroll position / search box / active tab survive a refresh).
  window.addEventListener('message', function (event) {
    const message = event.data;
    if (!message || message.type !== 'patch' || !message.sections) return;
    Object.keys(message.sections).forEach(function (id) {
      const target = document.getElementById('tab-' + id);
      if (!target) return;
      const wasActive = target.classList.contains('active');
      target.outerHTML = message.sections[id];
      const next = document.getElementById('tab-' + id);
      if (next && wasActive) next.classList.add('active');
    });
    if (typeof message.meta === 'string') {
      const meta = document.querySelector('.meta');
      if (meta) meta.textContent = message.meta;
    }
    if (Array.isArray(message.tabs)) {
      const nav = document.querySelector('nav.tabs');
      if (nav) {
        nav.innerHTML = message.tabs.map(function (tab, index) {
          return '<button data-tab="' + tab.id + '"' + (index === 0 ? ' class="active"' : '') + '>'
            + tab.label + '</button>';
        }).join('');
      }
    }
  });
}());`;

    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}';">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>${esc(title)}</title>
<style nonce="${nonce}">${style}
</style>
</head>
<body>
<header>
  <div>
    <h1>${esc(title)}</h1>
    ${meta ? `<div class="meta">${esc(meta)}</div>` : ''}
  </div>
  <div class="search">
    <input id="report-search" type="search" placeholder="Search report (module, clock, path, rule…)" />
    <span id="search-count" class="muted"></span>
  </div>
  <div class="actions">
    <button data-action="refresh" class="secondary">Refresh</button>
    <button data-action="syn">Run Synthesis</button>
    <button data-action="all">Run Full Flow</button>
    <button data-action="analysis" class="secondary">Run DRC/CDC</button>
    <button data-action="compare" class="secondary">Compare</button>
  </div>
</header>
${tabs}
${body}
<script nonce="${nonce}">${script}
</script>
</body>
</html>`;
}

module.exports = {
    renderReportsHtml,
    renderReportSection,
    buildReportView,
    reportSectionIds,
    reportMeta,
    // D1: the panel patches single sections, so it computes the area block once and reuses it.
    areaTab,
    REPORT_TABS,
    STAGE_TABS,
    tabId,
    esc,
    highlightQor,
    commandHref,
    utilizationPercent,
    fmtSize,
    fmtTime: fmtMtime,
    fmtElapsed,
};

/**
 * Full-page section list + HTML map for one payload (used by the panel's patch path).
 * @param {{reports: any, stage?: string}} options
 * @param {string} id section id
 * @param {{html: string, css: string}} [area] optional cached `areaTab(view.area)` result (D1:
 *   the panel now re-renders a single section, and recomputing the area table for every section
 *   was the most expensive part of a patch).
 */
function renderReportSection(options, id, area) {
    const opts = options || {};
    const reports = opts.reports && typeof opts.reports === 'object' ? opts.reports : { ok: false };
    const stage = opts.stage === 'syn' || opts.stage === 'phy' ? opts.stage : '';
    const view = buildReportView(reports, stage);
    if (view.ok !== true) return '';
    return reportSectionHtml(view, reports, id, area);
}
