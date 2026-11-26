# `src/server/` 命令与方法实现状态

> 权威协议：`%PYREF%\PROTOCOL-V2.md`
> 参考实现：`%PYREF%\fpgalsp\server.py`
> 本文件原为 **task-14 验收项**（"已实现 / 占位两栏"）。2026-11-18 Phase B 后
> **不再有占位命令/空实现**；本文件保留为"方法与命令的落点索引"，逐条证据见
> `doc/audit/2026-11-18/parity-matrix.md` 与 `doc/audit/2026-11-18/FIX-REPORT.md`。

## 铁律：占位必须显式返回

任何**没有实现**的 `executeCommand` 命令都返回

```json
{"ok": false, "error": "not implemented"}
```

**不能静默不返回**——LSP 客户端会一直等这条响应，表现为"命令卡住不动"。
未知**方法**（method 层面）则由 dispatcher 统一回 `-32601`，同样不会挂。

---

## 一、`workspace/executeCommand` —— 34 条，全部已实现（无占位）

| command | 返回 | 说明 / 依赖 |
| --- | --- | --- |
| `fpgalsp.info` | `{ok, project, dir, al, family, device, top, td_version, files{hdl,adc,sdc,ip}, warnings[], device_loaded, pins, td_session, td, projects[]}` | 工程摘要。`device_loaded` / `td_session` 是**实时状态位**（非确定性，见 PYTHON-BUGS **P-3**，已按契约复现） |
| `fpgalsp.projects` | `{ok, root, count, projects[{al,dir,name,family,device,top,hdl,warnings}], selected}` | `project::DiscoverProjects`（深度 6） |
| `fpgalsp.setProject` | `{ok, selected[, summary]}` | `arguments[0].al` 为空串 = 恢复自动发现；非 `.al` 或不存在 → `{ok:false,error}` |
| `fpgalsp.tdInfo` | `{ok, td:{ok,root,version,source,alternatives[]}}` | `td::FindTd`（explicit > env > 自动最新 > PATH） |
| `fpgalsp.device` | `{ok, device, db, pins, package}` | import_device + `query_current_package -fio/-clock_io/-lvds/-cfg_io` 的真实解析 |
| `fpgalsp.check` | `{ok, project, queued:true}` | 触发一次**异步**权威 TD 诊断；结果随后以 `textDocument/publishDiagnostics` 推送 |
| `fpgalsp.restart` | `{ok:true}` | 杀掉当前 TD 会话（下一次检查会重建） |
| `fpgalsp.runFlow` | `{ok:true, runId, stages, job}` / `{ok:false, error:"busy"}` | 真实 flow 引擎（`src/flow`），异步；`jobs` = TD multirun 并发 run 数（1~6，PRJ-8802），不是线程数 |
| `fpgalsp.runFlowStep` | `{ok, runId, step, force, singleStep, job}` / `{ok:true, skipped:true, reason}` | **单步**（GUI 的 6 步：`read_design/opt_rtl/opt_gate/opt_place/opt_route/bitgen`）。`force:false` = Run（该步已完成则跳过）；`force:true` = Reset and Run（**先清该步及下游**的标记与 `flow.status` 行，再 `launch_runs <run> -step <step>`，只跑这一步）。单步的 `-jobs` 强制为 1（PRJ-8803） |
| `fpgalsp.cancelFlow` | `{ok:true}` / `{ok:true, note:"not running"}` | 写 `.stop.f`，超过宽限期再杀 TD 进程树 |
| `fpgalsp.metrics` | `{ok, index:{completed, maxRunMs, files, generation}}` | 索引服务统计（观察索引是否真的在增量重建） |
| `fpgalsp.probeClocks` | `{ok, clocks[{name,...}], current?, source, reason?}` | ChipWatcher 采样时钟候选：来自 `phy_1/{route,place}_clock_utilization.txt`；还没跑过布局布线时 `ok:true` + 空列表 + `reason` |
| `fpgalsp.compareStrategies` | `{ok, runs[], best, elapsed, work}` | `src/flow/strategy.cpp` |
| `fpgalsp.cables` | `{ok, inspect{...}}` | 下载器探测 |
| `fpgalsp.program` | `{ok, refused?, stage?, device_id, log}` | **三条硬规则在 `flow::Program()` 内部**：`confirm:true` 必须显式传 / 无下载器直接拒 / 器件不匹配即使 `allowMismatch:true` 也拒 |
| `fpgalsp.svfgen` | `{ok, svf, size, command}` | 生成 SVF |
| `fpgalsp.probeStatus` | `{ok, ready, chip_debugger:{exists,count,signals[]}, watcher:{present,instance,clock,sample_depth,buses[]}}` | 纯读文件（`chip_debugger/user.mdn` + `phy_1/cw/compiled.cwc`），毫秒级、不碰硬件 |
| `fpgalsp.probeConfig` | `{ok, written, count, before[], after[], added[], removed[]}` | 读写 `chip_debugger/user.mdn` 的 `mark_debug`；**只有 `write:true` 才落盘**，空清单拒绝写 |
| `fpgalsp.probeCapture` | `{ok, samples, clock, signals{bus:[值…]}, summary, csv}` | 片上采集：起独立 TD 会话，`config_chipwatcher → download -mode jtag_burst → run_hw_cw → wait_on_hw_cw → write_hw_cw_data`。`download:true`（默认）必须带 `confirm:true` |

