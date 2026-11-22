# 后端功能缺口与开发建议（LSP 能力 + FPGA 领域能力）

> **范围**：C++ 后端 `src/`。**基线**：2026-11-19 仓库状态（`doc/status.md`）。
> **方法**：先 `grep` 证明"缺失"，再读实现确认"已有"；不重复建议已存在的功能。
> **人日**：1 名熟悉本仓库的工程师的净工作量（含单测，不含评审/发布）；每条附"估算假设"。
> **约定**：`file:line` 全部指当前 HEAD；`A.` = 标准合规（LSP 3.17/3.18），`B.` = 领域差异化。

## 0. 一句话结论

后端**不缺"能算的东西"**（HDL 索引、ADC/SDC 分析、时序/面积/QoR 解析、DRC/CDC 解析、Flow 引擎都已落地），
缺的是**把它们接进编辑器的通道**：

1. `.adc/.sdc` 的三套分析器只有 MCP 能看见，LSP 的 Problems 面板一条都不出（B1）；
2. 时序报告里的 `begin_at/end_at` 恒为 `resolved=false`，插件里 `openTimingPoint` 因此永远是死链接（B2）；
3. `.al` 工程模型的 `warnings`、`exists=false` 从不发布，重命名源文件后工程静默少文件（B8/A3）。

三件事都在"已有实现的接线层"，不是从零造轮子。

---

## 1. 总表

| 编号 | 名称 | 类别 | 优先级 | 人日 | 依赖 |
| --- | --- | --- | --- | --- | --- |
| A1 | `workspace/didChangeWorkspaceFolders` 未注册（却声明了 `changeNotifications:true`） | A | P0 | 1.5 | — |
| A2 | `executeCommandProvider` 未声明（25 条命令对第三方客户端不可见） | A | P0 | 0.5 | — |
| A3 | `workspace/willRenameFiles`+`didRenameFiles` 全缺（改名即断工程） | A | P0 | 2.5 | A1 |
| A4 | `textDocument/signatureHelp` 未声明（例化端口签名） | A | P1 | 2.5 | — |
| A5 | `textDocument/documentLink` 未声明（`.al` 的 `<File Path>`、HDL 的 include 指令） | A | P1 | 2 | — |
| A6 | `textDocument/codeLens` 未声明（模块实例数/资源透镜） | A | P1 | 3 | — |
| A7 | `textDocument/documentHighlight` 未声明 | A | P2 | 1 | — |
| A8 | `textDocument/selectionRange` 未声明 | A | P2 | 1.5 | — |
| A9 | pull diagnostics（`textDocument/diagnostic`、`workspace/diagnostic`）未声明 | A | P2 | 4 | B1 |
| A10 | `textDocument/willSave` / `willSaveWaitUntil` 未声明 | A | P2 | 1 | — |
| A11 | `$/setTrace`、`$/logTrace` 被无条件丢弃 | A | P2 | 0.5 | — |
| A12 | `completionItem/resolve`、`semanticTokens/range` 缺失（性能向） | A | P2 | 2 | — |
| B1 | 约束诊断接入 LSP 诊断通道（`.adc/.sdc` 的 Problems） | B | P0 | 4 | — |
| B2 | 时序路径端点 → 源码双向跳转（`openTimingPoint` 真正可用） | B | P0 | 5 | — |
| B3 | 源码 → 时序反查（光标处寄存器属于哪条路径） | B | P1 | 3 | B2 |
| B4 | CDC/DRC 违规定位到 RTL + codeAction 修复 | B | P1 | 6 | B2 |
| B5 | 约束覆盖与冲突审计（未约束 IO/时钟、bank 电压域、交叉表） | B | P1 | 5 | B1 |
| B6 | 两次 run 的 QoR/时序差异归因（含策略对比解释） | B | P1 | 6 | — |
| B7 | 面积/资源热点 → 源码（含黑盒标记） | B | P2 | 3 | B2 |
| B8 | `.al` 工程一致性诊断 + IP/黑盒可见性 | B | P0 | 2 | — |
| B9 | 设计规则/端口文档生成 | B | P2 | 3 | B8 |
| B10 | testbench 骨架生成（自带 Verible 自检） | B | P1 | 4 | — |
| B11 | 多工程 / 多 `.al` 的工作区语义 | B | P1 | 3 | A1 |
| B12 | TD 版本兼容矩阵（报告/命令表按版本收窄） | B | P2 | 3 | — |
| B13 | 波形（VCD）与源码联动 MVP | B | P2 | 8 | B2 |

**小计**：A 12 条 ≈ 22 人日；B 13 条 ≈ 55 人日；合计 ≈ 77 人日。
**证据基线**：已注册 30 个 handler（26 个标准方法 + `workspace/executeCommand` + 3 个 `fpgaLsp/*`，见 `src/server/lsp_server.cpp:231-310`）；
`initialize` 声明 17 组能力（`src/server/lsp_server.cpp:336-396`）；MCP 39 个工具（`src/mcp/bridge.cpp`，`fpga_*` 全量清单）。

---

## 2. A 部分：LSP 合规缺口

> **状态（2026-11-22，0.23.0）：A 部分 12 条全部落地。** A1（`workspace/didChangeWorkspaceFolders`）、
> A2（`executeCommandProvider`）、A3（`willRenameFiles`/`didRenameFiles`）早先已修；A4–A12
> （signatureHelp / documentLink / codeLens / documentHighlight / selectionRange / pull diagnostics /
> willSave / `$/setTrace` / `completionItem.resolve` / `semanticTokens.range`）在 0.23.0 实现并声明，
> 实现与单测见 `src/server/features_optional.{h,cpp}`、`tests/unit/features_optional_test.cpp`。
> 下面的条目保留为**问题记录**（修复前的证据），不再是待办。

### A.0 声明 vs 注册对照

| 能力（`lsp_server.cpp:336-396`） | 声明位置 | 对应 handler |
| --- | --- | --- |
| `textDocumentSync{openClose, change:2, save.includeText}` | :340-346 | didOpen/didChange/didSave/didClose :237-244 ✅ |
| `completionProvider{triggerCharacters}` | :349-352 | completion :255 ✅ |
| `workspace.workspaceFolders{supported:true, changeNotifications:true}` | :355-360 | **无** `workspace/didChangeWorkspaceFolders` ❌ |
| `hoverProvider` | :362 | hover :257 ✅ |
| `definitionProvider` | :363 | definition :259 ✅ |
| `documentSymbolProvider` | :364 | documentSymbol :261 ✅ |
| `workspaceSymbolProvider` | :365 | workspace/symbol :263 ✅ |
| `referencesProvider` | :366 | references :265 ✅ |
| `codeActionProvider{codeActionKinds:[refactor,quickfix]}` | :368-370 | codeAction :271 ✅ |
| `documentFormattingProvider` | :373 | formatting :273 ✅ |
| `documentRangeFormattingProvider` | :374 | rangeFormatting :275 ✅ |
| `renameProvider{prepareProvider}` | :376-378 | prepareRename/rename :277-280 ✅ |
| `callHierarchyProvider` | :380 | prepare/incoming/outgoing :281-286 ✅ |
| `semanticTokensProvider{legend, full.delta}` | :382-390 | full + full/delta :287-292 ✅ |
| `foldingRangeProvider` | :392 | foldingRange :293 ✅ |
| `inlayHintProvider` | :393 | inlayHint :267 ✅ |
| `positionEncoding:"utf-16"` | :396 | UTF-16 换算层（`src/platform/utf16.cpp`）✅ |
| —— **未声明** —— | — | `workspace/executeCommand` :305-306（已注册、未声明）❌ |

结论：**"声明了没实现"只有 1 条（工作区文件夹变更），"实现了没声明"1 条（executeCommand），
"标准有但完全没做"10 条**。

