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
const { FlowController } = require('../src/flowController');
const { renderReportsHtml } = require('../src/reportsHtml');
const { ReportsPanel } = require('../src/reportsPanel');

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
            if (payload && payload.command === 'fpgalsp.runFlow') {
                return { ok: true, runId: 'r1', stages: payload.arguments[0].stages };
            }
            if (payload && payload.command === 'fpgalsp.cancelFlow') return { ok: true };
            return { ok: true };
        },
    };

    // extension.js is loaded lazily so the mocked vscode-languageclient above is used.
    const extension = require('../extension.js');
    const context = {
    extensionPath: path.join(__dirname, '..'),
    subscriptions: [],
    workspaceState: { get: () => undefined, update: () => Promise.resolve() },
};
    extension.activate(context);
    await new Promise((resolve) => setTimeout(resolve, 40));

    eq(mock.__state.treeViews.length, 3, 'tree view count');
    eq(mock.__state.treeViews[0].viewId, 'fpgaLsp.modulesView');
    eq(mock.__state.treeViews[1].viewId, 'fpgaLsp.flowView');

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

    const flowChannel = mock.__state.outputChannels.find((channel) => channel.name === 'FPGA Flow');
    ok(flowChannel, 'FPGA Flow output channel exists');
    ok(flowChannel.lines.some((line) => line.includes('done')), 'flow done logged');
    ok(mock.__state.statusBarItems[0] && /done/.test(mock.__state.statusBarItems[0].text), 'status bar updated');

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
});

// ------------------------------------------------------------------ run

(async () => {
    let passed = 0;
    let failed = 0;
    for (const entry of tests) {
        mock.__reset();
        FakeLanguageClient.responses = {};
        FakeLanguageClient.requests.length = 0;
        FakeLanguageClient.instance = undefined;
        try {
            await entry.fn();
            passed += 1;
            console.log(`PASS  ${entry.name}`);
        } catch (err) {
            failed += 1;
            console.error(`FAIL  ${entry.name}`);
            console.error(`      ${err && err.message ? err.message : err}`);
        }
    }
    console.log('');
    console.log(`${passed} passed, ${failed} failed, ${tests.length} total`);
    if (failed > 0) process.exitCode = 1;
})();