### 其余命令的模块归属

> 2026-11-18 Phase B 后：下表 11 条命令全部走 `LspServer::Feature*`（LSP 与 MCP 同一实现），
> 不再有 `{"ok":false,"error":"not implemented"}`。逐条验收见 `doc/audit/2026-11-18/parity-matrix.md` 的 CMD-01..CMD-10。

| command | 归属模块 | 说明 |
| --- | --- | --- |
| `fpgalsp.reports` | reports | 与 `fpgaLsp/reports` 同一实现 |
| `fpgalsp.runAnalysis` | analysis | DRC / CDC（无布线产物时 `ok:false` + 可读 error） |
| `fpgalsp.addSourceFile` | ide | 写 `.al` 的 `<Source_Files>`（原子写 + 回滚） |
| `fpgalsp.setProjectName` | ide | 改 `.al` 的 `<Name>`（原子写 + 回滚） |
| `fpgalsp.updateProject` | ide + TD | `check_project_version` + `open_project -update`（TD 会话 cwd = `.al` 所在目录；`ok` 只由 `<CODE> ERROR` 判定，`CRITICAL-WARNING` 不算失败；`.al` 记录的 `<Project Path>` 与实际目录不一致时额外返回 `warning`） |
| `fpgalsp.instantiate` | hdl / ide | 例化文本生成（`BuildInstantiationText`） |
| `fpgalsp.copyInstantiation` | ide | 当前文件第一个 module 的例化 |
| `fpgalsp.symbolPath` | ide | 光标处层次路径 |
| `fpgalsp.duplicateModules` | ide | 跨构建文件重名 module |
| `fpgalsp.searchHierarchy` | ide | 模块名/例化名搜索 |
| `fpgalsp.debugHighlight` | semantics | 语义 token 统计（与 `FeatureSemanticTokens` 同源） |

未在以上两栏里的命令 → `{ok:false, error:"unknown command <name>"}`（与 Python 一致）。

---

## 二、`fpgalsp.*` 命令的形态补充（仍是 executeCommand，不是 `fpgaLsp/*`）

| 命令 | 状态 | 说明 |
| --- | --- | --- |
| `fpgalsp.compareRuns` | **已实现（B-B6）** | `{from?,to?}` → 两次 run 的 QoR/时序/面积差异（方向 + 汇总） |
| `fpgalsp.runs` | **已实现（X-B2）** | run 列表 + 指标（趋势面板数据源） |
| `fpgalsp.testbench` | **已实现（B-B10）** | `{module?,clock?,reset?}` → 可解析的 testbench 文本 + `parseOk` |
| `fpgalsp.constraintTable` | **已实现（X-B3）** | `.adc` 逐行结构化 + 每行诊断数（只读） |