补充一条**代码卫生**观察：`HandleEmptyList`/`HandleEmptyObject`/`HandleNull` 是早期"声明了但只有空壳"的产物
（声明 `lsp_server.h:195-201`，定义 `lsp_server.cpp:493-501`），现在**注册表里没有任何引用**（dead code），
其中 `HandleEmptyObject` 只有声明、连定义都没有。它们的存在说明"空壳能力"曾是真实问题；
现在所有已声明能力都有真实实现，建议删除这三个 handler（或加注释说明用途），否则下一个人会误以为还有空壳能力。

### A.0.1 A 部分按规范严重度排序

| 严重度 | 判据 | 条目 |
| --- | --- | --- |
| 高（协议说谎 / 数据损坏） | 客户端行为与服务端声明不一致，或导致工程构建结果错误 | A1（声明支持多根却不响应）、A3（改名断工程）、A2（命令不可见） |
| 中（编辑体验缺失） | 标准能力缺位，用户有可感的"这不是 IDE" | A4、A5、A6 |
| 低（可用性/性能/可观测） | 不影响正确性 | A7、A8、A9、A10、A11、A12 |

### A1 声明了 `workspaceFolders.changeNotifications:true`，但没注册 `workspace/didChangeWorkspaceFolders`

| 项 | 内容 |
| --- | --- |
| 规范要求 | 服务端声明 `workspace.workspaceFolders.changeNotifications`（布尔或 `{globPatterns}`）时，客户端会发 `workspace/didChangeWorkspaceFolders` 通知；服务端必须据此更新工作区根集合。 |
| 当前行为 | 声明在 `src/server/lsp_server.cpp:355-360`，注册表 `:231-310` 里无该 method。`jsonrpc::Dispatcher::Dispatch` 对"无 handler + 无 id"的 notification **静默丢弃**（`src/jsonrpc/dispatcher.cpp:74-85`）。根只有一个字段 `root_uri_`，只在 `initialize` 写入（`lsp_server.cpp:330`），`RootPath()` 直接读它（`:1532-1534`）。 |
| 客户端可见后果 | 用户在 VS Code 里"添加文件夹到工作区"后：新根下的 `.al` 永远找不到（`project::FindProjectAl` 只在旧 root 递归扫，`src/project/al_project.h:157-158`）；模块树/工作区符号/新建工程全部停留在第一个根。更糟的是 `supported:true` 会让客户端显示"该服务端支持多根"，属于**对客户端撒谎**。 |
| 修法 | 注册 handler：把 `root_uri_` 换成 `std::vector<std::string> roots_`（首个 = initialize 的 root）；`ResolveProjectAl` 改为"按 roots 顺序逐个试"，命中即返回（发现缓存的键里**已经**含 root，`lsp_server.h:396`，所以键不用改，只需多试几次）；增删后 `InvalidateProjectCaches()` + 推 `fpgaLsp/indexChanged`（`lsp_server.cpp:465-475`）。 |
| 工作量 | **1.5 人日**（假设：单根→多根只改 `LspServer` 的 root 状态与发现缓存键，不动 `project::` 层；handler 本身 ≤ 60 行） |
| 验收 | 两个根各放一个 `.al`：添加第二个根后 `workspace/symbol` 能返回第二个根的模块（不重启）；`fpgaLsp/moduleTree` 按当前活动文件所属工程切换。 |

### A2 `workspace/executeCommand` 已注册，但 `executeCommandProvider` 没声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | `initialize` 必须返回 `executeCommandProvider.commands`（字符串数组）；客户端只用这份清单注册命令，且**只有声明过的命令**才允许被 `workspace/executeCommand` 调用。 |
| 当前行为 | handler 已注册（`src/server/lsp_server.cpp:305-306`），`commands.cpp:698-966` 是 25 条 `fpgalsp.*` 命令（info/projects/setProject/tdInfo/device/check/restart/cancelFlow/runFlow/compareStrategies/cables/program/svfgen/reports/runAnalysis/instantiate/copyInstantiation/symbolPath/duplicateModules/searchHierarchy/addSourceFile/setProjectName/updateProject/debugHighlight/metrics）。能力声明 `:336-396` 里没有 `executeCommandProvider`（grep `executeCommandProvider` 全仓 0 命中）。 |
| 客户端可见后果 | 本仓库的 VS Code 扩展绕过了这条路径（自己 `rpc.execute`，`vscode-extension/extension.js:1295` 等），所以"能用"；但任何**第三方客户端**（Neovim/Emacs/Helix/自研 agent UI）拿不到命令清单 → 只能硬编码命令名，`codeAction` 里也不能回传 `Command` 对象（当前 `codeAction` 只返回带 `edit` 的动作，`src/server/features_code_actions.cpp:9-11`）。 |
| 修法 | 把 `commands.cpp` 的 if 链抽成一张 `constexpr` 命令表（名字 + 参数 schema），`initialize` 输出 `executeCommandProvider:{commands:[…]}`；同时被 MCP 的 `tools/list` 复用一份元数据，避免两处漂移。 |
| 工作量 | **0.5 人日**（假设：只做命令名声明，不引入参数校验） |
| 验收 | `initialize` 响应里 `executeCommandProvider.commands.size()==25`；e2e 断言清单与 `commands.cpp` 的 if 链集合完全一致（用脚本对比，防漏）。 |

### A3 `workspace/willRenameFiles` + `didRenameFiles` 全缺 → 改名即断工程（P0）

| 项 | 内容 |
| --- | --- |
| 规范要求 | LSP 3.16+ 文件操作：服务端声明 `workspace.fileOperations.willRename.filters`，客户端在资源管理器重命名前发 `workspace/willRenameFiles`（请求，期望返回 `WorkspaceEdit`），完成后发 `workspace/didRenameFiles`（通知）。 |
| 当前行为 | grep `willRenameFiles`/`didRenameFiles`/`fileOperations` 全仓 **0 命中**；`.al` 用**相对路径**记录源文件（`src/project/al_project.h:59-72`，`AlFile.rel`），文件不存在时 `exists=false` 且 `InBuild()` 返回 false（`:72`），而 `MakeReadHdlCommand` 只喂存在的路径（`src/td/td_diagnostics.h:107-108`）。 |
| 客户端可见后果 | 重命名 `top.v`→`top_core.v`：`.al` 仍指旧路径 → TD `read_hdl` 少喂一个文件 → elaborate 少一个模块 → 用户看到"0 error"的**假绿**（与 `doc/status.md:67` 的 P-1 同族故障）。同理移动/删除源文件后 `Problems` 一片空白，因为 `.al` 的 `warnings` 从不发布（见 B8）。 |
| 修法 | 声明 `workspace.fileOperations.willRename.filters=[{pattern:{glob:"**/*.{v,sv,vh,svh,vhd,vhdl,adc,sdc}"}}]`；handler 内 `project::ParseAl` → 匹配 `AlFile.rel`（口径复用 `ide::ProjectRelativePath`，`src/server/ide_features.h:86`）→ 返回 `WorkspaceEdit{changes:{<al_uri>:[{range,newText}]}}`，写盘沿用 `ide::WriteTextFileAtomic`（`ide_features.h:83`，已是原子写 + 回滚）。`--allow-write` 关闭时返回空 edit 并 `window/showMessage` 提示（不要静默）。 |
| 工作量 | **2.5 人日**（假设：只处理 `.al` 里的 `<File Path>` 改写；`didRenameFiles` 只用于失效索引缓存） |
| 验收 | 在客户端重命名一个源文件：`.al` 的 `Path` 被自动改写、`ParseAl` 后 `exists=true`、`InBuild()=true`；重命名 `.adc` 时同样生效；删除文件时 `.al` 上出现 1 条 error（与 B8 联动）。 |
| 风险 | 若客户端不支持 fileOperations（旧版 VS Code / 部分 LSP 客户端），需保留"`didChangeWatchedFiles` 检测到 .v 消失 + .al 未变 → 提示"的兜底路径。 |

