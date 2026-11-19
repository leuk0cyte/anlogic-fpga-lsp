// X-B7: expose a small, read-only slice of the backend to Copilot Chat
// (`vscode.lm.registerTool`).
//
// Why a curated slice instead of all 43 MCP tools: an `lm` tool is invoked by the model
// with a JSON schema, and every tool call is a round trip in the chat loop. These five
// answer the questions users actually ask an assistant ("what project is this?", "show me
// the hierarchy", "how did the timing change?"), and none of them can modify the project.
// Anything that writes stays behind the MCP server and its --allow-write flag.
//
// Kept free of `require('vscode')` (the caller passes the API) so `test/run.js` can
// exercise the definitions with a fake rpc.

'use strict';

const MAX_TEXT = 200000; // characters handed back to the model per call

/** Compact JSON, capped so a huge report cannot blow up the chat context. */
function encode(value) {
    let text;
    try {
        text = JSON.stringify(value);
    } catch (err) {
        text = JSON.stringify({ ok: false, error: String((err && err.message) || err) });
    }
    if (text === undefined) text = 'null';
    if (text.length <= MAX_TEXT) return text;
    return `${text.slice(0, MAX_TEXT)}\n… truncated (${text.length} characters)`;
}

/**
 * @param {{request: Function, execute: Function}} rpc
 * @returns {Array<{name: string, description: string, inputSchema: object,
 *                  invoke: (input?: object) => Promise<string>}>}
 */
function createLmTools(rpc) {
    return [
        {
            name: 'fpgalsp_project_info',
            description: 'Selected Anlogic project: device, top module, file counts and TD summary.',
            inputSchema: { type: 'object', properties: {} },
            invoke: async () => encode(await rpc.execute('fpgalsp.info', [])),
        },
        {
            name: 'fpgalsp_module_hierarchy',
            description: 'Module hierarchy of the current project (one level at a time). Seeds the tree when module is omitted.',
            inputSchema: {
                type: 'object',
                properties: {
                    module: { type: 'string', description: 'Parent module name; omit for the root level.' },
                    limit: { type: 'number', description: 'Maximum children (default 200).' },
                },
            },
            invoke: async (input) => {
                const args = input || {};
                return encode(await rpc.request('fpgaLsp/moduleChildren', {
                    module: args.module || '',
                    limit: Number.isFinite(Number(args.limit)) ? Number(args.limit) : 200,
                }));
            },
        },
        {
            name: 'fpgalsp_reports_summary',
            description: 'QoR/timing/area summary of the current project (no raw report text).',
            inputSchema: { type: 'object', properties: {} },
            invoke: async () => encode(await rpc.request('fpgaLsp/reports', { section: 'summary' })),
        },
        {
            name: 'fpgalsp_flow_status',
            description: 'Current flow state, last message and recent log lines.',
            inputSchema: { type: 'object', properties: {} },
            invoke: async () => encode(await rpc.request('fpgaLsp/flowStatus', {})),
        },
        {
            name: 'fpgalsp_testbench_skeleton',
            description: 'Generate a compilable testbench skeleton for a module (read-only: returns text).',
            inputSchema: {
                type: 'object',
                properties: {
                    module: { type: 'string', description: 'Module name; default is the project top.' },
                    clock: { type: 'string', description: 'Clock port name.' },
                    reset: { type: 'string', description: 'Reset port name.' },
                },
            },
            invoke: async (input) => {
                const args = input || {};
                return encode(await rpc.execute('fpgalsp.testbench', [args]));
            },
        },
    ];
}

/**
 * Register the tools with VS Code. Returns the disposables (empty when the API is absent).
 * @param {any} vscode
 * @param {{request: Function, execute: Function}} rpc
 * @param {(message: string) => void} [log]
 */
function registerLmTools(vscode, rpc, log) {
    const diag = typeof log === 'function' ? log : () => {};
    if (!vscode || !vscode.lm || typeof vscode.lm.registerTool !== 'function') {
        diag('lm.registerTool is not available in this VS Code version');
        return [];
    }
    const disposables = [];
    for (const spec of createLmTools(rpc)) {
        try {
            disposables.push(vscode.lm.registerTool(spec.name, {
                async invoke(options) {
                    const text = await spec.invoke(options && options.input ? options.input : {});
                    return new vscode.LanguageModelToolResult([
                        new vscode.LanguageModelTextPart(typeof text === 'string' ? text : encode(text)),
                    ]);
                },
            }));
        } catch (err) {
            diag(`lm tool ${spec.name}: ${(err && err.message) || err}`);
        }
    }
    diag(`lm tools registered: ${disposables.length}`);
    return disposables;
}

module.exports = { createLmTools, registerLmTools, MAX_TEXT };