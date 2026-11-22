// Single reusable "FPGA Reports" webview panel.
//
// Phase 2 (A4) changes:
//   * one **shared** `ReportsService` per session (injected) so the flow tree and this panel never
//     send two `fpgaLsp/reports` requests for the same data;
//   * first paint = full page, later refreshes = per-section DOM patch (`postMessage`), so the
//     webview keeps its scroll position, search text and active tab;
//   * refreshes are debounced (150 ms) and the previous request is cancelled through a
//     `CancellationTokenSource`; a stale response can never repaint the panel.
//
// D1 (wave 2, audit/2026-11-19/perf/VSCODE.md appendix D1):
//   The panel used to request only `{section:'summary'}` and render the stage tabs from that
//   payload. On a server that honours `section` (ours: `summary` = files+artifacts only) that
//   left QoR / Timing / Area **permanently empty**. Now:
//     1. `summary` paints the skeleton (project, stage, flow, file lists) at once;
//     2. `qor` + `area` are fetched right after it (small; they feed the default Dashboard tab);
//     3. `timing` is fetched only when its tab is actually opened (it can be megabytes of paths),
//        driven by the `{type:'tab', id}` message the webview now posts.
//   Servers that ignore `section` (older builds) keep working: `ReportsService` marks them
//   `unsupported`, serves the full payload it already has, and no extra request is made.
//
// The panel only knows how to fetch reports and how to turn webview messages into callbacks;
// the flow controller is injected by extension.js. Depends only on `vscode`, so it can be loaded
// under test/mock-vscode.js.

'use strict';

const crypto = require('crypto');
const vscode = require('vscode');
const { ReportsService } = require('./reportsService');
const {
    renderReportsHtml,
    renderReportSection,
    buildReportView,
    reportSectionIds,
    reportMeta,
    areaTab,
} = require('./reportsHtml');

const VIEW_TYPE = 'fpgaLsp.reports';
const TITLES = { syn: 'Synthesis Reports', phy: 'Place & Route Reports' };
const DEBOUNCE_MS = 150;

// D1: report sections the panel can pull on demand. `summary` is the skeleton; the other three
// are the tables the server only includes when asked for them.
const SECTION_NAMES = ['qor', 'timing', 'area'];
// Sections needed by the default (Dashboard) tab and cheap enough to load with the first paint.
const EAGER_SECTIONS = ['qor', 'area'];
// Webview tab id -> report section. Tab ids are lower-cased labels (see reportsHtml.tabId).
const SECTION_BY_TAB = { qor: 'qor', timing: 'timing', area: 'area' };

class ReportsPanel {
    /**
     * @param {{
     *   rpc: {request: Function},
     *   reportsService?: ReportsService,
     *   onRunFlow: (stages: string[]) => any,
     *   onRunAnalysis?: () => any,
     *   onCompareStrategies?: () => any,
     * }} options
     */
    constructor(options) {
        const opts = options || {};
        this.rpc = opts.rpc;
        this.reportsService = opts.reportsService || new ReportsService(opts.rpc);
        this.onRunFlow = typeof opts.onRunFlow === 'function' ? opts.onRunFlow : () => undefined;
        this.onRunAnalysis = typeof opts.onRunAnalysis === 'function' ? opts.onRunAnalysis : () => undefined;
        this.onCompareStrategies = typeof opts.onCompareStrategies === 'function'
            ? opts.onCompareStrategies : () => undefined;
        this.stage = opts.stage === 'phy' ? 'phy' : 'syn';
        this.panel = undefined;
        this.reports = undefined;
        // D1: payloads of the on-demand sections (qor / timing / area), keyed by section name.
        this._sectionPayloads = Object.create(null);
        this._disposables = [];
        this._painted = false;
        this._paintedIds = '';
        this._debounce = undefined;
        this._cancellation = undefined;
        this._seq = 0;
        /** @type {{html:number, patch:number, cancelled:number}} */
        this.stats = { html: 0, patch: 0, cancelled: 0 };
    }

    get visible() {
        return !!(this.panel && this.panel.visible !== false);
    }

    async show() {
        if (this.panel) {
            this.panel.reveal();
            return this.refresh({ force: true });
        }
        const panel = vscode.window.createWebviewPanel(
            `${VIEW_TYPE}.${this.stage}`,
            TITLES[this.stage] || 'FPGA Reports',
            vscode.ViewColumn.Active,
            {
                enableScripts: true,
                enableCommandUris: true,
                // Keep the webview alive (and its DOM) when the tab is hidden: the next reveal
                // then costs a section patch instead of a full rebuild.
                retainContextWhenHidden: true,
            }
        );
        this.panel = panel;
        this._painted = false;
        this._paintedIds = '';
        panel.webview.html = this._loadingHtml();
        this._disposables.push(panel.webview.onDidReceiveMessage((message) => this._onMessage(message)));
        this._disposables.push(panel.onDidDispose(() => {
            this.panel = undefined;
            this._painted = false;
            this._sectionPayloads = Object.create(null);  // D1: release the fetched tables
            this._clearDisposables();
        }));
        return this.refresh({ force: true });
    }