### A4 `textDocument/signatureHelp` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | 声明 `signatureHelpProvider{triggerCharacters, retriggerCharacters}`，在 `(`、`,` 处返回 `SignatureHelp{signatures[],activeSignature,activeParameter}`。 |
| 当前行为 | grep `signatureHelp` 全仓 0 命中；`completionProvider` 只有 `triggerCharacters:["{","="," ","[","\t","|"]`（`lsp_server.cpp:349-352`）。索引里已有例化所需全部信息：`Module.ports`（方向/`msb`/`lsb`/`width`，`src/hdl/hdl_index.h:76-86`）、`Instance.connections` + `connection_ports`（`:116-127`）。 |
| 客户端可见后果 | 写 `u_alu (` 时没有任何参数提示；用户只能先 `fpgalsp.copyInstantiation` 再粘贴（`extension.js:610-625`）。对新用户这是最直观的"这 IDE 不懂 Verilog"。 |
| 修法 | 声明 `signatureHelpProvider{triggerCharacters:["(","#",","], retriggerCharacters:[","]}`；在 handler 里用 `HdlIndex::SymbolAt`（`hdl_index.h:354`）判定光标是否落在实例化语句内（`Instance.start_line..end_line`），用 `ModuleByName(Instance.module)->ports` 生成 `SignatureInformation`（label 形如 `module foo(a, b, clk)`），`activeParameter` 由 `connection_ports` 已出现的端口数给出。 |
| 工作量 | **2.5 人日**（假设：只支持命名端口 `.a(x)` 与非 ANSI 端口两种写法，不支持位置端口嵌套函数） |
| 验收 | 在实例化括号内 `,` 之后 `activeParameter` 自增；label 的端口顺序与 `Module.ports` 完全一致（golden 断言）；`.adc/.sdc` 返回 null（不误触发）。 |

