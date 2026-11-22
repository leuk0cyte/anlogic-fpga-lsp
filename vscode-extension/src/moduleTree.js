// Module hierarchy tree for the "FPGA" activity-bar container.
//
// Data comes from the custom `fpgaLsp/moduleTree` request (PROTOCOL-V2.md §1). This module
// deliberately depends only on the `vscode` API (and the shared protocol constants) so the
// unit tests can load it under test/mock-vscode.js.

'use strict';

const crypto = require('crypto');
const vscode = require('vscode');
const { METHODS, VSCodeCommands } = require('./protocol');

const VIEW_ID = 'fpgaLsp.modulesView';

// Phase 2 (A4): every index-changing event is coalesced for this long before the tree asks the
// server again. Short enough to feel instant, long enough that "save while typing" collapses.
const INDEX_DEBOUNCE_MS = 150;

function normalizePath(value) {
    return String(value || '').replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}

function dirOf(path) {
    const text = String(path || '').replace(/\\/g, '/');
    const at = text.lastIndexOf('/');
    return at > 0 ? text.slice(0, at) : '';
}

/** True when `file` lives inside `dir` (case-insensitive, Windows-friendly). */
function isInsideDir(file, dir) {
    const target = normalizePath(file);
    const base = normalizePath(dir);
    if (!target || !base) return false;
    return target === base || target.startsWith(`${base}/`);
}

// TangDynasty primitive families. PROTOCOL-V2 has no dedicated "primitive" flag, so we use
// this heuristic for the icon only; everything undefined-but-not-blackbox still opens usedAt.
const PRIMITIVE_PREFIXES = ['EF2', 'EF3', 'EG4', 'EL3', 'PH1A', 'AL3', 'SL2'];

/** Convert an LSP range (0-based) into a vscode.Range, tolerating missing/partial data. */
function toRange(range) {
    const start = (range && range.start) || { line: 0, character: 0 };
    const end = (range && range.end) || start;
    const sl = Number.isFinite(Number(start.line)) ? Number(start.line) : 0;
    const sc = Number.isFinite(Number(start.character)) ? Number(start.character) : 0;
    const el = Number.isFinite(Number(end.line)) ? Number(end.line) : sl;
    const ec = Number.isFinite(Number(end.character)) ? Number(end.character) : sc;
    return new vscode.Range(sl, sc, el, ec);
}

/** `file:line` for a raw node, using 1-based lines for display (LSP ranges are 0-based). */
function fileLine(raw) {
    if (!raw || !raw.file) return '(no file)';
    const line = raw.range && raw.range.start && Number.isFinite(Number(raw.range.start.line))
        ? Number(raw.range.start.line) + 1
        : 1;
    return `${raw.file}:${line}`;
}

function looksPrimitive(name) {
    const upper = String(name || '').toUpperCase();
    return PRIMITIVE_PREFIXES.some((prefix) => upper.startsWith(prefix));
}

/** Icon for a module node: module / blackbox / primitive. */
function moduleIcon(raw) {
    if (!raw) return new vscode.ThemeIcon('symbol-class');
    if (raw.defined === false) {
        // PROTOCOL-V2 has no explicit "primitive" flag; TD primitive-family names get the
        // primitive icon, everything else undefined is rendered as a blackbox.
        if (looksPrimitive(raw.module)) return new vscode.ThemeIcon('symbol-constant');
        return new vscode.ThemeIcon('circle-slash');
    }
    if (raw.recursive) return new vscode.ThemeIcon('sync');
    return new vscode.ThemeIcon('symbol-class');
}

/** `viewItem` context value used by the view/item/context menus in package.json. */
function contextValueFor(raw, isRoot) {
    if (isRoot) return 'fpgaModuleRoot';
    return raw && raw.defined === false ? 'fpgaModuleBlackbox' : 'fpgaModuleDefined';
}

/** Location of the module definition, only for modules that actually have one. */
function definitionTarget(raw) {
    if (!raw || raw.defined === false || !raw.file) return undefined;
    return {
        uri: vscode.Uri.file(String(raw.file)),
        range: toRange(raw.range),
        where: 'definition',
        module: raw.module,
    };
}

