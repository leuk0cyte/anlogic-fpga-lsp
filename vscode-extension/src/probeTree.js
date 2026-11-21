// =============================================================================
//  vscode-extension/src/probeTree.js —— ChipWatcher 探针（**原生 TreeView**）
// =============================================================================
//  0.19.3：从底栏 WebviewView（registerWebviewViewProvider）改成和 flow/reports
//  一样的原生 TreeView（registerTreeDataProvider）。底栏 webview view 在部分环境里
//  一直停在“没有可提供视图数据的已注册数据提供程序”，树视图不依赖 webview provider。
//  编辑区的富面板（src/probePanel.js）保留：`fpgaLsp.showProbePanel` 命令打开它。
//
//  service 契约（extension.js 里由 probeService 实现）：
//    getStatus():  Promise<status>                       后端的 fpgalsp.probeStatus
//    applyConfig({signals, disabled}): Promise<result>   后端的 fpgalsp.probeConfig
//    capture():    Promise<result>                        带触发设置采集一次
//    getDisabled(): string[]                             插件侧 globalState 的 disabled 总线
//    setDisabled(list): Promise<string[]>                写回 globalState
//
//  ⚠ 不要在 src/*.js 里写 `fpgaLsp.*` 字面量：test/run.js 的守卫会把它当成
//    "被引用的命令 id"，要求它在 package.json 里声明。命令一律走 VSCodeCommands。
//    viewType 同理用无点形式（'fpgaLspProbePanel'），只有 VIEW_ID 白名单例外。
// =============================================================================

'use strict';

const vscode = require('vscode');
const { VSCodeCommands } = require('./protocol');

const VIEW_ID = 'fpgaLsp.probeView';

// TD GUI 的 ChipWatcher 列表只有 enable/disable 两种状态；树上的图标就这两组
// （外加状态行/动作行用的 circuit-board / refresh / play）。
const ICONS = Object.freeze({
    enabled: 'check',
    disabled: 'circle-slash',
    ready: 'check',
    stale: 'refresh',
    missing: 'circle-slash',
    signal: 'circuit-board',
    capture: 'play',
    add: 'check',
    panel: 'circuit-board',
    refresh: 'refresh',
});

function asNumber(value, fallback) {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
}

/**
 * `WHG/pwm_reg0_i[15:0]` / `WHG/pwm_reg0_i[15]` / `WHG/flag` -> {base, msb, lsb, width}.
 * 位宽/位号都从名字推：compiled.cwc 的 `<bus name>` 与 user.mdn 的逐位清单都用这个格式。
 */
function parseBusName(raw) {
    const text = String(raw === undefined || raw === null ? '' : raw).trim();
    const range = /^(.*?)\[(\d+):(\d+)\]$/.exec(text);
    if (range) {
        const first = Number(range[2]);
        const second = Number(range[3]);
        const msb = Math.max(first, second);
        const lsb = Math.min(first, second);
        return { base: range[1], msb, lsb, width: msb - lsb + 1 };
    }
    const single = /^(.*?)\[(\d+)\]$/.exec(text);
    if (single) {
        const index = Number(single[2]);
        return { base: single[1], msb: index, lsb: index, width: 1 };
    }
    return { base: text, msb: null, lsb: null, width: 1 };
}

/** 树/描述里显示的总线名（带位范围）。 */
function busLabel(base, msb, lsb) {
    if (msb === null || msb === undefined) return base;
    if (lsb === null || lsb === undefined || lsb === msb) return `${base}[${msb}]`;
    return `${base}[${msb}:${lsb}]`;
}

/** `WHG/e2e_hstat` -> `e2e_hstat`（compiled.cwc 的 bus 名不带顶层前缀）。 */
function leafName(name) {
    const text = String(name === undefined || name === null ? '' : name);
    const slash = text.lastIndexOf('/');
    return slash >= 0 ? text.slice(slash + 1) : text;
}

/**
 * 名字匹配，与后端 probe.cpp::SameBusName 同语义：完全相等，或一方是另一方
 * `/` 边界之后的尾段（`WHG/x` vs `x`；cwc 里的名字不带顶层前缀）。
 */
