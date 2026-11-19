// Dedicated "Reports" activity-bar view: kept separate from Project & Flow.
//
// Two top-level groups (Synthesis / Place & Route); each group opens its own webview and lists the
// parsed report files. Only depends on `vscode` so the unit tests can load it with mock-vscode.

'use strict';

const vscode = require('vscode');
const { METHODS, VSCodeCommands } = require('./protocol');

class ReportsTreeProvider {
    /**
     * @param {{request: Function}} rpc
     * @param {{fetch: Function}} [reportsService] shared single-flight reports cache (Phase 2/A4).
     */
    constructor(rpc, reportsService) {
        this.rpc = rpc;
        this.reportsService = reportsService && typeof reportsService.fetch === 'function'
            ? reportsService : undefined;
        this.reports = undefined;
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
        this._view = undefined;
    }

    attachView(view) {
        this._view = view;
    }

    dispose() {
        this._onDidChangeTreeData.dispose();
        this._view = undefined;
    }

    async refresh(options) {
        const opts = options || {};
        try {
            this.reports = this.reportsService
                ? await this.reportsService.fetch({ section: 'summary', force: opts.force === true })
                : await this.rpc.request(METHODS.reports, {});
        } catch (err) {
            this.reports = { ok: false, error: err && err.message ? err.message : String(err) };
        }
        this._onDidChangeTreeData.fire(undefined);
        return this.reports;
    }

    applyReports(reports) {
        this.reports = reports;
        this._onDidChangeTreeData.fire(undefined);
    }

    _stageSections() {
        const reports = this.reports && this.reports.ok ? this.reports : undefined;
        const stages = [
            ['syn', 'Synthesis', 'chip', VSCodeCommands.showSynReports],
            ['phy', 'Place & Route', 'circuit-board', VSCodeCommands.showPhyReports],
        ];
        const sections = [];
        for (const [stage, label, icon, openCommand] of stages) {
            const data = (reports && reports[stage]) || {};
            const files = (data && data.files) || {};
            const children = [];
            children.push({
                kind: 'action',
                id: `reports:${stage}:open`,
                label: `Open ${label} Reports`,
                command: openCommand,
                icon: 'graph',
            });
            for (const [key, title] of [
                ['qor', 'QoR report'],
                ['timing', 'Timing report'],
                ['area', 'Area report'],
                ['flow_status', 'Flow status'],
            ]) {
                const file = files[key];
                if (typeof file !== 'string' || !file) continue;
                children.push({
                    kind: 'report-file',
                    id: `report:${stage}:${key}`,
                    label: title,
                    path: file,
                });
            }
            if (!reports) {
                children.push({
                    kind: 'placeholder',
                    id: `reports:${stage}:placeholder`,
                    label: this.reports && this.reports.error
                        ? String(this.reports.error)
                        : 'No reports available',
                });
            } else if (data.ok === false) {
                children.push({
                    kind: 'placeholder',
                    id: `reports:${stage}:placeholder`,
                    label: 'No run artifacts for this stage',
                });
            } else if (children.length === 1) {
                children.push({
                    kind: 'placeholder',
                    id: `reports:${stage}:placeholder`,
                    label: 'No report files parsed yet',
                });
            }
            sections.push({
                kind: 'section',
                id: `reports:section:${stage}`,
                label: `${label} reports`,
                icon: new vscode.ThemeIcon(icon),
                description: files && Object.keys(files).length
                    ? `${Object.values(files).filter(Boolean).length} files` : 'not run',
                children,
            });
        }
        return sections;
    }

    getChildren(element) {
        if (!element) return this._stageSections();
        return Array.isArray(element.children) ? element.children : [];
    }

    getTreeItem(node) {
        if (!node) return new vscode.TreeItem('?');
        switch (node.kind) {
            case 'section': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
                item.id = node.id;
                item.iconPath = node.icon;
                item.description = node.description || undefined;
                item.contextValue = 'fpgaReportsStage';
                return item;
            }
            case 'action': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon(node.icon || 'graph');
                item.contextValue = 'fpgaReportsAction';
                item.command = { command: node.command, title: node.label };
                return item;
            }
            case 'report-file': {
                const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon('file');
                item.description = String(node.path || '');
                item.tooltip = String(node.path || '');
                item.contextValue = 'fpgaReportFile';
                item.command = {
                    command: VSCodeCommands.openReportFile,
                    title: 'Open report',
                    arguments: [node.path],
                };
                return item;
            }
            default: {
                const item = new vscode.TreeItem(node.label || '?', vscode.TreeItemCollapsibleState.None);
                item.id = node.id;
                item.iconPath = new vscode.ThemeIcon('comment');
                item.contextValue = 'fpgaReportsPlaceholder';
                return item;
            }
        }
    }
}

module.exports = { ReportsTreeProvider };
