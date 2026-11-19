// VS Code client for fpga-lsp (Anlogic TangDynasty FPGA projects).
//
// Two layers:
//   * the standard LSP client (`vscode-languageclient`) connects to `python -m fpgalsp --stdio`
//     and provides diagnostics/completion/hover/definition/documentSymbol;
//   * the IDE views in ./src add the module hierarchy tree, the project/flow tree with
//     one-click syn/phy buttons, and the reports webview on top of the custom fpga-lsp v2
//     protocol (see ../PROTOCOL-V2.md).
//
// Only this file imports `vscode-languageclient`; the view modules depend on `vscode` alone so
// they can be unit tested without VS Code or node_modules.

'use strict';

const net = require('net');
const path = require('path');
const fs = require('fs');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

const { METHODS, COMMANDS, VSCodeCommands, VIEWS } = require('./src/protocol');
const { resolveServer, resolveServerRoot: locateServerRoot } = require('./src/locateServer');
const { resolveVerible } = require('./src/verible');
const { VeribleDiagnosticsClient } = require('./src/veribleDiagnostics');
const { checkPython, appendLog, flushLogSync } = require('./src/launchCheck');
const {
    ModuleTreeProvider,
    resolveTarget,
    definitionTarget,
    instantiationTarget,
} = require('./src/moduleTree');
const { FlowTreeProvider } = require('./src/flowTree');
const { FlowController } = require('./src/flowController');
const { ReportsPanel } = require('./src/reportsPanel');
const { ReportsService } = require('./src/reportsService');
const { ReportsTreeProvider } = require('./src/reportsTree');
const {
    buildMcpServerOptions,
    buildPortableConfig,
    buildVsCodeConfig,
    buildWorkspaceMcpServerOptions,
} = require('./src/mcpConfig');

let client;
let veribleDiag;
let moduleProvider;
let flowProvider;
let flowController;
let reportsTreeProvider;
let mcpEmitter;
let mcpProviderRegistered = false;
let mcpProviderError = '';
let reportsPanels;
let reportsPanel;

function workspaceFolder() {
    const folders = vscode.workspace.workspaceFolders;
    if (!folders || !folders.length) return undefined;
    // prefer the folder that actually contains a .al project
    for (const folder of folders) {
        if (fs.existsSync(folder.uri.fsPath)) return folder.uri.fsPath;
    }
    return folders[0].uri.fsPath;
}

// Phase 2 / B1：loopback MCP 端口按工作区派生（FNV-1a），同一工作区稳定。
function mcpPortForWorkspace(root) {
    let hash = 2166136261;
    for (let i = 0; i < root.length; i += 1) {
        hash = Math.imul(hash ^ root.charCodeAt(i), 16777619);
    }
    return 45000 + (Math.abs(hash) % 1000);
}

// 探测 LSP 进程的 loopback MCP 是否已就绪；失败则 MCP 定义回退独立进程。
function mcpPortOpen(port, timeoutMs = 300) {
    return new Promise((resolve) => {
        if (!port) return resolve(false);
        const socket = net.connect({ host: '127.0.0.1', port });
        let settled = false;
        const finish = (value) => {
            if (settled) return;
            settled = true;
            socket.destroy();
            resolve(value);
        };
        socket.once('connect', () => finish(true));
        socket.once('error', () => finish(false));
        socket.setTimeout(timeoutMs, () => finish(false));
    });
}

function resolveServerRoot(context) {
    const configured = vscode.workspace.getConfiguration('fpgaLsp').get('serverRoot') || '';
    const resolved = locateServerRoot({ extensionPath: context.extensionPath, configured });
    if (resolved.source === 'missing' || resolved.source === 'setting-invalid') {
        vscode.window.showErrorMessage(
            'FPGA: language server (fpgalsp) not found. Set fpgaLsp.serverRoot to the directory containing fpgalsp/' +
            ' (this repo: Y:\\fpga_lsp), or install the vsix with the bundled server.');
    }
    return resolved.root || context.extensionPath;
}

/**
 * 决定用哪个服务器：原生 exe 优先，找不到再回退 Python。
 *
 * 原生 exe 由 C++ 重写版提供（`fpgalsp-native.exe`）：
 *   * 启动快（无 Python 解释器 + import 开销）
 *   * 不依赖用户机器上的 Python 环境
 * 过渡期保留 Python 回退，避免"扩展升级了但还没构建 exe"的用户突然用不了。
 */
function resolveLanguageServer(context) {
    const config = vscode.workspace.getConfiguration('fpgaLsp');
    return resolveServer({
        extensionPath: context.extensionPath,
        configuredPath: config.get('serverPath') || '',
        configuredRoot: config.get('serverRoot') || '',
    });
}

// NOTE (F6/R1): there used to be a `debounce(fn, delay)` helper here, used only to wrap
// `scheduleModuleRefresh()`. That wrapper stacked a second 150 ms window on top of the module
// tree provider's own 150 ms coalescing window, so every save sat 300 ms away from its
// `fpgaLsp/moduleTree` request (audit/2026-11-19/perf/VSCODE.md, P1). The provider's window is
// the one that actually merges N editor events into one request, so the helper is gone: the
// editor-event fallback calls `requestRefresh()` directly.

function activeDocumentUri() {
    const editor = vscode.window.activeTextEditor;
    if (!editor || !editor.document || !editor.document.uri) return undefined;
    return editor.document.uri.toString();
}

function serverReady() {
    try {
        return !!(client && typeof client.isRunning === 'function' && client.isRunning());
    } catch (_) {
        return false;
    }
}

/** Open a `{uri, range}` target returned by src/moduleTree.js. */
async function openTarget(target) {
    if (!target || !target.uri) {
        vscode.window.showInformationMessage('FPGA: nothing to open for this node.');
        return;
    }
    try {
        const document = await vscode.workspace.openTextDocument(target.uri);
        const editor = await vscode.window.showTextDocument(document, {
            preview: false,
            selection: target.range,
        });
        if (editor && target.range && typeof editor.revealRange === 'function') {
            editor.revealRange(target.range, vscode.TextEditorRevealType.InCenter);
        }
    } catch (err) {
        vscode.window.showErrorMessage(
            `FPGA: cannot open ${target.uri.fsPath || target.uri}: ${(err && err.message) || err}`
        );
    }
}

/** Tree-view context-menu commands receive the tree element; tree `command`s receive a target. */
function rawFrom(arg) {
    if (!arg) return undefined;
    if (arg.raw && typeof arg.raw === 'object') return arg.raw;
    if (arg.kind === 'module') return arg.raw;
    return arg;
}

function asTarget(arg) {
    if (!arg) return undefined;
    if (arg.uri && arg.range) return arg;
    return resolveTarget(rawFrom(arg));
}

/** Context-menu commands receive the tree element; command links pass a plain path string. */
function pathFrom(arg) {
    if (!arg) return undefined;
    if (typeof arg === 'string') return arg;
    if (typeof arg.path === 'string') return arg.path;
    const raw = rawFrom(arg);
    if (raw && typeof raw.file === 'string') return raw.file;
    if (raw && raw.usedAt && typeof raw.usedAt.file === 'string') return raw.usedAt.file;
    return undefined;
}

async function copyText(text) {
    if (vscode.env && vscode.env.clipboard && typeof vscode.env.clipboard.writeText === 'function') {
        await vscode.env.clipboard.writeText(text);
        return;
    }
    await vscode.commands.executeCommand('clipboard.writeText', text);
}

