# 端到端优化与功能路线（总览）

> 回答两个问题：
> 1. 既然不再需要兼容 Python 版接口，除了 LSP 必须保持标准合规，**其余协议与后端还能做哪些端到端优化**？
> 2. **插件与后端还有哪些功能值得开发**？
>
> 三份支撑文档（本文只做取舍与排序，细节看它们）：
> - [protocol-optimization.md](protocol-optimization.md) —— 32 条协议/传输/后端链路优化
> - [backend-features.md](backend-features.md) —— 25 条后端功能缺口（12 合规 + 13 领域）
> - [extension-features.md](extension-features.md) —— 28 条插件短板与新功能
>
> 证据等级沿用仓库约定：`实测` = 本机当前 exe 探针测量；`审计` = 引用 `doc/audit/2026-11-19/` 的已有实测；`代码` = 读代码可证；`待测` = 可见但未量化。**没有数字的地方不编数字。**

---

## 0. 三句话结论

| # | 判断 | 依据（最硬的一条） |
| --- | --- | --- |
| 1 | **真正的浪费不在算法，在"协议契约"**：同一份数据发两遍、通知只发提示让客户端全量重取、长任务串行阻塞、超限整份丢弃、本机监听无鉴权。 | 同一 `fpga_project_structure` 响应 **79,892 B，其中 64% 是重复表示**（实测）；`reportsChanged` 为算一个 80 B 通知去建 56 MB 全量快照（代码 + 审计 768–784 ms）。 |
| 2 | **后端"有数据没出口"**：ADC/SDC 分析器、时序端点、`.al` 警告、DRC/CDC 解析都已实现，缺的是接进 Problems / 跳转 / MCP 形状的最后一公里。功能开发的高 ROI 是**接线**，不是新造分析器。 | `.adc/.sdc` 三套分析器只有 MCP 能看见，LSP Problems 一条都不出（`lsp_server.cpp:1215-1236` 只合并 lint+TD）；`timing.cpp:439-452` 恒 `resolved=false` → 插件的"时序跳源码"是死链接。 |
| 3 | **插件缺的不是功能，是"确信感"**：服务器崩了不重启、Reports 树不自动刷新、错误提示是原始 JSON、时序跳转是死代码、默认不启用 MCP 动态提供者。 | `extension.js` 无 `onDidChangeState` 恢复逻辑；`reportsChanged` 只刷 webview 不刷树（`extension.js:1391-1400`）；`timingPointCell` 定义在 `reportsHtml.js:94-103` 但**无调用点**。 |

一句话给决策者：**先做"减法"（协议层去掉重复与全量，约 4 人日，收益可直接用字节/毫秒验收），再把已有分析器接进编辑器（约 9 人日，产品价值最大），最后才考虑新领域功能。**

---

## 1. 端到端链路：现在一次操作到底发生了什么

### 1.1 保存一个 `.v` 文件 → 模块树刷新

```
VS Code 保存
  → provider 的 150 ms 合并窗口（F6/R1 之后只剩这一层；审计的 309–315 ms 是修前口径，现约 150–160 ms）
  → fpgaLsp/moduleTree（服务端 1.5–2.4 ms，回 16,426 B 全树）
  → 客户端整棵树丢弃重建
```

四处可省：① 150 ms 合并窗口是纯等待（服务端只占 <1%）——事件本身已带 `uri`，可按文件粒度把窗口压到 ≤50 ms，或直接消费 `indexChanged` 的推送结果；② 服务端**从不发** `indexChanged.modules[]`，而客户端**早已实现**原地打补丁（`moduleTree.js:221-255`）；③ `moduleTree` 不在重请求白名单，与 `didChange` 抢同一个 IO 线程；④ 一次保存回传 16 KB 全树，只为改一个节点。

### 1.2 一次 agent 工具调用 → 返回

```
agent tools/call
  → MCP 读循环（同步，单连接，一次只服务一个客户端）
  → Bridge::Call → handler（可能 BuildReports 768 ms / TD 进程 / 全索引扫描）
  → WrapToolResult：content[0].text（dump(1) 缩进）+ structuredContent（同一份数据再来一遍）
```