## 三、自定义方法 `fpgaLsp/*` —— 4 个请求 + 3 个服务端推送

| 方法 | 方向 | 说明 |
| --- | --- | --- |
| `workspace/willRenameFiles` | 客户端→服务端 | **已实现（B-A3）**：改名 HDL 文件时返回 `.al` 的精确 `WorkspaceEdit`；`workspace/didRenameFiles` 重解析工程并重发诊断（这两个是标准 LSP 方法，不是 `fpgaLsp/*`，放这里便于对照） |
| `fpgaLsp/moduleChildren` | 请求 | **已实现（P-C2）**：`{uri?, module, offset, limit, includePrimitives}` → `{ok,parent,children[],total,nextOffset,hasMore}`；只回一层，子节点带 `hasChildren` 不带 `children`；未知模块 `ok:false` |
| `fpgaLsp/moduleTree` | 请求 | **已实现（真实索引）**：基于 `src/hdl` 的 Verible 符号索引；`includePrimitives=false` 时按 745 条器件原语过滤黑盒 |
| `fpgaLsp/reports` | 请求 | **已实现**：`reports::BuildReports` + `routed_db` + `strategies.available` + `flow`；无 flow 时 `ok:false` + Python 同文案 |
| `fpgaLsp/flowStatus` | 请求 | **已实现**：`{ok, flow{stage,running,steps[6],jobs,log}, artifacts{runs[]}}`（GUI 的 6 步 + 暂停/失败状态） |
| `fpgaLsp/flowEvent` | 通知（服务端→客户端） | **已实现**：`LspServer::SendFlowEvent(event)` → `SendNotification`；调用点是 `FlowRunner` 的事件回调，由后台 worker 线程推送 |
| `fpgaLsp/indexChanged` | 通知（服务端→客户端） | **已实现**：索引重建后推送（带 `modules[]` 补丁，模块树原地更新，不再全量重取） |
| `fpgaLsp/reportsChanged` | 通知（服务端→客户端） | **已实现**：报告失效后推送 `{stage, sections, revision}`（不阻塞 IO 线程；客户端按需重取或走 `ifRevision`） |

### `fpgaLsp/moduleTree`（阶段 3 起用真实 HDL 索引）

数据来源：`src/hdl`（Verible 词法 + `hdl.py` 的结构识别算法）。
语义与 `server.py::on_fpgaLsp_moduleTree` 逐条对齐：

* `range` = 模块定义处；`usedAt` = 例化处（根节点 `null`）
* `defined=false` → `blackbox=true`，`file`/`range` 指向**例化**位置，`portCount=0`
* 环路保护：`recursive=true` + `children=[]`（同名在栈上，或 `depth >= 24`）
* 节点预算：全局 5000 → `truncated=true`；单节点子项上限 500 → 该节点 `truncated=true`
* `modulesSeen` = 索引里的模块定义数
* **IP 生成的 Verilog 与 VHDL entity 都进索引** → 它们不再是黑盒
  （实测 `corpus_alpha`：`pll_gen` 来自 `al_ip/pll_gen.v`，`defined=true`、`portCount=4`）

与 Python 版的**有意差异**（都只会更准，见 `tests/PYTHON-BUGS.md` P-9 附注）：

1. `ifdef` 只取激活分支（Python 两个分支都解析，同名模块可能被统计两次）
2. 宏调用在 filtered token 流里已展开
3. 非 ASCII 标识符按规范拒绝（Python 接受）

### `src/hdl` 索引 vs `hdl.py` 的逐字段对比结果

`tests/e2e/compare_hdl_python.py`（手工验证工具，不进 ctest）：

