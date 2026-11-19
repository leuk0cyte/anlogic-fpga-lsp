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
} = require('./reportsHtml');

const VIEW_TYPE = 'fpgaLsp.reports';
const TITLES = { syn: 'Synthesis Reports', phy: 'Place & Route Reports' };
const DEBOUNCE_MS = 150;

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
     * @param {{force?: boolean}} [options]
     */
    async refresh(options) {
        if (!this.panel) return undefined;
        const opts = options || {};
        const seq = ++this._seq;
        this._cancelInflight();
        const source = typeof vscode.CancellationTokenSource === 'function'
            ? new vscode.CancellationTokenSource() : undefined;
        this._cancellation = source;

        let reports;
        try {
            reports = await this.reportsService.fetch({
                section: 'summary',
                force: opts.force === true,
                token: source ? source.token : undefined,
            });
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
        if (seq !== this._seq || !this.panel) return reports; // closed / superseded while awaiting
        this.reports = reports && typeof reports === 'object' ? reports : { ok: false };

        const view = buildReportView(this.reports, this.stage);
        const ids = reportSectionIds(view, this.stage).join(',');
        if (!this._painted || view.ok !== true || ids !== this._paintedIds) {
            // first paint, empty state, or the tab set changed -> one full render
            this._paint(this.reports, view, ids);
        } else {
            this._patch(this.reports, view);
        }
        return this.reports;
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

    _patch(reports, view) {
        const sections = {};
        for (const id of reportSectionIds(view, this.stage)) {
            const html = renderReportSection({ reports, stage: this.stage }, id);
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
        this._clearDisposables();
        if (panel) panel.dispose();
    }
}

module.exports = { ReportsPanel, VIEW_TYPE, TITLES, DEBOUNCE_MS };