// =============================================================================
//  vscode-extension/src/probePanel.js —— ChipWatcher 探针面板（webview）
// =============================================================================
//  只负责面板生命周期与消息路由；数据全部通过注入的 service 拿，
//  这样 test/run.js 可以用 mock 直接驱动（与 reportsPanel.js / flowController.js 同一约定）。
//
//  service 契约：
//    getStatus():  Promise<status>           后端 fpgalsp.probeStatus 的返回
//    applySignals(signals: string[]): Promise<result>   写 user.mdn
//    capture():    Promise<result>           采集一次
// =============================================================================

'use strict';

const { renderProbePanelHtml } = require('./probeHtml');

class ProbePanel {
    constructor(vscodeApi, service) {
        this.vscode = vscodeApi;
        this.service = service || {};
        this.panel = null;
        this.state = null;
    }

    show() {
        if (this.panel) {
            this.panel.reveal();
            return this.refresh();
        }
        this.panel = this.vscode.window.createWebviewPanel(
            // ⚠ 不要写成 `fpgaLsp.xxx` —— test/run.js 的守卫会把 src/*.js 里出现的
            //   `fpgaLsp.*` 一律当成"被引用的命令 id"，要求它在 package.json 里声明。
            //   这里只是 webview 的类型标识，不需要是命令。
            'fpgaLspProbePanel',
            'ChipWatcher probes',
            this.vscode.ViewColumn.One,
            { enableScripts: true, retainContextWhenHidden: true }
        );
        this.panel.onDidDispose(() => { this.panel = null; }, null, []);
        this.panel.webview.onDidReceiveMessage((message) => this.onMessage(message), null, []);
        return this.refresh();
    }

    async refresh() {
        if (!this.panel) return undefined;
        try {
            this.state = await this.service.getStatus();
        } catch (err) {
            this.state = { ok: false, reason: String((err && err.message) || err) };
        }
        this.render();
        return this.state;
    }

    render() {
        if (!this.panel) return;
        this.panel.webview.html = renderProbePanelHtml(this.state || {});
    }

    say(text) {
        if (this.panel) this.panel.webview.postMessage({ type: 'status', text });
    }

    async onMessage(message) {
        const kind = message && message.type;
        if (kind === 'reload') {
            await this.refresh();
            return;
        }
        if (kind === 'apply') {
            this.say('Saving...');
            const result = await this.service.applySignals(message.signals || []);
            if (!result || result.ok !== true) {
                this.say(`Save failed: ${(result && (result.reason || result.error)) || 'unknown'}`);
                return;
            }
            this.say(`Wrote ${result.count} signal(s) (+${(result.added || []).length}/-${(result.removed || []).length}). ` +
                'Run Place & Route so the probes get embedded in the bitstream.');
            await this.refresh();
            return;
        }
        if (kind === 'remove' || kind === 'removeBase') {
            const current = ((this.state && this.state.chip_debugger && this.state.chip_debugger.signals) || []);
            const next = kind === 'remove' ? current.filter((s) => s !== message.signal)
                : current.filter((s) => !String(s).startsWith(`${message.base}[`));
            if (!next.length) {
                this.say('Refusing to empty the list (that would delete the watcher)');
                return;
            }
            const result = await this.service.applySignals(next);
            if (!result || result.ok !== true) {
                this.say(`Remove failed: ${(result && (result.reason || result.error)) || 'unknown'}`);
                return;
            }
            await this.refresh();
            return;
        }
        if (kind === 'capture') {
            this.say('Capturing (trying the bitstream already in the device first)...');
            const result = await this.service.capture();
            if (!result || result.ok !== true) {
                this.say(`Capture failed: ${(result && (result.reason || result.error)) || 'unknown'}`);
                return;
            }
            this.say(`Captured ${result.samples} sample(s) on ${result.clock} - ${result.csv || ''}`);
            return;
        }
    }

    dispose() {
        if (this.panel) {
            this.panel.dispose();
            this.panel = null;
        }
    }
}

module.exports = { ProbePanel };