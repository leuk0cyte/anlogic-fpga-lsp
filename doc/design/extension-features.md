# VS Code 扩展功能设计：现状地图与待开发功能

> 对象：`fpga-lsp-anlogic` 0.15.3（`vscode-extension/`）+ 同仓原生后端 `fpgalsp-native.exe`。
> 视角：**产品 / 前端**。后端只作为"数据是否已经存在"的证据来源，不讨论后端内部设计。
> 方法：只读代码与文档，未运行扩展、未构建、未安装依赖。所有结论都给出 `文件:行`。
> 前提（用户已确认）：**不再要求兼容 Python 版协议**，前后端可以一起重构。

---

## 0. 一页结论

| 问题 | 结论 |
| --- | --- |
| 插件现在是"编辑器前端"还是"agent 工具提供者"？ | **两者都半成品**。前端：3 个树视图 + 2 个报告 webview（syn/phy，各 7 个 tab），UI 明显落后于后端数据；agent：39 个 MCP 工具是全项目最成熟的一面，但扩展对它**默认关闭**（`fpgaLsp.mcp.dynamicProvider=false`），用户必须手动装 `.vscode/mcp.json` 才能用。 |
| 最该补的短板是什么？ | 不是缺功能，是**缺"确信感"**：服务器崩了不恢复、Reports 视图不自动刷新、错误提示是原始 JSON、时序点跳转是死代码。 |
| 最值得做的 3 件事 | ① 服务器健康与错误可读（S5+S2）② 时序路径浏览器（B1）③ Reports 自动同步 + 状态栏工程态（S3+S1）。 |
| 最不该现在做的 | Test Explorer、拥塞热图、独立的"模块层级图"重绘引擎——数据链或需求都还没验证。 |
| 数据侧的意外发现 | 后端的"证据链"比前端展示的多：报告分节、`begin_end_resolved` 覆盖率、run 目录枚举、约束审计全都有，**只是没有 LSP 出口或没有 UI**。 |

---

## 1. 现状地图

### 1.1 三层结构

```
扩展进程 (extension.js)
 ├─ LanguageClient ── stdio ──> fpgalsp-native.exe --stdio [--td] [--mcp-port N] [--allow-flow] [--allow-write] [--log]
 │    · 25 个标准 LSP 方法（诊断/补全/定义/引用/重命名/语义高亮/格式化/callHierarchy…）
 │    · 6 个自定义方法：fpgaLsp/moduleTree、reports、flowStatus（请求）
 │                     fpgaLsp/flowEvent、indexChanged、reportsChanged（服务端推送）
 │    · 21 条已封装的 fpgalsp.* 命令（服务端还支持更多，见 doc/commands.md）；扩展侧另贡献 45 条 VS Code 命令
 ├─ 3 个 TreeView + 2 个 webview（syn / phy）
 └─ MCP 定义提供者（默认不注册）
```

证据：`vscode-extension/src/protocol.js:8-57`（方法与命令全表）、`extension.js:266-300`（client 选项）、`doc/status.md:16-17`（25 个 LSP 方法 / 39 个 MCP 工具）。

**六个自定义方法都已真实实现**，不是占位：

| 方法 | 方向 | 服务端实现 | 客户端消费 |
| --- | --- | --- | --- |
| `fpgaLsp/moduleTree` | 请求 | `src/server/lsp_server.cpp:297-298` → HDL 索引 | `src/moduleTree.js:321-359` |
| `fpgaLsp/reports` | 请求 | `src/server/commands.cpp:293-409`（**支持 section 分节**） | `src/reportsService.js:78-110`（单飞 + TTL 1.5 s） |
| `fpgaLsp/flowStatus` | 请求 | `src/server/commands.cpp:415-453`（含 runs 枚举） | `src/flowController.js:342-372`（10 s 兜底） |
| `fpgaLsp/flowEvent` | 推送 | `src/server/lsp_server.cpp:1740-1768`（真实回调点） | `src/flowController.js:388-419` |
| `fpgaLsp/indexChanged` | 推送 | `src/server/lsp_server.cpp:465-474` | `src/moduleTree.js:201-235`（原地补丁） |
| `fpgaLsp/reportsChanged` | 推送 | `src/server/lsp_server.cpp:477-490` | `extension.js:1391-1400`（**只刷面板**） |

> 注：`doc/commands.md:70` 仍写 `flowEvent`"预留、没有调用点"，**与代码不符**（已有调用点）。这类文档漂移会误导后续排期，见 S6。

### 1.2 用户操作 → 命令 → 协议 → UI

| # | 用户操作 | VS Code 命令 | 实际协议 | UI 落点 | 断点 |
| --- | --- | --- | --- | --- | --- |
| 1 | 打开含 `.al` 的文件夹 | —（激活） | — | 无 | 无首启引导；`workspaceContains` 触发全库扫描（`package.json:25`） |
| 2 | 打开任意 Verilog 工程 | —（激活） | 启动服务器 | 状态栏 `FPGA: idle` | 非 Anlogic 工程也付一次原生 exe 启动成本（`package.json:19-24`） |
| 3 | 看模块层次 | 视图 `fpgaLsp.modulesView` | `fpgaLsp/moduleTree` | TreeView | 空态仅一句英文（`src/moduleTree.js:293-297`） |
| 4 | 点模块节点 | `fpgaLsp.openModuleNode` | 纯前端 | 打开定义 | 黑盒/IP/未定义不区分（`src/moduleTree.js:65-76`） |
| 5 | 层次搜索 | `fpgaLsp.searchHierarchy` | `fpgalsp.searchHierarchy` | QuickPick | 只按名字子串，不支持正则/按文件过滤（`extension.js:794-830`） |
| 6 | 加源文件到 `.al` | `fpgaLsp.addSourceFile` | `fpgalsp.addSourceFile` | 文件对话框 + toast | 顺序写、无进度，几十个文件时"卡住了"（`extension.js:754-792`） |
| 7 | 综合 / 布局 / 全流程 | `fpgaLsp.runSyn/runPhy/runAll` | `fpgalsp.runFlow` | 通知进度 + 状态栏 + `FPGA Flow` 输出 | 无百分比（事件无 percent，`src/flow/flow_runner.cpp:436-456`） |
| 8 | 取消 flow | `fpgaLsp.cancelFlow` | `fpgalsp.cancelFlow` | 立即提示已取消 | **本地先 finish**，服务器可能仍在跑 → 再点 run 报 busy（`src/flowController.js:284-295`） |
| 9 | 看报告 | `fpgaLsp.showReports` | `fpgaLsp/reports?section=…` | webview（每页 7 个 tab） | 永远打开 syn（`extension.js:938`） |
| 10 | 点时序路径跳源码 | **无入口** | — | — | 前端死代码 + 后端存根（见 B1） |
| 11 | DRC / CDC | `fpgaLsp.runAnalysis` | `fpgalsp.runAnalysis` | toast + 报告页 | 只有计数，无违例列表/跳转（`extension.js:1228-1243`） |
| 12 | 策略比较 | `fpgaLsp.compareStrategies` | `fpgalsp.compareStrategies` | QuickPick + toast | 结果不留档，下次要重跑（`extension.js:1245-1275`） |
| 13 | 烧录 SRAM/Flash | `fpgaLsp.programSram/programFlash` | `fpgalsp.cables` + `fpgalsp.program` | 两次 modal | 每次从"探测线缆"重来，无板卡档案（`extension.js:1088-1117`） |
| 14 | 查日志 | `fpgaLsp.showLogs` | 无 | 打开 `extension.log` | `server.log` / flow 日志无入口（`extension.js:1316-1323`） |
| 15 | 看整体状态 | — | 无 | 状态栏一段文字 | 无工程/器件/WNS（`src/flowController.js:77-82`） |
| 16 | 给 Copilot 用本扩展 | `fpgaLsp.installMcpConfig` | 写 `.vscode/mcp.json` | 打开生成的 JSON | 需 reload；动态 provider 默认关（`extension.js:409-426`） |

