// Runs TangDynasty flows through `workspace/executeCommand` / `fpgalsp.runFlow` and tracks them
// with the pushed `fpgaLsp/flowEvent` notifications (PROTOCOL-V2.md §3-§5).
//
// Phase 2 (A4): the old 2 s `fpgaLsp/flowStatus` poll is gone -- it hammered the server for data
// that `fpgaLsp/flowEvent` already pushes. What is left is a **10 s fallback** that only runs
// while a flow is active, for the case where the server never sends an event (old build, dropped
// notification). When the flow finishes the timer stops.
//
// Depends only on `vscode`; the tree providers and the reports panel are injected.

'use strict';

const vscode = require('vscode');
const { METHODS, COMMANDS } = require('./protocol');
const { parseFlowIssue, resolveFlowFile } = require('./flowDiagnostics');

// Only a safety net: the primary signal is `fpgaLsp/flowEvent`.
const FALLBACK_POLL_MS = 10000;
const FINISH_GRACE_MS = 4000;

function nowText() {
    return new Date().toLocaleTimeString();
}

function fmtElapsed(seconds) {
    const n = Number(seconds);
    if (!Number.isFinite(n) || n < 0) return '';
    return n < 100 ? `${n.toFixed(1)}s` : `${Math.round(n)}s`;
}

function flowLabel(stages) {
    const wanted = (stages || []).map((stage) => String(stage).toLowerCase());
    if (wanted.length === 1 && wanted[0] === 'syn') return 'synthesis';
    if (wanted.length === 1 && wanted[0] === 'phy') return 'place & route';
    return 'full flow';
}

function readFlowJobs() {
    let raw = 2;
    try {
        raw = vscode.workspace.getConfiguration('fpgaLsp').get('flowJobs', 2);
    } catch (_) {
        raw = 2;
    }
    const jobs = parseInt(raw, 10);
    return Math.max(1, Math.min(64, Number.isFinite(jobs) ? jobs : 2));
}

class FlowController {
    /**
     * @param {{
     *   rpc: {request: Function, execute: Function},
     *   moduleTree?: {refresh: Function},
     *   flowTree?: {refresh: Function, applyFlowStatus: Function, applyFlowEvent: Function},
     *   reports?: {refresh: Function},
     * }} options
     */
    constructor(options) {
        const opts = options || {};
        this.rpc = opts.rpc;
        this.moduleTree = opts.moduleTree;
        this.flowTree = opts.flowTree;
        this.reports = opts.reports;

        this.output = vscode.window.createOutputChannel('FPGA Flow');
        // P-C4: flowStatus 只回新增日志行；这里保存合并后的最近窗口（给树用）。
        this._logSeq = undefined;
        this._recentLog = [];
        this._flowDiagnostics = undefined;
        try {
            if (vscode.languages && typeof vscode.languages.createDiagnosticCollection === 'function') {
                this._flowDiagnostics = vscode.languages.createDiagnosticCollection('fpga-flow');
            }
        } catch (_) {
            this._flowDiagnostics = undefined;
        }
        this._flowDiagMap = new Map();
        this._projectInfo = undefined;
        this.getProjectInfo = typeof opts.getProjectInfo === 'function' ? opts.getProjectInfo : undefined;
        this.statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
        this.statusBar.name = 'FPGA Flow';
        this.statusBar.command = 'fpgaLsp.showReports';
        this.statusBar.text = '$(cpu) FPGA: idle';
        this.statusBar.tooltip = 'Anlogic FPGA flow — click to open reports';
        if (typeof this.statusBar.show === 'function') this.statusBar.show();

        this.active = undefined;
        this.poller = undefined;
        this.progress = undefined;
        this.lastMessage = '';
        this._doneResolve = undefined;
        this._cancelRequested = false;
    }

    get running() {
        return !!this.active;
    }

    dispose() {
        this.stopPolling();
        const resolve = this._doneResolve;
        this._doneResolve = undefined;
        this.active = undefined;
        if (resolve) resolve({ ok: false, error: 'disposed' });
        try {
            if (this._flowDiagnostics) this._flowDiagnostics.dispose();
        } catch (_) { /* ignore */ }
        try {
            this.output.dispose();
        } catch (_) {
            // ignore
        }
        try {
            this.statusBar.dispose();
        } catch (_) {
            // ignore
        }
    }

    // ---------------------------------------------------------- Problems diagnostics

