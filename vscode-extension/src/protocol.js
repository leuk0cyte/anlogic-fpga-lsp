// Shared names for the fpga-lsp "v2" custom protocol.
// The authoritative contract lives in ../../PROTOCOL-V2.md; keep this file as the single
// place in the client where method/command strings are spelled.

'use strict';

// Custom JSON-RPC methods (server -> client and client -> server).
const METHODS = Object.freeze({
    moduleTree: 'fpgaLsp/moduleTree',
    reports: 'fpgaLsp/reports',
    flowStatus: 'fpgaLsp/flowStatus',
    flowEvent: 'fpgaLsp/flowEvent',
    // Phase 2: pushed by the server when its HDL index changed (file saved / added / removed).
    // Payload (all fields optional, see A4-REPORT.md §2):
    //   { al?, project?, paths?: string[], modules?: string[], full?: boolean, generation? }
    // Older servers never send it -> the client degrades to a debounced full refresh.
    indexChanged: 'fpgaLsp/indexChanged',
    // Server -> client notification: the parsed report store changed (flow wrote new reports,
    // a watched file was touched, ...). Payload: { stage?, sections?: string[], generatedAt? }.
    reportsChanged: 'fpgaLsp/reportsChanged',
});

// `fpgaLsp/reports` section hints (Phase 2). The server may ignore them (older builds) --
// the client then falls back to one full request and slices locally.
const REPORT_SECTIONS = Object.freeze({
    summary: 'summary',
    timing: 'timing',
    area: 'area',
    qor: 'qor',
});

// Commands executed through the standard `workspace/executeCommand` request.
const COMMANDS = Object.freeze({
    executeCommand: 'workspace/executeCommand',
    runFlow: 'fpgalsp.runFlow',
    cancelFlow: 'fpgalsp.cancelFlow',
    check: 'fpgalsp.check',
    info: 'fpgalsp.info',
    restart: 'fpgalsp.restart',
    cables: 'fpgalsp.cables',
    program: 'fpgalsp.program',
    svfgen: 'fpgalsp.svfgen',
    runAnalysis: 'fpgalsp.runAnalysis',
    compareStrategies: 'fpgalsp.compareStrategies',
    instantiate: 'fpgalsp.instantiate',
    projects: 'fpgalsp.projects',
    setProject: 'fpgalsp.setProject',
    tdInfo: 'fpgalsp.tdInfo',
    debugHighlight: 'fpgalsp.debugHighlight',
    copyInstantiation: 'fpgalsp.copyInstantiation',
    symbolPath: 'fpgalsp.symbolPath',
    duplicateModules: 'fpgalsp.duplicateModules',
    searchHierarchy: 'fpgalsp.searchHierarchy',
    addSourceFile: 'fpgalsp.addSourceFile',
    setProjectName: 'fpgalsp.setProjectName',
    updateProject: 'fpgalsp.updateProject',
    // 0.16.0: on-chip probe (Anlogic ChipWatcher)
    probeStatus: 'fpgalsp.probeStatus',
    probeConfig: 'fpgalsp.probeConfig',
    probeCapture: 'fpgalsp.probeCapture',
});

// VS Code contribution ids (`contributes.commands`). All of these must be declared in
// package.json and registered in extension.js.
const VSCodeCommands = Object.freeze({
    // module hierarchy
    openModuleNode: 'fpgaLsp.openModuleNode',
    goToDefinition: 'fpgaLsp.goToDefinition',
    goToInstantiation: 'fpgaLsp.goToInstantiation',
    copyModulePath: 'fpgaLsp.copyModulePath',
    refreshModules: 'fpgaLsp.refreshModules',
    // flow
    runSyn: 'fpgaLsp.runSyn',
    runPhy: 'fpgaLsp.runPhy',
    runAll: 'fpgaLsp.runAll',
    cancelFlow: 'fpgaLsp.cancelFlow',
    refreshFlow: 'fpgaLsp.refreshFlow',
    showFlowLog: 'fpgaLsp.showFlowLog',
    // reports
    showReports: 'fpgaLsp.showReports',
    showSynReports: 'fpgaLsp.showSynReports',
    showPhyReports: 'fpgaLsp.showPhyReports',
    refreshReports: 'fpgaLsp.refreshReports',
    openArtifact: 'fpgaLsp.openArtifact',
    openReportFile: 'fpgaLsp.openReportFile',
    // legacy v1 commands (kept for backwards compatibility)
    check: 'fpgalsp.check',
    info: 'fpgalsp.info',
    restart: 'fpgalsp.restart',
    // launch diagnostics
    showLogs: 'fpgaLsp.showLogs',
    doctor: 'fpgaLsp.doctor',
    // v3: analysis / programming / strategy comparison
    showActions: 'fpgaLsp.showActions',
    runIncrementalRoute: 'fpgaLsp.runIncrementalRoute',
    runAnalysis: 'fpgaLsp.runAnalysis',
    compareStrategies: 'fpgaLsp.compareStrategies',
    programSram: 'fpgaLsp.programSram',
    programFlash: 'fpgaLsp.programFlash',
    generateSvf: 'fpgaLsp.generateSvf',
    showCables: 'fpgaLsp.showCables',
    openTimingPoint: 'fpgaLsp.openTimingPoint',
    // B-B7：面积热点跳源码（与 openTimingPoint 同一实现，命名更通用）
    openSourceLocation: 'fpgaLsp.openSourceLocation',
    // X-B2：跨 run 的 QoR 趋势面板
    showRunTrend: 'fpgaLsp.showRunTrend',
    // X-B3：引脚约束表格（只读视图 + 跳行）
    showConstraintTable: 'fpgaLsp.showConstraintTable',
    insertInstantiation: 'fpgaLsp.insertInstantiation',
    selectProject: 'fpgaLsp.selectProject',
    tdInfo: 'fpgaLsp.tdInfo',
    highlightDiagnostics: 'fpgaLsp.highlightDiagnostics',
    copyMcpConfig: 'fpgaLsp.copyMcpConfig',
    installMcpConfig: 'fpgaLsp.installMcpConfig',
    copyInstantiation: 'fpgaLsp.copyInstantiation',
    readDesign: 'fpgaLsp.readDesign',
    copySymbolPath: 'fpgaLsp.copySymbolPath',
    addSourceFile: 'fpgaLsp.addSourceFile',
    searchHierarchy: 'fpgaLsp.searchHierarchy',
    duplicateModules: 'fpgaLsp.duplicateModules',
    renameProject: 'fpgaLsp.renameProject',
    updateProject: 'fpgaLsp.updateProject',
    // 0.16.0: on-chip probe (ChipWatcher)
    probeStatus: 'fpgaLsp.probeStatus',
    probeCapture: 'fpgaLsp.probeCapture',
    probeConfig: 'fpgaLsp.probeConfig',
    showProbePanel: 'fpgaLsp.showProbePanel',
});

const VIEWS = Object.freeze({
    modules: 'fpgaLsp.modulesView',
    flow: 'fpgaLsp.flowView',
    reports: 'fpgaLsp.reportsView',
    probe: 'fpgaLsp.probeView',
});

module.exports = { METHODS, COMMANDS, VSCodeCommands, VIEWS, REPORT_SECTIONS };