### 1.3 断点分类汇总

| 类别 | 具体断点 | 证据 |
| --- | --- | --- |
| **无反馈** | `readDesign` 只提示"已开始"，没有进度/完成态 | `extension.js:627-644` |
| | `updateProject` / `selectProject` 同步等待 TD，期间无 UI | `extension.js:872-880`、`984-1006` |
| | 分析类命令用 `withProgress(cancellable:false)`，长时间无消息更新 | `extension.js:1062-1071` |
| **要手动刷新** | Reports 树不响应 `reportsChanged` | `extension.js:1391-1400`（只 `reportsPanel.requestRefresh`） |
| | 报告页搜索词在增量 patch 后不重新生效 | `src/reportsHtml.js:798-813`（只在 input 事件里重查 DOM） |
| | 改 `tdPath` / `allowFlow` 后不重启 LSP，只有 MCP emitter 刷新 | `extension.js:427-434` |
| **报错不可读** | `fpgalsp.check` / `fpgalsp.info` 失败时把整包 JSON 打进弹窗 | `extension.js:964`、`970` |
| | 业务错误的 `error` 字段被原样透出（如 `busy`、`unknown error`） | `src/flowController.js:243-255` |
| | 服务器未就绪时纯文本 `fpga-lsp is not running yet` | `extension.js:311`、`317` |

---

## 2. 现有体验短板（12 条）

按"用户多久会撞上一次"排序。每条只写一处最硬的证据。

| # | 短板 | 证据 | 用户感受 |
| --- | --- | --- | --- |
| **E1** | **服务器崩溃不恢复**：`onDidChangeState` 只写日志，没有重启、没有 UI 状态；`serverReady()` 直接把命令挡掉 | `extension.js:303-305`、`133-139` | 树停在旧数据，命令全部报 `not running yet`，只能手动 Reload Window |
| **E2** | **flow 可能永久卡住**：兜底轮询 `catch` 后直接 `return`，没有超时；`active` 不清空就一直 busy | `src/flowController.js:342-352`、`195-200` | "An FPGA flow is already running." 且无法取消，只能重启窗口 |
| **E3** | **取消是假取消**：`requestCancel()` 立刻 `finish(false,'cancelled')`，不等服务器 ack | `src/flowController.js:284-295` | 提示已取消，但 TD 还在跑；再点 run 报 busy |
| **E4** | **激活时机没有项目级门槛**：`onLanguage:*` 五个语言 + `workspaceContains:**/*.al`，没有 `onStartupFinished` | `package.json:16-26`、`extension.js:206` | 打开任何 Verilog 仓库都会拉起一次原生 exe（`cwd` 退回 serverRoot）；大仓库上 `workspaceContains` 的全库 glob 拖慢启动；而"只想先看看环境"（Doctor / TD 探测）时什么都不会发生 |
| **E5** | **多根工作区取错目录**：注释说"优先含 `.al` 的目录"，代码只判断目录存在 | `extension.js:55-63` | 多根时状态目录、MCP root、`cwd` 全落在第一个文件夹上 |
| **E6** | **首启无引导**：`viewsWelcome` 只有两条、内容也只是两三个命令链接；Reports 视图没有任何欢迎态；全项目没有 walkthrough | `package.json:130-139`、`125-127` | 装了扩展、打开工程，Reports 视图一片空白，也没有"下一步做什么" |
| **E7** | **Reports 视图不自动刷新**：`reportsChanged` 只重绘可见面板，不刷树 | `extension.js:1391-1400` | 跑完 flow，报告文件列表还是上一次的 |
| **E8** | **flow 进度不可见**：进度通知只报 message，从不 `report({increment})`；事件本身也没有 percent | `src/flowController.js:380-382`、`src/flow/flow_runner.cpp:436-456` | 长 flow（phy 常 10+ 分钟）只能盯着转圈 |
| **E9** | **错误提示是原始 JSON**：两条老命令直接把 `JSON.stringify(result)` 塞进弹窗 | `extension.js:964`、`970` | 弹窗里出现 `{"ok":false,"error":"..."}` |
| **E10** | **日志无统一入口**：`showLogs` 只开 `extension.log`；`server.log`、`FPGA LSP` 输出通道、`FPGA Flow` 输出通道三处分散 | `extension.js:1316-1323`、`299`、`src/flowController.js:65` | 出问题时不知道该看哪个 |
| **E11** | **设置项含义不清 + 4 个设置根本没声明**：代码读 `serverRoot`、`verible.path`、`verible.lintArgs`、`verible.highlight`，`package.json` 里一个都没有；报错弹窗还让用户去设 `fpgaLsp.serverRoot` | `extension.js:96-99`、`291-296`、`package.json:140-235` | 设置面板搜不到，弹窗指路指向不存在的键 |
| **E12** | **`allow-write` / `allow-flow` 无 UI 提示**：两者只在启动时变成命令行开关，界面上没有任何"agent 现在能改我的工程/能启动 flow"的指示；改设置也不生效 | `extension.js:224-228`、`427-434` | 用户不知道自己刚刚授权了什么，或改完没反应 |

补充（不计入 12 条，但排期时会碰到）：

| 现象 | 证据 |
| --- | --- |
| `fpgaLsp.flowJobs` 代码兜底 2，`package.json` 默认 6，README 也写 6 | `src/flowController.js:38-47`、`package.json:163-169` |
| README 提到的 `FPGA: Verible Status` 命令不存在（45 条里没有） | `vscode-extension/README.md:245` |
| README 说日志默认在 `<workspace>/.fpgalsp/logs/server.log`，实际在系统 temp | `vscode-extension/README.md:190`、`extension.js:207-215` |
| 45 条命令零快捷键；`category` 只在少数命令上有，命令面板里排不到一起 | `package.json:236-457`（无 `keybindings`）、`262`、`271` |