    _clearFlowDiagnostics() {
        this._flowDiagMap = new Map();
        if (this._flowDiagnostics) {
            try { this._flowDiagnostics.clear(); } catch (_) { /* ignore */ }
        }
    }

    _captureFlowIssue(message, forceError) {
        if (!this._flowDiagnostics || !message) return;
        let issue;
        try {
            issue = parseFlowIssue(message, { forceError: !!forceError });
        } catch (_) {
            return;
        }
        if (!issue) return;
        const project = this._projectInfo;
        const file = resolveFlowFile(issue.file, project) || (project && project.al) || '';
        if (!file) return;
        const uri = vscode.Uri.file(file);
        const key = uri.toString();
        const line = Math.max(0, (Number(issue.line) || 1) - 1);
        let diagnostic;
        try {
            const severity = issue.severity === 'error'
                ? vscode.DiagnosticSeverity.Error : vscode.DiagnosticSeverity.Warning;
            diagnostic = new vscode.Diagnostic(new vscode.Range(line, 0, line, 1),
                issue.message, severity);
            diagnostic.source = 'fpga-flow';
            diagnostic.code = issue.code || 'FLOW';
        } catch (_) {
            return;
        }
        const list = this._flowDiagMap.get(key) || [];
        if (list.some((item) => item.code === diagnostic.code
            && item.message === diagnostic.message
            && item.range.start.line === line)) return;
        if (list.length >= 300) return;
        list.push(diagnostic);
        this._flowDiagMap.set(key, list);
        try { this._flowDiagnostics.set(uri, list); } catch (_) { /* ignore */ }
    }

    // ---------------------------------------------------------------- status bar

    _setStatus(text, tooltip) {
        if (!this.statusBar) return;
        this.statusBar.text = text;
        this.statusBar.tooltip = tooltip;
        if (typeof this.statusBar.show === 'function') this.statusBar.show();
    }

    setStatusRunning(text) {
        this._setStatus(`$(sync~spin) FPGA: ${text}`, 'FPGA flow running — click to open the report dashboard');
    }

    setStatusDone(ok, active, reason) {
        const label = active ? active.label : 'flow';
        if (ok) {
            this._setStatus(`$(check) FPGA: ${label} done`, 'FPGA flow finished — click to open the report dashboard');
        } else {
            this._setStatus(`$(error) FPGA: ${label} failed`, `FPGA flow failed${reason ? `: ${reason}` : ''}`);
        }
    }

    setStatusIdle() {
        this._setStatus('$(cpu) FPGA: idle', 'Anlogic FPGA flow — click to open reports');
    }

    // ------------------------------------------------------------------- running

    /**
     * Start a flow. Returns the `fpgalsp.runFlow` result normalsed to `{ok, ...}`.
     * While running, the returned promise resolves when the flow reports done/error.
     * @param {string[]} stages ["syn"] | ["phy"] | ["all"]
     */
    async run(stages) {
        const wanted = Array.isArray(stages) && stages.length ? stages.map(String) : ['all'];
        if (this.active) {
            vscode.window.showInformationMessage('An FPGA flow is already running.');
            return { ok: false, error: 'busy' };
        }
        const jobs = readFlowJobs();
        const label = flowLabel(wanted);
        this.lastMessage = '';
        this._cancelRequested = false;
        this._clearFlowDiagnostics();
        try {
            this._projectInfo = this.getProjectInfo ? await this.getProjectInfo() : undefined;
        } catch (_) {
            this._projectInfo = undefined;
        }

        this.output.show(true);
        this.output.appendLine(`[${nowText()}] ▶ run ${wanted.join('+')} (jobs=${jobs})`);
        this.setStatusRunning(`${label} starting…`);

        return vscode.window.withProgress(
            {
                location: vscode.ProgressLocation.Notification,
                title: `FPGA ${label}`,
                cancellable: true,
            },
            async (progress, token) => {
                this.progress = progress;
                let cancelRequested = false;
                if (token && typeof token.onCancellationRequested === 'function') {
                    token.onCancellationRequested(() => {
                        cancelRequested = true;
                        this.requestCancel();
                    });
                }

                let result;
                try {
                    result = await this.rpc.execute(COMMANDS.runFlow, [{ stages: wanted, jobs }]);
                } catch (err) {
                    const why = err && err.message ? err.message : String(err);
                    this.output.appendLine(`[${nowText()}] ✖ runFlow failed: ${why}`);
                    this.setStatusIdle();
                    vscode.window.showErrorMessage(`FPGA flow failed to start: ${why}`);
                    return { ok: false, error: why };
                }

                if (!result || !result.ok) {
                    const why = result && result.error ? String(result.error) : 'unknown error';
                    this.output.appendLine(`[${nowText()}] ✖ runFlow rejected: ${why}`);
                    this.setStatusIdle();
                    if (cancelRequested) {
                        vscode.window.showInformationMessage('FPGA flow cancelled.');
                    } else if (why === 'busy') {
                        vscode.window.showWarningMessage('An FPGA flow is already running on the server.');
                    } else {
                        vscode.window.showWarningMessage(`FPGA flow did not start: ${why}`);
                    }
                    return result || { ok: false, error: why };
                }

                this.active = {
                    runId: result.runId,
                    stages: wanted,
                    label,
                    startedAt: Date.now(),
                    sawRunning: false,
                };
                this.output.appendLine(`[${nowText()}] runId ${result.runId || '(none)'}`);
                const done = new Promise((resolve) => {
                    this._doneResolve = resolve;
                });
                this.startPolling();
                if (cancelRequested) this.requestCancel();
                return done;
            }
        );
    }

