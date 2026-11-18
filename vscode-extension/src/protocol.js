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
    insertInstantiation: 'fpgaLsp.insertInstantiation',
    veribleStatus: 'fpgaLsp.veribleStatus',
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
});

const VIEWS = Object.freeze({
    modules: 'fpgaLsp.modulesView',
    flow: 'fpgaLsp.flowView',
    reports: 'fpgaLsp.reportsView',
});

module.exports = { METHODS, COMMANDS, VSCodeCommands, VIEWS };