function sameBusName(left, right) {
    const a = String(left === undefined || left === null ? '' : left);
    const b = String(right === undefined || right === null ? '' : right);
    if (!a || !b) return false;
    if (a === b) return true;
    const tail = (longer, shorter) => longer.length > shorter.length
        && longer.endsWith(shorter)
        && longer[longer.length - shorter.length - 1] === '/';
    return tail(a, b) || tail(b, a);
}

/**
 * user.mdn 的逐位清单 (`WHG/x[0]`, `WHG/x[1]`, …) 按 base 聚合成总线。
 * 返回 [{base, msb, lsb, width, bits}]，按 base 排序（稳定，刷新不跳）。
 */
function groupSignals(signals) {
    const groups = new Map();
    for (const raw of Array.isArray(signals) ? signals : []) {
        const signal = String(raw === undefined || raw === null ? '' : raw).trim();
        if (!signal) continue;
        const bit = /^(.*?)\[(\d+)\]$/.exec(signal);
        const base = bit ? bit[1] : signal;
        const index = bit ? Number(bit[2]) : null;
        let group = groups.get(base);
        if (!group) {
            group = { base, indices: [], bits: [] };
            groups.set(base, group);
        }
        if (index === null) {
            group.scalar = true;
        } else if (!group.indices.includes(index)) {
            group.indices.push(index);
        }
        if (!group.bits.includes(signal)) group.bits.push(signal);
    }
    const buses = [];
    for (const group of groups.values()) {
        group.indices.sort((left, right) => left - right);
        const msb = group.indices.length ? group.indices[group.indices.length - 1] : null;
        const lsb = group.indices.length ? group.indices[0] : null;
        buses.push({
            base: group.base,
            msb,
            lsb,
            width: msb === null ? 1 : msb - lsb + 1,
            bits: group.bits.slice(),
            // 逐位清单里出现纯 base（没有 [i]）的标量，保留 bits 即可，宽度按 1 算
            scalar: group.scalar === true && group.indices.length === 0,
        });
    }
    buses.sort((left, right) => left.base.localeCompare(right.base, 'en'));
    return buses;
}

/**
 * 没有 user.mdn 清单时退回 `watcher.buses`（compiled.cwc 里的总线，名字可能自带
 * `[msb:lsb]`）。Status 的 bus 项只有 {name, clock, type, enabled}，位宽只能从名字推。
 */
function busesFromWatcher(watcher) {
    const list = watcher && Array.isArray(watcher.buses) ? watcher.buses : [];
    const buses = [];
    for (const entry of list) {
        const parsed = parseBusName(entry && entry.name);
        if (!parsed.base) continue;
        buses.push({ base: parsed.base, msb: parsed.msb, lsb: parsed.lsb, width: parsed.width, bits: [] });
    }
    buses.sort((left, right) => left.base.localeCompare(right.base, 'en'));
    return buses;
}

class ProbeTreeProvider {
    /**
     * @param {{getStatus: Function, applyConfig?: Function, capture?: Function,
     *          getDisabled?: Function, setDisabled?: Function}} service
     */
    constructor(service) {
        this.service = service || {};
        this.viewType = VIEW_ID;
        this.state = undefined;
        this.loading = false;
        this.unsupportedDisable = false;
        this._hintShown = false;
        this._knownBuses = new Map();
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    }

    dispose() {
        this._onDidChangeTreeData.dispose();
        this._knownBuses.clear();
    }

    fire() {
        this._onDidChangeTreeData.fire(undefined);
    }

    // --------------------------------------------------------------- disabled state

    _storedDisabled() {
        try {
            const list = this.service.getDisabled ? this.service.getDisabled() : [];
            return Array.isArray(list) ? list.map((item) => String(item)) : [];
        } catch (err) {
            return [];
        }
    }

    /**
     * 后端 0.19.3+ 在 probeStatus 里回 `disabled[]`（权威）；旧后端没有这个字段，
     * 就退回插件侧 globalState（用户上一次 toggle 的结果）。
     */
    _disabledList() {
        const status = this.state && typeof this.state === 'object' ? this.state : {};
        if (Array.isArray(status.disabled)) return status.disabled.map((item) => String(item));
        return this._storedDisabled();
    }