    async cancel() {
        if (!this.active) {
            vscode.window.showInformationMessage('No FPGA flow is running.');
            return { ok: false, error: 'idle' };
        }
        await this.requestCancel();
        return { ok: true };
    }

    async requestCancel() {
        if (!this.active || this._cancelRequested) return;
        this._cancelRequested = true;
        this.output.appendLine(`[${nowText()}] cancel requested`);
        this.setStatusRunning('cancelling…');
        try {
            await this.rpc.execute(COMMANDS.cancelFlow, []);
        } catch (err) {
            this.output.appendLine(`[${nowText()}] cancel request failed: ${err && err.message ? err.message : err}`);
        }
        this.finish(false, 'cancelled');
    }

    finish(ok, reason) {
        if (!this.active) return;
        if (!ok) this._captureFlowIssue(reason || 'flow failed', true);
        const active = this.active;
        this.active = undefined;
        this.stopPolling();
        const elapsed = (Date.now() - active.startedAt) / 1000;
        if (ok) {
            this.output.appendLine(`[${nowText()}] ✓ ${active.label} finished after ${fmtElapsed(elapsed)}`);
        } else {
            this.output.appendLine(`[${nowText()}] ✖ ${active.label} failed after ${fmtElapsed(elapsed)}: ${reason || 'unknown error'}`);
        }
        this.setStatusDone(!!ok, active, reason);
        if (ok) {
            vscode.window.showInformationMessage(`FPGA ${active.label} finished in ${fmtElapsed(elapsed)}.`);
        } else {
            vscode.window.showWarningMessage(
                `FPGA flow failed${reason ? `: ${reason}` : ''}. See the "FPGA Flow" output channel for details.`
            );
        }
        const resolve = this._doneResolve;
        this._doneResolve = undefined;
        if (resolve) resolve({ ok: !!ok, error: ok ? undefined : reason });
        this.refreshAll();
    }

    // ------------------------------------------------------------------ polling

    /** Start the 10 s safety-net poll (only while a flow is active). */
    startPolling() {
        this.stopPolling();
        this.poller = setInterval(() => {
            this.pollOnce().catch(() => undefined);
        }, FALLBACK_POLL_MS);
        // Do not keep a test runner / extension host alive just for the poll timer.
        if (this.poller && typeof this.poller.unref === 'function') this.poller.unref();
    }

    stopPolling() {
        if (this.poller) {
            clearInterval(this.poller);
            this.poller = undefined;
        }
    }