### A5 `textDocument/documentLink` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | 声明 `documentLinkProvider`，返回 `DocumentLink{range,target,tooltip}`，客户端渲染成可点击链接。 |
| 当前行为 | grep `documentLink` 全仓 0 命中。`.al` 的结构是 `<Verilog><File Path="..."/>`（`src/project/al_project.h:16-18`），`xml_lite` **只保留 `error_offset`、无节点位置**（`src/project/xml_lite.h:54`），所以不能靠 XML 解析拿 range。 |
| 客户端可见后果 | 打开工程配置 `.al` 只能看到一堆 XML，不能点进源文件；Verilog 的 `` `include "defs.vh" `` 也不可跳转。 |
| 修法 | 声明 `documentLinkProvider{resolveProvider:false}`；`.al` 用**行级正则**扫 `Path="([^"]+)"`（不依赖 `xml_lite` 的位置信息，也不会因 TD 改格式而崩），`target` = `file://` + `project::ResolveRelative`（`al_project.h:134-135`）结果；HDL 侧扫 `` `include "..." ``。文件不存在时 `target` 缺省 + `tooltip:"file not found"`。 |
| 工作量 | **2 人日**（假设：不做完整 XML 位置跟踪，只做 `Path="`/`` `include "`` 两种字面量） |
| 验收 | 点击 `.al` 里的 `Path` 打开对应文件并定位行；文件缺失时链接置灰 + tooltip（不报错）；对 500 个 `<File>` 的 `.al` 响应 < 20 ms。 |

### A6 `textDocument/codeLens` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | 声明 `codeLensProvider{resolveProvider}`；返回 `CodeLens{range, command}`，在编辑器里以"行内小字"呈现可点信息。 |
| 当前行为 | grep `codeLens` 全仓 0 命中。可用数据全在手上：`HdlIndex::AllInstances`（`hdl_index.h:342`）、`AreaReport.modules[].resources`（`src/reports/area.h:57-65`）、`TimingGroup`（`src/reports/timing.h:116-128`）。 |
| 客户端可见后果 | "这个模块被例化了几次 / 占多少 LUT / 有没有时序违例"必须开 reports webview 才知道；编辑器里没有任何就地提示。 |
| 修法 | 声明 `codeLensProvider{resolveProvider:true}`；MVP：每个 module 头上一行 `N instances`（纯索引，`IndexForProject` 命中缓存时 < 5 ms）；`resolve` 阶段再叠加面积/时序（走 `ReportStore::Get(mask=kArea|kTiming)`，`src/server/report_store.h:96`）。命令先用空串（纯展示，不点击）；若目标客户端对空 command 报错，改用扩展已注册的 no-op 命令。 |
| 工作量 | **3 人日**（2 人日索引版 + 1 人日报告版；假设不新增客户端命令） |
| 验收 | 打开 200 文件工程：首次 `< 300 ms` 出 lens，之后滚动不重算（resolve 分离生效）；实例计数与 `fpgaLsp/moduleTree` 的 children 数一致。 |
| 风险 | lens 数量 = 模块数，超大工程（>500 模块）可能拖慢渲染 → 加 `settings.codeLens.maxModules`（默认 200，超出停用）。 |

### A7 `textDocument/documentHighlight` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | 声明 `documentHighlightProvider`，光标处符号的同名出现全部返回（`kind`: Text/Read/Write）。 |
| 当前行为 | grep `documentHighlight` 0 命中；但 token 级引用搜索已实现（`HdlIndex::References`，`hdl_index.h:362-365`，注释里明确"用 token 而不是正则，注释/字符串不误命中"）。 |
| 客户端可见后果 | 光标放在 `state` 上不高亮其余出现；用户只能手动"查找全部"（会命中文档注释）。 |
| 修法 | 声明 `documentHighlightProvider:true`；handler = `References(...)` 结果按当前文件过滤，`kind` 统一 `1`（Text，避免说谎）。**不做**读写区分（token 流判不出赋值关系）。 |
| 工作量 | **1 人日**（几乎只是接线 + 过滤） |
| 验收 | 光标在端口/信号/实例名上：同文件同名 token 全部高亮，注释里的同名文本不高亮；`.adc` 返回 []。 |

### A8 `textDocument/selectionRange` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | 声明 `selectionRangeProvider`，返回按"由内向外"嵌套的 `SelectionRange{range,parent}`，驱动"扩展选区"。 |
| 当前行为 | grep `selectionRange` 只命中 CallHierarchyItem 的字段（`src/server/features_call_hierarchy.cpp:98`），无 provider。位置索引其实够用：`ModulePositions{port_pos, signal_pos, inst_name_pos, modref_pos}`（`hdl_index.h:165-173`）+ `Instance.start_line/end_line`（`:122-125`）。 |
| 客户端可见后果 | `Shift+Alt+→` 只能按词/行扩，不能"标识符 → 语句 → 模块体"。 |
| 修法 | 声明 `selectionRangeProvider:true`；构造 4 层链：标识符 span → 所在语句（实例化用 `Instance.start_line..end_line`，声明用行）→ 所在模块（`Module.line..end_line`）→ 全文。 |
| 工作量 | **1.5 人日** |
| 验收 | 光标在 `u_core` 上连按 3 次扩展依次覆盖实例化语句、模块体、全文；`.adc` 至少支持"词 → 行"。 |

### A9 pull diagnostics（`textDocument/diagnostic`、`workspace/diagnostic`）未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | LSP 3.17 诊断 pull 模型：`diagnosticProvider{identifier, interFileDependencies, workspaceDiagnostics}`，客户端按文档拉取 `DocumentDiagnosticReport`（可 `relatedDocuments` + `partialResultToken` 流式）。 |
| 当前行为 | grep `diagnosticProvider` 0 命中；现在是纯 push：`PublishDiagnostics`（`lsp_server.cpp:1084`）+ 两个来源表 `diag_td_`/`diag_lint_`（`lsp_server.h:380-381`）+ 合并函数 `MergeDiagnosticSources`（`lsp_server.cpp:1111-1167`）。TD 一轮检查实测 **628 ms（合成小工程）/ 1154 ms（真实工程副本）**（`doc/audit/2026-11-19/perf/TD.md:13-14`，分段字段见 `src/td/td_timing.h:25-38`），靠 push 会在"保存后清空再重发"之间闪（`lsp_server.cpp:978-989`）。 |
| 客户端可见后果 | 大工程里 Problems 会闪；客户端无法"只拉当前文件的诊断"，也无法做"工作区诊断报告"。 |
| 修法 | 声明 `diagnosticProvider{identifier:"fpga-lsp", interFileDependencies:true, workspaceDiagnostics:true}`；`textDocument/diagnostic` 直接返回现有两张表（+B1 的第三张）的并集，`resultId` 用现有 revision 概念；**保留 push** 作为兼容（VS Code 优先 pull）。与 B1 一起做省一次改动。 |
| 工作量 | **4 人日**（含 `resultId`/unchanged 语义与 push 共存测试；依赖 B1 落地后表结构稳定） |
| 验收 | 客户端切到 pull 后 Problems 不再闪；`resultId` 不变时返回 `{kind:"unchanged"}`；同一工程 push 与 pull 的诊断集合逐条一致。 |

### A10 `textDocument/willSave` / `willSaveWaitUntil` 未声明

| 项 | 内容 |
| --- | --- |
| 规范要求 | `textDocumentSync.willSave`（0/1/2）与 `willSaveWaitUntil`（布尔）。 |
| 当前行为 | `textDocumentSync` 只有 `openClose/change/save`（`lsp_server.cpp:340-346`），无 `willSave`；`didSave` 里才清 TD 诊断（`:978-989`）。 |
| 客户端可见后果 | 保存前无法做"清旧诊断/预校验"，用户按下保存到诊断刷新之间仍看到旧波浪线（"我明明改好了"）。 |
| 修法 | 声明 `willSave:true`（只收通知）+ `willSaveWaitUntil:true`，在 `willSaveWaitUntil` 里做"最多 150 ms 的 lint 重算"并返回 `TextEdit[]`（无改动则空）。**注意**：waitUntil 会阻塞保存，超时必须立即返回空数组。 |
| 工作量 | **1 人日** |
| 验收 | 保存耗时增加 < 150 ms（`ctest -L bench` 口径）；超时场景返回 `[]` 且保存不卡死。 |
| 风险 | 与 `td_check_on_save` 的 300 ms 去抖叠加会放大保存延迟 → 默认只跑 lint，不跑 TD。 |

### A11 `$/setTrace`、`$/logTrace` 被无条件丢弃

| 项 | 内容 |
| --- | --- |
| 规范要求 | `$/setTrace` 设置 `trace` 级别（off/messages/verbose），服务端据此通过 `$/logTrace` 输出协议级跟踪。 |
| 当前行为 | `Dispatcher::Dispatch` 对**所有** `$/` 前缀直接 `return false`（`src/jsonrpc/dispatcher.cpp:59-61`）。`$/cancelRequest` 不受影响（`AsyncDispatcher` 先行处理，`src/server/async_dispatcher.cpp:125-126`，且 LSP 模式确实走 AsyncDispatcher，`src/app/main.cpp:302-306`）。 |
| 客户端可见后果 | 客户端开 `trace:verbose` 后服务端毫无反应，排查协议问题时拿不到 `$/logTrace`（只能开 `--log` 文件，但那是服务端日志不是协议 trace）。 |
| 修法 | 在 dispatcher 的 `$/` 分支前特判 `$/setTrace`：记录 level，`verbose` 时把每条 `recv <method>` 也写成 `$/logTrace` 通知。其余 `$/` 仍静默忽略（符合规范"未知 `$/` 必须忽略"）。 |
| 工作量 | **0.5 人日** |
| 验收 | 发 `$/setTrace{value:"verbose"}` 后能收到 `$/logTrace` 通知；`value:"off"` 后停止；未知 `$/foo` 仍不报错。 |

### A12 `completionItem/resolve` 与 `semanticTokens/range` 缺失（性能向）

| 项 | 内容 |
| --- | --- |
| 规范要求 | `completionProvider.resolveProvider:true` 允许先发轻量 item、再按需补 `documentation/detail`；`semanticTokensProvider.range:true` 允许客户端只请求可见区域。 |
| 当前行为 | `completionProvider` 只有 `triggerCharacters`（`lsp_server.cpp:349-352`）；semantic 只有 `full` + `full/delta`（`:287-292`）。补全候选集是 745 个器件原语 + 48 个关键字（`src/server/hdl_handlers.cpp:73-75` 明确"745 原语 + 48 关键字"）。 |
| 客户端可见后果 | 每次补全都一次性下发全部原语的 documentation，大工程的补全弹窗有可感延迟；超大文件 semantic token 每次全量（`FeatureSemanticTokens` 无条件全篇编码，`src/server/features_semantics.cpp:39-65`）。 |
| 修法 | (1) `resolveProvider:true`，`completion` 只填 `label/kind/insertText`，`completionItem/resolve` 再查原语表补 `documentation`；(2) 声明 `semanticTokensProvider.range:true` 并接 `textDocument/semanticTokens/range`。 |
| 工作量 | **2 人日**（各 1 人日） |
| 验收 | 补全响应字节数下降 ≥ 50%（`ctest -L bench` 的 completion 用例）；range 版本返回的 token 与 full 版在同一区间逐条相同。 |

---

## 3. B 部分：FPGA/EDA 领域功能

> 取舍原则：**只提能复用现有模块、且"TD 数据 → 源码"这条链路缺环**的功能。
> 已被 MCP 覆盖（39 工具）的不重复造，只补"编辑器内可达"。

### B1 约束诊断接入 LSP 诊断通道（`.adc/.sdc` 的 Problems）

| 项 | 内容 |
| --- | --- |
| 现状基础 | 分析器全在：`constraints::adc::Diagnostics`（`src/constraints/adc.h:166-181`）、覆盖率摘要 `CoverageDiagnostic`（`adc.h:187-188`，恒 severity 3、全约束时不产生）、`sdc::Diagnostics`（`src/constraints/sdc.h:113-115`）、`sdc::UndefinedClockDiagnostics`（`sdc.h:173`，本工程新增的 `SDC-CLOCK`）。**但 LSP 侧一条都没接**：`RunDiagnostics` 只走 `PublishMerged`（`lsp_server.cpp:1215-1236`），合并函数只接受 lint + TD 两个来源（`lsp_server.cpp:1111`、`lsp_server.h:460-461`）。 |
| 缺口 | (1) `lsp_server.cpp:1228-1230` 的注释仍写"这两个分析器在本项目里属于独立任务，本阶段没有"——**已过时**，实际只有 MCP 在用（`src/mcp/bridge.cpp:2087-2109`）；(2) MCP 读的是**磁盘**（`ReadTextFile`，`bridge.cpp:2093/2102`），未保存缓冲区里的编辑看不到；(3) ADC 覆盖率诊断只在 MCP 的 `constraint_audit` 里出现，编辑器里没有。 |
| 方案 | 加第三个诊断来源 `diag_constraint_`（键 = 文件路径）；新增 `ScheduleConstraint()`，复用 `LintService` 的"去抖 + 单飞 + 结果作废"骨架（`src/server/lint_service.h`，`lsp_server.cpp:1257-1260` 是入口）；`ComputeConstraints()` 用 `LoadText`（缓冲区优先，`lsp_server.h:322`）而非磁盘；`MergeDiagnosticSources` 扩成三方稳定排序（现有排序/去重规则 `lsp_server.h:456-461`）；`.adc/.sdc` 打开即算一次（`HandleDidOpen` 里与 `ScheduleLint` 并列）。 |
| 工作量 | **4 人日**（假设：复用 LintService 骨架、不改协议、不加配置项；新增 ~350 行 + 单测） |
| 验收 | 打开含未约束总线的 `.adc`：Problems 出现 1 条 `ADC-COVERAGE`（info）+ 每 bit 的 `BIT-8123`（沿用现有 severity），且**改缓冲区不保存**后 300 ms 内更新；同一工程 LSP 诊断条数与 `fpga_constraint_audit` 的 `issues` 完全一致（同源断言）；正常工程 0 诊断。 |
| 风险 | 双通道（LSP push + MCP pull）口径漂移 → 用同一个纯函数、加 golden 对比测试；`.adc` 有 71 项 IO 标准白名单（`adc.h:73`），误报代价高，新增检查必须先过白名单回归。 |

### B2 时序路径端点 → 源码双向跳转（`openTimingPoint` 真正可用）

| 项 | 内容 |
| --- | --- |
| 现状基础 | 报告解析完备：`TimingReport.paths[]` 带 `begin/end/slack/group/details/delay`（`src/reports/timing.h:82-104`），端点清洗 `CleanTimingPoint`（`timing.h:184`），协议 v3 已定义 `begin_at/end_at{signal,hierarchy,file,line,character,resolved,module,reason}`（`timing.h:68-79`）。 |
| 缺口 | 端点映射**恒降级**：`timing.cpp:439-452` 无条件 `resolved=false; reason="hdl-index-unavailable"`（文件头 `:60-67` 自述"当前 HDL 索引尚未实现"——**也已过时**，索引早就有了）。后果：插件 `openTimingPoint` 拿到 `file=null`，直接弹 "no parsable RTL location"（`vscode-extension/extension.js:1303-1313`，判断在 `:1305-1307`）。 |
| 方案 | 分层解法：`reports` 层定义注入点 `using EndpointResolver = std::function<std::optional<TimingEndpoint>(const std::string& point)>`（照抄 `hdl::TextLoader` 的做法，`src/hdl/hdl_index.h:395`），`AttachEndpointLocations(report, resolver)` 保留无 resolver 时的降级；resolver 实现在 `server` 层：`CleanTimingPoint` → 逐级剥层次（`hierarchy`）→ `ModuleByName` 走实例链 → 在目标模块行范围内用 `signal_pos`（`hdl_index.h:168`）命中信号名 → 填 `file/line/character/module`。 |
| 工作量 | **5 人日**（resolver 3 + 接线/回归 2；假设不改 `reports` 的 JSON 形状，只把 `resolved` 从恒 false 变成真值） |
| 验收 | 对 `tests/fixtures/reports/` 里的真实 `final_timing.rpt`：`begin_end_resolved.resolved / total ≥ 0.6`；VS Code 的 `openTimingPoint` 能跳到寄存器/端口声明行（而不是弹提示）；解析失败项仍带 `reason` 且不崩。 |
| 风险 | **综合改名**：报告里的 `reg352_or_reg[2]_syn_4` 带 TD 后缀，源码里可能叫 `reg352`。必须实现"去后缀（`_syn_\d+`/`_reg`/`_rep`）+ 层级前缀回退"两段启发式；**若某路径只能定位到模块级，应返回 `module` 且 `resolved=false` + `reason:"module-only"`**，不要假装精确——否则用户点了会跳到错误的行。 |

### B3 源码 → 时序反查（光标处寄存器属于哪条路径）

| 项 | 内容 |
| --- | --- |
| 现状基础 | 报告已在 `ReportStore` 里按文件缓存成不可变快照（`report_store.h:96-99`、`:150-154`），`fpgaLsp/reports?section=timing` 已能取到全部 paths（`src/server/commands.cpp:293-310`）。 |
| 缺口 | 只有"报告 → JSON"，没有"源码位置 → 路径"的倒排；`inlayHint` 目前只做例化类型提示（`features.h:51-52`），hover 里没有时序信息。 |
| 方案 | `ReportStore` 在解析 `TimingReport` 后建倒排 `(path,line) -> [path_index]`（复用 B2 的正向映射，跑一次即可）；提供 `fpgaLsp/timingAtPoint{uri,line,character}` 返回 `{ok, slack, check, group, fromClock, toClock, pathIndex}`；同时在 `hover` 尾部追加一行（"Timing: slack 0.171 ns (setup, clk0→clk0)"）——hover 是零客户端改动的出口。 |
| 工作量 | **3 人日**（依赖 B2 的映射；倒排 ≤ 150 行） |
| 验收 | 光标停在顶层输出寄存器：hover 出现 slack 行；同一寄存器属于多条路径时取 slack 最小者并注明 `pathIndex`；无时序数据时 hover 行为与现在**逐字节一致**。 |
| 风险 | 建倒排要把整份 timing 的 paths 全解析（大报告 10 MB 级，`report_store.h:60-62` 提到 dump 100–145 ms）→ 必须懒建（第一次请求 hover 时建，按 revision 失效）。 |

### B4 CDC/DRC 违规定位到 RTL + codeAction 修复

| 项 | 内容 |
| --- | --- |
| 现状基础 | 解析层齐备：`analysis::ParseDrc`（`violations/rules/details/clean`）、`ParseCdc`（`rows:[[cell]]`、`counts{safe,unsafe,unknown,no_async_reg}`）（`src/analysis/analysis.h:60-70`），器件不支持时的降级文案也有（`CdcUnsupportedByDevice`，`analysis.h:78-79`）；执行入口 `fpgalsp.runAnalysis`（`commands.cpp:936`）+ MCP `fpga_analysis_run`。 |
| 缺口 | (1) CDC 的 `rows` 是 **cell 名**，没有任何"cell → 源文件行"的映射；(2) DRC 违规也只给 rule/count，不给位置；(3) 只有 webview 文本输出，Problems 里没有、也没有 `codeAction` 修复；(4) `doc/status.md:58-59` 明确"真实 DRC/CDC 端到端未验证（本机无 `*_pr.db`）"。 |
| 方案 | 用 B2 的 resolver 把 cell 名（层次路径 `a/b/c_reg`）解析成 `(file,line)`；发布为 `source:"fpga-lsp:cdc"` 的 diagnostic（`code:"CDC-UNSAFE"`/`"CDC-NO-ASYNC-REG"`，severity 2）；`codeAction` 给三个动作：(a) 生成两级同步器模板（与 B10 共享"代码生成 + Verible 格式化"），(b) 往 `.al` 关联的 `.sdc` 追加 `set_clock_groups -asynchronous` / `set_false_path`（用 `sdc::Parse` 自检插入后不新增诊断），(c) suppress（写 `.fpgalsp/cdc-ignore.json`）。 |
| 工作量 | **6 人日**（映射 3、codeAction + 模板 2、测试 1；依赖 B2） |
| 验收 | 给一份录制的 CDC 文本 fixture：Problems 在对应 RTL 行出 `CDC-UNSAFE`；动作 (b) 生成的 SDC 行能被 `sdc::Parse` 解析且 `sdc::Diagnostics` 无新增；器件不支持 CDC 时只出 1 条 info 说明（沿用 `CdcUnsupportedReason`），不刷屏。 |
| 风险 | 没有真实 `*_pr.db` → 门禁只能用文本 fixture（把 `check_cdc` 的原始输出录成 `tests/fixtures/analysis/cdc_*.txt`）；一旦拿到真机产物，映射命中率可能有落差，需保留"映射失败 → 只报 cell 名"的降级。 |

### B5 约束覆盖与冲突审计（未约束 IO/时钟、bank 电压域、交叉表）

| 项 | 内容 |
| --- | --- |
| 现状基础 | `adc::Coverage` 已产出 `covered/missing/unconstrained_ports/extra_ports/duplicate_ports/duplicate_pins`（`adc.h:142-151`），`PortBits` 处理 width vs msb/lsb 的优先级（`src/constraints/port.h:9-15`）；器件的 `clock_capable`/`int_bank`/`ext_bank`/`lvds` 都能从 TD 拿到（`src/mcp/bridge.cpp:2394-2401`）。 |
| 缺口 | (1) 覆盖率只经 MCP 暴露（`bridge.cpp:2280-2341`），编辑器不可见（这条被 B1 解决一半）；(2) **SDC 侧没有覆盖分析**——只有 `create_clock` 的字符串计数（`bridge.cpp:2320-2322`）；(3) **没有 bank 电压域检查**：`adc.cpp` 里 grep `int_bank|ext_bank|bank|VCC|voltage` **0 命中**，即 71 项 IO 标准只校验"名字是否合法"（`adc.h:73-77`），不校验"同一 bank 混 3.3V/1.8V"；(4) 没有 ADC↔SDC↔top 三方交叉：时钟接在普通 IO 上、SDC `get_ports` 引用了 ADC 里没约束的引脚、跨时钟域但没有任何 `set_clock_groups`（命令表里有 `set_clock_groups`/`set_false_path`/`set_multicycle_path`，`sdc.cpp:95-108`，但**没有任何"该有却没有"的检查**）。 |
| 方案 | 新增 `constraints::audit` 纯函数层（不动 `adc.cpp/sdc.cpp` 的既有诊断，避免破坏 Python 对齐的 severity 契约），产出四类新诊断（新 code 前缀 `AUD-*`）：`AUD-CLK-PIN`（create_clock 的端口在 ADC 里不是 `clock_capable` 脚）、`AUD-BANK-VOLT`（同 bank 混用不同电压的 IOSTANDARD，需内置"标准 → 电压"小表，先覆盖板上常见 6 种）、`AUD-IO-UNCON`（未约束 IO，按端口聚合一条 + 可展开）、`AUD-CDC-NOEXC`（有 `create_generated_clock` 但无 `set_clock_groups`/`set_false_path` 时提示）。全部 severity ≤ 2，且**默认只在"工程级命令"里出**（不污染单文件编辑）。 |
| 工作量 | **5 人日**（4 个检查 × 1 人日 + 1 人日汇总/回归；假设"标准→电压"表人工维护 6 项） |
| 验收 | 一个故意做错的工程（时钟接普通 IO + 同 bank 混 3.3V/1.8V + 半个总线没约束）各出 1 条对应 code；正常示例工程 0 条；`AUD-*` 不影响现有 `ADC-*`/`SDC-*` 的条数与 severity（回归断言）。 |
| 风险 | "标准 → 电压"靠人工维护，错表 = 误报，最伤信任 → 表放 `constraints/data/`，与 `adc_io.inc` 同目录，缺项时**不检查**（宁漏不误）。 |

### B6 两次 run 的 QoR/时序差异归因

| 项 | 内容 |
| --- | --- |
| 现状基础 | `artifacts.runs[]` 已列出全部 run 目录（`src/reports/artifacts.h:43-50`，`RunEntry{name,dir,exists}`）；`AreaReport.modules[]` 带层次 `path` + 每种资源的用量（`area.h:57-65`）；`strategy::QorNumbers` 已能从 qor 里摘 `setup_wns_ps/setup_tns_ps/luts/registers/slices`（`src/flow/strategy.h:38-48`）；`ReportStore` 的解析缓存以 `(path,stamp)` 为键（`report_store.h:150-154`），天然支持"同时读两个 run 的文件"。 |
| 缺口 | `BuildReports` **只读最新一次**（`reports_aggregate.h:158-164` 的 `BuildReports(project)` 语义）；`fpgaLsp/reports` 没有 diff 段（section 掩码见 `reports_aggregate.h:53-65`）；`fpgalsp.compareStrategies` 只在**临时副本**里跑多策略并给 `runs[]/best`（`strategy.h:72-76`），没有"量化归因"。 |
| 方案 | (1) `reports/sections` 加一位 `kDiff`；(2) `BuildReports` 增 `compareWith=<runName>` 参数，内部对两个 run 各解析一次（走同一个 `ReportParseCache`）；(3) 产出 `diff{timing:{wnsDelta,tnsDelta,perGroup[],worstPaths[]}, area:{perModule[{path,lutΔ,seqΔ,matched}],unmatched[]}, qor:{metricsΔ[]}, suggestions[]}`——`suggestions` 直接取 `*_qor_suggestion.tcl`（`qor.h:66-68`，`ReportStore::GetSuggestions` 已实现 `report_store.h:129-130`）挂到恶化项上，让输出"可解释"；(4) `fpgaLsp/reportDiff` 自定义方法 + MCP 工具 `fpga_report_diff`。 |
| 工作量 | **6 人日**（解析复用 1、diff 算法 3、协议/测试 2；假设只比两个 run，不做 N 路） |
| 验收 | 用两个 run fixture：输出按恶化量降序、同输入两次**字节一致**；层次路径对不上的模块进 `unmatched[]`（不静默丢）；只有一个 run 时 `ok=false` + 可读 error；`fpgaLsp/reports?section=summary` 的现有输出**一字不变**（回归）。 |
| 风险 | 两次 run 之间用户改了模块名 → 大量 `unmatched`，diff 看起来"全是新模块" → 必须在输出头部给出 `matched/unmatched` 比例，并把 `unmatched` 单独成节。 |

### B7 面积/资源热点 → 源码（含黑盒标记）

| 项 | 内容 |
| --- | --- |
| 现状基础 | `AreaReport.modules[]` 有 `level/path/resources/utilization`（`area.h:57-65`），且**不假设固定资源名**（`area.h:7-34` 说明 EF2/EG4/PH1 三种表头差异）；`NormAreaResource`/`AreaTotal` 已做归一（`area.h:86-90`）。 |
| 缺口 | 报告里只有层次字符串 `a/b/c`，没有到 `.v` 行的映射；没有"top-N 热点"接口；黑盒（CWC/IP，`area.h:60` 注释"module 常是空串"）没有区分标记。 |
| 方案 | 复用 B2 的"层次路径 → 实例行"半边：把 `AreaModule.path` 逐级匹配 `Instance.name`（`hdl_index.h:110-128`）；输出 `fpgaLsp/areaHotspots{topN}` = `[{path,instanceFile,line,resources,utilization,blackbox}]`；`blackbox` 判定：`index.ModuleByName(module)` 为空 或 `project.IpFiles()` 命中。 |
| 工作量 | **3 人日**（依赖 B2；纯匹配逻辑） |
| 验收 | 面积 top-10 里 ≥ 8 个能跳到实例化行；黑盒项 `line=null` 且 `blackbox=true`；`.area` 换器件族（EF2→PH1）后仍不崩（沿用现有容错）。 |

### B8 `.al` 工程一致性诊断 + IP/黑盒可见性（P0，便宜且救命）

| 项 | 内容 |
| --- | --- |
| 现状基础 | `Project.warnings`（`al_project.h:90`）、`AlFile.exists/InBuild()`（`:66-72`）、`ResolvedVia{Project,Declared,Absolute,Missing}`（`:49-54`）、`PathMatches()`（`:109`）全都有；`td_diagnostics::MakePathSpaceDiagnostics` 已能把"路径含空格"变成诊断（`src/td/td_diagnostics.h:90`）；`hdl::IpGeneratedHdlFor` 已识别 IP 生成 HDL（`hdl_index.h:387`）。 |
| 缺口 | **一条都没发布到 LSP**：grep `warnings` 在 `lsp_server.cpp` 只出现在 `:93`（`ProjectSummaryOf` 里的**条数**统计）。结果：打开 `.al` 的 Problems 面板永远是空的，`.al` 里写着 8 个源文件而磁盘只有 7 个也毫无提示。 |
| 方案 | 把 `.al` 作为第 4 个诊断来源（与 B1 的 `diag_constraint_` 同一机制），检查：文件缺失（`exists=false`，error，range 指向 `Path="..."` 行）、`declared_path` 与目录不一致（`PathMatches()==false`，warning，附 P-7b 的改写建议）、被 exclude 但被例化的模块、IP 的 `.ipc` 缺生成 HDL、路径含空格（复用 `LSP-PATH-SPACE`）。行号用 `Path="` 字面量搜索（`xml_lite` 无节点位置，`xml_lite.h:54`）。 |
| 工作量 | **2 人日**（假设：只做上述 5 类，不改 `.al` 写盘逻辑） |
| 验收 | 删掉一个源文件 → `.al` 上出现 1 条 error 指向该 `<File>` 行；把它移回后消失；含空格的工程出现 `LSP-PATH-SPACE`（与 `fpga_diagnostics` 一致）。 |
| 风险 | TD 改 `.al` 的 XML 排版会让字面量搜索失效 → 宽松正则 + 找不到行号时降级到 `(0,0)`，且**诊断内容本身仍要出**（不能因为定位失败就吞掉）。 |