/** Location of the instantiation site, only when the payload provides `usedAt`. */
function instantiationTarget(raw) {
    if (!raw || !raw.usedAt) return undefined;
    const usedAt = raw.usedAt;
    let uri;
    if (usedAt.uri) uri = vscode.Uri.parse(String(usedAt.uri));
    else if (usedAt.file) uri = vscode.Uri.file(String(usedAt.file));
    if (!uri) return undefined;
    return {
        uri,
        range: toRange(usedAt.range),
        where: 'instantiation',
        module: raw.module,
    };
}

// What a plain click on a module node does (PROTOCOL-V2 §1): defined -> definition,
// blackbox / undefined -> instantiation site.
function resolveTarget(raw) {
    if (!raw) return undefined;
    if (raw.defined === false) return instantiationTarget(raw) || definitionTarget(raw);
    return definitionTarget(raw) || instantiationTarget(raw);
}

function moduleTooltip(raw, payload) {
    if (!raw) return '';
    const lines = [];
    lines.push(`${raw.module || '?'}${raw.instance ? ` (instance ${raw.instance})` : ''}`);
    lines.push(fileLine(raw));
    lines.push(`ports: ${Number.isFinite(Number(raw.portCount)) ? Number(raw.portCount) : 0}`);
    if (raw.defined === false) {
        lines.push(raw.blackbox ? 'blackbox: no definition in this project' : 'primitive / undefined module');
        if (raw.usedAt && raw.usedAt.range && raw.usedAt.range.start) {
            lines.push(`click opens the instantiation site (line ${Number(raw.usedAt.range.start.line) + 1})`);
        }
    } else {
        lines.push('click opens the module definition');
    }
    if (raw.recursive) lines.push('recursive instantiation — tree expansion stops here');
    if (payload && payload.project) lines.push(`project: ${payload.project}`);
    return lines.join('\n');
}

class ModuleTreeProvider {
    constructor(rpc) {
        this.rpc = rpc;
        this.payload = undefined;
        this.rootNode = undefined;
        this.error = undefined;
        this.loading = false;
        this.view = undefined;
        this._rootExpanded = false;
        this._seq = 0;
        this._debounce = undefined;
        this._cancellation = undefined;
        /** P2-C: content digest of the tree currently shown (`''` = nothing painted yet). */
        this._digest = '';
        /**
         * `precise` / `wide` split the in-place counter by invalidation granularity:
         * `precise` = one `fire(node)` (1 node repainted), `wide` = the `fire(undefined)` fallback
         * used when several modules changed at once (P3-B audit #4).
         * @type {{requests:number, debounced:number, ignored:number, inPlace:number, skipped:number,
         *         precise:number, wide:number}}
         */
        this.stats = {
            requests: 0, debounced: 0, ignored: 0, inPlace: 0, skipped: 0,
            precise: 0, wide: 0,
        };
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    }

    dispose() {
        if (this._debounce) {
            clearTimeout(this._debounce);
            this._debounce = undefined;
        }
        this._cancelInflight();
        this._onDidChangeTreeData.dispose();
    }

    /** Project root directory of the currently loaded tree ('' when nothing is loaded). */
    projectDir() {
        const al = this.payload && this.payload.al;
        return al ? dirOf(al) : '';
    }

    /** True when `file` belongs to the project currently shown in the tree. */
    ownsFile(file) {
        const dir = this.projectDir();
        return !!dir && isInsideDir(file, dir);
    }

    /**
     * Debounced refresh (150 ms). The last call within the window wins.
     * @param {string} [uri]
     * @param {{reason?: string}} [options]
     */
    requestRefresh(uri, options) {
        const opts = options || {};
        this.stats.debounced += 1;
        if (this._debounce) clearTimeout(this._debounce);
        this._debounce = setTimeout(() => {
            this._debounce = undefined;
            this.refresh(uri, { reason: opts.reason }).catch(() => undefined);
        }, INDEX_DEBOUNCE_MS);
        if (this._debounce && typeof this._debounce.unref === 'function') this._debounce.unref();
    }

