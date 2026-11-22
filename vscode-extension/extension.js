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
// Wave 3：状态目录（原来写在工作目录的 .fpgalsp）——统一解析到系统 temp。
const { STATE_ENV, resolveStateDir } = require('./src/stateDir');
const { readMcpEndpoint, endpointAlive } = require('./src/mcpEndpoint');
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
const { renderTrendHtml } = require('./src/trendHtml');
const { registerLmTools } = require('./src/lmTools');
const { renderConstraintHtml } = require('./src/constraintHtml');
const { ProbePanel } = require('./src/probePanel');
const { ProbeTreeProvider } = require('./src/probeTree');
const {
    buildMcpServerOptions,
    buildPortableConfig,
    buildVsCodeConfig,
    buildWorkspaceMcpServerOptions,
} = require('./src/mcpConfig');

let client;
// S5: crash recovery state (module scope: deactivate() must be able to stop it).
let restartAttempts = 0;
let shuttingDown = false;
let restartTimer;
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
    // S8: `fs.existsSync(folder)` is true for every workspace folder, so the old loop always
    // returned the **first** folder -- a multi-root workspace therefore pointed the server,
    // the state dir and the MCP endpoint at the wrong project.
    // Prefer a folder that actually contains an .al project (root or one level down).
    const hasProject = (dir) => {
        const candidates = [dir];
        try {
            for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
                if (entry.isDirectory() && entry.name !== '.git' && entry.name !== 'node_modules') {
                    candidates.push(path.join(dir, entry.name));
                }
            }
        } catch (_) {
            return false;
        }
        for (const candidate of candidates) {
            try {
                const names = fs.readdirSync(candidate);
                if (names.some((name) => name.toLowerCase().endsWith('.al'))) return true;
            } catch (_) { /* unreadable: skip */ }
        }
        return false;
    };
    for (const folder of folders) {
        if (folder.uri.scheme !== 'file') continue;
        if (hasProject(folder.uri.fsPath)) return folder.uri.fsPath;
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
    // Wave 3：状态目录统一到系统 temp（用户要求工作目录不再出现 .fpgalsp）。
    const stateDir = resolveStateDir(cwd);

    const env = Object.assign({}, process.env);
    env[STATE_ENV] = stateDir;  // 交给服务端决定 logs/run/cache 落哪

    const tdPath = config().get('tdPath') || '';
    const logFileSetting = config().get('logFile') || '';
    const logFile = logFileSetting || path.join(stateDir, 'logs', 'server.log');

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

    // everything about the launch goes to <state>/logs/extension.log (state = 系统 temp), because
    // "Server process exited with code N" alone is impossible to act on.
    const extLog = path.join(stateDir, 'logs', 'extension.log');
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
    // S5: the server used to die silently ("Server process exited" in a log nobody opens).
    // Now: reset the view state, tell the user once, and restart with a small backoff.
    restartAttempts = 0;
    shuttingDown = false;
    try {
        client.onDidChangeState((event) => {
            diag(`client state: ${event.oldState} -> ${event.newState}`);
            // State: 1 = Stopped, 2 = Running, 3 = Starting (vscode-languageclient enum)
            if (event.newState === 2) {
                restartAttempts = 0;
                return;
            }
            if (event.newState !== 1 || shuttingDown) return;
            if (restartAttempts >= 3) {
                vscode.window.showErrorMessage(
                    'FPGA: the language server stopped repeatedly. Run "FPGA: Show Logs" for details.');
                return;
            }
            restartAttempts += 1;
            const delay = 500 * restartAttempts;
            vscode.window.showWarningMessage(
                `FPGA: language server stopped; restarting (${restartAttempts}/3)…`);
            if (restartTimer) clearTimeout(restartTimer);
            restartTimer = setTimeout(() => {
                restartTimer = undefined;
                if (shuttingDown || !client) return;
                diag(`restarting language server (attempt ${restartAttempts})`);
                client.start().catch((err) => diag('restart failed: ' + ((err && err.message) || err)));
            }, delay);
        });
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

    // S1: status bar — project / device / WNS / flow state at a glance.
    // Previously the only status text was flowController's "FPGA: idle", so the user had no
    // idea which project or device the extension was even looking at.
    const statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    statusItem.command = VSCodeCommands.selectProject;
    statusItem.text = 'FPGA: starting…';
    statusItem.tooltip = 'Anlogic FPGA LSP — click to select a project';
    statusItem.show();
    context.subscriptions.push(statusItem);
    const statusState = { project: '', device: '', wns: undefined, flow: '' };
    const renderStatus = () => {
        const parts = [statusState.project || 'no project'];
        if (statusState.device) parts.push(statusState.device);
        if (Number.isFinite(statusState.wns)) parts.push(`WNS ${statusState.wns} ns`);
        if (statusState.flow) parts.push(statusState.flow);
        statusItem.text = `FPGA: ${parts.join(' · ')}`;
        statusItem.tooltip = statusState.project
            ? `Project: ${statusState.project}${statusState.device ? `\nDevice: ${statusState.device}` : ''}`
            : 'No Anlogic .al project found in this workspace';
    };
    // S7: make agent permissions visible. allowWrite/allowFlow silently become command line
    // flags; the user had no way to see that an agent may now modify the project or start TD.
    const agentItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
    agentItem.command = 'workbench.action.openSettings';
    agentItem.tooltip = 'MCP agent permissions (fpgaLsp.mcp.allowFlow / allowWrite)';
    context.subscriptions.push(agentItem);
    const renderAgentBadge = () => {
        const flow = config().get('mcp.allowFlow') === true;
        const write = config().get('mcp.allowWrite') === true;
        if (!flow && !write) {
            agentItem.hide();
            return;
        }
        const parts = [];
        if (write) parts.push('write');
        if (flow) parts.push('flow');
        agentItem.text = `$(shield) agent: ${parts.join('+')}`;
        agentItem.tooltip = `MCP agents may ${parts.join(' and ')}. Click to change fpgaLsp.mcp settings.`;
        agentItem.show();
    };
    renderAgentBadge();

    const refreshStatus = async () => {
        try {
            const info = await getProjectInfo();
            if (info && info.ok !== false) {
                statusState.project = info.project || '';
                statusState.device = info.device || '';
            }
        } catch (_) { /* server may still be starting */ }
        renderStatus();
        try {
            const summary = await reportsService.getSummary();
            const ps = summary && summary.flowStatus && summary.flowStatus.setup
                ? summary.flowStatus.setup.wns_ps
                : undefined;
            if (Number.isFinite(Number(ps))) {
                statusState.wns = (Number(ps) / 1000).toFixed(3);
            } else {
                statusState.wns = undefined;
            }
        } catch (_) { /* no reports yet */ }
        renderStatus();
    };
    renderStatus();


    // ------------------------------------------------------------ MCP server definition
    // P-A3: the server writes <stateDir>/mcp-endpoint-<port>.json once its loopback
    // listener is up. Read it (real port, token, pid) instead of guessing the port.
    const resolveAttachPort = async () => {
        if (!isNative) return 0;
        try {
            const endpoint = readMcpEndpoint(stateDir, { root: cwd, port: mcpPort });
            if (endpoint && endpointAlive(endpoint)) {
                diag(`MCP endpoint: port=${endpoint.port} pid=${endpoint.pid} token=${endpoint.token ? 'yes' : 'no'}`);
                return endpoint.port;
            }
        } catch (err) {
            diag(`MCP endpoint lookup failed: ${(err && err.message) || err}`);
        }
        // Old server without the endpoint file: keep the historical probe.
        return (mcpPort && (await mcpPortOpen(mcpPort))) ? mcpPort : 0;
    };

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
            logFile: path.join(resolveStateDir(root), 'logs', 'mcp.log'),
            allowFlow: config().get('mcp.allowFlow') === true,
            allowWrite: config().get('mcp.allowWrite') === true,
            version: context.extension.packageJSON.version,
            existingPythonPath: process.env.PYTHONPATH,
            // P-A3: prefer the advertised endpoint (real port + token) over the
            // FNV-derived port + 300 ms TCP probe; old servers fall back to it.
            attachPort: await resolveAttachPort(),
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
        // BUG（审计 2026-11-22）：这里原本第一行是 `if (!mcpEmitter) return;`，而
        // mcpEmitter 只在 dynamicProvider=true 时才创建（默认 false），于是整段处理器
        // 被短路 —— 改 fpgaLsp.mcp.allowFlow/allowWrite 后状态栏的 "agent: …" 徽标
        // 永远不更新（要重载窗口）。现在 emitter 只是"可选刷新"，徽标永远更新。
        const affectsMcp = event.affectsConfiguration('fpgaLsp.projectFile')
            || event.affectsConfiguration('fpgaLsp.mcp.allowFlow')
            || event.affectsConfiguration('fpgaLsp.mcp.allowWrite');
        if (affectsMcp && mcpEmitter) mcpEmitter.fire();
        renderAgentBadge();
    }));

    // ------------------------------------------------------------ hints
    // 单一 exe：Verible 已内嵌在 fpgalsp-native.exe 里（格式化 / lint / autofix 都由服务端提供），
    // 所以这里不再探测外部 verible，也不再提示"请安装 Verible"。
    // VHDL 仍只有基础高亮（Verible 不支持 VHDL），这条提示保留。
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument((document) => {
        if (document.languageId !== 'vhdl') return;
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
        // D2: `fpgaLsp/reportsChanged` repaints only what the user can see, debounced per panel.
        requestRefresh(options) {
            for (const panel of Object.values(reportsPanels)) {
                if (panel.visible) panel.requestRefresh(options);
            }
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
                // S2: one friendly sentence + an action, instead of a raw exception string.
                const message = String((err && err.message) || err);
                const friendly = /ENOENT|not found|cannot find/i.test(message)
                    ? 'the language server could not be found (check fpgaLsp.serverPath)'
                    : /ECONNREFUSED|socket hang up|EPIPE/i.test(message)
                        ? 'the language server connection dropped (it may be restarting)'
                        : message;
                const choice = await vscode.window.showErrorMessage(
                    `FPGA: ${friendly}`, 'Show Logs');
                if (choice === 'Show Logs') {
                    await vscode.commands.executeCommand(VSCodeCommands.showLogs);
                }
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
            const dir = path.join(stateDir, 'logs');
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
        const file = path.join(stateDir, 'logs', 'highlight.txt');
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
        // ChipWatcher 里要的是"顶层/实例链/网名[msb:lsb]"这种网表层次名，
        // 后端在 chipWatcherPath 里给；老后端没有这个字段时回落到 path。
        const copied = result.chipWatcherPath || result.path;
        await vscode.env.clipboard.writeText(copied);
        vscode.window.showInformationMessage(`FPGA: copied ChipWatcher path ${copied}`);
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
    // Flow 六步右键：Run（已完成则跳过）/ Reset run（清该 run 的步骤标记后整条 run 重跑）
    // 走 FlowController：输出通道、状态栏、进度通知、日志轮询、结束后的树刷新都在那里；
    // 直接 rpc.execute 只会拿到 runId，界面上什么都看不到（"没有新输出"的根因）。
    register(VSCodeCommands.flowStepRun, (argument) => flowController.runStep(argument, false));
    register(VSCodeCommands.flowStepReset, (argument) => flowController.runStep(argument, true));
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

    // ---- 0.16.0：片上探针（Anlogic ChipWatcher）-----------------------------
    // 三层语义，和 program 家族对齐：
    //   probeStatus  纯读文件（探针清单 + 编译产物），毫秒级
    //   probeConfig  改 chip_debugger/user.mdn（不碰板子，但会改变下次埋进 bit 的探针）
    //   probeCapture 碰硬件：会先用 -mode jtag_burst 把带探针的 bit 灌进 SRAM，必须二次确认
    const probeStatus = async (silent) => {
        const result = await runWithProgress('FPGA: reading ChipWatcher probe setup...',
            COMMANDS.probeStatus, []);
        if (!silent) {
            if (!result || result.ok !== true) {
                openErrors((result && result.reason) || 'ChipWatcher probe setup is not ready');
                return result;
            }
            const watcher = result.watcher || {};
            const buses = (watcher.buses || []).map((b) => b.name).join(', ') || '(none)';
            const rows = [
                `Project        : ${result.project || ''}  (${result.target || ''})`,
                `Probe signals  : ${(result.chip_debugger || {}).count || 0} (chip_debugger/user.mdn)`,
                `Watcher inst   : ${watcher.instance || '(none)'}`,
                `Sample clock   : ${watcher.clock || '(none)'}`,
                `Sample depth   : ${watcher.sample_depth || 0}`,
                `Trigger        : ${watcher.trigger_type || '(none)'} @ ${watcher.trigger_position || 0}`,
                `Buses          : ${buses}`,
                `Compiled bit   : ${watcher.compiled_bit || '(none)'}`,
                `Ready          : ${result.ready ? 'yes' : 'no'} - ${result.reason || ''}`,
            ];
            const doc = await vscode.workspace.openTextDocument({ content: rows.join('\n'), language: 'text' });
            await vscode.window.showTextDocument(doc, { preview: true });
        }
        return result;
    };

    const probeCapture = async () => {
        const status = await probeStatus(true);
        if (!status || status.ok !== true) {
            openErrors((status && status.reason)
                || 'No probe-enabled bitstream. Run the phy flow first so chip_debugger/user.mdn gets embedded.');
            return;
        }
        const watcher = status.watcher || {};
        const confirmed = await vscode.window.showWarningMessage(
            `Capture on-chip probe data?\n` +
            `Watcher: ${watcher.instance}\nClock: ${watcher.clock}\nDepth: ${watcher.sample_depth}\n` +
            `This reloads ${(watcher.compiled_bit || '').split('/').pop()} into SRAM over JTAG (the FPGA stops for ~15 s).`,
            { modal: true }, 'Capture');
        if (confirmed !== 'Capture') return;

        const result = await runWithProgress('FPGA: capturing on-chip probe data (JTAG)...',
            COMMANDS.probeCapture, [{ confirm: true }]);
        if (!result || result.ok !== true) {
            openErrors(`Probe capture failed - ${(result && (result.reason || result.error)) || 'unknown error'}`);
            return;
        }
        const summary = result.summary || {};
        const lines = Object.keys(summary).map((bus) => {
            const item = summary[bus] || {};
            return `${bus}: ${item.distinct} distinct value(s), first ${item.first}, last ${item.last}`;
        });
        vscode.window.showInformationMessage(
            `FPGA probe: ${result.samples} samples on ${result.clock} -> ${result.csv}`,
            'Show details').then(async (picked) => {
            if (picked !== 'Show details') return;
            const content = [
                `Watcher   : ${result.instance}`,
                `Clock     : ${result.clock}`,
                `Depth     : ${result.sample_depth}`,
                `Trigger   : ${result.trigger_mode} (index ${result.trigger_index})`,
                `CSV       : ${result.csv}`,
                '',
                ...lines,
            ];
            const doc = await vscode.workspace.openTextDocument({ content: content.join('\n'), language: 'text' });
            await vscode.window.showTextDocument(doc, { preview: true });
        });
        // 抓取后 watcher/stale 可能变化（下载了新 bit），把树和状态栏一起刷新。
        probeTree.refresh().catch(() => undefined);
    };

    // ---- 0.19.3：ChipWatcher 探针 —— **原生 TreeView**（不再是底栏 webview view）----
    //  0.16~0.19.2 把它注册成 panel 容器的 WebviewViewProvider，部分环境里一直停在
    //  “没有可提供视图数据的已注册数据提供程序”。现在改成和 flow/reports 完全相同的
    //  机制：contributes.views 里的 fpgaLsp.probeView + registerTreeDataProvider，
    //  不依赖 webview provider；编辑区的富面板（ProbePanel）保留给 showProbePanel。
    //  数据全部来自后端的 probeStatus / probeConfig / probeCapture。
    const probeService = {
        getStatus: async () => (await runWithProgress('FPGA: reading ChipWatcher state...',
            COMMANDS.probeStatus, [])) || {},
        applySignals: async (signals) => {
            const result = (await runWithProgress('FPGA: updating chip_debugger/user.mdn...',
                COMMANDS.probeConfig, [{ signals, write: true }])) || {};
            // 编辑区面板改完清单后，树也要跟着刷新（两者看同一份 user.mdn）。
            probeTree.refresh().catch(() => undefined);
            return result;
        },
        // 0.19.3：disabled[] 由后端并行实现（fpgalsp.probe_config{disabled}）。
        // apply_build_config:false = 只落盘清单/disable 配置，不把自写 cwc 塞进下次 gate。
        applyConfig: async (options) => (await runWithProgress('FPGA: updating ChipWatcher probe list...',
            COMMANDS.probeConfig, [{
                signals: Array.isArray(options && options.signals) ? options.signals : [],
                disabled: Array.isArray(options && options.disabled) ? options.disabled : [],
                // 采样时钟 / 深度：TD 把它们当编译期量，设置后要重建 watcher 才生效
                ...(options && options.clock ? { clock: options.clock } : {}),
                ...(options && options.depth ? { depth: options.depth } : {}),
                write: true,
                apply_build_config: false,
            }])) || {},
        capture: async (trigger) => (await runWithProgress('FPGA: capturing on-chip probe data...',
            COMMANDS.probeCapture, [{ confirm: true, trigger: trigger || {} }])) || {},
    };

    // disabled 列表持久化在插件侧 globalState：后端 probe_config 只是执行者，用户
    // 重开窗口后树里还得看到哪几条是 disabled 的。旧后端不认识 disabled 参数时
    // 只保留本地状态（见 src/probeTree.js 的 _applyConfig），并提示需要 0.19.3+ 后端。
    const PROBE_DISABLED_KEY = 'fpgaLsp.probe.disabled';
    const probeGlobalState = (context.globalState && typeof context.globalState.get === 'function')
        ? context.globalState
        : { get: () => undefined, update: () => Promise.resolve() };
    const readProbeDisabled = () => {
        const stored = probeGlobalState.get(PROBE_DISABLED_KEY);
        return Array.isArray(stored) ? stored.map((item) => String(item)) : [];
    };
    const writeProbeDisabled = async (list) => {
        const next = Array.from(new Set((list || []).map((item) => String(item)).filter(Boolean)));
        await probeGlobalState.update(PROBE_DISABLED_KEY, next);
        return next;
    };
    const probeTree = new ProbeTreeProvider({
        getStatus: probeService.getStatus,
        applyConfig: probeService.applyConfig,
        capture: probeService.capture,
        getDisabled: readProbeDisabled,
        setDisabled: writeProbeDisabled,
    });
    context.subscriptions.push(probeTree);
    context.subscriptions.push(vscode.window.registerTreeDataProvider(VIEWS.probe, probeTree));
    // 编辑区里的富面板：showProbePanel 命令打开它（唯一保留的“面板”入口）。
    const probePanel = new ProbePanel(vscode, probeService, VIEWS.probe);

    // 0.19.1：状态栏入口 —— 用户明确要求"chipwatcher 放到状态栏"。
    // 点了就开编辑区面板；文字顺便显示探针规模（字段全在 watcher 里，旧代码读的是
    // 顶层的 present/clock/sample_depth，后端的 Status() 根本不会给这几个键）。
    const probeItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 89);
    probeItem.command = VSCodeCommands.showProbePanel;
    probeItem.text = '$(pulse) ChipWatcher';
    probeItem.tooltip = 'Anlogic ChipWatcher — click to open the probe panel (mark_debug list, trigger, capture)';
    probeItem.show();
    context.subscriptions.push(probeItem);
    const renderProbeItem = (state) => {
        const watcher = (state && state.watcher) || {};
        const nets = Number(watcher.net_count || 0);
        const clock = watcher.clock || (state && state.clock) || '';
        const depth = watcher.sample_depth || (state && state.sample_depth) || '';
        const diag = probeTree.unsupportedDisable
            ? '\nenable/disable: local state only — needs the 0.19.3+ native backend (probe_config disabled[])'
            : '';
        if (state && watcher.present && nets > 0) {
            probeItem.text = `$(pulse) ChipWatcher: ${nets}`;
            probeItem.tooltip =
                `ChipWatcher embedded: ${nets} net(s), clock ${clock || '?'}, depth ${depth || '?'}` +
                (watcher.stale ? ' — STALE vs user.mdn' : '') +
                '\nThe ChipWatcher view lists the probe buses; click a bus to enable/disable it.' + diag;
        } else {
            probeItem.text = '$(pulse) ChipWatcher';
            probeItem.tooltip =
                'ChipWatcher — click to open the probe panel (no probes embedded yet). ' +
                'The chipwatcher view in the FPGA activity bar lists signals and captures.' + diag;
        }
    };
    renderProbeItem(undefined);
    probeService.getStatus().then(renderProbeItem).catch(() => renderProbeItem(undefined));

    const showProbePanel = async () => {
        const state = await probePanel.openInEditor();
        renderProbeItem(state);
    };

    // 树里点一条总线 = 翻转它的 disabled 状态（不弹输入框）。
    // 从命令面板/FPGA Actions 进来（没有参数）时先让用户选一条总线。
    const probeToggleSignal = async (argument) => {
        let name = typeof argument === 'string'
            ? argument
            : (argument && (argument.name || argument.signal)) || '';
        if (!name) {
            const buses = probeTree.listBuses();
            if (!buses.length) {
                vscode.window.showInformationMessage(
                    'FPGA: no ChipWatcher signals to toggle yet - use "Add signals…" first.');
                return undefined;
            }
            const picked = await vscode.window.showQuickPick(buses.map((bus) => ({
                label: bus.label,
                description: bus.disabled ? 'disabled' : 'enabled',
                detail: `width ${bus.width}`,
                bus,
            })), { title: 'Toggle a ChipWatcher signal (enable / disable)' });
            if (!picked) return undefined;
            name = picked.bus.base;
        }
        const outcome = await probeTree.toggleSignal(name);
        renderProbeItem(probeTree.state);
        if (!outcome) return outcome;
        if (probeTree.unsupportedDisable) return outcome;
        const count = probeTree.listBuses().filter((bus) => bus.disabled).length;
        const verb = outcome.disabled ? 'disabled' : 'enabled';
        const result = outcome.result || {};
        if (result.ok !== true) {
            vscode.window.showWarningMessage(
                `FPGA: ChipWatcher signal ${outcome.name} was not ${verb} - `
                + `${result.reason || result.error || 'the backend rejected the probe update'}`);
            return outcome;
        }
        // 后端 disabled 只能通过“自写 cwc”真正进 bit；我们按约定传 apply_build_config:false，
        // 所以这里必须诚实说明“配置已存，位流要等一次应用自写 cwc 的重建”。
        const needsApply = result.requires_apply_build_config === true;
        const suffix = needsApply
            ? ' Saved to the probe config; the bitstream keeps the previous enable/disable state '
              + 'until a rebuild applies the authored ChipWatcher config.'
            : ' Run Place & Route so the probes are rebuilt into the bitstream.';
        vscode.window.showInformationMessage(
            `FPGA: ChipWatcher signal ${outcome.name} ${verb} (${count} disabled in total).${suffix}`);
        return outcome;
    };

    // 树里的 "Add signals…"：逗号/空白分隔，接受 `WHG/x[15:0]` 这种范围写法。
    const probeAddSignals = async () => {
        const value = await vscode.window.showInputBox({
            title: 'Add ChipWatcher signals',
            prompt: 'Signal buses to add, comma separated (e.g. WHG/x[15:0], WHG/flag)',
            placeHolder: 'WHG/x[15:0], WHG/flag',
            ignoreFocusOut: true,
        });
        if (!value || !value.trim()) return undefined;
        const result = await probeTree.addSignals(value);
        renderProbeItem(probeTree.state);
        if (result && result.ok === true) {
            const count = (result.count !== undefined)
                ? result.count
                : ((result.after && result.after.length) || 0);
            vscode.window.showInformationMessage(
                `FPGA: ChipWatcher signal list updated (${count} bit(s)).`
                + (result.reason ? ` ${result.reason}` : ' Run a full flow so the probes are rebuilt into the bitstream.'));
        } else if (result && (result.reason || result.error)) {
            vscode.window.showWarningMessage(`FPGA: probe list update failed - ${result.reason || result.error}`);
        }
        return result;
    };

    register(VSCodeCommands.doctor, async () => {
        try {
            const root = workspaceFolder();
            // doctor 的职责是"把状态打印出来"，任何一项取不到都不该让命令失败：
            // 以前 getProjectInfo() 一抛错整个命令就走 catch 弹错误框，报告根本不会打开
            // （真实 VS Code 集成测试因此长期红：active editor 还停在上一个文档）。
            const info = await getProjectInfo().catch(() => undefined);
            const tdResult = await rpc.execute(COMMANDS.tdInfo, []).catch(() => undefined);
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

                `- Server root: ${serverRoot}`,
                `- Server log: ${config().get('logFile') || (root ? path.join(resolveStateDir(root), 'logs', 'server.log') : '(n/a)')}`,
                '',
                '## Verible (embedded, no external binary needed)',
                `- Format args: ${(config().get('verible.args') || []).length} option(s)`,
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
            { label: '$(pulse) ChipWatcher probe status', command: VSCodeCommands.probeStatus },
            { label: '$(circuit-board) Capture on-chip probe data', command: VSCodeCommands.probeCapture },
            { label: '$(check) Toggle ChipWatcher signal…', command: VSCodeCommands.probeToggleSignal },
            { label: '$(add) Add ChipWatcher signals…', command: VSCodeCommands.probeAddSignals },
            { label: '$(circuit-board) ChipWatcher panel…', command: VSCodeCommands.showProbePanel },
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

    // 0.16.0：片上探针（0.19.3：ChipWatcher 走原生树，编辑区面板是唯一“面板”入口）
    register(VSCodeCommands.probeStatus, () => probeStatus(false));
    register(VSCodeCommands.probeCapture, () => probeCapture());
    register(VSCodeCommands.probeToggleSignal, (argument) => probeToggleSignal(argument));
    register(VSCodeCommands.probeAddSignals, () => probeAddSignals());
    // 采样时钟 / 深度：TD 在 Optimize Gate 之后才消费它们，所以这里只落盘 + 提示重跑。
    register(VSCodeCommands.probeSetClock, async () => {
        const state = await probeService.getStatus().catch(() => ({}));
        const current = (state && state.watcher && state.watcher.clock) || '';
        // 与 TD GUI 一致：时钟是**从列表里选**，不是手输。
        // 列表来自后端的 fpgalsp.probeClocks（TD 时钟树报告里的时钟网名）。
        const listed = await runWithProgress('FPGA: reading clock tree...',
            COMMANDS.probeClocks, []).catch(() => ({}));
        const names = Array.isArray(listed && listed.clocks) ? listed.clocks : [];
        let clock;
        if (names.length) {
            const items = names.map((name) => ({
                label: name,
                description: name === current ? 'current' : '',
            }));
            items.push({ label: '$(edit) Enter manually…', description: '' });
            const picked = await vscode.window.showQuickPick(items, {
                title: 'ChipWatcher sample clock',
                placeHolder: 'Clock net used to sample the probes (TD lists the clock tree nets here)',
            });
            if (!picked) return;
            if (String(picked.label).indexOf('Enter manually') >= 0) {
                const typed = await vscode.window.showInputBox({
                    title: 'ChipWatcher sample clock',
                    value: current,
                    prompt: 'Net name used to sample the probes',
                });
                if (typed === undefined) return;
                clock = String(typed).trim();
            } else {
                clock = String(picked.label);
            }
        } else {
            const typed = await vscode.window.showInputBox({
                title: 'ChipWatcher sample clock',
                prompt: 'No clock tree report yet - run stages:["all"] once, or type a net name (e.g. reg_init/clk).',
                value: current,
            });
            if (typed === undefined) return;
            clock = String(typed).trim();
        }
        const result = await probeService.applyConfig({ clock });
        if (result && result.ok === false) {
            openErrors(`ChipWatcher clock update failed - ${(result.reason || result.error) || 'unknown error'}`);
            return;
        }
        vscode.window.showInformationMessage(
            `FPGA: sample clock saved as "${clock}". TD applies it when it rebuilds the watcher - run stages:["all"].`);
        await probeTree.refresh();
    });
    register(VSCodeCommands.probeSetDepth, async () => {
        const state = await probeService.getStatus().catch(() => ({}));
        const current = (state && state.watcher && state.watcher.sample_depth) || 1024;
        const value = await vscode.window.showInputBox({
            title: 'ChipWatcher sample depth',
            prompt: 'Samples per capture (TD default 1024, range 128..65536). Applied on the next stages:["all"] run.',
            value: String(current),
            validateInput: (text) => {
                const n = Number(text);
                if (!Number.isFinite(n) || n < 128 || n > 65536) return 'Enter a number between 128 and 65536';
                return '';
            },
        });
        if (value === undefined) return;
        const depth = Number(String(value).trim());
        const result = await probeService.applyConfig({ depth });
        if (result && result.ok === false) {
            openErrors(`ChipWatcher depth update failed - ${(result.reason || result.error) || 'unknown error'}`);
            return;
        }
        vscode.window.showInformationMessage(
            `FPGA: sample depth saved as ${depth}. TD applies it when it rebuilds the watcher - run stages:["all"].`);
        await probeTree.refresh();
    });
    register(VSCodeCommands.refreshProbe, () => probeTree.refresh());
    register(VSCodeCommands.showProbePanel, () => showProbePanel());

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

    // X-B3: pin constraint table (read-only). Editing stays in the editor so every change
    // is a normal undoable edit -- a webview that writes .adc behind the user's back is not.
    register(VSCodeCommands.showConstraintTable, async () => {
        const payload = await rpc.execute('fpgalsp.constraintTable', []);
        if (!payload || payload.ok === false) {
            vscode.window.showInformationMessage(
                `FPGA: ${(payload && payload.error) || 'no constraint data'}`);
            return;
        }
        const files = Array.isArray(payload.files) ? payload.files : [];
        if (!files.length) {
            vscode.window.showInformationMessage('FPGA: no .adc pin constraints in this project.');
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            'fpgaLsp.constraintTable', 'FPGA Pin Constraints', vscode.ViewColumn.Active,
            { enableCommandUris: true, retainContextWhenHidden: true });
        panel.webview.html = renderConstraintHtml({ files, project: payload.project });
        context.subscriptions.push(panel);
    });

    // X-B2: QoR trend across runs. Data comes from `fpgalsp.runs`; the panel is a pure
    // renderer (src/trendHtml.js) so it stays testable without VS Code.
    register(VSCodeCommands.showRunTrend, async () => {
        const payload = await rpc.execute('fpgalsp.runs', []);
        if (!payload || payload.ok === false) {
            vscode.window.showInformationMessage(
                `FPGA: ${(payload && payload.error) || 'no flow runs found'}`);
            return;
        }
        const runs = Array.isArray(payload.runs) ? payload.runs : [];
        if (!runs.length) {
            vscode.window.showInformationMessage('FPGA: no flow runs under _Runs/ yet; run a flow first.');
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            'fpgaLsp.runTrend', 'FPGA QoR Trend', vscode.ViewColumn.Active,
            { enableCommandUris: true, retainContextWhenHidden: true });
        panel.webview.html = renderTrendHtml({ runs });
        context.subscriptions.push(panel);
    });

    // B-B7: 面积热点跳源码（与 openTimingPoint 同一个实现，只是提示语更通用）。
    register(VSCodeCommands.openSourceLocation, async (argument) => {
        const target = Array.isArray(argument) ? argument[0] : argument;
        if (!target || !target.file) {
            vscode.window.showInformationMessage('FPGA: no parsable RTL location for this row.');
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
        refreshStatus().catch(() => undefined);
        // X-B7: expose the read-only slice to Copilot Chat. Only in a trusted workspace:
        // these tools read project files and reports, which is exactly what an untrusted
        // workspace forbids.
        try {
            if (vscode.workspace && vscode.workspace.isTrusted === false) {
                diag('lm tools skipped: workspace is not trusted');
            } else {
                const disposables = registerLmTools(vscode, rpc, diag);
                context.subscriptions.push(...disposables);
            }
        } catch (err) {
            diag('lm tools registration failed: ' + ((err && err.message) || err));
        }
        try {
            // Register after the connection is up so no custom notification is missed.
            client.onNotification(METHODS.flowEvent, (event) => {
                if (flowController) flowController.onFlowEvent(event);
                // S1: flow state in the same status line.
                const done = event && (event.type === 'done' || event.type === 'error');
                statusState.flow = done ? '' : ((event && (event.stage || 'running')) || 'running');
                renderStatus();
            });
            // D2: the server's report store changed (flow wrote new reports / a watched report
            // file was touched). Drop the cached payloads so the next fetch is fresh, and repaint
            // **only a visible panel** -- and only once per burst (the panel debounces 150 ms).
            // Hidden panels re-read when they are opened, so there is nothing to do for them.
            let reportsChangedCount = 0;
            client.onNotification(METHODS.reportsChanged, () => {
                reportsChangedCount += 1;
                reportsService.invalidate();
                if (reportsPanel && reportsPanel.visible) {
                    reportsPanel.requestRefresh({ force: true });
                }
                // S3: the Reports *tree* used to keep showing stale data forever -- only the
                // webview was repainted. The tree goes through the same single-flight service,
                // so this cannot double the request count.
                if (reportsTreeProvider) {
                    reportsTreeProvider.refresh().catch(() => undefined);
                }
                refreshStatus().catch(() => undefined);
                if (reportsChangedCount <= 3 || reportsChangedCount % 20 === 0) {
                    diag(`reportsChanged (#${reportsChangedCount})`);
                }
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
            // 0.19.3：窗口加载后 ChipWatcher 树也要先拉一次 probeStatus（否则空树）。
            probeTree.refresh().then((state) => { renderProbeItem(state); return state; }),
        ]);
        const activeDocument = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
        if (activeDocument) writeHighlightReport(activeDocument).catch(() => undefined);
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

    // 测试/调试用 API：extension-host 集成测试用它观察 FlowController 的输出行、状态栏
    // 与 flow 树刷新（这些在 VS Code 的公开 API 里都读不到）。只读暴露，不影响正常行为。
    return {
        flowController,
        flowProvider,
        moduleProvider,
        reportsTreeProvider,
        probeTree,
    };
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
    // F6/R8: the log is buffered now, so flush whatever is still in memory before the extension
    // host goes away (shutdown is the one moment a lost tail would be hard to explain).
    try {
        flushLogSync();
    } catch (_) { /* logging must never break deactivation */ }
    if (restartTimer) {
        clearTimeout(restartTimer);
        restartTimer = undefined;
    }
    if (!client) return undefined;
    shuttingDown = true;
    const instance = client;
    client = undefined;
    return instance.stop();
}

module.exports = { activate, deactivate };
