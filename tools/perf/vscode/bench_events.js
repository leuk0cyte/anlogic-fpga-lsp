// Plugin-side cost of the *event paths*: what the extension host main thread does when a
// server notification or an editor event arrives.
//
// Loads the real provider classes (src/moduleTree.js, reportsPanel.js, reportsTree.js,
// flowTree.js, flowController.js, reportsService.js) under the repo's own vscode mock
// (vscode-extension/test/mock-vscode.js), with fake RPC adapters. Nothing here is a proxy for
// "VS Code internals" -- it measures our code, which is the part we can fix.
//
//   node --expose-gc bench_events.js [--nodes 36] [--nodes-large 1000]
//
// Output: csv lines + tools/perf/vscode/results/events.json

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./lib/harness');

const mock = harness.installVscodeMock();
const { ModuleTreeProvider, INDEX_DEBOUNCE_MS } = require(path.join(harness.EXT, 'src', 'moduleTree.js'));
const { ReportsTreeProvider } = require(path.join(harness.EXT, 'src', 'reportsTree.js'));
const { FlowTreeProvider } = require(path.join(harness.EXT, 'src', 'flowTree.js'));
const { FlowController, FALLBACK_POLL_MS } = require(path.join(harness.EXT, 'src', 'flowController.js'));
const { ReportsService } = require(path.join(harness.EXT, 'src', 'reportsService.js'));
const { ReportsPanel } = require(path.join(harness.EXT, 'src', 'reportsPanel.js'));

const PROJECT_ROOT = '%WORKSPACE%/CorpusAlpha';
const AL = `${PROJECT_ROOT}/demo_project.al`;

function parseArgs(argv) {
    const args = { nodes: 36, nodesLarge: 1000 };
    for (let i = 0; i < argv.length; i += 1) {
        if (argv[i] === '--nodes') { args.nodes = Number(argv[i + 1]); i += 1; }
        if (argv[i] === '--nodes-large') { args.nodesLarge = Number(argv[i + 1]); i += 1; }
    }
    return args;
}

/** A moduleTree payload with `count` nodes (root + children + grandchildren). */
function makeModuleTreePayload(count) {
    const root = {
        module: 'top',
        instance: '',
        file: `${PROJECT_ROOT}/src/top.v`,
        range: { start: { line: 14, character: 7 }, end: { line: 14, character: 10 } },
        defined: true,
        blackbox: false,
        recursive: false,
        portCount: 25,
        usedAt: null,
        children: [],
    };
    const children = Math.max(1, Math.round(count / 4));
    for (let i = 0; i < children; i += 1) {
        const child = {
            module: `sub_${i}`,
            instance: `u_sub_${i}`,
            file: `${PROJECT_ROOT}/src/sub_${i}.v`,
            range: { start: { line: 1, character: 7 }, end: { line: 1, character: 12 } },
            defined: true,
            blackbox: false,
            recursive: false,
            portCount: 8,
            usedAt: {
                uri: `file:///${PROJECT_ROOT}/src/top.v`,
                range: { start: { line: 20 + i, character: 3 }, end: { line: 20 + i, character: 12 } },
            },
            children: [],
        };
        for (let j = 0; j < 3; j += 1) {
            child.children.push({
                module: `leaf_${i}_${j}`,
                instance: `u_leaf_${i}_${j}`,
                file: `${PROJECT_ROOT}/src/leaf_${i}_${j}.v`,
                range: { start: { line: 0, character: 7 }, end: { line: 0, character: 15 } },
                defined: i % 7 !== 0,
                blackbox: i % 7 === 0,
                recursive: false,
                portCount: 3,
                usedAt: null,
                children: [],
            });
        }
        root.children.push(child);
    }
    return { ok: true, project: 'demo_project', al: AL, root, modulesSeen: count, truncated: false };
}

/** Walk the tree exactly like VS Code's TreeView does for expanded nodes. */
function renderVisibleTree(provider, expandAll) {
    let items = 0;
    const stack = [{ node: undefined, siblings: provider.getChildren(undefined) }];
    while (stack.length) {
        const frame = stack.pop();
        for (const node of frame.siblings) {
            provider.getTreeItem(node);
            items += 1;
            if (expandAll) {
                const children = provider.getChildren(node);
                if (children.length) stack.push({ node, siblings: children });
            }
        }
    }
    return items;
}

function makeRpc(responses) {
    const calls = [];
    return {
        calls,
        request: async (method, params) => {
            calls.push({ method, params });
            const handler = responses[method];
            return typeof handler === 'function' ? handler(params) : handler;
        },
        execute: async (command) => {
            calls.push({ method: 'execute:' + command });
            return { ok: true, flow: { running: false, stage: 'idle' } };
        },
        onNotification: () => undefined,
    };
}

