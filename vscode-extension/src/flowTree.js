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

/**
 * `1m11s` style duration for a wall/cpu time in seconds (TD writes `1m11s`).
 * 0 / unknown renders as '' so a pending step shows no fake `0s`.
 */
function fmtWall(seconds) {
    const n = Number(seconds);
    if (!Number.isFinite(n) || n <= 0) return '';
    const total = Math.round(n);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    if (h > 0) return `${h}h${m}m${s}s`;
    if (m > 0) return `${m}m${s}s`;
    return `${s}s`;
}

function statusIcon(status) {
    const text = String(status || '').toLowerCase();
    // "not run" 是 TD GUI 的“没跑过”，不是 running（否则 /run/ 会把它显示成转圈）。
    if (/^not run$/.test(text)) return new vscode.ThemeIcon('circle-outline');
    if (/fail|error/.test(text)) return new vscode.ThemeIcon('error');
    if (/finish|done|pass|success|met/.test(text)) return new vscode.ThemeIcon('pass');
    if (/run|progress|active/.test(text)) return new vscode.ThemeIcon('sync~spin');
    return new vscode.ThemeIcon('circle-outline');
}

// syn / phy 的分组：后端 steps 的 stage 是 TD 的参数命名空间
// （design/rtl/gate = 综合，place/route/bitgen = 布局布线），run 是 syn_1/phy_1。
function guiGroup(stage, run) {
    const text = `${stage || ''} ${run || ''}`.toLowerCase();
    if (/place|route|bitgen|phy/.test(text)) return 'phy';
    return 'syn';
}

// TD GUI 的六个步骤（syn 3 + phy 3）。**始终**渲染这 6 行：后端 `flow.steps` 的
// name 已经就是这些 GUI 名（Read Design … Generate Bitstream），不要再翻译一遍；
// 没有 steps / 没跑过的步骤显示 "not run"。
const GUI_FLOW_STEPS = Object.freeze([
    { id: 'design', name: 'Read Design', stage: 'design', group: 'syn' },
    { id: 'rtl', name: 'Optimize RTL', stage: 'rtl', group: 'syn' },
    { id: 'gate', name: 'Optimize Gate', stage: 'gate', group: 'syn' },
    { id: 'place', name: 'Optimize Placement', stage: 'place', group: 'phy' },
    { id: 'route', name: 'Optimize Routing', stage: 'route', group: 'phy' },
    { id: 'bitgen', name: 'Generate Bitstream', stage: 'bitgen', group: 'phy' },
]);

/** 名字比较用：忽略大小写/空格/下划线，`Optimize_RTL` == `Optimize RTL`。 */
function stepKey(value) {
    return String(value || '').toLowerCase().replace(/[^a-z0-9]+/g, '');
}

/** pending / 缺失 / 空 = 没跑过；TD GUI（和后端 flow.steps）里显示成 "not run"。 */
function stepStatusLabel(status) {
    const text = String(status === undefined || status === null ? '' : status).trim();
    if (!text || /^(pending|not\s*run|notrun|none|idle)$/i.test(text)) return 'not run';
    return text;
}

/** 后端 `flowStatus.steps` / 老 `reports.flowStatus` 的统一行形状。 */
function stepRow(entry, fallbackGroup) {
    const wall = Number(entry.wall) || 0;
    return {
        name: String(entry.name || ''),
        stage: entry.stage ? String(entry.stage) : '',
        run: entry.run ? String(entry.run) : '',
        status: stepStatusLabel(entry.status),
        wall,
        cpu: Number(entry.cpu) || 0,
        mem_mb: Number(entry.mem_mb) || 0,
        // 老格式（reports.flowStatus）只有现成的 elapsed 文本，没有秒数
        elapsed: entry.elapsed ? String(entry.elapsed) : '',
        group: fallbackGroup,
    };
}

/** 一行的树节点（标签带 wall 时间，状态用图标 + description 区分）。 */
function stageNode(row, group, canonicalId) {
    const description = [
        row.status,
        row.elapsed,
        row.cpu > 0 ? `cpu ${fmtWall(row.cpu)}` : '',
        row.mem_mb > 0 ? `${row.mem_mb} MB` : '',
    ].filter(Boolean).join(' · ');
    return {
        kind: 'stage',
        id: `flow:step:${group}:${canonicalId}`,
        label: row.wall > 0 ? `${row.name} · ${fmtWall(row.wall)}` : row.name,
        description,
        status: row.status,
        group,
        wall: row.wall,
    };
}

/**
 * Backend GUI steps — `fpgaLsp/flowStatus` → `flow.steps`:
 * `[{name, stage, status, wall, cpu, mem_mb, run}]` where wall/cpu are **seconds**
 * and name is the TD GUI label (`Read Design` … `Generate Bitstream`).
 * JSON 里的 wall/cpu 单位是秒（后端把 "1m11s(62%)" 归一化过）。
 * Older servers never emit `steps`; the caller then falls back to the stage rows.
 */