    /**
     * Apply a pushed `fpgaLsp/indexChanged` event (Phase 2).
     *
     * Returns 'ignored' | 'in-place' | 'refresh' so callers/tests can assert the targeted path.
     * When the event carries nothing we can patch in, the tree refreshes **immediately** (F6/R1):
     * the server only pushes after it re-indexed, so a second coalescing window would only add
     * latency. `refresh()` cancels any pending debounced fallback so it cannot fire twice.
     */
    applyIndexChanged(event) {
        if (!event || typeof event !== 'object') return 'ignored';

        // Another project changed -> nothing to do (the tree still shows the current project).
        const ownAl = normalizePath(this.payload && this.payload.al);
        const eventAl = normalizePath(event.al || '');
        if (eventAl && ownAl && eventAl !== ownAl) {
            this.stats.ignored += 1;
            return 'ignored';
        }

        const paths = Array.isArray(event.paths) ? event.paths.filter(Boolean) : [];
        if (paths.length) {
            const dir = this.projectDir();
            if (dir && !paths.some((path) => isInsideDir(path, dir))) {
                this.stats.ignored += 1;
                return 'ignored';
            }
        }

        const modules = Array.isArray(event.modules) ? event.modules : [];
        if (event.full !== true && modules.length) {
            const touched = this._applyModuleUpdates(modules);
            if (touched.length) {
                this.stats.inPlace += 1;
                // Keep the digest in sync with what is now painted, so a subsequent full refresh
                // carrying the same (patched) content is a no-op instead of a rebuild.
                this._digest = this._treeDigest(this.payload);
                this._syncMessage();
                // P3-B (audit §7 #4): the in-place branch only rewrote one module's fields, yet it
                // used to `fire(undefined)` -- which makes VS Code re-fetch *every expanded node*
                // (34 / 143 TreeItems on CorpusAlpha / CorpusBeta, 1.16 / 0.97 ms). `fire(node)` invalidates
                // just that node's subtree, so exactly one TreeItem is rebuilt. Everything else is
                // unchanged: the digest was already refreshed above, `getChildren()` still answers
                // from the cached (or lazily loaded) children, so no expansion state is lost and no
                // `moduleChildren` request is repeated. Only when several modules moved (or the
                // node cannot be located) do we fall back to the whole-tree invalidation, because a
                // TreeView fires one element per event and N events are not cheaper than one.
                if (touched.length === 1) {
                    this.stats.precise += 1;
                    this._onDidChangeTreeData.fire(touched[0]);
                } else {
                    this.stats.wide += 1;
                    this._onDidChangeTreeData.fire(undefined);
                }
                return 'in-place';
            }
        }

        // F6/R1: the server only pushes this notification *after* it re-indexed, so waiting for
        // another coalescing window just adds latency (it used to double the end-to-end time:
        // 150 ms here + 150 ms in extension.js). Refresh now; `refresh()` clears a pending
        // debounced fallback so the save-triggered timer cannot fire a duplicate request.
        this.refresh(event.uri || undefined, { reason: 'indexChanged' }).catch(() => undefined);
        return 'refresh';
    }

    /**
     * Patch node payloads in place for module-level events.
     * @returns {Array<any>} the distinct nodes that were matched (empty = nothing to patch)
     */
    _applyModuleUpdates(modules) {
        if (!this.rootNode) return [];
        /** @type {Set<any>} */
        const touched = new Set();
        for (const entry of modules) {
            const name = typeof entry === 'string' ? entry : (entry && entry.module ? entry.module : '');
            if (!name) continue;
            const node = this._findNodeByName(name);
            if (!node) continue;
            if (entry && typeof entry === 'object') {
                const raw = node.raw || {};
                if (Number.isFinite(Number(entry.portCount))) raw.portCount = Number(entry.portCount);
                if (typeof entry.defined === 'boolean') raw.defined = entry.defined;
                node.raw = raw;
            }
            touched.add(node);
        }
        return Array.from(touched);
    }

    _findNodeByName(name) {
        if (!this.rootNode) return undefined;
        const stack = [this.rootNode];
        while (stack.length) {
            const node = stack.pop();
            if (node.raw && node.raw.module === name) return node;
            for (const child of this._childrenOf(node)) stack.push(child);
        }
        return undefined;
    }

    _cancelInflight() {
        const source = this._cancellation;
        this._cancellation = undefined;
        if (source && typeof source.cancel === 'function') {
            try { source.cancel(); } catch (_) { /* already cancelled */ }
        }
        if (source && typeof source.dispose === 'function') {
            try { source.dispose(); } catch (_) { /* ignore */ }
        }
    }