五处可省：① 双份表示（实测 −58%~−64% 字节）；② `flow_run` 在飞时 `ping` 也排队；③ 第二个客户端**根本连不上**（`accept` 在 `Serve` 返回前不会被调用）；④ 不支持取消（LSP 侧有 `$/cancelRequest`，MCP 侧没有）；⑤ 超过 800 KB **整份丢弃**，只回一句 `truncated`。

### 1.3 一次报告更新 → 报告面板

```
flow 写新报告 / 用户覆盖 .rpt
  → didChangeWatchedFiles（IO 线程）
  → ReportStore::InvalidatePaths != 0
  → SendReportsChanged：为了填 sections，Get(kAll) → 冷构建 768–784 ms（审计）
  → 只发 {stage, sections, generatedAt}（~80 B）→ 客户端再全量取一次
  → MCP 侧同一条报告路径完全没有 ReportStore 缓存：每次 BuildReports
```

**这是全仓最典型的"端到端浪费"**：一次全量构建换来一个提示，然后客户端再触发一次全量。

---

## 2. 端到端优化：按 ROI 排序

### 2.1 第一批：纯减法（不改契约，可直接用字节/毫秒验收）

> **状态（2026-11-19）**：以下 4 项 + `P-C6`/`B-A2` 已落地，实测
> `project_structure` **38,393 → 16,168 B（−58%）**、`workspace_symbols` **−57%**、
> 保存回传 **16,426 B → ≤400 B**，`ctest` 59/59。详见 [../status.md](../status.md)。

| 编号 | 动作 | 现在 → 之后 | 成本 | 出处 |
| --- | --- | --- | --- | --- |
| **D9** | `reportsChanged` 不再 `Get(kAll)`，改用"失效文件→段名"映射 + revision | IO 线程阻塞 **768–784 ms → ≤5 ms** | 0.5 人日 | protocol §D9 |
| **B1** | 工具结果二选一：`structuredContent`（新客户端）或紧凑 `text`（旧客户端），按 `initialize` 版本协商 | 响应 **79,892 → ≈28.5 KB（−64%）**；默认参数 38,393 → ≤16,000 B（实测） | 1 人日 | protocol §B1 |
| **C1** | `indexChanged` 带 `modules[]`（客户端已支持补丁） | 一次保存回传 **16,426 B → ≤400 B**，且 moduleTree 请求数 → 0 | 1.5 人日 | protocol §C1 |
| **D1** | MCP 报告路径接 `ReportStore`（现在每次 `BuildReports`） | 冷 **768–784 ms → 命中 0.54 ms**（审计） | 1 人日 | protocol §D1 |

合计 **4 人日**，四项互不依赖、都只动单个文件。这是"还能做哪些端到端优化"里性价比最高的答案。

> **为什么不是"换二进制 / 加压缩"**：LSP 与 MCP 的正文都被规范锁死为 JSON 文本，换编码要两端同时改且 VS Code 客户端不支持；而现在的浪费是"同一份数据发两遍 + 通知后全量重取"，压缩只会把重复数据压小、仍然发两遍。**先减重复，再谈编码**（详见 protocol §9"明确不做"）。

### 2.2 第二批：推送与增量（消灭"全量重取"这个模式）

| 编号 | 动作 | 收益 | 成本 |
| --- | --- | --- | --- |
| **C3** | `reportsChanged` 带 `revision`；`fpgaLsp/reports` 支持 `ifRevision` | 一次 flow 结束的 4 次 section 重取 → 1 次；命中路径 461–489 ms 只付一次（审计） | 2.5 人日 |
| **C2** | `moduleTree` 懒加载 / 扁平化（现在一次建整棵树，500 子节点上限会静默截断） | 大工程从"一次几十 KB + 截断"变成"按需一屏" | 3 人日 |
| **C4** | `flowStatus` 不再每次带 200 行日志并扫 `runs/` 目录（日志走 `flowEvent` 增量 + `since` 游标） | 轮询载荷与 CPU 降到常数级；兜底轮询可再降频 | 1.5 人日 |
| **D2** | 报告装饰项（`NewestRoutedDb` + `FindTd` + `analysis.json`）每请求重做 | `FindTd` 0.89–0.93 ms/次；占 `section=summary` 约 27% | 1 人日 |
| **D5** | 语义高亮 `delta` 是"假增量"（把新数组整个塞进 delta，比 full 还贵） | 大文件每次按键少传一整份 token 数组 | 0.5–2 人日 |

### 2.3 第三批：并发、安全与可观测（不做的话多 agent 场景不成立）

