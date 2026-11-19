// X-B2: QoR trend across flow runs (`<project>_Runs/<name>`).
//
// Pure renderer so `test/run.js` can assert the SVG/table without VS Code.
// Data comes from the server command `fpgalsp.runs`
// ({ok, runs:[{name, dir, files:[...], values:{metric: number}}]}).
//
// The panel must be created with `enableCommandUris: true` — each point links to the
// run's QoR file through `command:fpgaLsp.openReportFile?...`.

'use strict';

const METRICS = [
    { key: 'Setup WNS', label: 'Setup WNS (ps)', higher: true },
    { key: 'Setup TNS', label: 'Setup TNS (ps)', higher: true },
    { key: 'Hold WNS', label: 'Hold WNS (ps)', higher: true },
    { key: 'FMAX', label: 'FMAX (MHz)', higher: true },
    { key: 'area:lut', label: 'LUT', higher: false },
    { key: 'area:seq', label: 'SEQ (FF)', higher: false },
    { key: 'area:bram', label: 'BRAM', higher: false },
];

function esc(value) {
    return String(value === undefined || value === null ? '' : value)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function commandHref(command, argument) {
    return `command:${command}?${encodeURIComponent(JSON.stringify([argument]))}`;
}

function fmt(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return '';
    return Math.abs(number) >= 100 ? number.toFixed(0) : number.toFixed(2);
}

/** Which metric to plot by default: first one the runs actually provide. */
function pickMetric(runs) {
    for (const metric of METRICS) {
        if (runs.some((run) => Number.isFinite(Number(run.values && run.values[metric.key])))) {
            return metric;
        }
    }
    return METRICS[0];
}

/** Geometry for one metric: [{x, y, run, value, file}]. */
function seriesFor(runs, metricKey) {
    const points = [];
    runs.forEach((run, index) => {
        const raw = run.values ? run.values[metricKey] : undefined;
        const value = Number(raw);
        if (!Number.isFinite(value)) return;
        points.push({ index, run, value, file: (run.files || [])[0] || '' });
    });
    return points;
}

function svgFor(points, width, height) {
    const pad = 28;
    const innerWidth = width - pad * 2;
    const innerHeight = height - pad * 2;
    if (!points.length) {
        return `<svg viewBox="0 0 ${width} ${height}" class="trend"><text x="${pad}" y="${height / 2}">no data</text></svg>`;
    }
    const values = points.map((point) => point.value);
    const min = Math.min(...values);
    const max = Math.max(...values);
    const span = max - min || 1;
    const step = points.length > 1 ? innerWidth / (points.length - 1) : 0;
    const coords = points.map((point, index) => {
        const x = pad + (points.length > 1 ? index * step : innerWidth / 2);
        const y = pad + innerHeight - ((point.value - min) / span) * innerHeight;
        return { ...point, x, y };
    });
    const polyline = coords.map((point) => `${point.x.toFixed(1)},${point.y.toFixed(1)}`).join(' ');
    const dots = coords.map((point) => {
        const href = point.file
            ? commandHref('fpgaLsp.openReportFile', point.file)
            : '';
        const circle = `<circle cx="${point.x.toFixed(1)}" cy="${point.y.toFixed(1)}" r="4">`
            + `<title>${esc(point.run.name)}: ${esc(fmt(point.value))}</title></circle>`;
        return href ? `<a href="${esc(href)}">${circle}</a>` : circle;
    }).join('');
    const labels = coords.map((point) => `<text x="${point.x.toFixed(1)}" y="${height - 8}" `
        + `text-anchor="middle" class="axis">${esc(point.run.name)}</text>`).join('');
    return `<svg viewBox="0 0 ${width} ${height}" class="trend" role="img">
  <polyline points="${polyline}" fill="none" class="line"/>
  ${dots}
  ${labels}
  <text x="${pad}" y="14" class="axis">max ${esc(fmt(max))}</text>
  <text x="${pad}" y="${height - 8}" class="axis">min ${esc(fmt(min))}</text>
</svg>`;
}

/**
 * @param {{runs?: Array, nonce?: string}} options
 * @returns {string} a complete webview HTML document
 */
function renderTrendHtml(options) {
    const opts = options || {};
    const runs = Array.isArray(opts.runs) ? opts.runs.filter((run) => run && run.name) : [];
    const nonce = String(opts.nonce || 'fpga-trend').replace(/[^A-Za-z0-9_-]/g, '') || 'n';
    if (!runs.length) {
        return `<!DOCTYPE html><html><head><meta charset="utf-8"><title>FPGA QoR trend</title></head>
<body><p>No flow runs found under <code>_Runs/</code> yet. Run a flow first.</p></body></html>`;
    }

    const metric = pickMetric(runs);
    const points = seriesFor(runs, metric.key);
    const header = runs.map((run) => `<th>${esc(run.name)}</th>`).join('');
    const rows = METRICS.map((entry) => {
        const cells = runs.map((run) => {
            const value = run.values ? run.values[entry.key] : undefined;
            return `<td>${esc(fmt(value))}</td>`;
        }).join('');
        return `<tr><th>${esc(entry.label)}</th>${cells}</tr>`;
    }).join('');

    return `<!DOCTYPE html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
<title>FPGA QoR trend</title>
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 12px; }
  .trend { width: 100%; max-width: 720px; }
  .trend .line { stroke: var(--vscode-charts-blue, #3794ff); stroke-width: 2; }
  .trend circle { fill: var(--vscode-charts-blue, #3794ff); cursor: pointer; }
  .trend .axis { fill: var(--vscode-descriptionForeground); font-size: 10px; }
  table { border-collapse: collapse; margin-top: 16px; font-size: 12px; }
  th, td { border: 1px solid var(--vscode-panel-border, #444); padding: 3px 8px; text-align: right; }
  th:first-child { text-align: left; }
  h2 { font-size: 14px; }
  .muted { color: var(--vscode-descriptionForeground); }
</style></head>
<body>
<h2>QoR trend — ${esc(metric.label)}</h2>
<p class="muted">${runs.length} runs · click a point to open that run's QoR file</p>
${svgFor(points, 720, 180)}
<table><thead><tr><th>Metric</th>${header}</tr></thead><tbody>${rows}</tbody></table>
<p class="muted">Source: <code>&lt;project&gt;_Runs/&lt;run&gt;</code> (sparkline = ${esc(metric.label)})</p>
<script nonce="${nonce}"></script>
</body></html>`;
}

module.exports = { renderTrendHtml, pickMetric, seriesFor, METRICS };