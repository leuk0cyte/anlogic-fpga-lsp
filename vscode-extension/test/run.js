// Unit tests for the fpga-lsp VS Code IDE views.
//
// Run with:  node test/run.js
// No VS Code installation and no `npm install` are required: `require('vscode')` is patched to
// ./mock-vscode.js before the src/ modules are loaded.

'use strict';

const Module = require('module');
const mock = require('./mock-vscode');

// Stand-in for `vscode-languageclient/node`, the only dependency of extension.js.
class FakeLanguageClient {
    constructor(id, name, serverOptions, clientOptions) {
        this.id = id;
        this.name = name;
        this.serverOptions = serverOptions;
        this.clientOptions = clientOptions;
        this.notificationHandlers = {};
        this.running = false;
        FakeLanguageClient.instance = this;
    }

    async start() {
        this.running = true;
    }

    isRunning() {
        return this.running;
    }

    onNotification(method, handler) {
        this.notificationHandlers[method] = handler;
    }

    async sendRequest(method, params) {
        FakeLanguageClient.requests.push({ method, params });
        const handler = FakeLanguageClient.responses[method];
        if (handler) return handler(params);
        return { ok: true };
    }

    async stop() {
        this.running = false;
    }
}
FakeLanguageClient.responses = {};
FakeLanguageClient.requests = [];
FakeLanguageClient.instance = undefined;

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
    if (request === 'vscode') return mock;
    if (request === 'vscode-languageclient/node') {
        return { LanguageClient: FakeLanguageClient, TransportKind: { stdio: 0 } };
    }
    return originalLoad.call(this, request, parent, isMain);
};

const {
    ModuleTreeProvider,
    resolveTarget,
    definitionTarget,
    instantiationTarget,
} = require('../src/moduleTree');
const { FlowTreeProvider } = require('../src/flowTree');
const { ReportsTreeProvider } = require('../src/reportsTree');
const mcpConfig = require('../src/mcpConfig');
const { parseFlowIssue } = require('../src/flowDiagnostics');
const {
    FlowController,
    resolveFlowTimeoutMs,
    resolveFlowDeadline,
    isFlowTimedOut,
    shouldAbandonOnErrors,
    MAX_CONSECUTIVE_FLOW_ERRORS,
} = require('../src/flowController');
const { FALLBACK_POLL_MS } = require('../src/flowController');
const { ReportsService, CancelledError } = require('../src/reportsService');
const { renderReportsHtml } = require('../src/reportsHtml');
const { renderTrendHtml } = require('../src/trendHtml');
const { createLmTools, registerLmTools } = require('../src/lmTools');
const { renderConstraintHtml } = require('../src/constraintHtml');
const { ReportsPanel } = require('../src/reportsPanel');
const { appendLog, flushLogSync } = require('../src/launchCheck');
const writeService = require('../src/writeService');

// ------------------------------------------------------------------ tiny test harness

const tests = [];

function test(name, fn) {
    tests.push({ name, fn });
}

