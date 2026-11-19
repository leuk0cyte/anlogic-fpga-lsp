// P-A3: read the MCP loopback endpoint that the language server advertises.
//
// Why this exists: the extension used to derive the port from a FNV hash of the
// workspace path and then probe it with a TCP connect. That had three problems:
//   * 1000 hash slots -> two workspaces can collide, and whichever starts second
//     silently loses the port;
//   * probing costs up to 300 ms on every activation;
//   * there was no way to authenticate: any local process could talk to the port.
//
// Now the server writes `<stateDir>/mcp-endpoint-<port>.json` with
// {version, port, token, pid, root, startedAt} and the extension reads it. The token
// is what the attach proxy injects into `initialize` (see src/app/mcp_tcp.cpp).
//
// Pure module (no `vscode` import) so test/run.js can exercise it.

'use strict';

const fs = require('fs');
const path = require('path');

const PREFIX = 'mcp-endpoint-';
const SUFFIX = '.json';

/** Parse one endpoint file; returns null when it is missing/corrupt/foreign. */
function parseEndpointFile(file, root) {
    let raw;
    try {
        raw = fs.readFileSync(file, 'utf8');
    } catch (_) {
        return null;
    }
    let payload;
    try {
        payload = JSON.parse(raw);
    } catch (_) {
        return null; // half-written file (the server writes via rename, but be defensive)
    }
    if (!payload || typeof payload !== 'object') return null;
    if (!Number.isInteger(payload.port) || payload.port <= 0 || payload.port > 65535) return null;
    if (root && payload.root && normalize(payload.root) !== normalize(root)) return null;
    return {
        port: payload.port,
        token: typeof payload.token === 'string' ? payload.token : '',
        pid: Number.isInteger(payload.pid) ? payload.pid : 0,
        root: typeof payload.root === 'string' ? payload.root : '',
        startedAt: Number(payload.startedAt) || 0,
        file,
    };
}

function normalize(value) {
    return String(value || '').replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}

/**
 * Find the endpoint for a workspace.
 * @param {string} stateDir  the same dir passed to the server as FPGALSP_STATE_DIR
 * @param {{root?: string, port?: number}} [options]
 * @returns {null | {port:number, token:string, pid:number, root:string, startedAt:number, file:string}}
 */
function readMcpEndpoint(stateDir, options) {
    const opts = options || {};
    if (!stateDir) return null;
    if (Number.isInteger(opts.port) && opts.port > 0) {
        const direct = parseEndpointFile(path.join(stateDir, `${PREFIX}${opts.port}${SUFFIX}`), opts.root);
        if (direct) return direct;
    }
    let names;
    try {
        names = fs.readdirSync(stateDir);
    } catch (_) {
        return null;
    }
    const candidates = names
        .filter((name) => name.startsWith(PREFIX) && name.endsWith(SUFFIX))
        .map((name) => path.join(stateDir, name));
    let best = null;
    for (const file of candidates) {
        const endpoint = parseEndpointFile(file, opts.root);
        if (!endpoint) continue;
        if (!best || endpoint.startedAt >= best.startedAt) best = endpoint;
    }
    return best;
}

/** True when the endpoint's owner process is still alive (Windows-friendly). */
function endpointAlive(endpoint) {
    if (!endpoint || !endpoint.pid) return false;
    try {
        process.kill(endpoint.pid, 0);
        return true;
    } catch (err) {
        // EPERM means "exists but not ours" -> still alive
        return Boolean(err && err.code === 'EPERM');
    }
}

module.exports = { readMcpEndpoint, endpointAlive, parseEndpointFile };