function flowRunSteps(flow) {
    const raw = flow && Array.isArray(flow.steps) ? flow.steps : [];
    const rows = [];
    for (const entry of raw) {
        if (!entry || typeof entry !== 'object') continue;
        const name = entry.name || entry.step || entry.stage || '';
        if (!name) continue;
        rows.push({
            name: String(name),
            stage: entry.stage ? String(entry.stage) : '',
            status: String(entry.status || 'pending'),
            wall: Number(entry.wall) || 0,
            cpu: Number(entry.cpu) || 0,
            mem_mb: Number(entry.mem_mb) || 0,
            run: entry.run ? String(entry.run) : '',
        });
    }
    return rows;
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
        return ['failed', stage !== 'idle' ? stage : '', elapsed, message].filter(Boolean).join(' · ');
    }
    const status = flow.status && flow.status !== stage ? String(flow.status) : '';
    // 总耗时留在父节点（Flow 段）上：跑完/失败后 elapsed 也要看得见
    return [stage, status, elapsed, message].filter(Boolean).join(' · ') || 'idle';
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
        // 单独问一次 flowStatus：GUI 的 6 步（含每步耗时）只在 `flow.steps` 里，
        // `fpgaLsp/reports` 的 flowStatus 是 command 级的。旧服务器会忽略这个请求的
        // 结果（没有 flow 字段），这里用 allSettled 兜住失败，不阻塞树的刷新。
        const flowPromise = this.rpc && typeof this.rpc.request === 'function'
            ? this.rpc.request(METHODS.flowStatus, {})
            : Promise.resolve(undefined);
        const [reportsResult, infoResult, flowResult] = await Promise.allSettled([
            reportsPromise,
            this.getInfo(),
            flowPromise,
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
        // flowStatus 是 flow 的权威来源（reports.flow 只是同一份状态的快照副本）：
        // steps 只从这里来，所以放在 reports.flow 之后合并。
        const flowPayload = flowResult.status === 'fulfilled' ? flowResult.value : undefined;
        const backendFlow = flowPayload && typeof flowPayload === 'object'
            && flowPayload.flow && typeof flowPayload.flow === 'object'
            ? flowPayload.flow
            : undefined;
        if (backendFlow) this.flow = Object.assign({}, this.flow, backendFlow);
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
        // 两种数据来源：
        //   1) 后端给出了 `flow.steps`（TD GUI 的 6 步 + 每步耗时）—— 首选；
        //   2) 老服务器只有 `reports.flowStatus`（command 级、没有 status/秒数）—— 回落。
        // 不管哪种，最终都按 GUI 六步渲染：缺的步骤补 "not run"，老格式多出来的行
        // （例如 opt_rtl）追加在所属分组末尾，不丢数据。
        const backendSteps = flowRunSteps(flow);
        const source = [];
        if (backendSteps.length) {
            for (const step of backendSteps) {
                source.push(stepRow({
                    name: step.name,
                    stage: step.stage,
                    run: step.run,
                    status: step.status,
                    wall: step.wall,
                    cpu: step.cpu,
                    mem_mb: step.mem_mb,
                }, guiGroup(step.stage, step.run)));
            }
        } else {
            const classify = (stage) => {
                const text = `${stage.step} ${(stage.raw && (stage.raw.stage || stage.raw.name)) || ''}`.toLowerCase();
                if (/place|route|phy|pr|hold|bit|timing/.test(text)) return 'phy';
                if (/syn|gate|elab|rtl|opt|logic/.test(text)) return 'syn';
                return 'syn';
            };
            for (const stage of flowSteps(reports)) {
                const stageName = (stage.raw && (stage.raw.stage || stage.raw.name)) || '';
                source.push(stepRow({
                    name: stage.step || `step ${source.length + 1}`,
                    stage: stageName,
                    status: stage.status,
                    elapsed: stage.elapsed,
                }, classify(stage)));
            }
        }

        const groups = { syn: [], phy: [] };
        const used = new Set();
        for (const canonical of GUI_FLOW_STEPS) {
            let match = -1;
            for (let index = 0; index < source.length; index += 1) {
                if (used.has(index)) continue;
                const row = source[index];
                if (stepKey(canonical.name) === stepKey(row.name)
                    || (row.stage && stepKey(canonical.stage) === stepKey(row.stage))) {
                    match = index;
                    break;
                }
            }
            let row;
            if (match >= 0) {
                used.add(match);
                row = source[match];
            } else {
                row = stepRow({
                    name: canonical.name,
                    stage: canonical.stage,
                    status: 'not run',
                }, canonical.group);
            }
            groups[canonical.group].push(stageNode(row, canonical.group, canonical.id));
        }
        // 老格式 / 非 GUI 名的步骤：追加到各自分组（不丢掉后端给的信息）。
        for (let index = 0; index < source.length; index += 1) {
            if (used.has(index)) continue;
            const row = source[index];
            const group = row.group === 'phy' ? 'phy' : 'syn';
            groups[group].push(stageNode(row, group, `extra${index}-${stepKey(row.name) || index}`));
        }

        const groupDefs = [
            { id: 'syn', label: 'Synthesis (syn)', icon: new vscode.ThemeIcon('chip') },
            { id: 'phy', label: 'Place & Route (phy)', icon: new vscode.ThemeIcon('circuit-board') },
        ];
        for (const group of groupDefs) {
            const groupStages = groups[group.id];
            const ranStages = groupStages.filter((row) => !/^not run$/i.test(String(row.status)));
            const groupWall = groupStages.reduce((sum, row) => sum + (Number(row.wall) || 0), 0);
            children.push({
                kind: 'section',
                id: `flow:group:${group.id}`,
                label: group.label,
                icon: group.icon,
                description: ranStages.length
                    ? [`${groupStages.length} steps`, fmtWall(groupWall)].filter(Boolean).join(' · ')
                    : 'not run',
                children: groupStages,
            });
        }
        children.push({
            kind: 'message',
            id: 'flow:last',
            label: flow.lastMessage ? String(flow.lastMessage) : 'No flow message yet',
            description: flow.elapsed ? `elapsed ${fmtElapsed(flow.elapsed)}` : '',
        });
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
    flowRunSteps,
    fmtSize,
    fmtTime,
    fmtElapsed,
    fmtWall,
    describeFlow,
    statusIcon,
    VIEW_ID,
};