function activate(context) {
    const config = () => vscode.workspace.getConfiguration('fpgaLsp');
    // [0.11.0] 原生版：resolveServer() 永不返回 python（见 src/locateServer.js）。
    // python() 仍被 MCP 配置与部分兼容字段引用，但**只作为字符串**出现在日志/配置里，
    // 不会启动任何 Python 进程。
    const python = () => 'python';
    const server = resolveLanguageServer(context);
    const isNative = server.kind === 'native';
    // 原生 exe 没有"serverRoot"概念，但 MCP/日志路径仍需要一个稳定的工作目录
    const serverRoot = path.dirname(server.exe);
    const cwd = workspaceFolder() || serverRoot;

    const env = Object.assign({}, process.env);

    const tdPath = config().get('tdPath') || '';
    const logFileSetting = config().get('logFile') || '';
    const logFile = logFileSetting || path.join(cwd, '.fpgalsp', 'logs', 'server.log');

    // 两种服务器的参数形状不同：
    //   native: fpgalsp-native.exe --stdio [--td <root>] [--log <file>]
    //   python: python -m fpgalsp --stdio [--td <root>] --log <file>
    const args = ['--stdio'];
    if (tdPath) args.push('--td', tdPath);
    // Phase 2 / B1：LSP 进程内 loopback MCP；MCP provider 用 --attach-port 接入。
    const mcpPort = isNative ? mcpPortForWorkspace(cwd) : 0;
    if (isNative && mcpPort) {
        args.push('--mcp-port', String(mcpPort));
        if (config().get('mcp.allowFlow') === true) args.push('--allow-flow');
        if (config().get('mcp.allowWrite') === true) args.push('--allow-write');
    }
    args.push('--log', logFile);

    const serverOptions = {
        command: server.exe,
        args: args,
        transport: TransportKind.stdio,
        options: { cwd: cwd, env: env },
    };

    // everything about the launch goes to <workspace>/.fpgalsp/logs/extension.log, because
    // "Server process exited with code N" alone is impossible to act on.
    const extLog = path.join(cwd, '.fpgalsp', 'logs', 'extension.log');
    const diag = (message) => appendLog(extLog, message);
    diag(`activate ext=${context.extensionPath} kind=${server.kind} source=${server.source} ` +
        `${isNative ? 'exe=' + server.exe : 'serverRoot=' + serverRoot} cwd=${cwd}`);
    if (isNative) {
        // 原生 exe：预检只需确认文件在（没有 Python 依赖可查）
        if (!fs.existsSync(server.exe)) {
            vscode.window.showErrorMessage(
                `FPGA: language server binary not found at ${server.exe}`);
        }
    } else {
        // Preflight in parallel with the client start: if the interpreter cannot import fpgalsp we want
        // to say exactly why instead of letting the client report a bare exit code.
        checkPython({ python: python(), env, cwd }).then((result) => {
            diag('preflight: ' + JSON.stringify(result));
            if (!result.ok) {
                vscode.window.showErrorMessage(
                    `FPGA: unable to start the language server (${result.python}). ${result.error}`,
                    'Open Log'
                ).then((choice) => {
                    if (choice === 'Open Log') vscode.commands.executeCommand(VSCodeCommands.showLogs);
                });
            }
        }).catch((err) => diag('preflight crashed: ' + (err && err.stack ? err.stack : err)));
    }

    const clientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'verilog' },
            { scheme: 'file', language: 'systemverilog' },
            { scheme: 'file', language: 'vhdl' },
            { scheme: 'file', language: 'adc' },
            { scheme: 'file', language: 'sdc' },
            { scheme: 'file', language: 'al-project' },
            // pattern matches keep working when a .v/.vhd file is plaintext (no Verilog extension
            // installed) -- VS Code only reports the `language` id after someone claims the file.
            { scheme: 'file', pattern: '**/*.{v,sv,vh,svh}' },
            { scheme: 'file', pattern: '**/*.{vhd,vhdl}' },
            { scheme: 'file', pattern: '**/*.al' },
            { scheme: 'file', pattern: '**/*.adc' },
            { scheme: 'file', pattern: '**/*.sdc' },
        ],
        synchronize: {
            configurationSection: 'fpgaLsp',
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{v,sv,vh,svh,vhd,vhdl,adc,sdc,al}'),
        },
        initializationOptions: {
            fpgaLsp: {
                tdCheckOnSave: config().get('tdCheckOnSave'),
                projectFile: config().get('projectFile') || '',
                tdPath: tdPath,
                veriblePath: config().get('verible.path') || '',
                veribleArgs: config().get('verible.args') || [],
                veribleFormat: config().get('verible.format') !== false,
                veribleLintArgs: config().get('verible.lintArgs') || [],
                veribleAutofix: config().get('verible.autofix') !== false,
                veribleHighlight: config().get('verible.highlight') !== false,
            },
        },
        outputChannelName: 'FPGA LSP',
    };

    client = new LanguageClient('fpgalsp', 'Anlogic FPGA LSP', serverOptions, clientOptions);
    try {
        client.onDidChangeState((event) => diag(`client state: ${event.oldState} -> ${event.newState}`));
    } catch (_) { /* older client */ }

    // -------------------------------------------------------------- rpc adapter

    const rpc = {
        request: (method, params, token) => {
            if (!client) return Promise.reject(new Error('fpga-lsp is not running yet'));
            // The 3rd argument is a vscode.CancellationToken: cancellation is advisory for
            // older servers (they still answer), but the client-side request is dropped.
            return client.sendRequest(method, params === undefined ? {} : params, token);
        },
        execute: (command, commandArgs) => {
            if (!client) return Promise.reject(new Error('fpga-lsp is not running yet'));
            return client.sendRequest(COMMANDS.executeCommand, { command, arguments: commandArgs || [] });
        },
        onNotification: (method, handler) => client.onNotification(method, handler),
    };

    // Phase 2: one shared reports cache for the flow tree + reports panel (single-flight).
    const reportsService = new ReportsService(rpc);

    const getProjectInfo = async () => {
        try {
            return await rpc.execute(COMMANDS.info, []);
        } catch (_) {
            return undefined;
        }
    };

    // ------------------------------------------------------------ MCP server definition
    const buildMcpDefinition = async () => {
        const root = workspaceFolder();
        if (!root) return undefined;
        let project = '';
        try {
            const info = await getProjectInfo();
            if (info && info.ok) project = info.al || '';
        } catch (_) { /* the language server may still be starting */ }
        if (!project) project = config().get('projectFile') || '';
        return buildMcpServerOptions({
            root,
            project,
            serverRoot,
            pythonPath: python(),
            nativeExe: isNative ? server.exe : '',
            tdPath: config().get('tdPath') || '',
            logFile: path.join(root, '.fpgalsp', 'logs', 'mcp.log'),
            allowFlow: config().get('mcp.allowFlow') === true,
            allowWrite: config().get('mcp.allowWrite') === true,
            version: context.extension.packageJSON.version,
            existingPythonPath: process.env.PYTHONPATH,
            attachPort: (mcpPort && await mcpPortOpen(mcpPort)) ? mcpPort : 0,
        });
    };
    const registerMcpProvider = () => {
        if (!vscode.lm || typeof vscode.lm.registerMcpServerDefinitionProvider !== 'function'
            || typeof vscode.McpStdioServerDefinition !== 'function') {
            mcpProviderRegistered = false;
            mcpProviderError = 'MCP extension API is not available in this VS Code version';
            diag(mcpProviderError);
            return;
        }
        mcpEmitter = new vscode.EventEmitter();
        try {
            const disposable = vscode.lm.registerMcpServerDefinitionProvider('fpgaLsp.mcp', {
                onDidChangeMcpServerDefinitions: mcpEmitter.event,
                provideMcpServerDefinitions: async () => {
                    try {
                        const definition = await buildMcpDefinition();
                        if (!definition) {
                            diag('MCP provider: no workspace folder');
                            return [];
                        }
                        diag(`MCP provider: ${definition.command} ${definition.args.join(' ')}`.slice(0, 500));
                        return [new vscode.McpStdioServerDefinition({
                            label: definition.label,
                            command: definition.command,
                            args: definition.args,
                            cwd: vscode.Uri.file(definition.cwd),
                            env: definition.env,
                            version: definition.version,
                        })];
                    } catch (err) {
                        diag('MCP definition failed: ' + ((err && err.message) || err));
                        return [];
                    }
                },
                resolveMcpServerDefinition: async (definition) => definition,
            });
            context.subscriptions.push(disposable, mcpEmitter);
            diag('MCP server definition provider registered');
            mcpProviderRegistered = true;
            mcpProviderError = '';
        } catch (err) {
            mcpProviderRegistered = false;
            mcpProviderError = (err && err.message) || String(err);
            diag('MCP provider registration failed: ' + mcpProviderError);
            try { mcpEmitter.dispose(); } catch (_) { /* ignore */ }
            mcpEmitter = undefined;
        }
    };
    // Phase 2/A4: client-side attach switch. "Attach" (auto-starting the MCP server for the
    // workspace) is B4 work; here we only keep the knob + feature detection so the behaviour can
    // be turned off without touching the dynamic-provider code path.
    const mcpAttachOnStart = () => {
        try {
            return config().get('mcp.attachOnStart') !== false;
        } catch (_) {
            return true;
        }
    };
    if (!mcpAttachOnStart()) {
        mcpProviderRegistered = false;
        mcpProviderError = 'disabled by fpgaLsp.mcp.attachOnStart';
        diag('MCP attach disabled (fpgaLsp.mcp.attachOnStart = false)');
    } else if (config().get('mcp.dynamicProvider') === true) {
        registerMcpProvider();
    } else {
        mcpProviderRegistered = false;
        mcpProviderError = 'disabled by fpgaLsp.mcp.dynamicProvider (use FPGA: Install MCP Configuration to add a workspace config)';
        diag('MCP dynamic provider disabled; configure a workspace .vscode/mcp.json if needed');
    }
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (!mcpEmitter) return;
        if (event.affectsConfiguration('fpgaLsp.projectFile')
            || event.affectsConfiguration('fpgaLsp.mcp.allowFlow')
            || event.affectsConfiguration('fpgaLsp.mcp.allowWrite')) {
            mcpEmitter.fire();
        }
    }));

    // ------------------------------------------------------------ hints
    const veribleResolved = resolveVerible({ veriblePath: config().get('verible.path') });
    if (!veribleResolved.formatter) {
        const hintKey = 'fpgaLsp.hint.noVerible';
        if (!context.workspaceState.get(hintKey)) {
            context.workspaceState.update(hintKey, true);
            diag('hint: verible not found, basic grammar highlighting only');
            vscode.window.showInformationMessage(
                'FPGA: Verible not detected - basic Verilog/SystemVerilog/VHDL syntax highlighting is enabled; ' +
                'install Verible (or set fpgaLsp.verible.path) for fine-grained highlighting, formatting and lint autofix.',
                'Open Settings'
            ).then((choice) => {
                if (choice === 'Open Settings') {
                    vscode.commands.executeCommand('workbench.action.openSettings', 'fpgaLsp.verible.path');
                }
            });
        }
    }
    // VHDL is outside Verible's scope: tell the user once that only basic highlighting applies
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument((document) => {
        if (document.languageId !== 'vhdl' || veribleResolved.formatter) return;
        const key = 'fpgaLsp.hint.vhdl';
        if (context.workspaceState.get(key)) return;
        context.workspaceState.update(key, true);
        vscode.window.showInformationMessage(
            'FPGA: VHDL uses the built-in basic highlighting; Verible only supports Verilog/SystemVerilog, ' +
            'VHDL formatting and fine-grained tokens require another tool.');
    }));

    // ------------------------------------------------------------ tree + panel

    moduleProvider = new ModuleTreeProvider(rpc);
    flowProvider = new FlowTreeProvider(rpc, getProjectInfo, reportsService);
    reportsTreeProvider = new ReportsTreeProvider(rpc, reportsService);
    const panelOptions = (stage) => ({
        stage,
        rpc,
        // Shared, single-flight reports cache: opening the panel does not re-request what the
        // flow tree just fetched (Phase 2/A4).
        reportsService,
        onRunFlow: (stages) => {
            if (flowController) flowController.run(stages);
        },
        onRunAnalysis: () => vscode.commands.executeCommand(VSCodeCommands.runAnalysis),
        onCompareStrategies: () => vscode.commands.executeCommand(VSCodeCommands.compareStrategies),
    });
    reportsPanels = {
        syn: new ReportsPanel(panelOptions('syn')),
        phy: new ReportsPanel(panelOptions('phy')),
    };
    reportsPanel = {
        get visible() {
            return Object.values(reportsPanels).some((panel) => panel.visible);
        },
        show(stage) {
            return (reportsPanels[stage] || reportsPanels.syn).show();
        },
        refresh() {
            return Promise.allSettled(Object.values(reportsPanels).map((panel) => panel.refresh()));
        },
        dispose() {
            Object.values(reportsPanels).forEach((panel) => panel.dispose());
        },
    };
    flowController = new FlowController({
        rpc,
        moduleTree: moduleProvider,
        flowTree: flowProvider,
        reports: reportsPanel,
        getProjectInfo,
    });

    const modulesView = vscode.window.createTreeView(VIEWS.modules, {
        treeDataProvider: moduleProvider,
        showCollapseAll: true,
    });
    moduleProvider.attachView(modulesView);
    context.subscriptions.push(modulesView);

    const flowView = vscode.window.createTreeView(VIEWS.flow, {
        treeDataProvider: flowProvider,
        showCollapseAll: true,
    });
    flowProvider.attachView(flowView);
    context.subscriptions.push(flowView);

    const reportsView = vscode.window.createTreeView(VIEWS.reports, {
        treeDataProvider: reportsTreeProvider,
        showCollapseAll: true,
    });
    reportsTreeProvider.attachView(reportsView);
    context.subscriptions.push(reportsView);

    // ----------------------------------------------------------------- commands

    const register = (id, handler) => {
        context.subscriptions.push(vscode.commands.registerCommand(id, async (...commandArgs) => {
            try {
                return await handler(...commandArgs);
            } catch (err) {
                vscode.window.showErrorMessage(`FPGA: ${(err && err.message) || err}`);
                return undefined;
            }
        }));
    };

    // ----------------------------------------------------------------- highlight plumbing
    // Restored tabs from an older session can keep languageId "plaintext" even though the
    // extension now contributes .v/.vhd languages; fix those so grammar + language features apply.
    const HDL_LANGUAGE_PATTERNS = [
        ['vhdl', /\.(vhd|vhdl)$/i],
        ['systemverilog', /\.(sv|svh)$/i],
        ['verilog', /\.(v|vh)$/i],
    ];
    const ensureHdlLanguage = async (document) => {
        if (!document || document.languageId !== 'plaintext') return;
        const match = HDL_LANGUAGE_PATTERNS.find(([, pattern]) => pattern.test(document.uri.path));
        if (!match) return;
        try {
            await vscode.languages.setTextDocumentLanguage(document, match[0]);
            diag(`language: ${document.uri.fsPath} plaintext -> ${match[0]}`);
        } catch (err) {
            diag('setTextDocumentLanguage failed: ' + ((err && err.message) || err));
        }
    };

    const writeHighlightReport = async (document) => {
        if (!document || !/\.(v|vh|sv|svh|vhd|vhdl)$/i.test(document.uri.path)) return;
        try {
            const extension = vscode.extensions.getExtension('fpga-lsp.fpga-lsp-anlogic');
            const languages = await vscode.languages.getLanguages();
            const highlight = await rpc.execute(COMMANDS.debugHighlight,
                [{ uri: document.uri.toString() }]).catch((err) => ({ ok: false, error: String(err) }));
            const report = [
                'FPGA highlight diagnostics',
                '=========================',
                `time             : ${new Date().toISOString()}`,
                `extension version: ${(extension && extension.packageJSON.version) || '?'}`,
                `extension path   : ${(extension && extension.extensionPath) || '?'}`,
                `file             : ${document.uri.fsPath}`,
                `languageId       : ${document.languageId}`,
                `registered langs : verilog=${languages.includes('verilog')} systemverilog=${languages.includes('systemverilog')} vhdl=${languages.includes('vhdl')}`,
                `theme            : ${vscode.workspace.getConfiguration('workbench').get('colorTheme')}`,
                `semanticHighlight: ${vscode.workspace.getConfiguration('editor').get('semanticHighlighting.enabled')}`,
                `file associations: ${JSON.stringify(vscode.workspace.getConfiguration('files').get('associations') || {})}`,
                `grammars         : ${JSON.stringify(((extension && extension.packageJSON.contributes.grammars) || []).map((item) => item.language))}`,
                `semanticTokenScopes: ${JSON.stringify(((extension && extension.packageJSON.contributes.semanticTokenScopes) || []).map((item) => item.language))}`,
                `server project   : ${(highlight && highlight.path) || '?'}`,
                `verible syntax   : ${(highlight && highlight.verible) || false}  veribleHighlight=${(highlight && highlight.veribleHighlight) || false}`,
                `semantic tokens  : ${(highlight && highlight.tokens) || 0}  ${JSON.stringify((highlight && highlight.counts) || {})}`,
                '',
                'If languageId is plaintext/other: run "Developer: Reload Window" (this build tries to fix it automatically).',
                'If semantic tokens are 0 but languageId is verilog: grammar coloring should still work;',
                'check the theme scope colors with "Developer: Inspect Editor Tokens and Scopes".',
            ].join('\n');
            const dir = path.join(cwd, '.fpgalsp', 'logs');
            fs.mkdirSync(dir, { recursive: true });
            fs.writeFileSync(path.join(dir, 'highlight.txt'), report);
            diag('highlight report written: ' + path.join(dir, 'highlight.txt'));
        } catch (err) {
            diag('highlight report failed: ' + ((err && err.stack) || err));
        }
    };

    for (const document of vscode.workspace.textDocuments) ensureHdlLanguage(document);
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument((document) => {
        ensureHdlLanguage(document);
        setTimeout(() => writeHighlightReport(document).catch(() => undefined), 2500);
    }));

    register(VSCodeCommands.highlightDiagnostics, async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor) {
            vscode.window.showWarningMessage('FPGA: open a .v/.vhd file first');
            return;
        }
        await writeHighlightReport(editor.document);
        const file = path.join(cwd, '.fpgalsp', 'logs', 'highlight.txt');
        const opened = await vscode.workspace.openTextDocument(file);
        await vscode.window.showTextDocument(opened, { preview: true });
    });

    // ------------------------------------------------------------ source/project helpers
    register(VSCodeCommands.copyInstantiation, async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor) {
            vscode.window.showWarningMessage('FPGA: open a Verilog file first');
            return;
        }
        const result = await rpc.execute(COMMANDS.copyInstantiation,
            [{ uri: editor.document.uri.toString() }]);
        if (!result || !result.ok) {
            vscode.window.showWarningMessage(`FPGA: ${(result && result.error) || 'unable to generate instantiation'}`);
            return;
        }
        await vscode.env.clipboard.writeText(result.text);
        vscode.window.showInformationMessage(
            `FPGA: copied the instantiation template for ${result.module} (${result.portCount} ports)`);
    });

    register(VSCodeCommands.readDesign, async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || !/\.(v|vh|sv|svh)$/i.test(editor.document.uri.fsPath)) {
            vscode.window.showInformationMessage('FPGA: open a Verilog/SystemVerilog file to run the read_design check.');
            return;
        }
        try {
            const result = await rpc.execute(COMMANDS.check, []);
            if (!result || !result.ok) {
                vscode.window.showWarningMessage(`FPGA: read_design check not started: ${(result && result.error) || 'unknown error'}`);
                return;
            }
            vscode.window.showInformationMessage(
                'FPGA: TD read_design check started; HDL/TD errors will appear in Problems (source fpga-lsp:td).');
        } catch (err) {
            vscode.window.showErrorMessage(`FPGA: read_design check failed: ${(err && err.message) || err}`);
        }
    });

    register(VSCodeCommands.copySymbolPath, async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor) return;
        const position = editor.selection.active;
        const result = await rpc.execute(COMMANDS.symbolPath, [{
            uri: editor.document.uri.toString(),
            position: { line: position.line, character: position.character },
        }]);
        if (!result || !result.ok) {
            vscode.window.showWarningMessage(`FPGA: ${(result && result.error) || 'no symbol under the cursor'}`);
            return;
        }
        await vscode.env.clipboard.writeText(result.path);
        vscode.window.showInformationMessage(`FPGA: copied hierarchy path ${result.path}`);
    });

    register(VSCodeCommands.copyMcpConfig, async () => {
        try {
            const definition = await buildMcpDefinition();
            if (!definition) {
                vscode.window.showWarningMessage('FPGA: open a folder containing an .al project first.');
                return;
            }
            const workspaceDefinition = buildWorkspaceMcpServerOptions({
                serverRoot,
                pythonPath: python(),
                nativeExe: isNative ? server.exe : '',
                tdPath: config().get('tdPath') || '',
                allowFlow: config().get('mcp.allowFlow') === true,
                allowWrite: config().get('mcp.allowWrite') === true,
                version: context.extension.packageJSON.version,
                existingPythonPath: process.env.PYTHONPATH,
            });
            const choices = [
                { label: 'Portable (.mcp.json / Claude, Cline)', value: buildPortableConfig(definition) },
                { label: 'VS Code (.vscode/mcp.json)', value: buildVsCodeConfig(workspaceDefinition) },
            ];
            const picked = await vscode.window.showQuickPick(choices, {
                title: 'Copy MCP configuration',
            });
            if (!picked) return;
            await vscode.env.clipboard.writeText(JSON.stringify(picked.value, null, 2));
            vscode.window.showInformationMessage('FPGA: MCP configuration copied to the clipboard.');
        } catch (err) {
            vscode.window.showErrorMessage(`FPGA: ${(err && err.message) || err}`);
        }
    });

    register(VSCodeCommands.installMcpConfig, async () => {
        try {
            const definition = await buildMcpDefinition();
            if (!definition) {
                vscode.window.showWarningMessage('FPGA: open a folder containing an .al project first.');
                return;
            }
            const root = workspaceFolder();
            if (!root) {
                vscode.window.showWarningMessage('FPGA: open a workspace folder first.');
                return;
            }
            const choice = await vscode.window.showQuickPick([
                { label: 'VS Code workspace (.vscode/mcp.json)', format: 'vscode' },
                { label: 'Portable workspace (.mcp.json for Cline/Claude)', format: 'portable' },
            ], { title: 'Install MCP configuration' });
            if (!choice) return;
            const target = choice.format === 'vscode'
                ? path.join(root, '.vscode', 'mcp.json')
                : path.join(root, '.mcp.json');
            const workspaceDefinition = buildWorkspaceMcpServerOptions({
                serverRoot,
                pythonPath: python(),
                nativeExe: isNative ? server.exe : '',
                tdPath: config().get('tdPath') || '',
                allowFlow: config().get('mcp.allowFlow') === true,
                allowWrite: config().get('mcp.allowWrite') === true,
                version: context.extension.packageJSON.version,
                existingPythonPath: process.env.PYTHONPATH,
            });
            const generated = choice.format === 'vscode'
                ? buildVsCodeConfig(workspaceDefinition) : buildPortableConfig(definition);
            let mcpData = {};
            if (fs.existsSync(target)) {
                try {
                    mcpData = JSON.parse(fs.readFileSync(target, 'utf8'));
                } catch (err) {
                    const document = await vscode.workspace.openTextDocument(target);
                    await vscode.window.showTextDocument(document);
                    vscode.window.showErrorMessage(
                        'FPGA: existing MCP config is not plain JSON (comments?); add the fpga-lsp entry manually.');
                    return;
                }
            }
            if (choice.format === 'vscode') {
                mcpData.servers = Object.assign({}, mcpData.servers || {}, generated.servers);
            } else {
                mcpData.mcpServers = Object.assign({}, mcpData.mcpServers || {}, generated.mcpServers);
            }
            fs.mkdirSync(path.dirname(target), { recursive: true });
            fs.writeFileSync(target, JSON.stringify(mcpData, null, 2) + '\n', 'utf8');
            const document = await vscode.workspace.openTextDocument(target);
            await vscode.window.showTextDocument(document);
            vscode.window.showInformationMessage(
                `FPGA: MCP configuration written to ${target}. Run MCP: List Servers (or refresh tools in Copilot) to load it.`);
        } catch (err) {
            vscode.window.showErrorMessage(`FPGA: ${(err && err.message) || err}`);
        }
    });

    register(VSCodeCommands.addSourceFile, async () => {
        const picked = await vscode.window.showOpenDialog({
            canSelectMany: true,
            openLabel: 'Add to .al',
            filters: { HDL: ['v', 'sv', 'vh', 'svh', 'vhd', 'vhdl'] },
        });
        if (!picked || !picked.length) return;
        const results = [];
        const errors = [];
        // Sequential on purpose: every command rewrites the .al; parallel calls would race.
        for (const file of picked) {
            try {
                const result = await rpc.execute(COMMANDS.addSourceFile, [{ file: file.fsPath }]);
                if (!result || !result.ok) {
                    errors.push(`${path.basename(file.fsPath)}: ${(result && result.error) || 'failed'}`);
                } else {
                    results.push(result);
                }
            } catch (err) {
                errors.push(`${path.basename(file.fsPath)}: ${(err && err.message) || err}`);
            }
        }
        const added = results.filter((item) => item.added);
        const already = results.filter((item) => !item.added);
        if (errors.length) {
            vscode.window.showWarningMessage(
                `FPGA: added ${added.length}/${picked.length} selected file(s) to the .al. ` +
                `Errors: ${errors.join('; ')}`);
        } else if (added.length) {
            vscode.window.showInformationMessage(
                `FPGA: added ${added.length} file(s) to the .al` +
                (already.length ? `; ${already.length} already present` : '') +
                '. Use FPGA: Update Project to reload it in TD.');
        } else {
            vscode.window.showInformationMessage(
                `FPGA: all ${already.length} selected file(s) are already in the .al`);
        }
        await Promise.allSettled([moduleProvider.refresh(activeDocumentUri()), flowProvider.refresh()]);
    });

    register(VSCodeCommands.searchHierarchy, async () => {
        const query = await vscode.window.showInputBox({
            title: 'Search module / instance names in the hierarchy',
            placeHolder: 'e.g. pwm_gen or u_pwm_gen',
        });
        if (!query) return;
        const result = await rpc.execute(COMMANDS.searchHierarchy, [{ query }]);
        const matches = (result && result.matches) || [];
        if (!matches.length) {
            vscode.window.showInformationMessage(`FPGA: no module/instance matches "${query}"`);
            return;
        }
        const items = matches.map((match) => ({
            label: match.kind === 'instance' ? `${match.name}  (instance)` : match.name,
            description: match.kind === 'instance'
                ? `→ ${match.module} @ ${path.basename(match.file)}:${match.line + 1}`
                : path.basename(match.file) + ':' + (match.line + 1),
            detail: match.file,
            match,
        }));
        const picked = await vscode.window.showQuickPick(items, { title: `Hierarchy matches (${items.length})` });
        if (!picked) return;
        const match = picked.match;
        const target = vscode.Uri.file(match.file);
        const document = await vscode.workspace.openTextDocument(target);
        const editor = await vscode.window.showTextDocument(document);
        const range = new vscode.Range(match.line, 0, match.line, 0);
        editor.selection = new vscode.Selection(range.start, range.end);
        editor.revealRange(range, vscode.TextEditorRevealType.InCenter);
        // also reveal the node in the hierarchy tree when it belongs to the current project
        try {
            const nodes = moduleProvider.findNodes(match.name);
            if (nodes.length && modulesView) {
                await modulesView.reveal(nodes[0].node, { select: true, focus: true, expand: true });
            }
        } catch (_) { /* tree reveal is best effort */ }
    });

    register(VSCodeCommands.duplicateModules, async () => {
        const result = await rpc.execute(COMMANDS.duplicateModules, []);
        const duplicates = (result && result.duplicates) || [];
        if (!duplicates.length) {
            vscode.window.showInformationMessage('FPGA: no duplicate module definitions found');
            return;
        }
        const picked = await vscode.window.showQuickPick(duplicates.map((entry) => ({
            label: entry.name,
            description: `${entry.definitions.length} definitions`,
            detail: entry.definitions.map((def) => `${path.basename(def.file)}:${def.line + 1}`).join('  |  '),
            entry,
        })), { title: `Duplicate modules (${duplicates.length})`, matchOnDetail: true });
        if (!picked) return;
        const definitions = picked.entry.definitions;
        if (definitions.length < 2) return;
        const left = vscode.Uri.file(definitions[0].file);
        const right = vscode.Uri.file(definitions[1].file);
        await vscode.commands.executeCommand('vscode.diff', left, right,
            `${picked.entry.name}: ${path.basename(definitions[0].file)} ↔ ${path.basename(definitions[1].file)}`);
    });

    register(VSCodeCommands.renameProject, async () => {
        const info = await getProjectInfo();
        const current = (info && info.project) || '';
        const name = await vscode.window.showInputBox({
            title: 'Rename .al project (edits <Name>)',
            value: current,
            validateInput: (value) => (/[<>"&]/.test(value || '') ? 'Cannot contain < > " &' : undefined),
        });
        if (!name || name === current) return;
        const result = await rpc.execute(COMMANDS.setProjectName, [{ name }]);
        if (!result || !result.ok) {
            vscode.window.showErrorMessage(`FPGA: ${(result && result.error) || 'rename failed'}`);
            return;
        }
        vscode.window.showInformationMessage(`FPGA: project renamed to ${name} (.al saved)`);
        await Promise.allSettled([moduleProvider.refresh(activeDocumentUri()), flowProvider.refresh()]);
    });

    register(VSCodeCommands.updateProject, async () => {
        const result = await rpc.execute(COMMANDS.updateProject, []);
        if (!result || !result.ok) {
            vscode.window.showWarningMessage(`FPGA: failed to update the project: ${(result && result.error) || 'see logs'}`);
            return;
        }
        vscode.window.showInformationMessage('FPGA: open_project -update completed (.al is in sync with disk)');
        await Promise.allSettled([moduleProvider.refresh(activeDocumentUri()), flowProvider.refresh()]);
    });

    // Module hierarchy
    register(VSCodeCommands.openModuleNode, async (arg) => {
        const target = asTarget(arg);
        if (!target) {
            vscode.window.showInformationMessage('FPGA: no source location for this module.');
            return;
        }
        await openTarget(target);
    });
    register(VSCodeCommands.goToDefinition, async (arg) => {
        const raw = rawFrom(arg) || {};
        const target = definitionTarget(raw);
        if (!target) {
            const name = raw.module || 'this module';
            vscode.window.showInformationMessage(
                `FPGA: "${name}" is a blackbox (IP/primitive/undefined) — no definition in this project.`
            );
            return;
        }
        await openTarget(target);
    });
    register(VSCodeCommands.goToInstantiation, async (arg) => {
        const raw = rawFrom(arg) || {};
        const target = instantiationTarget(raw);
        if (!target) {
            const name = raw.module || 'this module';
            vscode.window.showInformationMessage(
                `FPGA: no instantiation site for "${name}" (it is the project top, or the payload has no usedAt).`
            );
            return;
        }
        await openTarget(target);
    });
    register(VSCodeCommands.copyModulePath, async (arg) => {
        const raw = rawFrom(arg) || {};
        const file = raw.file || (raw.usedAt && raw.usedAt.file) || '';
        if (!file) {
            vscode.window.showInformationMessage('FPGA: this node has no file path.');
            return;
        }
        await copyText(String(file));
        vscode.window.showInformationMessage(`Copied ${file}`);
    });
    register(VSCodeCommands.refreshModules, async () => {
        await moduleProvider.refresh(activeDocumentUri());
    });

    // Flow
    register(VSCodeCommands.runSyn, () => flowController.run(['syn']));
    register(VSCodeCommands.runPhy, () => flowController.run(['phy']));
    register(VSCodeCommands.runAll, () => flowController.run(['all']));
    register(VSCodeCommands.cancelFlow, () => flowController.cancel());
    register(VSCodeCommands.refreshFlow, () => flowProvider.refresh());
    register(VSCodeCommands.showFlowLog, () => flowController.showLog());

    // Reports
    register(VSCodeCommands.showReports, () => reportsPanel.show('syn'));
    register(VSCodeCommands.showSynReports, () => reportsPanel.show('syn'));
    register(VSCodeCommands.showPhyReports, () => reportsPanel.show('phy'));
    register(VSCodeCommands.refreshReports, () => Promise.allSettled([
        reportsPanel.refresh(),
        reportsTreeProvider.refresh(),
    ]));
    register(VSCodeCommands.openArtifact, async (arg) => {
        const filePath = pathFrom(arg);
        if (!filePath) return;
        try {
            await vscode.commands.executeCommand('vscode.open', vscode.Uri.file(String(filePath)));
        } catch (err) {
            vscode.window.showErrorMessage(`FPGA: cannot open ${filePath}: ${(err && err.message) || err}`);
        }
    });
    register(VSCodeCommands.openReportFile, async (arg) => {
        const filePath = pathFrom(arg);
        if (!filePath) return;
        await openTarget({ uri: vscode.Uri.file(String(filePath)), range: new vscode.Range(0, 0, 0, 0) });
    });

    // Legacy v1 commands (kept working exactly as before).
    register(VSCodeCommands.check, async () => {
        const result = await rpc.execute(COMMANDS.check, []);
        vscode.window.showInformationMessage(
            result && result.ok ? `TD check queued for ${result.project}` : `TD check failed: ${JSON.stringify(result)}`
        );
    });
    register(VSCodeCommands.info, async () => {
        const info = await rpc.execute(COMMANDS.info, []);
        if (!info || !info.ok) {
            vscode.window.showWarningMessage(`No Anlogic project found: ${JSON.stringify(info)}`);
            return;
        }
        const lines = [
            `project   : ${info.project} (${info.family} ${info.device})`,
            `top       : ${info.top}`,
            `file      : ${info.al}`,
            `sources   : ${info.files.hdl} HDL, ${info.files.adc} ADC, ${info.files.sdc} SDC, ${info.files.ip} IP`,
            `pins      : ${info.pins} (${info.device_loaded ? 'device table cached' : 'run FPGA: Run TD Check'})`,
            `TD session: ${info.td_session ? 'running' : 'not started'}`,
        ];
        const doc = await vscode.workspace.openTextDocument({ content: lines.join('\n'), language: 'text' });
        vscode.window.showTextDocument(doc, { preview: true });
    });
    // ------------------------------------------------------------ Verible (optional)
    const veribleSettings = () => ({
        enabled: config().get('verible.enable') === true,
        path: config().get('verible.path') || '',
        args: config().get('verible.args') || [],
        format: config().get('verible.format') !== false,
    });

    async function startVerible() {
        const settings = veribleSettings();
        if (!settings.enabled) return;
        const resolved = resolveVerible({ veriblePath: settings.path });
        if (!resolved.languageServer) {
            vscode.window.showWarningMessage(
                'FPGA: Verible diagnostics are enabled but verible-verilog-ls was not found. Set fpgaLsp.verible.path or add it to PATH.');
            return;
        }
        const lsCommand = config().get('verible.command') || resolved.languageServer;
        const lsArgs = config().get('verible.lsArgs') || [];
        diag(`verible diagnostics: exe=${lsCommand} args=${JSON.stringify(lsArgs)} cwd=${cwd}`);
        veribleDiag = new VeribleDiagnosticsClient({ exe: lsCommand, args: lsArgs, cwd, env, log: diag });
        const ok = await veribleDiag.start();
        if (!ok) {
            veribleDiag.dispose();
            veribleDiag = undefined;
            vscode.window.showWarningMessage('FPGA: failed to start Verible diagnostics (see FPGA: Show Logs).');
            return;
        }
        const isVerilog = (document) => /\.(v|sv|vh|svh)$/i.test(document.uri.path);
        for (const document of vscode.workspace.textDocuments) {
            if (isVerilog(document)) veribleDiag.didOpen(document);
        }
        context.subscriptions.push(
            vscode.workspace.onDidOpenTextDocument((document) => {
                if (veribleDiag && isVerilog(document)) veribleDiag.didOpen(document);
            }),
            vscode.workspace.onDidChangeTextDocument((event) => {
                if (veribleDiag && isVerilog(event.document)) veribleDiag.didChange(event.document);
            }),
            vscode.workspace.onDidCloseTextDocument((document) => {
                if (veribleDiag) veribleDiag.didClose(document);
            })
        );
        diag('verible diagnostics client started: ' + lsCommand);
        vscode.window.showInformationMessage(
            'FPGA: Verible diagnostics started (source: verible; formatting is still performed by fpga-lsp calling Verible).');
    }

    register(VSCodeCommands.selectProject, async () => {
        const result = await rpc.execute(COMMANDS.projects, []);
        if (!result || !result.ok || !(result.projects || []).length) {
            openErrors((result && result.error) || 'No .al project found (no .al file in the workspace)');
            return;
        }
        const items = result.projects.map((project) => ({
            label: project.name || project.al,
            description: project.al,
            detail: `top ${project.top || '?'} · ${project.device || '?'} · ${project.hdl} HDL`,
            project,
        }));
        const picked = await vscode.window.showQuickPick(items, { title: 'Select Anlogic project' });
        if (!picked) return;
        const al = picked.project.al;
        await rpc.execute(COMMANDS.setProject, [{ al }]);
        try {
            await config().update('projectFile', al, vscode.ConfigurationTarget.Workspace);
        } catch (_) { /* read-only workspace: runtime override already applied */ }
        vscode.window.showInformationMessage(`FPGA: selected project ${picked.project.name}`);
        await Promise.allSettled([moduleProvider.refresh(activeDocumentUri()), flowProvider.refresh()]);
        if (mcpEmitter) mcpEmitter.fire();
    });

    register(VSCodeCommands.tdInfo, async () => {
        const result = await rpc.execute(COMMANDS.tdInfo, []);
        const td = (result && result.td) || {};
        if (!td.ok) {
            vscode.window.showWarningMessage(`FPGA: ${td.error || 'TD installation not found'}`);
            return;
        }
        const lines = [
            `Currently using: ${td.root}`,
            `Version      : ${td.version || '?'}  (source=${td.source || 'auto'})`,
            '',
            'Other detected installations (auto mode picks the newest by year + version):',
            ...((td.alternatives || []).length
                ? td.alternatives.map((item) => `  - ${item.version}: ${item.root}`)
                : ['  (none)']),
            '',
            'To pin a version: set fpgaLsp.tdPath or the TD_ROOT environment variable.',
        ];
        const doc = await vscode.workspace.openTextDocument({ content: lines.join('\n'), language: 'text' });
        vscode.window.showTextDocument(doc, { preview: true });
    });

    register(VSCodeCommands.veribleStatus, async () => {
        const settings = veribleSettings();
        const resolved = resolveVerible({ veriblePath: settings.path });
        const lines = [
            `fpgaLsp.verible.enable : ${settings.enabled}`,
            `fpgaLsp.verible.path   : ${settings.path || '(PATH)'}`,
            `formatter              : ${resolved.formatter || '(not found)'}`,
            `language server        : ${resolved.languageServer || '(not found)'}`,
            `format via server      : ${settings.format}`,
            '',
            resolved.formatter
                ? 'Format Document (Shift+Alt+F) is served by the language server via verible-verilog-format.'
                : 'verible-verilog-format not found: Format Document is unavailable (other features are unaffected).',
        ];
        const doc = await vscode.workspace.openTextDocument({ content: lines.join('\n'), language: 'text' });
        vscode.window.showTextDocument(doc, { preview: true });
    });

    register(VSCodeCommands.insertInstantiation, async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || !/\.(v|sv|vh|svh)$/i.test(editor.document.uri.fsPath)) {
            vscode.window.showInformationMessage('FPGA: use this command in a Verilog/SystemVerilog file.');
            return;
        }
        const listed = await rpc.execute(COMMANDS.instantiate, [{}]);
        const modules = (listed && listed.ok && listed.modules) || [];
        if (!modules.length) {
            openErrors(`Unable to get module list${listed && listed.error ? ': ' + listed.error : ''}`);
            return;
        }
        const picked = await vscode.window.showQuickPick(modules, { title: 'Select a module to instantiate' });
        if (!picked) return;
        const result = await rpc.execute(COMMANDS.instantiate, [{ module: picked }]);
        if (!result || !result.ok) {
            openErrors(`Failed to generate instantiation${result && result.error ? ': ' + result.error : ''}`);
            return;
        }
        const position = editor.selection.active;
        const edit = new vscode.WorkspaceEdit();
        edit.insert(editor.document.uri, new vscode.Position(position.line, 0), result.text);
        const applied = await vscode.workspace.applyEdit(edit);
        if (!applied) {
            openErrors('Failed to insert the instantiation text (the editor rejected the edit)');
            return;
        }
        vscode.window.showInformationMessage(
            `FPGA: inserted instantiation of ${picked} (${result.portCount} ports, instance ${result.instanceName})`);
    });

    // ------------------------------------------------------------ v3: actions
    const runWithProgress = async (title, command, commandArgs) => vscode.window.withProgress(
        { location: vscode.ProgressLocation.Notification, title, cancellable: false },
        async () => {
            try {
                return await rpc.execute(command, commandArgs || []);
            } catch (err) {
                return { ok: false, error: (err && err.message) || String(err) };
            }
        }
    );

    const openErrors = (message) => {
        diag('action failed: ' + message);
        vscode.window.showErrorMessage(`FPGA: ${message}`, 'Show Logs').then((choice) => {
            if (choice === 'Show Logs') vscode.commands.executeCommand(VSCodeCommands.showLogs);
        });
    };

    const readReports = async () => {
        try {
            return await rpc.request(METHODS.reports, {}) || {};
        } catch (_) {
            return {};
        }
    };

    const programFlow = async (mode) => {
        const preflight = await runWithProgress('FPGA: checking cable and online device...', COMMANDS.cables, []);
        const info = (preflight && (preflight.inspect || preflight)) || {};
        if (!info.cables || !info.cables.length) {
            openErrors(info.reason || 'No cable/board detected (list_cable is empty)');
            return;
        }
        const cableText = info.cables.map((c) => `${c.index}:${c.device}`).join(', ');
        if (!info.match) {
            const forced = await vscode.window.showWarningMessage(
                `Device mismatch! Project target ${info.expected}, online ${info.device_id || '?'} (cable ${cableText}).\n` +
                'Programming a mismatched bitstream may make the board unusable. Continue?',
                { modal: true }, 'Program anyway');
            if (forced !== 'Program anyway') return;
        }
        const label = mode === 'flash' ? 'Flash (SPI)' : 'SRAM (JTAG)';
        const confirmed = await vscode.window.showWarningMessage(
            `Program device?\nMode: ${label}\nOnline device: ${info.device_id}\nFile: ${info.bit}\nCable: ${cableText}`,
            { modal: true }, 'Program');
        if (confirmed !== 'Program') return;
        const result = await runWithProgress(`FPGA: programming ${label}...`, COMMANDS.program,
            [{ mode, bit: info.bit, confirm: true, allowMismatch: !info.match }]);
        if (result && result.ok) {
            vscode.window.showInformationMessage(`FPGA: programming succeeded (${label} -> ${result.device_id || 'device'})`);
        } else {
            openErrors(`Programming failed - ${(result && (result.error || result.reason)) || 'unknown error'}`);
        }
        reportsPanel.refresh().catch(() => undefined);
        reportsTreeProvider.refresh().catch(() => undefined);
    };

    register(VSCodeCommands.doctor, async () => {
        try {
            const root = workspaceFolder();
            const info = await getProjectInfo();
            const tdResult = await rpc.execute(COMMANDS.tdInfo, []).catch(() => undefined);
            const vSettings = veribleSettings();
            const vResolved = resolveVerible({ veriblePath: vSettings.path });
            let pythonCheck;
            try {
                pythonCheck = await checkPython({
                    python: python(),
                    cwd: root || serverRoot,
                    env,
                    timeoutMs: 15000,
                });
            } catch (err) {
                pythonCheck = { ok: false, error: (err && err.message) || String(err) };
            }
            const mcpDefinition = await buildMcpDefinition().catch(() => undefined);
            const workspaceMcpPath = root ? path.join(root, '.vscode', 'mcp.json') : '';
            const describeMcpConfig = (filePath) => {
                if (!filePath || !fs.existsSync(filePath)) return `${filePath || '(unknown)'} (missing)`;
                try {
                    const parsed = JSON.parse(fs.readFileSync(filePath, 'utf8'));
                    const entry = (parsed.servers && parsed.servers['fpga-lsp'])
                        || (parsed.mcpServers && parsed.mcpServers['fpga-lsp']);
                    if (!entry) return `${filePath} (no fpga-lsp entry)`;
                    return `${filePath} (fpga-lsp: ${(entry.args || []).join(' ')})`;
                } catch (err) {
                    return `${filePath} (cannot parse: ${(err && err.message) || err})`;
                }
            };
            const td = (tdResult && tdResult.td) || (info && info.td) || {};
            const lspServer = (client && client.initializeResult && client.initializeResult.serverInfo) || {};
            const mcpApi = !!(vscode.lm && typeof vscode.lm.registerMcpServerDefinitionProvider === 'function'
                && typeof vscode.McpStdioServerDefinition === 'function');
            const rows = [
                '# FPGA LSP Doctor',
                '',
                `Generated: ${new Date().toLocaleString()}`,
                '',
                '## Extension',
                `- Version: **${context.extension.packageJSON.version}**`,
                `- VS Code: ${vscode.version}`,
                `- Workspace: ${root || '(no workspace folder)'}`,
                `- Project: ${(info && info.al) || '(not found)'}`,
                `- Top / device: ${(info && info.top) || '?'} / ${(info && info.device) || '?'}`,
                `- HDL/ADC/SDC files: ${(info && info.files && `${info.files.hdl}/${info.files.adc}/${info.files.sdc}`) || '?'}`,
                `- Project warnings: ${(info && info.warnings && info.warnings.length) || 0}`,
                '',
                '## Language server (fpga-lsp)',
                `- Client created: ${!!client}`,
                `- Client state: ${client ? client.state : 'n/a'}`,
                `- Server: ${lspServer.name || '?'} ${lspServer.version || ''}`.trim(),
                `- Python: ${python()}${pythonCheck && pythonCheck.executable ? ` (${pythonCheck.executable})` : ''}`,
                `- Python check: ${pythonCheck && pythonCheck.ok ? `OK, fpgalsp ${pythonCheck.version || '?'}` : `FAILED - ${(pythonCheck && pythonCheck.error) || 'unknown'}`}`,
                `- Server root: ${serverRoot}`,
                `- Server log: ${config().get('logFile') || (root ? path.join(root, '.fpgalsp', 'logs', 'server.log') : '(n/a)')}`,
                '',
                '## Verible',
                `- fpgaLsp.verible.enable: ${vSettings.enabled}`,
                `- fpgaLsp.verible.format: ${vSettings.format}`,
                `- Configured path: ${vSettings.path || '(PATH)'}`,
                `- verible-verilog-format: ${vResolved.formatter || '(not found)'}`,
                `- verible-verilog-ls: ${vResolved.languageServer || '(not found)'}`,
                `- Diagnostics client: ${vSettings.enabled ? (veribleDiag ? 'running' : 'not running') : 'disabled by setting'}`,
                `- Format args: ${(vSettings.args || []).length} option(s)`,
                '',
                '## TangDynasty',
                `- Detected: ${td.ok ? 'yes' : 'no'}`,
                `- Root: ${td.root || '(not found)'}`,
                `- Version: ${td.version || '?'}`,
                `- Source: ${td.source || '?'}`,
                `- Alternatives: ${(td.alternatives || []).length}`,
                `- TD session: ${(info && info.td_session) ? 'running' : 'not running'}`,
                `- Configured fpgaLsp.tdPath: ${config().get('tdPath') || '(auto)'}`,
                '',
                '## MCP',
                `- VS Code MCP API: ${mcpApi ? 'available' : 'unavailable'}`,
                `- Dynamic provider setting: ${config().get('mcp.dynamicProvider') === true}`,
                `- Dynamic provider: ${mcpProviderRegistered ? 'registered (fpgaLsp.mcp)' : `not registered${mcpProviderError ? ` - ${mcpProviderError}` : ''}`}`,
                `- Definition: ${mcpDefinition ? `${mcpDefinition.command} ${mcpDefinition.args.join(' ')}` : '(not available - open a project folder)'}`,
                `- fpgaLsp.mcp.allowFlow: ${config().get('mcp.allowFlow') === true}`,
                `- fpgaLsp.mcp.allowWrite: ${config().get('mcp.allowWrite') === true}`,
                `- Workspace config: ${describeMcpConfig(workspaceMcpPath)}`,
                '',
                '## Flow',
                `- Running: ${flowController && flowController.running ? 'yes' : 'no'}`,
                `- Last message: ${flowController && flowController.lastMessage ? flowController.lastMessage : '(none)'}`,
                '',
                '> Notes: the MCP process itself is owned by VS Code/Copilot; this report shows the extension provider and persisted configuration. Use "MCP: List Servers" for the live server list.',
            ];
            const doc = await vscode.workspace.openTextDocument({ content: rows.join('\n'), language: 'markdown' });
            await vscode.window.showTextDocument(doc, { preview: true });
        } catch (err) {
            vscode.window.showErrorMessage(`FPGA: doctor failed: ${(err && err.message) || err}`);
        }
    });

    register(VSCodeCommands.showActions, async () => {
        const actions = [
            { label: '$(run) Incremental route: reuse synthesis and rerun P&R only (~8 s)', command: VSCodeCommands.runIncrementalRoute },
            { label: '$(shield) Run DRC / CDC analysis', command: VSCodeCommands.runAnalysis },
            { label: '$(diff) Compare multiple strategies', command: VSCodeCommands.compareStrategies },
            { label: '$(plug) Show cable / online device', command: VSCodeCommands.showCables },
            { label: '$(zap) Program SRAM (JTAG)', command: VSCodeCommands.programSram },
            { label: '$(database) Program Flash (SPI)', command: VSCodeCommands.programFlash },
            { label: '$(file-binary) Generate SVF (for third-party JTAG tools)', command: VSCodeCommands.generateSvf },
            { label: '$(stethoscope) Doctor: extension / LSP / Verible / TD / MCP status', command: VSCodeCommands.doctor },
            { label: '$(output) Show logs', command: VSCodeCommands.showLogs },
        ];
        const picked = await vscode.window.showQuickPick(actions, { title: 'FPGA Actions' });
        if (picked) await vscode.commands.executeCommand(picked.command);
    });

    register(VSCodeCommands.runIncrementalRoute, () => flowController.run(['route']));

    register(VSCodeCommands.runAnalysis, async () => {
        const result = await runWithProgress('FPGA: DRC / CDC analysis (reads the placed/routed database, ~3 s)...',
            COMMANDS.runAnalysis, []);
        if (!result || !result.ok) {
            openErrors((result && (result.error || result.reason)) || 'Analysis failed');
            return;
        }
        const drc = result.drc || {};
        const cdc = result.cdc || {};
        const drcText = drc.supported === false ? 'DRC unavailable'
            : `DRC ${drc.violations === undefined ? '?' : drc.violations} violation(s)`;
        const cdcText = cdc.supported === false ? `CDC unsupported (${cdc.reason || 'device limitation'})`
            : `CDC ${(cdc.counts && cdc.counts.unsafe) || 0} unsafe`;
        vscode.window.showInformationMessage(`FPGA analysis complete: ${drcText} · ${cdcText}`);
        reportsPanel.show().then(() => Promise.allSettled([reportsPanel.refresh(), reportsTreeProvider.refresh()]));
    });

    register(VSCodeCommands.compareStrategies, async () => {
        const reports = await readReports();
        const available = ((reports.strategies || {}).available) || [];
        if (!available.length) {
            openErrors('No strategy list available (run a flow first, or check the TD installation)');
            return;
        }
        const preferred = ['TimingHigh', 'CongestionHigh', 'AreaOptimize'];
        const items = available.map((name) => ({ label: name, picked: preferred.includes(name) }));
        const picked = await vscode.window.showQuickPick(items, {
            canPickMany: true,
            title: 'Select strategies to compare (at least 2)',
            placeHolder: 'TimingHigh / CongestionHigh / AreaOptimize …',
        });
        if (!picked || picked.length < 2) {
            vscode.window.showInformationMessage('FPGA: select at least two strategies.');
            return;
        }
        const result = await runWithProgress('FPGA: comparing strategies (synthesis + P&R per strategy)...',
            COMMANDS.compareStrategies,
            [{ strategies: picked.map((item) => item.label), jobs: Math.min(4, picked.length) }]);
        if (!result || !result.ok) {
            openErrors((result && (result.error || result.reason)) || 'Strategy comparison failed');
            return;
        }
        const runs = (result.runs || []).filter((run) => run && run.ok);
        const summary = runs.map((run) => `${run.strategy}: WNS ${run.setup_wns_ps}ps`).join(' · ');
        const best = result.best ? `best ${result.best.strategy}` : '';
        vscode.window.showInformationMessage(`FPGA strategy comparison complete (${result.elapsed}s): ${summary}${best ? ' - ' + best : ''}`);
        reportsPanel.show().then(() => Promise.allSettled([reportsPanel.refresh(), reportsTreeProvider.refresh()]));
    });

    register(VSCodeCommands.showCables, async () => {
        const preflight = await runWithProgress('FPGA: reading cable information...', COMMANDS.cables, []);
        const info = (preflight && (preflight.inspect || preflight)) || {};
        const lines = [
            `Project target : ${info.expected}`,
            `Online device  : ${info.device_id || '(not connected)'}`,
            `Cable          : ${(info.cables || []).map((c) => c.index + ':' + c.device).join(', ') || '(none)'}`,
            `Bitstream      : ${info.bit || '(none)'}`,
            `Result         : ${info.reason || ''}`,
        ];
        const doc = await vscode.workspace.openTextDocument({ content: lines.join('\n'), language: 'text' });
        vscode.window.showTextDocument(doc, { preview: true });
    });

    register(VSCodeCommands.programSram, () => programFlow('sram'));
    register(VSCodeCommands.programFlash, () => programFlow('flash'));

    register(VSCodeCommands.generateSvf, async () => {
        const result = await runWithProgress('FPGA: generating SVF...', COMMANDS.svfgen, [{}]);
        if (result && result.ok) {
            vscode.window.showInformationMessage(`FPGA: generated ${result.svf} (${Math.round((result.size || 0) / 1024)} KB)`);
        } else {
            openErrors(`SVF generation failed - ${(result && (result.error || result.reason)) || 'unknown error'}`);
        }
    });

    register(VSCodeCommands.openTimingPoint, async (argument) => {
        const target = Array.isArray(argument) ? argument[0] : argument;
        if (!target || !target.file) {
            vscode.window.showInformationMessage('FPGA: no parsable RTL location for this timing point.');
            return;
        }
        const position = new vscode.Position(Math.max(0, Number(target.line) || 0), 0);
        await openTarget({
            uri: vscode.Uri.file(String(target.file)),
            range: new vscode.Range(position, position),
        });
    });

    register(VSCodeCommands.showLogs, async () => {
        if (!fs.existsSync(extLog)) {
            vscode.window.showInformationMessage('FPGA: no log yet (extension has not started the server).');
            return;
        }
        const doc = await vscode.workspace.openTextDocument(extLog);
        await vscode.window.showTextDocument(doc, { preview: false });
    });
    register(VSCodeCommands.restart, async () => {
        await rpc.execute(COMMANDS.restart, []);
        vscode.window.showInformationMessage('TD session closed; it restarts on the next check.');
    });

    // ------------------------------------------------------------ refresh triggers

    // ------------------------------------------------------- refresh triggers (A4 + F6/R1)
    //
    // The tree is refreshed **event first**: `fpgaLsp/indexChanged` (pushed by the server after
    // it re-indexed) drives an immediate, targeted update (moduleTree.js::applyIndexChanged).
    // The editor events below are only the fallback for servers that do not push that event;
    // they go through the provider's own 150 ms coalescing window (`requestRefresh`) and are
    // ignored when the file is not part of the project currently displayed.
    //
    // F6/R1: this layer used to add a *second* 150 ms debounce on top of the provider's one, so
    // a save reached its `fpgaLsp/moduleTree` request only after ~300 ms
    // (audit/2026-11-19/perf/VSCODE.md §P1). The provider's window is the one that merges N
    // editor events into one request, so only that one is kept here.

    const ownsActiveFile = (uri) => {
        if (!uri || !moduleProvider) return false;
        const text = String(uri);
        const file = text.startsWith('file:') ? decodeURIComponent(text.replace(/^file:\/\/\/?/, '')) : text;
        return moduleProvider.ownsFile(file);
    };

    const scheduleModuleRefresh = (reason, uri) => {
        if (!serverReady()) return;
        const target = uri || activeDocumentUri();
        // Only refresh when the document belongs to the project the tree is showing; a file from
        // another project must not trigger a tree rebuild.
        if (target && moduleProvider && !moduleProvider.ownsFile(String(target).startsWith('file:')
            ? decodeURIComponent(String(target).replace(/^file:\/\/\/?/, '')) : target)) {
            diag(`module refresh skipped (not in project): ${target}`);
            return;
        }
        moduleProvider.requestRefresh(target, { reason });
    };

    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        scheduleModuleRefresh('active-editor', editor && editor.document && editor.document.uri);
    }));
    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument((document) => {
        // Saving an unrelated file (another project, a .md, ...) must not rebuild the tree.
        if (!document || !ownsActiveFile(document.uri)) return;
        scheduleModuleRefresh('saved', document.uri);
    }));
    if (typeof vscode.workspace.onDidChangeWorkspaceFolders === 'function') {
        context.subscriptions.push(vscode.workspace.onDidChangeWorkspaceFolders(() => {
            scheduleModuleRefresh('workspace-folders');
        }));
    }

    // ------------------------------------------------------------------- startup

    client.start().then(async () => {
        try {
            // Register after the connection is up so no custom notification is missed.
            client.onNotification(METHODS.flowEvent, (event) => {
                if (flowController) flowController.onFlowEvent(event);
            });
            // Phase 2: the server tells us when its HDL index changed. Targeted updates first;
            // an unknown/newer payload shape degrades to an immediate full refresh.
            //
            // F6/R8: the per-event log line was a synchronous `mkdirSync + appendFileSync` on the
            // project volume (6-7 ms on Y:, paid even for ignored events). It is sampled now and
            // ignored events are never written; `appendLog()` itself is asynchronous as well.
            let indexEventCount = 0;
            client.onNotification(METHODS.indexChanged, (event) => {
                if (!moduleProvider) return;
                indexEventCount += 1;
                let outcome = 'ignored';
                try {
                    outcome = moduleProvider.applyIndexChanged(event);
                } catch (err) {
                    diag('indexChanged failed: ' + ((err && err.message) || err));
                    return;
                }
                if (outcome !== 'ignored' && (indexEventCount <= 5 || indexEventCount % 50 === 0)) {
                    diag(`indexChanged -> ${outcome} (#${indexEventCount})`);
                }
            });
        } catch (err) {
            // Older clients: no custom notifications at all -> the editor events still refresh.
            vscode.window.showWarningMessage(
                `FPGA: could not listen for server events (${(err && err.message) || err}); falling back to save-triggered refresh.`
            );
        }
        await Promise.allSettled([
            moduleProvider.refresh(activeDocumentUri()),
            flowProvider.refresh(),
            reportsTreeProvider.refresh(),
        ]);
        const activeDocument = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        if (activeDocument) writeHighlightReport(activeDocument).catch(() => undefined);
        startVerible().catch((err) => diag('verible start failed: ' + (err && err.message)));
    }).catch((err) => {
        const message = (err && err.message) || String(err);
        diag('START FAILED: ' + (err && err.stack ? err.stack : message));
        moduleProvider.setServerError(`language server failed to start: ${message}`);
        flowProvider.refresh().catch(() => undefined);
        vscode.window.showErrorMessage(
            `fpga-lsp failed to start (${message}). See "FPGA: Show Logs" (${extLog}).`
        );
    });

    context.subscriptions.push({ dispose: () => client && client.stop() });
}

function deactivate() {
    if (moduleProvider) {
        moduleProvider.dispose();
        moduleProvider = undefined;
    }
    if (flowProvider) {
        flowProvider.dispose();
        flowProvider = undefined;
    }
    if (flowController) {
        flowController.dispose();
        flowController = undefined;
    }
    if (reportsPanel) {
        reportsPanel.dispose();
        reportsPanel = undefined;
    }
    reportsPanels = undefined;
    if (reportsTreeProvider) {
        reportsTreeProvider.dispose();
        reportsTreeProvider = undefined;
    }
    if (veribleDiag) {
        veribleDiag.dispose();
        veribleDiag = undefined;
    }
    // F6/R8: the log is buffered now, so flush whatever is still in memory before the extension
    // host goes away (shutdown is the one moment a lost tail would be hard to explain).
    try {
        flushLogSync();
    } catch (_) { /* logging must never break deactivation */ }
    if (!client) return undefined;
    const instance = client;
    client = undefined;
    return instance.stop();
}

module.exports = { activate, deactivate };
