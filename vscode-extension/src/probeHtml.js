// =============================================================================
//  vscode-extension/src/probeHtml.js —— 片上探针（ChipWatcher）面板的静态 HTML
// =============================================================================
//  只做渲染，不含任何 vscode 依赖（与 reportsHtml.js / trendHtml.js 同一约定），
//  因此可以在 test/run.js 的 mock-vscode 下直接单测。
//
//  数据形状（来自后端 fpgalsp.probeStatus / probeConfig）：
//    {
//      ok, ready, reason, project, target, matches_user_mdn,
//      chip_debugger: { path, exists, count, signals: [ "WHG/xxx[0]", ... ] },
//      watcher: { present, instance, clock, sample_depth, trigger_position,
//                 trigger_type, stale, missing_in_bit: [base...], extra_in_bit: [base...],
//                 compiled_cwc, compiled_bit, buses: [{ name, clock, type, enabled }] }
//    }
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

// `WHG/pwm_reg0_i[12]` -> { base: 'WHG/pwm_reg0_i', bit: 12 }
function splitSignal(signal) {
    const text = String(signal || '');
    const match = /^(.*)\[(\d+)\]$/.exec(text);
    if (!match) return { base: text, bit: null };
    return { base: match[1], bit: Number(match[2]) };
}

// 把逐位清单按 base 分组，位号升序；组内保持紧凑显示（像 0-15 就折叠成范围）
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
        return '<p class="empty">还没有编译产物（<code>phy_1/cw/compiled.cwc</code> 不存在）。' +
            '先点「Run Place &amp; Route」把探针埋进 bit。</p>';
    }
    const rows = buses.map((bus) => {
        // compiled.cwc 里的名字是 `pwm_reg0_i[15:0]`，stale 列表里是去掉顶层的 base
        const base = String(bus.name || '').replace(/\[[^\]]*\]$/, '');
        const flag = missing.has(base) ? '<span class="warn" title="清单里有、bit 里没有">缺失</span>' : '';
        return `<tr><td><code>${escapeHtml(bus.name)}</code></td>` +
            `<td>${escapeHtml(bus.clock || '')}</td>` +
            `<td>${escapeHtml(bus.type || '')}</td>` +
            `<td>${bus.enabled === false ? 'no' : 'yes'} ${flag}</td></tr>`;
    }).join('');
    return `<table><thead><tr><th>总线</th><th>采样时钟</th><th>类型</th><th>启用</th></tr></thead>` +
        `<tbody>${rows}</tbody></table>`;
}

function renderSignalList(signals) {
    const groups = groupSignals(signals);
    if (!groups.length) {
        return '<p class="empty">清单是空的 —— 点下面「添加信号」，或直接在文本框里粘贴层次名。</p>';
    }
    return groups.map((group) => {
        const chips = group.bits.map((bit) => {
            const label = bit < 0 ? escapeHtml(group.base) : `[${bit}]`;
            const value = bit < 0 ? escapeHtml(group.base) : `${escapeHtml(group.base)}[${bit}]`;
            return `<button class="chip" data-remove="${value}" title="移除 ${value}">${label}&times;</button>`;
        }).join('');
        return `<div class="group"><div class="group-head"><code>${escapeHtml(group.base)}</code>` +
            `<span class="count">${group.count} 位</span>` +
            `<button class="link" data-remove-base="${escapeHtml(group.base)}">整组移除</button></div>` +
            `<div class="chips">${chips}</div></div>`;
    }).join('');
}

