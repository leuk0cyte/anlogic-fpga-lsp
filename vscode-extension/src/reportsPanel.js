// Single reusable "FPGA Reports" webview panel.
//
// The panel only knows how to fetch `fpgaLsp/reports` and how to turn webview messages into
// callbacks; the flow controller is injected by extension.js. Depends only on `vscode`, so it
// can be loaded under test/mock-vscode.js.

'use strict';

const crypto = require('crypto');
const vscode = require('vscode');
const { METHODS } = require('./protocol');
const { renderReportsHtml } = require('./reportsHtml');

const VIEW_TYPE = 'fpgaLsp.reports';
const TITLES = { syn: 'Synthesis Reports', phy: 'Place & Route Reports' };

class ReportsPanel {
    /**
     * @param {{rpc: {request: Function}, onRunFlow: (stages: string[]) => any}} options
     */
    constructor(options) {
        const opts = options || {};
        this.rpc = opts.rpc;
        this.onRunFlow = typeof opts.onRunFlow === 'function' ? opts.onRunFlow : () => undefined;
        this.onRunAnalysis = typeof opts.onRunAnalysis === 'function' ? opts.onRunAnalysis : () => undefined;
        this.onCompareStrategies = typeof opts.onCompareStrategies === 'function'
            ? opts.onCompareStrategies : () => undefined;
        this.stage = opts.stage === 'phy' ? 'phy' : 'syn';
        this.panel = undefined;
        this.reports = undefined;
        this._disposables = [];
    }

    get visible() {
        return !!(this.panel && this.panel.visible !== false);
    }

    async show() {
        if (this.panel) {
            this.panel.reveal();
            return this.refresh();
        }
        const panel = vscode.window.createWebviewPanel(
            `${VIEW_TYPE}.${this.stage}`,
            TITLES[this.stage] || 'FPGA Reports',
            vscode.ViewColumn.Active,
            {
                enableScripts: true,
                enableCommandUris: true,
                retainContextWhenHidden: true,
            }
        );
        this.panel = panel;
        panel.webview.html = this._loadingHtml();
        this._disposables.push(panel.webview.onDidReceiveMessage((message) => this._onMessage(message)));
        this._disposables.push(panel.onDidDispose(() => {
            this.panel = undefined;
            this._clearDisposables();
        }));
        return this.refresh();
    }

    /** Re-fetch reports and re-render. No-op when the panel is not open. */
    async refresh() {
        if (!this.panel) return undefined;
        let reports;
        try {
            reports = await this.rpc.request(METHODS.reports, {});
        } catch (err) {
            reports = {
                ok: false,
                error: `fpga-lsp reports request failed: ${err && err.message ? err.message : String(err)}`,
            };
        }
        if (!this.panel) return reports; // closed while awaiting
        this.reports = reports && typeof reports === 'object' ? reports : { ok: false };
        const nonce = crypto.randomBytes(16).toString('hex');
        try {
            this.panel.webview.html = renderReportsHtml({
                reports: this.reports,
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
        return this.reports;
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
            this.refresh();
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
        const panel = this.panel;
        this.panel = undefined;
        this._clearDisposables();
        if (panel) panel.dispose();
    }
}

module.exports = { ReportsPanel, VIEW_TYPE, TITLES };