### B9 设计规则/端口文档生成

| 项 | 内容 |
| --- | --- |
| 现状基础 | 索引能给出模块/端口/参数/信号（`hdl_index.h:130-180`）；ADC 能给"每个端口 → 引脚 + IO 标准"（`adc.h:95-111` 的 `Entry` 含 `port/bit/attrs`）；SDC 能给时钟名（`sdc.h:132` `DefinedClockNames`）；器件/版本在 `.al`（`al_project.h:83-85`）。 |
| 缺口 | 没有"导出"出口：只能靠人读 JSON。 |
| 方案 | `fpgalsp.exportDesignDoc`（executeCommand + MCP 工具）：Markdown = 工程概览（器件/族/速度等级/TD 版本）、模块层次表、端口表（方向/位宽/引脚/IO 标准/是否已约束）、时钟表（名字/周期/来源）、约束覆盖率、资源用量（有 area 时）。默认写 `<project>/.fpgalsp/docs/<top>.md`（不污染用户目录），`--allow-write` 保护。 |
| 工作量 | **3 人日**（模板 + 数据装配 2、测试 1） |
| 验收 | 示例工程生成 30–80 行 md；端口表行数与索引 `ports.size()` 严格相等；两次生成字节一致（工程未变时）。 |

### B10 testbench 骨架生成（自带 Verible 自检）