```
python tests/e2e/compare_hdl_python.py \
    --unit build/bin/unit_hdl_index.exe \
    --corpus tests/gate-corpus/corpus_alpha \
    --python-root Y:/fpga-lsp
```

实测（`corpus_alpha` 的 20 个 `.v` + `al_ip/pll_gen.v`）：

```
一致项: 1214
差异项: 0
✅ 与 Python 逐字段完全一致
```

对比字段：module 的 `name/line/character/end_line/ansi_ports/parameters`、
port 的 `name/direction/msb/lsb/width/line/character`、
instance 的 `module/name/line/character`。

---

## 三、标准 LSP 方法

### 已实现（真语义）

| 方法 | 说明 |
| --- | --- |
| `initialize` / `initialized` | 能力声明见下节；`initializationOptions.fpgaLsp` 生效 |
| `textDocument/didOpen` | 存入 `DocumentStore`（未保存内容优先），重发该文件诊断 |
| `textDocument/didChange` | **增量**同步（UTF-16 列，`DocumentStore`） |
| `textDocument/didSave` | 重发诊断；若 `tdCheckOnSave` 且 `enableTd`：**立即清空该文件 TD 诊断** → **只排队一次**重跑（复现 0.10.28） |
| `textDocument/didClose` | 关闭缓冲并清空该文件诊断 |
| `workspace/didChangeConfiguration` | 重读配置；`projectFile` 变化时清缓存 |
| `shutdown` / `exit` | 由 `main.cpp` 的 serve 循环处理；`exit` 前后关 TD 会话 |
| `textDocument/documentSymbol` | 模块(kind 2) + 端口(8) + 参数(14) + 信号(13)；模块 range 用 `module` 关键字→`endmodule` |
| `workspace/symbol` | 模块/端口/信号/参数，子符号带 `containerName`；**实例名不在此列**（那是 callHierarchy/searchHierarchy） |
| `textDocument/definition` | 实例名→模块定义；模块参考→定义；`.port(sig)` 的端口名→目标模块端口；端口/信号/参数→声明处 |
| `textDocument/hover` | markdown：`**名字** — 种类` + 类型/位宽 + 所属模块 + `文件:行:列` |
| `textDocument/completion` | 模块名(kind 9) + **行首例化模板 snippet**(kind 15, `insertTextFormat=2`, `${1:...}` tab stop, 实例名 `u_<module>` 自动避让)**
| `textDocument/references` | **基于 token** 的跨工程同名标识符搜索（注释/字符串里的同名文本天然不命中），支持 `includeDeclaration` |
| `textDocument/inlayHint` | 例化名后 `: 模块名`（`InlayHintKind.Type = 1`） |

### 仍返回空结果（占位，不报错）

> 2026-11-18 Phase B 后**已无空实现**：下表全部变为真实现，逐条见 `parity-matrix.md`
> LSP-01..LSP-09。保留本节只是为了让旧读者知道这里曾经是空的。

| 方法 | Phase B 后的行为 |
| --- | --- |
| `textDocument/codeAction` | 实例化 refactor + 内嵌 Verible lint autofix + `.adc` 引脚 quickfix |
| `textDocument/formatting` | 内嵌 Verible 全文档格式化（与 `fpga_format` 同一桥） |
| `textDocument/rangeFormatting` | 内嵌 Verible，区间编辑 |
| `textDocument/prepareRename` / `rename` | 符号级 range + 全工程 token 级编辑 |
| `textDocument/prepareCallHierarchy` / `callHierarchy/*` | 真实层次调用（in/out） |
| `textDocument/semanticTokens/full` | Verible 原始词法流 → 语义 token |
| `textDocument/foldingRange` | 关键字/括号折叠区间 |

> 备注：`formatting` 按 v5 协议**始终声明**（内嵌 Verible 负责格式化）。
> Verible 版本基线 = `thirdparty/verible` 的 `c6b29d62`；与 Python 侧外部
> `verible 0.0-4296` 的差异只在 LSP-18（formatting 字节）一族，属于版本基线而非能力缺失。

