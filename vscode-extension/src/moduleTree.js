// Module hierarchy tree for the "FPGA" activity-bar container.
//
// Data comes from the custom `fpgaLsp/moduleTree` request (PROTOCOL-V2.md §1). This module
// deliberately depends only on the `vscode` API (and the shared protocol constants) so the
// unit tests can load it under test/mock-vscode.js.

'use strict';

const vscode = require('vscode');
const { METHODS, VSCodeCommands } = require('./protocol');

const VIEW_ID = 'fpgaLsp.modulesView';

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
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    }

    dispose() {
        this._onDidChangeTreeData.dispose();
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
     * Fetch `fpgaLsp/moduleTree` and rebuild the tree.
     * @param {string} [uri] optional document URI that decides which project is used.
     */
    async refresh(uri) {
        const seq = ++this._seq;
        this.loading = true;
        this._syncMessage();
        let result;
        try {
            const params = uri ? { uri: String(uri) } : {};
            result = await this.rpc.request(METHODS.moduleTree, params);
        } catch (err) {
            result = { ok: false, error: err && err.message ? err.message : String(err) };
        }
        if (seq !== this._seq) return this.payload; // a newer refresh superseded this one
        this.loading = false;
        this.payload = result && typeof result === 'object' ? result : { ok: false, error: 'empty response' };
        if (this.payload.ok === false || !this.payload.root) {
            this.rootNode = undefined;
            this.error = this.payload.error || 'no Anlogic .al project found';
        } else {
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
        if (element.kind === 'module') return this._childrenOf(element);
        return [];
    }

    getTreeItem(node) {
        if (!node || node.kind !== 'module') return new vscode.TreeItem('?');
        const raw = node.raw || {};
        const rawChildren = raw && Array.isArray(raw.children) ? raw.children : [];
        let state = rawChildren.length
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
    VIEW_ID,
};
