// =============================================================================
//  vscode-extension/src/probeHtml.js —— ChipWatcher probe panel (static HTML)
// =============================================================================
//  Rendering only, no vscode imports (same convention as reportsHtml.js) so
//  test/run.js can drive it with the mock. UI language is English on purpose.
// =============================================================================

'use strict';

function escapeHtml(text) {
    return String(text == null ? '' : text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function splitSignal(signal) {
    const text = String(signal || '');
    const match = /^(.*)\[(\d+)\]$/.exec(text);
    if (!match) return { base: text, bit: null };
    return { base: match[1], bit: Number(match[2]) };
}

function groupSignals(signals) {
    const groups = new Map();
    for (const signal of signals || []) {
        const { base, bit } = splitSignal(signal);
        if (!groups.has(base)) groups.set(base, []);
        groups.get(base).push(bit == null ? -1 : bit);
    }
    const out = [];
    for (const [base, bits] of groups) {
        bits.sort((a, b) => a - b);
        out.push({ base, bits, count: bits.length });
    }
    out.sort((a, b) => (a.base < b.base ? -1 : a.base > b.base ? 1 : 0));
    return out;
}

function renderBusTable(buses, staleMissing) {
    const missing = new Set(staleMissing || []);
    if (!buses || !buses.length) {
        return '<p class="empty">No compiled watcher yet (<code>phy_1/cw/compiled.cwc</code> is missing). ' +
            'Run <b>FPGA: Run Place &amp; Route</b> to embed the probes first.</p>';
    }
    const rows = buses.map((bus) => {
        const base = String(bus.name || '').replace(/\[[^\]]*\]$/, '');
        const flag = missing.has(base)
            ? '<span class="warn" title="In user.mdn but not embedded in the bitstream">missing</span>'
            : '';
        return `<tr><td><code>${escapeHtml(bus.name)}</code></td>` +
            `<td>${escapeHtml(bus.clock || '')}</td>` +
            `<td>${escapeHtml(bus.type || '')}</td>` +
            `<td>${bus.enabled === false ? 'no' : 'yes'} ${flag}</td></tr>`;
    }).join('');
    return '<table><thead><tr><th>Bus</th><th>Sample clock</th><th>Type</th><th>Enabled</th></tr></thead>' +
        `<tbody>${rows}</tbody></table>`;
}

function renderSignalList(signals) {
    const groups = groupSignals(signals);
    if (!groups.length) {
        return '<p class="empty">The probe list is empty. Paste net names below, then Save.</p>';
    }
    return groups.map((group) => {
        const chips = group.bits.map((bit) => {
            const label = bit < 0 ? escapeHtml(group.base) : `[${bit}]`;
            const value = bit < 0 ? escapeHtml(group.base) : `${escapeHtml(group.base)}[${bit}]`;
            return `<button class="chip" data-remove="${value}" title="Remove ${value}">${label}&times;</button>`;
        }).join('');
        return `<div class="group"><div class="group-head"><code>${escapeHtml(group.base)}</code>` +
            `<span class="count">${group.count} bit(s)</span>` +
            `<button class="link" data-remove-base="${escapeHtml(group.base)}">remove group</button></div>` +
            `<div class="chips">${chips}</div></div>`;
    }).join('');
}

function renderTriggerRows(buses) {
    const list = buses || [];
    if (!list.length) return '<tr><td colspan="3" class="empty">No buses embedded yet.</td></tr>';
    return list.map((bus) => {
        const name = String(bus.name || '');
        const base = name.replace(/\[[^\]]*\]$/, '');
        const options = ['dont_care', 'equal', 'not_equal', 'less', 'less_equal', 'greater',
            'greater_equal'].map((op) => `<option value="${op}">${op}</option>`).join('');
        return `<tr><td><code>${escapeHtml(name)}</code></td>` +
            `<td><select data-bus-op="${escapeHtml(base)}"><option value="">(keep)</option>${options}</select></td>` +
            `<td><input data-bus-value="${escapeHtml(base)}" placeholder="0x00FF" style="width:8em"></td></tr>`;
    }).join('');
}