---

## 三之二、`LspServer` 的 Feature API（**MCP 工具等非 LSP 通道请用这一层**）

| 方法 | 说明 |
| --- | --- |
| `Json FeatureInitialize()` / `FeatureInitialize(params)` | 能力声明；带 params 的版本等价 LSP `initialize` |
| `Json FeatureInfo()` | 与 `fpgalsp.info` **同一实现** |
| `Json FeatureModuleTree(uri, include_primitives=false)` | 与 `fpgaLsp/moduleTree` 同一实现；`uri` 空 = 当前工程 |
| `Json FeatureDocumentSymbols(path)` | 同 `textDocument/documentSymbol` |
| `Json FeatureWorkspaceSymbols(query)` | 同 `workspace/symbol` |
| `Json FeatureDefinition(path, line, ch)` | 同 `textDocument/definition` |
| `Json FeatureHover(path, line, ch)` | 同 `textDocument/hover` |
| `Json FeatureReferences(path, line, ch, include_decl)` | 同 `textDocument/references` |
| `Json FeatureInlayHints(path)` | 同 `textDocument/inlayHint` |
| `Json FeatureCompletion(path, line, ch)` | 同 `textDocument/completion`（自动用缓冲区） |

**分层（重要）**：

```
src/hdl          词法/结构解析 + 索引（不认识 LSP，不产出协议 JSON）
src/server/features.{h,cpp}   index -> LSP JSON 的**唯一实现**（无状态、不碰传输）
src/server/lsp_server.cpp     LSP 传输/线程/缓存；handler 只解包 params 再调 Feature*
```

> ⚠ 不要在 MCP 侧另写一份"索引 → JSON"。两份实现必然漂移（字段名/kind 编号/边界条件），
> 而且 `fpgalsp.*` 的契约只有一份。MCP 只负责"怎么拿到 LspServer 与参数"。

---

## 三之二·原、`src/hdl`（HDL 索引层）

`src/hdl/hdl_index.{h,cpp}` —— 用 Verible 的词法流 + `hdl.py` 的结构识别算法，
给上面 5 个符号类 handler 提供数据。**独立可测**（`tests/unit/hdl_index_test.cpp`；
用例数随实现变化，以 `ctest -N` / `unit_hdl_index` 的输出为准）。

| 能力 | 状态 |
| --- | --- |
| 模块/端口（ANSI + 非 ANSI）/参数/信号/例化提取 | ✅ |
| 参数常量求值（`[W-1:0]`、`$clog2`） | ✅ |
| VHDL `entity ... is` 合成模块 | ✅（ports 为空，避免层次树黑盒） |
| IP 生成的 `.v` 纳入索引（P-1：`.vhd` 走独立路径） | ✅ |
| 按 (path, 内容哈希) 的解析缓存 + 工程级签名缓存 | ✅ |
| 文件数 / token 预算（超限 `truncated`） | ✅ |
| never-raise（畸形输入只进 `parse_errors`） | ✅ |
| **positions 一律 UTF-16 列** | ✅（有中文用例） |

---

## 三之三、`initialize` 能力声明与实现的对照

早期版本有一批能力"声明了但只返回空值"（`references` / `codeAction` / `formatting` / `rename` /
`callHierarchy` / `semanticTokens` / `foldingRange` / `inlayHint`）。**Phase B 之后它们全部是
真实现**（见 `doc/status.md` 与 `doc/audit/2026-11-22/02-backend-health.md` 的逐项核查），
下面的对照表只描述"声明了什么"，不再有"空实现"这一档。

**0.23.0 起，LSP 3.17/3.18 的可选能力也补齐了**（审计 A4–A12）：