function eq(actual, expected, what) {
    if (actual !== expected) {
        throw new Error(`${what || 'value'}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

function ok(condition, what) {
    if (!condition) throw new Error(what || 'expected a truthy value');
}

function includes(haystack, needle, what) {
    const text = String(haystack);
    if (!text.includes(needle)) {
        throw new Error(`${what || 'expected text'} ${JSON.stringify(needle)} — got ${JSON.stringify(text.slice(0, 300))}`);
    }
}

function excludes(haystack, needle, what) {
    if (String(haystack).includes(needle)) {
        throw new Error(`${what || 'unexpected text'} ${JSON.stringify(needle)}`);
    }
}

// ------------------------------------------------------------------ sample payloads

function sampleModuleTree() {
    return JSON.parse(JSON.stringify({
        ok: true,
        project: 'demo_project',
        al: '%WORKSPACE%/CorpusAlpha/demo_project.al',
        root: {
            module: 'top',
            instance: '',
            file: '%WORKSPACE%/CorpusAlpha/src/top.v',
            range: { start: { line: 10, character: 7 }, end: { line: 10, character: 10 } },
            defined: true,
            blackbox: false,
            recursive: false,
            portCount: 25,
            usedAt: null,
            children: [
                {
                    module: 'pll_gen',
                    instance: 'pll_gen',
                    file: '%WORKSPACE%/CorpusAlpha/src/top.v',
                    range: { start: { line: 64, character: 9 }, end: { line: 64, character: 16 } },
                    defined: false,
                    blackbox: true,
                    recursive: false,
                    portCount: 0,
                    usedAt: {
                        uri: 'file:///%WORKSPACE%/CorpusAlpha/src/top.v',
                        range: { start: { line: 65, character: 0 }, end: { line: 65, character: 7 } },
                    },
                    children: [],
                },
                {
                    module: 'pwm_gen',
                    instance: 'u_pwm',
                    file: '%WORKSPACE%/CorpusAlpha/src/system/pwm_gen.v',
                    range: { start: { line: 3, character: 4 }, end: { line: 3, character: 11 } },
                    defined: true,
                    blackbox: false,
                    recursive: false,
                    portCount: 11,
                    usedAt: {
                        uri: 'file:///%WORKSPACE%/CorpusAlpha/src/top.v',
                        range: { start: { line: 70, character: 1 }, end: { line: 70, character: 6 } },
                    },
                    children: [
                        {
                            module: 'counter',
                            instance: 'u_cnt',
                            file: '%WORKSPACE%/CorpusAlpha/src/system/counter.v',
                            range: { start: { line: 1, character: 7 }, end: { line: 1, character: 12 } },
                            defined: true,
                            blackbox: false,
                            recursive: false,
                            portCount: 4,
                            usedAt: {
                                uri: 'file:///%WORKSPACE%/CorpusAlpha/src/system/pwm_gen.v',
                                range: { start: { line: 9, character: 2 }, end: { line: 9, character: 7 } },
                            },
                            children: [],
                        },
                    ],
                },
                {
                    module: 'EF2_LUT4',
                    instance: 'u_lut',
                    file: '%WORKSPACE%/CorpusAlpha/src/top.v',
                    range: { start: { line: 80, character: 5 }, end: { line: 80, character: 10 } },
                    defined: false,
                    blackbox: false,
                    recursive: false,
                    portCount: 0,
                    usedAt: null,
                    children: [],
                },
            ],
        },
        modulesSeen: 20,
        truncated: false,
    }));
}

function sampleReports() {
    return {
        ok: true,
        generatedAt: 1761800000,
        flow: { running: true, stage: 'phy', elapsed: 12.3, lastMessage: 'Routing net 1234' },
        artifacts: {
            bit: { path: '%WORKSPACE%/CorpusAlpha/prj/top.bit', exists: true, size: 1234, mtime: 1761800000 },
            syn_db: { path: '%WORKSPACE%/CorpusAlpha/prj/syn.db', exists: true },
            route_db: { path: '%WORKSPACE%/CorpusAlpha/prj/route.db', exists: true },
            runs: [{ name: 'syn_1', dir: '%WORKSPACE%/CorpusAlpha/prj/syn_1', exists: true }],
        },
        qor: {
            step: 'route',
            metrics: { 'Setup WNS': '4388', 'Slice Utilization': '12.3%' },
            suggestions: [{
                id: 'QOR-1302',
                priority: 'MEDIUM',
                text: '- Route HWNS -2649ps smaller than 0, do not meet timing.',
            }],
        },
        timing: {
            summary: { SWNS: 4.388, STNS: 0, HWNS: 1.2, HTNS: 0, status: 'Met' },
            clocks: [{ name: 'clk_input', period: 31.25, fmax: 45.1 }],
            paths: [{ slack: 4.388, from: 'ff_a/Q', to: 'ff_b/D' }],
        },
        area: { rows: [{ resource: '#lut6', used: 1234, total: 45000, utilization: '2.7%' }] },
        flowStatus: [
            { step: 'Optimize RTL', status: 'Finished', elapsed: '12.3s' },
            { step: 'Place', status: 'Running', elapsed: '5s' },
        ],
        files: {
            qor: '%WORKSPACE%/CorpusAlpha/prj/route.qor',
            timing: '%WORKSPACE%/CorpusAlpha/prj/final_timing.rpt',
            area: '%WORKSPACE%/CorpusAlpha/prj/route.area',
        },
    };
}

const sampleInfo = {
    ok: true,
    project: 'demo_project',
    family: 'EG4',
    device: 'EG4S20BG256',
    top: 'top',
    al: '%WORKSPACE%/CorpusAlpha/demo_project.al',
};

// 0.19.3：后端 probeStatus 的真实形状（字段来自 src/flow/probe.cpp::Status）：
// watcher 里才有 present/clock/sample_depth/net_count，disabled[] 是 0.19.3+ 新加的。
function sampleProbeStatus() {
    return {
        ok: true,
        ready: true,
        project: 'demo_project',
        target: 'EG4S20BG256',
        matches_user_mdn: true,
        reason: 'ready to capture on reg_init/clk',
        chip_debugger: {
            path: '%WORKSPACE%/CorpusAlpha/chip_debugger/user.mdn',
            exists: true,
            count: 3,
            signals: ['WHG/b[1]', 'WHG/b[0]', 'WHG/a[7]'],
        },
        watcher: {
            present: true,
            instance: 'auto_created_cwc1',
            clock: 'reg_init/clk',
            sample_depth: 1024,
            net_count: 3,
            stale: false,
            // 真实 compiled.cwc 的 bus 名不带顶层前缀（`e2e_cnt2l[15:0]`），
            // user.mdn 里是带前缀的逐位名（`WHG/e2e_cnt2l[0]`）—— 树必须按叶子名合并。
            buses: [
                { name: 'b[1:0]', clock: 'reg_init/clk', type: 'MARK_DEBUG_PROBE', enabled: true },
                { name: 'a[7:7]', clock: 'reg_init/clk', type: 'MARK_DEBUG_PROBE', enabled: true },
            ],
        },
    };
}

// ------------------------------------------------------------------ module tree tests

test('module tree: nested payload builds depth, blackbox and primitive icons', async () => {
    const provider = new ModuleTreeProvider({ request: async () => sampleModuleTree() });
    const view = mock.window.createTreeView('fpgaLsp.modulesView', { treeDataProvider: provider });
    provider.attachView(view);
    await provider.refresh();

    eq(view.message, undefined, 'view.message when payload ok');

    const roots = provider.getChildren();
    eq(roots.length, 1, 'root count');
    const rootItem = provider.getTreeItem(roots[0]);
    eq(rootItem.label, 'top');
    eq(rootItem.collapsibleState, mock.TreeItemCollapsibleState.Expanded, 'root auto-expanded');
    eq(rootItem.contextValue, 'fpgaModuleRoot');

    const level1 = provider.getChildren(roots[0]);
    eq(level1.length, 3, 'level-1 modules');

    const pllItem = provider.getTreeItem(level1[0]);
    eq(pllItem.label, 'pll_gen');
    eq(pllItem.description, 'pll_gen');
    eq(pllItem.contextValue, 'fpgaModuleBlackbox');
    eq(pllItem.iconPath.id, 'circle-slash');
    includes(pllItem.tooltip, '%WORKSPACE%/CorpusAlpha/src/top.v:65');
    includes(pllItem.tooltip, 'ports: 0');
    includes(pllItem.tooltip, 'blackbox');

    const pwmItem = provider.getTreeItem(level1[1]);
    eq(pwmItem.label, 'pwm_gen');
    eq(pwmItem.description, 'u_pwm');
    eq(pwmItem.contextValue, 'fpgaModuleDefined');
    eq(pwmItem.iconPath.id, 'symbol-class');

    const primitiveItem = provider.getTreeItem(level1[2]);
    eq(primitiveItem.label, 'EF2_LUT4');
    eq(primitiveItem.iconPath.id, 'symbol-constant', 'primitive icon');

    const level2 = provider.getChildren(level1[1]);
    eq(level2.length, 1, 'level-2 modules');
    eq(provider.getTreeItem(level2[0]).label, 'counter');
    eq(provider.getChildren(level2[0]).length, 0);
    eq(provider.getTreeItem(level2[0]).collapsibleState, mock.TreeItemCollapsibleState.None);

    const emptyProvider = new ModuleTreeProvider({ request: async () => ({ ok: false }) });
    const emptyView = mock.window.createTreeView('fpgaLsp.modulesView', { treeDataProvider: emptyProvider });
    emptyProvider.attachView(emptyView);
    await emptyProvider.refresh();
    includes(emptyView.message, 'no Anlogic .al project found');
    eq(emptyProvider.getChildren().length, 0);
});

test('module tree: request errors show the friendly empty state', async () => {
    const provider = new ModuleTreeProvider({
        request: async () => {
            throw new Error('ECONNREFUSED');
        },
    });
    const view = mock.window.createTreeView('fpgaLsp.modulesView', { treeDataProvider: provider });
    provider.attachView(view);
    eq(view.message, 'Waiting for the FPGA language server…', 'initial message');
    await provider.refresh();
    includes(view.message, 'no Anlogic .al project found');
    includes(view.message, 'ECONNREFUSED');
    eq(provider.getChildren().length, 0);
});

test('module tree: refresh sends the optional document uri and click opens the definition', async () => {
    const calls = [];
    const provider = new ModuleTreeProvider({
        request: async (method, params) => {
            calls.push({ method, params });
            return sampleModuleTree();
        },
    });
    await provider.refresh('file:///%WORKSPACE%/CorpusAlpha/src/top.v');
    eq(calls.length, 1);
    eq(calls[0].method, 'fpgaLsp/moduleTree');
    eq(calls[0].params.uri, 'file:///%WORKSPACE%/CorpusAlpha/src/top.v');

    const root = provider.getChildren()[0];
    const rootTarget = provider.getTreeItem(root).command.arguments[0];
    eq(provider.getTreeItem(root).command.command, 'fpgaLsp.openModuleNode');
    eq(rootTarget.where, 'definition');
    eq(rootTarget.uri.fsPath, '%WORKSPACE%/CorpusAlpha/src/top.v');
    eq(rootTarget.range.start.line, 10);
    eq(rootTarget.range.start.character, 7);
});

test('module tree: defined -> definition, blackbox -> usedAt, top -> no instantiation', () => {
    const payload = sampleModuleTree();
    const top = payload.root;
    const pll = top.children[0];
    const pwm = top.children[1];

    const definition = resolveTarget(pwm);
    eq(definition.where, 'definition');
    eq(definition.uri.fsPath, '%WORKSPACE%/CorpusAlpha/src/system/pwm_gen.v');
    eq(definition.range.start.line, 3);
    eq(definition.range.start.character, 4);

    const click = resolveTarget(pll);
    eq(click.where, 'instantiation');
    eq(click.uri.fsPath, '%WORKSPACE%/CorpusAlpha/src/top.v');
    eq(click.range.start.line, 65);
    eq(click.range.start.character, 0);

    eq(definitionTarget(pll), undefined, 'blackbox has no definition target');
    eq(instantiationTarget(pll).range.start.line, 65);
    eq(instantiationTarget(top), undefined, 'top module has no usedAt');
});

// ------------------------------------------------------------------ reports webview

test('reports webview: renders QoR/timing/area/flow metrics and report-file links', () => {
    const html = renderReportsHtml({ reports: sampleReports(), nonce: 'nonce123' });
    for (const needle of [
        'FPGA Reports',
        'tab-qor', 'tab-timing', 'tab-area', 'tab-flow',
        'Run Synthesis', 'Run Full Flow', 'Refresh',
        'Setup WNS', '4388', 'Slice Utilization', 'QOR-1302', 'QOR-1302',
        'SWNS', 'STNS', 'HWNS', 'HTNS', '4.388', 'clk_input', 'ff_a/Q',
        '#lut6', '1234', '45000', 'Optimize RTL', 'Finished',
        'route.qor', 'final_timing.rpt', 'route.area',
        'command:fpgaLsp.openReportFile?', 'command:fpgaLsp.openArtifact?',
        'nonce="nonce123"',
    ]) {
        includes(html, needle);
    }
    includes(html, '<code class="qor-id">QOR-1302</code>');
});

test('reports webview: escapes report text (no XSS from report files)', () => {
    const reports = sampleReports();
    reports.qor.metrics['Injected <b>metric</b>'] = '<script>alert(1)</script>';
    reports.qor.suggestions[0].text = 'bad <img src=x onerror=alert(2)> and <script>alert(3)</script> QOR-9999';
    reports.files.qor = 'Y:/x/<script>alert(4)</script>.qor';
    const html = renderReportsHtml({ reports, nonce: 'n1' });

    excludes(html, '<script>alert', 'raw script payload');
    excludes(html, '<img src=x', 'raw img payload');
    excludes(html, '<b>metric</b>', 'raw html metric name');
    includes(html, '&lt;script&gt;alert(1)&lt;/script&gt;');
    includes(html, '&lt;img src=x onerror=alert(2)&gt;');
    includes(html, '<code class="qor-id">QOR-9999</code>');
});

test('reports webview: tolerates the real reports.py shapes (array metrics, clocks, flowStatus.steps)', () => {
    const reports = sampleReports();
    reports.qor.metrics = [
        { group: 'Timing', key: 'Setup WNS', value: '3828ps' },
        { group: 'Timing', key: 'Hold WNS', value: '57ps' },
    ];
    reports.timing.clocks = [{ name: 'clk0', freq_mhz: 64.0, fanout: 1234, source: 'clk_input' }];
    reports.timing.paths = [{ slack: -0.326, begin: 'u_a/Q', end: 'u_b/D', check: 'Setup' }];
    reports.flowStatus = {
        setup: { wns_ps: 3828, tns_ps: 0, feps: 0 },
        hold: { wns_ps: 57, tns_ps: 0, feps: 0 },
        mode: 'Normal',
        steps: [{ name: 'opt_rtl', stage: 'syn', calls: 1, wall: '12.3s', cpu: '12.1s', mem_mb: 500, peak_mb: 640 }],
    };
    const html = renderReportsHtml({ reports, nonce: 'n3' });
    includes(html, 'Timing / Setup WNS');
    includes(html, '3828ps');
    includes(html, '1234');
    includes(html, 'clk_input');
    includes(html, 'u_a/Q');
    includes(html, 'u_b/D');
    includes(html, 'opt_rtl');
    includes(html, '12.1s');
    includes(html, '500 MB');
    includes(html, 'Normal');
});

test('reports webview: "Run a flow first" empty state when ok=false', () => {
    const html = renderReportsHtml({ reports: { ok: false, error: 'no flow has run' }, nonce: 'n2' });
    includes(html, 'Run a flow first');
    includes(html, 'no flow has run');
    excludes(html, 'tab-qor', 'tabs hidden in empty state');
});

test('reports panel: single reusable "FPGA Reports" panel with working buttons', async () => {
    const runs = [];
    const panel = new ReportsPanel({
        rpc: { request: async () => sampleReports() },
        onRunFlow: (stages) => runs.push(stages),
    });
    await panel.show();
    eq(mock.__state.webviewPanels.length, 1, 'one panel');
    const created = mock.__state.webviewPanels[0];
    eq(created.viewType, 'fpgaLsp.reports.syn');
    eq(created.title, 'Synthesis Reports');
    includes(created.webview.html, 'Setup WNS');
    includes(created.webview.html, 'QOR-1302');
    eq(created.webview.options.enableScripts, true);
    eq(created.webview.options.enableCommandUris, true);
    eq(created.webview.options.retainContextWhenHidden, true, 'webview context is retained');

    created.webview.__receive({ type: 'run', stages: ['syn'] });
    eq(runs.length, 1);
    eq(runs[0][0], 'syn');

    await panel.show();
    eq(mock.__state.webviewPanels.length, 1, 'panel is reused');
    created.webview.__receive({ type: 'refresh' });
    eq(mock.__state.webviewPanels.length, 1);

    panel.dispose();
    eq(created.disposed, true);
});

// ------------------------------------------------------------------ flow tree

test('flow tree: running flowStatus, stages, artifacts and report files', async () => {
    const requested = [];
    const provider = new FlowTreeProvider(
        { request: async (method) => { requested.push(method); return sampleReports(); } },
        async () => sampleInfo
    );
    const view = mock.window.createTreeView('fpgaLsp.flowView', { treeDataProvider: provider });
    provider.attachView(view);
    await provider.refresh();

    ok(requested.includes('fpgaLsp/reports'), 'fpgaLsp/reports requested');
    eq(view.message, undefined, 'no empty-state message');

    const sections = provider.getChildren();
    eq(sections.length, 3, 'section count');
    eq(
        sections.map((section) => section.id).join(','),
        'section:project,section:flow,section:artifacts'
    );

    const projectSection = sections.find((section) => section.id === 'section:project');
    const projectText = provider.getChildren(projectSection)
        .map((node) => `${node.label}=${node.description}`).join('|');
    includes(projectText, 'Project=demo_project');
    includes(projectText, 'Device=EG4S20BG256');
    includes(projectText, 'Top module=top');

    const flowSection = sections.find((section) => section.id === 'section:flow');
    includes(flowSection.description, 'running');
    includes(flowSection.description, 'phy');
    includes(flowSection.description, '12.3');

    const stageNodes = provider.getChildren(flowSection)
        .flatMap((node) => (node.kind === 'section' ? provider.getChildren(node) : [node]));
    const finished = stageNodes.find((node) => node.kind === 'stage' && node.label === 'Optimize RTL');
    ok(finished, 'stage row present');
    includes(finished.description, 'Finished');
    includes(finished.description, '12.3s');
    eq(provider.getTreeItem(finished).iconPath.id, 'pass');
    const lastMessage = stageNodes.find((node) => node.id === 'flow:last');
    eq(lastMessage.label, 'Routing net 1234');
    includes(lastMessage.description, 'elapsed 12.3s');

    const artifactSection = sections.find((section) => section.id === 'section:artifacts');
    const artifactNodes = provider.getChildren(artifactSection);
    const bit = artifactNodes.find((node) => node.id === 'artifact:bit');
    ok(bit, 'bit artifact row present');
    const bitItem = provider.getTreeItem(bit);
    eq(bitItem.command.command, 'fpgaLsp.openArtifact');
    eq(bitItem.command.arguments[0], '%WORKSPACE%/CorpusAlpha/prj/top.bit');
    includes(bitItem.description, '%WORKSPACE%/CorpusAlpha/prj/top.bit');
    includes(bitItem.description, '1.2 KB');
    includes(bitItem.tooltip, '%WORKSPACE%/CorpusAlpha/prj/top.bit');
    eq(artifactNodes.filter((node) => node.id.indexOf('artifact:run:') === 0).length, 1, 'run row');

    provider.applyFlowStatus({ ok: true, flow: { running: false, stage: 'done', elapsed: 42, lastMessage: 'bitstream saved' } });
    const doneSection = provider.getChildren().find((section) => section.id === 'section:flow');
    includes(doneSection.description, 'done');
    includes(doneSection.description, 'bitstream saved');
    ok(
        provider.getChildren(doneSection).some((node) => node.kind === 'message' && node.label === 'bitstream saved'),
        'last flow message updated'
    );
});

test('flow tree: pushed flowEvent updates the summary', async () => {
    const provider = new FlowTreeProvider({ request: async () => sampleReports() }, async () => undefined);
    await provider.refresh();
    provider.applyFlowEvent({ runId: 'r1', type: 'progress', stage: 'syn', elapsed: 3.5, message: 'Optimize RTL' });
    const running = provider.getChildren().find((section) => section.id === 'section:flow');
    includes(running.description, 'running');
    includes(running.description, 'syn');
    includes(running.description, '3.5');
    provider.applyFlowEvent({ runId: 'r1', type: 'done', ok: true, stage: 'syn', elapsed: 9.1, message: 'bitstream saved' });
    const done = provider.getChildren().find((section) => section.id === 'section:flow');
    includes(done.description, 'done');
    includes(done.description, 'bitstream saved');
});

test('flow tree: backend flow.steps renders the six GUI steps with wall times', async () => {
    // 后端 `fpgaLsp/flowStatus` 的 flow.steps 形状（wall/cpu 单位是秒）：
    // syn_1 的三步跑完、phy_1 的三步还没开始 —— 与 TD GUI 的 6 步一一对应。
    const flowStepsPayload = [
        { name: 'Read Design', stage: 'design', status: 'done', wall: 2, cpu: 1, mem_mb: 577, run: 'syn_1' },
        { name: 'Optimize RTL', stage: 'rtl', status: 'done', wall: 40, cpu: 46, mem_mb: 907, run: 'syn_1' },
        { name: 'Optimize Gate', stage: 'gate', status: 'done', wall: 71, cpu: 238, mem_mb: 1125, run: 'syn_1' },
        { name: 'Optimize Placement', stage: 'place', status: 'running', wall: 0, cpu: 0, mem_mb: 0, run: 'phy_1' },
        { name: 'Optimize Routing', stage: 'route', status: 'failed', wall: 0, cpu: 0, mem_mb: 0, run: 'phy_1' },
        { name: 'Generate Bitstream', stage: 'bitgen', status: 'pending', wall: 0, cpu: 0, mem_mb: 0, run: 'phy_1' },
    ];
    const provider = new FlowTreeProvider(
        {
            request: async (method) => {
                if (method === 'fpgaLsp/flowStatus') {
                    return {
                        ok: true,
                        flow: {
                            running: false,
                            stage: 'done',
                            elapsed: 113,
                            lastMessage: 'syn_1 finished',
                            steps: flowStepsPayload,
                        },
                    };
                }
                return sampleReports();
            },
        },
        async () => sampleInfo
    );
    await provider.refresh();

    const flowSection = provider.getChildren().find((section) => section.id === 'section:flow');
    // 父节点仍然带总耗时
    includes(flowSection.description, 'done');
    includes(flowSection.description, '113s');

    const groups = provider.getChildren(flowSection).filter((node) => node.kind === 'section');
    eq(groups.length, 2, 'syn / phy groups');
    const nodes = groups.flatMap((group) => provider.getChildren(group).filter((node) => node.kind === 'stage'));
    eq(nodes.length, 6, 'six GUI steps');
    eq(
        nodes.map((node) => node.label).join('|'),
        'Read Design · 2s|Optimize RTL · 40s|Optimize Gate · 1m11s|'
        + 'Optimize Placement|Optimize Routing|Generate Bitstream',
        'step labels carry the wall time'
    );
    // 状态用图标 + 描述区分
    eq(provider.getTreeItem(nodes[0]).iconPath.id, 'pass', 'done icon');
    eq(provider.getTreeItem(nodes[3]).iconPath.id, 'sync~spin', 'running icon');
    eq(provider.getTreeItem(nodes[4]).iconPath.id, 'error', 'failed icon');
    eq(provider.getTreeItem(nodes[5]).iconPath.id, 'circle-outline', 'pending icon');
    includes(nodes[2].description, 'done');
    includes(nodes[2].description, 'cpu 3m58s');   // 238 s
    includes(nodes[2].description, '1125 MB');
    // 后端对没跑过的步骤给 status:"pending"；0.19.3 起树里统一显示成 TD GUI 的 "not run"。
    includes(nodes[5].description, 'not run');
    // 组上的小结：3 步 + 该 run 的累计耗时
    includes(groups[0].description, '3 steps');
    includes(groups[0].description, '1m53s');      // 2 + 40 + 71 = 113 s

    // 0.19.3: 没有 flow 运行时（后端没回 steps / 旧服务器）也必须按 TD GUI 六步渲染，
    // 缺的步骤统一说明成 "not run"，而不是只给两个空分组。
    const idleProvider = new FlowTreeProvider(
        { request: async () => ({ ok: false, error: 'no flow data' }) },
        async () => sampleInfo
    );
    await idleProvider.refresh();
    const idleFlow = idleProvider.getChildren().find((section) => section.id === 'section:flow');
    ok(idleFlow, 'flow section present without a run');
    const idleGroups = idleProvider.getChildren(idleFlow).filter((node) => node.kind === 'section');
    eq(idleGroups.length, 2, 'syn / phy groups without a run');
    const idleSteps = idleGroups.flatMap((group) => idleProvider.getChildren(group));
    eq(idleSteps.length, 6, 'six GUI steps without a run');
    eq(
        idleSteps.map((node) => node.label).join('|'),
        'Read Design|Optimize RTL|Optimize Gate|Optimize Placement|Optimize Routing|Generate Bitstream',
        'canonical TD GUI step names'
    );
    eq(idleSteps.map((node) => node.status).join(','), 'not run,not run,not run,not run,not run,not run',
        'every idle step says not run');
    includes(idleProvider.getTreeItem(idleSteps[0]).tooltip, 'Read Design');
    eq(idleGroups[0].description, 'not run', 'idle syn group summary');
    eq(idleGroups[1].description, 'not run', 'idle phy group summary');
    // 后端对「没跑过」给的 status 是 pending；树里统一显示成 TD GUI 的 not run。
    eq(idleProvider.getTreeItem(idleSteps[5]).iconPath.id, 'circle-outline', 'idle step icon');
});

test('flow tree: tolerates reports.py flowStatus object with steps', async () => {
    const reports = sampleReports();
    reports.flowStatus = {
        steps: [{ name: 'opt_rtl', stage: 'syn', wall: '12.3s', cpu: '12.1s', mem_mb: 500 }],
    };
    const provider = new FlowTreeProvider({ request: async () => reports }, async () => sampleInfo);
    await provider.refresh();
    const flowSection = provider.getChildren().find((section) => section.id === 'section:flow');
    const stage = provider.getChildren(flowSection)
        .flatMap((node) => (node.kind === 'section' ? provider.getChildren(node) : [node]))
        .find((node) => node.kind === 'stage' && node.label === 'opt_rtl');
    ok(stage, 'stage row from flowStatus.steps');
    includes(stage.description, 'syn');
    includes(stage.description, '12.3s');
});

test('flow tree: friendly empty state when neither project nor reports exist', async () => {
    const provider = new FlowTreeProvider(
        { request: async () => ({ ok: false, error: 'no reports' }) },
        async () => undefined
    );
    const view = mock.window.createTreeView('fpgaLsp.flowView', { treeDataProvider: provider });
    provider.attachView(view);
    await provider.refresh();
    eq(view.message, 'No Anlogic .al project or flow data found.', 'empty message');
    eq(provider.getChildren().length, 0);
});

test('extension: activation wires views, commands, reports panel and flow events', async () => {
    const path = require('path');
    FakeLanguageClient.responses = {
        'fpgaLsp/moduleTree': async () => sampleModuleTree(),
        'fpgaLsp/reports': async () => sampleReports(),
        'workspace/executeCommand': async (payload) => {
            if (payload && payload.command === 'fpgalsp.info') return sampleInfo;
            if (payload && payload.command === 'fpgalsp.probeStatus') return sampleProbeStatus();
            if (payload && payload.command === 'fpgalsp.probeConfig') {
                const options = (payload.arguments && payload.arguments[0]) || {};
                // 0.19.3+ 后端认识 disabled[]，并把它回显出来（旧后端没有这个字段）。
                return {
                    ok: true,
                    count: (options.signals || []).length,
                    disabled: options.disabled || [],
                    enabled_bus_count: 1,
                    enabled_bit_count: 1,
                };
            }
            if (payload && payload.command === 'fpgalsp.runFlow') {
                return { ok: true, runId: 'r1', stages: payload.arguments[0].stages };
            }
            if (payload && payload.command === 'fpgalsp.cancelFlow') return { ok: true };
            return { ok: true };
        },
    };

    // extension.js is loaded lazily so the mocked vscode-languageclient above is used.
    const extension = require('../extension.js');
    const globalStateStore = new Map();
    const context = {
    extensionPath: path.join(__dirname, '..'),
    subscriptions: [],
    workspaceState: { get: () => undefined, update: () => Promise.resolve() },
    globalState: {
        get: (key) => globalStateStore.get(key),
        update: async (key, value) => { globalStateStore.set(key, value); },
    },
};
    extension.activate(context);
    await new Promise((resolve) => setTimeout(resolve, 40));

    eq(mock.__state.treeViews.length, 3, 'tree view count');
    eq(mock.__state.treeViews[0].viewId, 'fpgaLsp.modulesView');
    eq(mock.__state.treeViews[1].viewId, 'fpgaLsp.flowView');

    // 0.19.3: ChipWatcher is a native TreeView (registerTreeDataProvider), NOT a
    // bottom-panel webview view. This is the whole point of the change: a tree view
    // cannot get stuck on "no data provider registered for the view".
    eq(Object.prototype.hasOwnProperty.call(mock.__state.webviewViewProviders, 'fpgaLsp.probeView'), false,
        'no webview view provider for ChipWatcher');
    const probeProvider = mock.__state.treeDataProviders['fpgaLsp.probeView'];
    ok(probeProvider, 'ChipWatcher tree data provider registered');
    await probeProvider.refresh();
    const probeRoot = probeProvider.getChildren();
    eq(probeRoot.map((node) => node.id).join(','), 'probe:status,probe:signals,probe:actions',
        'ChipWatcher root rows');

    const probeStatusItem = probeProvider.getTreeItem(probeRoot[0]);
    includes(probeStatusItem.label, 'ready');
    eq(probeStatusItem.iconPath.id, 'check', 'ready status icon');
    includes(probeStatusItem.description, 'clock reg_init/clk');
    includes(probeStatusItem.description, 'depth 1024');
    includes(probeStatusItem.description, '3 nets');

    const busNodes = probeProvider.getChildren(probeRoot[1]);
    eq(busNodes.map((node) => node.label).join(','), 'WHG/a[7],WHG/b[1:0]',
        'signals grouped into buses from user.mdn');
    const busNode = busNodes.find((node) => node.label === 'WHG/b[1:0]');
    const busItem = probeProvider.getTreeItem(busNode);
    eq(busItem.iconPath.id, 'check', 'enabled bus icon');
    includes(busItem.description, 'width 2');
    eq(busItem.command.command, 'fpgaLsp.probeToggleSignal');
    eq(busItem.command.arguments[0], 'WHG/b');
    const actionNodes = probeProvider.getChildren(probeRoot[2]);
    ok(actionNodes.some((node) => node.id === 'probe:action:capture'), 'Capture now action');
    ok(actionNodes.some((node) => node.id === 'probe:action:add'), 'Add signals action');

    // 点击总线 -> disabled 列表进 globalState + 后端 probeConfig（0.19.3+ 回显 disabled[]）。
    // 总线级 toggle 会展开成逐位名字交给后端（后端 ResolveDisabled 的逐位语义最稳）。
    await mock.commands.executeCommand('fpgaLsp.probeToggleSignal', 'WHG/b');
    eq((globalStateStore.get('fpgaLsp.probe.disabled') || []).join(','), 'WHG/b[1],WHG/b[0]',
        'disabled list persisted in globalState');
    const toggledBus = probeProvider.getChildren()[1].children.find((node) => node.label === 'WHG/b[1:0]');
    eq(probeProvider.getTreeItem(toggledBus).iconPath.id, 'circle-slash', 'disabled bus icon');
    includes(probeProvider.getTreeItem(toggledBus).description, 'disabled');
    ok(mock.__state.messages.info.some((entry) => /disabled/.test(String(entry.message))),
        'toggle result reported');

    for (const entry of require('../package.json').contributes.commands) {
        ok(mock.__state.commands.has(entry.command), `command registered: ${entry.command}`);
    }

    const moduleProvider = mock.__state.treeViews[0].treeDataProvider;
    eq(moduleProvider.getChildren().length, 1, 'module tree loaded');
    const rootNode = moduleProvider.getChildren()[0];
    const rootItem = moduleProvider.getTreeItem(rootNode);
    eq(rootItem.command.command, 'fpgaLsp.openModuleNode');
    await mock.commands.executeCommand(rootItem.command.command, ...rootItem.command.arguments);
    eq(mock.__state.shownDocuments.length, 1, 'module click opened a document');
    eq(mock.__state.shownDocuments[0].document.uri.fsPath, '%WORKSPACE%/CorpusAlpha/src/top.v');

    await mock.commands.executeCommand('fpgaLsp.showReports');
    eq(mock.__state.webviewPanels.length, 1, 'reports panel created');
    includes(mock.__state.webviewPanels[0].webview.html, 'Setup WNS');

    const runPromise = mock.commands.executeCommand('fpgaLsp.runSyn');
    await new Promise((resolve) => setTimeout(resolve, 40));
    const clientInstance = FakeLanguageClient.instance;
    ok(clientInstance, 'language client created');
    const handler = clientInstance.notificationHandlers['fpgaLsp/flowEvent'];
    ok(handler, 'flowEvent handler registered');
    handler({ runId: 'r1', type: 'done', ok: true, stage: 'syn', elapsed: 1, message: 'bitstream saved' });
    await runPromise;
    await new Promise((resolve) => setTimeout(resolve, 20));

    // Phase 2: the index-changed notification drives a targeted in-place tree update.
    const indexHandler = clientInstance.notificationHandlers['fpgaLsp/indexChanged'];
    ok(indexHandler, 'indexChanged handler registered');
    const beforeRequests = FakeLanguageClient.requests.filter((entry) => entry.method === 'fpgaLsp/moduleTree').length;
    indexHandler({
        al: '%WORKSPACE%/CorpusAlpha/demo_project.al',
        paths: ['%WORKSPACE%/CorpusAlpha/src/top.v'],
        modules: [{ module: 'pll_gen', portCount: 9 }],
    });
    const afterRequests = FakeLanguageClient.requests.filter((entry) => entry.method === 'fpgaLsp/moduleTree').length;
    eq(afterRequests, beforeRequests, 'in-place index update does not re-request the tree');
    eq(moduleProvider.getChildren(moduleProvider.getChildren()[0])
        .find((node) => node.raw.module === 'pll_gen').raw.portCount, 9, 'tree node patched in place');
    // A foreign-project event is ignored (no refresh, no request).
    indexHandler({ paths: ['X:/elsewhere/src/top.v'] });
    await new Promise((resolve) => setTimeout(resolve, 200));
    eq(FakeLanguageClient.requests.filter((entry) => entry.method === 'fpgaLsp/moduleTree').length,
        afterRequests, 'foreign event ignored');

    // D2: `fpgaLsp/reportsChanged` must repaint only a *visible* panel, and only once per burst.
    const reportsHandler = clientInstance.notificationHandlers['fpgaLsp/reportsChanged'];
    ok(reportsHandler, 'reportsChanged handler registered');
    const countReports = () => FakeLanguageClient.requests
        .filter((entry) => entry.method === 'fpgaLsp/reports').length;
    const visibleBefore = countReports();
    reportsHandler({ stage: 'all', sections: ['qor'] });
    reportsHandler({ stage: 'all', sections: ['qor'] });
    reportsHandler({ stage: 'all', sections: ['qor'] });
    await new Promise((resolve) => setTimeout(resolve, 300));  // > panel debounce (150 ms)
    const visibleDelta = countReports() - visibleBefore;
    ok(visibleDelta >= 1, `visible panel refreshed (${visibleDelta} request(s))`);
    ok(visibleDelta <= 2, `one debounced refresh, not three (${visibleDelta} request(s))`);

    // With no visible panel the event must not touch the network at all (cache is still dropped).
    for (const webviewPanel of mock.__state.webviewPanels) webviewPanel.dispose();
    const hiddenBefore = countReports();
    reportsHandler({ stage: 'all', sections: ['qor'] });
    reportsHandler({});
    await new Promise((resolve) => setTimeout(resolve, 300));
    // S1: the status bar also refreshes its WNS summary on reportsChanged, so one extra
    // (single-flight, shared) request is expected. What must NOT happen is a per-event burst.
    ok(countReports() - hiddenBefore <= 1, 'no panel repaint when nothing is visible');

    const flowChannel = mock.__state.outputChannels.find((channel) => channel.name === 'FPGA Flow');
    ok(flowChannel, 'FPGA Flow output channel exists');
    ok(flowChannel.lines.some((line) => line.includes('done')), 'flow done logged');
    // S1: multiple status items now exist (project/WNS + flow); assert that one shows done.
    ok(mock.__state.statusBarItems.some((item) => /done/.test(item.text)), 'status bar updated');
    // S1: the project status line must show the project/device from fpgalsp.info, not just "idle".
    const projectStatus = mock.__state.statusBarItems.find((item) => /^FPGA: /.test(item.text || ''));
    ok(projectStatus, 'a project status bar item exists');
    ok(projectStatus.text.includes(sampleInfo.project),
        'status bar shows the project name (got: ' + projectStatus.text + ')');
    ok(projectStatus.text.includes(sampleInfo.device),
        'status bar shows the device (got: ' + projectStatus.text + ')');

    await extension.deactivate();
});

test('manifest: every command used by the client is declared and registered', () => {
    const fs = require('fs');
    const path = require('path');
    test('reports tree: separate syn/phy groups with report files and open commands', async () => {
    const sample = {
        ok: true,
        syn: { ok: true, files: { qor: 'Y:/p/syn/gate.qor', timing: 'Y:/p/syn/gate.timing' } },
        phy: { ok: true, files: { qor: 'Y:/p/phy/route.qor', area: 'Y:/p/phy/route.area' } },
    };
    const provider = new ReportsTreeProvider({ request: async () => sample });
    const view = mock.window.createTreeView('fpgaLsp.reportsView', { treeDataProvider: provider });
    provider.attachView(view);
    await provider.refresh();
    const sections = provider.getChildren();
    eq(sections.map((section) => section.id).join(','), 'reports:section:syn,reports:section:phy');
    const synNodes = provider.getChildren(sections[0]);
    eq(synNodes[0].kind, 'action', 'open action first');
    eq(provider.getTreeItem(synNodes[0]).command.command, 'fpgaLsp.showSynReports');
    const synQor = synNodes.find((node) => node.id === 'report:syn:qor');
    ok(synQor, 'syn qor row');
    eq(provider.getTreeItem(synQor).command.command, 'fpgaLsp.openReportFile');
    const phyNodes = provider.getChildren(sections[1]);
    eq(provider.getTreeItem(phyNodes[0]).command.command, 'fpgaLsp.showPhyReports');
    const phyArea = phyNodes.find((node) => node.id === 'report:phy:area');
    ok(phyArea, 'phy area row');
    eq(provider.getTreeItem(phyArea).command.arguments[0], 'Y:/p/phy/route.area');
});

test('mcp config: builds stdio definition and portable/VS Code JSON', () => {
    const definition = mcpConfig.buildMcpServerOptions({
        root: '%WORKSPACE%/CorpusAlpha',
        project: '%WORKSPACE%/CorpusAlpha/demo_project.al',
        serverRoot: 'Y:/fpga_lsp',
        pythonPath: 'python',
        tdPath: 'C:/Anlogic/TD',
        allowFlow: true,
        allowWrite: false,
        version: '0.10.0',
        existingPythonPath: '',
    });
    eq(definition.label, 'Anlogic FPGA LSP');
    eq(definition.command, 'python');
    ok(definition.args.includes('-m') && definition.args.includes('fpgalsp.mcp'));
    ok(definition.args.includes('%WORKSPACE%/CorpusAlpha/demo_project.al'));
    ok(definition.args.includes('--allow-flow') && !definition.args.includes('--allow-write'));
    includes(definition.env.PYTHONPATH, 'Y:/fpga_lsp');
    const portable = mcpConfig.buildPortableConfig(definition);
    ok(portable.mcpServers['fpga-lsp'], 'portable mcpServers entry');
    const workspaceDefinition = mcpConfig.buildWorkspaceMcpServerOptions({ serverRoot: 'Y:/fpga_lsp', existingPythonPath: '' });
    const vscode = mcpConfig.buildVsCodeConfig(workspaceDefinition);
    eq(vscode.servers['fpga-lsp'].type, 'stdio');
    ok(!('cwd' in vscode.servers['fpga-lsp']));
    includes(vscode.servers['fpga-lsp'].args.join(' '), '${workspaceFolder}');
    ok(!vscode.servers['fpga-lsp'].args.join(' ').includes('%WORKSPACE%/CorpusAlpha'));
});

test('snippets: contributes Verilog/SystemVerilog/VHDL/ADC/SDC snippet files', () => {
    const fs = require('fs');
    const path = require('path');
    const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8'));
    const entries = pkg.contributes.snippets || [];
    eq(entries.length, 5, 'snippet contributions');
    for (const entry of entries) {
        const file = path.join(__dirname, '..', entry.path.replace(/^\.\//, ''));
        ok(fs.existsSync(file), entry.path);
        const snippets = JSON.parse(fs.readFileSync(file, 'utf8'));
        ok(Object.keys(snippets).length >= 3, entry.language + ' snippet count');
        for (const snippet of Object.values(snippets)) {
            ok(snippet.prefix, entry.language + ' prefix');
            ok(snippet.body, entry.language + ' body');
        }
    }
});

test('flow diagnostics: parses TD errors/warnings and ignores progress chatter', () => {
    const error = parseFlowIssue('PRJ-8806 ERROR: child process died');
    ok(error && error.severity === 'error' && error.code === 'PRJ-8806', JSON.stringify(error));
    const warning = parseFlowIssue("HDL-1007 : undeclared symbol 'x', assumed wire in src/top.v(580)");
    ok(warning && warning.severity === 'warning' && warning.file === 'src/top.v'
        && warning.line === 580, JSON.stringify(warning));
    const critical = parseFlowIssue('CRITICAL-WARNING: setup violation in C:\\work\\a.v(12)');
    ok(critical && critical.severity === 'warning' && /a\.v$/.test(critical.file)
        && critical.line === 12, JSON.stringify(critical));
    const runLog = parseFlowIssue('[run.log] WARNING: hold slack negative');
    ok(runLog && runLog.severity === 'warning', JSON.stringify(runLog));
    eq(parseFlowIssue('RUN-1002 : start command "launch_runs"'), null);
    eq(parseFlowIssue('Project loaded: demo'), null);
    eq(parseFlowIssue(''), null);
});

test('flow diagnostics publish to the Problems collection', () => {
    const controller = new FlowController({
        rpc: { request: async () => ({}), execute: async () => ({}) },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al', dir: 'Y:/p' }),
    });
    controller._projectInfo = { al: 'Y:/p/demo.al', dir: 'Y:/p' };
    controller.onFlowEvent({
        type: 'log',
        message: "HDL-1007 : undeclared symbol 'x', assumed wire in src/top.v(12)",
    });
    eq(controller._flowDiagMap.size, 1, 'one Problems file');
    const diagnostics = Array.from(controller._flowDiagMap.values())[0];
    eq(diagnostics.length, 1, 'one diagnostic');
    eq(diagnostics[0].severity, 1, 'warning severity');
    eq(diagnostics[0].source, 'fpga-flow');
    eq(diagnostics[0].range.start.line, 11);
    controller.dispose();
});

const protocol = require('../src/protocol');
    const manifest = require('../package.json');

    const declared = new Set(manifest.contributes.commands.map((entry) => entry.command));
    const registered = new Set();
    const referenced = new Set();

    const extensionSource = fs.readFileSync(path.join(__dirname, '..', 'extension.js'), 'utf8');
    for (const match of extensionSource.matchAll(/register\(\s*VSCodeCommands\.(\w+)/g)) {
        registered.add(protocol.VSCodeCommands[match[1]]);
        referenced.add(protocol.VSCodeCommands[match[1]]);
    }

    const srcDir = path.join(__dirname, '..', 'src');
    const nonCommandIds = new Set([...Object.values(protocol.VIEWS), 'fpgaLsp.reports']);
    for (const file of fs.readdirSync(srcDir)) {
        if (!file.endsWith('.js')) continue;
        const source = fs.readFileSync(path.join(srcDir, file), 'utf8');
        for (const match of source.matchAll(/command:(fpgaLsp\.[A-Za-z0-9]+|fpgalsp\.[A-Za-z0-9]+)/g)) {
            referenced.add(match[1]);
        }
        for (const match of source.matchAll(/['"](fpgaLsp\.[A-Za-z0-9]+)['"]/g)) {
            if (!nonCommandIds.has(match[1])) referenced.add(match[1]);
        }
        for (const match of source.matchAll(/VSCodeCommands\.(\w+)/g)) {
            if (protocol.VSCodeCommands[match[1]]) referenced.add(protocol.VSCodeCommands[match[1]]);
        }
    }

    // viewsWelcome command links in package.json
    const manifestText = fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8');
    for (const match of manifestText.matchAll(/command:(fpgaLsp\.[A-Za-z0-9]+|fpgalsp\.[A-Za-z0-9]+)/g)) {
        referenced.add(match[1]);
    }

    for (const id of referenced) {
        ok(declared.has(id), `referenced command ${id} is declared in package.json`);
        ok(registered.has(id), `referenced command ${id} is registered in extension.js`);
    }
    for (const id of declared) {
        ok(registered.has(id), `declared command ${id} is registered in extension.js`);
    }

    // 0.19.3 view layout: Project & Flow / Reports / ChipWatcher in the FPGA container,
    // Module Hierarchy moved to the Explorer, and no bottom-panel ChipWatcher any more.
    const views = manifest.contributes.views;
    eq(Object.prototype.hasOwnProperty.call(views, 'panel'), false, 'no panel view container');
    eq(views.explorer.map((view) => view.id).join(','), 'fpgaLsp.modulesView', 'explorer views');
    eq(views.explorer.map((view) => view.name).join(','), 'Module Hierarchy', 'explorer view title');
    eq(views.fpgaLsp.map((view) => view.id).join(','),
        'fpgaLsp.flowView,fpgaLsp.reportsView,fpgaLsp.probeView', 'fpgaLsp view order');
    eq(views.fpgaLsp.map((view) => view.name).join(','), 'Project & Flow,Reports,ChipWatcher',
        'fpgaLsp view titles');
    for (const view of ['fpgaLsp.modulesView', 'fpgaLsp.flowView', 'fpgaLsp.probeView']) {
        ok(manifest.activationEvents.includes(`onView:${view}`), `activation event ${view}`);
    }
    // The duplicate "Edit probe signal list" command is gone: showProbePanel is the only
    // rich-panel entry point, and the tree itself is the signal list.
    ok(!declared.has('fpgaLsp.probeConfig'), 'duplicate probeConfig command removed');
    const showProbePanel = manifest.contributes.commands.find((entry) => entry.command === 'fpgaLsp.showProbePanel');
    eq(showProbePanel.title, 'FPGA: ChipWatcher (signals, trigger, capture)', 'showProbePanel title');
    ok(declared.has('fpgaLsp.probeToggleSignal'), 'probeToggleSignal declared');
    ok(declared.has('fpgaLsp.probeAddSignals'), 'probeAddSignals declared');
    ok(declared.has('fpgaLsp.probeSetClock'), 'probeSetClock declared (sample clock)');
    ok(declared.has('fpgaLsp.probeSetDepth'), 'probeSetDepth declared (sample depth)');
    ok(declared.has('fpgaLsp.refreshProbe'), 'refreshProbe declared');
    const probeTitleMenus = manifest.contributes.menus['view/title']
        .filter((entry) => entry.when === 'view == fpgaLsp.probeView');
    ok(probeTitleMenus.some((entry) => entry.command === 'fpgaLsp.refreshProbe'), 'probe title refresh button');
    ok(probeTitleMenus.some((entry) => entry.command === 'fpgaLsp.showActions'), 'probe title actions button');
});

// ------------------------------------------------------------------ A4: event-driven refresh

test('reports service: concurrent callers share one in-flight request (single flight)', async () => {
    const calls = [];
    let release;
    const service = new ReportsService({
        request: (method, params) => {
            calls.push({ method, params });
            return new Promise((resolve) => { release = () => resolve({ ok: true, section: 'summary', flow: {} }); });
        },
    });
    const first = service.getSummary();
    const second = service.getSummary();
    eq(calls.length, 1, 'only one fpgaLsp/reports request for two concurrent callers');
    release();
    const [a, b] = await Promise.all([first, second]);
    ok(a && a.ok === true, 'first caller got the payload');
    ok(a === b, 'both callers share the same payload object');
    eq(service.requests, 1, 'request counter');
});

test('reports service: falls back to the full payload when the server ignores "section"', async () => {
    const calls = [];
    const service = new ReportsService({
        request: async (method, params) => {
            calls.push({ method, params });
            // An older server ignores the unknown parameter and returns the full report.
            return sampleReports();
        },
    });
    const payload = await service.getSummary();
    ok(payload && payload.ok === true, 'payload returned');
    eq(service.sectionSupport, 'unsupported', 'section support detected as unsupported');
    eq(calls.length, 1, 'the ignored-parameter reply IS the full report: no second request');
    eq(calls[0].params.section, 'summary', 'first request asked for the summary');

    // Subsequent section requests must not try `section` again, and reuse the full payload.
    const timing = await service.getSection('timing');
    ok(timing.ok === true, 'timing payload');
    eq(calls.length, 1, 'cached full payload reused (no extra request)');
    eq(Object.keys(calls[0].params).length, 1, 'only the first request carried a hint');
});

test('reports service: echoes section when supported and caches per section', async () => {
    const calls = [];
    const service = new ReportsService({
        request: async (method, params) => {
            calls.push(params);
            return { ok: true, section: params.section || '', qor: {} };
        },
    });
    await service.getSummary();
    eq(service.sectionSupport, 'supported');
    eq(calls.length, 1);
    await service.getSummary();
    eq(calls.length, 1, 'cached within the TTL');
    await service.getSection('area');
    eq(calls.length, 2, 'different section -> different request');
    service.invalidate();
    await service.getSection('area');
    eq(calls.length, 3, 'invalidate drops the cache');
});

test('module tree: indexChanged patches the affected module in place', async () => {
    const requested = [];
    const provider = new ModuleTreeProvider({
        request: async (method, params) => { requested.push(params); return sampleModuleTree(); },
    });
    await provider.refresh();
    eq(requested.length, 1, 'initial refresh');
    const root = provider.getChildren()[0];
    const pll = provider.getChildren(root).find((node) => node.raw.module === 'pll_gen');
    ok(pll, 'pll_gen node exists');
    eq(pll.raw.portCount, 0, 'portCount before');

    const outcome = provider.applyIndexChanged({
        al: '%WORKSPACE%/CorpusAlpha/demo_project.al',
        paths: ['%WORKSPACE%/CorpusAlpha/src/top.v'],
        modules: [{ module: 'pll_gen', portCount: 7 }],
    });
    eq(outcome, 'in-place', 'module-level event patches without a request');
    eq(pll.raw.portCount, 7, 'node payload updated');
    eq(requested.length, 1, 'no extra fpgaLsp/moduleTree request');
    eq(provider.stats.inPlace, 1);
});

test('module tree: indexChanged ignores files of another project', async () => {
    const requested = [];
    const provider = new ModuleTreeProvider({
        request: async (method, params) => { requested.push(params); return sampleModuleTree(); },
    });
    await provider.refresh();
    eq(provider.applyIndexChanged({ paths: ['X:/other_project/src/top.v'] }), 'ignored');
    eq(provider.applyIndexChanged({ al: 'X:/other_project/other.al' }), 'ignored');
    eq(requested.length, 1, 'no request for a foreign project');
    eq(provider.stats.ignored, 2);
});

// F6/R1: a pushed `indexChanged` is already "the server finished re-indexing", so the provider
// refreshes **immediately** now (it used to re-arm a 150 ms window, which stacked with the
// extension-side debounce and made every save take ~300 ms -- see
// audit/2026-11-19/perf/VSCODE.md §P1). This test used to assert the debounced behaviour.
test('module tree: indexChanged without a patchable module refreshes immediately', async () => {
    const requested = [];
    const provider = new ModuleTreeProvider({
        request: async (method, params) => { requested.push(params); return sampleModuleTree(); },
    });
    await provider.refresh();
    const outcome = provider.applyIndexChanged({ paths: ['%WORKSPACE%/CorpusAlpha/src/system/pwm_gen.v'] });
    eq(outcome, 'refresh', 'unknown effect -> refresh');
    await new Promise((resolve) => setTimeout(resolve, 20));
    eq(requested.length, 2, 'F6/R1: the refresh request goes out at once (no 150 ms wait)');

    // Each push is one server-side re-index, so each gets its own request; a slow older response
    // can never win because refresh() cancels the in-flight one and `_seq` drops stale results.
    provider.applyIndexChanged({ paths: ['%WORKSPACE%/CorpusAlpha/src/a.v'] });
    provider.applyIndexChanged({ paths: ['%WORKSPACE%/CorpusAlpha/src/b.v'] });
    await new Promise((resolve) => setTimeout(resolve, 20));
    eq(requested.length, 4, 'two pushes -> two requests (superseded responses are dropped)');

    // A pending debounced fallback must be cancelled by the pushed refresh, otherwise the save
    // handler would fire the same request again 150 ms later.
    const before = requested.length;
    provider.requestRefresh('%WORKSPACE%/CorpusAlpha/src/top.v', { reason: 'saved' });
    provider.applyIndexChanged({ paths: ['%WORKSPACE%/CorpusAlpha/src/c.v'] });
    await new Promise((resolve) => setTimeout(resolve, 220));
    eq(requested.length, before + 1, 'the pushed refresh cancelled the pending fallback timer');
});


// F6/R8: `appendLog()` is called from event handlers, so it must never do synchronous disk I/O.
// The bytes themselves go through src/writeService.js (state root + policy + never-throw).
test('launch log: appendLog is buffered and flushes asynchronously', async () => {
    const fs = require('fs');
    const os = require('os');
    const path = require('path');
    const { STATE_ENV } = require('../src/stateDir');
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fpgal-log-'));
    const previous = process.env[STATE_ENV];
    process.env[STATE_ENV] = path.join(dir, 'state');
    try {
        const file = path.join(dir, 'state', 'logs', 'extension.log');
        appendLog(dir, 'extension', 'first line');
        eq(fs.existsSync(file), false, 'no synchronous write from the caller');
        await new Promise((resolve) => setTimeout(resolve, 80));
        eq(fs.existsSync(file), true, 'flushed on a later tick');
        includes(fs.readFileSync(file, 'utf8'), 'first line');

        // flushLogSync() drains the tail (shutdown / fatal path).
        appendLog(dir, 'extension', 'tail line');
        flushLogSync();
        includes(fs.readFileSync(file, 'utf8'), 'tail line');
    } finally {
        if (previous === undefined) delete process.env[STATE_ENV];
        else process.env[STATE_ENV] = previous;
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test('reports panel: first paint is a full render, later refreshes patch sections', async () => {
    const requests = [];
    const service = new ReportsService({
        request: async (method, params) => { requests.push(params); return sampleReports(); },
    });
    const panel = new ReportsPanel({ rpc: { request: async () => sampleReports() }, reportsService: service });
    await panel.show();
    const created = mock.__state.webviewPanels[0];
    ok(created.webview.html.includes('Setup WNS'), 'first paint rendered the report');
    eq(panel.stats.html, 1, 'one full render (the summary skeleton)');
    // D1: the table sections are pulled afterwards and patched in -- for a server that ignores
    // `section` (this fake) the service answers from the full payload it already has, so the
    // whole open costs exactly one request.
    eq(panel.stats.patch, 1, 'sections patched in right after the skeleton');
    eq(requests.length, 1, 'no extra round trip when the server ignores `section`');

    await panel.refresh();
    eq(panel.stats.html, 1, 'no full re-render on refresh');
    eq(panel.stats.patch, 2, 'one section patch');
    eq(mock.__state.webviewPanels[0].webview.html, created.webview.html, 'webview.html untouched');
    const patch = mock.__state.webviewMessages.filter((message) => message.type === 'patch').pop();
    ok(patch, 'patch message posted');
    ok(patch.sections && patch.sections.dashboard, 'dashboard section patched');
    ok(patch.sections.qor && patch.sections.timing, 'qor + timing patched');
    includes(patch.sections.qor, 'QOR-1302', 'patched QoR keeps its content');
    ok(typeof patch.meta === 'string', 'meta line included');
    panel.dispose();
});

test('reports panel: a newer refresh cancels the older request', async () => {
    let release;
    // The *summary* request is the one held open; section requests resolve at once so the test
    // stays about cancellation (D1 added section fetches behind the summary fetch).
    const service = new ReportsService({
        request: (method, params) => {
            const section = (params && params.section) || 'summary';
            if (section === 'summary') {
                return new Promise((resolve) => {
                    release = () => resolve(Object.assign(sampleReports(), { section: 'summary' }));
                });
            }
            return Promise.resolve(Object.assign(sampleReports(), { section }));
        },
    });
    const panel = new ReportsPanel({ rpc: { request: async () => sampleReports() }, reportsService: service });
    const pendingShow = panel.show(); // paints the loading page, then awaits reports
    await new Promise((resolve) => setTimeout(resolve, 0));
    const slow = panel.refresh(); // supersedes the show() refresh
    const newest = panel.refresh(); // supersedes `slow`
    release();
    await Promise.allSettled([pendingShow, slow, newest]);
    ok(panel.stats.cancelled >= 1, `older refresh cancelled (got ${panel.stats.cancelled})`);
    eq(panel.stats.html, 1, 'only the newest refresh paints the page');
    eq(panel.stats.patch, 1, 'only the newest refresh patches the sections');
    panel.dispose();
});

// D1 (wave 2): the panel used to request only `{section:'summary'}`, and a section-capable
// server answers that with files+artifacts only -- so QoR / Timing / Area were always empty.
// The panel now paints the summary skeleton and then pulls the table sections on demand.
test('reports panel: QoR / Timing / Area are loaded on demand and are non-empty', async () => {
    const requests = [];
    const payloads = {
        summary: {
            ok: true, section: 'summary',
            files: { qor: 'Y:/p/route.qor', timing: 'Y:/p/final.rpt', area: 'Y:/p/route.area' },
            artifacts: { bit: null, runs: [] },
            flow: { running: false, stage: 'idle', elapsed: 0, lastMessage: '' },
        },
        qor: { ok: true, section: 'qor', qor: { step: 'route', metrics: { 'Setup WNS': '4388' }, suggestions: [] } },
        area: {
            ok: true, section: 'area',
            area: { rows: [{ resource: '#lut6', used: 1234, total: 45000, utilization: '2.7%' }] },
        },
        timing: {
            ok: true, section: 'timing',
            timing: {
                summary: { SWNS: 4.388, STNS: 0, HWNS: 1.2, HTNS: 0, status: 'Met' },
                groups: [{ name: 'clk_50M', paths: [{ slack: 1.25, begin: 'ff_a/Q', end: 'ff_b/D', check: 'Setup' }] }],
            },
        },
    };
    const service = new ReportsService({
        request: async (method, params) => {
            const section = (params && params.section) || 'summary';
            requests.push(section);
            return JSON.parse(JSON.stringify(payloads[section]));
        },
    });
    const panelsBefore = mock.__state.webviewPanels.length;
    const messagesBefore = mock.__state.webviewMessages.length;
    const panel = new ReportsPanel({ rpc: { request: async () => payloads.summary }, reportsService: service });

    await panel.show();
    const created = mock.__state.webviewPanels[panelsBefore];
    ok(created, 'panel created');
    // summary -> skeleton, then qor + area (the Dashboard tab needs both). Timing is NOT fetched
    // until its tab is opened: it can be megabytes of path data.
    eq(requests.join(','), 'summary,qor,area', 'eager: summary + qor + area, timing stays lazy');
    eq(panel.stats.html, 1, 'skeleton painted once');
    eq(panel.stats.patch, 1, 'sections patched in after the skeleton');
    const firstPatch = mock.__state.webviewMessages.slice(messagesBefore).filter((m) => m.type === 'patch').pop();
    ok(firstPatch, 'patch posted');
    includes(firstPatch.sections.qor, 'Setup WNS', 'QoR tab is non-empty');
    includes(firstPatch.sections.area, '#lut6', 'Area tab is non-empty');
    includes(firstPatch.sections.dashboard, '#lut6', 'dashboard pulls the loaded sections too');
    includes(firstPatch.sections.timing, 'No path groups.', 'Timing stays empty until opened');

    // Opening the Timing tab fetches exactly that section and patches just it (+dashboard).
    created.webview.__receive({ type: 'tab', id: 'timing' });
    await new Promise((resolve) => setTimeout(resolve, 20));
    eq(requests.filter((name) => name === 'timing').length, 1, 'timing fetched exactly once');
    const timingPatch = mock.__state.webviewMessages.slice(messagesBefore)
        .filter((m) => m.type === 'patch' && m.sections && m.sections.timing).pop();
    ok(timingPatch, 'timing section patch posted');
    includes(timingPatch.sections.timing, 'ff_a/Q', 'Timing tab is non-empty (real path data)');
    includes(timingPatch.sections.dashboard, '4.388', 'dashboard WNS card filled from the timing section');

    // Re-visiting the tab must not fetch again (the section payload is kept).
    created.webview.__receive({ type: 'tab', id: 'timing' });
    await new Promise((resolve) => setTimeout(resolve, 20));
    eq(requests.filter((name) => name === 'timing').length, 1, 'no duplicate fetch when the tab is revisited');

    // All three tables are now non-empty in one merged view (what the webview would show).
    const merged = panel.reports;
    ok(merged.qor && merged.qor.metrics['Setup WNS'] === '4388', 'qor merged');
    ok(merged.area && merged.area.rows.length === 1, 'area merged');
    ok(merged.timing && merged.timing.groups.length === 1, 'timing merged');
    panel.dispose();
});

test('flow controller: 10 s fallback poll only, and it stops when the flow finishes', async () => {
    eq(FALLBACK_POLL_MS, 10000, 'fallback interval is 10 s (the 2 s poll is gone)');
    const polled = [];
    const controller = new FlowController({
        rpc: {
            request: async (method) => { polled.push(method); return { ok: true, flow: { running: true, stage: 'syn' } }; },
            execute: async () => ({ ok: true, runId: 'r9' }),
        },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al' }),
    });
    const running = controller.run(['syn']);
    await new Promise((resolve) => setTimeout(resolve, 30));
    ok(controller.poller, 'fallback timer armed while running');
    eq(controller.poller._idleTimeout, FALLBACK_POLL_MS, 'timer period');
    // 起跑后立刻查一次状态（服务端此刻已清掉本 run 的步骤标记，树要马上显示 not run），
    // 之后仍然只靠 10 s 兜底 + flowEvent 推送，不做 2 s 轮询。
    eq(polled.filter((method) => method === 'fpgaLsp/flowStatus').length, 1, 'exactly one immediate flowStatus call');
    controller.onFlowEvent({ runId: 'r9', type: 'done', ok: true, stage: 'syn' });
    await running;
    eq(controller.poller, undefined, 'poll stopped after done');
    eq(polled.filter((method) => method === 'fpgaLsp/flowStatus').length, 1, 'no polling after the immediate one');
    controller.dispose();
});

test('flow controller: right-click Run / Reset run goes through the controller', async () => {
    // 回归：六步右键曾经在 extension.js 里直接 rpc.execute(runFlowStep)，
    // 于是没有输出通道、没有状态栏、没有进度、结束后也不刷树 ——
    // 表现就是"点了 reset and run，FPGA Flow 没有任何输出，下游步骤还是已完成"。
    const calls = [];
    const notices = [];
    const controller = new FlowController({
        rpc: {
            request: async () => ({ ok: true, flow: { running: true, stage: 'syn', steps: [] } }),
            execute: async (tool, args) => {
                calls.push([tool, args]);
                return { ok: true, async: true, runId: 'r-step', stages: ['syn'], step: args && args[0] && args[0].step };
            },
        },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al' }),
    });
    const window = require('./mock-vscode');
    const originalInfo = window.window.showInformationMessage;
    const originalWarning = window.window.showWarningMessage;
    window.window.showInformationMessage = (text) => { notices.push(String(text)); };
    window.window.showWarningMessage = (text) => { notices.push(String(text)); };
    try {
        // view/item/context 的真实参数是**树节点**（getChildren 的 element）：
        // {kind:'stage', step:'read_design', stepName:'Read Design', label:'Read Design · 105 MB'}
        const item = { kind: 'stage', step: 'read_design', stepName: 'Read Design', label: 'Read Design · 105 MB' };
        const reset = controller.runStep(item, true);
        await new Promise((resolve) => setTimeout(resolve, 30));
        eq(calls.length, 1, 'one server call');
        eq(calls[0][0], 'fpgalsp.runFlowStep', 'uses runFlowStep');
        eq(calls[0][1][0].step, 'read_design', 'step passed through (tree element shape)');
        eq(calls[0][1][0].force, true, 'Reset run sends force:true');
        eq(Boolean(controller.active && controller.active.step === 'read_design'), true, 'controller tracks the step run');
        ok(controller.output.lines.some((line) => /reset run Read Design/.test(line)),
            'FPGA Flow output shows the step run (was silent before)');
        ok(controller.output.lines.some((line) => /fpga-lsp \d+\.\d+\.\d+/.test(line)),
            'FPGA Flow output names the running extension version (stale host is visible)');
        // 结束（服务端 done）后要收尾 + 刷树
        controller.onFlowEvent({ runId: 'r-step', type: 'done', ok: true, stage: 'syn' });
        await reset;
        eq(controller.active, undefined, 'active cleared after done');
        ok(controller.statusBar.text.includes('done') || controller.statusBar.text.includes('idle'),
            `status bar settled: ${controller.statusBar.text}`);

        // Run（非 force）遇到 skipped：提示"已完成，用 Reset run"，不再等 flow
        controller.rpc.execute = async (tool, args) => {
            calls.push([tool, args]);
            return { ok: true, skipped: true, step: 'opt_gate', reason: 'step already completed' };
        };
        const skipped = await controller.runStep({ flowStep: 'opt_gate', flowStepName: 'Optimize Gate' }, false);
        eq(skipped.skipped, true, 'skipped result returned');
        ok(notices.some((text) => /already completed/.test(text)), 'user told to use Reset run');
        eq(controller.active, undefined, 'nothing tracked for a skipped run');

        // 既没有 step 也没有 flowStep（点到了非步骤行）：给一句可执行的提示，别静默
        const bogus = await controller.runStep({ kind: 'stage', label: 'No flow message yet' }, true);
        eq(bogus.ok, false, 'unknown shape is refused');
        ok(notices.some((text) => /right-click one of the six flow steps/.test(text)),
            'user told how to use it');
    } finally {
        window.window.showInformationMessage = originalInfo;
        window.window.showWarningMessage = originalWarning;
        controller.dispose();
    }
});

// ------------------------------------------------------------------ run

// Wave 3 / W2：状态目录（原 `<workspace>/.fpgalsp`）必须落在系统 temp，且启动服务端时
// 通过环境变量把同一个目录传下去。这两条以前只有"跑一遍看目录"的人工验证，
// 这里补成回归断言，防止以后有人改回 path.join(cwd, '.fpgalsp', ...)。
test('state dir: resolves under system temp and is passed to the server env', async () => {
    const os = require('os');
    const path = require('path');
    const stateDir = require('../src/stateDir');
    const mcpConfig = require('../src/mcpConfig');

    const root = path.join(os.tmpdir(), 'fpgalsp-state-test-project');
    const resolved = stateDir.resolveStateDir(root);
    eq(path.isAbsolute(resolved), true, 'must be absolute');
    eq(resolved.startsWith(path.join(os.tmpdir(), 'fpgalsp')), true,
        'must live under <tmp>/fpgalsp');
    eq(resolved.includes('.fpgalsp'), false, 'must not use the old workspace .fpgalsp path');
    eq(/[0-9a-f]{8}$/.test(resolved), true, 'directory name ends with an 8-hex-digit hash');

    // 环境变量优先（CI / 用户显式指定）
    const previous = process.env[stateDir.STATE_ENV];
    process.env[stateDir.STATE_ENV] = path.join(os.tmpdir(), 'fpgalsp-state-override');
    try {
        eq(stateDir.resolveStateDir(root), path.normalize(path.join(os.tmpdir(), 'fpgalsp-state-override')),
            'FPGALSP_STATE_DIR must win');
    } finally {
        if (previous === undefined) delete process.env[stateDir.STATE_ENV];
        else process.env[stateDir.STATE_ENV] = previous;
    }

    // MCP 定义：日志与 env 都要指向状态目录
    const options = mcpConfig.buildMcpServerOptions({
        nativeExe: 'C:/fake/fpgalsp-native.exe',
        root,
        allowFlow: false,
        allowWrite: false,
    });
    eq(options.env && options.env[stateDir.STATE_ENV] !== undefined, true,
        'native MCP server env must carry FPGALSP_STATE_DIR');
    const logIndex = options.args.indexOf('--log');
    eq(logIndex >= 0, true, 'must still pass --log explicitly');
    eq(String(options.args[logIndex + 1]).includes('.fpgalsp'), false,
        'mcp log must not be written into the workspace');
});

// ---------------------------------------------------------------------------
// 统一写服务（src/writeService.js）——与后端 platform::write_service 对称：
// root 策略 + 原子写 + never-throw + dryRun。门禁测试在本节最后。
// ---------------------------------------------------------------------------

function makeWriteSandbox() {
    const fs = require('fs');
    const os = require('os');
    const path = require('path');
    const { STATE_ENV } = require('../src/stateDir');
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fpgal-writeservice-'));
    const stateRoot = path.join(dir, 'state-root');
    const previous = process.env[STATE_ENV];
    process.env[STATE_ENV] = stateRoot;
    return {
        dir,
        stateRoot,
        restore() {
            if (previous === undefined) delete process.env[STATE_ENV];
            else process.env[STATE_ENV] = previous;
            fs.rmSync(dir, { recursive: true, force: true });
        },
    };
}

test('write service: state writes land under the state root and create parents', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const result = writeService.writeStateFile(sandbox.dir, 'logs/nested/report.txt', 'hello\n');
        eq(result.ok, true, `write should succeed: ${result.error}`);
        eq(result.path, path.join(sandbox.stateRoot, 'logs', 'nested', 'report.txt'), 'reported path');
        eq(fs.readFileSync(result.path, 'utf8'), 'hello\n', 'content written');
        eq(writeService.stateRootFor(sandbox.dir), path.resolve(sandbox.stateRoot),
            'stateRootFor mirrors src/stateDir.js (the directory the backend also gets)');
        eq(path.relative(writeService.stateRootFor(sandbox.dir), result.path).startsWith('..'), false,
            'reported path is inside the state root');
    } finally {
        sandbox.restore();
    }
});

test('write service: state paths that escape the root are refused', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const outside = [
            '../escape.txt',
            'logs/../../escape.txt',
            '/etc/passwd',
            'C:/Windows/evil.txt',
            '\\\\server\\share\\evil.txt',
            'logs/nul\u0000.txt',
        ];
        for (const rel of outside) {
            const result = writeService.writeStateFile(sandbox.dir, rel, 'x');
            eq(result.ok, false, `must refuse ${JSON.stringify(rel)}`);
            ok(result.error.length > 0, 'refusal carries a reason');
        }
        eq(fs.existsSync(path.join(sandbox.dir, 'escape.txt')), false, 'nothing escaped the state root');
        eq(fs.existsSync(path.join(sandbox.stateRoot, 'escape.txt')), false, 'nothing escaped into the root');
    } finally {
        sandbox.restore();
    }
});

test('write service: any .fpgalsp path segment is refused', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const stateCases = ['.fpgalsp/log.txt', 'logs/.fpgalsp/report.txt', 'logs/old.fpgalsp/x.txt'];
        for (const rel of stateCases) {
            const result = writeService.writeStateFile(sandbox.dir, rel, 'x');
            eq(result.ok, false, `state must refuse ${rel}`);
            includes(result.error, '.fpgalsp', 'reason names the forbidden segment');
        }
        const project = writeService.writeProjectFile(sandbox.dir, '.fpgalsp/mcp.json', '{}');
        eq(project.ok, false, 'project writes must refuse a .fpgalsp segment too');
        includes(project.error, '.fpgalsp');
        eq(fs.existsSync(path.join(sandbox.dir, '.fpgalsp')), false, 'no .fpgalsp directory is created');
    } finally {
        sandbox.restore();
    }
});

test('write service: dryRun validates without touching the disk', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const state = writeService.writeStateFile(sandbox.dir, 'logs/plan.txt', 'x', { dryRun: true });
        eq(state.ok, true, `dry run should succeed: ${state.error}`);
        eq(state.dryRun, true, 'result is flagged dry-run');
        eq(fs.existsSync(state.path), false, 'dry run writes nothing');
        eq(fs.existsSync(sandbox.stateRoot), false, 'dry run creates no directories');

        const project = writeService.writeProjectFile(sandbox.dir, '.vscode/mcp.json', '{}',
            { dryRun: true, backup: true });
        eq(project.ok, true, `project dry run should succeed: ${project.error}`);
        eq(fs.existsSync(path.join(sandbox.dir, '.vscode')), false, 'project dry run creates no directories');
    } finally {
        sandbox.restore();
    }
});

test('write service: atomic overwrite replaces the file and leaves no temp files', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const first = writeService.writeStateFile(sandbox.dir, 'logs/data.txt', 'old');
        eq(first.ok, true, first.error);
        const second = writeService.writeStateFile(sandbox.dir, 'logs/data.txt', 'new');
        eq(second.ok, true, second.error);
        eq(fs.readFileSync(second.path, 'utf8'), 'new', 'existing file is replaced');
        const leftovers = fs.readdirSync(path.dirname(second.path)).filter((name) => name.startsWith('.tmp-'));
        eq(leftovers.length, 0, 'no temporary file survives a successful write');
    } finally {
        sandbox.restore();
    }
});

test('write service: project writes are limited to the explicit whitelist', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        for (const rel of ['src/top.v', 'package.json', '.vscode/settings.json', 'notes.txt']) {
            const result = writeService.writeProjectFile(sandbox.dir, rel, 'x');
            eq(result.ok, false, `must refuse project file ${rel}`);
            includes(result.error, 'whitelist', 'reason names the whitelist');
        }
        eq(fs.existsSync(path.join(sandbox.dir, 'src')), false, 'refused writes create nothing');

        const allowed = writeService.writeProjectFile(sandbox.dir, 'constraints/pin.al', '<al/>');
        eq(allowed.ok, true, `*.al must be allowed: ${allowed.error}`);
        eq(fs.readFileSync(allowed.path, 'utf8'), '<al/>');
    } finally {
        sandbox.restore();
    }
});

test('write service: .vscode/mcp.json is written atomically with a .fpgalsp-bak backup', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const target = path.join(sandbox.dir, '.vscode', 'mcp.json');
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, '{"servers":{"old":{}}}\n', 'utf8');
        const result = writeService.writeProjectFile(sandbox.dir, '.vscode/mcp.json',
            '{"servers":{"new":{}}}\n', { backup: true, caller: 'test.mcp' });
        eq(result.ok, true, `mcp.json write should succeed: ${result.error}`);
        eq(fs.readFileSync(target, 'utf8'), '{"servers":{"new":{}}}\n', 'new config written');
        const backup = `${target}.fpgalsp-bak`;
        eq(result.backup, backup, 'backup path is reported');
        eq(fs.readFileSync(backup, 'utf8'), '{"servers":{"old":{}}}\n', 'backup keeps the previous bytes');
    } finally {
        sandbox.restore();
    }
});

test('write service: appendLog appends under the state logs dir and never throws', () => {
    const fs = require('fs');
    const path = require('path');
    const sandbox = makeWriteSandbox();
    try {
        const first = writeService.appendLog(sandbox.dir, 'extension', 'one');
        eq(first.ok, true, first.error);
        const second = writeService.appendLog(sandbox.dir, 'extension.log', 'two');
        eq(second.path, path.join(sandbox.stateRoot, 'logs', 'extension.log'), 'one sink per name');
        const other = writeService.appendLog(sandbox.dir, 'mcp', 'separate');
        eq(fs.readFileSync(first.path, 'utf8'), 'one\ntwo\n', 'lines append in order; .log is not doubled');
        eq(fs.readFileSync(other.path, 'utf8'), 'separate\n', 'different names use different files');

        const bad = writeService.appendLog(sandbox.dir, '../evil', 'x');
        eq(bad.ok, false, 'a traversing log name is refused');
        ok(bad.error.length > 0, 'refusal carries a reason');

        // IO failure must come back as ok=false, never as an exception.
        writeService.writeStateFile(sandbox.dir, 'logs/blocker', 'this is a file, not a directory');
        const failedAppend = writeService.appendLog(sandbox.dir, 'blocker/child', 'x');
        eq(failedAppend.ok, false, 'append under a file-as-directory fails');
        ok(failedAppend.error.length > 0, 'failure carries a reason');
        const failedWrite = writeService.writeStateFile(sandbox.dir, 'logs/blocker/child.txt', 'x');
        eq(failedWrite.ok, false, 'write under a file-as-directory fails');
    } finally {
        sandbox.restore();
    }
});

// 门禁（防复发）：以后新增的写文件代码必须走 writeService。扫描范围：extension.js 与
// src/*.js（test/** 是测试夹具，故意不扫；白名单只有 src/writeService.js 一个文件）。
test('write gate: raw fs write APIs live only in src/writeService.js', () => {
    const fs = require('fs');
    const path = require('path');
    const extensionRoot = path.join(__dirname, '..');
    const files = ['extension.js'].concat(
        fs.readdirSync(path.join(extensionRoot, 'src'))
            .filter((name) => name.endsWith('.js'))
            .map((name) => path.join('src', name)));
    const rawWrite = /fs\.(?:writeFileSync|appendFileSync|createWriteStream)\s*\(/;
    const violations = [];
    for (const relative of files) {
        if (relative.replace(/\\/g, '/') === 'src/writeService.js') continue;
        const source = fs.readFileSync(path.join(extensionRoot, relative), 'utf8');
        source.split(/\r?\n/).forEach((line, index) => {
            if (rawWrite.test(line)) violations.push(`${relative}:${index + 1}: ${line.trim()}`);
        });
    }
    eq(violations.length, 0,
        `raw file writes must go through src/writeService.js:\n  ${violations.join('\n  ')}`);
});

// ---------------------------------------------------------------------------
// 0.17.1：ChipWatcher 探针面板
//   * 界面文案必须是英文（仓库约定：只有 doc/ 里是中文）
//   * 信号按 base 分组、点芯片能拿到移除用的完整名字
//   * 报告里的文本必须转义
// ---------------------------------------------------------------------------
test('probe panel: renders the watcher state and groups signals by bus', () => {
    const { renderProbePanelHtml, groupSignals } = require('../src/probeHtml');
    const html = renderProbePanelHtml({
        ok: true,
        ready: true,
        reason: 'ready to capture on reg_init/clk',
        project: 'demo',
        target: 'PH1A100SFG676',
        matches_user_mdn: true,
        chip_debugger: {
            path: 'Y:/demo/chip_debugger/user.mdn',
            exists: true,
            count: 3,
            signals: ['WHG/b[1]', 'WHG/b[0]', 'WHG/a[7]'],
        },
        watcher: {
            present: true,
            instance: 'auto_created_cwc1',
            clock: 'reg_init/clk',
            sample_depth: 1024,
            trigger_position: 127,
            trigger_type: 'basic_and',
            stale: false,
            missing_in_bit: [],
            extra_in_bit: [],
            compiled_bit: 'Y:/demo/corpus_delta_Runs/phy_1/cw/compiled.bit',
            buses: [
                { name: 'b[1:0]', clock: 'reg_init/clk', type: 'MARK_DEBUG_PROBE', enabled: true },
            ],
        },
    });

    includes(html, 'ChipWatcher probes');
    includes(html, 'ready');
    includes(html, 'auto_created_cwc1');
    includes(html, 'reg_init/clk');
    includes(html, 'b[1:0]');
    // 每个 bit 一个可移除的芯片，值必须是完整层次名
    includes(html, 'data-remove="WHG/b[0]"');
    includes(html, 'data-remove="WHG/b[1]"');
    includes(html, 'data-remove-base="WHG/b"');
    eq(/[\u4e00-\u9fff]/.test(html), false, 'panel UI must be English only');

    const groups = groupSignals(['x[10]', 'x[2]', 'x[1]', 'y']);
    eq(groups.length, 2, 'one group per base');
    eq(groups[0].base, 'x', 'groups sorted by base');
    eq(groups[0].bits.join(','), '1,2,10', 'bits sorted numerically');
    eq(groups[1].bits.join(','), '-1', 'a scalar signal has no bit index');
});

test('probe panel: refuses the stale state and escapes untrusted text', () => {
    const { renderProbePanelHtml } = require('../src/probeHtml');
    const html = renderProbePanelHtml({
        ok: false,
        ready: false,
        reason: 'chip_debugger/user.mdn changed',
        project: 'demo<script>alert(1)</script>',
        target: 'PH1A100SFG676',
        chip_debugger: { path: 'p', exists: true, count: 1, signals: ['WHG/a[0]'] },
        watcher: {
            present: true,
            instance: 'auto_created_cwc1',
            clock: 'c',
            sample_depth: 1024,
            stale: true,
            missing_in_bit: ['ANPC_init0/ANPC_SVPWM_init/cnt'],
            extra_in_bit: [],
            buses: [{ name: 'a[0:0]', clock: 'c', type: 'MARK_DEBUG_PROBE', enabled: true }],
        },
    });

    includes(html, 'needs rebuild');
    includes(html, 'ANPC_init0/ANPC_SVPWM_init/cnt');
    includes(html, 'incremental route');
    excludes(html, '<script>alert(1)</script>', 'project name must be escaped');
    includes(html, '&lt;script&gt;alert(1)&lt;/script&gt;');
});

const TEST_TIMEOUT_MS = 5000;

(async () => {
    let passed = 0;
    let failed = 0;
    for (const entry of tests) {
        mock.__reset();
        FakeLanguageClient.responses = {};
        FakeLanguageClient.requests.length = 0;
        FakeLanguageClient.instance = undefined;
        let timer;
        try {
            // A test that awaits a promise nobody settles would otherwise let the event loop
            // drain and exit the runner *silently*. Fail loudly instead.
            await Promise.race([
                entry.fn(),
                new Promise((resolve, reject) => {
                    timer = setTimeout(() => reject(new Error(`timeout after ${TEST_TIMEOUT_MS} ms`)), TEST_TIMEOUT_MS);
                }),
            ]);
            passed += 1;
            console.log(`PASS  ${entry.name}`);
        } catch (err) {
            failed += 1;
            console.error(`FAIL  ${entry.name}`);
            console.error(`      ${err && err.message ? err.message : err}`);
        } finally {
            if (timer) clearTimeout(timer);
        }
    }
    console.log('');
    console.log(`${passed} passed, ${failed} failed, ${tests.length} total`);
    if (failed > 0) process.exitCode = 1;
})();


// P-C3: conditional requests. Trusted server keeps a revision; a matching `ifRevision`
// reply is "unchanged" and must be served from the cache without a second render.
test('reports service: uses ifRevision and reuses the cached payload when unchanged', async () => {
    const calls = [];
    let revision = 7;
    const service = new ReportsService({
        request: async (method, params) => {
            calls.push(Object.assign({}, params));
            if (params.ifRevision !== undefined && params.ifRevision === revision) {
                return { ok: true, unchanged: true, revision };
            }
            return { ok: true, section: params.section, revision, flow: {} };
        },
    }, { ttlMs: 0 });

    const first = await service.getSection('timing');
    ok(first && first.section === 'timing', 'first request returns real data');
    eq(calls.length, 1, 'first request carries no ifRevision');
    eq(calls[0].ifRevision, undefined, 'no revision before we have seen one');

    const second = await service.getSection('timing');
    eq(calls.length, 2, 'second request uses the conditional form');
    eq(calls[1].ifRevision, 7, 'ifRevision echoes the server revision');
    ok(second === first, 'unchanged reply is served from cache (same object)');

    revision = 8; // flow wrote new reports
    const third = await service.getSection('timing');
    eq(calls.length, 3, 'a changed revision falls back to a real request');
    ok(third !== first, 'changed revision produces fresh data');
});

// P-C4: flowStatus 增量日志。第二次轮询必须带 sinceSeq，且本地窗口要合并而不是被
// 增量切片覆盖（否则树里的历史日志会消失）。
test('flow controller: polls flowStatus incrementally (sinceSeq) and merges the log window', async () => {
    const requests = [];
    let seq = 0;
    const controller = new FlowController({
        rpc: {
            request: async (method, params) => {
                requests.push({ method, params });
                if (method !== 'fpgaLsp/flowStatus') return { ok: true };
                const next = seq + 2;
                const lines = [`line-${seq}`, `line-${seq + 1}`];
                seq = next;
                return { ok: true, flow: { running: true, stage: 'syn', logSeq: seq, log: lines } };
            },
            execute: async () => ({ ok: true, runId: 'r1' }),
        },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al' }),
    });
    controller.active = { runId: 'r1', startedAt: Date.now(), sawRunning: true, label: 'syn' };

    await controller.pollOnce();
    eq(requests.length, 1, 'first poll');
    eq(requests[0].params.sinceSeq, undefined, 'no sinceSeq before we know the sequence');

    await controller.pollOnce();
    eq(requests[1].params.sinceSeq, 2, 'second poll asks only for newer lines');
    eq(controller._recentLog.length, 4, 'local window keeps both batches');

    controller.dispose();
});


// ------------------------------------------------------------------ absolute flow timeout

// 2026-11-22 审计 major：flow 只有"推送事件 + 10 s 兜底轮询"，没有任何上界 —— flowStatus
// 连续抛错、或服务端永远不翻 running 时，"busy" 会一直挂着且无法取消。下面覆盖超时决策
// （自动档 / 固定档 / 截止时间 / 连续错误阈值）以及控制器里的收尾行为。
test('flow timeout: automatic tiers per stage and the fixed override', () => {
    const MIN = 60000;
    // 自动档（setting = 0）：syn 30 / phy 90 / all 120
    eq(resolveFlowTimeoutMs(['syn'], 0), 30 * MIN, 'synthesis -> 30 min');
    eq(resolveFlowTimeoutMs(['phy'], 0), 90 * MIN, 'place & route -> 90 min');
    eq(resolveFlowTimeoutMs(['all'], 0), 120 * MIN, 'full flow -> 120 min');
    eq(resolveFlowTimeoutMs([], 0), 120 * MIN, 'empty stages -> full-flow cap');
    // 单步 run（["opt_route"] 等）按所属阶段算
    eq(resolveFlowTimeoutMs(['read_design'], 0), 30 * MIN, 'syn step -> 30 min');
    eq(resolveFlowTimeoutMs(['opt_route'], 0), 90 * MIN, 'phy step -> 90 min');
    eq(resolveFlowTimeoutMs(['route'], 0), 90 * MIN, 'incremental route (phy) -> 90 min');
    // 多阶段 / phase 子集取对应最大值
    eq(resolveFlowTimeoutMs(['syn', 'phy'], 0), 90 * MIN, 'syn+phy -> the largest tier');
    eq(resolveFlowTimeoutMs(['opt_rtl', 'bitgen'], 0), 90 * MIN, 'mixed steps -> the largest tier');
    eq(resolveFlowTimeoutMs(['syn', 'all'], 0), 120 * MIN, 'all wins over syn');
    eq(resolveFlowTimeoutMs(['vendored_stage'], 0), 120 * MIN, 'unknown stage -> never too early');
    // 大小写 / 非 0 = 固定分钟（>600 钳住，<=0 回退自动档）
    eq(resolveFlowTimeoutMs(['SYN'], 0), 30 * MIN, 'stage names are case-insensitive');
    eq(resolveFlowTimeoutMs(['syn'], 3), 3 * MIN, 'fixed value overrides the tier');
    eq(resolveFlowTimeoutMs(['all'], 1), 1 * MIN, 'fixed 1 min');
    eq(resolveFlowTimeoutMs(['syn'], 0.5), 0.5 * MIN, 'fractional minutes are allowed');
    eq(resolveFlowTimeoutMs(['syn'], '15'), 15 * MIN, 'numeric strings are accepted');
    eq(resolveFlowTimeoutMs(['syn'], 601), 600 * MIN, 'fixed value is clamped to 600');
    eq(resolveFlowTimeoutMs(['syn'], -5), 30 * MIN, 'negative -> automatic');
    eq(resolveFlowTimeoutMs(['syn'], NaN), 30 * MIN, 'NaN -> automatic');
});

test('flow timeout: deadline comparison and the consecutive-error threshold', () => {
    eq(resolveFlowDeadline(['syn'], 0, 5000), 5000 + 30 * 60000, 'deadline = now + tier');
    eq(isFlowTimedOut({ deadline: 1000, now: 1000 }), false, 'exactly at the deadline is not late');
    eq(isFlowTimedOut({ deadline: 1000, now: 1001 }), true, 'one ms late -> timed out');
    eq(isFlowTimedOut({ deadline: 1000, now: 999 }), false, 'before the deadline');
    eq(isFlowTimedOut({ deadline: undefined, now: 9999999 }), false, 'no deadline -> never timed out');
    eq(isFlowTimedOut({}), false, 'no options -> never timed out');

    eq(shouldAbandonOnErrors(0), false, 'no failures');
    eq(shouldAbandonOnErrors(MAX_CONSECUTIVE_FLOW_ERRORS - 1), false, 'below the bound keeps polling');
    eq(shouldAbandonOnErrors(MAX_CONSECUTIVE_FLOW_ERRORS), true, 'the 12th consecutive failure gives up');
    eq(shouldAbandonOnErrors(MAX_CONSECUTIVE_FLOW_ERRORS + 5), true, 'beyond the bound too');
    eq(shouldAbandonOnErrors(undefined), false, 'missing counter -> no abandon');
    eq(shouldAbandonOnErrors(NaN), false, 'broken counter -> no abandon');
    eq(MAX_CONSECUTIVE_FLOW_ERRORS, 12, '12 x 10 s fallback poll ~= 2 minutes');
});

test('flow controller: the absolute timeout cancels once and finishes failed', async () => {
    const executes = [];
    let requests = 0;
    const controller = new FlowController({
        rpc: {
            request: async () => {
                requests += 1;
                return { ok: true, flow: { running: true, stage: 'syn' } };
            },
            execute: async (tool) => { executes.push(tool); return { ok: true }; },
        },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al' }),
    });
    // 摆一个已经超时的 active（自动档 syn = 30 分钟）
    const startedAt = Date.now() - 31 * 60 * 1000;
    controller.active = {
        runId: 'r1',
        stages: ['syn'],
        label: 'synthesis',
        startedAt,
        sawRunning: true,
        pollErrors: 0,
        timeoutMs: resolveFlowTimeoutMs(['syn'], 0),
        deadline: resolveFlowDeadline(['syn'], 0, startedAt),
    };
    controller.startPolling();
    controller.setStatusRunning('synthesis');

    await controller.pollOnce();
    eq(controller.active, undefined, 'a timed-out flow is not left busy');
    eq(controller.poller, undefined, 'poll timer stopped');
    eq(requests, 0, 'no flowStatus request after the deadline');
    eq(executes.filter((tool) => tool === 'fpgalsp.cancelFlow').length, 1, 'cancel sent exactly once');
    includes(controller.output.lines.join('\n'), 'flow timed out after 30 min - cancelling');
    ok(controller.statusBar.text.includes('failed'), `status bar failed: ${controller.statusBar.text}`);
    eq(mock.__state.messages.warning.length, 1, 'exactly one warning');
    includes(mock.__state.messages.warning[0].message, 'timed out after 30 min');

    // 同一次 flow 只收尾/提示一次：再轮询、再来事件都不会再 cancel 或再提示
    await controller.pollOnce();
    controller.onFlowEvent({ runId: 'r1', type: 'progress', message: 'still running' });
    eq(executes.filter((tool) => tool === 'fpgalsp.cancelFlow').length, 1, 'no second cancel');
    eq(mock.__state.messages.warning.length, 1, 'no second warning');

    // 推送事件也检查 deadline（它可能比 10 s 兜底轮询更早到）
    controller._cancelRequested = false;
    controller.active = {
        runId: 'r2',
        stages: ['all'],
        label: 'full flow',
        startedAt: Date.now() - 121 * 60 * 1000,
        sawRunning: true,
        pollErrors: 0,
        timeoutMs: resolveFlowTimeoutMs(['all'], 0),
        deadline: Date.now() - 60000,
    };
    controller.onFlowEvent({ runId: 'r2', type: 'start', stage: 'syn' });
    eq(controller.active, undefined, 'a pushed event after the deadline also aborts');
    includes(controller.output.lines.join('\n'), 'flow timed out after 120 min - cancelling');
    controller.dispose();
});

test('flow controller: consecutive flowStatus failures abandon a dead-server flow', async () => {
    let requests = 0;
    const executes = [];
    const controller = new FlowController({
        rpc: {
            request: async () => { requests += 1; throw new Error('ECONNREFUSED'); },
            execute: async (tool) => { executes.push(tool); throw new Error('hub closed'); },
        },
        getProjectInfo: async () => ({ al: 'Y:/p/demo.al' }),
    });
    const startedAt = Date.now();
    controller.active = {
        runId: 'r3',
        stages: ['all'],
        label: 'full flow',
        startedAt,
        sawRunning: true,
        pollErrors: MAX_CONSECUTIVE_FLOW_ERRORS - 1,
        timeoutMs: resolveFlowTimeoutMs(['all'], 0),
        deadline: resolveFlowDeadline(['all'], 0, startedAt),
    };

    await controller.pollOnce(); // 第 12 次连续失败 -> 收尾
    eq(requests, 1, 'gives up on the 12th consecutive failure');
    eq(controller.active, undefined, 'dead-server flow is not left busy');
    includes(controller.output.lines.join('\n'), 'language server is not responding');
    eq(executes.filter((tool) => tool === 'fpgalsp.cancelFlow').length, 1, 'cancel attempted once');
    ok(controller.statusBar.text.includes('failed'), `status bar failed: ${controller.statusBar.text}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
    includes(controller.output.lines.join('\n'), 'cancel request failed', 'a failed cancel is only logged');

    // 成功一次就清零；未到阈值时只是继续轮询，不收尾
    controller._cancelRequested = false;
    controller.active = {
        runId: 'r4',
        stages: ['syn'],
        label: 'synthesis',
        startedAt: Date.now(),
        sawRunning: true,
        pollErrors: 5,
        timeoutMs: resolveFlowTimeoutMs(['syn'], 0),
        deadline: resolveFlowDeadline(['syn'], 0, Date.now()),
    };
    controller.rpc.request = async () => ({ ok: true, flow: { running: true, stage: 'syn' } });
    await controller.pollOnce();
    eq(controller.active.pollErrors, 0, 'a successful poll clears the error streak');

    controller.rpc.request = async () => { throw new Error('ECONNREFUSED'); };
    controller.active.pollErrors = MAX_CONSECUTIVE_FLOW_ERRORS - 2;
    await controller.pollOnce();
    ok(controller.active, 'below the bound the flow keeps polling');
    eq(controller.active.pollErrors, MAX_CONSECUTIVE_FLOW_ERRORS - 1, 'the streak keeps counting');
    controller.dispose();
});


// P-A3: 端点文件（真实端口 + token）是扩展连接 MCP 的唯一依据，不再靠 FNV 派生端口。
test('mcp endpoint: reads port/token, rejects a foreign root, ignores corrupt files', () => {
    const os = require('os');
    const fs = require('fs');
    const path = require('path');
    const { readMcpEndpoint } = require('../src/mcpEndpoint');
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fpgalsp-endpoint-'));

    fs.writeFileSync(path.join(dir, 'mcp-endpoint-45001.json'), JSON.stringify({
        version: 1, port: 45001, token: 'a'.repeat(32), pid: process.pid,
        root: 'Y:/p/demo', startedAt: 2,
    }));
    const found = readMcpEndpoint(dir, { root: 'Y:\\p\\demo' });
    ok(found && found.port === 45001, 'reads the advertised port');
    eq(found.token.length, 32, 'carries the token');

    eq(readMcpEndpoint(dir, { root: 'Y:/other' }), null, 'foreign workspace is ignored');

    fs.writeFileSync(path.join(dir, 'mcp-endpoint-45002.json'), '{not json');
    fs.writeFileSync(path.join(dir, 'mcp-endpoint-45003.json'), JSON.stringify({
        version: 1, port: 45003, token: 'b'.repeat(32), pid: process.pid,
        root: 'Y:/p/demo', startedAt: 3,
    }));
    eq(readMcpEndpoint(dir, { root: 'Y:/p/demo' }).port, 45003,
       'newest endpoint wins even when another file is corrupt');
});


// P-C2: 懒加载。payload 只给根一层（hasChildren），展开时才发 moduleChildren，
// 而且只发一次（重复展开不重复请求）。
test('module tree: lazy children are fetched on expand, exactly once', async () => {
    const calls = [];
    const provider = new ModuleTreeProvider({
        request: async (method, params) => {
            calls.push({ method, params });
            if (method === 'fpgaLsp/moduleTree') {
                return {
                    ok: true, project: 'demo', al: 'Y:/p/demo.al',
                    root: { module: 'top', hasChildren: true, range: { start: { line: 0, character: 0 }, end: { line: 0, character: 3 } } },
                };
            }
            if (method === 'fpgaLsp/moduleChildren') {
                return {
                    ok: true, parent: params.module, total: 2, nextOffset: 2, hasMore: false,
                    children: [
                        { module: 'a', instance: 'u_a', hasChildren: false },
                        { module: 'b', instance: 'u_b', hasChildren: true },
                    ],
                };
            }
            return { ok: false };
        },
    });
    await provider.refresh();

    const [root] = provider.getChildren();
    const children = await provider.getChildren(root);
    eq(children.length, 2, 'one level fetched on expand');
    eq(calls.filter((c) => c.method === 'fpgaLsp/moduleChildren').length, 1, 'moduleChildren called once');
    eq(calls.find((c) => c.method === 'fpgaLsp/moduleChildren').params.module, 'top', 'asks for the expanded module');

    const again = await provider.getChildren(root);
    eq(again.length, 2, 'second expand reuses loaded children');
    eq(calls.filter((c) => c.method === 'fpgaLsp/moduleChildren').length, 1, 'no duplicate request');
});


// M3c: 时序端点可点击。后端解析成功（resolved + file/line）时必须渲染成
// command:fpgaLsp.openTimingPoint 链接；未解析时退回原始文本。
test('reports webview: resolved timing endpoints become clickable links', () => {
    const reports = sampleReports();
    reports.timing = {
        groups: [{
            name: 'clk_group',
            statistics: {},
            paths: [
                {
                    slack: -0.35, begin: 'u_a/reg_a/Q', end: 'u_b/reg_b/D',
                    begin_at: { resolved: true, file: 'Y:/p/src/a.v', line: 41, signal: 'reg_a' },
                    end_at: { resolved: false, reason: 'signal-not-in-module' },
                    details: {}, delay: [],
                },
            ],
        }],
        paths: [],
    };
    const html = renderReportsHtml({ reports, nonce: 'n' });
    const index = html.indexOf('openTimingPoint');
    ok(index > 0, '定时端点必须生成 openTimingPoint 链接');
    ok(html.includes('a.v:42'), '链接文本带 1-based 行号');
    ok(html.includes('reg_a'), '链接标题/内容包含信号名');
    ok(html.includes('u_b/reg_b/D'), '未解析的一侧保留原始文本');
});


// B-B7: 面积热点跳源码。resolved 的模块行必须渲染成 openSourceLocation 链接。
test('reports webview: resolved area modules link to their definition', () => {
    const reports = sampleReports();
    reports.area = {
        rows: [],
        counts: {},
        modules: [
            { name: 'u2', module: 'm2', path: 'top/u2', resolved: true,
              file: 'Y:/p/src/m2.v', line: 9, resources: { lut: 12 }, utilization: {} },
            { name: 'u_bb', module: 'blackbox_x', path: 'top/u_bb', resolved: false,
              resources: { lut: 3 }, utilization: {} },
        ],
    };
    delete reports.syn;
    delete reports.phy;
    const html = renderReportsHtml({ reports, nonce: 'n' });
    ok(html.includes('openSourceLocation'), 'resolved 的面积模块必须生成跳转链接');
    ok(html.includes('m2.v:10'), '链接文本带 1-based 行号');
    ok(html.includes('blackbox_x'), '未解析的模块仍然显示模块名');
});


// X-B2: 跨 run 趋势。3 个 run 必须有 3 个点，且每个点链接到该 run 的 QoR 文件。
test('qor trend: three runs produce three linked points', () => {
    const runs = [
        { name: 'phy_1', dir: '/p/demo_Runs/phy_1', files: ['/p/demo_Runs/phy_1/route.qor'],
          values: { 'Setup WNS': -861, 'Setup TNS': -23866, 'area:lut': 663 } },
        { name: 'phy_2', dir: '/p/demo_Runs/phy_2', files: ['/p/demo_Runs/phy_2/route.qor'],
          values: { 'Setup WNS': -947, 'Setup TNS': -24474, 'area:lut': 4923 } },
        { name: 'phy_3', dir: '/p/demo_Runs/phy_3', files: ['/p/demo_Runs/phy_3/route.qor'],
          values: { 'Setup WNS': 171, 'Setup TNS': 0, 'area:lut': 4321 } },
    ];
    const html = renderTrendHtml({ runs });
    eq((html.match(/<circle/g) || []).length, 3, 'three points');
    ok(html.includes('openReportFile'), 'points link to the run report');
    ok(html.includes('phy_3'), 'run names are on the axis');
    ok(html.includes('Setup WNS'), 'default metric is Setup WNS');

    // 没有 run 时给可读提示，而不是空图
    const empty = renderTrendHtml({ runs: [] });
    ok(/No flow runs/i.test(empty), 'empty state is explained');
});


// X-B7: Copilot 只读工具。名字唯一、都有描述/schema，invoke 走 rpc 并返回 JSON 文本。
test('lm tools: read-only slice maps to the right protocol calls', async () => {
    const calls = [];
    const rpc = {
        request: async (method, params) => { calls.push({ method, params }); return { ok: true, method }; },
        execute: async (command, args) => { calls.push({ command, args }); return { ok: true, command }; },
    };
    const tools = createLmTools(rpc);
    eq(tools.length, 5, 'five curated read-only tools');
    const names = new Set(tools.map((tool) => tool.name));
    eq(names.size, tools.length, 'tool names are unique');
    for (const tool of tools) {
        ok(tool.description && tool.description.length > 10, `description for ${tool.name}`);
        ok(tool.inputSchema && tool.inputSchema.type === 'object', `schema for ${tool.name}`);
    }

    const info = await tools.find((t) => t.name === 'fpgalsp_project_info').invoke({});
    ok(JSON.parse(info).command === 'fpgalsp.info', 'project info goes through executeCommand');

    await tools.find((t) => t.name === 'fpgalsp_module_hierarchy').invoke({ module: 'top', limit: 5 });
    const children = calls.find((call) => call.method === 'fpgaLsp/moduleChildren');
    ok(children && children.params.module === 'top' && children.params.limit === 5,
        'hierarchy call carries module/limit');

    // 注册路径：没有 lm API 时返回空数组（不抛），有 API 时逐个注册并返回 disposable
    eq(registerLmTools({}, rpc).length, 0, 'no lm API -> no tools');
    const registered = [];
    const fakeVscode = {
        lm: { registerTool: (name, impl) => { registered.push({ name, impl }); return { dispose() {} }; } },
        LanguageModelToolResult: class { constructor(parts) { this.parts = parts; } },
        LanguageModelTextPart: class { constructor(text) { this.text = text; } },
    };
    const disposables = registerLmTools(fakeVscode, rpc);
    eq(disposables.length, 5, 'registers every tool');
    eq(registered[0].name, 'fpgalsp_project_info', 'first tool registered by name');
    const result = await registered[0].impl.invoke({ input: {} });
    ok(result.parts[0].text.includes('fpgalsp.info'), 'invoke wraps rpc output as text');
});


// X-B3: 引脚约束表。每行可跳转到 .adc 的确切行，有诊断的行带徽标。
test('constraints html: rows jump to the adc line and flag diagnostics', () => {
    const html = renderConstraintHtml({
        project: 'demo',
        files: [{
            file: 'Y:/p/constrains/pin.adc',
            rows: [
                { line: 3, port: 'led[0]', location: 'P76', iostandard: 'LVCMOS33', pulltype: null, issues: 0 },
                { line: 4, port: 'key[0]', location: '', iostandard: 'LVCMOS33', pulltype: null, issues: 2 },
            ],
        }],
    });
    ok(html.includes('openSourceLocation'), 'line numbers link to the source');
    ok(html.includes('pin.adc:4'), 'link text uses 1-based line numbers');
    ok(html.includes('P76') && html.includes('LVCMOS33'), 'assignment values are shown');
    ok(html.includes('class="badge"'), 'rows with diagnostics carry a badge');
    ok(html.includes('has-issue'), 'flagged rows are highlighted');
    const empty = renderConstraintHtml({ files: [] });
    ok(empty.includes('No <code>.adc</code>'), 'empty state is explained');
});