function loadPayload(key) {
    const manifestPath = path.join(harness.WORKDIR, 'payload-manifest.json');
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    return JSON.parse(fs.readFileSync(manifest.files[key].path, 'utf8'));
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const before = harness.cpuSnapshot();
    const results = { generatedAt: new Date().toISOString(), constants: { INDEX_DEBOUNCE_MS, FALLBACK_POLL_MS }, benches: {}, csv: [] };
    const csv = [];

    const summaryPayload = loadPayload('summary');
    const treeSmall = makeModuleTreePayload(args.nodes);
    const treeLarge = makeModuleTreePayload(args.nodesLarge);

    // ------------------------------------------------------------------ 1. module tree
    for (const [label, payload] of [['36nodes', treeSmall], ['1000nodes', treeLarge]]) {
        const provider = new ModuleTreeProvider(makeRpc({}));
        provider.rpc = makeRpc({ 'fpgaLsp/moduleTree': payload });
        await provider.refresh();

        // (a) in-place patch: `indexChanged` carrying modules[] (no server request)
        const inPlace = harness.benchSync(() => {
            provider.applyIndexChanged({ al: AL, modules: [{ module: 'sub_3', portCount: 12, defined: true }] });
            renderVisibleTree(provider, false);
        }, { label: `moduleTree.inPlace.${label}`, rounds: 3, iterations: 21 });
        results.benches[`moduleTreeInPlace_${label}`] = inPlace;
        csv.push(...inPlace.csv);

        // (b) full rebuild of the provider side (a request happened; VS Code re-queries the tree)
        const refresh = harness.benchSync(() => {
            const data = { ok: true, project: 'demo_project', al: AL, root: payload.root };
            provider.payload = data;
            provider.rootNode = provider._wrap(data.root, undefined, 'root');
            provider.fire();
            renderVisibleTree(provider, false);
        }, { label: `moduleTree.rebuild+render.${label}`, rounds: 3, iterations: 21 });
        results.benches[`moduleTreeRebuild_${label}`] = refresh;
        csv.push(...refresh.csv);

        // (c) worst case: every node expanded (user collapsed nothing)
        const expanded = harness.benchSync(() => {
            provider._rootExpanded = true;
            renderVisibleTree(provider, true);
        }, { label: `moduleTree.renderAllExpanded.${label}`, rounds: 3, iterations: 21 });
        results.benches[`moduleTreeRenderAll_${label}`] = expanded;
        csv.push(...expanded.csv);

        provider.dispose();
    }

    // debounce flush: requestRefresh() -> one real refresh after 150 ms
    {
        const provider = new ModuleTreeProvider(makeRpc({ 'fpgaLsp/moduleTree': treeSmall }));
        const flush = await harness.benchAsync(async () => {
            provider.requestRefresh(`${PROJECT_ROOT}/src/top.v`, { reason: 'saved' });
            await new Promise((resolve) => setTimeout(resolve, INDEX_DEBOUNCE_MS + 10));
        }, { label: 'moduleTree.debouncedFlush(150ms window)', rounds: 3, iterations: 11 });
        results.benches.moduleTreeDebouncedFlush = flush;
        csv.push(...flush.csv);
        results.moduleTreeDebounce = { requests: provider.stats.requests, debounced: provider.stats.debounced };
        provider.dispose();
    }

    // The push path *without* any debounce: this is what `applyIndexChanged()` would cost if its
    // refresh branch called `refresh()` directly instead of `requestRefresh()` (see VSCODE.md §B).
    {
        const provider = new ModuleTreeProvider(makeRpc({ 'fpgaLsp/moduleTree': treeSmall }));
        const direct = await harness.benchAsync(async () => {
            await provider.refresh(`${PROJECT_ROOT}/src/top.v`, { reason: 'indexChanged' });
            renderVisibleTree(provider, false);
        }, { label: 'moduleTree.refreshDirect(fake rpc,36nodes)', rounds: 3, iterations: 21 });
        results.benches.moduleTreeRefreshDirect = direct;
        csv.push(...direct.csv);
        provider.dispose();
    }

    // ------------------------------------------------------------------ 2. reports panel
    {
        const rpc = makeRpc({});
        const service = new ReportsService(rpc, { now: () => Date.now() });
        // Pre-seed the cache so every refresh measures the *render* path, not the RPC.
        const panel = new ReportsPanel({ rpc, reportsService: service, stage: 'syn' });
        await panel.show();
        panel.stats.html = 0;
        panel.stats.patch = 0;

        // measure the whole refresh (await fetch -> build view -> patch/paint -> postMessage)
        const refresh = await harness.benchAsync(async () => {
            service._cache.set('summary', { at: Date.now(), payload: summaryPayload });
            await panel.refresh({ force: false });
        }, { label: 'reportsPanel.refresh(summary,tty-cached)', rounds: 3, iterations: 21 });
        results.benches.panelRefreshSummary = refresh;
        csv.push(...refresh.csv);

        // measure the patch builder only (renderReportSection x sections + postMessage)
        const patch = harness.benchSync(() => {
            panel._patch(summaryPayload, { ok: true, stage: 'syn' });
        }, { label: 'reportsPanel.patch.build(summary)', rounds: 3, iterations: 21 });
        results.benches.panelPatchSummary = patch;
        csv.push(...patch.csv);
        results.panelStats = { ...panel.stats };
        panel.dispose();
    }

    // same for the 26 MB payload: shows what "first paint / patch" would cost if the panel
    // ever received the full report (old server, or a future per-tab fetch).
    {
        const fullPayload = loadPayload('full');
        const rpc = makeRpc({});
        const service = new ReportsService(rpc, { now: () => Date.now() });
        const panel = new ReportsPanel({ rpc, reportsService: service, stage: 'syn' });
        await panel.show();
        const firstPaint = harness.benchSync(() => {
            panel._painted = false;
            panel._paint(fullPayload, undefined, '');
        }, { label: 'reportsPanel.firstPaint(full-26mb)', rounds: 3, iterations: 11 });
        results.benches.panelFirstPaintFull = firstPaint;
        csv.push(...firstPaint.csv);
        const patchFull = harness.benchSync(() => {
            panel._patch(fullPayload, { ok: true, stage: 'syn' });
        }, { label: 'reportsPanel.patch.build(full-26mb)', rounds: 3, iterations: 11 });
        results.benches.panelPatchFull = patchFull;
        csv.push(...patchFull.csv);
        panel.dispose();
    }

    // ------------------------------------------------------------------ 3. trees that also
    // fetch on every event (reports tree + flow tree)
    {
        const rpc = makeRpc({
            'fpgaLsp/reports': summaryPayload,
            'fpgalsp.info': { ok: true, project: 'demo_project', al: AL, files: { hdl: 21, adc: 1, sdc: 1 } },
        });
        const service = new ReportsService(rpc, { now: () => Date.now() });
        const reportsTree = new ReportsTreeProvider(rpc, service);
        const reportsRefresh = await harness.benchAsync(async () => {
            service._cache.clear();
            await reportsTree.refresh({});
            reportsTree.getChildren(undefined).forEach((node) => reportsTree.getTreeItem(node));
        }, { label: 'reportsTree.refresh(summary)', rounds: 3, iterations: 21 });
        results.benches.reportsTreeRefresh = reportsRefresh;
        csv.push(...reportsRefresh.csv);

        const flowTree = new FlowTreeProvider(rpc, async () => ({ ok: true, project: 'p', al: AL }), service);
        const flowRefresh = await harness.benchAsync(async () => {
            service._cache.clear();
            await flowTree.refresh({});
            flowTree.getChildren(undefined).forEach((node) => flowTree.getTreeItem(node));
        }, { label: 'flowTree.refresh(summary+info)', rounds: 3, iterations: 21 });
        results.benches.flowTreeRefresh = flowRefresh;
        csv.push(...flowRefresh.csv);

        // flowEvent notification path (during a running flow: this is the hot path)
        const flowEvent = harness.benchSync(() => {
            flowTree.applyFlowEvent({ type: 'stage', stage: 'syn', elapsed: 3.2, message: 'synthesize: mapping' });
            flowTree.getChildren(undefined).forEach((node) => flowTree.getTreeItem(node));
        }, { label: 'flowTree.applyFlowEvent', rounds: 3, iterations: 21 });
        results.benches.flowTreeApplyEvent = flowEvent;
        csv.push(...flowEvent.csv);

        // flowStatus fallback poll (10 s, only while running)
        const controller = new FlowController({
            rpc: makeRpc({ 'fpgaLsp/flowStatus': { ok: true, flow: { running: true, stage: 'syn', elapsed: 12.5 } } }),
            moduleTree: { refresh: async () => undefined },
            flowTree,
            reports: { refresh: async () => undefined, visible: false },
            getProjectInfo: async () => ({ ok: true }),
        });
        const poll = await harness.benchAsync(async () => {
            await controller.pollOnce();
        }, { label: 'flowController.pollOnce(flowStatus)', rounds: 3, iterations: 21 });
        results.benches.flowPollOnce = poll;
        csv.push(...poll.csv);
        results.flowPoll = { fallbackMs: FALLBACK_POLL_MS, pollerPresent: !!controller.poller };
        controller.dispose();
        reportsTree.dispose();
        flowTree.dispose();
    }

    const after = harness.cpuSnapshot();
    results.cpuBusy = harness.cpuBusy(before, after);
    results.csv = csv;
    const target = harness.writeJson('events.json', results);
    harness.printTable('event paths (ms)', csv);
    console.log(`\nmachine busy during run: ${(results.cpuBusy * 100).toFixed(1)}%`);
    console.log(`wrote ${target}`);
}

if (require.main === module) {
    main().catch((err) => {
        console.error(err && err.stack ? err.stack : err);
        process.exitCode = 1;
    });
}