    /** Debounced refresh: coalesces flow events / button mashing into one request. */
    requestRefresh(options) {
        if (this._debounce) clearTimeout(this._debounce);
        this._debounce = setTimeout(() => {
            this._debounce = undefined;
            this.refresh(options).catch(() => undefined);
        }, DEBOUNCE_MS);
        if (this._debounce && typeof this._debounce.unref === 'function') this._debounce.unref();
    }

    /**
     * Re-fetch reports and update the webview. No-op when the panel is not open.
     *
     * D1: paints the `summary` skeleton first, then loads the table sections the visible tabs
     * need (qor + area eagerly, timing only once its tab is opened) and patches them in.
     *
     * P2-C: `options.summary` lets the caller pass a payload it already fetched (the
     * `reportsChanged` handler does exactly that), so a burst refresh costs **one**
     * `fpgaLsp/reports` request instead of two.
     * @param {{force?: boolean, summary?: object}} [options]
     */
    async refresh(options) {
        if (!this.panel) return undefined;
        const opts = options || {};
        const force = opts.force === true;
        const seq = ++this._seq;
        this._cancelInflight();
        const source = typeof vscode.CancellationTokenSource === 'function'
            ? new vscode.CancellationTokenSource() : undefined;
        this._cancellation = source;
        const token = source ? source.token : undefined;

        let reports;
        if (opts.summary && typeof opts.summary === 'object') {
            reports = opts.summary;
        } else {
            try {
                reports = await this.reportsService.fetch({ section: 'summary', force, token });
            } catch (err) {
                if (err && err.cancelled) {
                    this.stats.cancelled += 1;
                    return this.reports; // superseded by a newer refresh
                }
                reports = {
                    ok: false,
                    error: `fpga-lsp reports request failed: ${err && err.message ? err.message : String(err)}`,
                };
            }
        }
        if (seq !== this._seq || !this.panel) return reports; // closed / superseded while awaiting
        this.reports = reports && typeof reports === 'object' ? reports : { ok: false };

        // 1) skeleton: everything `summary` carries (project, stage, flow, file lists).
        const skeletonView = buildReportView(this.reports, this.stage);
        const skeletonIds = reportSectionIds(skeletonView, this.stage).join(',');
        if (!this._painted || skeletonView.ok !== true || skeletonIds !== this._paintedIds) {
            this._paint(this.reports, skeletonView, skeletonIds);
        }

        // 2) table sections, then patch them in. A server that ignores `section` answers from
        //    the full payload it already returned (ReportsService marks it `unsupported`), so
        //    this costs no extra round trip there.
        const wanted = this._wantedSections();
        if (wanted.length) {
            // A server that ignores `section` (older builds) answers section requests from the
            // full payload the summary fetch just brought in, so forcing a section re-fetch there
            // would only repeat the same round trip. Only force on servers that honour sections.
            const forceSections = force && this.reportsService.sectionSupport === 'supported';
            await this._ensureSections(wanted, { force: forceSections, token });
            if (seq !== this._seq || !this.panel) return this.reports; // superseded while loading
        }
        this.reports = this._mergeSections();
        const view = buildReportView(this.reports, this.stage);
        const ids = reportSectionIds(view, this.stage);
        if (!this._painted || view.ok !== true || ids.join(',') !== this._paintedIds) {
            this._paint(this.reports, view, ids.join(','));
        } else {
            this._patch(this.reports, view, ids);
        }
        return this.reports;
    }

    /** D1: sections to load on a normal refresh — eager ones plus anything already opened. */
    _wantedSections() {
        const wanted = new Set(EAGER_SECTIONS);
        for (const name of SECTION_NAMES) {
            if (this._sectionPayloads[name]) wanted.add(name);
        }
        return [...wanted];
    }

    /** D1: fetch one report section (served from `ReportsService`'s TTL cache / single flight). */
    async _ensureSections(names, options) {
        const opts = options || {};
        const jobs = [];
        for (const name of names) {
            if (!opts.force && this._sectionPayloads[name]) continue;
            jobs.push(this.reportsService.fetch({ section: name, force: opts.force === true, token: opts.token })
                .then((payload) => {
                    if (payload && typeof payload === 'object') this._sectionPayloads[name] = payload;
                })
                .catch(() => undefined));  // a failed section keeps its empty state
        }
        await Promise.allSettled(jobs);
    }

