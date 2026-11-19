// Project & flow tree for the "FPGA" activity-bar container.
//
// Data comes from `fpgaLsp/reports` (PROTOCOL-V2.md §2), `fpgaLsp/flowStatus` (§3) and the
// legacy `fpgalsp.info` command (project/device/top summary). Only depends on `vscode` so the
// unit tests can load it under test/mock-vscode.js.

'use strict';

const vscode = require('vscode');
const { METHODS, VSCodeCommands } = require('./protocol');

const VIEW_ID = 'fpgaLsp.flowView';

function pad2(value) {
    return value < 10 ? `0${value}` : String(value);
}

/** Human readable byte size for the artifact rows. */
function fmtSize(bytes) {
    const n = Number(bytes);
    if (!Number.isFinite(n) || n < 0) return '';
    if (n < 1024) return `${n} B`;
    const units = ['KB', 'MB', 'GB', 'TB'];
    let value = n / 1024;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit += 1;
    }
    return `${value >= 10 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

/** Format a unix timestamp (seconds) as local `YYYY-MM-DD HH:mm`. */
function fmtTime(seconds) {
    const n = Number(seconds);
    if (!Number.isFinite(n) || n <= 0) return '';
    const date = new Date(n * 1000);
    return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())} `
        + `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
}

function fmtElapsed(seconds) {
    const n = Number(seconds);
    if (!Number.isFinite(n) || n < 0) return '';
    return n < 100 ? `${n.toFixed(1)}s` : `${Math.round(n)}s`;
}

function statusIcon(status) {
    const text = String(status || '').toLowerCase();
    if (/fail|error/.test(text)) return new vscode.ThemeIcon('error');
    if (/finish|done|pass|success|met/.test(text)) return new vscode.ThemeIcon('pass');
    if (/run|progress|active/.test(text)) return new vscode.ThemeIcon('sync~spin');
    return new vscode.ThemeIcon('circle-outline');
}

/**
 * Normalise `reports.flowStatus` into rows with `{step, status, elapsed}`.
 * PROTOCOL-V2 §2 documents an array, but reports.py also emits
 * `{setup, hold, mode, steps: [{name, wall, cpu, mem_mb, stage}]}` — accept both.
 */
function flowSteps(reports) {
    const raw = reports && reports.flowStatus;
    const list = Array.isArray(raw) ? raw : (raw && Array.isArray(raw.steps) ? raw.steps : []);
    const rows = [];
    for (const entry of list) {
        if (!entry || typeof entry !== 'object') continue;
        const step = entry.step || entry.name || entry.stage || '';
        const status = entry.status || entry.stage || '';
        const elapsed = entry.elapsed || entry.wall || '';
        rows.push({ step: String(step), status: String(status), elapsed: String(elapsed), raw: entry });
    }
    return rows;
}

function artifactIcon(exists) {
    return new vscode.ThemeIcon(exists ? 'file-binary' : 'circle-slash');
}

function describeFlow(flow) {
    if (!flow) return 'idle';
    const stage = flow.stage ? String(flow.stage) : 'idle';
    const elapsed = flow.elapsed ? fmtElapsed(flow.elapsed) : '';
    const message = flow.lastMessage ? String(flow.lastMessage) : '';
    if (flow.running) return ['running', stage, elapsed].filter(Boolean).join(' · ');
    if (flow.ok === false || flow.status === 'error') {
        return ['failed', stage !== 'idle' ? stage : '', message].filter(Boolean).join(' · ');
    }
    const status = flow.status && flow.status !== stage ? String(flow.status) : '';
    return [stage, status, message].filter(Boolean).join(' · ') || 'idle';
}

class FlowTreeProvider {
    /**
     * @param {{request: Function}} rpc
     * @param {() => Promise<any>} [getInfo] fetch `fpgalsp.info` for the project summary.
     * @param {{fetch: Function}} [reportsService] shared, single-flight reports cache (Phase 2).
     */
    constructor(rpc, getInfo, reportsService) {
        this.rpc = rpc;
        this.getInfo = typeof getInfo === 'function' ? getInfo : async () => undefined;
        this.reportsService = reportsService && typeof reportsService.fetch === 'function'
            ? reportsService : undefined;
        this.reports = undefined;
        this.info = undefined;
        this.flow = { running: false, stage: 'idle', elapsed: 0, lastMessage: '' };
        this.error = undefined;
        this.loaded = false;
        this.loading = false;
        this.view = undefined;
        this._seq = 0;
        this._emitter = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._emitter.event;
    }

    dispose() {
        this._emitter.dispose();
    }

    attachView(view) {
        this.view = view;
        this._syncMessage();
    }

    fire() {
        this._emitter.fire(undefined);
    }

    _syncMessage() {
        if (!this.view) return;
        if (this.loading && !this.loaded) {
            this.view.message = 'Loading FPGA flow state…';
            return;
        }
        if (!this.loaded) {
            this.view.message = 'Waiting for the FPGA language server…';
            return;
        }
        const reportsOk = !!(this.reports && this.reports.ok);
        if (!reportsOk && !this.info) {
            this.view.message = 'No Anlogic .al project or flow data found.';
            return;
        }
        if (!reportsOk) {
            this.view.message = 'No flow reports yet — run a flow.';
            return;
        }
        this.view.message = undefined;
    }

    /**
     * Pull the report payload (shared with the reports panel when a ReportsService is injected)
     * plus `fpgalsp.info`, then rebuild the tree.
     * @param {{force?: boolean}} [options]
     */
    async refresh(options) {
        const opts = options || {};
        const seq = ++this._seq;
        this.loading = true;
        this._syncMessage();
        // One shared request: the reports panel uses the same service, so opening both views
        // costs a single `fpgaLsp/reports` round trip (Phase 2 single-flight).
        const reportsPromise = this.reportsService
            ? this.reportsService.fetch({ section: 'summary', force: opts.force === true })
            : this.rpc.request(METHODS.reports, {});
        const [reportsResult, infoResult] = await Promise.allSettled([
            reportsPromise,
            this.getInfo(),
        ]);
        if (seq !== this._seq) return this.reports;
        this.loading = false;

        let reports = reportsResult.status === 'fulfilled' ? reportsResult.value : undefined;
        if (!reports || typeof reports !== 'object') {
            reports = {
                ok: false,
                error: reportsResult.status === 'rejected'
                    ? String((reportsResult.reason && reportsResult.reason.message) || reportsResult.reason)
                    : 'no report data',
            };
        }
        this.reports = reports;
        this.error = reports.ok ? undefined : (reports.error || 'no flow reports yet');

        const info = infoResult.status === 'fulfilled' ? infoResult.value : undefined;
        if (info && typeof info === 'object' && info.ok !== false) this.info = info;

        if (reports.flow && typeof reports.flow === 'object') {
            this.flow = Object.assign({}, this.flow, reports.flow);
        }
        this.loaded = true;
        this._syncMessage();
        this.fire();
        return reports;
    }

    /** Called by the 2 s poller / on demand while a flow runs. */
    applyFlowStatus(payload) {
        const flow = payload && payload.flow;
        if (!flow || typeof flow !== 'object') return;
        this.flow = Object.assign({}, this.flow, flow);
        this.loaded = true;
        this._syncMessage();
        this.fire();
    }

    /** Called for pushed `fpgaLsp/flowEvent` notifications. */
    applyFlowEvent(event) {
        if (!event || typeof event !== 'object' || !event.type) return;
        const next = Object.assign({}, this.flow);
        if (event.stage) next.stage = event.stage;
        if (event.elapsed !== undefined && event.elapsed !== null) next.elapsed = event.elapsed;
        if (event.message) next.lastMessage = event.message;
        if (event.type === 'done' || event.type === 'error') {
            next.running = false;
            next.ok = event.type === 'done' && event.ok !== false;
            next.status = event.type === 'done' && event.ok !== false ? 'done' : 'error';
        } else {
            next.running = true;
            next.status = 'running';
        }
        this.flow = next;
        this.loaded = true;
        this.fire();
    }

    _projectRows(reports) {
        const info = this.info || {};
        const rows = [
            ['Project', info.project || (reports && reports.project)],
            ['Device', info.device || (reports && reports.device)],
            ['Top module', info.top || (reports && reports.top)],
            ['Project file', info.al || (reports && reports.al)],
        ];
        return rows
            .filter(([, value]) => value !== undefined && value !== null && value !== '')
            .map(([label, value], index) => ({
                kind: 'kv',
                id: `project:${index}:${label}`,
                label,
                description: String(value),
                contextValue: label === 'Project' ? 'fpgaProject'
                    : (label === 'Project file' ? 'fpgaProjectFile' : 'fpgaInfo'),
            }));
    }

    _flowChildren(flow, reports) {
        const children = [];
        if (flow.running) {
            children.push({
                kind: 'stage',
                id: 'flow:current',
                label: String(flow.stage || 'flow'),
                description: ['running', fmtElapsed(flow.elapsed)].filter(Boolean).join(' · '),
                status: 'Running',
            });
        }
        const stages = flowSteps(reports);
        const classify = (stage) => {
            const text = `${stage.step} ${(stage.raw && (stage.raw.stage || stage.raw.name)) || ''}`.toLowerCase();
            if (/syn|gate|elab|rtl|opt|logic/.test(text)) return 'syn';
            if (/place|route|phy|pr|hold|bit|timing/.test(text)) return 'phy';
            return 'syn';
        };
        const groups = [
            { id: 'syn', label: 'Synthesis (syn)', icon: new vscode.ThemeIcon('chip') },
            { id: 'phy', label: 'Place & Route (phy)', icon: new vscode.ThemeIcon('circuit-board') },
        ];
        for (const group of groups) {
            const groupStages = stages.filter((stage) => classify(stage) === group.id);
            children.push({
                kind: 'section',
                id: `flow:group:${group.id}`,
                label: group.label,
                icon: group.icon,
                description: groupStages.length ? `${groupStages.length} steps` : 'not run',
                children: groupStages.length ? groupStages.map((stage, index) => ({
                    kind: 'stage',
                    id: `flow:${group.id}:stage:${index}`,
                    label: stage.step || `step ${index + 1}`,
                    description: [stage.status, stage.elapsed].filter(Boolean).join(' · '),
                    status: stage.status,
                })) : [{
                    kind: 'placeholder',
                    id: `flow:${group.id}:placeholder`,
                    label: group.id === 'syn' ? 'No synthesis run yet' : 'No place & route run yet',
                }],
            });
        }
        children.push({
            kind: 'message',
            id: 'flow:last',
            label: flow.lastMessage ? String(flow.lastMessage) : 'No flow message yet',
            description: flow.elapsed ? `elapsed ${fmtElapsed(flow.elapsed)}` : '',
        });
        if (!stages.length && !flow.running) {
            children.push({
                kind: 'placeholder',
                id: 'flow:placeholder',
                label: 'No flow has run yet — use the buttons in the view title',
            });
        }
        return children;
    }

    _artifactChildren(reports) {
        const artifacts = (reports && reports.artifacts) || {};
        const children = [];
        const rows = [
            ['bit', 'Bitstream', 'file-binary'],
            ['syn_db', 'Synthesis DB', 'database'],
            ['route_db', 'Route DB', 'database'],
        ];
        for (const [key, label, icon] of rows) {
            const artifact = artifacts[key];
            if (!artifact || typeof artifact !== 'object') continue;
            const path = artifact.path ? String(artifact.path) : '';
            const exists = artifact.exists !== false && !!path;
            children.push({
                kind: 'artifact',
                id: `artifact:${key}`,
                label,
                icon,
                path,
                exists,
                size: artifact.size,
                mtime: artifact.mtime,
            });
        }
        const runs = Array.isArray(artifacts.runs) ? artifacts.runs : [];
        runs.forEach((run, index) => {
            if (!run || typeof run !== 'object') return;
            const dir = run.dir ? String(run.dir) : '';
            children.push({
                kind: 'artifact',
                id: `artifact:run:${index}`,
                label: `Run: ${run.name || index + 1}`,
                icon: 'folder',
                path: dir,
                exists: run.exists !== false && !!dir,
                size: undefined,
                mtime: undefined,
            });
        });
        if (!children.length) {
            children.push({
                kind: 'placeholder',
                id: 'artifact:placeholder',
                label: 'No artifacts yet — run Synthesis or the Full Flow',
            });
        }
        return children;
    }

    _sections() {
        const reports = this.reports && this.reports.ok ? this.reports : undefined;
        const info = this.info;
        if (!reports && !info) return [];

        const flow = this.flow || {};
        const sections = [];

        const projectRows = this._projectRows(reports);
        sections.push({
            kind: 'section',
            id: 'section:project',
            label: 'Project',
            icon: new vscode.ThemeIcon('chip'),
            description: (info && info.device) || (reports && reports.device) || '',
            children: projectRows.length ? projectRows : [{
                kind: 'placeholder',
                id: 'project:placeholder',
                label: 'No project information available',
            }],
        });

        sections.push({
            kind: 'section',
            id: 'section:flow',
            label: 'Flow',
            icon: new vscode.ThemeIcon(flow.running ? 'sync~spin' : 'play'),
            description: describeFlow(flow),
            children: this._flowChildren(flow, reports),
        });

        sections.push({
            kind: 'section',
            id: 'section:artifacts',
            label: 'Artifacts',
            icon: new vscode.ThemeIcon('package'),
            description: (reports && reports.artifacts && reports.artifacts.bit && reports.artifacts.bit.exists)
                ? 'bitstream ready'
                : '',
            children: this._artifactChildren(reports),
        });

        return sections;
    }

    getChildren(element) {
        if (!element) return this._sections();
        return Array.isArray(element.children) ? element.children : [];
    }

    getTreeItem(node) {
        if (!node) return new vscode.TreeItem('?');
        switch (node.kind) {
            case 'section': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
                item.id = node.id;
                item.iconPath = node.icon;
                item.description = node.description || undefined;
                item.tooltip = node.label;
                item.contextValue = 'fpgaSection';
                return item;
            }
            case 'kv': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description;
                item.iconPath = new vscode.ThemeIcon('symbol-field');
                item.tooltip = `${node.label}: ${node.description}`;
                item.contextValue = node.contextValue || 'fpgaInfo';
                return item;
            }
            case 'stage': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description || undefined;
                item.iconPath = statusIcon(node.status);
                item.tooltip = `${node.label}${node.description ? ` — ${node.description}` : ''}`;
                item.contextValue = 'fpgaStage';
                return item;
            }
            case 'message': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description || undefined;
                item.iconPath = new vscode.ThemeIcon('comment');
                item.tooltip = node.label;
                item.contextValue = 'fpgaMessage';
                return item;
            }
            case 'artifact': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                const bits = [];
                if (node.path) bits.push(node.path);
                if (node.size !== undefined && node.size !== null) bits.push(fmtSize(node.size));
                if (node.mtime !== undefined && node.mtime !== null) bits.push(fmtTime(node.mtime));
                item.description = node.exists ? bits.filter(Boolean).join(' · ') : 'not built';
                item.tooltip = [node.label, node.path, item.description].filter(Boolean).join('\n');
                item.iconPath = artifactIcon(node.exists);
                item.contextValue = node.exists && node.path ? 'fpgaArtifact' : 'fpgaArtifactMissing';
                if (node.exists && node.path) {
                    item.command = {
                        command: VSCodeCommands.openArtifact,
                        title: 'Open Artifact',
                        arguments: [node.path],
                    };
                }
                return item;
            }
            case 'report-file': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.path;
                item.tooltip = node.path;
                item.iconPath = new vscode.ThemeIcon('graph');
                item.contextValue = 'fpgaReportFile';
                item.command = {
                    command: VSCodeCommands.openReportFile,
                    title: 'Open Report File',
                    arguments: [node.path],
                };
                return item;
            }
            case 'action': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon(node.icon || 'graph');
                item.contextValue = 'fpgaAction';
                item.command = { command: node.command, title: node.label };
                return item;
            }
            case 'placeholder':
            default: {
                const item = new vscode.TreeItem(node.label || '?', vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon('info');
                item.contextValue = 'fpgaPlaceholder';
                return item;
            }
        }
    }
}

module.exports = {
    FlowTreeProvider,
    flowSteps,
    fmtSize,
    fmtTime,
    fmtElapsed,
    describeFlow,
    statusIcon,
    VIEW_ID,
};
