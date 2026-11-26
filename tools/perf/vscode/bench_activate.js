// End-to-end plugin-side measurement of `activate()` and the editor/notification event chains.
//
// Unlike bench_events.js (which drives the providers directly) this loads the real
// `vscode-extension/extension.js` under the repo's vscode mock, with every `onDid*` event
// replaced by an emitter we can fire. That is the only way to measure the *wiring*:
//   save / active-editor change -> 150 ms debounce -> provider.requestRefresh (another 150 ms)
//   -> one `fpgaLsp/moduleTree` request -> tree re-render.
//
//   node --expose-gc bench_activate.js
//
// Output: csv lines + tools/perf/vscode/results/activate.json

'use strict';

const path = require('path');
const harness = require('./lib/harness');

const mock = harness.installVscodeMock();

const WS = path.join(harness.WORKDIR, 'project').replace(/\\/g, '/');

// IMPORTANT: the extension writes `<workspace>/.fpgalsp/logs/extension.log` on *every* `diag()`
// call (src/launchCheck.js::appendLog does mkdirSync + appendFileSync). We therefore point the
// fake workspace at a %TEMP% copy of the corpus, never at C:/work/CorpusAlpha (read-only rule).
const WORKSPACE = path.join(harness.WORKDIR, 'project');
if (!require('fs').existsSync(path.join(WORKSPACE, 'demo_project.al'))) {
    throw new Error(`missing temp workspace ${WORKSPACE}; run: node prep_workspace.js`);
}

// The highlight report calls vscode.extensions.getExtension(); the repo mock does not model it.
if (!mock.extensions) {
    mock.extensions = { all: [], getExtension: () => ({ packageJSON: { version: '0.0.0-perf' } }) };
}

// ---------------------------------------------------------------------------
// our own event emitters (the mock creates them at module load and does not expose them)
// ---------------------------------------------------------------------------
function makeEmitter() {
    const handlers = [];
    return {
        handlers,
        event(listener) {
            handlers.push(listener);
            return { dispose() { const at = handlers.indexOf(listener); if (at >= 0) handlers.splice(at, 1); } };
        },
        fire(value) {
            for (const handler of [...handlers]) handler(value);
        },
    };
}

const emitters = {
    save: makeEmitter(),
    open: makeEmitter(),
    change: makeEmitter(),
    close: makeEmitter(),
    activeEditor: makeEmitter(),
    config: makeEmitter(),
    folders: makeEmitter(),
};
mock.workspace.onDidSaveTextDocument = emitters.save.event;
mock.workspace.onDidOpenTextDocument = emitters.open.event;
mock.workspace.onDidChangeTextDocument = emitters.change.event;
mock.workspace.onDidCloseTextDocument = emitters.close.event;
mock.workspace.onDidChangeConfiguration = emitters.config.event;
mock.workspace.onDidChangeWorkspaceFolders = emitters.folders.event;
mock.window.onDidChangeActiveTextEditor = emitters.activeEditor.event;

// ---------------------------------------------------------------------------
// fake language client that records every request (the perf question is "how many, when")
// ---------------------------------------------------------------------------
const Module = require('module');
const originalLoad = Module._load;

function moduleTreePayload() {
    const root = {
        module: 'top',
        instance: '',
        file: `${WS}/src/top.v`,
        range: { start: { line: 14, character: 7 }, end: { line: 14, character: 10 } },
        defined: true,
        blackbox: false,
        recursive: false,
        portCount: 25,
        usedAt: null,
        children: [],
    };
    for (let i = 0; i < 12; i += 1) {
        const child = {
            module: `sub_${i}`,
            instance: `u_sub_${i}`,
            file: `${WS}/src/sub_${i}.v`,
            range: { start: { line: 1, character: 7 }, end: { line: 1, character: 12 } },
            defined: true,
            blackbox: false,
            recursive: false,
            portCount: 8,
            usedAt: null,
            children: [],
        };
        for (let j = 0; j < 3; j += 1) {
            child.children.push({
                module: `leaf_${i}_${j}`,
                instance: `u_leaf_${i}_${j}`,
                file: `${WS}/src/leaf_${i}_${j}.v`,
                range: { start: { line: 0, character: 7 }, end: { line: 0, character: 15 } },
                defined: true,
                blackbox: false,
                recursive: false,
                portCount: 3,
                usedAt: null,
                children: [],
            });
        }
        root.children.push(child);
    }
    return { ok: true, project: 'demo_project', al: `${WS}/demo_project.al`, root, modulesSeen: 36, truncated: false };
}

