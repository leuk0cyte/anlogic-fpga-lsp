// Pure helpers for the MCP server definition / copied configuration.
//
// Kept free of the `vscode` import so test/run.js can exercise it with plain node.

'use strict';

const path = require('path');

const SERVER_LABEL = 'Anlogic FPGA LSP';
const SERVER_NAME = 'fpga-lsp';

function pythonPathValue(serverRoot, existing) {
    return [serverRoot, existing].filter(Boolean).join(path.delimiter);
}

/**
 * Build the args for the **native** server (`fpgalsp-native.exe --mcp ...`).
 * @param {{root?: string, project?: string, tdPath?: string, logFile?: string,
 *          allowFlow?: boolean, allowWrite?: boolean}} options
 */
function nativeArgs(options) {
    const opts = options || {};
    // Phase 2 / B1：attach 模式只做 stdio <-> loopback 字节泵，engine 在 LSP 进程里。
    if (opts.attachPort) {
        return ['--mcp', '--attach-port', String(opts.attachPort)];
    }
    const args = ['--mcp'];
    if (opts.root) args.push('--root', opts.root);
    if (opts.project) args.push('--project', opts.project);
    if (opts.tdPath) args.push('--td', opts.tdPath);
    if (opts.logFile) args.push('--log', opts.logFile);
    if (opts.allowFlow) args.push('--allow-flow');
    if (opts.allowWrite) args.push('--allow-write');
    return args;
}

/**
 * @param {{
 *   root: string, project?: string, serverRoot: string, tdPath?: string,
 *   logFile?: string, allowFlow?: boolean, allowWrite?: boolean,
 *   existingPythonPath?: string, nativeExe?: string
 * }} options
 */
function buildMcpServerOptions(options) {
    const opts = options || {};
    if (!opts.root) throw new Error('MCP server requires a workspace root');

    // 原生 exe 优先：不需要 PYTHONPATH，参数形状也不同（`--mcp` 而不是 `-m fpgalsp.mcp`）
    if (opts.nativeExe) {
        return {
            label: SERVER_LABEL,
            command: opts.nativeExe,
            args: nativeArgs({
                root: opts.root,
                project: opts.project,
                tdPath: opts.tdPath,
                logFile: opts.logFile || path.join(opts.root, '.fpgalsp', 'logs', 'mcp.log'),
                allowFlow: opts.allowFlow,
                allowWrite: opts.allowWrite,
                attachPort: opts.attachPort,
            }),
            cwd: opts.root,
            env: {},
            version: opts.version || '',
        };
    }

    if (!opts.serverRoot) throw new Error('MCP server requires the fpga-lsp server root');
    const args = ['-m', 'fpgalsp.mcp', '--root', opts.root];
    if (opts.project) args.push('--project', opts.project);
    if (opts.tdPath) args.push('--td', opts.tdPath);
    const logFile = opts.logFile || path.join(opts.root, '.fpgalsp', 'logs', 'mcp.log');
    args.push('--log', logFile);
    if (opts.allowFlow) args.push('--allow-flow');
    if (opts.allowWrite) args.push('--allow-write');
    return {
        label: SERVER_LABEL,
        command: opts.pythonPath || 'python',
        args,
        cwd: opts.root,
        env: { PYTHONPATH: pythonPathValue(opts.serverRoot, opts.existingPythonPath) },
        version: opts.version || '',
    };
}

/** Workspace-scoped server entry: use VS Code's ${workspaceFolder}, not a hardcoded disk path. */
function buildWorkspaceMcpServerOptions(options) {
    const opts = options || {};
    const root = '${workspaceFolder}';

    if (opts.nativeExe) {
        return {
            label: SERVER_LABEL,
            command: opts.nativeExe,
            args: nativeArgs({
                root,
                tdPath: opts.tdPath,
                logFile: `${root}/.fpgalsp/logs/mcp.log`,
                allowFlow: opts.allowFlow,
                allowWrite: opts.allowWrite,
            }),
            env: {},
            version: opts.version || '',
        };
    }

    if (!opts.serverRoot) throw new Error('MCP server requires the fpga-lsp server root');
    const args = ['-m', 'fpgalsp.mcp', '--root', root,
        '--log', `${root}/.fpgalsp/logs/mcp.log`];
    if (opts.tdPath) args.push('--td', opts.tdPath);
    if (opts.allowFlow) args.push('--allow-flow');
    if (opts.allowWrite) args.push('--allow-write');
    return {
        label: SERVER_LABEL,
        command: opts.pythonPath || 'python',
        args,
        env: { PYTHONPATH: pythonPathValue(opts.serverRoot, opts.existingPythonPath) },
        version: opts.version || '',
    };
}

/** VS Code `.vscode/mcp.json` shape (`servers`). */
function buildVsCodeConfig(definition) {
    const server = {
        type: 'stdio',
        command: definition.command,
        args: definition.args,
        env: definition.env,
    };
    return { servers: { [SERVER_NAME]: server } };
}

/** Portable `.mcp.json` / Claude-Cline shape (`mcpServers`). */
function buildPortableConfig(definition) {
    return {
        mcpServers: {
            [SERVER_NAME]: {
                command: definition.command,
                args: definition.args,
                env: definition.env,
            },
        },
    };
}

module.exports = {
    SERVER_LABEL,
    SERVER_NAME,
    buildMcpServerOptions,
    buildVsCodeConfig,
    buildWorkspaceMcpServerOptions,
    buildPortableConfig,
};