    /** Give the provider a TreeView so it can show `message` empty/loading states. */
    attachView(view) {
        this.view = view;
        this._syncMessage();
    }

    fire() {
        this._onDidChangeTreeData.fire(undefined);
    }

    _setMessage(message) {
        if (this.view) this.view.message = message;
    }

    _friendlyError() {
        const friendly = 'no Anlogic .al project found';
        const detail = this.error && this.error !== friendly ? ` (${this.error})` : '';
        return friendly + detail;
    }

    _syncMessage() {
        if (!this.view) return;
        if (this.loading) {
            this._setMessage('Loading module hierarchy…');
            return;
        }
        if (!this.payload) {
            this._setMessage('Waiting for the FPGA language server…');
            return;
        }
        if (this.payload.ok === false || !this.rootNode) {
            this._setMessage(this._friendlyError());
            return;
        }
        this._setMessage(undefined);
    }

    /**
     * P2-C: content hash of the hierarchy (not of the raw JSON envelope), used to skip the
     * repaint when the server answers with exactly the tree we already show.
     */
    _treeDigest(payload) {
        if (!payload || payload.ok === false || !payload.root) return '';
        try {
            return crypto.createHash('sha1').update(JSON.stringify({
                ok: payload.ok,
                al: payload.al || '',
                project: payload.project || '',
                root: payload.root,
            })).digest('hex');
        } catch (_) {
            return '';
        }
    }

    /**
     * Fetch `fpgaLsp/moduleTree` and rebuild the tree.
     * @param {string} [uri] optional document URI that decides which project is used.
     * @param {{reason?: string}} [options]
     */
    async refresh(uri, options) {
        void options;
        // A direct (event-driven) refresh supersedes a pending debounced fallback: without this
        // the fallback timer would fire ~150 ms later and send the same request twice.
        if (this._debounce) {
            clearTimeout(this._debounce);
            this._debounce = undefined;
        }
        const seq = ++this._seq;
        this.loading = true;
        this._syncMessage();
        this._cancelInflight();
        const source = typeof vscode.CancellationTokenSource === 'function'
            ? new vscode.CancellationTokenSource() : undefined;
        this._cancellation = source;
        let result;
        try {
            const params = uri ? { uri: String(uri) } : {};
            this.stats.requests += 1;
            result = await this.rpc.request(METHODS.moduleTree, params,
                source ? source.token : undefined);
        } catch (err) {
            if (seq !== this._seq) return this.payload; // superseded: a newer refresh is running
            result = { ok: false, error: err && err.message ? err.message : String(err) };
        }
        if (seq !== this._seq) return this.payload; // a newer refresh superseded this one
        this.loading = false;
        this.payload = result && typeof result === 'object' ? result : { ok: false, error: 'empty response' };
        if (this.payload.ok === false || !this.payload.root) {
            this.rootNode = undefined;
            this.error = this.payload.error || 'no Anlogic .al project found';
            this._digest = '';
        } else {
            const digest = this._treeDigest(this.payload);
            if (digest && digest === this._digest && this.rootNode) {
                // P2-C (audit §7 #5): the server re-sent the same hierarchy. Keep the existing
                // nodes -- expansion state and lazily loaded children survive -- and skip the
                // whole-tree repaint (`fire(undefined)` rebuilds every expanded node).
                this.stats.skipped += 1;
                this.loading = false;
                this._syncMessage();
                return this.payload;
            }
            this._digest = digest;
            this.rootNode = this._wrap(this.payload.root, undefined, 'root');
            this.error = undefined;
        }
        this._syncMessage();
        this._onDidChangeTreeData.fire(undefined);
        return this.payload;
    }

    /** Show the friendly empty state when the language server itself cannot be started. */
    setServerError(error) {
        this.loading = false;
        this.payload = { ok: false, error: error ? String(error.message || error) : 'language server unavailable' };
        this.rootNode = undefined;
        this._digest = '';
        this.error = this.payload.error;
        this._syncMessage();
        this._onDidChangeTreeData.fire(undefined);
    }

    _wrap(raw, parent, id) {
        return {
            kind: 'module',
            id,
            raw: raw || {},
            parent,
            isRoot: !parent,
            children: undefined,
        };
    }

    getParent(node) {
        return node && node.parent ? node.parent : undefined;
    }