---

## 3. 值得开发的新功能

> **人日口径**：1 名熟悉本仓库与 VS Code API 的工程师；范围 = 编码 + 单元测试 + 手工验收；**不含** C++ 后端大改（需要后端时单独标注并另计）；不含评审/排队。区间下限=顺利，上限=踩到真实 TD 工程的数据形态问题。
> 假设：手边有 ≥1 个可跑 flow 的真实工程（如 `Y:\sic`、`CorpusAlpha`）用于验收。

### 3.1 小改动高收益（S 系列）

#### S1 状态栏：工程 / 器件 / TD / WNS / flow

| 项 | 内容 |
| --- | --- |
| 优先级 | **P0**（性价比最高的一格像素） |
| 现状证据 | 唯一的状态栏项在 `src/flowController.js:77-82`，只有 flow 文案；工程/器件/WNS 只在树里（`src/flowTree.js:226-244`）和报告页里 |
| 做什么 | 两个 item：`[EF3L90 · top] [TD 2026.2] [WNS -0.42ns] [flow idle]`。数据全部现成：`fpgalsp.info`（device/top/td）+ `reports.flowStatus.setup.wns_ps`（`src/reportsHtml.js:369-374` 已读同一字段）。点击分别打开 Select Project / TD Installations / Timing tab / Reports。WNS 为负时用 `statusBarItem.backgroundColor = error` |
| 涉及文件 | `src/flowController.js`（拆出独立 `src/statusBar.js`）、`extension.js:491-497`、`src/reportsService.js`（复用缓存） |
| 人日 | 1.5–2 |
| 验收 | 打开工程 2 s 内出现工程名与器件；跑完一次 phy 后 WNS 自动更新且符号正确；无工程时显示 `FPGA: no project` 且点击进 Select Project；断开服务器时变灰 |
| 风险 | 状态栏是全局单例，flow 与工程两项抢位——先定"最多 2 个 item"，别做成 5 个 |

#### S2 错误呈现统一：从 JSON 到"一句人话 + 一个动作"

| 项 | 内容 |
| --- | --- |
| 优先级 | **P0** |
| 现状证据 | `extension.js:964`、`970` 输出 `JSON.stringify(result)`；`src/flowController.js:243-255` 直接显示 `why`；`src/reportsService.js:117-123` 把 RPC 异常拼进 message |
| 做什么 | 新增 `src/errors.js`：`describe(result)` → `{title, detail, actions[]}`。约定：`error` 是 snake_case 码（`busy`/`no_cable`/`device_mismatch`/`no_routed_db`/`ok:false+unknown command`）时映射中文说明 + 主按钮（重试 / 打开日志 / 选工程 / 安装 MCP）。全项目弹窗都走它，禁止再出现 `JSON.stringify` |
| 涉及文件 | 新增 `src/errors.js`；改 `extension.js`（`register` 的 catch、`openErrors`、`check`/`info`）、`src/flowController.js` |
| 人日 | 1–1.5 |
| 验收 | 全仓 grep 无 `JSON.stringify(result)`；把服务器 exe 改名后点 Run Synthesis，弹窗文案是"语言服务器未启动（点击查看日志）"而不是堆栈；单测覆盖 6 个错误码 |
| 风险 | 错误码目前是自由字符串（`src/flow/programmer.cpp` 多处 `out["error"]=...` 英文整句），映射表会漏；先做"透传但加动作"，不要妄想一次列全 |

#### S3 Reports 视图与 flow 事件自动同步

| 项 | 内容 |
| --- | --- |
| 优先级 | **P0** |
| 现状证据 | `reportsChanged` 处理只 invalidate 缓存 + 刷可见面板（`extension.js:1391-1400`）；`flowProvider.refresh` 只在 flow 收尾时调（`src/flowController.js:430-442`） |
| 做什么 | 在 `reportsChanged` 里追加 `reportsTreeProvider.refresh()`（它已走同一个 `ReportsService`，单飞不会翻倍请求）；`flowEvent` 的 `step`/`done` 也触发一次带 300 ms 去抖的树刷新；顺手修 `src/reportsHtml.js:798-813` 的搜索状态（patch 后把当前 query 重放一遍） |
| 涉及文件 | `extension.js`、`src/reportsTree.js`、`src/reportsHtml.js` |
| 人日 | 0.5–1 |
| 验收 | 跑一次 syn，Reports 视图无需点刷新即出现新的 qor/timing 文件行；flow 运行中树里 stage 行实时变化；报告页输入搜索词后触发一次自动 patch，过滤仍然生效 |
| 风险 | 事件风暴（`reportsChanged` 一次 flow 可能来 N 次）——必须去抖，否则 `reportsTree` 会反复 fire 导致视图闪烁 |

#### S4 首启引导：`viewsWelcome` 补齐 + 一条 walkthrough

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | `viewsWelcome` 只有两条（`package.json:130-139`），Reports 视图没有任何欢迎态；无 `walkthroughs` 贡献 |
| 做什么 | ① Reports 视图加欢迎态（"还没有报告 → Run Synthesis / 打开示例工程"）；② 三视图统一"无工程"态：`Open Folder` + `Select Project…` + `Doctor`；③ 加 4 步 walkthrough（装 TD → 选工程 → 跑 syn → 看报告），用 `command:` 链接 |
| 涉及文件 | `package.json`（`viewsWelcome`/`walkthroughs` + `media/` 图）、`extension.js`（`selectProject` 需可在无工程时打开） |
| 人日 | 1–1.5 |
| 验收 | 新用户打开空窗口点 FPGA 图标：三个视图都有可点动作；walkthrough 四步全部可完成且每步有 `completionEvents` |
| 风险 | walkthrough 的完成条件要靠命令触发，容易写错（`onCommand:` 与 `onView:` 语法区别）；不做图片也能上，别卡在素材 |

#### S5 服务器健康：崩溃可见 + 自动拉起