| 项 | 内容 |
| --- | --- |
| 现状基础 | `ide::BuildInstantiationText` 能按端口生成例化文本（`ide_features.h:40-45`，与 Python `_instantiation_text` 逐字节一致，已有单测）；`Module.ports` 带 `msb/lsb/width`（`hdl_index.h:76-86`）；内嵌 Verible 能格式化 + lint（`src/verible_bridge/format_text.h`、`lint_text.h`）；时钟周期能从 SDC 读（`sdc.h:132`）。 |
| 缺口 | 没有任何 tb 生成；也没有"生成后自检"的闭环（现有 codeAction 的 Verible 路径只做 autofix，`features_code_actions.cpp:18-21`）。 |
| 方案 | `fpgalsp.generateTestbench{module?, clockPort?, resetPort?}` → 生成 `tb_<top>.sv`：`timescale`、时钟（周期取 `create_clock -period`，缺省 10 ns）、复位（端口名启发式 `rst|reset|nrst|arst`，极性按名字）、DUT 例化（复用 `BuildInstantiationText`）、未接输入置 0、输出接 `wire`；生成后跑 `verible_bridge::FormatVerilog` + `LintText`，**有 error 则不落盘**并返回诊断。默认只返回文本（MCP），写盘需 `--allow-write` + 显式 `path`。 |
| 工作量 | **4 人日**（生成 2、自检闭环 1、测试 1） |
| 验收 | 对 3 个真实顶层端口表生成 tb：Verible lint 0 error、端口数与位宽与索引完全一致、时钟周期与 SDC 一致；DUT 是纯组合逻辑时也能生成（无时钟分支）。 |
| 风险 | 复位/时钟命名启发式不通用 → 做成参数（`clockPort`/`resetPort`/`polarity`），MCP 侧只返回候选让 agent 选，不猜。 |