    _busDisabled(disabledEntries, bus) {
        const entries = Array.isArray(disabledEntries) ? disabledEntries : [];
        if (entries.some((entry) => sameBusName(entry, bus.base))) return true;
        if (!bus.bits || !bus.bits.length) return false;
        return bus.bits.every((bit) => entries.some((entry) => sameBusName(entry, bit)));
    }

    // --------------------------------------------------------------------- data

    /** 全部总线：user.mdn（将构建的清单）优先，watcher（已进 bit 的）补位。 */
    _buses() {
        const status = this.state && typeof this.state === 'object' ? this.state : {};
        const mdn = status.chip_debugger || {};
        const mdnBuses = groupSignals(mdn.signals);
        const watcherBuses = busesFromWatcher(status.watcher);
        const disabledEntries = this._disabledList();

        // 按叶子名合并：user.mdn 里是 `WHG/e2e_hstat[4:0]`，compiled.cwc 里的 bus 名是
        // `e2e_hstat[4:0]`（不带顶层前缀）。不合并就会出现重复的两条总线。
        const merged = new Map();
        for (const bus of mdnBuses) merged.set(leafName(bus.base), bus);
        for (const extra of watcherBuses) {
            const key = leafName(extra.base);
            const existing = merged.get(key);
            if (!existing) {
                merged.set(key, extra);
            } else if (existing.msb === null && extra.msb !== null) {
                // user.mdn 里是标量/逐位清单而 cwc 给了范围：用 cwc 的位宽补全显示。
                existing.msb = extra.msb;
                existing.lsb = extra.lsb;
                existing.width = extra.width;
            }
        }
        for (const bus of merged.values()) this._knownBuses.set(leafName(bus.base), bus);

        // 旧后端可能把 disabled 总线从 user.mdn 移除了；只要本地/后端还记着它 disabled，
        // 就继续显示这一行，否则用户没法在树里重新启用它。
        for (const [key, bus] of this._knownBuses) {
            if (merged.has(key)) continue;
            const mentioned = disabledEntries.some((entry) => sameBusName(entry, bus.base));
            if (mentioned) merged.set(key, bus);
        }

        const out = Array.from(merged.values()).map((bus) => {
            const disabledBits = bus.bits.filter(
                (bit) => disabledEntries.some((entry) => sameBusName(entry, bit)));
            return Object.assign({}, bus, {
                disabled: this._busDisabled(disabledEntries, bus),
                disabledBits,
            });
        });
        out.sort((left, right) => left.base.localeCompare(right.base, 'en'));
        return out;
    }

    /** 传给 probeConfig 的完整清单（含 disabled 总线：后端用 disabled[] 标记，不删清单）。 */
    _allSignals(buses) {
        const out = [];
        for (const bus of buses) {
            if (bus.bits && bus.bits.length) {
                for (const bit of bus.bits) out.push(bit);
            } else {
                out.push(busLabel(bus.base, bus.msb, bus.lsb));
            }
        }
        return out;
    }

    /**
     * 把「总线级」disabled 条目展开成逐位名字再交给后端。
     * 后端 `ResolveDisabled` 目前只稳定解析单根 `WHG/x[2]` / 范围（总线基名的解析依赖
     * 后端修复）；逐位写法和总线级语义等价（cwc 里逐个 net 写 data="disable"，
     * 整条都 disable 时 bus 的 is_enabled 也是 "no"），所以这里主动展开更稳。
     */
    _expandDisabled(entries, buses) {
        const out = [];
        const push = (value) => { if (!out.includes(value)) out.push(value); };
        for (const entry of Array.isArray(entries) ? entries : []) {
            const text = String(entry || '');
            if (!text) continue;
            const bit = /\[\d+\]$/.test(text);
            const bus = bit ? undefined : buses.find((item) => sameBusName(item.base, text));
            if (bus && bus.bits && bus.bits.length) {
                for (const item of bus.bits) push(item);
            } else {
                push(text);
            }
        }
        return out;
    }

    // ----------------------------------------------------------------- refresh

