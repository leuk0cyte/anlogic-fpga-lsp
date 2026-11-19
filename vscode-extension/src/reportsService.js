// Single-flight, section-aware cache for `fpgaLsp/reports` (Phase 2 / A4).
//
// Why this exists:
//   * `flowTree` and the reports webview panel both need report data. Before Phase 2 they each
//     sent their own `fpgaLsp/reports` request on every refresh, so opening the panel next to the
//     FPGA view doubled the RPC count (and the server work behind it).
//   * The panel only needs `summary` first, then `timing` / `area` / `qor` on demand.
//
// Contract with the server (all of it optional -- older servers are supported):
//   * request params `{ section: 'summary' | 'timing' | 'area' | 'qor' }` return a payload whose
//     `section` field echoes the requested section and/or which carries a `sections` array.
//   * when the server ignores `section` (returns the full report, no marker) this service detects
//     it once and afterwards always asks for the full report, slicing locally.
//
// Depends only on the injected `rpc` adapter (`request(method, params, token)`), so it is
// trivially testable without VS Code.

'use strict';

const { METHODS, REPORT_SECTIONS } = require('./protocol');

const DEFAULT_TTL_MS = 1500;

/** True when the caller has been cancelled (VS Code CancellationToken or a plain object). */
function isCancelled(token) {
    if (!token) return false;
    if (typeof token.isCancellationRequested === 'boolean') return token.isCancellationRequested;
    return false;
}

/** Thrown by `await` callers whose token fired while the request was in flight. */
class CancelledError extends Error {
    constructor(what) {
        super(`cancelled: ${what || 'request'}`);
        this.name = 'CancelledError';
        this.cancelled = true;
    }
}

class ReportsService {
    /**
     * @param {{request: (method: string, params?: object, token?: any) => Promise<any>}} rpc
     * @param {{ttlMs?: number, now?: () => number}} [options]
     */
    constructor(rpc, options) {
        const opts = options || {};
        this.rpc = rpc;
        this.ttlMs = Number.isFinite(Number(opts.ttlMs)) ? Number(opts.ttlMs) : DEFAULT_TTL_MS;
        this.now = typeof opts.now === 'function' ? opts.now : () => Date.now();
        /** @type {'unknown'|'supported'|'unsupported'} */
        this.sectionSupport = 'unknown';
        this.requests = 0; // observable in tests: total RPC calls actually sent
        this._revision = undefined; // P-C3: 服务端报告 revision（用于条件请求）
        this._cache = new Map(); // key -> {at, payload}
        this._inflight = new Map(); // key -> Promise<payload>
    }

    /** Drop cached payloads (flow finished, project switched, manual refresh, ...). */
    invalidate(section) {
        if (section) {
            this._cache.delete(this._key(section));
            return;
        }
        this._cache.clear();
    }

    get cached() {
        return this._cache.size;
    }

    _key(section) {
        return section || 'full';
    }

    /**
     * Fetch (or reuse) one report payload.
     * @param {{section?: string, token?: any, force?: boolean}} [options]
     */
    async fetch(options) {
        const opts = options || {};
        const wanted = opts.section && REPORT_SECTIONS[opts.section] ? opts.section : '';
        // Once we know the server ignores `section`, always ask for the full report.
        const effective = this.sectionSupport === 'unsupported' ? '' : wanted;
        const key = this._key(effective);

        if (!opts.force) {
            const hit = this._cache.get(key);
            if (hit && this.now() - hit.at < this.ttlMs) {
                if (isCancelled(opts.token)) throw new CancelledError('reports');
                return hit.payload;
            }
        }

        // Single flight: a concurrent caller joins the in-flight request instead of sending
        // a second one (this is what removes the flowTree / reports-panel duplicate).
        let pending = this._inflight.get(key);
        if (!pending) {
            pending = this._send(effective).then((payload) => {
                this._cache.set(key, { at: this.now(), payload });
                this._inflight.delete(key);
                return payload;
            }, (err) => {
                this._inflight.delete(key);
                throw err;
            });
            this._inflight.set(key, pending);
        }
        const payload = await pending;
        if (isCancelled(opts.token)) throw new CancelledError('reports');
        return payload;
    }

    async _send(section) {
        const params = section ? { section } : {};
        // P-C3: conditional request. Once we know the server's revision we ask
        // "is it still that one?" -- an unchanged reply costs no report build at all.
        if (this._revision !== undefined) params.ifRevision = this._revision;
        this.requests += 1;
        let payload;
        try {
            payload = await this.rpc.request(METHODS.reports, params);
        } catch (err) {
            return {
                ok: false,
                error: `fpga-lsp reports request failed: ${(err && err.message) || err}`,
            };
        }
        if (!payload || typeof payload !== 'object') return { ok: false, error: 'empty response' };

        // Unchanged: reuse whatever we already cached for this section. If we have nothing
        // for this section yet (first fetch of it), ask once more without the condition --
        // otherwise the caller would receive `{unchanged:true}` and no data.
        if (payload.unchanged === true) {
            const hit = this._cache.get(this._key(section));
            if (hit) return hit.payload;
            const retry = section ? { section } : {};
            this.requests += 1;
            try {
                payload = await this.rpc.request(METHODS.reports, retry);
            } catch (err) {
                return {
                    ok: false,
                    error: `fpga-lsp reports request failed: ${(err && err.message) || err}`,
                };
            }
            if (!payload || typeof payload !== 'object') {
                return { ok: false, error: 'empty response' };
            }
        }
        if (Number.isFinite(payload.revision)) this._revision = payload.revision;

        if (section && this.sectionSupport === 'unknown') {
            // Did the server understand `section`? It either echoes it or lists the sections it
            // can serve. Anything else means it ignored the parameter (old build) -- and an
            // ignored parameter means the reply we already have **is** the full report, so no
            // second round trip is needed: remember it under the full key and carry on.
            const echoed = payload.section === section;
            const listed = Array.isArray(payload.sections) && payload.sections.length > 0;
            this.sectionSupport = (echoed || listed) ? 'supported' : 'unsupported';
            if (this.sectionSupport === 'unsupported') {
                this._cache.set(this._key(''), { at: this.now(), payload });
            }
        }
        return payload;
    }

    /** `{ok, flow, artifacts, ...}` without the heavy tables when the server supports sections. */
    async getSummary(options) {
        return this.fetch(Object.assign({}, options || {}, { section: 'summary' }));
    }

    /** One section (`timing` / `area` / `qor`); falls back to the full payload. */
    async getSection(section, options) {
        return this.fetch(Object.assign({}, options || {}, { section }));
    }

    /** Everything (used by the "Compare"/dashboard tab and by older servers). */
    async getFull(options) {
        return this.fetch(Object.assign({}, options || {}, { section: '' }));
    }
}

module.exports = { ReportsService, CancelledError, isCancelled, DEFAULT_TTL_MS };