| 项 | 内容 |
| --- | --- |
| 优先级 | **P0**（这是"扩展不可信"的根因） |
| 现状证据 | `client.onDidChangeState` 只 `diag()`（`extension.js:303-305`）；`serverReady()` 只在刷新路径上被查（`extension.js:1344-1362`）；只有 `start()` 的 catch 有提示（`extension.js:1435-1443`） |
| 做什么 | 状态机 `starting / ready / degraded / failed`：`State.Stopped` 且非主动 dispose 时 → 状态栏红点 + 树头 message "语言服务器已停止（第 n 次）"；指数退避 1/2/5/15 s 自动 `client.restart()`（最多 3 次）；每次失败写 `extension.log` 并给"查看日志 / 重试 / 禁用自动重启"三个动作；新增命令 `fpgaLsp.restartServer`（现在 `fpgaLsp.restart` 只是杀 TD 会话，`extension.js:1324-1327`，名字极易误解） |
| 涉及文件 | `extension.js`（activate 尾部 + 新命令）、`src/protocol.js`、`package.json`（新命令）、`src/moduleTree.js`（`setServerError` 已有，复用） |
| 人日 | 2–3 |
| 验收 | 任务管理器杀掉 `fpgalsp-native.exe`：3 s 内状态栏变红，≤2 s 后自动恢复，树自动重载且不丢展开状态；连续杀 3 次后停止重试并给出可读原因；`FPGA: Restart Language Server` 可手动恢复 |
| 风险 | `vscode-languageclient` 的 stop/start 在 `onDidChangeState` 回调里重入会死锁——必须 `setTimeout(…, 0)` 出栈；另外 TD 会话被杀后旧诊断不会自动清空，要在重启后主动清一次 |

#### S6 配置项对齐与"改了就生效"

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | 代码读 4 个未声明设置（`extension.js:93`、`291-296`）；`fpgaLsp.serverRoot` 出现在错误文案里（`extension.js:96-99`）；`allowFlow/allowWrite/tdPath` 只在 spawn 时进命令行（`extension.js:221-229`），配置监听只 fire MCP emitter（`427-434`） |
| 做什么 | ① 声明 `fpgaLsp.serverRoot`（或删掉这条文案，统一只说 `serverPath`）；② 删掉 `verible.path/lintArgs/highlight` 三个死读取；③ 加"需要重载才生效"的设置分组：改 `tdPath`/`serverPath`/`mcp.allowFlow`/`mcp.allowWrite` 时提示 `Reload Window`（带按钮），MCP 三项则直接 `mcpEmitter.fire()` 热生效 |
| 涉及文件 | `package.json:140-235`、`extension.js:92-101`、`211-236`、`427-434` |
| 人日 | 1–1.5 |
| 验收 | 设置面板搜 `fpgaLsp` 只剩 13 条且每条都能改出效果；改 `tdPath` 弹出"需重载"提示且点按钮能重载；`verible.*` 只留 3 条 |
| 风险 | 改 `package.json` 配置会触发"设置已弃用"警告，老用户 json 里残留键会有黄波浪——需要留 `deprecationMessage` |

#### S7 授权面可见：`allow-flow` / `allow-write` 变成看得见的状态

| 项 | 内容 |
| --- | --- |
| 优先级 | P1（安全相关，越早越好） |
| 现状证据 | 两个布尔直接变命令行开关（`extension.js:224-228`），默认 `allowFlow=true`、`allowWrite=false`（`package.json:214-223`）；界面上没有任何痕迹；`doctor` 里只是两行文本（`extension.js:1193-1194`） |
| 做什么 | ① 状态栏或 Project & Flow 视图顶部加一枚 `MCP: flow/write` 徽标（只有非只读时才显示）；② 首次检测到 MCP 配置文件或 `allowWrite=true` 时弹一次"agent 可以修改你的 `.al`"提示（带"撤销"）；③ `doctor` 里加"当前生效值 vs 设置值 vs 需重启"三列 |
| 涉及文件 | `extension.js`、`src/flowTree.js`（或新 `src/agentStatus.js`）、`package.json` |
| 人日 | 1 |
| 验收 | `allowWrite=true` 后 3 s 内出现可见徽标并可一键恢复 `false`；`allowFlow` 改动给出"需重载"提示；doctor 能指出"设置为 true 但进程参数未带" |
| 风险 | 徽标容易被当成装饰，用户仍然不会读——真要有效，得在"第一次由 agent 触发的 flow"时也弹一次（这需要服务端在事件里带 `origin`，取值 `agent` / `lsp`，见 B7） |

#### S8 多根工作区与工程选择的正确性

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | `workspaceFolder()` 只找第一个"存在的"目录（`extension.js:55-63`），注释与实现不一致；`selectProject` 把结果写进 Workspace 级 `projectFile`（`extension.js:1000-1002`），多根下所有文件夹共用 |
| 做什么 | ① `workspaceFolder()` 改为：优先 `folder` 内含 `.al` → 其次 `projectFile` 前缀匹配 → 最后第一个；② 多根时在 Flow 树顶加"Workspace folder"节点，切换即 `setProject` + 更新 Resource 级设置；③ 状态目录 `stateDir` 按最终选中的根重算（现在在 `activate` 时一次性算好，`extension.js:206-208`） |
| 涉及文件 | `extension.js:55-63`、`206-208`、`984-1006`；`src/stateDir.js`（已支持，纯调用） |
| 人日 | 1.5–2 |
| 验收 | 两文件夹工作区（只有第二个含 `.al`）：MCP root、`cwd`、状态目录全部指向第二个；切换文件夹后模块树/报告同步切换；`projectFile` 写在正确的 scope |
| 风险 | 状态目录一旦中途换根，已经启动的服务器仍用旧目录——换根必须连带重启服务器，别只改 JS 侧变量 |

小计：S 系列 **9–14 人日**。

### 3.2 大功能（B 系列）

#### B1 时序路径浏览器：点一下跳到源码的那一行

| 项 | 内容 |
| --- | --- |
| 优先级 | **P0**（这是"别人做不到"的功能，且离完成只有两步） |
| 现状证据 | 前端：`timingPointCell()` 已写好但**没有任何调用点**（`src/reportsHtml.js:94-103`，导出表 `903-921` 里也没有它），Timing 页只渲染纯文本 `path.begin/end`（`251-255`）；`fpgaLsp.openTimingPoint` 命令已注册但**除注册外零引用**（`extension.js:1303-1314`）。后端：`AttachEndpointLocations()` 是显式存根——`resolved=false`、`reason="hdl-index-unavailable"`，带 `TODO(阶段5 / HDL 索引)`（`src/reports/timing.cpp:439-454`），但端点结构 `begin_at/end_at` 已经序列化（`482-505`）、覆盖率 `begin_end_resolved` 已经统计（`591-593`） |
| 做什么 | ① 后端：`AttachEndpointLocations` 接 HDL 索引（`src/hdl/hdl_index`），按 `hierarchy` 逐级例化名走到模块，再按 `signal` 找声明行；失败时保留 `reason`（`no-top` / `signal-not-found` / `blackbox`）；② 前端：Timing 页把 Begin/End 渲染成 `timingPointCell()`，点击走已有 `command:fpgaLsp.openTimingPoint`；③ 路径列表加列：slack 排序、按 group 折叠、复制路径文本；④ 顶部显示 `begin_end_resolved = 37/48` 的覆盖率（数据已有，别让用户猜为什么点不动） |
| 涉及文件 | `src/reports/timing.cpp`（端点映射）、`src/hdl/`（层次索引）、`vscode-extension/src/reportsHtml.js:212-290`、`extension.js:1303-1314` |
| 人日 | 后端 3–5 + 前端 1.5–2（**共 5–7**） |
| 验收 | 真实工程 phy 报告里 ≥80% 的 Begin/End 可点击；点击后打开正确文件的正确行且高亮；未解析的点显示原因而不是不可点的纯文本；覆盖率数字与实际可点数一致；单测覆盖 `hierarchy`→文件、`signal`→行、blackbox 三种路径 |
| 风险 | ① 时序里的信号名常是 `u_core/u_alu/acc_reg[7]` 这种"网表名"，位选/重命名后索引找不到——先把"部分解析"做出来，别追求 100%；② 大报告（10 MB+）在 webview 里渲染上万条路径会卡，必须默认只渲染 top-N（服务端 `limit` 参数已存在，`src/server/commands.cpp:326-336`） |