const summaryReports = {
    ok: true,
    section: 'summary',
    project: 'demo_project',
    al: `${WS}/demo_project.al`,
    generatedAt: Math.floor(Date.now() / 1000),
    artifacts: { bit: `${WS}/demo_project.bit`, runs: ['run_1'] },
    files: {
        qor: `${WS}/.fpgalsp/run/syn/qor.rpt`,
        timing: `${WS}/.fpgalsp/run/syn/timing.rpt`,
        area: `${WS}/.fpgalsp/run/syn/area.rpt`,
        flow_status: `${WS}/.fpgalsp/run/syn/status.txt`,
    },
    flow: { running: false, stage: 'idle', elapsed: 0, lastMessage: '' },
    sections: ['qor', 'timing', 'area'],
};

const requests = [];

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
    async start() { this.running = true; }
    isRunning() { return this.running; }
    onNotification(method, handler) {
        if (this.notificationHandlers[method]) {
            // duplicate registration on the same client: would double the per-event work
            FakeLanguageClient.duplicateNotifications.push(method);
        }
        this.notificationHandlers[method] = handler;
    }
    async sendRequest(method, params) {
        requests.push({ method, params, at: process.hrtime.bigint() });
        if (method === 'fpgaLsp/moduleTree') return moduleTreePayload();
        if (method === 'fpgaLsp/reports') return summaryReports;
        if (method === 'workspace/executeCommand') {
            const command = params && params.command;
            if (command === 'fpgalsp.info') {
                return { ok: true, project: 'demo_project', al: `${WS}/demo_project.al`, top: 'top', device: 'EF2L45LG144B', files: { hdl: 21, adc: 1, sdc: 1 }, warnings: [] };
            }
            return { ok: true };
        }
        if (method === 'initialize') return { capabilities: {} };
        return { ok: true, data: [] };
    }
    async stop() { this.running = false; }
}
FakeLanguageClient.instance = undefined;
FakeLanguageClient.duplicateNotifications = [];

Module._load = function patched(request, parent, isMain) {
    if (request === 'vscode') return mock;
    if (request === 'vscode-languageclient/node') {
        return { LanguageClient: FakeLanguageClient, TransportKind: { stdio: 0 } };
    }
    return originalLoad.call(this, request, parent, isMain);
};