| 能力 | 方法 | 语义 |
| --- | --- | --- |
| `signatureHelpProvider` | `textDocument/signatureHelp` | 例化括号内给模块端口签名 + `activeParameter`（顶层逗号计数，命名端口内层括号不会算错） |
| `documentLinkProvider` | `textDocument/documentLink` | `.al` 的 `<File Path="…">` → 可跳转 `file://` 链接（**不**链接 `<Project Path>`；缺失文件给 tooltip） |
| `codeLensProvider` | `textDocument/codeLens` | 每个 module 定义上方 `"N instantiations"`，命令 `fpgalsp.searchHierarchy` |
| `documentHighlightProvider` | `textDocument/documentHighlight` | 同文档同名标识符全部高亮（token 流匹配，注释/字符串不误命中） |
| `selectionRangeProvider` | `textDocument/selectionRange` | 单词 → 行 → `begin/end` 或 `{}` 块 → module 体 → 整篇 |
| `diagnosticProvider` | `textDocument/diagnostic`、`workspace/diagnostic` | pull 诊断：返回**最近一次 push 的同源 items**（`previousResultId` 命中回 `unchanged`）；push 行为不变 |
| `textDocumentSync.willSave` / `willSaveWaitUntil` | 同名通知/请求 | 通知 no-op；请求立即返回 `[]`（不阻塞保存） |
| `$/setTrace` / `$/logTrace` | 同名通知 | 记录 trace level；未知 `$/` 方法仍按规范静默忽略 |
| `completionProvider.resolveProvider` | `completionItem/resolve` | 模块补全项补 `detail`/markdown `documentation`（端口表 + 定义位置） |
| `semanticTokensProvider.range` | `textDocument/semanticTokens/range` | 全量 token 按 range 过滤后**重新编码** 5 元组 |

仍未声明、也**不做**的：`implementationProvider`、`typeDefinitionProvider`、`inlineValue`、
`inlineCompletion`、`typeHierarchy`（见 `doc/design/backend-features.md` §9「明确不做」）。

---

## 四、`initialize` 能力声明（与 PROTOCOL-V2.md 逐项对照）

| capability | 值 | 协议出处 |
| --- | --- | --- |
| `textDocumentSync` | `{openClose:true, change:2, save:{includeText:true}}` | v7（Incremental）+ Phase B 与 Python 对齐 |
| `completionProvider` | `{triggerCharacters:["{","="," ","[","\t","|"], resolveProvider:true}` | Phase B 与 Python 集合一致；0.23.0 起支持 `completionItem/resolve` |
| `workspace.workspaceFolders` | `{supported:true, changeNotifications:true}` | Phase B；**A1 起同时注册 `workspace/didChangeWorkspaceFolders` handler**（切根 + 失效工程/报告缓存） |
| `hoverProvider` | `true` | v1 |
| `definitionProvider` | `true` | v1 |
| `documentSymbolProvider` | `true` | v1 |
| `workspaceSymbolProvider` | `true` | v7 |
| `referencesProvider` | `true` | v4 |
| `codeActionProvider` | `{codeActionKinds:["refactor","quickfix"]}` | v4 |
| `documentFormattingProvider` | `true` | v5（始终声明） |
| `documentRangeFormattingProvider` | `true` | v5 |
| `renameProvider` | `{prepareProvider:true}` | v5 |
| `callHierarchyProvider` | `true` | v5 |
| `semanticTokensProvider` | `{legend:{tokenTypes:[…12 类…], tokenModifiers:[]}, full:{delta:true}, range:true}` | v7 + task-14；0.23.0 起支持 `semanticTokens/range` |
| `foldingRangeProvider` | `true` | v5 |
| `inlayHintProvider` | `true` | v5 |
| `positionEncoding` | `"utf-16"` | LSP 3.17（本项目位置换算口径） |
| `executeCommandProvider` | `{commands:[…31 条…]}` | **B-A2 起声明**（见下） |
| `workspace.fileOperations` | `{willRename,didRename}`（`**/*.{v,sv,vh,svh,vhd,vhdl}`） | **B-A3 起声明** |
| `signatureHelpProvider` | `{triggerCharacters:["(",","], retriggerCharacters:[","]}` | 0.23.0（A4） |
| `documentLinkProvider` | `{resolveProvider:false}` | 0.23.0（A5） |
| `codeLensProvider` | `{resolveProvider:false}` | 0.23.0（A6） |
| `documentHighlightProvider` | `true` | 0.23.0（A7） |
| `selectionRangeProvider` | `true` | 0.23.0（A8） |
| `diagnosticProvider` | `{identifier:"fpga-lsp", interFileDependencies:false, workspaceDiagnostics:true}` | 0.23.0（A9 pull diagnostics；push 不变） |
| `serverInfo` | `{name:"fpga-lsp", version:"0.23.0"}`（`src/platform/version.h`，与扩展一致） | — |