### B11 多工程 / 多 `.al` 的工作区语义

| 项 | 内容 |
| --- | --- |
| 现状基础 | `DiscoverProjects`（递归扫 `*.al`，深度 6、上限 500，`al_project.h:149-150`）+ `Affinity` 亲和度选工程（`:152-158`）；`fpgalsp.projects`/`setProject` 已存在（`commands.cpp:703-762`）；`ProjectFromParams` 支持按 `params.uri` 选工程（`lsp_server.h:302-303`，`commands.cpp:231-236` 已用）。 |
| 缺口 | LSP 侧只有一个 `root_uri_`（见 A1）；没有"列出工作区所有工程并让用户切换"的编辑器内入口；`workspaceSymbol` 只在单一工程里搜（`FeatureWorkspaceSymbols` → `CurrentProjectShared`，`hdl_handlers.cpp:104-110`）。 |
| 方案 | 依赖 A1 的多根状态；(1) `fpgaLsp/projects` 自定义方法返回 `[{al,name,top,device,files}]`；(2) `workspaceSymbol` 增 `allProjects:true` 参数（跨根合并，结果带 `project` 字段）；(3) 插件侧用现有 tree view 做工程切换（`vscode-extension/src/moduleTree.js:201` 的 `applyIndexChanged` 已按事件定向刷新，不需要新机制）。 |
| 工作量 | **3 人日**（A1 之后追加；假设不做 per-folder 配置隔离） |
| 验收 | 两个根各有 `.al`：`fpgaLsp/projects` 返回 2 条；`workspaceSymbol{allProjects:true}` 能同时命中两个工程的模块；切换工程后 `moduleTree` 立刻反映（推 `indexChanged`）。 |

### B12 TD 版本兼容矩阵

| 项 | 内容 |
| --- | --- |
| 现状基础 | `.al` 记录 `TD_Version`（`al_project.h:85`）；时序报告 metadata 有 `Created by : Tang Dynasty Release_2026.2`（`timing.h:10`）；`td_install.h` 负责定位本机 TD；命令表是 864 项扁表（`sdc.h:148-155`，`data/td_commands.inc` 880 行）。 |
| 缺口 | 报告/工程来自别的 TD 版本时**零提示**；补全会把当前 TD 不支持的命令也推荐出来（命令表无版本维度）。 |
| 方案 | (1) 报告解析后在 `warnings` 里比对 `metadata["Created by"]` 与本机 TD 版本，产出 1 条 info（不 error，避免误伤）；(2) 给命令表加 `since`/`families` 列（人工标注 PH1/PH2/CPLD 差异），`sdc::Completion` 按 `project.family` 过滤（`flow::StrategyFileForFamily` 已有 family→文件映射，`strategy.h:31` 可复用其分类口径）。 |
| 工作量 | **3 人日**（版本解析 1、命令表标注 1.5、过滤 0.5） |
| 验收 | 用一份 2025.x 生成的报告：出 1 条 info "report generated by TD Release_2025.1, current 2026.2"；CPLD 工程补全里不出现 PH2-only 命令。 |
| 风险 | 版本号格式不统一（`Release_2026.2` / `2026.2` / `r2026.2`）→ 宽松正则；命令表标注是人工活，可能滞后于新 TD。 |

### B13 波形（VCD）与源码联动 MVP

| 项 | 内容 |
| --- | --- |
| 现状基础 | **没有实现代码**：`src/` 里 grep `vcd`/`fst` 无实现命中（只命中 `sdc.cpp:84` 的 SDC 选项 `-waveform` 与注释里的阶段名 "Wave 3"）。仅有索引侧的信号表（`Signal{name,msb,lsb,width,line}`，`hdl_index.h:98-108`）。 |
| 缺口 | 波形与源码完全没有连接；用户要在 GTKWave 里手工对名字。 |
| 方案 | MVP 只做"信号名表"：解析 VCD header（`$scope`/`$var`/`$upscope`/`$enddefinitions`）建"层次作用域 → 信号名/位宽"表（**只读 header 段，不读数据段**，100 MB 文件也能 < 1 s）；与索引信号表做模糊对齐；`hover` 里追加"VCD: tb/dut/u_core.state[7:0]"；`fpgaLsp/waveSignals` 返回表供插件/agent 使用。**不做**波形渲染（GTKWave 已装，重复造轮子无收益）。 |
| 工作量 | **8 人日**（VCD 解析 3、层次对齐 3、UX 2） |
| 验收 | 100 MB VCD 解析 < 1 s（只读 header）；hover 命中率 ≥ 90%（对一份 Verilator 生成的 fst→vcd 转档）；文件缺失/损坏返回空表不崩。 |
| 风险 | 价值/成本比最差的一条；且本机已有 GTKWave + Verilator 流程（`verilator2-cmake-sim` skill）。**建议只在有明确用户需求时启动**，不进前三迭代。 |

---

## 4. 建议的 3 个迭代