function renderProbePanelHtml(state) {
    const data = state || {};
    const watcher = data.watcher || {};
    const mdn = data.chip_debugger || {};
    const depth = watcher.sample_depth || 0;
    const ready = data.ready === true;
    const stale = watcher.stale === true;
    const badge = ready
        ? '<span class="badge ok">ready</span>'
        : stale ? '<span class="badge warn">needs rebuild</span>' : '<span class="badge bad">not ready</span>';

    const info = [
        ['Project / device', `${data.project || '?'} / ${data.target || '?'}`],
        ['Probe signals', `${mdn.count == null ? 0 : mdn.count} (${mdn.path || 'chip_debugger/user.mdn'})`],
        ['Watcher instance', watcher.instance || '(none)'],
        ['Sample clock', watcher.clock || '(none)'],
        ['Sample depth', String(depth)],
        ['Trigger', `${watcher.trigger_type || '(none)'} @ ${watcher.trigger_position == null ? '?' : watcher.trigger_position}`],
        ['Compiled bit', watcher.compiled_bit || '(none)'],
    ].map(([key, value]) => `<tr><th>${escapeHtml(key)}</th><td>${escapeHtml(value)}</td></tr>`).join('');

    const staleNote = stale
        ? `<p class="warn-box">The probe list no longer matches what is embedded in the bitstream:
             ${(watcher.missing_in_bit || []).length ? 'missing from the bit: ' + escapeHtml(watcher.missing_in_bit.join(', ')) + '; ' : ''}
             ${(watcher.extra_in_bit || []).length ? 'left over in the bit: ' + escapeHtml(watcher.extra_in_bit.join(', ')) : ''}
             Capture is refused until you rebuild. Run <b>Place &amp; Route</b> (the incremental route
             does not re-insert the debugger).</p>`
        : '';

    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<title>ChipWatcher probes</title>
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 10px 14px; font-size: 12px; }
  h2 { font-size: 1.15em; margin: 0 0 6px; display: flex; gap: 8px; align-items: center; }
  h3 { font-size: 1em; margin: 16px 0 6px; }
  h4 { margin: 12px 0 4px; font-size: .95em; }
  table { border-collapse: collapse; width: 100%; margin: 6px 0; }
  th, td { text-align: left; padding: 3px 8px; border-bottom: 1px solid var(--vscode-panel-border, #8884); vertical-align: top; }
  th { font-weight: 600; opacity: .8; white-space: nowrap; }
  code { font-family: var(--vscode-editor-font-family); }
  .badge { font-size: .85em; padding: 1px 8px; border-radius: 10px; border: 1px solid; }
  .badge.ok { color: #26a269; border-color: #26a269; }
  .badge.warn { color: #c88800; border-color: #c88800; }
  .badge.bad { color: #c01c28; border-color: #c01c28; }
  .group { margin: 6px 0 10px; }
  .group-head { display: flex; gap: 10px; align-items: baseline; }
  .count { opacity: .65; font-size: .85em; }
  .chips { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 4px; }
  .chip { font-family: var(--vscode-editor-font-family); font-size: .9em; padding: 1px 6px;
          cursor: pointer; border-radius: 4px; border: 1px solid var(--vscode-button-border, #8886);
          background: var(--vscode-button-secondaryBackground, #8882); color: inherit; }
  .chip:hover { background: var(--vscode-button-secondaryHoverBackground, #8884); }
  .link { background: none; border: none; color: var(--vscode-textLink-foreground); cursor: pointer; padding: 0; }
  .empty { opacity: .7; }
  textarea { width: 100%; min-height: 64px; font-family: var(--vscode-editor-font-family); font-size: .9em;
             background: var(--vscode-input-background); color: var(--vscode-input-foreground);
             border: 1px solid var(--vscode-input-border, #8886); padding: 6px; }
  input, select { background: var(--vscode-input-background); color: var(--vscode-input-foreground);
                  border: 1px solid var(--vscode-input-border, #8886); padding: 2px 4px; }
  .row { display: flex; gap: 8px; align-items: center; margin: 8px 0; flex-wrap: wrap; }
  button.primary { padding: 4px 12px; background: var(--vscode-button-background);
                   color: var(--vscode-button-foreground); border: none; cursor: pointer; }
  .warn-box { border-left: 3px solid #c88800; padding: 6px 10px; background: #c888001a; }
  .hint { opacity: .7; font-size: .9em; }
</style>
</head>
<body>
<h2>ChipWatcher probes ${badge}</h2>
<p class="hint">${escapeHtml(data.reason || '')}</p>
${staleNote}

<h3>Current setup</h3>
<table>${info}</table>

<h3>Probes embedded in the bitstream</h3>
${renderBusTable(watcher.buses, watcher.missing_in_bit)}

<h3>Probe list (<code>chip_debugger/user.mdn</code>)</h3>
<div id="signals">${renderSignalList(mdn.signals)}</div>
<div class="row">
  <textarea id="editor" placeholder="Whitespace separated hierarchical net names. Ranges are expanded: WHG/pwm_reg7_i[15:0] becomes 16 nets."></textarea>
</div>
<div class="row">
  <button class="primary" id="apply">Save list</button>
  <button id="reload">Reload</button>
</div>

<h3>Trigger (applied at capture time, no rebuild)</h3>
<p class="hint">
  Position / window / condition are pushed to the chip when the capture starts, so they can be
  changed without re-running the flow. Measured on hardware: position 127 &rarr; 512 moves the
  <code>Trigger</code> marker to sample 512, and a bus set to <code>not_equal</code> with data 0
  does not fire at all while <code>-trigger_now</code> still completes.
</p>
<table>
  <tr><th>Trigger position</th><td><input id="trg-position" type="number" min="0" max="${escapeHtml(String(depth ? depth - 1 : 65535))}" value="${escapeHtml(String(watcher.trigger_position == null ? 0 : watcher.trigger_position))}" style="width:8em"></td>
      <td class="hint">0 .. depth-1 (depth ${escapeHtml(String(depth))})</td></tr>
  <tr><th>Windows x depth</th><td><input id="trg-winnum" type="number" min="1" value="1" style="width:5em"> x
      <input id="trg-windepth" type="number" min="1" value="${escapeHtml(String(depth || 1024))}" style="width:7em"></td>
      <td class="hint">window count x samples per window</td></tr>
  <tr><th>Combinator</th><td><select id="trg-type">
      <option value="">(keep)</option><option value="basic_and">Basic AND</option><option value="basic_or">Basic OR</option>
      </select></td><td class="hint">${escapeHtml(watcher.trigger_type || '')}</td></tr>
  <tr><th>Condition</th><td><select id="trg-enabled">
      <option value="">(keep)</option><option value="yes">enabled</option><option value="no">disabled</option>
      </select></td><td class="hint">disabled = fire on the first sample</td></tr>
</table>

<h4>Per-bus trigger value</h4>
<p class="hint">
  Operator + expected value; the backend expands the value into one <code>cond</code> per bit.
  Leave the operator at <i>keep</i> to leave a bus untouched.
</p>
<table id="bus-conds">
<thead><tr><th>Bus</th><th>Operator</th><th>Value (hex)</th></tr></thead>
<tbody>${renderTriggerRows(watcher.buses)}</tbody>
</table>

<div class="row">
  <button class="primary" id="capture">Capture with this trigger</button>
  <span class="hint" id="status"></span>
</div>

<h3>Sample clock / depth</h3>
<p class="hint">
  Both are baked into the bitstream: changing them requires a full <b>Place &amp; Route</b>,
  so they are read-only here.
</p>
<table>
  <tr><th>Sample clock</th><td>${escapeHtml(watcher.clock || '(none)')}</td></tr>
  <tr><th>Sample depth</th><td>${escapeHtml(String(depth))}</td></tr>
</table>

<script>
  const vscode = acquireVsCodeApi();
  const status = document.getElementById('status');
  function say(text) { status.textContent = text || ''; }

  function collectTrigger() {
      const trigger = {};
      const position = document.getElementById('trg-position');
      if (position && position.value !== '') trigger.position = Number(position.value);
      const winnum = document.getElementById('trg-winnum');
      if (winnum && winnum.value !== '' && Number(winnum.value) > 1) trigger.windowNum = Number(winnum.value);
      const windepth = document.getElementById('trg-windepth');
      if (windepth && windepth.value !== '') trigger.windowDepth = Number(windepth.value);
      const type = document.getElementById('trg-type');
      if (type && type.value) trigger.type = type.value;
      const enabled = document.getElementById('trg-enabled');
      if (enabled && enabled.value) trigger.enabled = (enabled.value === 'yes');
      const buses = [];
      document.querySelectorAll('[data-bus-op]').forEach((select) => {
          const base = select.getAttribute('data-bus-op');
          const input = document.querySelector('[data-bus-value="' + base + '"]');
          const raw = input && input.value ? input.value.trim() : '';
          if (!select.value && !raw) return;
          const entry = { name: base };
          if (select.value) entry.op = select.value;
          if (raw) {
              const value = Number(raw);
              if (!Number.isNaN(value)) entry.value = value;
          }
          buses.push(entry);
      });
      if (buses.length) trigger.buses = buses;
      return trigger;
  }

  document.getElementById('apply').addEventListener('click', () => {
      const raw = document.getElementById('editor').value || '';
      const inner = /mark_debug\\s*\\{([^}]*)\\}/.exec(raw);
      const text = inner ? inner[1] : raw;
      const signals = text.split(/\\s+/).map((s) => s.trim()).filter(Boolean);
      if (!signals.length) { say('At least one signal is required'); return; }
      say('Saving...');
      vscode.postMessage({ type: 'apply', signals });
  });
  document.getElementById('reload').addEventListener('click', () => vscode.postMessage({ type: 'reload' }));
  document.getElementById('capture').addEventListener('click', () => {
      vscode.postMessage({ type: 'capture', trigger: collectTrigger() });
  });

  document.body.addEventListener('click', (event) => {
      const target = event.target;
      if (!target || !target.dataset) return;
      if (target.dataset.remove) {
          vscode.postMessage({ type: 'remove', signal: target.dataset.remove });
      } else if (target.dataset.removeBase) {
          vscode.postMessage({ type: 'removeBase', base: target.dataset.removeBase });
      }
  });

  window.addEventListener('message', (event) => {
      const message = event.data || {};
      if (message.type === 'status') say(message.text || '');
      if (message.type === 'error') say('Error: ' + (message.text || ''));
  });
</script>
</body>
</html>`;
}

module.exports = { renderProbePanelHtml, groupSignals, splitSignal, escapeHtml };