#### B2 QoR 跨 run 趋势

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | run 目录能枚举（`src/reports/artifacts.cpp:198-224`、`flowStatus.artifacts.runs`），但每次只解析"最新"一套；策略比较结果只在 toast 里念一遍（`extension.js:1270-1274`），不留档；报告页无任何图表（全文件只有 CSS 条形，`src/reportsHtml.js:778-782`） |
| 做什么 | ① 新增 `fpgaLsp/history`（或 `reports?section=history`）：扫 `<proj>_Runs/*/` 下的 `*.qor` / `*_phy.area` / `flow_status`，输出 `[{run, strategy, wns_ps, tns_ps, luts, slices, elapsed, ok}]`，落一份 `<stateDir>/history.json` 增量缓存（按 mtime）；② webview 新增 "Trend" tab：WNS/TNS/LUT 折线（内联 SVG，CSP 已允许 style nonce，注意不要 `unsafe-inline`）；③ 策略比较结果写进同一份历史，"Compare" 页改成表格 + 最优标记 |
| 涉及文件 | `src/reports/*`、`src/server/commands.cpp`、新增 `src/server/features_history.cpp`、`vscode-extension/src/reportsHtml.js`、`extension.js` |
| 人日 | 后端 2–3 + 前端 2–3（**共 4–6**） |
| 验收 | 同一工程连跑 3 次 syn（不同策略）后 Trend 页出现 3 个点且 X 轴是 run 名/时间；点某点跳到该 run 的报告；无 `.qor` 的 run 显示为空心点并在 tooltip 说明原因；新增 run 不重扫全历史（第二次打开 <300 ms） |
| 风险 | ① 旧 run 的报告格式可能与当前解析器不兼容（历史工程常见），解析失败必须降级为"缺数据"而不是抛错；② `best_result` 目录要排除（`src/reports/artifacts.cpp:220` 已这么做，注意别在历史里又抓进来） |

#### B3 约束表格化编辑器（复用 `constraint_audit`）

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | 审计逻辑完整：ADC 覆盖/冲突/器件引脚检查全在 `src/mcp/bridge.cpp:2343-2432`，**但只有 MCP 出口**；LSP 侧只有一个符号入口 `FeatureConstraintSymbols`（`src/server/constraints_features.h:5`），扩展里没有任何约束 UI（45 条命令中无 constraint 相关） |
| 做什么 | ① 后端加 `fpgalsp.auditConstraints`（直接复用 `Bridge::ConstraintAudit` 的实现，抽成 `LspServer::FeatureConstraintAudit`，别复制一份）；② 新视图或新 webview：表格列 = 端口 / 方向 / 已分配引脚 / IOSTANDARD / 状态（ok / 未约束 / 冲突 / 不可用 IO / 专用引脚），支持按状态筛选；③ 行内动作：跳到 `.adc` 对应行、插入 `IOSTANDARD` 片段、把"未约束端口"批量生成 `.adc` 草稿（写文件前必须先 diff 预览） |
| 涉及文件 | `src/server/features_constraints.cpp`、`src/mcp/bridge.{h,cpp}`（抽函数）、`vscode-extension/src/protocol.js`、新增 `src/constraintPanel.js` + `src/constraintsHtml.js`、`package.json` |
| 人日 | 后端 1.5–2 + 前端 3–4（**共 5–6**） |
| 验收 | 打开工程看到全部 top 端口及约束状态；未约束数 = `constraint_audit` 的 `coverage` 数字（同一实现，必须一致）；双击行跳到 `.adc` 精确行；"生成草稿"必须先出 diff，取消后文件不变；对 `.adc` 的写操作在 Workspace Trust 未授予时禁用 |
| 风险 | ① 引脚分配可能有多种合法写法（`LOCATION`/`PACKAGE_PIN`），生成的草稿要跟现有文件风格一致，否则用户会拒用；② 表格与 `.adc` 文本双向同步是最容易出 bug 的地方——**先只做单向（文本→表格）+ 跳转，不先做表格→文本** |

#### B4 Flow 时间线与失败证据链（不是"又一个报告页"）

| 项 | 内容 |
| --- | --- |
| 优先级 | P1 |
| 现状证据 | 失败时只有一句 message + Problems 里若干 `fpga-flow` 诊断（`src/flowController.js:297-321`、`src/flowDiagnostics.js:18-48`）；`flowStatus.flow.log` 保留 200 行（`src/flow/flow_runner.cpp:443-447`）但 UI 不展示（树里只显示最后一条，`src/flowTree.js:289-294`）；`run.log` 路径解析存在但只在错误文案里出现 |
| 做什么 | ① 报告页新增 "Flow" 时间线：每个 stage/step 一行（状态、wall、cpu、mem、输出摘要），点行展开该段 TD 输出（取 `flowStatus.flow.log` + `run.log` 尾部）；② 失败时给"证据卡片"：失败步骤 / `.error.f` 路径 / `run.log` 是否存在 / 最后 3 条 ERROR / 一键打开目录；③ 与 Problems 双向：点诊断跳时间线对应行 |
| 涉及文件 | `vscode-extension/src/reportsHtml.js`（flow tab 已有一半，`:341-390`）、`src/flowTree.js`、`extension.js`、`src/flow/*`（可能需要 step 级 wall/cpu） |
| 人日 | 2.5–3.5 |
| 验收 | 故意制造一次失败（改坏一个 `.v`）：失败卡片能指出哪个 step、run.log 在不在、最后一条 ERROR 原文；时间线里每个 step 可展开且内容与 `FPGA Flow` 输出一致；点击诊断能定位 |
| 风险 | `PRJ-8806` 这类"TD 没写 run.log"的失败天生缺证据（`vscode-extension/README.md:268` 已记录），卡片必须能优雅表达"没有日志"，而不是显示空块 |

#### B5 模块层级图（webview / Mermaid）