| 迭代 | 主题 | 条目 | 人日 |
| --- | --- | --- | --- |
| **迭代 1** | 「让已有能力在编辑器里可见 + 止血」 | A2(0.5)、A1(1.5)、A3(2.5)、B8(2)、B1(4) | **10.5** |
| **迭代 2** | 「时序闭环：报告 ↔ 源码」+ 编辑体验补齐 | B2(5)、A6(3)、B3(3)、A4(2.5)、A5(2) | **15.5** |
| **迭代 3** | 「质量归因：CDC / 约束 / run 对比」 | B4(6)、B5(5)、B6(6) | **17** |
| 待办池 | 其余 | A7、A8、A9、A10、A11、A12、B7、B9、B10、B11、B12、B13 | 34 |

**迭代内的顺序约束**：
- 迭代 1 里 A2 必须先做（0.5 人日，让 25 条命令对第三方可见），否则 A1/A3 的验证要靠扩展的自定义通道，看不出真实体验。
- A3 依赖 A1 的 root 状态改造，但**只依赖字段改造**，可与 A1 同分支串行提交。
- B1 与 B8 共用"新增诊断来源"的机制（`diag_constraint_` / `diag_al_` + 三方合并 + LintService 式去抖），先 B1 再 B8 能复用 ~60% 代码；**不要并行做**（同一个合并函数，必然冲突）。
- 迭代 2 里 B2 是 B3/B4/B7/B13 的前置；A6 的"报告版 lens"也吃 B2 的映射。

**迭代 2 的取舍说明**：把 A6（codeLens）排在 B3 之前，是因为 lens 用纯索引就能出效果（0.5 天内可见），可以先给用户正反馈，而 B3 需要 B2 的映射完全稳定。

---

## 5. 如果只能做 3 件事

**B1（约束诊断进 Problems）→ B2（时序端点 → 源码）→ A3（`willRenameFiles` 保工程一致）**

| 选择 | 理由 |
| --- | --- |
| **B1**（4 人日） | 成本最低、感知最强：三套分析器**已经写完并通过单测**（`tests/unit/adc_test.cpp`、`tests/unit/sdc_test.cpp`），现在只有 MCP agent 能看到。接进 Problems 面板 = 把已完成投资变现，且不需要新协议、不需要客户端改动。同一个机制还能顺带解决 B8（`.al` 诊断）。 |
| **B2**（5 人日） | 唯一能把 TD 最有价值的数据（时序报告）与源码连起来的能力。当前 `openTimingPoint` 是**已存在的死链接**（`extension.js:1303-1313` 判断 `target.file` 为空就弹提示），用户每点一次都在提醒"这功能没做完"。做完后 B3/B4/B7 都变成薄封装（各 3–6 人日 → 2–3 人日）。 |
| **A3**（2.5 人日） | 唯一的**数据破坏性**问题：改名 → `.al` 不更新 → TD 少喂文件 → "0 error"假绿（与 `doc/status.md:67` 的 P-1 同族）。11.5 人日换三类问题（可见性、可用性、正确性）里最不可接受的一类。 |

**明确不选**：
- **B6（run 差异归因，6 人日）**：本机连一份真实 `*_pr.db` 都没有（`doc/status.md:58-59`），无法验收；等有真机产物再做。
- **A4/A6（signatureHelp/codeLens）**：锦上添花，且都不解决"看不到"和"会丢数据"，排在 B1/B2 之后。
- **B13（波形，8 人日）**：本机已有 GTKWave + Verilator 流程，重复投入收益最低。

---

## 6. 验证与门禁（这些提议怎么证明没破坏现状）

现有基线：`ctest` 58/58（`unit` 40 · `e2e` 3 · `formatgate` 4 · `lintgate` 3 · `gate` 2 · `generator` 1 ·
`mcp` 1 · `td` 3 · `bench` 1，见 `doc/status.md:8`）。每条提议的落地方式必须落在下表的某一格里：

| 提议 | 主要门禁 | 关键断言 |
| --- | --- | --- |
| A1 / B11 | `e2e`（新增 `lsp_multiboot`） | 两个根各一个 `.al`，`workspace/symbol` 命中数 = 两个工程模块数之和；重启后仍一致 |
| A2 | `e2e` | `initialize` 的 `executeCommandProvider.commands` 与 `commands.cpp` 的 if 链集合**相等**（脚本比对，不是人工核对） |
| A3 / B8 | `unit`（`al_project_test`）+ `e2e` | 改名后 `ParseAl().files[i].exists==true`；`.al` 诊断条数与"缺失文件数"相等 |
| B1 | `unit`（`adc_test`/`sdc_test` 复用）+ `e2e` | LSP 诊断条数 == `fpga_constraint_audit` 的 `issues` 条数（同源断言）；正常工程 0 条 |
| B2 | `unit`（`reports_timing_test` 扩）+ `e2e` | fixture 上 `begin_end_resolved.resolved/total ≥ 0.6`；`resolved=false` 的项必须带 `reason` |
| B4 | `unit`（`analysis_test` 扩） | CDC cell → `(file,line)` 映射率；不支持 CDC 的器件只出 1 条 info |
| B6 | `unit`（`reports_aggregate_test` 扩） | 两个 run fixture 的 diff **字节确定**；`matched+unmatched == 全部模块数` |
| A6 / A12 | `bench` | codeLens 首屏 < 300 ms；补全响应字节数下降 ≥ 50% |
| A9 | `e2e` | pull 与 push 的诊断集合逐条一致；`resultId` 相同返回 `unchanged` |

**四条不可越过的红线**（来自现有审计结论）：

1. **JSON 形状只许 additive**：`fpgaLsp/reports`、`fpgaLsp/moduleTree`、25 条 executeCommand 的返回形状有
   golden 快照对比（`doc/audit/2026-11-18/golden-final/COMPARE.md`）。新增字段必须是**可选键**；
   已有键的名字/类型/单位一律不动（尤其 `flow_status.h:23-24` 里"WNS/TNS 是 ps 整数、timing.summary 是 ns 浮点"这个坑）。
2. **诊断合并必须复用同一把尺**：B1/B8 新增来源后，排序键与去重规则仍走
   `MergeDiagnosticSources`（`lsp_server.cpp:1111-1167`，键 = line→character→severity→source→code→message）。
   三张表**永不互相写**（`lsp_server.cpp:1109` 的既有约定）。
3. **写操作一律受 `--allow-write` 保护**：B8/B9/B10/A3 所有落盘路径必须先检查开关（现状参考 `ide_features.h:18-20`
   的原子写 + 回滚）。MCP 只读模式是产品默认（`doc/status.md:17`）。
4. **never-throw**：`constraints/`、`reports/`、`analysis/` 三个模块的公开函数不抛异常是硬契约
   （`adc.h:24-27`、`timing.h:175`、`analysis.h:20-21`）；新增的 `audit` 层（B5）必须同样 never-throw。

---

## 7. 未纳入本文的建议（避免重复）

| 项 | 原因 |
| --- | --- |
| `shutdown` / `exit` 生命周期 | 已在传输循环层正确实现（`src/app/main.cpp:184-203`：`shutdown` 回 `null`、`exit` 不回包），不是缺口。 |
| `$/cancelRequest` | 已由 `AsyncDispatcher` 处理（`src/server/async_dispatcher.cpp:125-166`，LSP 模式启用见 `main.cpp:302-306`）。 |
| `textDocument/rangeFormatting` | **已真实实现**（`src/server/features_call_hierarchy.cpp:360-383`：整篇 `FormatVerilog` + 按行 diff）；顺带一条卫生问题：`lsp_server.h:208-218` 的注释仍写"rangeFormatting 仍返回 []"，**已过时**，建议删改。 |
| MCP 工具层 | 39 个工具全部真实实现（`doc/status.md:17`），本文只在"需要新出口"时新增（B6 的 `fpga_report_diff`）。 |
| TD 诊断 / lint 的合并机制 | 已实现（`MergeDiagnosticSources`，`lsp_server.cpp:1111-1167`），B1/B8 是**扩展**它而不是重写。 |