    /** 重新拉一次 probeStatus 并重画。 */
    async refresh() {
        if (this.loading) return this.state;
        this.loading = true;
        this.fire();
        let state;
        try {
            state = this.service.getStatus ? await this.service.getStatus() : {};
        } catch (err) {
            state = { ok: false, reason: String((err && err.message) || err) };
        }
        this.state = state && typeof state === 'object' ? state : {};
        this.loading = false;
        this.fire();
        return this.state;
    }

    /**
     * 调后端 probeConfig{signals, disabled, write:true, apply_build_config:false}。
     * 旧后端根本没有 `disabled` 参数（返回里没有 disabled 字段）时只保留本地状态，
     * 并提示需要 0.19.3+ 后端 —— 不能假装 disable 生效了。
     * 新后端拒绝请求（ok:false）是真实错误，交给调用方报错，不归到“旧后端”里。
     */
    async _applyConfig(signals, disabled) {
        if (typeof this.service.applyConfig !== 'function') return { ok: false, error: 'no probe config service' };
        let result;
        try {
            result = await this.service.applyConfig({ signals, disabled });
        } catch (err) {
            result = { ok: false, error: String((err && err.message) || err) };
        }
        const hasDisabledField = !!(result && Array.isArray(result.disabled));
        this.unsupportedDisable = !hasDisabledField;
        if (!hasDisabledField) this._showUnsupportedHint(result);
        return result || {};
    }

    _showUnsupportedHint(result) {
        if (this._hintShown) return;
        this._hintShown = true;
        const detail = (result && (result.reason || result.error)) || '';
        vscode.window.showWarningMessage(
            'FPGA: ChipWatcher enable/disable is kept locally only - the native backend needs '
            + '0.19.3+ (probe_config disabled[]). Run the 0.19.3 flow rebuild for it to take effect.'
            + (detail ? ` Backend said: ${detail}` : ''));
    }

    // ------------------------------------------------------------------ actions

    /** 供 quick pick / 命令入口使用：当前树里的总线（带 label / disabled）。 */
    listBuses() {
        return this._buses().map((bus) => Object.assign({}, bus, {
            label: busLabel(bus.base, bus.msb, bus.lsb),
        }));
    }

    /** 点击总线行：把这条总线在 disabled 列表里翻转，然后写回后端 + globalState。 */
    async toggleSignal(name) {
        const target = String(name === undefined || name === null ? '' : name).trim();
        if (!target) return undefined;
        const buses = this._buses();
        const bus = buses.find((item) => sameBusName(item.base, target))
            || buses.find((item) => item.bits.some((bit) => sameBusName(bit, target)));
        const base = bus ? bus.base : target;
        const bits = bus ? bus.bits : [];
        const disabledEntries = this._disabledList();
        const wasDisabled = bus
            ? this._busDisabled(disabledEntries, bus)
            : disabledEntries.some((entry) => sameBusName(entry, target));
        const keep = (entry) => !sameBusName(entry, base)
            && !bits.some((bit) => sameBusName(entry, bit));
        const semantic = wasDisabled
            ? disabledEntries.filter(keep)
            : disabledEntries.filter(keep).concat([base]);
        const list = this._expandDisabled(semantic, buses);
        if (typeof this.service.setDisabled === 'function') {
            try {
                await this.service.setDisabled(list);
            } catch (err) {
                // 本地状态写不进去也要继续：后端能不能接手由 _applyConfig 决定
            }
        }
        const result = await this._applyConfig(this._allSignals(buses), list);
        await this.refresh();
        return { name: base, disabled: !wasDisabled, unsupported: this.unsupportedDisable, result };
    }