    async pollOnce() {
        if (!this.active) return;
        let payload;
        try {
            // P-C4: ask only for lines we have not seen. The server answers with a monotonic
            // `flow.logSeq` plus just the new lines; older servers ignore the field and return
            // the whole window, which still works.
            const params = this._logSeq === undefined ? {} : { sinceSeq: this._logSeq };
            payload = await this.rpc.request(METHODS.flowStatus, params);
        } catch (_) {
            return; // transient failure: keep polling
        }
        if (!this.active) return;
        const flow = payload && payload.flow;
        if (!flow || typeof flow !== 'object') return;
        const logSeq = Number(flow.logSeq);
        if (Number.isFinite(logSeq)) this._logSeq = logSeq;
        if (Array.isArray(flow.log) && flow.log.length) {
            for (const line of flow.log) this._appendFlowLog(line);
            // The tree must keep the whole recent history even though the server only sent
            // the delta: hand it the merged window rather than the slice.
            flow.log = this._recentLog;
        }

        if (this.flowTree && typeof this.flowTree.applyFlowStatus === 'function') {
            this.flowTree.applyFlowStatus(payload);
        }
        this._logMessage(flow.lastMessage);

        if (flow.running) {
            this.active.sawRunning = true;
            const elapsed = fmtElapsed(flow.elapsed);
            this.setStatusRunning(`${flow.stage || this.active.label}${elapsed ? ` · ${elapsed}` : ''}`);
            return;
        }
        // The server may not have flipped `running` yet right after runFlow returned; wait for
        // either one running observation or a grace period before declaring the flow finished.
        const age = Date.now() - this.active.startedAt;
        if (this.active.sawRunning || age > FINISH_GRACE_MS) {
            const ok = flow.ok !== false && !flow.error;
            this.finish(ok, ok ? undefined : (flow.error || flow.lastMessage || 'flow failed'));
        }
    }

    /** P-C4: append one server log line to the local ring buffer (200 lines). */
    _appendFlowLog(line) {
        if (line === undefined || line === null) return;
        const text = String(line);
        if (!text) return;
        this._recentLog.push(text);
        if (this._recentLog.length > 200) this._recentLog.splice(0, this._recentLog.length - 200);
        this.output.appendLine(`[${nowText()}] ${text}`);
    }

    _logMessage(message) {
        if (!message) return;
        const text = String(message);
        if (text === this.lastMessage) return;
        this.lastMessage = text;
        this.output.appendLine(`[${nowText()}] ${text}`);
        if (this.progress && typeof this.progress.report === 'function') {
            this.progress.report({ message: text });
        }
    }

    // ------------------------------------------------------ pushed flow events

    /** Handle an `fpgaLsp/flowEvent` notification (PROTOCOL-V2.md §4). */
    onFlowEvent(event) {
        if (!event || typeof event !== 'object' || !event.type) return;
        if (event.message) this._captureFlowIssue(event.message, event.type === 'error');
        // Events can arrive before runFlow (contract §6) and for a different run: ignore both.
        if (!this.active) return;
        if (event.runId && this.active.runId && event.runId !== this.active.runId) return;

        if (this.flowTree && typeof this.flowTree.applyFlowEvent === 'function') {
            this.flowTree.applyFlowEvent(event);
        }

        const stage = event.stage ? `[${event.stage}] ` : '';
        const type = String(event.type);
        if (type === 'done') {
            this.output.appendLine(`${stage}✓ done`);
            this.finish(event.ok !== false, event.ok === false ? 'reported failure' : undefined);
        } else if (type === 'error') {
            this.output.appendLine(`${stage}✖ error${event.message ? `: ${event.message}` : ''}`);
            this.finish(false, event.message || 'error');
        } else if (type === 'start') {
            this.output.appendLine(`${stage}▶ started`);
            if (event.elapsed !== undefined) {
                this.setStatusRunning(`${event.stage || this.active.label} · ${fmtElapsed(event.elapsed)}`);
            }
        } else if (event.message) {
            this.output.appendLine(`${stage}${event.message}`);
            this._logMessage(event.message);
            if (event.elapsed !== undefined) {
                this.setStatusRunning(`${event.stage || this.active.label} · ${fmtElapsed(event.elapsed)}`);
            }
        }
    }

    /** Reveal the dedicated FPGA Flow output channel. */
    showLog() {
        try {
            this.output.show(true);
        } catch (_) { /* ignore */ }
    }

    // ------------------------------------------------------------------ refresh

    async refreshAll() {
        const tasks = [];
        if (this.moduleTree && typeof this.moduleTree.refresh === 'function') {
            tasks.push(this.moduleTree.refresh());
        }
        if (this.flowTree && typeof this.flowTree.refresh === 'function') {
            tasks.push(this.flowTree.refresh());
        }
        if (this.reports && typeof this.reports.refresh === 'function') {
            tasks.push(this.reports.refresh());
        }
        await Promise.allSettled(tasks);
    }
}

module.exports = {
    FlowController,
    flowLabel,
    readFlowJobs,
    fmtElapsed,
    FALLBACK_POLL_MS,
    // Kept for older callers/tests: the value now is the 10 s fallback interval.
    POLL_INTERVAL_MS: FALLBACK_POLL_MS,
};