| 项 | 内容 |
| --- | --- |
| 优先级 | P2 |
| 现状证据 | 数据已足够：`fpgaLsp/moduleTree` 返回 `module/instance/children/portCount/defined/usedAt/recursive/truncated`，节点预算 5000（`doc/commands.md:72-84`）；现有表现只有 TreeView（`src/moduleTree.js:418-453`） |
| 做什么 | 命令 `FPGA: Show Module Graph`：把当前树（可限定深度/根节点）转成 Mermaid `graph TD`，在 webview 里渲染；节点点击 → `fpgaLsp.openModuleNode`；黑盒/递归用不同样式；导出 `.mmd` / `.svg` |
| 涉及文件 | 新增 `src/moduleGraph.js` + `src/moduleGraphPanel.js`、`package.json`、`extension.js` |
| 人日 | 3–4 |
| 验收 | 20 模块工程图 1 s 内渲染完；点节点跳定义；`truncated` 节点有可见标记；5000 节点工程给出"图太大，已限制到 top 300"的提示而不是卡死 |
| 风险 | ① Mermaid 需要打包进 webview（现在扩展**没有** webpack，`doc/extension-development.md:32-35` 明确要求不要动 node_modules 打包方式）——引第三方库会破坏现有打包假设，建议**手写 SVG 布局**（分层/树形算法 300 行内）；② 大图布局算法调优是无底洞，先做"按深度限制" |

#### B6 Task / ProblemMatcher：让 TD 在 Tasks 与终端里可用

| 项 | 内容 |
| --- | --- |
| 优先级 | P2（但成本最低的"生态位"扩展） |
| 现状证据 | `package.json` 无 `taskDefinitions`、无 `problemMatchers`（全文 grep 只有 `viewsWelcome`）；flow 只能从命令/视图按钮启动 |
| 做什么 | ① `taskDefinitions` 提供 `fpga: syn` / `fpga: phy` / `fpga: program`：Task 启动一次性 `fpgalsp-native.exe --mcp` 进程并调用 `fpga_flow_run`（或新增 `--task` 入口）；② `problemMatchers` 解析 TD 的 `FILE(LINE): ERROR/WARNING` 格式（正则可直接借鉴 `src/flowDiagnostics.js:10-11`）；③ 提供默认 `tasks.json` 片段（不写用户文件，只做 `Run Task` 模板） |
| 涉及文件 | `package.json`、`extension.js`（`registerTaskProvider`）、可能新增后端 `--task` 入口 |
| 人日 | 2–3（如果后端加 CLI 入口，+1.5） |
| 验收 | `Tasks: Run Task → fpga: syn` 能跑完并在 Problems 里出 TD 错误（不依赖扩展 UI 打开）；CI 里用同一命令跑通 |
| 风险 | 后端目前只有 `--stdio` / `--mcp` 两种模式（`doc/protocol.md:6-21`），做 Task 等于加第三种入口——**先确认这是不是想要的重复**（flow 已经能从 MCP 触发，或许 Task 只需壳子调 MCP） |

#### B7 `vscode.lm.registerTool`：把 39 个 MCP 工具变成 Copilot 原生工具

| 项 | 内容 |
| --- | --- |
| 优先级 | **P1**（战略优先级高于多数 UI） |
| 现状证据 | 全仓 `registerTool` **零命中**（只有 `vscode.lm.registerMcpServerDefinitionProvider`，`extension.js:359-405`）；MCP 能力很强（39 工具，`doc/status.md:17`；含 `constraint_audit` / `flow_wait` / `flow_issues`），但动态 provider 默认关闭（`package.json:229-233`），用户必须手动装 `.vscode/mcp.json` 再 reload |
| 做什么 | ① 选 6–8 个"只读 + 高价值"工具（`project_info`、`module_info`、`constraint_audit`、`flow_status`、`reports`、`search_hierarchy`）用 `vscode.lm.registerTool` 注册成 `fpga_*` 工具（`inputSchema` 可直接从 `src/mcp/tools.cpp` 的 schema 复用）；② 工具实现走**已在跑的 LSP 连接**（不需要另起进程），因此不依赖 MCP 配置；③ `prepareInvocation` 里给出可读确认文案（"将在 <project> 上运行约束审计"）；④ 写/flow 类工具仍走 MCP + `allowWrite`，不进 lm 工具 |
| 涉及文件 | 新增 `src/lmTools.js`、`extension.js`、`package.json`（`engines.vscode` 下限 + 可选的工具门控声明） |
| 人日 | 2–3 |
| 验收 | 在 Copilot Chat 里问"这个工程有几个未约束的 IO"，模型能调用 `fpga_constraint_audit` 并给出数字；工具调用不依赖任何 `.vscode/mcp.json`；未启用的工具不出现在工具列表 |
| 风险 | ① `languageModelTools` 需要 `engines.vscode` ≥ 1.95 左右（当前 `^1.80`），要抬版本并重新验证旧版行为；② Copilot 的工具选择是概率性的，别指望"必定调用"，验收用"至少一次成功"而不是"每次都成功" |

#### B8 烧录向导：从"点两次确认"到"板卡档案 + 一键复现"

| 项 | 内容 |
| --- | --- |
| 优先级 | P2 |
| 现状证据 | 后端三条硬规则已实现（`confirm:true` 必传、无线缆直接拒、器件不匹配即使 `allowMismatch` 也拒，`doc/commands.md:37`），SRAM/Flash/SVF 都通（`src/flow/programmer.cpp:742-836`）；前端每次从"探测线缆"重来，两次 modal（`extension.js:1088-1117`），没有"上次烧的是哪块板/哪个 bit"的记忆 |
| 做什么 | ① 板卡档案：`<stateDir>/boards.json` 记住 cable index + 期望器件 + bit 路径，下次一键预选；② 向导式 QuickPick：选工程 → 选产物（bit/svf，列出 mtime/size）→ 选模式 → 预检结果一屏展示 → 一次确认；③ 烧录后把结果（时间/器件/文件 hash）写入历史，供 B2 复用；④ 危险操作（Flash）保留 modal，SRAM 降为确认条 |
| 涉及文件 | `extension.js:1088-1117`、新增 `src/programWizard.js`、`src/stateDir.js` |
| 人日 | 2–3 |
| 验收 | 第二次烧录用 3 次点击完成；器件不匹配时仍强制 modal 且文案包含在线器件型号与工程目标；历史里能看到上次烧录的 bit 路径与时间 |
| 风险 | 板卡档案会"过期"（换板子），预选错器件的代价很高——预检结果必须显式显示"线缆上实际是什么"，并默认不信任缓存 |

小计：B 系列 **21–32 人日**（B1 占 5–7）。

### 3.3 优先级矩阵

| | 影响大 | 影响小 |
| --- | --- | --- |
| **成本低（≤2 人日）** | **S1 状态栏**、**S2 错误可读**、**S3 报告同步**、**S7 授权可见** | S6 配置对齐、S4 引导（略超 1 人日但收益稳） |
| **成本高** | **B1 时序跳转**、**B7 lm 工具**、B3 约束编辑器 | B5 层级图、B6 Task、B8 烧录向导、B2 趋势 |