| 编号 | 动作 | 现状证据 | 成本 |
| --- | --- | --- | --- |
| **A1** | MCP 支持多客户端并发（thread-per-connection + 共享 Engine） | `mcp_tcp.cpp:164-173` 串行 `accept→Serve→closesocket`；一次只有一个客户端 | 3–4 人日 |
| **A2** | 长工具异步 + `notifications/cancelled` | `main.cpp:174-182` 同步；MCP 侧无取消令牌 | 2 人日 |
| **A3** | 端点广告（状态文件写实际端口）+ 随机令牌鉴权 | 端口用工作区 hash 推导（`extension.js:66-72`，45000+hash%1000），无鉴权 → 本机任意进程可读工程文件、可跑 flow（`--allow-write/--allow-flow` 打开时） | 2–3 人日 |
| **B2** | 800 KB 整份丢弃 → 分页/游标 | `mcp_server.h:72`、`WrapToolResult` 超限即全丢 | 3 人日 |
| **A4** | 可观测性：MCP 侧暴露 `fpgalsp.metrics` 同款计数（LSP 已有） | `GetAsyncMetrics` 只服务 LSP；MCP 无任何计数 | 1 人日 |

### 2.4 结构性建议：让"一个引擎"真的只有一个

现在的形态是"一个 exe、两种协议、**可能两个进程**"：

- 扩展在 LSP 进程里开 loopback MCP（`--mcp-port`），MCP 客户端通过 `--attach-port` 字节泵接进来 → **共享同一个 LSP Engine**（好）。
- 但这条路需要 `fpgaLsp.mcp.dynamicProvider=true` 且端口探测成功（`extension.js:352-357`）；否则回退"独立 MCP 进程" → **索引、报告缓存、TD 会话全部复制一份**（`审计` 每连接重建 Bridge +9–11 ms，内存增量未测）。
- 建议把 attach 变成默认路径（探测失败才回退），并让 MCP 定义由扩展自动注册（MCP provider 已实现，只是默认关）；收益是"agent 看到的工程状态 = 编辑器看到的工程状态"，避免两套缓存各自失效。**这是零代码风险的策略调整，先于任何新功能。**

---

## 3. 功能开发：先接线，再造轮子

### 3.1 后端：三项"接线"就能兑现的价值（P0）

| 编号 | 功能 | 已有基础（复用） | 缺口 | 人日 |
| --- | --- | --- | --- | --- |
| **B1** | `.adc/.sdc` 约束诊断进 Problems | `constraints/adc.cpp`、`sdc.cpp`、`constraint_audit` MCP 工具、单测齐全 | `RunDiagnostics` 只合并 lint+TD（`lsp_server.cpp:1215-1236`，注释还写着"这两个分析器本阶段没有"，已过时） | 4 |
| **B2** | 时序端点 → 源码双向跳转（`openTimingPoint` 真正可用） | `reports/timing.cpp` 已解析 `begin_at/end_at`、`hdl_index` 已有 Instance 链与 `signal_pos` | `timing.cpp:439-452` 恒 `resolved=false`、`reason="hdl-index-unavailable"`（注释同样过时） | 5 |
| **B8/A3** | `.al` 工程一致性 + 改名不断工程 | `project/al_project.cpp` 有 `warnings`、`InBuild()` | `warnings` 从不发布；`willRenameFiles/didRenameFiles` 全缺 → 改名后工程静默少文件（与 `status.md` 的 P-1 同族"0 error 假绿"） | 2 + 2.5 |

### 3.2 后端：合规缺口（按规范严重度）

| 优先级 | 项 | 说明 |
| --- | --- | --- |
| P0 | `workspace/didChangeWorkspaceFolders` 未注册 | 却声明了 `changeNotifications:true`；`dispatcher.cpp:74-85` 对无 handler 的通知**静默丢弃**，多根工作区切换不生效 |
| P0 | `executeCommandProvider` 未声明 | 25 条命令已注册但对第三方客户端不可见 |
| P0 | `willRenameFiles` / `didRenameFiles` | 见上，唯一会**破坏数据**的一类 |
| P1 | `signatureHelp`、`documentLink`、`codeLens` | 例化端口签名、`.al` 的 `<File Path>`、模块实例数/资源透镜 |
| P2 | pull diagnostics、`documentHighlight`、`selectionRange`、`willSave`、`$/setTrace`、`completionItem/resolve`、`semanticTokens/range` | 合计约 12 人日，逐项见 backend-features §2 |