    /** "Add signals…"：`WHG/x[15:0], WHG/flag` -> 追加到 user.mdn（并把它们重新启用）。 */
    async addSignals(text) {
        const additions = String(text === undefined || text === null ? '' : text)
            .split(/[,;\s]+/)
            .map((item) => item.trim())
            .filter(Boolean);
        if (!additions.length) return undefined;
        const buses = this._buses();
        const signals = this._allSignals(buses);
        for (const addition of additions) {
            if (!signals.includes(addition)) signals.push(addition);
        }
        const disabledEntries = this._disabledList();
        const remaining = disabledEntries.filter((entry) => !additions.some((addition) => {
            const parsed = parseBusName(addition);
            if (sameBusName(entry, parsed.base) || sameBusName(entry, addition)) return true;
            const known = buses.find((bus) => sameBusName(bus.base, parsed.base));
            return !!known && known.bits.some((bit) => sameBusName(entry, bit));
        }));
        const list = this._expandDisabled(remaining, buses);
        if (typeof this.service.setDisabled === 'function') {
            try {
                await this.service.setDisabled(list);
            } catch (err) {
                // 同上
            }
        }
        const result = await this._applyConfig(signals, list);
        await this.refresh();
        return result;
    }

    // -------------------------------------------------------------- tree nodes

    _statusNode() {
        const status = this.state && typeof this.state === 'object' ? this.state : {};
        const watcher = status.watcher || {};
        const present = watcher.present === true;
        const stale = watcher.stale === true;
        const clock = watcher.clock || status.clock || (status.sampling && status.sampling.clock) || '';
        const depth = asNumber(watcher.sample_depth || status.sample_depth
            || (status.sampling && status.sampling.depth), 0);
        const nets = asNumber(watcher.net_count, 0);
        const enabledNets = asNumber(watcher.enabled_net_count, nets);
        // disabled：顶层来自 .fpgalsp/probe/config.txt（请求）；watcher.disabled_net_count
        // 是 compiled.cwc 里**实际**标成 disable 的数量（重建过才非 0）。
        const disabledCount = asNumber(status.disabled_count, asNumber(watcher.disabled_count, 0));
        const disabledInBit = asNumber(watcher.disabled_net_count, 0);

        let label = 'ChipWatcher: not embedded';
        let icon = ICONS.missing;
        if (present && stale) {
            label = 'ChipWatcher: stale (rebuild Place & Route)';
            icon = ICONS.stale;
        } else if (present) {
            label = 'ChipWatcher: ready';
            icon = ICONS.ready;
        } else if (this.loading) {
            label = 'ChipWatcher: reading…';
        }

        const description = present
            ? [
                clock ? `clock ${clock}` : '',
                depth ? `depth ${depth}` : '',
                `${nets} net${nets === 1 ? '' : 's'}`,
                disabledCount ? `${disabledCount} disabled` : '',
            ].filter(Boolean).join(' · ')
            : '';

        const unresolved = Array.isArray(status.disabled_unresolved) ? status.disabled_unresolved : [];
        const tooltip = [
            'Anlogic ChipWatcher probe status',
            `present     : ${present ? 'yes' : 'no'}`,
            `stale       : ${stale ? 'yes - user.mdn changed after the bitstream was built' : 'no'}`,
            `clock       : ${clock || '(none)'}`,
            `sample depth: ${depth || '(unknown)'}`,
            `nets        : ${nets} (${enabledNets} enabled)`,
            `disabled    : ${disabledCount} bit(s) requested via the probe config; `
                + `${disabledInBit} marked disable in the compiled watcher`,
            status.disabled_unresolved && unresolved.length
                ? `unresolved  : ${unresolved.join(', ')}`
                : '',
            status.reason ? `reason      : ${status.reason}` : '',
            this.unsupportedDisable
                ? 'enable/disable: local state only - needs the 0.19.3+ native backend (probe_config disabled[]).'
                : '',
        ].filter(Boolean).join('\n');

        return {
            kind: 'status',
            id: 'probe:status',
            label,
            description,
            icon,
            tooltip,
            contextValue: 'fpgaProbeStatus',
        };
    }