function renderProbePanelHtml(state) {
    const data = state || {};
    const watcher = data.watcher || {};
    const mdn = data.chip_debugger || {};
    const ready = data.ready === true;
    const stale = watcher.stale === true;
    const badge = ready
        ? '<span class="badge ok">就绪</span>'
        : stale ? '<span class="badge warn">需要重跑</span>' : '<span class="badge bad">未就绪</span>';

    const info = [
        ['工程 / 器件', `${data.project || '?'} / ${data.target || '?'}`],
        ['探针信号', `${mdn.count == null ? 0 : mdn.count} 个（${mdn.path || 'chip_debugger/user.mdn'}）`],
        ['Watcher 实例', watcher.instance || '(none)'],
        ['采样时钟', watcher.clock || '(none)'],
        ['采样深度', String(watcher.sample_depth || 0)],
        ['触发', `${watcher.trigger_type || '(none)'} @ ${watcher.trigger_position == null ? '?' : watcher.trigger_position}`],
        ['编译产物', watcher.compiled_bit || '(none)'],
    ].map(([key, value]) => `<tr><th>${escapeHtml(key)}</th><td>${escapeHtml(value)}</td></tr>`).join('');

    const staleNote = stale
        ? `<p class="warn-box">清单与 bit 里嵌的探针不一致：
             ${(watcher.missing_in_bit || []).length ? 'bit 里缺 ' + escapeHtml(watcher.missing_in_bit.join(', ')) + '；' : ''}
             ${(watcher.extra_in_bit || []).length ? 'bit 里多 ' + escapeHtml(watcher.extra_in_bit.join(', ')) : ''}
             采集会被拒绝。请重跑 <b>Place &amp; Route</b>（增量布线不会重新埋探针）。</p>`
        : '';

    return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<title>ChipWatcher 探针</title>
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 12px 16px; }
  h2 { font-size: 1.1em; margin: 0 0 8px; display: flex; gap: 8px; align-items: center; }
  h3 { font-size: 1em; margin: 18px 0 6px; }
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
  textarea { width: 100%; min-height: 90px; font-family: var(--vscode-editor-font-family); font-size: .9em;
             background: var(--vscode-input-background); color: var(--vscode-input-foreground);
             border: 1px solid var(--vscode-input-border, #8886); padding: 6px; }
  .row { display: flex; gap: 8px; align-items: center; margin: 8px 0; flex-wrap: wrap; }
  button.primary { padding: 4px 12px; background: var(--vscode-button-background);
                   color: var(--vscode-button-foreground); border: none; cursor: pointer; }
  button.primary:disabled { opacity: .5; cursor: default; }
  .warn-box { border-left: 3px solid #c88800; padding: 6px 10px; background: #c888001a; }
  .hint { opacity: .7; font-size: .9em; }
</style>
</head>
<body>
<h2>ChipWatcher 片上探针 ${badge}</h2>
<p class="hint">${escapeHtml(data.reason || '')}</p>
${staleNote}

<h3>当前配置</h3>
<table>${info}</table>

<h3>bit 里已经嵌进去的探针</h3>
${renderBusTable(watcher.buses, watcher.missing_in_bit)}

<h3>探针清单（<code>chip_debugger/user.mdn</code>）</h3>
<div id="signals">${renderSignalList(mdn.signals)}</div>

<div class="row">
  <textarea id="editor" placeholder="每行或空格分隔的层次名，例如 WG/pwm_reg0_i[0]&#10;也可以粘贴整段 mark_debug { ... }"></textarea>
</div>
<div class="row">
  <button class="primary" id="apply">保存清单</button>
  <button id="reload">重新读取</button>
  <button id="capture">采集一次</button>
  <span class="hint" id="status"></span>
</div>

<h3>采样时钟 / 深度</h3>
<p class="hint">
  这两个参数固化在 bit 里，改了必须重跑 <b>Place &amp; Route</b>。<br>
  实测：<code>insert_debugger &lt;dir&gt; -cwc &lt;file&gt;</code> 能让 TD 接受手写的
  <code>.cwc</code>（改了 <code>sample_depth</code> 立刻体现在
  <code>report_cwc_status</code>）；但 TD 生成的流程脚本调用的是不带 <code>-cwc</code> 的
  <code>insert_debugger</code>，会把配置冲掉（实测——第二次调用后 watcher 列表变空）。
  所以这里先只显示，不写盘。
</p>
<table>
  <tr><th>采样时钟</th><td>${escapeHtml(watcher.clock || '(none)')}</td></tr>
  <tr><th>采样深度</th><td>${escapeHtml(String(watcher.sample_depth || 0))}</td></tr>
  <tr><th>触发位置</th><td>${escapeHtml(String(watcher.trigger_position == null ? '' : watcher.trigger_position))}</td></tr>
</table>

<script>
  const vscode = acquireVsCodeApi();
  const status = document.getElementById('status');
  function say(text) { status.textContent = text || ''; }

  document.getElementById('apply').addEventListener('click', () => {
      const raw = document.getElementById('editor').value || '';
      const inner = /mark_debug\\s*\\{([^}]*)\\}/.exec(raw);
      const text = inner ? inner[1] : raw;
      const signals = text.split(/\\s+/).map((s) => s.trim()).filter(Boolean);
      if (!signals.length) { say('至少要一个信号'); return; }
      say('保存中…');
      vscode.postMessage({ type: 'apply', signals });
  });
  document.getElementById('reload').addEventListener('click', () => vscode.postMessage({ type: 'reload' }));
  document.getElementById('capture').addEventListener('click', () => vscode.postMessage({ type: 'capture' }));

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
      if (message.type === 'error') say('错误：' + (message.text || ''));
  });
</script>
</body>
</html>`;
}

module.exports = { renderProbePanelHtml, groupSignals, splitSignal, escapeHtml };