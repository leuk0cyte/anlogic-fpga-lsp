// =============================================================================
//  vscode-extension/src/probePanel.js —— ChipWatcher 探针面板（**底栏** webview view）
// =============================================================================
//  用 WebviewViewProvider 注册到 panel 容器（底部面板，和 Terminal / Problems 同级），
//  不是浮在编辑区的 webviewPanel —— 那是 0.17.1 之前的做法。
//
//  service 契约：
//    getStatus():  Promise<status>                       后端的 probeStatus
//    applySignals(signals: string[]): Promise<result>     写 user.mdn
//    capture(trigger):  Promise<result>                   带触发设置采集一次
//
//  ⚠ viewId 由构造函数传入（extension.js 给 VIEWS.probe）。
//    不要在 src/*.js 里写 `fpgaLsp.*` 字面量：test/run.js 的守卫会把它当成
//    "被引用的命令 id"，要求它在 package.json 里声明。
// =============================================================================

'use strict';

const { renderProbePanelHtml } = require('./probeHtml');

class ProbePanel {
    constructor(vscodeApi, service, viewId) {
        this.vscode = vscodeApi;
        this.service = service || {};
        this.viewId = viewId || '';
        this.view = null;
        this.state = null;
    }

    // WebviewViewProvider 的实现
    resolveWebviewView(view) {
        this.view = view;
        view.webview.options = { enableScripts: true };
        view.webview.onDidReceiveMessage((message) => this.onMessage(message), null, []);
        view.onDidDispose(() => { this.view = null; }, null, []);
        this.refresh();
    }

    // 命令入口：把底栏那个视图聚焦（视图没被创建过时 registerWebviewViewProvider
    // 会在第一次 focus 时调 resolveWebviewView）
    show() {
        if (!this.view && this.viewId) {
            return this.vscode.commands.executeCommand(`${this.viewId}.focus`);
        }
        return this.refresh();
    }

    async refresh() {
        if (!this.view) return undefined;
        try {
            this.state = await this.service.getStatus();
        } catch (err) {
            this.state = { ok: false, reason: String((err && err.message) || err) };
        }
        this.render();
        return this.state;
    }

    render() {
        if (!this.view) return;
        this.view.webview.html = renderProbePanelHtml(this.state || {});
    }

    say(text) {
        if (this.view) this.view.webview.postMessage({ type: 'status', text });
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
            const trigger = message.trigger || {};
            this.say('Capturing (trying the bitstream already in the device first)...');
            const result = await this.service.capture(trigger);
            if (!result || result.ok !== true) {
                this.say(`Capture failed: ${(result && (result.reason || result.error)) || 'unknown'}`);
                return;
            }
            this.say(`Captured ${result.samples} sample(s) on ${result.clock}, trigger at sample ` +
                `${result.trigger_index} - ${result.csv || ''}`);
            return;
        }
    }

    dispose() {
        this.view = null;
    }
}

module.exports = { ProbePanel };