    _signalsGroup() {
        const buses = this._buses();
        const children = buses.map((bus) => {
            const description = [`width ${bus.width}`];
            if (bus.disabled) description.push('disabled');
            else if (bus.disabledBits.length) description.push(`${bus.disabledBits.length} bit(s) disabled`);
            return {
                kind: 'signal',
                id: `probe:signal:${bus.base}`,
                label: busLabel(bus.base, bus.msb, bus.lsb),
                description: description.join(' · '),
                disabled: bus.disabled,
                width: bus.width,
                bits: bus.bits,
                command: {
                    command: VSCodeCommands.probeToggleSignal,
                    title: 'Toggle ChipWatcher Signal',
                    arguments: [bus.base],
                },
            };
        });
        if (!children.length) {
            children.push({
                kind: 'placeholder',
                id: 'probe:signals:empty',
                label: this.loading ? 'Reading the probe list…' : 'No probe signals - use "Add signals…" under Actions',
                icon: ICONS.missing,
            });
        }
        return {
            kind: 'group',
            id: 'probe:signals',
            label: 'Signals',
            icon: ICONS.signal,
            description: buses.length ? `${buses.length} bus${buses.length === 1 ? '' : 'es'}` : 'none',
            children,
        };
    }

    _actionsGroup() {
        const status = this.state && typeof this.state === 'object' ? this.state : {};
        const watcher = status.watcher || {};
        const ready = watcher.present === true && watcher.stale !== true;
        return {
            kind: 'group',
            id: 'probe:actions',
            label: 'Actions',
            icon: ICONS.signal,
            description: '',
            children: [
                {
                    kind: 'action',
                    id: 'probe:action:capture',
                    label: 'Capture now',
                    description: ready ? '' : 'not ready',
                    icon: ICONS.capture,
                    command: { command: VSCodeCommands.probeCapture, title: 'Capture On-Chip Probe Data' },
                },
                {
                    kind: 'action',
                    id: 'probe:action:add',
                    label: 'Add signals…',
                    icon: ICONS.add,
                    command: { command: VSCodeCommands.probeAddSignals, title: 'Add ChipWatcher Signals' },
                },
                {
                    kind: 'action',
                    id: 'probe:action:panel',
                    label: 'Open rich panel',
                    description: 'editor tab',
                    icon: ICONS.panel,
                    command: { command: VSCodeCommands.showProbePanel, title: 'ChipWatcher Panel' },
                },
                {
                    kind: 'action',
                    id: 'probe:action:refresh',
                    label: 'Refresh',
                    icon: ICONS.refresh,
                    command: { command: VSCodeCommands.refreshProbe, title: 'Refresh ChipWatcher' },
                },
            ],
        };
    }

    getChildren(element) {
        if (!element) return [this._statusNode(), this._signalsGroup(), this._actionsGroup()];
        return Array.isArray(element.children) ? element.children : [];
    }

    getTreeItem(node) {
        if (!node) return new vscode.TreeItem('?');
        switch (node.kind) {
            case 'status': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description || undefined;
                item.iconPath = new vscode.ThemeIcon(node.icon);
                item.tooltip = node.tooltip || node.label;
                item.contextValue = node.contextValue || 'fpgaProbeStatus';
                return item;
            }
            case 'group': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
                item.id = node.id;
                item.description = node.description || undefined;
                item.iconPath = new vscode.ThemeIcon(node.icon);
                item.tooltip = node.label;
                item.contextValue = 'fpgaProbeGroup';
                return item;
            }
            case 'signal': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description;
                item.iconPath = new vscode.ThemeIcon(node.disabled ? ICONS.disabled : ICONS.enabled);
                item.tooltip = [
                    node.label,
                    `width ${node.width}`,
                    node.disabled ? 'disabled (click to enable)' : 'enabled (click to disable)',
                    'Disabling writes data="disable" into the ChipWatcher config (0.19.3+ backend); '
                        + 'run Place & Route to rebuild the probes into the bitstream.',
                ].join('\n');
                item.contextValue = 'fpgaProbeSignal';
                item.command = node.command;
                return item;
            }
            case 'action': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.description = node.description || undefined;
                item.iconPath = new vscode.ThemeIcon(node.icon);
                item.tooltip = node.label;
                item.contextValue = 'fpgaProbeAction';
                item.command = node.command;
                return item;
            }
            case 'placeholder':
            default: {
                const item = new vscode.TreeItem(node.label || '?', vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon(node.icon || 'info');
                item.contextValue = 'fpgaProbePlaceholder';
                return item;
            }
        }
    }
}

module.exports = {
    ProbeTreeProvider,
    VIEW_ID,
    groupSignals,
    parseBusName,
    busLabel,
    busesFromWatcher,
};