---

## 4. AI / agent 时代：5 条战略判断

### 4.1 判断一：短期"编辑器前端"，中期"工具提供者"，但前端不能省

- 依据：MCP 已经是这个项目最完整的一面（39 工具、0 占位，`doc/status.md:17`），而前端只有 3 树 + 1 面板，且 Trees 的信息密度低于报告页。
- 结论：**前端不再是"功能入口"，而是"证据展示面"**。用户需要亲眼看到 agent 得出的结论（WNS、违例、约束缺口），否则不会信任一次自动 flow。
- 推论：报告/审计/时序这三类**读**接口，前端与 agent 必须共用同一份实现（`doc/commands.md:173-182` 已经把这条写成铁律），扩展侧也应共用同一个 `ReportsService` / 同一个 `FeatureConstraintAudit`。

### 4.2 判断二：护城河不是 UI，是"TD 的确定性事实"

- 依据：报告解析（`src/reports/*`）、约束审计（`src/mcp/bridge.cpp:2343`）、HDL 索引（`src/hdl`）这些是**别人拿不到**的东西；TreeView/webview 任何人都能写。
- 结论：把预算优先投在"把 TD 的黑箱变成结构化事实"上（时序点映射、约束缺口、失败证据），而不是投在图表美观度上。
- 反面案例：B5 模块层级图很"好看"，但它只是已有索引的另一种画法，**不产生新事实**——所以排 P2。

### 4.3 判断三：VS Code 原生工具面（`lm.registerTool`）比动态 MCP provider 更值

- 依据：动态 provider 默认是关的，理由写在设置描述里——"Copilot 无法把动态 server 关联到 workspace"（`package.json:232`）；而 `registerTool` 不依赖 workspace 关联，也不需要用户改 JSON、reload。
- 结论：MCP 保留给 Cline/Claude 等外部 agent；VS Code 内的 Copilot 走 lm 工具。两条腿，但**优先把 lm 这条打通**（B7）。
- 风险：`engines.vscode` 要抬到支持 lm 工具的版本，等于放弃 1.80–1.9x 用户——用 `when` 能力检测降级，不要硬性抬。

### 4.4 判断四：agent 能"动手"之后，UI 的第一职责是"让人看得见授权"

- 依据：`allowFlow` 默认 `true`（agent 可以启动 TD flow），界面上零提示（E12）。
- 结论：在 agent 时代，"确认对话框"是不够的，需要**持续可见的状态**（S7）+ 事件里带 `origin`（谁触发的 flow）。后端 `flowEvent` 已经有 `runId/type/stage`，加 `origin: "lsp"` / `"mcp"` 是极小改动，但它决定了时间线能否区分"我点的"和"Copilot 点的"。
- 这条也解释了为什么 S7 排 P1 而不是 P2。

### 4.5 判断五：宁可少做 UI，也要让"一次操作可复现"

- 依据：策略比较结果只在 toast 里（`extension.js:1270-1274`），烧录不记历史（`extension.js:1088-1117`），报告页只有"当前"没有"上次"。
- 结论：agent 时代的价值在于"同样的输入能得到同样的结论"。凡是产生判断的操作（比较策略、跑 flow、烧录、审计），都应落一条可机读记录（B2 的 history、B8 的烧录历史），这既是 UI 的数据源，也是 agent 的记忆。
- 推论：`<stateDir>` 已有统一目录（`src/stateDir.js:43-51`），加 `history.json` 的成本极低。

---

## 5. 迭代排期建议

### 迭代 1（1.5 周）：先让它"可信"

| 项 | 人日 | 交付物 |
| --- | --- | --- |
| S5 服务器崩溃恢复 + `restartServer` | 2–3 | 状态机 + 退避重启 + 状态栏红点 |
| S2 错误可读 | 1–1.5 | `src/errors.js` + 全量替换 |
| S3 报告/树自动同步 | 0.5–1 | 事件驱动刷新 |
| S1 状态栏工程/器件/WNS | 1.5–2 | 2 个状态栏 item |
| S6 配置对齐 | 1–1.5 | 13 条设置 + 重载提示 |

**出口标准**：杀掉服务器能自愈；任何失败弹窗都有一句人话 + 一个动作；跑完 flow 后三个视图都不需要手动刷新。
**风险**：S5 与 S1 都要动 `extension.js` 的 activate 尾部，排前后顺序避免冲突（S5 先）。

### 迭代 2（2.5 周）：把"别人做不到"的那件事做出来

| 项 | 人日 | 交付物 |
| --- | --- | --- |
| B1 时序点 → 源码（后端索引映射） | 3–5 | `resolved=true` 的 begin/end + 覆盖率 |
| B1 前端（点击、排序、覆盖率、top-N） | 1.5–2 | Timing 页可点 |
| B4 Flow 时间线 + 失败证据卡 | 2.5–3.5 | 失败时能自证 |

**出口标准**：真实工程里 ≥80% 时序端点可跳；失败 flow 的证据卡能替代"翻 run.log"。
**风险**：B1 后端依赖 HDL 索引的层次还原能力，**第 3 天就要用真实报告验证命中率**，命中率 <50% 就立刻降级为"只做 top 路径 + 明确原因"，不要挤占 B4。

### 迭代 3（3 周）：把"能力"变成"产品面"

| 项 | 人日 | 交付物 |
| --- | --- | --- |
| B7 lm 工具（6–8 个只读工具） | 2–3 | Copilot 内可调用 |
| B3 约束表格（单向：文本→表格 + 跳转） | 3–4 | 约束缺口一屏看完 |
| S4 首启引导 / S7 授权可见 / S8 多根 | 3–4 | 新人 5 分钟上手 |
| B2 QoR 趋势（可选，视进度） | 4–6 | 跨 run 折线 |

**出口标准**：Copilot 能回答"有几个未约束 IO"并给出与 UI 一致的数字；新用户不看 README 也能跑通一次 syn。

### "只做 3 件事"的取舍

如果只能做 3 件，选 **S5（服务器健康）+ B1（时序跳转）+ S1/S3（状态与同步，算一件"感知层"）**，理由：

| 候选 | 选/不选 | 理由 |
| --- | --- | --- |
| S5 服务器健康 | **选** | 一切体验的地基。服务器不恢复时，后面所有功能都退化成"重启窗口"。 |
| B1 时序跳转 | **选** | 唯一"只有这个插件能做"的功能；数据已到位 90%，投入产出比最高。 |
| S1 状态栏 + S3 自动同步 | **选** | 二者合起来解决"我不知道现在什么状态"。都是 ≤1 人日级的改动，却决定日常体感。 |
| B7 lm 工具 | 不选（第 4 位） | 战略上重要，但它的价值取决于用户是否已经在用 Copilot 做 FPGA 工作——**先问一句"你的团队现在用 Copilot 改 HDL 吗"**，答案是否就先缓。 |
| B3 约束编辑器 | 不选 | 价值高但风险最高（写回 `.adc` 的正确性），且必须先补一个后端 LSP 出口。 |
| B5 层级图 / B6 Task / B8 烧录向导 | 不选 | 都是"已有能力的另一种呈现"，边际价值低于前三条。 |