**`executeCommandProvider` 为什么是"全表减 3 条"**：vscode-languageclient 会把
`commands` 里的**每一条**都注册成 VS Code 命令；扩展的 `contributes.commands` 已经
占了 `fpgalsp.check` / `fpgalsp.info` / `fpgalsp.restart`，重复注册会让 initialize 抛
`command 'fpgalsp.check' already exists` 并断开连接。
因此 `ExecuteCommandNames()`（34 条，分发表的唯一来源）减去这 3 条得到
`AdvertisedCommandNames()`（31 条）用于广播；那 3 条仍可被 `workspace/executeCommand`
直接调用，只是不对外广告。`unit.commands` 会断言这个子集关系。

---

## 五、诊断发布规则

* source 前缀固定 `fpga-lsp`（TD → `fpga-lsp:td`，其余见 `lsp/diagnostic.h`）
* **全量发布，不截断**（0.10.22 曾限制 200 条/文件，已移除）
* **按 `(severity, line, character)` 排序** → error(1) 在最前
* 同一 `(code, message, line, character)` 去重（对应 Python `_dedupe`）
* 发布前做 `Sanitize`（畸形输入补成合法形状），并摘掉内部用的 `data["path"]`
* 某文件这一轮已经没有诊断时**发空数组清屏**（否则 Problems 里会留着旧错误）

线程：TD 检查在**单独线程**里跑，诊断从该线程直接 `WriteMessage`（传输层内部持锁）。

### 保存（didSave）时的 TD 诊断时序（复现 0.10.28）

1. 重发该文件当前诊断（离线层为空 → 可能发空数组）
2. **立即清空该文件的 TD 诊断并重发**（"改好了但旧错误还挂着"会让用户以为没生效）
3. **只排队一次**重跑：若上一轮还在跑，只置一个 pending 标志（不叠加线程），
   当前轮结束后**再跑一轮**
4. 检查失败（建不起会话 / TD 报错）→ **清空该工程的 TD 诊断表**并重发

---

## 六、TD 会话与进程清理

* 每次 TD 检查**新建**会话，检查完立即 `Kill()` —— 因为 `read_hdl` 会关闭 stdout 管道，
  会话不可复用（`td_session.h` 硬约束，PYTHON-BUGS P-6）
* `TdSession` 内部用 **Windows Job Object**（`KILL_ON_JOB_CLOSE`）：
  服务器进程异常退出时整棵 TD 进程树一起被终止（PYTHON-BUGS P-5）
* `exit` / stdin EOF / 异常三条路径都走 `LspServer::Shutdown()`：先杀会话再 join 线程
* e2e 断言：`tests/e2e/lsp_smoke.py` 在退出后用 **PID 差集**证明 0 个残留 `td_commands_prompt.exe`

---

## 七、日志

* 只写文件（`--log <path>`）与可选 stderr（`--verbose`）
* **绝不写 stdout** —— stdout 归 LSP 协议独占
* handler 抛异常 → dispatcher 转 `-32603` 并记日志，进程不死（`jsonrpc/dispatcher.cpp`）