function wait(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function main() {
    const results = { generatedAt: new Date().toISOString(), activate: {}, requests: {}, events: {}, csv: [] };
    const csv = [];
    const before = harness.cpuSnapshot();

    // Fake workspace: one open HDL document + an active editor pointing at it.
    const document = {
        uri: mock.Uri.file(`${WS}/src/top.v`),
        fileName: `${WS}/src/top.v`,
        languageId: 'verilog',
        version: 1,
        getText: () => 'module top;\nendmodule\n',
    };
    mock.workspace.workspaceFolders = [{ uri: mock.Uri.file(WORKSPACE.replace(/\\/g, '/')), name: 'corpus_alpha', index: 0 }];
    mock.workspace.textDocuments = [];
    mock.window.activeTextEditor = { document, selection: { anchor: 0, active: 0, start: { line: 0, character: 0 }, end: { line: 0, character: 0 } } };

    const extension = require(path.join(harness.EXT, 'extension.js'));
    const context = {
        extensionPath: harness.EXT,
        subscriptions: [],
        workspaceState: { get: () => undefined, update: () => Promise.resolve() },
        globalState: { get: () => undefined, update: () => Promise.resolve() },
    };

    // ---- activation ---------------------------------------------------------------------
    const started = process.hrtime.bigint();
    extension.activate(context);
    const activateSyncMs = Number(process.hrtime.bigint() - started) / 1e6;
    // let `client.start().then(...)` + the three startup requests run
    await wait(120);
    results.activate.syncMs = activateSyncMs;
    results.activate.subscriptions = context.subscriptions.length;
    results.activate.commands = mock.__state.commands.size;
    results.activate.treeViews = mock.__state.treeViews.length;
    results.activate.notificationHandlers = Object.keys(FakeLanguageClient.instance.notificationHandlers);
    results.activate.duplicateNotifications = FakeLanguageClient.duplicateNotifications;
    results.requests.startup = requests.map((entry) => entry.method + (entry.params && entry.params.command ? ':' + entry.params.command : ''));
    csv.push(`activate,sync,ms,${activateSyncMs.toFixed(3)}`);
    csv.push(`activate,subscriptions,count,${results.activate.subscriptions}`);
    csv.push(`activate,startupRequests,count,${results.requests.startup.length}`);

    // ---- cost of the logging primitive the event handlers use (src/launchCheck.js::appendLog) --
    {
        const fs = require('fs');
        const logDir = path.join(harness.WORKDIR, 'logs');
        fs.mkdirSync(logDir, { recursive: true });
        const logFile = path.join(logDir, 'append-probe.log');
        fs.writeFileSync(logFile, '');
        const appendBench = harness.benchSync(() => {
            fs.mkdirSync(logDir, { recursive: true });
            fs.appendFileSync(logFile, `[${new Date().toISOString()}] indexChanged -> in-place\n`, 'utf8');
        }, { label: 'syncAppend.extensionLog(temp)', rounds: 3, iterations: 21 });
        results.benches = { syncAppendTemp: appendBench };
        csv.push(...appendBench.csv);
        fs.rmSync(logFile, { force: true });

        // Same append, but on the Y: volume (where the real workspace lives). This is what makes
        // the per-event `diag()` expensive: when the fake workspace pointed at C:/work/CorpusAlpha the
        // indexChanged handler measured ~6.5 ms instead of ~0.45 ms.
        const yLog = path.join(harness.REPO, 'tools', 'perf', 'vscode', 'results', '.append-probe.log');
        fs.writeFileSync(yLog, '');
        const appendY = harness.benchSync(() => {
            fs.mkdirSync(path.dirname(yLog), { recursive: true });
            fs.appendFileSync(yLog, `[${new Date().toISOString()}] indexChanged -> in-place\n`, 'utf8');
        }, { label: 'syncAppend.extensionLog(Y:)', rounds: 3, iterations: 21 });
        results.benches.syncAppendY = appendY;
        csv.push(...appendY.csv);
        fs.rmSync(yLog, { force: true });
    }

    // ---- save -> moduleTree request (the double-debounce question) -----------------------
    requests.length = 0;
    const saveStart = process.hrtime.bigint();
    emitters.save.fire(document);
    // poll until the first moduleTree request appears (max 2 s)
    let moduleTreeAt = 0;
    for (let i = 0; i < 1000; i += 1) {
        await wait(2);
        const hit = requests.find((entry) => entry.method === 'fpgaLsp/moduleTree');
        if (hit) {
            moduleTreeAt = Number(hit.at - saveStart) / 1e6;
            break;
        }
    }
    results.events.saveToModuleTreeRequestMs = moduleTreeAt;
    results.events.saveRequests = requests.map((entry) => entry.method);
    csv.push(`event,save->moduleTreeRequest,ms,${moduleTreeAt.toFixed(1)}`);

    // coalescing: 5 saves inside one debounce window -> still one request
    requests.length = 0;
    for (let i = 0; i < 5; i += 1) emitters.save.fire(document);
    await wait(400);
    results.events.fiveSavesRequestCount = requests.filter((entry) => entry.method === 'fpgaLsp/moduleTree').length;
    csv.push(`event,fiveSaves,moduleTreeRequests,${results.events.fiveSavesRequestCount}`);

    // ---- active editor switch -> moduleTree request -------------------------------------
    requests.length = 0;
    const editorStart = process.hrtime.bigint();
    emitters.activeEditor.fire({ document, selection: { anchor: 0, active: 0, start: { line: 0, character: 0 }, end: { line: 0, character: 0 } } });
    let editorMs = 0;
    for (let i = 0; i < 1000; i += 1) {
        await wait(2);
        const hit = requests.find((entry) => entry.method === 'fpgaLsp/moduleTree');
        if (hit) { editorMs = Number(hit.at - editorStart) / 1e6; break; }
    }
    results.events.editorSwitchToRequestMs = editorMs;
    csv.push(`event,activeEditor->moduleTreeRequest,ms,${editorMs.toFixed(1)}`);

    // ---- indexChanged (in-place) --------------------------------------------------------
    const indexHandler = FakeLanguageClient.instance.notificationHandlers['fpgaLsp/indexChanged'];
    requests.length = 0;
    const moduleProvider = mock.__state.treeViews[0].treeDataProvider;
    const indexCost = harness.benchSync(() => {
        indexHandler({ al: `${WS}/demo_project.al`, modules: [{ module: 'sub_3', portCount: 12 }] });
    }, { label: 'indexChanged.inPlace(handler)', rounds: 3, iterations: 21 });
    results.events.indexChangedInPlaceMs = indexCost.aggregate.p50;
    results.events.indexChangedRequests = requests.filter((entry) => entry.method === 'fpgaLsp/moduleTree').length;
    csv.push(...indexCost.csv);

    // ---- indexChanged as the real server sends it: {al, generation, paths} (no modules) -----
    // (src/server/lsp_server.cpp:449-453). This is the path a user actually gets on save.
    requests.length = 0;
    const pushStart = process.hrtime.bigint();
    indexHandler({ al: `${WS}/demo_project.al`, generation: 2, paths: [`${WS}/src/top.v`] });
    let pushMs = 0;
    for (let i = 0; i < 1000; i += 1) {
        await wait(2);
        const hit = requests.find((entry) => entry.method === 'fpgaLsp/moduleTree');
        if (hit) { pushMs = Number(hit.at - pushStart) / 1e6; break; }
    }
    results.events.pushToModuleTreeRequestMs = pushMs;
    csv.push(`event,indexChanged(paths-only)->moduleTreeRequest,ms,${pushMs.toFixed(1)}`);

    // ---- the real save path = save event + the server push ~5 ms later ---------------------
    requests.length = 0;
    const bothStart = process.hrtime.bigint();
    emitters.save.fire(document);
    setTimeout(() => indexHandler({ al: `${WS}/demo_project.al`, generation: 3, paths: [`${WS}/src/top.v`] }), 5);
    let bothMs = 0;
    for (let i = 0; i < 1000; i += 1) {
        await wait(2);
        const hit = requests.find((entry) => entry.method === 'fpgaLsp/moduleTree');
        if (hit) { bothMs = Number(hit.at - bothStart) / 1e6; break; }
    }
    results.events.savePlusPushToRequestMs = bothMs;
    csv.push(`event,save+push->moduleTreeRequest,ms,${bothMs.toFixed(1)}`);

    // ---- foreign event (must be free) --------------------------------------------------
    const foreignCost = harness.benchSync(() => {
        indexHandler({ al: 'X:/other/proj.al', paths: ['X:/other/src/a.v'] });
    }, { label: 'indexChanged.foreign', rounds: 3, iterations: 21 });
    results.events.indexChangedForeignMs = foreignCost.aggregate.p50;
    csv.push(...foreignCost.csv);

    // ---- per-open highlight report (debugHighlight RPC 2.5 s after every open) ---------
    requests.length = 0;
    emitters.open.fire(document);
    await wait(2700);
    results.events.openTriggers = requests.map((entry) => entry.method + (entry.params && entry.params.command ? ':' + entry.params.command : ''));
    results.events.highlightReportRequests = requests.filter((entry) => entry.method === 'workspace/executeCommand'
        && entry.params && entry.params.command === 'fpgalsp.debugHighlight').length;
    csv.push(`event,open->debugHighlight,requests,${results.events.highlightReportRequests}`);

    // ---- deactivate ---------------------------------------------------------------------
    await extension.deactivate();
    const after = harness.cpuSnapshot();
    results.cpuBusy = harness.cpuBusy(before, after);
    results.csv = csv;
    const target = harness.writeJson('activate.json', results);
    harness.printTable('activate / event wiring', csv);
    console.log(`\nmachine busy during run: ${(results.cpuBusy * 100).toFixed(1)}%`);
    console.log(`wrote ${target}`);
}

if (require.main === module) {
    main().then(() => process.exit(0)).catch((err) => {
        console.error(err && err.stack ? err.stack : err);
        process.exit(1);
    });
}