---

## 6. 附录

### A. 证据核验方式（红线自查）

| 结论 | 核验方式 | 结果 |
| --- | --- | --- |
| 没有 `registerTool` | `Select-String -Path extension.js,src\*.js -Pattern 'registerTool\|vscode\.lm'` | 仅 3 处命中，全是 `registerMcpServerDefinitionProvider` 相关（`extension.js:360`、`369`、`1151`） |
| 有 `registerMcpServerDefinitionProvider` | 同上 | `extension.js:369`，id 为 `fpgaLsp.mcp`，与 `package.json:726-731` 的贡献点对应 |
| 只有 1 个状态栏项 | `Select-String -Pattern 'createStatusBarItem'` | 仅 `src/flowController.js:77` |
| 零快捷键 | `package.json` 全文搜 `keybindings` | 无此字段 |
| 无 Workspace Trust 处理 | 搜 `isTrusted` / `Trust` | 0 命中 |
| 无 taskDefinitions / problemMatchers / walkthroughs | `package.json` 搜三个键 | 均无 |
| 4 个未声明设置 | 逐个比对代码 `get('...')` 与 `configuration.properties` | `serverRoot`、`verible.path`、`verible.lintArgs`、`verible.highlight` 未声明 |
| 45 条命令全部注册 | `register(VSCodeCommands.` 计数 | 45 处，与 `package.json:236-457` 数量一致 |
| 时序跳转无调用点 | 搜 `openTimingPoint` / `timingPointCell` | 只有定义与注册，无消费方 |
| 后端时序端点是存根 | 读 `src/reports/timing.cpp:439-467` | `resolved=false`、`reason="hdl-index-unavailable"`、`TODO(阶段5 / HDL 索引)` |

### B. MCP provider 到底注册了什么（澄清 `mcpProviderRegistered`）

```
mcpProviderRegistered (extension.js:50, 396, 400, 417, 423)
  └─ 仅当 fpgaLsp.mcp.attachOnStart !== false（默认 true）
       且 fpgaLsp.mcp.dynamicProvider === true（默认 false）时才为 true
  └─ 注册内容：vscode.lm.registerMcpServerDefinitionProvider('fpgaLsp.mcp', {...})
       provideMcpServerDefinitions() → [new vscode.McpStdioServerDefinition({
         label: 'Anlogic FPGA LSP',
         command: <fpgalsp-native.exe 绝对路径>,
         args: 端口可达时 ['--mcp','--attach-port',N]
               否则 ['--mcp','--root',<ws>,'--project',<al>,'--td',…,'--log',…,('--allow-flow'),('--allow-write')],
         cwd: <workspace>,
         env: { FPGALSP_STATE_DIR: <stateDir> },
         version: <扩展版本> })])
  └─ 端口 N = 45000 + FNV-1a(workspaceRoot) % 1000；LSP 以 --mcp-port N 启动，
     于是 provider 用 --attach-port 复用同一个进程里的 engine（不再起第二个后端）
```

证据：`extension.js:66-90`（端口派生与 300 ms 探测）、`221-228`（LSP 侧 `--mcp-port/--allow-*`）、`335-405`（定义与注册）、`409-426`（开关）、`src/mcpConfig.js:22-36`（`--attach-port` 参数形状）、`package.json:726-731`（贡献点声明）。

默认配置下的实际状态：**不注册**，`mcpProviderError = 'disabled by fpgaLsp.mcp.dynamicProvider…'`，`doctor` 会照实打印（`extension.js:1190-1191`）。用户要走的路是 `FPGA: Install MCP Configuration` → 写 `.vscode/mcp.json` → reload。

### C. 不确定项 / 未验证

| # | 不确定项 | 影响 | 如何验证 |
| --- | --- | --- | --- |
| 1 | 所有 UI 结论来自读码，**没有一帧截图**。`doc/status.md:60-61` 也记录了"GUI 渲染未由人眼确认" | 布局类判断（如 7 个 tab 是否过多）可能偏 | 用 `code --extensionDevelopmentPath` 打开一个真实工程，逐视图截图 |
| 2 | 时序点映射的真实命中率未知 | 直接决定 B1 是 5 人日还是 15 人日 | 拿一份真实 `*_phy.timing` 抽取 50 个 Begin/End，手工比对能否在 HDL 里定位 |
| 3 | 拥塞热图的数据源未确认：`src/reports/` 下没有 congestion 解析器 | 该功能是否可行完全未知，故**本文件不推荐** | 检查 TD 是否产出 congestion 报告（如 `*_pr.congestion`），有则可估 4–6 人日 |
| 4 | Test Explorer 需要"testbench 语义"（枚举/运行/断言），当前后端无任何 test 概念 | 工作量不可估（可能 >15 人日） | 先看用户是否真的在 VS Code 里跑仿真；若用 ModelSim/Vivado 流程则价值低 |
| 5 | 人日估算没有历史速度基线（仓库无迭代记录） | 估算区间可能整体偏乐观 20–30% | 用迭代 1 的实际耗时校准迭代 2/3 |
| 6 | 真实 DRC/CDC 端到端未验证（`doc/status.md:58-59`：缺 `*_pr.db` 语料） | B4 的"违例列表"可能拿不到数据 | 用一块能跑完 P&R 的板子生成 `phy_1/*_pr.db` 后复跑 `runAnalysis` |
| 7 | `fpgaLsp.flowJobs` 默认值三处不一致（2 / 6 / 6） | 只影响文档一致性 | 统一为 6，或让 `readFlowJobs()` 不带兜底默认 |
| 8 | `languageModelTools` 所需的最低 VS Code 版本未在本机核实 | B7 的工作量随版本要求变化 | 查 `engines.vscode` 与 lm 工具 API 的引入版本 |

### D. 与既有文档的关系（避免重复建议）

| 文档 | 已覆盖 | 本文不重复 |
| --- | --- | --- |
| `doc/extension-development.md` | 打包、调试、`node_modules` 不能排除、版本号同步 | 打包/发布相关建议 |
| `doc/status.md` | 构建与测试基线、MCP 39 工具、25 个 LSP 方法、已知差异与未验证项 | 后端等价性、Python 差异 |
| `doc/protocol.md` | 入口与传输、消息上限、日志纪律 | 协议层设计 |
| `vscode-extension/README.md` | 用户向功能与设置表（含 3 处漂移，见 §2 补充） | 功能说明书 |