    /** Flatten the hierarchy for the quick-pick search (module or instance name). */
    findNodes(query) {
        const needle = String(query || '').toLowerCase();
        if (!needle || !this.rootNode) return [];
        const results = [];
        const walk = (node, parentPath) => {
            const raw = (node && node.raw) || {};
            const label = raw.module || '';
            const instance = raw.instance || '';
            const here = parentPath ? `${parentPath}/${instance || label}` : (instance || label);
            if ((label && label.toLowerCase().includes(needle))
                || (instance && instance.toLowerCase().includes(needle))) {
                results.push({ node, label, instance, path: here });
            }
            for (const child of this._childrenOf(node)) walk(child, here);
        };
        walk(this.rootNode, '');
        return results;
    }

    _childrenOf(node) {
        if (!node.children) {
            const rawChildren = node.raw && Array.isArray(node.raw.children) ? node.raw.children : [];
            node.children = rawChildren.map((child, index) => this._wrap(
                child,
                node,
                `${node.id}/${index}:${(child && (child.instance || child.module)) || '?'}`
            ));
        }
        return node.children;
    }

    getChildren(element) {
        if (!element) {
            if (!this.payload || this.payload.ok === false || !this.rootNode) return [];
            return [this.rootNode];
        }
        if (element.kind !== 'module') return [];
        // P-C2: lazy children. The server can answer with just this level
        // (`hasChildren`), which keeps the first payload small on big designs.
        // Kept **synchronous** when children are already known (all existing callers and
        // tests rely on that); a lazy node returns a promise, which the TreeView accepts.
        if (!Array.isArray(element.raw.children) && element.raw.hasChildren === true) {
            if (!element._loadingPromise) {
                element._loadingPromise = this._loadChildren(element).finally(() => {
                    element._loadingPromise = undefined;
                    this._onDidChangeTreeData.fire(element);
                });
            }
            return element._loadingPromise.then(() => this._childrenOf(element));
        }
        return this._childrenOf(element);
    }

    /**
     * P-C2: fetch one level from `fpgaLsp/moduleChildren`.
     * Failures degrade to "no children" instead of a broken tree; the next refresh retries.
     */
    async _loadChildren(node) {
        if (node._loading) return;
        node._loading = true;
        try {
            const response = await this.rpc.request('fpgaLsp/moduleChildren', {
                module: (node.raw && node.raw.module) || '',
                offset: 0,
                limit: 200,
            });
            const children = response && Array.isArray(response.children) ? response.children : [];
            node.raw.children = children;
            if (response && typeof response === 'object') {
                node.raw.total = response.total;
                node.raw.nextOffset = response.nextOffset;
                node.raw.hasMore = response.hasMore === true;
            }
        } catch (_) {
            node.raw.children = [];
        } finally {
            node._loading = false;
        }
    }

    getTreeItem(node) {
        if (!node || node.kind !== 'module') return new vscode.TreeItem('?');
        const raw = node.raw || {};
        const rawChildren = raw && Array.isArray(raw.children) ? raw.children : [];
        // P-C2: `hasChildren` (from moduleChildren) means "expandable but not loaded yet"
        const expandable = rawChildren.length > 0 || raw.hasChildren === true;
        let state = expandable
            ? vscode.TreeItemCollapsibleState.Collapsed
            : vscode.TreeItemCollapsibleState.None;
        if (node.isRoot && rawChildren.length && !this._rootExpanded) {
            this._rootExpanded = true;
            state = vscode.TreeItemCollapsibleState.Expanded;
        }
        const item = new vscode.TreeItem(raw.module || '?', state);
        item.id = node.id;
        item.description = raw.instance || undefined;
        item.iconPath = moduleIcon(raw);
        item.tooltip = moduleTooltip(raw, this.payload);
        item.contextValue = contextValueFor(raw, node.isRoot);
        const target = resolveTarget(raw);
        if (target) {
            item.command = {
                command: VSCodeCommands.openModuleNode,
                title: 'Open Module',
                arguments: [target],
            };
        }
        return item;
    }
}

module.exports = {
    ModuleTreeProvider,
    resolveTarget,
    definitionTarget,
    instantiationTarget,
    moduleIcon,
    moduleTooltip,
    contextValueFor,
    toRange,
    fileLine,
    isInsideDir,
    INDEX_DEBOUNCE_MS,
    VIEW_ID,
};