### 3.3 后端：领域差异化（做深，不铺开）

| 编号 | 功能 | 为什么值得做 | 人日 |
| --- | --- | --- | --- |
| **B4** | CDC/DRC 违规定位到 RTL + codeAction | 已有 `td_diagnostics` 的 DRC/CDC 解析，但只给计数；能跳源码才有用 | 6 |
| **B5** | 约束覆盖与冲突审计（未约束 IO/时钟、bank 电压域） | 现成 `adc/sdc` 分析器升级为"体检报告"，是 FPGA 用户最常问的问题 | 5 |
| **B6** | 两次 run 的 QoR/时序差异归因 | `_Runs/phy_1..N` 目录已在枚举；"为什么这次更差"是真实痛点 | 6 |
| **B7** | 面积/资源热点 → 源码 | 复用 `reports/area.cpp` + B2 的映射设施 | 3 |
| **B10** | testbench 骨架生成（自带 Verible 自检） | 内嵌 Verible 可自检，别的插件做不到 | 4 |
| **B3** | 源码 → 时序反查（光标处寄存器属于哪条路径） | B2 的反向，用户调试时的高频动作 | 3 |

**不建议现在做**：波形联动（B13，8 人日，需求未验证）、拥塞热图（后端无 congestion 解析器）、独立"模块层级图"重绘引擎（`moduleTree` 数据够用，缺的是布局，不是数据）。

### 3.4 插件：先修"不可信"，再谈新面板

| 优先级 | 项 | 现状证据 | 人日 |
| --- | --- | --- | --- |
| P0 | 服务器健康：崩溃自动重启 + 可读错误 | 无 `onDidChangeState` 恢复逻辑；错误提示是原始 JSON | 2–3 |
| P0 | Reports 树自动同步 | `reportsChanged` 只刷 webview 不刷树（`extension.js:1391-1400`） | 1 |
| P0 | 时序路径浏览器（点击跳源码） | 前后端都是死代码/死链接（见 B2） | 5–7 |
| P1 | 状态栏：工程 / 器件 / flow / WNS 摘要 | 唯一状态栏项只有 "FPGA: idle"（`flowController.js:77`） | 2 |
| P1 | QoR 跨 run 趋势图 | `reportsService` + `_Runs` 枚举已具备 | 4 |
| P1 | 约束表格化编辑（ADC 引脚表 / SDC） | 复用 `constraint_audit`；现在是纯文本编辑 | 5 |
| P2 | `vscode.lm.registerTool` 把 39 个工具变成 Copilot 工具 | 全仓无 `registerTool`；MCP provider 已注册但默认关闭 | 3 |

另有三处**配置层面的不一致**要顺手修（**2026-11-22 复核：①② 已修，③ 仍成立**）：
① 代码读取了 4 个 `package.json` 未声明的设置（`serverRoot`/`verible.path`/`verible.lintArgs`/`verible.highlight`）—— 这 4 个键现已在 `contributes.configuration` 里声明，`fpgaLsp.verible.autofix`/`verible.highlight` 也已经在服务端被真正消费（0.22.0）；② `flowJobs` 兜底值现在是 6，与 `package.json` 一致；
③ **`fpgaLsp.mcp.allowFlow` 默认 `true`** —— 与服务器 CLI 的"默认只读"相反，用户装上 MCP 配置后 agent 即可启动 TD flow；界面上已有 `agent: flow` 状态栏徽标（S7），但默认值是否该改成 `false` 仍待产品决定（见 [../audit/2026-11-22/SUMMARY.md](../audit/2026-11-22/SUMMARY.md) §四）。

---

## 3.5 落地状态（2026-11-20，backlog 于 2026-11-22 复核）

| 批次 | 状态 |
| --- | --- |
| M1（6 项） | ✅ 全部落地，见 [../status.md](../status.md) §M1 |
| M2（14 项） | ✅ 全部落地（含 P-A3 令牌灰度、P-B2 分页、P-B5 job、P-B6 resources、P-C2 懒加载） |
| M3a（3 项） | ✅ B-A3 改名通知、B-B8 工程一致性 |
| M3b（2 项） | ✅ B-B1 约束诊断、B-B2 时序端点 |
| M3c 后端 | ✅ B-B4（诊断，无 codeAction）、B-B5 覆盖率概览、B-B6 run 差异、B-B7 面积热点、B-B10 testbench |
| M3c 插件 | ✅ S1/S2/S3/S5/S6/S7/S8、X-B1、X-B2、X-B3（只读表）、X-B7（5 个只读 lm 工具） |

