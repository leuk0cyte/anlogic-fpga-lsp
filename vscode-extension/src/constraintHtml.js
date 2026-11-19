// X-B3: constraint table (read-only view of `.adc` entries + per-line diagnostics).
//
// The table is the missing middle ground between "raw .adc text" and "Problems list":
// the user can see every pin assignment at a glance, see which rows have diagnostics, and
// jump to the exact line. Editing stays in the editor (WorkspaceEdit / undo) -- a webview
// table that writes files behind the user's back is exactly the kind of thing this
// extension must not do.
//
// Data: `fpgalsp.constraintTable` -> {ok, project, files:[{file, rows:[...]}]}.
// Kept free of `require('vscode')` so test/run.js can render it.

'use strict';

function esc(value) {
    return String(value === undefined || value === null ? '' : value)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function commandHref(command, argument) {
    return `command:${command}?${encodeURIComponent(JSON.stringify([argument]))}`;
}

function basename(path) {
    const parts = String(path || '').split(/[\\/]/);
    return parts[parts.length - 1] || String(path || '');
}

function cell(value) {
    return value === null || value === undefined || value === '' ? '<span class="muted">—</span>' : esc(value);
}

/** One table per .adc file. */
function renderConstraintHtml(options) {
    const opts = options || {};
    const files = Array.isArray(opts.files) ? opts.files.filter((file) => file && file.file) : [];
    const nonce = String(opts.nonce || 'fpga-constraints').replace(/[^A-Za-z0-9_-]/g, '') || 'n';

    if (!files.length) {
        return `<!DOCTYPE html><html><head><meta charset="utf-8"><title>FPGA constraints</title></head>
<body><p>No <code>.adc</code> pin constraints in this project.</p></body></html>`;
    }

    const sections = files.map((file) => {
        const rows = Array.isArray(file.rows) ? file.rows : [];
        const withIssues = rows.filter((row) => Number(row.issues) > 0).length;
        const body = rows.map((row) => {
            const line = Number(row.line) || 0;
            const href = commandHref('fpgaLsp.openSourceLocation', { file: file.file, line });
            const badge = Number(row.issues) > 0
                ? `<span class="badge">${Number(row.issues)}</span>`
                : '';
            return `<tr class="searchable${Number(row.issues) > 0 ? ' has-issue' : ''}">
  <td><a href="${esc(href)}" title="${esc(file.file + ':' + (line + 1))}">${line + 1}</a></td>
  <td>${cell(row.port)}</td>
  <td>${cell(row.location)}</td>
  <td>${cell(row.iostandard)}</td>
  <td>${cell(row.pulltype)}</td>
  <td>${badge}</td>
</tr>`;
        }).join('');
        return `<section class="file">
  <h3>${esc(basename(file.file))} <span class="muted">(${rows.length} rows${withIssues ? `, ${withIssues} with diagnostics` : ''})</span></h3>
  <p class="muted">${esc(file.file)}</p>
  ${rows.length
        ? `<table><thead><tr><th>Line</th><th>Port</th><th>LOCATION</th><th>IOSTANDARD</th><th>PULLTYPE</th><th>Issues</th></tr></thead><tbody>${body}</tbody></table>`
        : '<p class="muted">No pin assignments.</p>'}
</section>`;
    }).join('\n');

    return `<!DOCTYPE html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
<title>FPGA pin constraints</title>
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 12px; }
  table { border-collapse: collapse; margin: 8px 0 20px; font-size: 12px; }
  th, td { border: 1px solid var(--vscode-panel-border, #444); padding: 3px 8px; text-align: left; }
  th { background: var(--vscode-keybindingTable-headerBackground, transparent); }
  tr.has-issue td { background: color-mix(in srgb, var(--vscode-inputValidation-errorBackground) 35%, transparent); }
  .badge { display: inline-block; min-width: 16px; padding: 0 4px; border-radius: 8px;
           background: var(--vscode-inputValidation-errorBorder, #f14c4c); color: #fff;
           text-align: center; font-size: 11px; }
  .muted { color: var(--vscode-descriptionForeground); }
  a { color: var(--vscode-textLink-foreground); }
  h2 { font-size: 14px; }
</style></head>
<body>
<h2>Pin constraints — ${esc(opts.project || '')}</h2>
<p class="muted">Click a line number to jump to it. Editing stays in the editor (undo-friendly).</p>
${sections}
<script nonce="${nonce}"></script>
</body></html>`;
}

module.exports = { renderConstraintHtml };