    /** D1: fold the loaded sections into the summary payload (top level + per-stage copies). */
    _mergeSections() {
        const merged = Object.assign({}, this.reports || { ok: false });
        for (const name of SECTION_NAMES) {
            const payload = this._sectionPayloads[name];
            if (!payload || typeof payload !== 'object' || payload.ok === false) continue;
            if (payload[name] !== undefined) merged[name] = payload[name];
            for (const stage of ['syn', 'phy']) {
                const stageData = payload[stage];
                if (!stageData || typeof stageData !== 'object') continue;
                if (stageData[name] === undefined) continue;
                const current = merged[stage] && typeof merged[stage] === 'object' ? merged[stage] : {};
                merged[stage] = Object.assign({}, current, { [name]: stageData[name] });
            }
        }
        return merged;
    }

    /** D1: load one section because its tab became visible, then patch just that section. */
    async ensureSection(section) {
        if (!this.panel || !SECTION_NAMES.includes(section)) return;
        if (this._sectionPayloads[section]) return;  // already painted
        await this._ensureSections([section], { force: false });
        if (!this.panel) return;
        this.reports = this._mergeSections();
        const view = buildReportView(this.reports, this.stage);
        if (view.ok !== true) return;
        // The dashboard cards also come from these sections, so patch it together with the tab.
        this._patch(this.reports, view, [section, 'dashboard']);
    }

    _paint(reports, view, ids) {
        const nonce = crypto.randomBytes(16).toString('hex');
        try {
            this.panel.webview.html = renderReportsHtml({
                reports,
                nonce,
                stage: this.stage,
                cspSource: this.panel.webview.cspSource,
            });
        } catch (err) {
            this.panel.webview.html = renderReportsHtml({
                reports: { ok: false, error: `failed to render reports: ${err && err.message ? err.message : String(err)}` },
                nonce,
                stage: this.stage,
            });
        }
        this._painted = true;
        this._paintedIds = ids;
        this.stats.html += 1;
    }

    _patch(reports, view, ids) {
        const list = Array.isArray(ids) && ids.length ? ids : reportSectionIds(view, this.stage);
        const sections = {};
        // D1: compute the (expensive) area table once and let every section reuse it.
        const area = areaTab(view.area);
        for (const id of list) {
            const html = renderReportSection({ reports, stage: this.stage }, id, area);
            if (html) sections[id] = html;
        }
        const message = { type: 'patch', sections, meta: reportMeta(reports, this.stage) };
        try {
            this.panel.webview.postMessage(message);
        } catch (_) {
            // a disposed webview throws on postMessage; fall back to a full render next time
            this._painted = false;
            return;
        }
        this.stats.patch += 1;
    }

    _cancelInflight() {
        const source = this._cancellation;
        this._cancellation = undefined;
        if (source && typeof source.cancel === 'function') {
            try { source.cancel(); } catch (_) { /* already cancelled */ }
        }
        if (source && typeof source.dispose === 'function') {
            try { source.dispose(); } catch (_) { /* ignore */ }
        }
    }

    _loadingHtml() {
        const nonce = crypto.randomBytes(16).toString('hex');
        return `<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'nonce-${nonce}';">
<title>FPGA Reports</title>
<style nonce="${nonce}">body { font-family: var(--vscode-font-family, sans-serif); color: var(--vscode-foreground); background: var(--vscode-editor-background); padding: 24px; }</style>
</head><body>Loading FPGA reports…</body></html>`;
    }

    _onMessage(message) {
        if (!message || typeof message !== 'object') return;
        if (message.type === 'refresh') {
            this.requestRefresh({ force: true });
        } else if (message.type === 'tab' && typeof message.id === 'string') {
            // D1: a tab became visible -> make sure its report section is loaded (lazy).
            const section = SECTION_BY_TAB[message.id];
            if (section) this.ensureSection(section).catch(() => undefined);
        } else if (message.type === 'run' && Array.isArray(message.stages) && message.stages.length) {
            this.onRunFlow(message.stages);
        } else if (message.type === 'analysis') {
            this.onRunAnalysis();
        } else if (message.type === 'compare') {
            this.onCompareStrategies();
        } else if (message.type === 'openFile' && message.path) {
            vscode.commands.executeCommand('vscode.open', vscode.Uri.file(String(message.path)));
        }
    }

    _clearDisposables() {
        for (const disposable of this._disposables) {
            try {
                if (disposable && typeof disposable.dispose === 'function') disposable.dispose();
            } catch (_) {
                // ignore: a broken disposable must not prevent the panel from being reused
            }
        }
        this._disposables = [];
    }

    dispose() {
        if (this._debounce) {
            clearTimeout(this._debounce);
            this._debounce = undefined;
        }
        this._cancelInflight();
        const panel = this.panel;
        this.panel = undefined;
        this._painted = false;
        this._sectionPayloads = Object.create(null);  // D1: release the fetched tables
        this._clearDisposables();
        if (panel) panel.dispose();
    }
}

module.exports = { ReportsPanel, VIEW_TYPE, TITLES, DEBOUNCE_MS };