**仍留在 backlog（2026-11-22 审计复核 + 0.23.0 收尾后的准确版）**：B-B4 的 codeAction、
X-B3 的表格内编辑、B-B5 的 bank 电压域/交叉表、B-B9/B11/B12/B13、X-B4/B5/B6/B8，以及 §9 里
"明确不做"的那些；插件 UX 只剩"7 个 MCP 工具无人工 UI 入口"（flow 绝对超时已在 0.23.0 补）。
LSP 合规缺口（本文件 §3.2 的 A1–A12）全部关闭。完整清单与证据见
[../audit/2026-11-22/SUMMARY.md](../audit/2026-11-22/SUMMARY.md) §四。
判断标准没变：**先把已经算出来的数据接进协议与 UI，再考虑新领域功能。**

## 4. 建议的排期

| 迭代 | 内容 | 人日 | 验收锚点 |
| --- | --- | --- | --- |
| **1（1–2 周）** | D9 + B1 + C1 + D1（协议减法）；A2 `executeCommandProvider`；B1 约束诊断进 Problems；A3 `willRenameFiles` | ≈11 | 响应字节 −60%；报告变化 IO 阻塞 ≤5 ms；改名后工程不少文件 |
| **2（2–4 周）** | A1 多客户端 + A2 取消 + A3 端点/令牌 + B2 分页；B2 时序端点跳转；B8 `.al` 一致性；插件 S5 崩溃恢复 + S1 状态栏 | ≈22 | 两客户端并发 p95 ≤50 ms；时序点跳转命中率 ≥60%；崩服自动恢复 |
| **3（按需）** | C2 懒加载、C3 revision、D3 分级构建、B4 CDC、B5 覆盖审计、B6 QoR 差异、插件 QoR 趋势/约束表格 | ≈30 | 按单条验收标准逐项过 |

**如果只能做 3 件事**：
1. **协议减法四项（D9/B1/C1/D1，4 人日）** —— 唯一能用"字节数、毫秒数"直接验收的一组，且全是删工作量。
2. **B1 约束诊断进 Problems + B2 时序端点跳源码（9 人日）** —— 把已经写好但用户看不见的分析器接进编辑器，产品价值最大。
3. **A3 改名通知 + B8 工程一致性（4.5 人日）** —— 唯一会静默破坏工程数据的一类缺陷。

理由：前两件决定"这套工具是否好用"，第三件决定"是否可信"。领域新功能（CDC、QoR 归因）都排在它们之后，因为**数据已经在手，接口还没有**。

---

## 5. 文档索引

| 文档 | 内容 | 条目 |
| --- | --- | --- |
| [protocol-optimization.md](protocol-optimization.md) | 传输/进程（A）、MCP 协议层（B）、`fpgaLsp/*` 自定义协议（C）、后端链路（D）；含两张链路图与 W1–W12 浪费点、Top 9、两批落地顺序、"明确不做"表 | 32 |
| [backend-features.md](backend-features.md) | LSP 3.17/3.18 合规缺口（A 12 条，含"声明 vs 注册"逐条对照）+ FPGA 领域功能（B 13 条） | 25 |
| [extension-features.md](extension-features.md) | 现状地图（16 条操作链路）、体验短板（E 12 条）、新功能（S/B 16 条）、agent 时代 5 条战略判断 | 28 |
| [on-chip-probe.md](on-chip-probe.md) | 片上探针（ChipWatcher）能力与限制：配置/抓取/校验链路、cwc 与 ERAM 约束 | — |
| [write-service.md](write-service.md) | **统一写服务（已实现 0.24–0.26）**：`platform::write_service` + 插件 `writeService.js`、四类 root 策略、原子写/审计/dry-run、门禁与迁移清单 | — |

**维护约定**：这些是"设计提案 + 落地状态"，不是"现状描述"；落地后应把对应条目移进 [../status.md](../status.md) 的已完成区，并在本页删掉，避免又一次文档漂移。
