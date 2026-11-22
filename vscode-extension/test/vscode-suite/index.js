// Extension-host test suite (runs inside a real VS Code window):
//   code --extensionDevelopmentPath=<ext> --extensionTestsPath=<this dir> <project>
//   code --extensions-dir=<dir> --user-data-dir=<dir> --extensionTestsPath=<this dir> <project>
//
// Verifies the *real* editor path: the language client connects, LSP requests round-trip
// (document symbols / completion / definition), in-memory edits produce diagnostics, and the
// two trees + reports command exist. Writes result.txt next to this file.

const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');

const RESULT = path.join(__dirname, 'result.txt');
const lines = [];
let failures = 0;
function log(message) {
    lines.push(message);
    try { fs.writeFileSync(RESULT, lines.join('\n')); } catch (_) { /* ignore */ }
    console.log(message);
}
function check(name, ok, detail) {
    if (!ok) failures += 1;
    log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
}
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function findLine(text, needle) {
    const rows = text.split('\n');
    for (let i = 0; i < rows.length; i += 1) {
        const column = rows[i].indexOf(needle);
        if (column >= 0) return new vscode.Position(i, column + 1);
    }
    return undefined;
}

exports.run = async function () {
    const started = Date.now();
    let openedAdc;
    try {
        log('extension host: ' + vscode.version);
        log('workspaceFolders: ' + JSON.stringify((vscode.workspace.workspaceFolders || []).map((f) => f.uri.fsPath)));
        const folder = (vscode.workspace.workspaceFolders || [])[0];
        check('a workspace folder is open', !!folder, folder && folder.uri.fsPath);

        const ext = vscode.extensions.getExtension('fpga-lsp.fpga-lsp-anlogic');
        check('extension found', !!ext);
        if (!ext) return;
        log('extensionPath: ' + ext.extensionPath);
        log('version: ' + (ext.packageJSON && ext.packageJSON.version));
        await ext.activate();
        check('extension activates', ext.isActive);

        const commands = await vscode.commands.getCommands(true);
        const ours = commands.filter((c) => c.startsWith('fpgaLsp.') || c.startsWith('fpgalsp.'));
        check('commands registered', ours.length >= 25, `${ours.length} commands`);
        const v3 = ['fpgaLsp.runIncrementalRoute', 'fpgaLsp.runAnalysis', 'fpgaLsp.compareStrategies',
            'fpgaLsp.programSram', 'fpgaLsp.programFlash', 'fpgaLsp.generateSvf', 'fpgaLsp.showCables',
            'fpgaLsp.openTimingPoint', 'fpgaLsp.showActions', 'fpgaLsp.insertInstantiation',
            'fpgaLsp.selectProject', 'fpgaLsp.tdInfo'];
        check('v3 commands registered', v3.every((id) => ours.indexOf(id) >= 0),
            v3.filter((id) => ours.indexOf(id) < 0).join(',') || 'all present');

        // ---- 0.19.x：ChipWatcher 视图/命令（用户明确要求过的结构）----
        const probeCmds = ['fpgaLsp.showProbePanel', 'fpgaLsp.probeToggleSignal',
            'fpgaLsp.probeAddSignals', 'fpgaLsp.refreshProbe', 'fpgaLsp.probeStatus',
            'fpgaLsp.probeCapture'];
        check('probe commands registered', probeCmds.every((id) => ours.indexOf(id) >= 0),
            probeCmds.filter((id) => ours.indexOf(id) < 0).join(',') || 'all present');
        check('duplicate probe command removed (fpgaLsp.probeConfig)',
            ours.indexOf('fpgaLsp.probeConfig') < 0);
        const views = (ext.packageJSON.contributes && ext.packageJSON.contributes.views) || {};
        const fpgaViewIds = (views.fpgaLsp || []).map((v) => v.id);
        check('FPGA sidebar order = flow, reports, probe',
            JSON.stringify(fpgaViewIds) === JSON.stringify(['fpgaLsp.flowView', 'fpgaLsp.reportsView', 'fpgaLsp.probeView']),
            fpgaViewIds.join(','));
        const explorerIds = (views.explorer || []).map((v) => v.id);
        check('Module Hierarchy lives in the explorer sidebar',
            explorerIds.indexOf('fpgaLsp.modulesView') >= 0, explorerIds.join(','));
        check('no bottom-panel probe view remains', !views.panel);
        try {
            await vscode.commands.executeCommand('fpgaLsp.refreshProbe');
            check('probe tree refresh command runs', true);
        } catch (err) {
            check('probe tree refresh command runs', false, (err && err.message) || String(err));
        }
        try {
            await vscode.commands.executeCommand('fpgaLsp.refreshFlow');
            check('flow tree refresh command runs', true);
        } catch (err) {
            check('flow tree refresh command runs', false, (err && err.message) || String(err));
        }

        // ---- 0.19.6：刚开窗口（还没跑过 flow）也必须能拿到 TD GUI 的六步 ----
        try {
            const cp = require('child_process');
            const exe = path.join(ext.extensionPath, 'server', 'bin', 'fpgalsp-native.exe');
            const steps = await new Promise((resolve) => {
                let child;
                try {
                    child = cp.spawn(exe, ['--mcp', '--root', folder.uri.fsPath], { stdio: ['pipe', 'pipe', 'pipe'] });
                } catch (err) {
                    resolve([]);
                    return;
                }
                let buf = '';
                const send = (obj) => { try { child.stdin.write(JSON.stringify(obj) + '\n'); } catch (_) { /* ignore */ } };
                const timer = setTimeout(() => { try { child.kill(); } catch (_) { /* ignore */ } resolve([]); }, 20000);
                child.stdout.on('data', (chunk) => {
                    buf += chunk.toString();
                    let idx;
                    while ((idx = buf.indexOf('\n')) >= 0) {
                        const line = buf.slice(0, idx);
                        buf = buf.slice(idx + 1);
                        let msg;
                        try { msg = JSON.parse(line); } catch (_) { continue; }
                        if (msg.id === 1) {
                            send({ jsonrpc: '2.0', method: 'notifications/initialized' });
                            send({ jsonrpc: '2.0', id: 2, method: 'tools/call', params: { name: 'fpga_flow_status', arguments: {} } });
                        } else if (msg.id === 2) {
                            const content = (msg.result && msg.result.content) || [];
                            let payload = {};
                            try { payload = JSON.parse(content[0] ? content[0].text : '{}'); } catch (_) { payload = {}; }
                            clearTimeout(timer);
                            try { child.kill(); } catch (_) { /* ignore */ }
                            resolve((payload.flow && payload.flow.steps) || []);
                        }
                    }
                });
                child.on('error', () => { clearTimeout(timer); resolve([]); });
                send({ jsonrpc: '2.0', id: 1, method: 'initialize',
                       params: { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'suite', version: '1' } } });
            });
            const names = steps.map((s) => s.name).join(' | ');
            const expected = ['Read Design', 'Optimize RTL', 'Optimize Gate',
                'Optimize Placement', 'Optimize Routing', 'Generate Bitstream'];
            check('flowStatus returns the TD GUI six steps (idle window)',
                steps.length === 6 && expected.every((name) => names.indexOf(name) >= 0),
                names || 'no steps');
        } catch (err) {
            check('flowStatus returns the TD GUI six steps (idle window)', false, (err && err.message) || String(err));
        }

        // wait until the client answers a real request (this is what used to fail:
        // "command 'fpgalsp.check' already exists" aborted the connection during initialize)
        const topPath = path.join(folder.uri.fsPath, 'src', 'top.v');
        const topDoc = await vscode.workspace.openTextDocument(topPath);
        await vscode.window.showTextDocument(topDoc);
        let symbols = [];
        for (let attempt = 0; attempt < 40; attempt += 1) {
            symbols = await vscode.commands.executeCommand('vscode.executeDocumentSymbolProvider', topDoc.uri) || [];
            if (symbols.length) break;
            await sleep(500);
        }
        check('language client connected (documentSymbol round-trip)', symbols.length > 0,
            `${symbols.length} top-level symbols`);
        check('module symbol carries port children', !!(symbols[0] && symbols[0].children && symbols[0].children.length > 10),
            symbols[0] ? `${symbols[0].children ? symbols[0].children.length : 0} children` : 'no symbol');

        // completion on an instantiation line
        const text = topDoc.getText();
        const position = findLine(text, 'pwm_gen #(');
        let completions = [];
        if (position) {
            const result = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', topDoc.uri, position);
            completions = (result && result.items) || [];
        }
        // 冷启动时 HDL 索引可能还没建好，第一次 completion 只回离线候选（实测 <300 条）。
        // 这是测试时序，不是功能缺失：最多重试 ~10 s，直到能看到工程模块清单。
        // 注意位置用 `position.translate(0, -1)`：findLine 返回 column=index+1，而在单词内部
        // 触发补全时服务端会按当前前缀过滤（cursor 在 "pwm_gen" 的 'p' 之后 -> 只剩 p 开头的
        // 模块/原语，`top` 当然不在里面）。这个词首位置才等价于"列模块清单"。
        const modulePosition = position ? position.translate(0, -1) : undefined;
        let labels = new Set();
        for (let attempt = 0; attempt < 20; attempt += 1) {
            const retried = modulePosition
                ? await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', topDoc.uri, modulePosition)
                : null;
            completions = (retried && retried.items) || completions;
            labels = new Set(completions.map((item) => (item.label && item.label.label) || item.label));
            if (labels.has('top') && labels.has('pwm_gen')) break;
            await sleep(500);
        }
        check('completion returns project modules', labels.has('top') && labels.has('pwm_gen'), `${labels.size} labels`);

        // v10: line-leading module names offer a full instantiation snippet for Tab-completion
        let instantiateItem;
        const blankLine = text.split('\n').findIndex((row, idx) => idx > 20 && !row.trim());
        if (blankLine >= 0) {
            const instResult = await vscode.commands.executeCommand(
                'vscode.executeCompletionItemProvider', topDoc.uri, new vscode.Position(blankLine, 0));
            const instItems = (instResult && instResult.items) || [];
            instantiateItem = instItems.find((item) => {
                const label = (item.label && item.label.label) || item.label;
                return typeof label === 'string' && label.endsWith('(instantiate)');
            });
        }
        const instantiateText = instantiateItem
            ? (typeof instantiateItem.insertText === 'string'
                ? instantiateItem.insertText
                : (instantiateItem.insertText && instantiateItem.insertText.value) || '')
            : '';
        check('completion offers instantiation snippets',
            !!instantiateItem && (instantiateItem.insertTextFormat === undefined
                || instantiateItem.insertTextFormat === 2)
            && (!instantiateText || instantiateText.includes('u_')),
            instantiateItem
                ? `${String(instantiateItem.label && instantiateItem.label.label || instantiateItem.label)} format=${instantiateItem.insertTextFormat}`
                : 'none');

        // definition of the pwm_gen module name
        let locations = [];
        if (position) {
            locations = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', topDoc.uri, position) || [];
        }
        check('go-to-definition resolves the module file',
            locations.length > 0 && /pwm_gen\.v$/.test(locations[0].uri.fsPath),
            locations.length ? locations[0].uri.fsPath.replace(folder.uri.fsPath, '<ws>') : 'none');

        // hover and go-to-definition must agree for a port-connection token inside an instance
        const refLine = text.split('\n').findIndex((row) => row.includes('.refclk') && !row.includes('pll_gen'));
        if (refLine >= 0) {
            const refRow = text.split('\n')[refLine];
            const refColumn = refRow.indexOf('refclk') + 1;
            const refPosition = new vscode.Position(refLine, refColumn);
            const refHovers = await vscode.commands.executeCommand(
                'vscode.executeHoverProvider', topDoc.uri, refPosition) || [];
            const refLocations = await vscode.commands.executeCommand(
                'vscode.executeDefinitionProvider', topDoc.uri, refPosition) || [];
            const hoverText = refHovers.map((hover) => (hover.contents || [])
                .map((content) => (typeof content === 'string' ? content : content.value) || '').join('\n')).join('\n');
            const hoverClaimsDefinition = /Definition:|defined at/i.test(hoverText);
            check('hover/definition agree on an instance port connection',
                refLocations.length > 0
                || (refLocations.length === 0 && !hoverClaimsDefinition),
                `hover=${hoverText.slice(0, 40).replace(/\n/g, ' ')} def=${refLocations.length}`);
        }

        // v4: find references + quick-instantiate code action on the pwm_gen module name
        if (position) {
            const instLine = position.line;
            const instCol = position.character + 1;
            const range = new vscode.Range(instLine, instCol, instLine, instCol + 7);
            const refs = await vscode.commands.executeCommand(
                'vscode.executeReferenceProvider', topDoc.uri, new vscode.Position(instLine, instCol)) || [];
            check('find references on a module', refs.length >= 2, `${refs.length} references`);

        // v7: language associations + bundled grammars
        const languageIds = await vscode.languages.getLanguages();
        check('verilog/systemverilog/vhdl languages registered',
            ['verilog', 'systemverilog', 'vhdl'].every((id) => languageIds.includes(id)),
            languageIds.filter((id) => /veri|vhdl/i.test(id)).join(','));
        const grammars = (ext.packageJSON.contributes && ext.packageJSON.contributes.grammars) || [];
        check('bundled grammars present', grammars.length === 3,
            grammars.map((grammar) => path.basename(grammar.path)).join(','));
        const snippets = (ext.packageJSON.contributes && ext.packageJSON.contributes.snippets) || [];
        check('HDL snippet contributions present', snippets.length === 5,
            snippets.map((entry) => entry.language).join(','));
        const tokenScopes = (ext.packageJSON.contributes && ext.packageJSON.contributes.semanticTokenScopes) || [];
        const verilogScopes = (tokenScopes.find((entry) => entry.language === 'verilog') || {}).scopes || {};
        check('semantic token scopes map symbol kinds to distinct themes',
            tokenScopes.length >= 2 && !!verilogScopes.property && !!verilogScopes.parameter
            && !!verilogScopes.variable && !!verilogScopes['class'] && !!verilogScopes.function,
            Object.keys(verilogScopes).join(','));
        check('top.v languageId is verilog', topDoc.languageId === 'verilog', topDoc.languageId);
        const langDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fpga-lang-'));
        const vhdPath = path.join(langDir, 'demo.vhd');
        fs.writeFileSync(vhdPath, 'entity demo is\nend entity;\n');
        const svPath = path.join(langDir, 'demo.sv');
        fs.writeFileSync(svPath, 'module demo_sv;\nendmodule\n');
        const vhdDoc = await vscode.workspace.openTextDocument(vscode.Uri.file(vhdPath));
        const svDoc = await vscode.workspace.openTextDocument(vscode.Uri.file(svPath));
        check('.vhd languageId is vhdl', vhdDoc.languageId === 'vhdl', vhdDoc.languageId);
        check('.sv languageId is systemverilog', svDoc.languageId === 'systemverilog', svDoc.languageId);

        // highlight diagnostics: language + real semantic token counts from the server
        try {
            await vscode.commands.executeCommand('fpgaLsp.highlightDiagnostics');
            await sleep(1200);
            const stateDirModule = require(path.join(ext.extensionPath, 'src', 'stateDir'));
            const stateDir = stateDirModule.resolveStateDir(folder.uri.fsPath);
            const reportPath = path.join(stateDir, 'logs', 'highlight.txt');
            const report = fs.existsSync(reportPath) ? fs.readFileSync(reportPath, 'utf8') : '';
            const tokenMatch = /semantic tokens\s*:\s*(\d+)/.exec(report);
            check('highlight diagnostics report written', report.length > 0, reportPath);
            check('report sees verilog language', /languageId\s*:\s*verilog/.test(report),
                (report.match(/languageId\s*:\s*(.+)/) || [])[1] || '');
            check('server returns semantic tokens for the project file',
                !!tokenMatch && Number(tokenMatch[1]) > 20, tokenMatch ? tokenMatch[1] : '0');
        } catch (err) {
            check('highlight diagnostics report written', false, (err && err.message) || String(err));
        }

            const actions = await vscode.commands.executeCommand(
                'vscode.executeCodeActionProvider', topDoc.uri, range) || [];
            log('  code actions raw: ' + JSON.stringify(actions).slice(0, 600));
            const titles = actions.map((action) => action.title || (action.command && action.command.title));
            check('quick-instantiate code action available',
                titles.some((title) => /Insert instantiation/i.test(title || '')), titles.join(' | ').slice(0, 80));
        }

        // v5: rename / folding / inlay hints / range formatting / call hierarchy
        if (position) {
            const at = new vscode.Position(position.line, position.character + 1);
            try {
                const renameEdit = await vscode.commands.executeCommand(
                    'vscode.executeDocumentRenameProvider', topDoc.uri, at, 'pwm_gen2');
                const size = renameEdit && (renameEdit.size === undefined ? Object.keys(renameEdit).length : renameEdit.size);
                check('rename provider returns edits', !!renameEdit && size > 0, `size=${size}`);
            } catch (err) {
                check('rename provider returns edits', false, (err && err.message) || String(err));
            }
            try {
                const folds = await vscode.commands.executeCommand('vscode.executeFoldingRangeProvider', topDoc.uri) || [];
                const modLine = findLine(text, 'module top');
                check('folding ranges include the module', folds.length > 0
                    && folds.some((range) => range.start === (modLine ? modLine.line : -1)),
                    `${folds.length} ranges`);
            } catch (err) {
                check('folding ranges include the module', false, (err && err.message) || String(err));
            }
            try {
                const hints = await vscode.commands.executeCommand('vscode.executeInlayHintProvider',
                    topDoc.uri, new vscode.Range(0, 0, 4000, 0)) || [];
                const labels = hints.map((hint) => (typeof hint.label === 'string' ? hint.label : ''));
                check('inlay hints annotate instances', labels.some((label) => label.indexOf(': pwm_gen') === 0),
                    `${hints.length} hints`);
            } catch (err) {
                check('inlay hints annotate instances', false, (err && err.message) || String(err));
            }
            try {
                const edits = await vscode.commands.executeCommand('vscode.executeFormatRangeProvider',
                    topDoc.uri, new vscode.Range(position.line, 0, position.line + 3, 0),
                    { tabSize: 4, insertSpaces: true });
                check('range formatting works (Verible or built-in fallback)', Array.isArray(edits),
                    `${(edits || []).length} edits`);
            } catch (err) {
                check('range formatting works (Verible or built-in fallback)', false, (err && err.message) || String(err));
            }
            try {
                const items = await vscode.commands.executeCommand(
                    'vscode.prepareCallHierarchy', topDoc.uri, at) || [];
                const calls = items.length
                    ? await vscode.commands.executeCommand('vscode.provideIncomingCalls', items[0]) : [];
                const parents = calls.map((call) => call.from && call.from.name);
                check('call hierarchy incoming calls', parents.indexOf('top') >= 0, parents.join(','));
            } catch (err) {
                check('call hierarchy incoming calls', false, (err && err.message) || String(err));
            }
        }

        // in-memory ADC edit -> diagnostics must appear without saving the file
        const adcPath = path.join(folder.uri.fsPath, 'pin.adc');
        if (fs.existsSync(adcPath)) {
            openedAdc = await vscode.workspace.openTextDocument(adcPath);
            await vscode.window.showTextDocument(openedAdc);
            await sleep(1500);
            const before = vscode.languages.getDiagnostics(openedAdc.uri);
            log(`  pin.adc diagnostics before edit: ${before.length}`);
            const edit = new vscode.WorkspaceEdit();
            const adcText = openedAdc.getText();
            const target = findLine(adcText, 'w[1]');
            if (target) {
                const row = adcText.split('\n')[target.line];
                const column = row.indexOf('P77');
                if (column >= 0) {
                    edit.replace(openedAdc.uri, new vscode.Range(target.line, column, target.line, column + 3), 'P76');
                    await vscode.workspace.applyEdit(edit);
                    await sleep(2500);
                    const after = vscode.languages.getDiagnostics(openedAdc.uri);
                    check('in-memory ADC edit produces a diagnostic',
                        after.length > before.length,
                        `${before.length} -> ${after.length}: ${after.map((d) => d.code).join(',')}`);
                }
            }
        }

        // v9: separate synthesis / place&route report pages
        try {
            await vscode.commands.executeCommand('fpgaLsp.showSynReports');
            await vscode.commands.executeCommand('fpgaLsp.showPhyReports');
            check('separate syn/phy report pages open', true, '');
        } catch (err) {
            check('separate syn/phy report pages open', false, (err && err.message) || String(err));
        }

        // v8: copy instantiation + hierarchical symbol path (editor buttons / context menu)
        try {
            await vscode.window.showTextDocument(topDoc);
            await vscode.commands.executeCommand('fpgaLsp.copyInstantiation');
            await sleep(400);
            const clipboard = await vscode.env.clipboard.readText();
            check('copy instantiation puts a template on the clipboard',
                clipboard.includes('top') && clipboard.includes('('), clipboard.slice(0, 60));
        } catch (err) {
            check('copy instantiation puts a template on the clipboard', false, (err && err.message) || String(err));
        }
        try {
            const text = topDoc.getText();
            const wireLine = text.split('\n').findIndex((row) => row.includes('oc_chk_lock') && row.includes('wire'));
            if (wireLine >= 0) {
                const column = text.split('\n')[wireLine].indexOf('oc_chk_lock');
                const targetEditor = await vscode.window.showTextDocument(topDoc);
                targetEditor.selection = new vscode.Selection(wireLine, column + 1, wireLine, column + 1);
                await vscode.commands.executeCommand('fpgaLsp.copySymbolPath');
                await sleep(400);
                const symbolClip = await vscode.env.clipboard.readText();
                // 命令标题是 Copy Hierarchical Symbol Path：完整层次路径（top/oc_chk_lock）才是预期；oc_chk_lock 是旧断言。
                check('copy symbol path uses the hierarchy', symbolClip === 'top/oc_chk_lock', symbolClip);
            }
        } catch (err) {
            check('copy symbol path uses the hierarchy', false, (err && err.message) || String(err));
        }

        // v3: cable/device preflight command (read-only, never programs)
        try {
            await vscode.commands.executeCommand('fpgaLsp.showCables');
            await sleep(1500);
            const docs = vscode.workspace.textDocuments.map((d) => d.getText());
            check('Show Cable / Device produces a report', docs.some((text) => text.includes('Project target')),
                docs.find((text) => text.includes('Project target')) ? 'ok' : 'document not found');
        } catch (err) {
            check('Show Cable / Device produces a report', false, (err && err.message) || String(err));
        }

        // formatting availability: inside the workspace vs outside any workspace folder
        try {
            const ugly = 'module fmt_check;\nwire a;\nalways @(*) begin\nif (a) a = 1;\nend\nendmodule\n';
            const insidePath = path.join(folder.uri.fsPath, '.fpgalsp', 'fmt_check_inside.v');
            const outsidePath = path.join(os.tmpdir(), 'fpga_fmt_outside.v');
            fs.mkdirSync(path.dirname(insidePath), { recursive: true });
            fs.writeFileSync(insidePath, ugly);
            fs.writeFileSync(outsidePath, ugly);
            const insideDoc = await vscode.workspace.openTextDocument(insidePath);
            const outsideDoc = await vscode.workspace.openTextDocument(outsidePath);
            await vscode.window.showTextDocument(insideDoc);
            const fmtOptions = { tabSize: 4, insertSpaces: true };
            const insideEdits = await vscode.commands.executeCommand(
                'vscode.executeFormatDocumentProvider', insideDoc.uri, fmtOptions) || [];
            await vscode.window.showTextDocument(outsideDoc);
            const outsideEdits = await vscode.commands.executeCommand(
                'vscode.executeFormatDocumentProvider', outsideDoc.uri, fmtOptions) || [];
            check('format document works inside the workspace',
                insideEdits.length > 0, `lang=${insideDoc.languageId} edits=${insideEdits.length}`);
            check('format document works outside the workspace',
                outsideEdits.length > 0, `lang=${outsideDoc.languageId} edits=${outsideEdits.length}`);
            try { fs.unlinkSync(insidePath); } catch (_) { /* ignore */ }
            try { fs.unlinkSync(outsidePath); } catch (_) { /* ignore */ }
        } catch (err) {
            check('format document works inside/outside the workspace', false, (err && err.message) || String(err));
        }

        // MCP: the extension should register its definition provider on VS Code versions that support it.
        try {
            const extLogPath = path.join(
            require(path.join(ext.extensionPath, 'src', 'stateDir')).resolveStateDir(folder.uri.fsPath),
            'logs', 'extension.log');
            const extLog = fs.existsSync(extLogPath) ? fs.readFileSync(extLogPath, 'utf8') : '';
            const api = !!(vscode.lm && typeof vscode.lm.registerMcpServerDefinitionProvider === 'function');
            const dynamicDefault = ext.packageJSON
                && ext.packageJSON.contributes
                && ext.packageJSON.contributes.configuration
                && ext.packageJSON.contributes.configuration.properties
                && ext.packageJSON.contributes.configuration.properties['fpgaLsp.mcp.dynamicProvider']
                && ext.packageJSON.contributes.configuration.properties['fpgaLsp.mcp.dynamicProvider'].default;
            if (api && dynamicDefault === true) {
                check('MCP server definition provider registered',
                    extLog.includes('MCP server definition provider registered'), 'extension.log');
            } else if (api) {
                check('MCP dynamic provider disabled by default',
                    extLog.includes('MCP dynamic provider disabled'), 'workspace config required');
            }
            if (api) {
                let constructorOk = false;
                try {
                    if (typeof vscode.McpStdioServerDefinition === 'function') {
                        const definition = new vscode.McpStdioServerDefinition({
                            label: 'test', command: 'python', args: ['-m', 'fpgalsp.mcp'],
                            cwd: folder.uri, env: {}, version: 'test',
                        });
                        constructorOk = !!definition;
                    }
                } catch (err) {
                    constructorOk = false;
                }
                check('MCP stdio server definition constructor accepts our options', constructorOk, '');
            } else {
                check('MCP server definition provider registered', true, 'API unavailable (skipped)');
                check('MCP stdio server definition constructor accepts our options', true, 'API unavailable (skipped)');
            }
        } catch (err) {
            check('MCP server definition provider registered', false, (err && err.message) || String(err));
        }

        // doctor: one command must report LSP/Verible/TD/MCP status.
        try {
            await vscode.commands.executeCommand('fpgaLsp.doctor');
            await sleep(800);
            const doctorDoc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
            const doctorText = doctorDoc ? doctorDoc.getText() : '';
            check('doctor command opens a status report',
                !!doctorDoc && doctorDoc.languageId === 'markdown'
                && doctorText.includes('FPGA LSP Doctor')
                && doctorText.includes('## Language server')
                && doctorText.includes('## Verible')
                && doctorText.includes('## TangDynasty')
                && doctorText.includes('## MCP'),
                doctorText.split('\n')[0] || 'no document');
            if (doctorDoc) await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
        } catch (err) {
            check('doctor command opens a status report', false, (err && err.message) || String(err));
        }

        // reports command is registered and runs (no flow run yet -> friendly empty state)
        try {
            await vscode.commands.executeCommand('fpgaLsp.showReports');
            check('reports panel command works', true, '');
        } catch (err) {
            check('reports panel command works', false, (err && err.message) || String(err));
        }

        // ---- 0.21.4 回归：Flow 六步右键 "Reset and Run This Flow Step" ----
        // 用户反馈过的症状：点了之后 FPGA Flow 没有任何输出、下游两步还是"已完成"。
        // 这里用真实 extension host + 真实后端 + 真实 TD 检查四件事：
        //   1) 命令返回的是"flow 跑完"的结果（说明 controller 一直在跟踪，而不是发完就返回）；
        //   2) FlowController 的输出通道收到 "▶ reset run ..." 与每步进度行；
        //   3) syn 三步的 .end.f 标记全部被重写（reset run 语义 = 整条 run 重跑）；
        //   4) 结束后 controller 回到空闲，状态栏显示 done。
        try {
            const api = await ext.activate();
            const controller = api && api.flowController;
            check('flow controller exposed for tests', !!controller);
            const outputLines = [];
            if (controller && controller.output) {
                const original = controller.output.appendLine.bind(controller.output);
                controller.output.appendLine = (text) => {
                    outputLines.push(String(text));
                    original(text);
                };
            }
            const projectRoot = folder && folder.uri.fsPath;
            let synRun = '';
            if (projectRoot) {
                for (const name of fs.readdirSync(projectRoot)) {
                    if (!name.endsWith('_Runs')) continue;
                    const candidate = path.join(projectRoot, name, 'syn_1');
                    if (fs.existsSync(candidate)) { synRun = candidate; break; }
                }
            }
            check('syn run directory found', !!synRun, synRun);
            const markerTime = (name) => {
                try { return fs.statSync(path.join(synRun, name)).mtimeMs; } catch (_) { return 0; }
            };
            const before = {
                read_design: markerTime('.read_design.end.f'),
                opt_rtl: markerTime('.opt_rtl.end.f'),
                opt_gate: markerTime('.opt_gate.end.f'),
            };
            // 真实的 view/item/context 参数是**树节点**（getChildren 的 element），不是 TreeItem：
            // 只认 TreeItem 形状时右键永远拿到空 step（用户 2026-11-22 实测）。
            const item = { kind: 'stage', step: 'read_design', stepName: 'Read Design', label: 'Read Design · 1s' };
            const startedAt = Date.now();
            const result = await vscode.commands.executeCommand('fpgaLsp.flowStepReset', item);
            const elapsed = ((Date.now() - startedAt) / 1000).toFixed(1);
            check('Reset and Run resolves after the flow finished', !!(result && result.ok === true),
                JSON.stringify(result).slice(0, 160));
            const after = {
                read_design: markerTime('.read_design.end.f'),
                opt_rtl: markerTime('.opt_rtl.end.f'),
                opt_gate: markerTime('.opt_gate.end.f'),
            };
            check('reset and run re-ran ONLY the selected step',
                after.read_design > before.read_design,
                `${elapsed}s  read_design ${before.read_design} -> ${after.read_design}`);
            check('downstream steps are cleared to "not run" (no re-run)',
                after.opt_rtl === 0 && after.opt_gate === 0,
                `opt_rtl=${after.opt_rtl} opt_gate=${after.opt_gate}`);
            check('FPGA Flow output shows the step run header',
                outputLines.some((line) => /reset run Read Design/.test(line)),
                outputLines.filter((line) => /reset run|Read Design/.test(line)).slice(0, 2).join(' | '));
            check('FPGA Flow output names the running extension version',
                outputLines.some((line) => /fpga-lsp \d+\.\d+\.\d+/.test(line)),
                outputLines.find((line) => /fpga-lsp \d+\.\d+\.\d+/.test(line)) || 'no version tag');
            check('FPGA Flow output shows the step run progress',
                outputLines.some((line) => /run step read_design|Read Design finished|reset step/.test(line)),
                outputLines.slice(-3).join(' | '));
            check('flow controller returns to idle after the run',
                !!controller && controller.active === undefined, '');
            check('status bar reports done/idle',
                !!controller && /done|idle/.test(String((controller.statusBar && controller.statusBar.text) || '')),
                String((controller && controller.statusBar && controller.statusBar.text) || ''));
        } catch (err) {
            check('reset and run flow step works', false, (err && err.message) || String(err));
        }
    } catch (err) {
        check('suite completed without fatal error', false, (err && err.stack ? err.stack.split('\n')[0] : String(err)));
        log('FATAL: ' + (err && err.stack ? err.stack : String(err)));
    } finally {
        log(`elapsed ms: ${Date.now() - started}`);
        log(failures === 0 ? 'SUITE: PASS' : `SUITE: FAIL (${failures})`);
        try { fs.writeFileSync(RESULT, lines.join('\n')); } catch (_) { /* ignore */ }
    }
};
