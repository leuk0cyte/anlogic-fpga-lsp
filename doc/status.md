# 交付状态（当前：2026-11-23，扩展 0.31.0）

## 构建与测试

| 项目 | 数据 |
| --- | --- |
| **全新目录干净构建** | configure 29 s → build 3.3 min → **844 个目标全部成功**（Release + 全量 LTO）；无 LTO 的 `windows-msvc-dev` 全新目录 **56 s**（2026-11-20 实测；增量构建见 build.md） |
| **`ctest` 全量** | **72/72 通过**（其中 `unit` 46、`e2e` 5、`td` 5，其余为 `formatgate`/`lintgate`/`gate`/`generator`/`mcp`/`docs`/`parity` 门禁 + `docs_clang_tidy`）；**clang-tidy 门禁默认注册**（约 +145 s；跳过用 `ctest -E docs_clang_tidy`，约 90 s）|
| **原生产物** | `build/bin/fpgalsp-native.exe`（0.31.0，**7142400 B**，全量 LTCG；SHA256 `7A1094F0…1C292D`） |
| **单元测试规模** | 46 个 gtest target |

> 构建/测试的并行度分析（为什么 192 线程帮不上忙、时间精确花在哪、日常该用哪个 preset）见
> [doc/build.md](build.md) 的「并行度：192 线程用在了哪里」。

## 功能面

| 项目 | 数据 |
| --- | --- |
| LSP | **47 个注册 handler**（标准 LSP 3.18 + `fpgaLsp/*` 自定义 + `workspace/executeCommand`；后者实现 **34 条**命令、广播 **31 条**；可选能力 A4–A12 已全部声明：signatureHelp/documentLink/codeLens/documentHighlight/selectionRange/pull diagnostics/willSave/`$/setTrace`/completionItem.resolve/semanticTokens.range） |
| MCP | **50 个工具全部真实实现（0 占位）**（Python 对齐 39 + 11 个扩展；`--allow-flow` 可见 46、只读 38、`--allow-write` 再加 3）；服务端默认只读，flow / 写操作需显式开启；4 个只读 resources + `fpga_guide` 7 topic + 4 个 prompt |
| VS Code 扩展 | **58 条命令**（声明 = 注册 = 实现）、4 个视图、5 个 `vscode.lm` 工具、18 个设置项（含 flow 绝对超时） |
| TD 集成 | 会话与诊断、Flow（含 6 步单步 Run / Reset and Run）+ BitWizard、报告解析、ADC + SDC 符号、HDL 索引、ChipWatcher 采样 |

## M1 协议层减法（已落地，2026-11-19）

> 计划见 [doc/design/README.md](design/README.md) §3；本节只记实测结果。

| 项 | 改动 | 实测 |
| --- | --- | --- |
| P-B1 | 工具结果只发一份表示：默认紧凑 JSON 文本；声明 `experimental.structuredContent` 才走结构化档 | `fpga_project_structure` **38,393 → 16,168 B（−58%）**；`workspace_symbols` 111,545 → 47,795 B（−57%）；`project_files` 12,847 → 6,039 B（−53%） |
| P-D9 | `reportsChanged` 不再 `Get(kAll)`，改按文件名推段名 + `revision` | IO 线程阻塞 768–784 ms（冷构建）→ µs 级；不再为 80 B 通知建 56 MB 快照 |
| P-C1 | `indexChanged` 带 `modules[{module,portCount,defined}]` / `full`（客户端早已支持补丁） | 一次保存回传 16,426 B 全树 → ≤400 B，且 moduleTree 请求数 = 0（契约对齐 `moduleTree.js:221-255`） |
| P-D1 | MCP 报告路径接 `ReportStore`（附着模式借用宿主实例） | 不再每次 `BuildReports`（冷 768–784 ms）；第二次起命中快照 |
| P-C6 | `fpgaLsp/moduleTree`、`semanticTokens/full(/delta)` 进 worker 白名单 | 冷索引时不再与 `didChange` 抢 IO 线程 |
| B-A2 | 声明 `executeCommandProvider`（广播 22 条；`check/info/restart` 因扩展已 contributes 而排除） | 第三方客户端可见；`unit.commands` 断言表/能力一致 |

回归：`ctest` **59/59**（原 58 + 新增 `unit.commands`）。

## M2 增量与并发（已落地，2026-11-19/20）

> 计划见 [doc/design/README.md](design/README.md) §2.2–§2.3。M1 见上一节。

| 项 | 改动 | 验收 |
| --- | --- | --- |
| P-A1 | MCP loopback 每连接一个会话线程（`FPGALSP_MCP_CLIENTS`，默认 8）；超限 `-32002`；Shutdown 唤醒并 join | A 卡在半行时 B 仍能 `ping`（实测 <1 ms）；`e2e.mcp_concurrency` |
| P-B4 | `tools/call` 交给内置 worker；`notifications/cancelled` → 协作式取消；TCP 路径补上 `SetNotificationWriter` + 每会话写锁 | `flow_wait`/`search_text` 可取消，响应 `isError: cancelled` |
| P-C3 | `reports` 响应带 revision，请求可带 `ifRevision` → `{unchanged:true}` | 一次 flow 结束只真取一次；插件复用缓存对象 |
| P-C4 | `flowStatus` 支持 `sinceSeq`（`logSeq`/`logStartSeq`，落后时 truncated 标记） | 轮询只回新增行；插件合并成 200 行窗口 |
| P-D7 | flow `done`/`error` 回调里 `CheckForChanges` → `reportsChanged` | 报告落盘 1 s 内有通知 |
| P-D5 | 语义高亮真 delta（公共前后缀，5 元组对齐） | 单 token 改动只传 1 条编辑 |
| P-D2 | `FindTd` 5 s 缓存（负结果也缓存） | summary 请求少付 0.9 ms |
| P-B2 | >800 KB 改成分页裁剪 + `nextOffset`/`total`/`itemsField`；`workspace_symbols` 支持 `offset` | 两页拼接 == 全量前 6 项（单测） |
| P-A3 | 端点文件 `mcp-endpoint-<port>.json`（真实端口 + 随机 token + pid）+ TCP 鉴权（两版本灰度）；attach 代理自动注入 token | `e2e.mcp_auth` 10 条断言；token 不进日志 |
| P-B5 | `flow_run` 返回 `job`（= runId 别名）；新增 `fpga_job_status`/`fpga_job_cancel` | 空闲时 `found:false`（单测） |
| P-B6 | `resources/list` + `resources/read`（工程信息/文件/报告摘要/flow） | 未知 uri → `-32602` |
| P-C2 | `fpgaLsp/moduleChildren`（一层 + 分页 + `hasChildren`），客户端展开时懒加载 | 一层/翻页/未知模块（单测）；展开只请求一次（插件用例） |

回归：`ctest` **61/61**；插件 `node test/run.js` **36/36**。

## M3 功能开发（已落地，2026-11-20）

> 计划见 [doc/design/README.md](design/README.md) §3。M1/M2 见上面两节。

| 波次 | 项 | 结果 |
| --- | --- | --- |
| 数据完整性 | B-A3 改名通知 | 声明 `workspace.fileOperations`；`willRenameFiles` 精确改写 `.al` 路径；`didRenameFiles` 失效缓存→重解析→重建索引→重发诊断 |
| | B-B8 工程一致性 | 缺文件 `AL-MISSING-FILE`(error) + 解析告警 `AL-WARNING`(warning)，发布到 `.al` |
| 接线 | B-B1 约束诊断进 Problems | `ConstraintDiagnostics` 成为第三诊断来源（与 MCP 同源端口表） |
| | B-B2 时序端点→源码 | `ResolveTimingEndpoints`（实例链下钻 + 失败 reason 分级），插件端点可点击 |
| 领域 | B-B5 约束覆盖（第一批） | 未约束 IO 给一条 `ADC-COVERAGE`(info)；bank 电压域/交叉表仍缺（需要 device 引脚表接线） |
| | B-B6 两次 run 差异 | `fpgalsp.compareRuns` + MCP `fpga_run_diff`（方向判定 + 汇总） |
| | B-B7 面积热点→源码 | `ResolveAreaModules` + 面积表 Source 列可点 |
| | B-B10 testbench 骨架 | `fpgalsp.testbench` + MCP `fpga_testbench`（含 `ParseVerilog` 自检） |
| | B-B4 CDC/DRC 进 Problems | `ResolveHdlPath` 把 CDC cell 钉到信号声明（`CDC-UNSAFE`），DRC 给 `.al` 上计数摘要 |
| 插件 | S5/S3/S1 | 崩溃自动重启（3 次退避）、Reports 树随 `reportsChanged` 刷新、状态栏「项目·器件·WNS·flow」 |
| | S6/S7/S2/S8 | 补齐未声明设置、`agent: flow/write` 徽标、人话错误 + Show Logs、多根工作区按 `.al` 选目录 |
| | X-B1/X-B2/X-B3/X-B7 | 时序端点链接、QoR 跨 run 趋势面板、引脚约束表（只读+跳行）、`lm.registerTool` 5 个只读工具 |

**明确推迟（写在计划里但本轮没做）**：
B-B4 的 codeAction（一键加同步器/false_path —— "生成哪种修复"交给用户更稳妥）；
X-B3 的表格内编辑（保持在编辑器里改，避免不可撤销写文件）；
B-B5 的 bank 电压域/交叉表审计；以及计划 §8 的 backlog（B-B11 多工程语义、B-B12 TD 版本矩阵、
B-B13 波形联动、X-B4/B5/B6/B8 等）。

回归：`ctest` **61/61**、插件 `node test/run.js` **41/41**。

## 审计 §0.2 第 1–6 条的修复（详见 [doc/audit/2026-11-19/static/FIXES.md](audit/2026-11-19/static/FIXES.md)）

| 编号 | 问题 | 位置 |
| --- | --- | --- |
| SEC-01（High） | `.al` 的 `<Device>` 裸拼进 TD 的 Tcl → 任意命令执行（上轮修复的漏网点） | `server/commands.cpp` |
| SEC-02（Medium） | `ResolveFile` 缺包含校验 → `fpga_format` 可读任意文件；另堵掉 `FormatTool` 两条绕过路径 | `mcp/bridge.cpp` |
| SEC-03/04/05 | SDC 无界递归、`analysis.cpp` 五处裸 `{...}`、MCP TCP 接收缓冲无上限 | `constraints/sdc.cpp`、`analysis/analysis.cpp`、`app/mcp_tcp.cpp` |
| C1/C2（性能 P0） | 每请求两份 `Project` 深拷贝（读路径 31 处调用点）、报告快照整份深拷贝 | `server/*`、`server/commands.cpp` |
| 现代化 | 4 处 `snprintf` 定长缓冲 → `std::format`（截断会切断 UTF-8 多字节序列） | `constraints/sdc.cpp`、`flow/flow_runner.cpp` |

第 7 条（二进制加固：CFG / CET / `/Qspectre` / `/sdl`）按要求**维持现状**：
`DllCharacteristics` 仍为 `0x8160`（ASLR / DEP / HighEntropyVA / 终端服务感知 开，CFG 关）。

## VS Code 扩展（0.15.3）

```
vscode-extension/fpga-lsp-anlogic-0.15.3.vsix   228 files / 2.97 MB
  └─ extension/server/bin/fpgalsp-native.exe    6,352,384 B（全量 LTCG）
```

打包：`npm run package`（= `node scripts/bundle-server.js && npx --yes @vscode/vsce@4 package …`）。
已核对包内 exe 与 `build/bin` 哈希一致（`010156E7…`）。
0.15.0 起随包附带 `LICENSE.txt`（AGPL-3.0-or-later），原先被 `--skip-license` 略过。

### 排版 / lint 默认值（0.15.1 修正）

历史 bug：重写把排版风格在服务端**写死**成 Verible 默认（`indentation_spaces=2`），
同时 `veribleArgs` / `veribleLintArgs` 只被存进 `Settings`、**从未被使用**；
而插件 `package.json` 的默认值一直是旧 Python 版的 160 列 / **4 空格** / 对齐组。
结果：格式化一次就把整份工程从 4 空格重排成 2 空格（实测 `link_rx.v` 15 KB：
241 行增 / 251 行删）。

| 项 | 旧 Python 插件（生效值） | 0.15.0 | 0.15.1 起 |
| --- | --- | --- | --- |
| 列宽 `column_limit` | 160 | 160 | 160 |
| 缩进 `indentation_spaces` | **4** | **2**（写死） | **4** |
| `try_wrap_long_lines` | true | false（Verible 默认） | true |
| 12 个 `*_alignment` | align | infer（Verible 默认） | align |
| `alignment_group_boundary` | blank-lines-and-separator-comments | none | 同左 |
| lint `line-length` | 100（**旧插件默认值被静默丢弃**） | 160 | 160（可配置） |

现在的规则：

1. **默认值有意存在两处**（2026-11-20 确认的设计）：
   * 服务端内置 `verible_bridge::DefaultStyleFlags()` / `DefaultLintFlags()` ——
     覆盖 MCP、裸 LSP、客户端不发配置这几种场景；
   * 插件 `package.json` 的 `fpgaLsp.verible.args` / `.lintArgs` —— VS Code 启动后端时
     作为初始化选项发过去，**替换**服务端那份。
   两份必须逐条一致：`tests/unit/verible_style_test.cpp::PluginDefaultsMirrorServerDefaults`
   直接读 `vscode-extension/package.json` 与服务端列表比对（其中一份单独改，单测立刻红；
   已做过反向验证）。另外 `DefaultFlagsMatchOldPluginConfig` 把 23 条全量钉死。
2. **外部配置 = 整体替换，不逐条合并**（2026-11-20 拍板保留该语义）：后端收到非空
   `veribleArgs` 就完全接管，没写的字段回落到 Verible 原生默认。`UserListReplacesBuiltInDefaults`
   把这个契约钉住 —— 只写 `--column_limit=120` 时缩进会变 2，这是有意的。
3. **flag 解析不碰全局 absl flag**：`--name=value` / `--flag value` / `--flag` 三种写法
   都在 `verible_bridge/flag_tokens.h` 里归一，再逐字段覆盖 style 副本 —— 多线程安全，
   服务端进程状态不会变成隐式输入。
4. **非法取值直接失败**（返回原文 + error，记 warn 日志），未知 flag 只记日志不中断。
5. **lint 参数真的生效**：`--rules=` / `--ruleset=` / `--rules_config=` /
   `--rules_config_search` / `--waiver_files=`；插件设置同时接受数组和旧版字符串写法
   （旧 Python 服务端只认数组，所以它的字符串默认值其实没生效 —— 上面表里 lint 100 就是这么来的）。
6. 实测：`fpga_format`（MCP）与 `textDocument/formatting`（LSP）对 `link_rx.v` 都输出
   **14,802 B**，与用旧插件 23 条默认 args 跑 `format_dump` 的结果**逐字节一致**
   —— 客户端发插件默认值、以及完全不发配置（走服务端内置）两条路径都验过。

## 已知差异（有意保留 / 版本基线）

1. **Verible 版本基线**：内嵌 `c6b29d62` vs Python 侧外部 `0.0-4296`。
   `formatting`/`rangeFormatting` 字节与 autofix `no-trailing-spaces` 一行差异属此族（LSP-18）；
   单一静态 exe 优先，不为对齐外部版本改内嵌规则。
2. **`ifdef` 语义**：C++ 只索引活动分支，Python 两分支都收（LSP-14，[commands.md](commands.md) 已登记）。
3. **inlayHint additive**：C++ 比 Python 多 8 条实例类型提示（Python 是 C++ 的真子集）。
4. **非 BMP 列**：Python 语义高亮用码点列（其 docstring 写 UTF-16 但实现不是），C++ 按协议用 UTF-16；
   仅合成 emoji 语料有 7/102 token 差异，真实 HDL 零差异。
5. **Python 格式 fallback**：`format_fallback.py` 未移植；C++ 始终有内嵌 Verible，不需要 fallback。
6. **Python 参考实现的 28 条缺陷**：见 [tests/PYTHON-BUGS.md](../tests/PYTHON-BUGS.md)，
   除安全 High（B7 已修）外保持「忠实复现 + 登记」。

## 仍未验证 / 有意保留

1. **真实 DRC/CDC 端到端**：本机语料没有 `*_Runs/phy_1/*_pr.db`，`runAnalysis` 只验证了
   「无布线产物」降级路径与解析层单测；有 db 时的 `report_drc`/`check_cdc` 需要真实布线结果才能复跑。
2. **GUI 渲染**：Problems 波浪线、大纲树、InlayHint、reports webview 未由人眼确认；
   服务端协议层已逐项验证（`e2e.lsp_smoke` + `tools/audit/probe_lsp.py`）。
3. **`tests/golden/snapshots/`**：仓库内快照保留为历史基线；Phase B 的等价性证据在
   [doc/audit/2026-11-18/golden-final/](audit/2026-11-18/golden-final/)（Python/native 双采 + compare）。

## 下一步（设计提案，未实现）

端到端优化与功能缺口已整理成三份提案，按优先级与验收标准排序，入口见
[doc/design/README.md](design/README.md)：协议/传输/后端链路 32 条
（[protocol-optimization.md](design/protocol-optimization.md)）、后端能力缺口 25 条
（[backend-features.md](design/backend-features.md)）、插件能力 28 条
（[extension-features.md](design/extension-features.md)）。
第一批建议为协议层"纯减法"四项（≈4 人日，收益可用字节数/毫秒数直接验收），
其次是把已有分析器接进编辑器的"接线"三项（约束诊断、时序端点跳转、`.al` 一致性）。

## 参考实现缺陷里最该知道的两条

- **P-1**：`ip_generated_hdl` 把 `.vhd` 混进 Verilog 列表 → elaborate 整体失败 →
  用户看到 "0 warning" 而 elaborate 根本没跑。
- **P-7b**：`.al` 的 `Path` 决定 TD 的**工程根**，产物写到那里。复制工程再跑 TD 必须改写
  `Path`，否则会写进用户真实工程（本项目中差点发生）。

完整清单见 [tests/PYTHON-BUGS.md](../tests/PYTHON-BUGS.md)。
## 片上探针（ChipWatcher，2026-11-20 落地并上板验证）

> 设计与实测细节见 [doc/design/on-chip-probe.md](design/on-chip-probe.md)。
> 工程：`%WORKSPACE%\CorpusDelta\corpus_delta`（PH1A100SFG676，TD 2026.2_SP1.218141），真机在环。

| 项 | 数据 |
| --- | --- |
| 新增后端模块 | `src/flow/probe.{h,cpp}`（mdn / compiled.cwc / 采集 CSV 三套纯解析 + TD 会话编排）、`src/flow/hardware_lease.h`（进程级 JTAG 租约） |
| 新增 LSP 命令 | `fpgalsp.probeStatus` / `fpgalsp.probeConfig` / `fpgalsp.probeCapture`（广播进 `executeCommandProvider`） |
| 新增 MCP 工具 | `fpga_probe_status`（只读）· `fpga_probe_config`（`write` 门控）· `fpga_probe_capture`（`flow` 门控）→ 工具总数 **43 → 46** |
| 新增插件命令 | `fpgaLsp.probeStatus` / `fpgaLsp.probeCapture` / `fpgaLsp.probeConfig`（采集前有模态确认） |
| ChipWatcher 面板（0.17.0） | `src/probePanel.js` + `src/probeHtml.js`：按总线分组的信号芯片、单击/整组移除、粘贴清单保存、watcher 现状（时钟/深度/触发/`stale` 徽章）、一键采集 |
| 下载速度 | `-spd` 是**档位号**不是 MHz：2=200k 3=500k 4=1M 5=2M 6=3M **7=5M（= GUI 默认）** 8=8M 9=16M；`program`/`probeCapture` 都接受 `speed`（2–9） |
| 按需下载 | `probeCapture.download` 三态，默认 `"auto"`：先直接用器件里现有的 bit 抓，抓不动才下载并在**同一会话**内重抓；真机实测 `downloaded:false` 也能拿到 1024 点 |
| 插件界面语言（0.17.1） | 所有用户可见文案统一英文（`package.json` 的 17 个设置描述、命令标题、面板 HTML、Doctor 报告）；中文只留在代码注释与 `doc/` |
| 单测 | `tests/unit/probe_test.cpp` **18 例**（含两条回归：2026.2 两行式 `list_cable`、38 字符层次名不被截断） |
| 回归 | `ctest` **63/63**；插件 `node test/run.js` **41/41** |

### 烧录真机验证（Step 1）

| 模式 | 命令 | 耗时 | 结果 |
| --- | --- | --- | --- |
| JTAG / SRAM | `download -mode jtag -bit {…} -spd 7 -cable 0` | **14.2 s** | `PRG-2014 : Chip validation success: PH1A100`，`ok:true` |
| SPI / Flash | `download -mode program_spi -bit {…} -spd 7 -cable 0 -flashsize 128 -v` | **96.0 s** | `SPI flash sector erase` + `Page program and verify`，`ok:true` |

两处修好才通的**真 bug**：

1. **`ParseCables` 认不出 2026.2 的输出**：它要求 `Connect device` 与 `0:PH1A100` 在同一行，
   而 2026.2 是两行（`PRG-1000 : Connect device:` 换行 `0:PH1A100`）→ 电缆列表恒为空 →
   `Program` 在 `stage:"cable"` 直接拒绝。单测夹具用的正是单行假输出，所以 29 个用例全绿也
   盖不住它。
2. **命令形态**：`program` / `program_spi` 是 2026.1 写法（本机无成功样本）；2026.2 GUI 实际
   用的是 `download -mode …`。现在默认 `download`，`command_style:"legacy"` 可回退。
   成功判据也从"没有 ERROR"改成"必须出现 `Chip validation success:` 或 `Downloading succeeded`
   —— BitWizard 出错同样返回 0，只看错误标记会把静默失败当成功。

### 采集真机验证（Step 2）

| 项 | 实测 |
| --- | --- |
| 预检 | `ready:true`、`clock:reg_init/clk`、`sample_depth:1024`、4 条 bus / 64 net |
| 采集 | **17.4 s**，`ok:true`，**1024 个采样点**，4 条 16 位总线，`trigger_index:127`（= `compiled.cwc` 的 `position`） |
| 落盘 | `phy_1/cw/capture_20261120_121852.csv`（1025 行：表头 + 1024 采样） |
| 读数 | 4 条总线全部 `distinct:1`、值为 0 |

**关于"全 0"**：`pwm_reg{0,1,6,7}_i` 来自 `import/src/project/bus_reg.v` 的 `reg_bus`，
复位为 `'d0`，只有主机写总线才改变；板子当前没有主机在写 → 0 就是**真实值**。
探针链路本身的活性由三点佐证：`KIT-1001 : capture completed successfully`、
`Trigger` 列唯一标记在 127、1024 行数据从 ERAM 读回并逐位解码。

**改了探针清单必须跑完整的 Place & Route**（实测同一工程两种 stages）：

| stages | TD 实际动作 | 耗时 | 重新埋探针 |
| --- | --- | --- | --- |
| `["route"]` | `Incremental route`（`import_db place.db`，只重跑 route） | 227 s | **不会**（`insert_debugger` 在 `opt_place`，被跳过；`compiled.cwc` 时间戳不变） |
| `["phy"]` | `reset_runs phy_1` + `launch_runs phy_1` | **401 s** | 会 |

为此新增 `probeStatus.watcher.stale`：把 `user.mdn` 逐位清单聚合成 bus，与
`compiled.cwc` 的 `<bus>` 逐位比对，不一致就 `stale:true` + `missing_in_bit` / `extra_in_bit`，
并且 `probeCapture` **默认拒绝**（要读当前旧 bit 必须显式 `allow_stale:true`）。
真机验证：把清单改成 80 个信号而 bit 仍是 64 位 → `stale:true`、
`missing:[ANPC_init0/ANPC_SVPWM_init/cnt]`、采集在 `stage:"stale"` 拒绝；还原后
`stale:false / ready:true`，再采一次 1024 点通过。

**未完成：非 0 的动态读数（已定位到 TD 侧）**。想证明"采到的是随时间变化的活动数据"，
把 16 位三角载波 `WHG/ANPC_init0/ANPC_SVPWM_init/cnt[15:0]` 加进清单，做了三轮实验：

| 实验 | 结果 |
| --- | --- |
| `get_nets -hier ANPC_init0/ANPC_SVPWM_init/cnt\[0\]`（对 `_pr.db`） | 1 个对象，**名字完全对得上** → 不是名字写错 |
| gate db + `insert_debugger … -mdn probe80.mdn` | 回显展开成 `mark_debug <80 个网>`，但 watcher 仍是 **4 node / 64 net** |
| 完整 `["phy"]`（401 s） | `watcherInst.sv` 仍 `CWC_BUS_NUM = 4` / `CWC_BUS_DIN_NUM = 64` |

即 `mark_debug` 收到了 80 个名字、网表里也有这些网，最终只落了 64 个。最可能是
**推断不出触发时钟**（TD 的错误串 `failed to get clock for signal %s, please specify
the clock in chipwatcher`），而 CLI 侧没有公开的"给单个 bus 指定触发时钟"的命令。

排查障碍：**后端驱动 `launch_runs` 时 TD 不写 `phy_1/run.log`**（只有 GUI / `run.bat`
驱动才写），`insert_debugger` 内部的告警看不到。下期：给 run 加 `tcl_afterstep` 把
`report_cwc_status` 落盘；摸清 `set_net_cond` / `set_cwc` 参数。

已把清单还原成原来那 64 个信号（1192 B，与原始文件同尺寸）。

## TD 升级到 2026.2_SP1（6.2.3.218141）的复核（2026-11-20）

本机 TD 从 `Release_2026.2`（6.2.3.210850，目录 `TD_Release_2026.2.210.850`）升级为
`Release_2026.2_SP1`（6.2.3.218141，目录 `TD_Release_2026.2_SP1.218.141`），旧安装已卸载。
先读了安装目录的 `doc/release_note.pdf`（SP1 只修 bug、无新功能；BitWizard 的 release note
未更新，但 `bw_commands_prompt.exe` **是新编译的**，build 也是 6.2.3.218141），然后分四路验证。

| 验证线 | 结论 | 证据 |
| --- | --- | --- |
| TD/BW 命令面 | **完全兼容**：29 条 TD + 6 条 BW 命令全部存在，后端用到的每个选项（`download -mode/-bit/-spd/-cable/-flashsize/-v`、`config_chipwatcher`、`get_hw_cws`、`run_hw_cw -trigger_now`、`wait_on_hw_cw`、`write_hw_cw_data -csv_file`、`insert_debugger -cwc`、`launch_runs`/`wait_run`/`set_param`、`read_adc`/`read_sdc`…）无移除无改名 | `build/_audit_sp1/cmd/` |
| 版本发现与解析 | **无需改**：`VersionKey("2026.2_SP1.218.141")` = `[2026,2,1,218,141]`，第 3 分量（service pack）`1 > 0` → 比 `[2026,2,0,210,850]` 新，自动发现正确选中 SP1（`source=auto`） | `build/_audit_sp1/static/` |
| 构建与测试 | 干净重建 190 步 0 error；`ctest` **63/63**；插件 **43/43**；e2e `mcp_smoke` 28 项全过 | `build/_audit_sp1/build/` |
| 真实硬件 | **9/9 PASS**：probe 状态/抓取（`downloaded:false`、1024 样本、`trigger_position:256` → `trigger_index=256`）、jtag 下载、program flash（`-flashsize 128 -v`，含 `SPI flash Page program and verify`）、`-spd` 生效（7→13.93 s vs 9→6.49 s） | `build/_audit_sp1/hw/` |

与 210850 的等价性是**多口径交叉验证**的，不只是"能跑"：probe 会话日志 390 行 vs 390 行
（diff 只有 3 行版本横幅、52 WARNING/0 ERROR 两边一致）、下发的 10 条 Tcl 命令逐条相同、
`trigger_position=256` 生成的临时 `.cwc` **SHA256 完全相同**、同机 jtag 耗时 +1.2 % /
flash +0.5~0.7 %。

### 因这次升级而改的代码（SP1 唯一的行为变化：`read_adc` 的 CRITICAL-WARNING 不再中断 Flow）

| 位置 | 改动 | 为什么 |
| --- | --- | --- |
| `src/flow/flow_logic.cpp` `DecideRunOutcome` | 新增"条件 1b"：**只有 CRITICAL-WARNING、没有 ERROR，且产物齐全 → 接受为成功**，警告原文进 `RunOutcome::warnings_only` | 旧版这类警告会连带中断流程（判失败是对的）；SP1 起流程照常跑完、产物齐全，若继续"有警告即失败"，就会出现「TD 已产出 bit、后端却报失败」。仍要求产物齐全 —— 不把"只报警告、根本没跑完"当成功 |
| `src/flow/flow_runner.cpp` `RunStage` | `launch_runs` 的判定 `HasError` → **`HasFatal`**，非致命警告走 `Emit("log")` 留痕；`warnings_only` 透出 | 同上；与 `probe.cpp` 里 `open_project` 早就改用 `HasFatal` 的先例对齐（`HDL-7225 CRITICAL-WARNING` 那个坑） |
| `src/flow/strategy.cpp` | 基线 `wait_run` / `launch_runs` / 逐 run `wait_run` 三处 `HasError` → `HasFatal`，逐 run 结果新增 `warnings` 字段 | 同一口径漏改，不修则 `compareStrategies` 在带 ADC 告警的工程上会整片报失败（QoR 其实已产出） |
| `src/flow/probe.cpp` `Capture` | `config_chipwatcher` 成功判据改为**文本锚优先**（`chipwatcher: sanity check successfully`）+ `KIT-1004` 兜底；判致命改 `HasFatal` | 消息号是运行时拼的、在 exe 里扫不到，升级后无法静态确认是否改号；文本锚两版都稳定（SP1 实测仍发 `KIT-1004`） |
| `src/reports/timing.cpp` | `STA coverage` 的 pattern 允许数字与 `%` 之间有空白 | SP1 修了"STA Coverage 不准确"；标签没变但分隔符形态不该被赌死，否则静默变成 `coverage: null` |
| `src/flow/programmer.cpp` | 更正成功判据的注释（见下），flash 模式输出 `flash_verified` | 实测：后端驱动的 `bw_commands_prompt.exe` **从不**打印 `GUI-1001 : Downloading succeeded!`（那是 GUI 版 `bw.exe` 的输出），`HasDownloadSuccess()` 在这条链路上是死代码；`PRG-2014 : Chip validation success` 在失败路径上**也会先打印**（后面紧跟 `PRG-8002/PRG-8000 ERROR`），真正挡住假成功的是 `HasBwError()` |
| `tests/unit/td_install_test.cpp` | 不再硬编码机器上的 TD 版本号；新增 SP1 版本样本用例 | 旧断言 `version == "2026.2.210.850"` 在升级后必红（升级一次的代价不该是改测试） |
| `tests/unit/td_device_test.cpp` | 三处硬编码旧安装路径 → 走 `td::FindTd()` | 旧路径失效后这三个"真实 TD"用例静默 `GTEST_SKIP`：ctest 显示绿、覆盖已经消失（"绿着烂掉"） |
| `tests/unit/flow_logic_test.cpp`、`tests/unit/reports_timing_test.cpp` | 新增 5 条用例（警告+产物齐全=成功 / 警告+产物缺失=失败 / SP1 版本样本 / coverage 带空格） | 锁住上面这些口径 |
| `vscode-extension/package.json`、`src/td/td_install.h` | 设置项示例与注释里的旧版本号/旧路径改成 `<version>` 占位或更新为 SP1 | 用户会照抄示例；`210850` 已不存在 |

### 明确**没有**改的（避免破坏覆盖）

全仓 200+ 处 `210850` 里绝大多数是**工程元数据**，不是安装版本：`.al` 里的
`<TD_Version>6.2.3.210850</TD_Version>` 表示"这个工程由哪个 TD 创建"（`bench_servers.py`、
`al_project_test.cpp`、`gate-corpus/*.al`、`doc/audit/**` 历史快照…）。它们跟着**工程**走，
不跟安装走，批量替换会毁掉真实样本覆盖。要区分 `project.td_version`（`.al`）与
`TdChoice.version`（安装目录名），二者在 `fpgalsp.info` 里是不同字段。

### 交付产物

| 项 | 值 |
| --- | --- |
| 扩展包 | `vscode-extension/fpga-lsp-anlogic-0.19.1.vsix`（230 文件，3,292,627 B） |
| 打包内置的服务端 | `build/bin/fpgalsp-native.exe`（Release + 全量 LTO，6,723,000 B，SHA256 `A0ED69CB…C1090`） |
| 一致性校验 | 解包后 `extension/server/bin/fpgalsp-native.exe` 的 SHA256 与该二进制**逐字节相同**；`extension/package.json` 版本 = 0.19.1 |

### 升级后工程侧的变化

`%WORKSPACE%\CorpusDelta\corpus_delta\corpus_delta.al` 已被 SP1 的 GUI 重写为
`<!-- Last modified by TangDynasty Release_2026.2_SP1 -->` + `<TD_Version>6.2.3.218141</TD_Version>`。
即 [`doc/design/on-chip-probe.md`](design/on-chip-probe.md) 里记的版本回退陷阱现在方向反了：
用 `2026.2`（210850）及更早**打不开**这个工程（`PRJ-8641`），要回退必须先删掉 `.al` 第 2 行。

### 仍未处理（已知、优先级低）

- `tests/golden/` 快照把**活安装**的 `td.version` / `td.root` 写进了断言，且不在
  `GOLDEN_VOLATILE_KEYS` 里 → 重新采集会全量 diff。该目录**未注册进 ctest**，不阻塞门禁。
- `programmer.cpp` 的 `HasDownloadSuccess()` 仍是死代码（留给别的通道），flash 模式已额外
  输出 `flash_verified` 作为"写+校验走完"的证据，但**没有**把它做成硬门槛（避免将来 TD 改文案
  把正常烧录判成失败）。
- 采样时钟 / 采样深度仍是编译期量、改不动（三组隔离实验见
  [`doc/design/on-chip-probe.md`](design/on-chip-probe.md)）；SP1 的 `DefaultFlow.tcl`
  与 210850 **逐字节相同**（9452 B，SHA256 `4FC07E5E…`），所以该结论在新版本上继续成立。

## 探针闭环加固 + MCP 帮助层（2026-11-21）

前一轮的端到端实测（[doc/mcp-usage.md](mcp-usage.md) 的坑清单就是它的产出）暴露了三个真问题：
探针静默不生效、`KIT-*` 诊断不可见、agent 没有使用说明。这一轮全部补齐。

### P0：三处后端加固

| # | 改动 | 实测证据 |
| --- | --- | --- |
| P0-1 | `flow_runner.cpp`：phy 跑完若**配了探针**（`chip_debugger/` 下有非空 `*.mdn`）却没产出 `cw/compiled.cwc`（或解析出 0 net），**直接失败**，并把 TD 的 `KIT-*` 原文带进错误；成功时 step JSON 增 `watcher:{present,net_count}` | 触发实测：`phy_1 finished but the ChipWatcher probe is not in the bitstream (...compiled.cwc is missing): TD said: ... —— 探针没有编进 bit；请确认 chip_debugger 目录被 insert_debugger 实际采用` |
| P0-2 | `td_output.{h,cpp}`：新增 `HasNotice()` / `NoticeLines()`，收 `<CODE> :` 这种**没有 severity 词**的 TD 消息（`KIT-2501` 就是这种），`HasError`/`HasFatal` 语义不变 | 8 条单测；`KIT-2501` 命中、`ERROR`/`CRITICAL-WARNING` 不重复计数 |
| P0-3 | `flow_runner.cpp`：不再依赖裸 `insert_debugger`。配了探针时写 `probe_insert.tcl` / `probe_setup.tcl` 并用**工程模式步骤钩子**挂到 `syn_1 rtl`、`phy_1 place` 的 `tcl_beforestep` 与 `phy_1 bitgen` 的 `tcl_afterstep`；TD 不支持时只留痕不判死 | syn 的 `run.log` 实证：`source_tcl .../probe_insert.tcl` → `insert_debugger <chip_debugger>` → `mark_debug WHG/e2e_cnt2l[0] …`（64 网全部被接收），报告里 `#mark_debug 64` |

**仍未闭环**：即使 mark_debug 进了网表，`setup_debugger` 仍没有产出 `cw/compiled.cwc`
（syn 报告同时给出 `SYN-1001 : … 48 dangling mark_debug insts`）。目前行为是
**P0-1 大声失败**，不再有"自信地成功"。下一步应查 dangling mark_debug 的成因
（很可能是镜像寄存器在优化后网名变了）。

### 帮助层 L1–L4：让无上下文的 agent 能自己学会用

| 层 | 内容 | 规模 |
| --- | --- | --- |
| L1 | `initialize.instructions` 作战手册（三套流程 / 硬规则 / 成本 / 权限） | 1613 字符 |
| L2 | **`fpga_guide` 工具**（工具表第 1 位，只读、默认可见）7 个 topic：`overview`/`project`/`index`/`flow`/`probe`/`program`/`troubleshoot` | 1.4–2.4 KB/topic |
| L3 | `prompts/list` + `prompts/get`：`onboard-project` / `run-flow-to-bit` / `probe-on-board` / `debug-failed-flow` | 4 个模板 |
| L4 | `fpga_probe_config` / `fpga_probe_capture` / `fpga_flow_run` 的 description 追加 `Preconditions / Cost / Failure modes / Example` | 585–1073 字符 |

工具总数 **46 → 47**；默认可见 **36 → 37**。相关文档：[doc/mcp-usage.md](mcp-usage.md)。
`schema` 门禁（`ctest -L mcp`）放宽为"description 允许在 Python 原文之后**追加**说明"，
反向缺失仍算漂移。

### 验证

- 干净重建 0 error；`ctest --preset windows-msvc-release` **63/63**（含 `unit.mcp_tools` 47 条断言、
  `unit.mcp_server` 的 prompts 断言、`mcp_schema_consistency`）；插件 `node test/run.js` **43/43**。
- MCP 手工核验：`instructions=1613` 字符、`tools/list` 47 个且首位 `fpga_guide`、
  `prompts/list` 4 项、`prompts/get` 可渲染、`fpga_guide` 7 个 topic 全部返回、未知 topic 回落 `overview`。

## P1/P2 能力补齐（2026-11-21 深夜）

### P1：探针可观测性的三块短板

| 项 | 改动 | 验证 |
| --- | --- | --- |
| per-bus 触发值 | `fpga_probe_capture` 新增 `buses:[{name,op,value}]`（`op` 与 LSP 侧同一套枚举：`equal/not_equal/less/less_equal/greater/greater_equal/bit_trigger`），映射到 `CaptureOptions.trigger.buses`；非法 op/缺 name/value 非 0-1 → `ToolError`，不让 TD 去报 | 传 `op:"bogus"` 返回 `Error: buses entry "x": unsupported op "bogus" ...` |
| 采样时钟/深度"不可改+原因" | `fpga_probe_status` / `fpga_probe_verify` 新增 `sampling:{clock,depth,editable:false,reason}`；平铺字段保留 | 实测返回 `editable:false` 与英文 reason（编译期量、CLI 无 setter、后端只 patch 临时 cwc 的 `<trigger>`） |
| `fpga_probe_verify`（新工具，48 个） | 逐位回答"哪几根被丢了、为什么"：`signals{requested,embedded,missing}`、`buses[].{width,embedded_bits,missing_bits,status,reason}`、`missing_nets`、`hints`、`stale`。`reason` 优先级：`no-bitstream` → `partial` → `dropped-by-td`（HDL 索引能查到该名）→ `not-in-build`；默认前 32 条 + `truncated` | 真工程实测：`requested=64 / embedded=0 / missing=64`，`bus0.status=dropped, reason=no-bitstream`，hint 明确要求"改 mdn 后必须重跑 syn+phy" |

### P2：启动、工程文件视图、引用

| 项 | 改动 | 验证 |
| --- | --- | --- |
| 安装发现缓存 | 结果写入 `%TEMP%\fpgalsp\td-install-cache.txt`（root/exe/version/saved_at）；失效条件：`FPGALSP_NO_TD_CACHE=1`、exe 不存在、超过 TTL（默认 24 h，`FPGALSP_TD_CACHE_TTL` 秒）；显式 `--td`/`TD_ROOT` 不读缓存但仍刷新。`TdChoice.from_cache` + `fpga_td_info` 的 `cached` 字段暴露命中 | 单测 `TdInstallReal.SecondLookupIsServedFromCache`；实测第二次 `cached=true` |
| `project_files` 默认区分构建/排除 | 默认响应就带 `counts{in_build,excluded,missing}` 与 `excluded[{path,kind,reason}]`（`reason` = auto-excluded/disabled/missing），`files` 仍只含参与构建的；`include_excluded:true` 保持旧行为；新增 `limit` 限 `excluded` 条数 | 真工程：`in_build=84, excluded=21`（`src/` 那棵废树一次性可见） |
| `references` 支持端口/网 | **结论：hdl 索引层本来就支持**（文本级标识符匹配），之前"返回空"是文件不在构建索引里（`src/WHG.v` 不在 `.al`）。本轮把 `include_declaration` 过滤从 features 下沉到 `HdlIndex::References(path,line,char,bool)`，并补单测（端口 7 条、内部 net 3 条、端口连接 3 条；`include_declaration=false` 各减 1） | 旧二进制 + 真工程实测同结论 |

### ⚠ 一条被纠正的旧结论：所谓"每次 MCP 调用 20 s"

之前记的"每个新 MCP 进程约 20 s 启动开销"**是错的**。实测：`--help` 0.0 s、`initialize` 0.03 s、
真正扫盘的 TD 发现 0.04 s。20 s 来自**我们自己的客户端脚本**——发完 `exit` 通知没关 stdin，
服务端按 MCP 规范等 EOF，客户端于是耗到自己的 20 s 超时再 kill。
修掉 `build/_probe_audit/mcp_call.py` 里这一行后，同一次调用从 **20 s 降到 0.2 s**。
`fpga_guide` 的 overview、`initialize.instructions`、`doc/mcp-usage.md` 的相应文案都已改正；
安装发现缓存仍保留（在磁盘多/网络盘的机器上仍有用），但本机收益接近 0。

## 探针闭环 + Flow 六步对齐（2026-11-21 凌晨 04:00）

### 一、探针进 bit：根因是"漏了 gate 之后那一次 insert_debugger"

用户的 GUI 时序给了决定性线索：mark_debug 只能在 **Read Design 之后**；采样时钟/深度只能在
**Optimize Gate 之后**设；然后跑 phy 才进 bit。按这个时序复测：

| 做法 | 结果 |
| --- | --- |
| 只在 `rtl` 步前插 `insert_debugger <chip_debugger>`（上一轮） | syn 报 `#mark_debug 64`，phy 的 insert_debugger 报 `KIT-2501 : No mark_debug exists in the netlist`，**无 watcher** |
| `rtl` 步前 + **`gate` 步后** 各插一次 | gate 结束报 `KIT-1004 : import watcher inst auto_created_cwc1 completed, there are 4 view node(s), 64 trigger net(s), 64 data net(s)`，生成 `syn_1/cw/auto_created_cwc1/`（OOC 综合）；phy 里出现 `auto_created_cwc1/...` 与 `cwc_jtck`；**后端自己跑出的 bit 直接 `probe_capture` 成功：1024 点 / trigger_index=256** |

因此 `flow_runner` 的探针钩子改为 **`syn_1 rtl tcl_beforestep`（mark）+ `syn_1 gate tcl_afterstep`（建 watcher）**，
`phy_1 place` 前那次保留；phy 的成功判据也从"只看 `compiled.cwc`"改为
**"`compiled.cwc` 有 net" 或 "`syn_1/cw/` 下有 `auto_created_cwc*` OOC 产物"** ——
后者才是"watcher 真建出来了"的硬证据（批量跑 TD 不写 cwc，见下）。

**仍有一小截**：批量（`launch_runs`）跑完 phy 时 TD **不写** `phy_1/cw/compiled.cwc`
（只有 GUI 的 ChipWatcher 存盘会写），而 `probe_capture` 需要它拿 instance 名与触发条件。
当前可用的桥接办法：把任意一份**与当前 mdn 同构**的 cwc 放到该路径（上一次抓取留下的
`.fpgalsp/probe/trigger_*.cwc` 即可，实测对新 bit 完全适用）。彻底自动化需要从
`report_cwc_status` / `auto_created_cwc1_watcherInst.tcl` 反推并生成 cwc —— 列为下一步。

### 二、插件侧边栏 Flow 与 TD GUI 的 6 步不一致：原因与修复

**原因**：后端只把**阶段级**状态（`state/stage/elapsed`）发给插件；`flow.status` 里那张
command 级 Flow Statistics 表（含每步 wall/cpu/mem）从来没被翻译成 GUI 的 6 步，
`fpgaLsp/flowStatus` 的 `flow` 对象里根本没有 `steps` 键（`bridge.cpp` 只是透传一个永远不存在的字段）。
插件只能按名字正则猜，于是显示成 syn/phy 两段而不是 6 步。

**修复**：
- 新 `src/flow/flow_status.{h,cpp}`：解析 `flow.status` / `*_flow.status` 的 Flow Statistics 表，
  把命令名映射到 GUI 6 步（`analyze|elaborate|read_hdl|read_adc|read_sdc|import_db|import_device → Read Design`；
  `optimize_rtl`→Optimize RTL；`optimize_gate`→Optimize Gate；`place|legalize_phy_inst`→Optimize Placement；
  `route`→Optimize Routing；`bitgen|setup_debugger`→Generate Bitstream），同步骤耗时相加、mem 取 max；
  用 `.read_design/.opt_rtl/.opt_gate/.opt_place/.opt_route/.bitgen.{begin,end,error}.f` 判
  `pending|running|done|failed`；**没有 flow.status 时也给 6 步全 pending**。
- `flow_runner` 的 flow JSON 增 `steps:[{name,stage,status,wall,cpu,mem_mb,run}]`（1 s 缓存）；
  `src/server/commands.cpp` 加一行把 `steps` 透传到 LSP 通道（插件走这条）。
- `vscode-extension/src/flowTree.js`：有 `steps` 时渲染 6 个节点，标签 `Optimize Gate · 1m11s`、
  描述 `done · cpu 3m58s · 1125 MB`，父节点保留总耗时；无 `steps` 时**原样回落**到旧显示。
- 测试：新 `tests/unit/flow_status_test.cpp`（`1m11s`→71、同步骤相加、缺表 6 步 pending、
  标记→done/failed/running、`place_flow.status`+`route_flow.status` 取 max 不重复计数）；
  插件 `test/run.js` 新增"steps 渲染 6 步 + 耗时"用例（**44 passed**）。

已知不精确处：`fix_hold`/`update_timing` 在 gate/place/route 三步都出现，故意不归属任何一步
（记进去必然错），所以某些 run 的 place/route 耗时会略少。

## 干净上下文 agent 实测 + 缺口补齐（2026-11-21 清晨）

派了一个**干净上下文**的子 agent：只告诉它"后端可执行文件在哪、怎么起 MCP、工程与板子在哪、任务是什么"，
**不告诉任何使用步骤**，让它自己从 `initialize.instructions` / `tools/list` / `fpga_guide` / `prompts` 里学。
它最终**独立走通并把业务数拿出来了**。

### 业务结论（1500 Hz 两电平载波计数器：实测 = 理论）

| 项 | 理论（从 RTL 推） | 实测 |
| --- | --- | --- |
| 采样 | `reg_init/clk`=100 MHz，深度 1024，窗口 **10.24 µs**（1.54% 个载波周期） | 同（`editable:false`） |
| 计数器步进 | 每 100 ns ±1（`PWM_clk`=10 MHz） | 两次抓取均 **+102 步、步间隔恰好 10 个采样点** |
| 载波周期 | 6666 tick × 100 ns = 666.6 µs → **1500.15 Hz** | **6665 tick = 666.5 µs → 1500.38 Hz**（±1 tick 量化，差 0.015%） |
| 状态位 | `0b10101`（lock=1、mode=1 两电平、pwm_int=1、dir=0） | **恒 21** ✅ |

证据：`build/_audit_sp1/agent_clean/{REPORT.md, cap1.csv, cap2.csv, calls.jsonl}`。

### 它踩到的三个阻塞点（本轮已补齐两个 + 一个半）

| # | 问题 | 处理 |
| --- | --- | --- |
| 1 | **改完 `user.mdn` 只有 `stages:["all"]` 才真的插探针**（guide 写 phy，实测 phy-only 完全不插），白跑 5–10 min | ✅ `flow_run` 在 `user.mdn` 比 `syn_1/*_gate.db` 新时**自动把 phy 提升成 syn+phy**，并在返回里给 `stagesRequested`+`note`；guide/instructions 的措辞已改成 `["all"]` |
| 2 | **探针上限 4 总线 / 64 bit 不校验不报错**（5 条静默丢第 5 条、6 条→`empty ChipWatcher configuration` 白跑 8 min） | ✅ `fpga_probe_config` 先算 `bus_count/bit_count` 并和 `limits` 比对，超限 **直接 ok:false**（连 dry run 也不放行），错误文案说明"TD 会静默丢弃" |
| 3 | **后端从不生成 `phy_1/cw/compiled.cwc`**（批量 `launch_runs` 不写、`setup_debugger` 在 CLI 下静默 no-op），抓取被阻塞，只能手工拼 | ✅ 新增 `probe::BuildCwcFromWatcher` + `EnsureCompiledCwc`：从 TD 自己的 `syn_1/cw/auto_created_cwc*/*_watcherInst.sv`（总线数/位宽/DIN/深度）+ `phy_1/<prj>_inst.bid`（采集 RAM 的 rid/wid/offset）+ 当前 `user.mdn` **反推并写出 cwc**；`probe_status`/`probe_capture` 在 cwc 缺失时自动补齐。位宽顺序对不上时**拒绝生成**并提示重跑 `["all"]`（猜 bus 顺序 = 错的采样数据） |
| 4 | 采样时钟/深度 `editable:false`，没有任何工具能改 | 仍是 TD 编译期量（GUI 在 Optimize Gate 之后设）；已在 guide 里写明"要用别的时钟/深度只能改 RTL 或走 GUI"，并把**当前** depth/clock 从 watcher 产物里读出来报给 agent |

### 它指出但本轮**没有**动的

- 改 mdn 后 `probe_verify` 的 hints 与 guide 措辞需一致（已随本轮 changeset 统一，但没重跑它的原始路径验证）。
- 硬件采样 RAM 在"非 16 位宽总线"后有 5 bit 偏移（它靠数据自洽性还原）；我们生成的 cwc 按 TD 的总线顺序写，
  但**位序 ↔ 硬件 slot 的对应关系没有独立校验** —— 若要长期依赖，应再加一条"抓已知常数网自检"的用例。
- `flow_status` 在"客户端崩溃导致 TD 被杀"后没有 `interrupted` 标记（仍是 idle）。列为下一步。

### 第二轮：把"探针 sidecar"这条路走到底（结论：不能再猜）

- **名字预检**（`fpga_probe_config`）：把 `user.mdn` 里的每个总线名丢给 HDL 索引查一遍，
  查不到的放进 `unresolved` 并给 warning（**不判失败** —— 索引只认声明，层级名/生成块里的网
  可能查不到，硬拦会误伤）。实测：`WHG/no_such_sig` 被正确列出。
- **`cwc_bus_order` / `generate_cwc`**（`fpga_probe_capture`）：TD 会重排总线（watcher 产物里
  只有位宽、没有总线名），混合位宽时无法自动反推"哪个 16 位对哪个 slot"。因此：
  - 新增 `cwc_bus_order`：调用方把真实顺序传进来（`WHG/e2e_hstat` 或 `e2e_hstat`），
    生成器按该顺序写 cwc；实测能生成 10 KB 的 cwc，总线顺序与 TD 一致；
  - 新增 `generate_cwc`（**默认 false**）：生成的 ERAM 段没有经过位流侧校验，实测拿它去抓取
    会让 TD 会话在 `wait_on_hw_cw` 卡死（`capture completed successfully` 不再出现）→
    默认拒绝并让调用方提供一份可用 cwc，比静默给出错误数据安全。
- 生成的 cwc 里的总线/网名必须是**相对顶层模块**的（TD 自己的 cwc 写 `pwm_reg0_i[15:0]`，
  带 `WHG/` 前缀会让"mdn ↔ bit"逐位比对判 stale）。

**下一步（真正的解法）**：`insert_debugger <dir> -cwc <我们写的配置>` 在 gate 之后调用时，
TD 会按我们的配置编译 watcher（2048 深度那组实验已经证明 `-cwc` 被认），如果它同时把
`compiled.cwc` 写出来，sidecar 问题就彻底消失，而且顺带获得采样时钟/深度的控制权。
这需要一次带 `-cwc` 的完整 syn+phy 实验，留到下一轮。

### 第三轮：sidecar 能自动写了，但"自写 cwc 用于抓取"还没打通（诚实记录）

**已证实并落地**
- `insert_debugger <chip_debugger> -cwc <配置>` 在 **gate 之后**确实被 TD 采纳：把 depth=2048 写进配置传进去，
  新的 `syn_1/cw/auto_created_cwc1_watcherInst.sv` 里 `RAM_DATA_DEPTH = 2048`。
  也就是说"采样深度/时钟可配置"在机理上成立，只是还需稳定的写法。
- **phy 跑完后后端自动补 sidecar**：`probe::WriteCompiledCwcAfterPhy` 用 mdn + 指定的 clock/depth +
  **新位流的 `_inst.bid` ERAM 行**写 `phy_1/cw/compiled.cwc`，并把 `compiled.bit` 一起同步。
  实测（2026-11-21 06:49，一次完整的 `stages:["all"]`）：`cw/` 下同时出现
  `compiled.cwc` 10,313 B 与 `compiled.bit` 5,356,814 B，**全程没有 GUI 参与**；
  随后 `probe_status` 给出 `present:true / net_count:53 / clock:reg_init/clk`。
  ⚠ 只在**缺失**时写，不覆盖已有 cwc（TD/GUI 留下的那份是验证过的）。
- `fpga_probe_config` 新增 `clock` / `depth`（落盘到 `.fpgalsp/probe/config.txt`）与
  `apply_build_config`（默认 false，见下）。

**没打通的部分（所以默认关闭）**
- 自写的 cwc 拿去 **抓取** 仍会让 TD 会话在 `wait_on_hw_cw` / `write_hw_cw_data` 阶段挂掉；
  带 depth=2048 的 `-cwc` 进 place 也连续两次 `Run phy_1 has exited unexpectedly`。
  结论：cwc 里除了总线/时钟/深度，**`<logic_eram>` 的字段必须与位流严格一致**，我们这份还有偏差
  （对照组：干净 agent 用同一套元数据手工拼的那份在 04:40 抓到过数据，说明方向对、细节差）。
- 因此 `apply_build_config` 默认 false（不把自写 cwc 交给 gate）；`WriteCompiledCwcAfterPhy`
  只补缺失、不覆盖。**可靠路径仍是**：用一份 TD/GUI 或历史成功抓取留下的 cwc。

**下一步（明确）**：在 GUI 里保存一次 ChipWatcher，拿到一份**TD 亲笔**的 cwc，
与我们生成的逐字段 diff（`logic_eram` 的 width/data_offset/working_mode 等），把差异固化进生成器；
再跑一次 place 验证 depth 可调。这是最后一个"必须人参与"的点。

### 第四轮：定位到"生成的 cwc 为什么不能用"的关键证据

把 `Chipwatcher code` 当指纹来比对（`config_chipwatcher` 每次都会打印）：

| 场景 | Chipwatcher code | 抓取 |
| --- | --- | --- |
| 03:57（TD/GUI 留下的 64 位 cwc + 对应 bit） | `1011001100110001` | ✅ 成功 |
| 04:40（干净 agent 手工拼的 cwc + 04:36 bit） | `0110100011111110` | ✅ 成功 |
| 05:23 / 07:10（**我们生成的** cwc + 对应 bit） | `0000101010001110`（两次相同） | ❌ `wait_on_hw_cw` 永远等不到触发，会话挂死 |

结论：
1. **code 是由 cwc 决定的**（同 bit、不同 cwc → code 不同；不同 bit、同一份生成器 → code 相同），
   所以 code 是"这份 cwc 与 TD 编译时那份配置是否等价"的指纹。
2. 我们的生成器**光靠 `*_watcherInst.sv` + `_inst.bid` 还不够**：总线顺序已经用 `cwc_bus_order`
   对齐了，仍不等价 ⇒ 差异在 `<logic_eram>`/mem 段（带宽 `width`、`section_size`、
   mem 行集合）或 `<config>` 的某个字段。缺一份 **TD 亲笔的、同一份设计的 `compiled.cwc`** 才能逐字段 diff。
3. 因此本轮把"自动写 cwc"改为 **opt-in**（`probe_config{write_sidecar_cwc:true}`），
   默认只同步 `compiled.bit`；一条错误的 cwc 会让抓取挂死，比"没有 cwc"更糟。
   `probe_config{cwc_bus_order:[...]}` 会落盘到 `.fpgalsp/probe/config.txt`，供生成器使用。

**要做的一件事（需要 GUI 一次）**：在 TD GUI 里给当前设计保存一次 ChipWatcher（或直接跑一次
GUI 的 phy 让 TD 写 `compiled.cwc`），把那份文件留给我 —— 我按它 diff 出生成器的差异，
之后"无 GUI 抓取 + 采样深度可调"就能一起闭环。

### 第五轮：cwc 生成器修好了 —— 两个字段的事

把"能用的那份 cwc"（干净 agent 手工拼的）与我们生成的逐行 diff，差异只有两处：

| 差异 | 错的样子 | 对的样子 | 后果 |
| --- | --- | --- | --- |
| `<clock>` 缺 `clk_peroid` | `<clock clk_name="reg_init/clk"/>` | `<clock clk_name="reg_init/clk" clk_peroid="10ns"/>` | `config_chipwatcher` 照样打 KIT-1004，但算出的 `Chipwatcher code` 与设计不一致 → `wait_on_hw_cw` 永远等不到触发、会话挂死 |
| `phy_eram` 的 `working_mode_width` | 用了数据位宽（14） | 用**物理位宽** `width_a`（20） | 同上（code 不一致） |
| （无关）`trigger position` | 127 | 256 | 运行期量，不影响 code |

修掉这两处后：**生成的 cwc 与"能用的那份"一致，抓取立刻成功**（`ok:true`、1024 点、`trigger_index` 127/256 随设置）。

配套：
- `probe::ResolveClockPeriod`：周期从历史 cwc 里取（TD 不单独产出），取不到按 `10ns` 兜底。
- `phy` 结束后自动写 sidecar 恢复为**默认开启**（`write_sidecar_cwc`，可用 `probe_config{write_sidecar_cwc:false}` 关掉）；
  写之前用 watcher 的位宽逐位校验总线顺序，顺序对不上就拒绝并提示用 `probe_config{cwc_bus_order:[...]}`。
- 混合位宽（如 16/5/16/16）时 TD 会重排总线且产物里没有总线名 —— 需要 `cwc_bus_order` 提示一次；
  等宽总线（如 4×16）不需要。

### 第六轮：按用户实测事实去掉写死的探针上限 + ChipWatcher 进状态栏

用户的实测事实（覆盖我之前的推断）：
- GUI 里**可以对任意信号 mark_debug**；只有"Optimize Gate 之后跑 phy"时信号太多才会 error，
  而且**每颗芯片上限不同**；GUI 里还能 enable/disable 列表条目，**disable 的不占 debug 资源**。
- 所以后端**不该写死 4 总线 / 64 bit**：`fpga_probe_config` 现在只报 `bus_count`/`bit_count`/
  `budget{capped:false,note}`，超过 4/64 时给一句 warning，真正的上限让 TD 在 phy 阶段自己报错。
  实测 9 总线 / 93 bit 的 dry run 返回 `ok:true`（之前会被直接拒绝）。

ChipWatcher 面板：
- 状态栏新增 `$(pulse) ChipWatcher` 条目（点了开面板；有探针时显示 `ChipWatcher: <net 数>`，
  stale 会写进 tooltip）。
- `fpgaLsp.showProbePanel` / "Edit probe signal list" 现在有**兜底路径**：底栏 webview view
  拿不到时自动退回编辑区 webviewPanel，不会再出现"没有可提供视图数据的已注册数据提供程序"。
- `package.json` 补 `onView:fpgaLsp.probeView` 激活事件。

### 0.19.2：ChipWatcher 面板一律走编辑区（底栏 provider 在某些环境不生效）

用户在底栏仍然看到"没有可提供视图数据的已注册数据提供程序"，但同一批注册的活动栏视图
（Device / Flow 六步 / Reports）是正常的 —— 说明扩展已激活，只是底栏 panel 视图的
provider 没和视图关联上（VS Code 侧行为，未在扩展里找到可控开关）。

处理：
- **状态栏 `$(pulse) ChipWatcher` 与 "Edit probe signal list" 现在直接开编辑区 webviewPanel**，
  不再依赖底栏视图；底栏注册保留（能用的环境照旧）。
- 状态栏 tooltip 带诊断：provider 注册时间、底栏视图是否真的 resolve 过；
- 激活时 `console.log('[fpgalsp] probe view provider registered for ...')` 进 Exthost 日志。

### 第七轮（团队模式）：视图重排 + ChipWatcher 原生树 + disable 端到端结论

用户要求与结论：
1. **视图结构**（已改）：活动栏 FPGA 容器自上而下 = Project & Flow / Reports / ChipWatcher；
   Module Hierarchy 移到**资源管理器**侧栏；删掉 `views.panel`（底栏那个在所有环境都不显示）。
2. **命令去重**（已改）：`fpgaLsp.probeConfig`（Edit probe signal list）与
   `fpgaLsp.showProbePanel`（ChipWatcher probe panel）本来就是同一个动作，已删前者，
   只留 `showProbePanel`（编辑区面板）。
3. **ChipWatcher 改成原生 TreeView**（新 `src/probeTree.js`）：状态行 / Signals（每条总线
   带 enable-disable 勾选，disabled 用 `circle-slash`）/ Actions（Capture、Add signals、富面板、Refresh）。
   改原生树的理由：底栏 webview view 在用户环境里始终不显示 provider，而同一批注册的
   活动栏 TreeView 一直正常 —— 换成同机制后不再依赖 webview provider。
4. **Flow 六步常显**（已改）：无 run 也渲染 Read Design / Optimize RTL / Optimize Gate /
   Optimize Placement / Optimize Routing / Generate Bitstream，状态 `not run`。
5. **disable 条目写进 cwc 的 `data="disable"`（已实现，端到端有硬结论）**：
   - `fpga_probe_config{disabled:[...]}`（整条总线 / 单根 / 范围）→ 落盘 config.txt；
   - cwc 渲染：bus `is_enabled="no"`、net `data="disable" trig="disable"`、signal_vec `cond="dont_care"`；
     disabled 不计入 enabled_* 计数与 `<logic_eram width>`，但仍完整写进 XML；
   - **实测（14:15）**：`disabled:["WHG/e2e_hstat"] + apply_build_config:true` → syn 阶段 TD
     **确实按 3 总线 / 48 bit 建 watcher**（`CWC_BUS_NUM=3`、`CWC_BUS_DIN_NUM=48`，正常是 4/53），
     证明 disable 语义正确传进了 TD；
   - **但 phy 的 opt_place 直接崩**（`PRJ-8806 ERROR: Run phy_1 has exited unexpectedly`，
     TD 进程退出、日志无 PHY 错误）：**TD 2026.2 批量流程无法 place 由自写 cwc 建出的 watcher**。
     这不是我们 cwc 字段写错（`clk_peroid`/`working_mode_width` 都已对齐，补 `<logic_eram>` 也一样崩），
     而是 `-cwc` 路径在 batch 下的硬限制。
   - 结论：**disable 目前只能走 GUI**；后端 API 与渲染保留（数据模型正确、单测覆盖），
     `apply_build_config` 继续默认 false，文档写明上述崩溃。
6. sidecar 生成增加"形状"判断：`net_count/disabled_net_count` 与当前 mdn+disabled 计划不一致就重建，
   不再"只补缺"导致 disable 后仍用旧的 53-bit cwc。

### 第八轮：Flow 视图"刚开窗口也要有 TD GUI 六步"（后端补数据源）

用户的抱怨是准确的：`flowStatus` 只在**本次会话跑过 flow**（runner 存在）时才有 `steps`；
刚打开窗口时 runner 为空，返回的 idle 对象里根本没有 `steps`，插件只能显示旧的/空的。

修复：两条通道都改成**从磁盘的 `syn_1/flow.status`、`phy_1/place_flow.status`、
`phy_1/route_flow.status` 现算 6 步**（`flow::CollectAllFlowSteps` + 新增 `flow::StepsToJson`）：
- LSP `fpgaLsp/flowStatus`：`flow.steps` 一律由磁盘计算（覆盖 idle 情况）；
- MCP `fpga_flow_status`：同样补 `flow.steps`。

实测（工程当前状态，idle、无 runner）：
`Read Design 1s(done) / Optimize RTL 41s / Optimize Gate 56s / Optimize Placement 187s /
Optimize Routing 88s / Generate Bitstream 0s` —— 名称与顺序与 TD GUI 一致。

### 第九轮：disable 真正进了位流（3 总线/48 bit），cwc 改用"模板打补丁"

三个实测结论：

1. **`-cwc` 机制本身没问题**：把"TD 认可的那份 cwc"原样交给 `insert_debugger -cwc`
   跑完整 syn+phy，**place 正常**。之前一直崩是因为我们从零生成配置。
2. **模板 + 只改 enable 属性 = disable 生效**：同一份 cwc 只把 `e2e_hstat` 标成
   `is_enabled="no"` / `data="disable"`，syn 建出 **3 总线 / 48 bit**（正常 4/53），
   **place 成功**（`.opt_place.end.f`，无 error）。
3. **`-cwc` 只能给 syn 的 rtl/gate 步**：phy 的 place 步再喂一份"按 disabled 改小"的
   cwc 会让 TD 看到 4/53 的 netlist 配 3/48 的配置 → opt_place 必崩。已改成只在
   `run_name == "syn_1"` 时生成并挂 `-cwc`，phy 走普通 `insert_debugger`。

据此改后端：
- 新增 `probe::BuildCwcFromTemplate`：拿 `phy_1/cw/compiled.cwc` 当模板，按"尾名"把模板总线
  映射到 mdn 总线（**允许顺序不同**），只改 `is_enabled` / `data` / `trig`；打不出来就
  退回 `BuildCwcFromConfig` 并在日志里说明。
- 修 `NewestWatcherDir`：`phy_1/cw/compiled.cwc` 名字里也含 "cwc"，以前被当成 watcher 目录，
  于是生成的 cwc 里 `inst="compiled.cwc"`；现在只认目录。
- `fpga_probe_config{apply_build_config:false}` 现在**双向**生效（以前只置 true，残留的
  apply=1 会让下一次正常 flow 被带成自写 cwc 并崩 place）。

**仍未解决**：disabled 位流的**抓取**报
`KIT-8445 ERROR: readback data ... sample depth is shorter than the set value (54/48)` ——
硬件按 54 位、配置按 48 位算，说明 disable 后 `<logic_eram width>` 该用"物理/ERAM 合计宽度"
而不是 enable 位数（试过 48/53/54 都没过）。这一步留待下一轮（需要拿到禁用后位流里
TD 自己认可的宽度口径）。

工程状态已恢复：4 总线/53 bit、sidecar 10,313 B、`probe_capture` ok / 1024 点。

### 第十轮：在**真实 VS Code** 里验证视图与六步（不再是猜）

新增 `vscode-suite` 断言并用 `scripts/run-vscode-test.ps1` 真机跑（扩展宿主，非 mock）：

```
[PASS] probe commands registered  all present
[PASS] duplicate probe command removed (fpgaLsp.probeConfig)
[PASS] FPGA sidebar order = flow, reports, probe
       fpgaLsp.flowView,fpgaLsp.reportsView,fpgaLsp.probeView
[PASS] Module Hierarchy lives in the explorer sidebar   fpgaLsp.modulesView
[PASS] no bottom-panel probe view remains
[PASS] probe tree refresh command runs
[PASS] flow tree refresh command runs
[PASS] flowStatus returns the TD GUI six steps (idle window)
       Read Design | Optimize RTL | Optimize Gate | Optimize Placement | Optimize Routing | Generate Bitstream
```

说明：
- 六步那条是在宿主里直接起 `fpgalsp-native.exe --mcp` 问 `fpga_flow_status`（idle、没跑过 flow），
  证明"刚加载窗口也有六步"是数据层就成立，不依赖 UI 渲染猜测。
- 套件里仍有 4 条历史失败（completion 标签、semantic tokens、copy symbol path、doctor 报告），
  是把测试工程从 CorpusAlpha 换到 CorpusAlpha 后的既有差异，与本次改动无关（探针/视图 7 条全过）。

### 第十一轮：disabled 位流的抓取 = TD 2026.2 批量流程的硬限制（已定位，做快速失败）

**实验矩阵**（disabled 建出的位流 + 8 类 cwc 变体，均 `download:true` 实测）：

| cwc 变体 | Chipwatcher code | 结果 |
|---|---|---|
| gate cwc（53 net，5 个 `data="disable"`，旧 ERAM 行 section 54） | 1110011100101110 | 触发成功，回读报 **KIT-8445 (54/48)** |
| gate cwc + `logic_eram width` = 48/49/50/54/55 | 同上 | 报错数字不变（54/48），说明 54 来自位流侧、48 才是 cwc 的 enabled 位数 |
| gate cwc + section_size 改 49 | 同上 | 仍 (54/48) |
| 新 sidecar（48 net 全 enable / 带 disable 标志） | 0000101010001110 | code 与位流不一致 → `wait_on_hw_cw` 挂死 |
| 删掉 disabled 总线（3 总线 48 net） | 1110011100101110 | 挂死 |
| gate cwc + 新 bid 的 ERAM 行（width 48/49/54） | 1010101011001010 | 挂死 / 会话崩 |

结论：**位流里的采集 RAM 仍按 disable 前的宽度（54）分配**，而 cwc 里只有 48 个 enabled 位；
把 cwc 改成 54 宽又会让 `Chipwatcher code` 与位流不一致（TD 把 code 写进了位流）。
批量流程没有"重建 RAM + 重算 code"的接口，所以 **disabled 位流在 batch 下读不出来**。

处理（0.19.8）：
- `fpga_probe_capture` 在 `disabled_net_count > 0` 时**立刻拒绝**（0.3 s，不再挂 30 s），
  文案给出两条可执行路径：清空 `disabled` 重跑 `stages:["all"]`，或走 TD GUI 抓；
- 新增 `allow_disabled_capture:true` 让人可以强行尝试（默认关）；
- 正常（无 disable）位流验证未受影响：4 总线/53 bit、`probe_capture` ok / 1024 点。

下一步若要真正打通，需要一份 **TD GUI 为 disabled 配置亲笔写的 `compiled.cwc`** ——
对比它的 `<logic_eram>`/code 才能知道 TD 自己怎么表达"禁用后的 RAM"。

### 第十二轮：disabled 抓取的"正确配方"找到了，但复现性未稳定

**找到的配方**（2026-11-21 20:06 实测成功）：

> sidecar = **gate 阶段的 cwc**（总线/触发/时钟**原样保留**，含 `is_enabled="no"` 与
> `data="disable"`）+ 把 `<logic_eram>` 整块换成**当前位流 bid** 的行（widths 20/20/9、
> data_offset 0/20/40、section_size 49、正确的 rid/wid）。

结果：`ok=True`、`samples=1024`、`Chipwatcher code` 与位流一致（触发完成）。
`logic_eram width` 写 48 / 49 / 50 都能过（说明它不影响 code，也不影响回读）。

对照失败：
- 整份重生成 sidecar → code 变（`0000101010001110`）→ `wait_on_hw_cw` 挂死；
- 只改 gate cwc 的 width（48/49/50/53/54/55）或 section_size 不改 ERAM 行 → `KIT-8445 (54/48)`。

**已固化进后端**：
- 新增 `probe::WriteCompiledCwcFromGate(project, gate_cwc_path)`：phy 成功后，若本次 run
  给 TD 交过 `-cwc`（`gate_cwc_path` 非空），就用它写 sidecar（splice 新 bid 的 ERAM 块），
  否则仍走原来的 `BuildCwcAfterPhy`；
- 单测 `ProbeCwc.GateSidecarKeepsBusListAndSplicesFreshEramRows` 覆盖：总线/触发/时钟保留、
  旧 section_size 被替换、新 data_offset 出现、换行结构不被破坏；
- `probe_capture` 遇到含 disabled 的位流会给 `warning`（实验性；若触发不完成就清空 disabled
  重跑或走 GUI），不再一刀切拒绝。

**未稳定**：20:06 之后又重建了 3 次 disabled 位流，同样的 sidecar 写法有两次在
`wait_on_hw_cw` 挂住 / 会话退出（一次是硬件瞬态：正常位流也连不上，重跑后恢复）。
说明"gate cwc + 新 ERAM"还不是充分条件，可能还与 TD OOC watcher 的编译缓存/位流内容有关。
需要再定位的点：同一份 gate cwc 下，TD 生成的位流里 `RAM_LEN`（48）与 cwc 的
`<logic_eram width>`（49）为什么要差 1，以及为什么有时触发逻辑不响应。

### 第十三轮：disabled 抓取复现性结论 —— 系统性失败，改为快速失败

**复现实验**（2 次连续 disabled 重建，各用自己的 sidecar 与对方的 sidecar 交叉抓取）：

| 尝试 | 结果 |
|---|---|
| build1 自己的 sidecar（gate cwc + 新 bid ERAM） | code `1110011100101110`，`wait_on_hw_cw` 不返回 → 会话退出 |
| build2 + build1 的 sidecar | 同上 |
| build2 自己的 sidecar | 同上 |

三次全失败；两次构建的 watcher 都是 `DIN=48 / RAM_LEN=48`。
结合 20:06 唯一一次成功（同配方，1024 点），判断为 **TD 侧的系统性问题**（OOC watcher /
触发逻辑在不同构建下行为不一致），不是 cwc 字段能修好的：
- 换 `logic_eram width`（48/49/50/53/54/55）、`section_size`、整份重生成、删 disabled 总线，
  全部要么 `KIT-8445 (54/48)` 要么 `Chipwatcher code` 不一致而挂死；
- 保留 gate cwc 的总线/触发/时钟、只换 ERAM 行，是唯一一次成功过的组合，但不可复现。

**落地**：`fpga_probe_capture` 在 `disabled_net_count > 0` 时**立刻拒绝**（0.3 s），文案写清
"TD 批量回读不可靠：触发不返回；清空 disabled 重跑 stages:["all"]，或走 TD GUI"，
并保留 `allow_disabled_capture:true` 让人强行试。

**工程已恢复**：手动 syn+phy（plain insert）→ 4 总线/53 bit → `compiled.bit` 同步 →
生成 sidecar 10,313 B → `probe_capture` ok / 1024 点。

### 第十四轮（收尾）：CLI 路径全部排除，disabled 抓取阻塞在 TD 侧

最后一搏：place 之后跑 `setup_debugger <chip_debugger> -prj <prj>`，期望 TD 自己写出
`phy_1/cw/compiled.cwc`。实测：exit 0、日志无 KIT 输出、**cwc 未被创建/修改**（静默空操作）。
（gate 阶段调用会 coredump 0xE06D7363；bitgen 阶段此前也验证为空操作。）

至此 CLI 侧能试的都试完了：
1. 我们自己写 cwc（模板打补丁 / 整份生成 / 各种 width）→ code 不一致挂死 或 KIT-8445(54/48)；
2. 让 TD 自己写（insert_debugger -cwc / setup_debugger）→ 前者只有唯一一次成功且不可复现，
   后者在任何阶段都是空操作；
3. 交叉验证（2 次重建 × 3 份 cwc）→ 3/3 失败。

**结论：disabled 位流的抓取在 TD 2026.2 的批量流程里不可用**（GUI 可能可以）。
后端已按"快速失败 + 明确指引"处理，并保留了唯一成功过的 sidecar 写法
（`WriteCompiledCwcFromGate`）与单测，等 TD 修复/换版本后可直接启用。
若要立刻用：`fpga_probe_config{"disabled":[]}` 清空后重跑 `stages:["all"]`，或走 GUI 抓。

### 第十五轮：用户 GUI 参照揭示的真实语义 + 两个根因

**用户给的 GUI 参照（4 条总线，gate 后把 reg6 的 Data 关掉、reg7 的 Instance 清空）** ——
TD 亲笔写的 `compiled.cwc` 说明：

| GUI 勾选 | cwc 表达 |
| --- | --- |
| Data ✓ / Trigger ✓ | net `data="enable" trig="enable"` |
| Data ✗ / Trigger ✓ | net `data="disable" trig="enable"`，仍进 `<signal_vec>`，`op="not_equal"` |
| Trigger ✗ / Data ✓ | net `trig="disable"` |
| Instance 空 / is_enabled=no | bus `is_enabled="no"` + **`inst=""`**，**不进** `<signal_vec>` |

`<logic_eram width>` 只计 **Data 使能**的位（32），`section_size=33`（=32+1）。
用它抓取**成功**（`ok=true`、1024 点、2 列）。

**两个根因（都实测）**：
1. `chip_debugger/user.cfg` 是 TD 的 watcher 实例配置，**优先于 user.mdn**。不删它，改 mdn 后
   gate 仍然按旧配置建 watcher（这就是用户必须"reset"的原因）。删掉后同一份 mdn 立刻建出
   `CWC_BUS_NUM=3 / DIN=48`。
2. 我们在 **phy 的 place** 也挂了 `insert_debugger` 钩子 —— 在 syn 已建过 watcher 的状态下会
   让 `opt_place` 直接崩（PRJ-8806，2 秒内退出）。不挂这个钩子 phy 完全正常。已改成**只在 syn 挂钩子**。

**runs 配置污染（用户明确要求修）**：`set_param -run ... tcl_beforestep/afterstep` 会写进 .al。
现在每次流程结束（成功/失败/取消）都会把这三个参数**清空**还原；`get_param` 回读的值在 TD 日志里
会被掩码成 "NNN"，所以不能回写原值，只能清空。

### 第十六轮：`chip_debugger/user.cfg` 才是 TD 真正的探针配置（重大发现）

**user.cfg 与 compiled.cwc 同格式**（9660 vs 10630 字节，差异只有 HMAC 注释、trigger-only 总线行、ERAM 行）。
TD 在 gate 阶段读的就是 `chip_debugger/user.cfg`，**优先级高于 user.mdn**：
- 不删/不更新它 → 改 mdn 后 gate 仍按旧配置建 watcher（用户必须"reset"的根因）；
- 删掉它 → 同一份 mdn 立刻建出 `CWC_BUS_NUM=3 / DIN=48`（实测）。

**成功的抓取组合（已实测 ok/1024 点）**：
> `user.cfg`（完整 TD 格式，带总线/时钟/触发）+ 把 `<logic_eram>` 换成**当前 phy bid** 的行
> （width = data 位数 48、section_size 49、正确 rid/wid）→ code `0101100101110111`、触发完成。

据此本轮落地：
- `EnsureProbeHook`（仅 syn）在跑流程前**按 user.mdn 重写 `chip_debugger/user.cfg`**（旧文件备份到
  `.fpgalsp/probe/user.cfg.bak`），从此不再依赖 `-cwc`，也不再需要用户手动 reset；
- `WriteCompiledCwcAfterPhy` **优先用 user.cfg 当模板** + 新 phy bid 的 ERAM 行写 sidecar
  （模板没有 `<logic_eram>` 时自动补在 `</instance>` 之前）；
- 流程结束仍会把 syn/phy 的 tcl 钩子参数清空还原（不污染 .al 的 runs 配置）。

**仍未稳定**：自动生成的 sidecar 与手工那份 `Chipwatcher code` 相同（`0101100101110111`），
但抓取时 TD 仍会在 `wait_on_hw_cw` 崩/挂；手工那次（同一 code）能成功。差异尚未定位，
怀疑与「TD 建 watcher 时读到的 user.cfg 是否带 ERAM」有关（下一轮做 A/B）。

### 第十七轮（收尾）：**disabled 位流抓取打通** —— 关键是给 TD 一份完整的 user.cfg

**最终机制（实测 2026-11-22 00:31）**：
1. 按 user.mdn + disabled 计划写 **`chip_debugger/user.cfg`**（完整 TD 格式：总线/net 的
   data/trig 标志、clock、config、signal_vec、trigger、`<logic_eram>`）；
   - 模板优先用现有 `phy_1/cw/compiled.cwc`/`user.cfg`（TD 自己写的）；
   - 模板总线集合与 mdn 不一致时**从零生成**（ERAM 行取当前 `phy_1/<prj>_inst.bid`）；
   - 旧文件备份到 `.fpgalsp/probe/user.cfg.bak`。
2. 跑 `stages:["all"]`：TD 在 **phy** 阶段自己插入 watcher，并**自己写出**
   `phy_1/cw/compiled.cwc`（这是之前一直没拿到 sidecar 的真正原因）。
3. `fpga_probe_capture` 直接用它：`ok=true`、1024 点。

**实测结果**（mdn = pwm_reg0/1/6，disabled = pwm_reg6_i）：
```
config: disabled=16 enabled_bits=32
flow ok=true
user.cfg 8310 B（我们写） -> TD 写 compiled.cwc 7962 B
cwc: inst=""×1, is_enabled="no"×1, data="disable"×16, logic_eram width=32
status: present=true nets=48 enabled=32 disabled=16 stale=false
capture: ok=true samples=1024 columns=[pwm_reg0_i, pwm_reg1_i]   ← 被 disable 的 reg6 不在数据列
```

**结论**：`KIT-8445 (54/48)` 的根因是"我们自己拼的 cwc 与位流几何不一致"。
只要让 TD 用完整 user.cfg 自己生成 sidecar，disabled 位流完全可以抓。
`probe_capture` 的 disabled 拦截一并移除（保留 `allow_disabled_capture` 参数兼容旧调用）。
单测 `ProbeUserCfg.PatchesTemplateAndFallsBackWhenBusSetDiffers` 覆盖写入规则。

### 第十八轮：状态目录彻底移出工程（全部落到系统 temp）

用户要求：插件/后端不要再往工程目录写 `.fpgalsp`。

改动：
1. **C++ 侧默认值改掉**：`platform::state_dir()` 未设 `FPGALSP_STATE_DIR` 时不再返回
   `<project>/.fpgalsp`，而是 `<os tmp>/fpgalsp/<工程目录名>-<hash8>`；
   hash = FNV-1a 32（输入：小写、正斜杠的绝对路径），**与插件 `src/stateDir.js` 算法逐字节一致**，
   所以插件不传环境变量时两侧仍然落到同一目录。
2. **所有硬编码写入点收口到 `state_dir()`**：
   `mcp/bridge.cpp`（TD 会话 work_dir、flow 根）、`server/commands.cpp`（analysis / device cache）、
   `server/lsp_server.cpp`（run dir）、`flow/strategy.cpp`（compare 工作根）、
   `flow/probe.cpp`（probe 历史目录、生成配置时的 cwd）。
3. **只读回退保留**：`ResolveSampleClock` 仍会读旧的 `<project>/.fpgalsp/probe/*.cwc`
   （采样时钟只能从历史 cwc 取），但**不再往那里写**。
4. 文案更新：guide/tools/错误信息里 `.fpgalsp/probe/config.txt` → "state dir 的 probe/config.txt
   （默认 %TEMP%\\fpgalsp\\<project>-<hash>）"。

实测（裸后端、不传 FPGALSP_STATE_DIR、先把工程里的 .fpgalsp 移走）：
```
probe_status ok=True present=True nets=48
project .fpgalsp recreated? False
temp has project dir? True
```
即：后端默认只写 `%TEMP%\fpgalsp\corpus_delta-36bad370\{logs,flow,cache,probe,run}`，
工程目录保持干净。旧工程 `.fpgalsp` 里的 3 个历史 cwc 已迁到 temp 的 probe/ 后删除备份。

`ctest` 64/64、插件 44/44。

### 第十九轮：采样时钟改为"从工程直接问 TD"（不再依赖任何历史 cwc）

用户要求：去掉对旧 `.fpgalsp` 的只读回退；采样时钟应该能从用户工程里直接读。

改动：
1. **删掉 legacy 回退**：`ResolveSampleClock` 不再扫描 `<工程>/.fpgalsp/probe`。
2. **新增 `QuerySampleClockFromGateDb(td, project)`**：起一个 TD 会话
   `import_device → cd <工程> → open_project -single_run → import_db <gate.db> → report_cwc_status`，
   解析 `auto_created_cwc* | depth | Clock | ...` 那一行的时钟列。
   结果按 gate.db 的 (mtime,size) 缓存在 `<state>/probe/clock-cache.txt`，重复调用不再起 TD。
3. `Capture` 在没有 sidecar、配置里也没有 clock 时**自动**调用它，并写回
   `probe/config.txt` 的 `clock=`，后续 flow 写 user.cfg 直接复用。
4. 时钟解析顺序：`config.txt` 的 clock → phy sidecar（TD 自己写的 compiled.cwc）→
   `report_cwc_status`（工程 gate 库）→ 工程 `cwc/*.cwc`。

顺带修一个由"状态目录移到 temp"引入的问题：TD 解析 `.al` 里的相对路径以它自己的 cwd 为基准，
会话 cwd 在 temp 时会报 `HDL-8007 cannot open Verilog file '/al_ip/...'`。
现在在 `open_project` 前显式 `cd {<工程目录>}`（实测不会往工程里写日志）。

**实测**（把 sidecar、probe 历史 cwc、clock-cache、config 里的 clock 全删掉后）：
```
probe_config bits=32
capture ok=True samples=1024 clock_from_td=reg_init/clk columns=[pwm_reg0_i, pwm_reg1_i]
config after: clock=reg_init/clk
```
即：采样时钟确实是从工程的 gate 数据库里问出来的，不依赖任何历史文件。

`ctest` 64/64、插件 44/44。

### 第二十轮：`report_cwc_status`（gate 库）拿不到时的行为 + 两处收敛

**行为矩阵（实测）**：

| 场景 | 结果 |
| --- | --- |
| 无 gate 库、无 cwc、无历史 cwc（`generate_cwc:true`） | 立刻拒绝（precheck），`clock_query` 明确写 `no <...>_gate.db - run stages:["all"] first`；`reason` 提示两条出路（跑 `all` 或 `probe_config{clock:...}`） |
| 无 gate 库，但**有**旧 user.cfg/cwc（模板总线集与 mdn 一致） | 仍可工作：模板里带 clock，只按 disabled 打补丁 |
| 无 gate 库 → 跑 `stages:["all"]` | flow **照常成功**（hook 写 user.cfg 失败只留痕），TD 按 user.mdn 自动建 watcher 并写 `phy_1/cw/compiled.cwc`；实测 `nets=32 stale=false`、`capture ok / 1024 点` |

两处顺带收敛：
1. `probe_capture`：时钟查不到时不再回 `empty ChipWatcher configuration`，直接给出
   "cannot determine the ChipWatcher sample clock: …" + 两条可执行建议。
2. flow hook：写不出 user.cfg 时**把旧 user.cfg 移到 `<state>/probe/user.cfg.stale.bak`**，
   让 TD 回退到"按 user.mdn 自动建 watcher"。之前留着旧文件会导致 TD 按旧配置建 watcher、
   抓取时判 stale 直接拒绝（实测踩到）。

### 第二十一轮：TD 会话 cwd 对齐 GUI（= `.al` 所在目录）+ 格式化后高亮错乱的修复

用户澄清：**TD GUI 的会话 cwd 就是 `.al` 所在目录**，所以它把 `<prj>_Runs/`、`log/`、`chip_debugger/`
都写在 `.al` 同级；"不污染工作目录"指的是**我们插件/后端不要额外输出**，TD 自己的行为照旧。
实测确认：`%WORKSPACE%\CorpusDelta\CorpusDelta_Runs` 与 `corpus_delta.al`
同级；`%WORKSPACE%\CorpusAlpha\demo_project_Runs` 与 `demo_project.al` 同级，规则一致。

改动：
1. **所有 TD 会话 cwd 改为 `<project.dir>`（.al 所在目录）**：
   `flow_runner`（TdFlowSession）、`probe.cpp`（抓取会话 + 时钟查询）、`mcp/bridge.cpp`、
   `server/lsp_server.cpp` 的 RunDir。**我们自己的**文件（LSP/MCP 日志、probe 配置、
   clock 缓存、flow 记录）仍在 `%TEMP%\fpgalsp\<project>-<hash>`。
2. 删掉之前为"cwd 在 temp"加的 `cd {工程目录}` 兜底（现在 cwd 本身就是它）。
3. **修复格式化后语法高亮错乱**：`HdlSideTokens` 里从**索引**取的 module/port/instance
   token 现在先做**文本校验**（该位置的文本必须等于索引里的名字），不一致就丢弃。
   根因：格式化会整体挪动行号，而索引要等重新解析才更新；照抄旧行号就会把
   class/property/function 画到错位置 —— 用户看到"切走再切回来又好了"正是因为
   期间索引被重新解析、token 回到正确位置。新增单测
   `SemanticsTokens.StaleIndexDoesNotMisplaceTokens`。

**实测（小工程 %WORKSPACE%\CorpusAlpha）**：`stages:["syn"]` → `flow ok`，`demo_project_Runs/`（含
`syn_1/phy_1/best_result`）与 `log/` 都出现在 `.al` 同级，和 TD GUI 一致。

`ctest` 64/64、插件 44/44。

### 第二十二轮：CorpusAlpha 上跑通 mark_debug → gate → phy（探针确实进了位流）

用户要求换小工程验证（大工程编译太慢），且当前接的板子不是 EF2L45，只跑到 phy。

**实测（%WORKSPACE%\CorpusAlpha，EF2L45LG144B，top=top）**：
```
probe_config: 2 总线 / 32 bit，unresolved=[]
flow_run stages:["all"] -> flow ok
phy_1/demo_project.bit 163,906 B
syn_1/cw: auto_created_cwc1, auto_created_cwc2, debug_hub
phy_1/demo_project_inst.bid 里有 auto_created_cwc1/wrapper_cwc_top/U_cwc_ram/mem_syn_*
phy_1/demo_project_phy.area 里 auto_created_cwc1 实例 526 LUT
probe_verify: cntT_q embedded 16/16、d1_tx_s1 embedded 16/16、missing=[]
```
即：**mark_debug（Read Design 之后）→ gate 之后建 watcher → phy 把采集 RAM 布进位流**这条链路在小工程上是通的。

**这轮发现并修掉的三个问题**（都是 EF2 系列暴露的）：
1. 时钟查询把器件库写死成 `ph1_100.db` → EF2L45 上 import 失败、拿不到 watcher。
   改为 `td::DbCandidates(...)` 逐个候选（`ef2_1.db` 会报 `USR-8001 not support package`，
   必须继续试 `ef2_4.db`），且**每个候选都要新起会话**（import 失败时 TD 直接退出）。
   修完在 CorpusAlpha 上拿到 `clock_from_td = clk`。
2. **TD 会按时钟域把 watcher 拆成多个实例**（实测：`cntT_q`→`clk`→cwc1，
   `d1_tx_s1`→`trans/clk`→cwc2）。`probe_status` 现在返回 `instances[]`，
   时钟查询也收集全部实例；抓取侧需要按实例指定（`instance` 参数已存在）。
3. `probe_verify` 以前只认 `compiled.cwc`，没有它就把所有总线判成 `dropped/no-bitstream` ——
   但批量流程里 TD 常常不写 cwc，探针其实已经在位流里。现在先看 `phy_1/<prj>_inst.bid`
   里的 `U_cwc_ram`：有就按"embedded"上报（逐位无法核对时以 RAM 存在为准），
   `probe_status` 也加了 `bit_has_watcher` 与对应的 `note`。

**仍待做**：多实例下的 per-bus 状态与抓取（需要一个实例一份 cwc），以及回到 corpus_delta 工程复验。

### 第二十三轮：插件补上 ChipWatcher 采样时钟/深度配置 + 分步 Run/Reset run（进行中）

用户指出插件里看不到"采样时钟"配置。现在补上（后端与 MCP 早已支持，缺的是 UI）：

- **ChipWatcher 树的 Actions** 新增两行：
  - `Set sample clock…`（显示当前值）→ 输入框 → `probe_config{clock}` → 提示 `stages:["all"]` 重跑
  - `Set sample depth…`（显示当前值，校验 128..65536）→ `probe_config{depth}`
- `extension.js` 的 `applyConfig` 现在把 `clock`/`depth` 透传给后端（之前只发 signals/disabled）。
- `package.json` 声明 `fpgaLsp.probeSetClock` / `fpgaLsp.probeSetDepth`；`test/run.js` 增加断言。

同时开始的 Flow 分步控制（后端已落地，插件菜单待接）：
- `FlowRunner::StartFromStep(step, force)`：`force=false` 时该步已完成则 `{ok:true,skipped:true}`；
  `force=true` 清掉该步及下游标记后重跑（对应 GUI 的 Run / Reset run）。
- **跑 syn 时把 phy 的三步标记与 watcher 产物一起清掉** → Flow 视图里 phy 立刻回到 not run
  （这正是用户反馈"点了 run synthesis，phy 状态还显示完成"的根因）。
- MCP 新增工具 `fpga_flow_run_step{step,force,jobs}`。

### 第二十四轮：采样时钟改成"像 GUI 一样从列表里选"

用户指出：TD GUI 的采样时钟是**下拉选**，不该让人手输。研究结论：

- TD 把时钟树导出成 `phy_1/route_clock_utilization.txt`（`place_clock_utilization.txt` 同理），
  里面每个全局时钟都带**真实时钟网名**：实测 corpus_delta 工程能拿到
  `reg_init/clk`、`pll_100M/clk0_buf`、`ANPC_init0/ANPC_SVPWM_init/pwm_clk`、
  `rgmii_interface_init/…` 等 **49 个**候选，当前用的是 `reg_init/clk`（与 GUI 的 cwc 一致）。
- 其它命令（`report_clock_summary` / `<prj>_gate.clktree`）只给时钟组名或 PLL 引脚名
  （`pll_100M/pll_inst.clkc[0]`），不是可用的采样时钟网名，所以不用它们。

实现：
1. 后端 `probe::ClockCandidatesFromReports(project)`：解析 `phy_1/{route,place}_clock_utilization.txt`
   里的时钟网名（过滤 `PH1_PHY*`、`.clkc`、纯数字），去重。纯文件读取，零成本、不碰硬件。
2. MCP 新工具 **`fpga_probe_clocks`**（工具总数 49→50）：返回 `clocks[{name,current}]` + `current` + `source`；
   没有时钟树报告时给出可执行提示（先跑一次 `stages:["all"]`）。
3. LSP 命令 `fpgalsp.probeClocks`（插件用）。
4. 插件 `Set sample clock…` 改成 **QuickPick 列表**（当前值标 `current`，末尾保留 "Enter manually…" 兜底）；
   拿不到列表时退回输入框并说明原因。

实测：`fpga_probe_clocks` 在 corpus_delta 上返回 49 个候选、`current=reg_init/clk`。

`ctest` 64/64、插件 44/44。

### 第二十五轮：Flow 六步右键 Run / Reset run（完成）+ 多实例抓取（第一半）

**A. Flow 六步右键菜单（已完成并验证）**

- `flowTree.js`：六个 GUI 步骤节点带 `contextValue='fpgaFlowStep'` 与 `flowStep` id
  （`GUI_STEP_IDS` 把 `Read Design` … `Generate Bitstream` 映射成
  `read_design/opt_rtl/opt_gate/opt_place/opt_route/bitgen`，只在这一处翻译）。
- `package.json`：新增 `fpgaLsp.flowStepRun` / `fpgaLsp.flowStepReset`，
  `view/item/context` 里 `view == fpgaLsp.flowView && viewItem == fpgaFlowStep` 时出现。
- `extension.js`：两条命令都走 `fpgalsp.runFlowStep{step,force,jobs}`；
  `Run` 返回 `skipped:true` 时提示"已完成，用 Reset run 强制重跑"。
- 后端 `fpgalsp.runFlowStep` → `FlowRunner::StartFromStep`；`force=false` 已完成则跳过，
  `force=true` 清掉该步及下游标记后重跑。

实测（CorpusAlpha）：`fpga_flow_run_step{step:"opt_gate",force:false}` →
`{ok:true, skipped:true, reason:"step already completed (use reset to force a rerun)"}`。

**B. 多实例抓取（第一半已落地）**

- `probe::EnumerateWatcherInstances(project)`：从 `syn_1/cw/auto_created_cwc*` 读出
  每个实例的名字、目录、`CWC_BUS_NUM` / `CWC_BUS_DIN_NUM`。
- `probe_status` 的 `instances` 改为对象数组 + 新增 `instance_count` / `multi_instance_note`。
  实测 CorpusAlpha：`[{name:auto_created_cwc1,buses:1,bits:16},{name:auto_created_cwc2,buses:1,bits:16}]`，
  即"两个时钟域 = 两个 watcher"。
- `BuildCwcFromWatcher` 内部拆出 `BuildCwcFromWatcherImpl(..., watcher_dir)`，
  为"按实例生成 cwc"铺好路（旧签名保持不变）。

**下一步（B 的第二半）**：用 `report_cwc_status` 的节点表得到 `bus -> instance` 映射（缓存），
把 mdn 按实例切分后按实例生成 cwc，`probe_capture{instance}` 即可逐实例抓取。

### 第二十六轮：多实例抓取打通到触发阶段

链路（全部实测，工程 `%WORKSPACE%\CorpusAlpha`，两个时钟域）：

```
probe_capture{}                     -> refused: "one watcher per clock domain; pick one with instance"
probe_capture{instance:cwc1}        -> cwc_path=<state>/probe/cwc_auto_created_cwc1.cwc（4043 B，16 net）
                                       bit_path=<runs>/phy_1/demo_project.bit（sidecar 缺失时的回退）
                                       -> TD: config_chatcher 接受、run_hw_cw/wait_on_hw_cw
                                       -> KIT-8000 超时（板子是 PH1A100，EF2L45 没接 → 等不到触发，属预期）
```

实现要点：
1. **`QueryWatcherMap(td, project)`**：跑一次 `report_cwc_status`，解析节点表
   （`bus | MARK_DEBUG_PROBE | Yes | clock | instance`）与实例表，得到 `bus→instance` 与
   `instance→clock`；按 gate.db 的 (mtime,size) 缓存到 `<state>/probe/watcher-map.txt`。
   踩坑：报告行首带 `RUN-1001 :` 前缀，必须先剥掉再按 `|` 切分，否则 bus/instance 名全被污染。
2. **`EnsureCwcForInstance`**：把 mdn 按映射切成该实例的子集（总线名比较用"去层级/去位宽"的基名，
   `cntT_q[15:0]` 与 `top/cntT_q[0]` 要能对上），用该实例的 watcher 目录 + 该实例时钟生成 cwc，
   写到 `<state>/probe/cwc_<instance>.cwc`。`BuildCwcFromWatcherImpl` 增加 `clock_override`。
3. **Capture**：未指定 instance 且多实例 → 直接返回实例清单让人选；指定 instance → 用该实例 cwc，
   并且 stale 检查按实例作用域豁免（子集本来就和整份 mdn 不同）。
4. 位流回退：sidecar `cw/compiled.bit` 缺失时直接用 `phy_1/<prj>.bit`（批量流程常不写 sidecar）。

`ctest` 64/64、插件 44/44。

### 第二十七轮：真实板子上抓通（corpus_delta / PH1A100）

用户在 TD GUI 里重跑 flow 并 ChipWatcher 抓取成功之后，用同一个工程的同一份产物
验证**我们自己的** capture 链路（`%WORKSPACE%\CorpusDelta`，`corpus_delta.al`，
PH1A100SFG676，48 根探针 = `pwm_reg0_i/pwm_reg1_i/pwm_reg6_i` 三个 16 位总线，
单时钟域 `reg_init/clk`）。

**1. GUI 抓取留下的产物（这是 capture 能否跑起来的关键）**

GUI 会话 `log/td_2026-11-22_11-01-30.log` 末尾是 `connect_cable -allink`，
随之 `phy_1/cw/compiled.cwc`（59076 B）与 `phy_1/cw/compiled.data`（6035 B）
在 11:10:57 一起出现 —— 即 **sidecar 是 TD 在 ChipWatcher 上硬件时才写的**，
批量 flow（`launch_runs`）从头到尾都不写它。同目录 `compiled.cwc` 头部带
`<!-- HMAC is: ... -->`：TD 会给 cwc 签名，这正是"自己拼的 cwc 一定对不上"
（KIT-8445 / Chipwatcher code 不一致）的根因。`compiled.data` 是 XML
`<cwc_data>`，只有总线名/颜色/order 与空 `trigger_config`，不是采样数据。

**2. 我们后端在真实硬件上抓通（3/3 次）**

```
probe_status{}      -> ready:true, present:true, instance auto_created_cwc1,
                       clock reg_init/clk, depth 1024, 48 net / 3 bus, stale:false
capture{confirm}    -> ok, 3.8 s, downloaded:false, samples:1024, trigger_index:127
capture{trigger_position:512}
                    -> ok, 3.7 s, trigger_index:512（运行期改触发位置生效）
capture{trigger_now:false, bus_op:"equal", trigger_enabled:true,
        buses:[{name:"WHG/pwm_reg0_i",op:"equal",value:"0000..."}]}
                    -> ok, trigger_mode:"condition", trigger_index:127
```

三次都**没有重新下载**（器件里跑的就是 GUI 灌进去的那份带探针位流），CSV 落在
`phy_1/cw/capture_20261122_1114xx.csv`（1025 行 = 表头 + 1024 采样）。
之前那轮 `wait_on_hw_cw` 挂死/会话退出的现象在这台板子上没有复现 —— 当时是
EF2L45 的位流 + PH1A100 的板子，硬件与位流本来就不是一对。

采样值全是 0（`summary.distinct = 1`）：`pwm_reg0_i` 来自 `bus_reg reg_init`
（`WHG.v:892`，时钟 `clk`、复位 `reset_n_i`），寄存器文件没被总线写过、复位没释放，
数据就是 0 —— 这是"板子空转"，不是抓错位：触发行精确落在 position 指定的位置，
说明 cwc 与位流里的 watcher 几何一致。

**3. 批量流程能不能自己写 sidecar？实测不能（死路，记录免得再踩）**

复刻 GUI 的顺序跑批处理：

```
import_device ph1_100.db -package PH1A100SFG676
open_project {corpus_delta.al} -single_run
import_db {.../phy_1/corpus_delta_pr.db}     -> Import ChipWatcher / Import design success
config_chipwatcher                                -> TD 在 "start command" 之后就没了（静默退出，无 minidump）
```

`config_chipwatcher` 不带文件参数时，CLI 会话不是"静默空操作"而是**进程直接退出**，
所以 sidecar 只能来自 GUI（或 `apply_build_config` 写完整 `chip_debugger/user.cfg`，
让 TD 在 phy 阶段自己写）。这也解释了为什么在纯 CLI 跑完 flow 的工程上，
`probe_capture` 要么靠 `<state>/probe` 里历史 cwc，要么得 `generate_cwc`（未验签）。

**4. 本轮修掉的两个小问题**

- `probe_status` 的 `note` 误报：判据写成了 `out.value("present", false)`，而 `present`
  只在 `out["watcher"]` 子对象里 —— 于是"位流里有 watcher"的工程**永远**会收到
  "TD wrote no compiled.cwc"，哪怕 `ready:true`、cwc 就在那儿。改成看本工程的 `cwc.ok`。
- 运行期改触发条件用的临时 cwc 原来写在工程目录（`<al>/trigger_<ts>.cwc`），
  现在写到 `<state>/probe/trigger_<ts>.cwc` —— 插件不再往工程里留额外产物
  （工程里只留 capture 的 CSV，那是采集产物本身）。

`ctest` 64/64、插件 44/44。

### 第二十八轮：纯插件路径端到端打通（无 GUI）+ 六步右键的界面反馈

**A. 用户反馈：插件 Flow 里对 Read Design 右键 "Reset and run"，下游两步仍显示已完成、
FPGA Flow 没有任何输出**

根因在插件侧：`extension.js` 直接 `rpc.execute(COMMANDS.runFlowStep)`，绕过了
`FlowController` —— 没有输出通道、没有状态栏、没有进度通知、没有日志轮询，
结束后也不刷树。服务端其实**跑对了**（实测 CorpusAlpha 11:32:55 点击后 syn 三步在
11:32:56–11:33:10 全部重跑完，`.end.f` 时间戳全变），只是界面完全没反馈。

修法：新增 `FlowController.runStep(argument, force)`，和 `run()` 共用新的
`_startFlow({label, tool, callArgs, header, note})`：
- 输出通道里写 `▶ reset run Read Design (read_design, force, jobs=8)` 与
  "整条 run 会重跑、步骤标记先清后亮"的说明；
- 起跑后**立刻 poll 一次**（此刻标记已清，树马上显示 not run）；
- 10 s 兜底轮询 + `fpgaLsp/flowEvent` 推送照旧，结束后 `refreshAll()`；
- `skipped:true`（Run 已完成步）只提示"已完成，用 Reset run 强制"；
- 顺手修 `_logSeq` 跨 run 不重置 —— 上一轮的大 seq 会把新一轮日志全过滤掉，
  这也是"没有新输出"的一个隐蔽来源。

**B. 纯插件路径验证（corpus_delta，PH1A100，全程没有 TD GUI）**

清掉 `phy_1/cw`、步骤标记、`chip_debugger/user.cfg` 后，用 `fpga_flow_run{stages:["all"]}`
从零跑（= 插件里 Run Full Flow 的同一条路径）：

```
  2.2s [syn] syn_1: 生成 user.cfg 失败：generated config has no <phy_eram> rows
                (no phy bid yet) - leaving the watcher to TD (auto-created from user.mdn)
 12.8s Read Design done / 56.7s Optimize RTL done / 139.8s Optimize Gate done
452.6s [phy] phy_1: wrote phy_1/cw/compiled.cwc (depth 1024, 48 nets)
452.9s flow complete: Setup WNS 292ps, Hold WNS 20ps, bit 5231 KB
```

即：**没有 user.cfg 模板时，TD 按 user.mdn 自己建 watcher，phy 结束后由后端写 sidecar**
（`WriteCompiledCwcAfterPhy`）。但第一次 capture 失败：`Error: InternalError: TD session is closed`
（TD 在 `wait_on_hw_cw` 等不到触发后静默退出）。

**C. 那个失败的根因：`working_mode_depth` 不是采样深度**

对比 GUI 写的 cwc 和我们生成的 cwc，`<phy_eram>` 只有一处不同：

| | mem_syn_11 (20 位) | mem_syn_32 (20 位) | mem_syn_53 (9 位) |
| --- | --- | --- | --- |
| TD/GUI 写的 | 1024 | 1024 | **2048** |
| 我们写的 | 1024 | 1024 | 1024 |

`working_mode_depth` 是**这颗 RAM 自己的工作模式深度**（bid 的
`<working_mode><depth>`），不是采样深度：1024x48 的采集里，9 位宽那颗 RAM 被拼成
2048 深。`ParseBidErams` 以前 `wm_depth = TagText(sub_block, "depth")` 取到的是
sub_bid 的第一个 `<depth>`（=1024），于是永远等于采样深度。

后果很隐蔽：TD **照样接受**配置（`config_chipwatcher` 成功、`Chipwatcher code`
与 GUI 完全一致 = `0101100101110111`），`run_hw_cw -trigger_now` 也正常启动，
但 `wait_on_hw_cw` 永远等不到触发，30 s 后 TD 静默退出 —— 就是历史记录里反复出现的
"cwc 几何对了但抓不到"。

修：`ParseBidErams` 显式解析 `<working_mode>` 子块（depth/width/num_byte/ecc/address_step
都从这里取），生成器用 `eram.wm_depth`（空则退回采样深度）。

修完同一条路径：

```
capture{confirm:true, generate_cwc:true} -> ok, 1024 采样, trigger_index 127,
                                            downloaded:false, cwc_generated:true
（sidecar 里 mem_syn_53 working_mode_depth=2048，与 GUI 一致）
```

**D. 顺带**：`fpga_probe_capture` 在 TD 静默退出时不再抛 `InternalError`，改为返回
`{ok:false, stage:"session", reason:"the TD session died during the capture (...) - the
bitstream in the device most likely does not match this ChipWatcher configuration: retry
with download:true confirm:true, or re-run stages:[\"all\"]"}`。

`ctest` 64/64、插件 45/45（新增 runStep 回归）。

**E. 端到端复跑（同一晚上第二次，确认不是偶然）**

再清一次 `phy_1` 后重跑 `stages:["all"]`（452 s，12:02→12:09），这次 flow 在 phy 结束时
自己写出的 sidecar 已经是修好的几何（`mem_syn_53 working_mode_depth="2048"`），
随后**按插件的调用**（`probe_capture{confirm:true}`，不开 generate_cwc、不手动下载）：

```
-> ok, 1024 采样, trigger_index 127, downloaded:false
   csv: phy_1/cw/capture_20261122_121042.csv
```

即：插件的 "Run Full Flow" → "Capture" 两步，全程不需要 TD GUI、不需要 `generate_cwc`
开关，也不需要手动 download。

### 第二十九轮：TD 短会话日志不再落在工程根目录

用户反馈：TD 短会话日志散落在工程根目录。根因是两件"正确行为"叠在一起：

1. 会话 cwd 必须是 `.al` 所在目录（第二十一轮：否则 `.al` 里的相对路径解析失败，
   `HDL-8007` / `PRJ-8806`），而 TD 把每个会话的日志写在 `<cwd>/td_<YYYYMMDD_HHMMSS>.log`
   （崩溃时还有 `minidump-*.dmp`）——于是每一次 capture / 时钟查询 / watcher 查询
   都在工程根目录留一对文件；
2. TD GUI 自己也是这么写的，但它把日志放进 `<al>/log/`（GUI 会话日志带横线：
   `log/td_2026-11-22_11-01-30.log`）。我们之前没有跟着归位。

**修法：`td::TdSessionOptions::log_dir`**

- 会话开始前记下 cwd 里已有的 `td_*.log/.logw`、`minidump-*.dmp`；
- 会话结束（`Kill()`，也就是 `Close()`/析构/fail 路径）时，**只把本次会话新产生的**那些
  挪到 `log_dir`（先 `rename`，跨卷失败退回复制+删除，重试 3 次等 TD 释放句柄）；
- 传递：flow 会话 / capture / 时钟查询 / watcher 查询 / bridge 的 TD 服务会话
  都指向 `<al>/log`；cwd 本来就在 temp 或 state dir 里的会话（analysis、strategy、
  lsp_server）不需要挪，保持空。

`fpga_flow_log` 相应改成两处都读：状态目录里的 flow run 目录（老落点）+ `<al>/log`，
按 mtime 取最近 12 份，并在每份上标 `source`（`run-dir` / `project-log`）。

**实测**（corpus_delta）：capture 一次后工程根目录零新增，`log/` 下出现
`td_20261122_121901.log` + `.logw`；capture 本身照旧 `ok / 1024 采样`。

**顺手清干净历史遗留**（移动到 state 目录后删除）：

- corpus_delta 工作区根 + `.al` 目录：`td_*.log/.logw` 60 个 -> `<al>/log/`；
- CorpusAlpha 根：`td_*.log/.logw` + `minidump-*.dmp` 131 个 -> `log/`；
- 旧版本留下的 `<workspace>/.fpgalsp/`（corpus_delta、CorpusAlpha）和 e2e 备份
  `corpus_delta.al.bak_e2e` -> `<state>/legacy/`。

`ctest` 64/64、插件 45/45。

### 第三十轮：在**真实 VS Code**里验证六步右键（用户："还是没修复"）

用户反馈 0.21.4 之后仍然"点了没输出、下游两步还是已完成"。这次不再靠单测，直接把
`scripts/run-vscode-test.ps1` 扩成真实 extension-host 集成测试：真 VS Code 窗口 +
真扩展代码 + 真后端 + 真 TD（工程 CorpusAlpha，syn 15 s）。

新增断言（`test/vscode-suite/index.js`）：

```
[PASS] flow controller exposed for tests
[PASS] Reset and Run resolves after the flow finished                 {"ok":true}
[PASS] reset run re-ran the whole syn run (all three step marks rewritten)   16.0s
[PASS] FPGA Flow output shows the step run header
       [12:35:16] ▶ reset run Read Design (read_design, force, jobs=6) · fpga-lsp 0.21.5
[PASS] FPGA Flow output shows per-step progress                       13:20 Collecting reports ✓ done
[PASS] flow controller returns to idle after the run
[PASS] status bar reports done/idle                                   $(check) FPGA: Read Design done
```

结论：**代码路径本身是通的**（0.21.4 的修法有效）。用户看到"没反应"最可能是窗口还跑着
旧 extension host —— 证据：那台机器上的 `%WORKSPACE%\CorpusDelta` 窗口最后一次激活是
**2026-11-21 18:16 UTC（= 11-22 02:16）的 0.20.6**，而 0.20.6 里根本没有
`FlowController.runStep`，点右键只会发一条 RPC 然后什么都不显示。

为了让"装了新版但窗口还是旧代码"这种现象**一眼可见**：

- 输出通道的每一条 flow 头都带上版本号：`▶ reset run Read Design (…) · fpga-lsp 0.21.6`；
  看不到这一行 = 窗口跑的不是这份代码；
- `activate()` 返回 `{flowController, flowProvider, moduleProvider, reportsTreeProvider, probeTree}`
  供集成测试/调试观察（只读暴露）。

顺带修掉集成测试里两处**过期的状态目录断言**（还在读 `<project>/.fpgalsp/logs/*`，
第二十四轮起状态目录已经移到系统 temp）：现在用扩展自己的 `stateDir.resolveStateDir()`。
改完后该套件从 7 个失败降到 3 个（`completion` 标签数 / `copy symbol path` / `doctor`
三个是历史遗留问题，与本轮无关，已记录）。

`ctest` 64/64（后端未改）、插件 45/45、集成套件新增 7 条断言全 PASS。

### 第三十一轮：右键菜单参数形状错了 —— 真正的根因（用户截图定位）

用户截图里的提示是 `FPGA: right-click one of the six flow steps.` —— 这句话正是
`runStep` 在 `step` 为空时的早退提示。也就是说：**命令收到了参数，但 `item.flowStep` 是空的**。

原因：`view/item/context` 的命令参数是**树的数据节点**（`getChildren()` 返回的 element），
不是 `getTreeItem()` 造出来的 `TreeItem`：

| 形状 | `step`/`stepName` | `flowStep`/`flowStepName` |
| --- | --- | --- |
| 树节点（`stageNode()`，命令实际收到的） | ✔ `step: 'read_design'` | ✘ 没有 |
| TreeItem（`getTreeItem()` 返回的） | ✘ 没有 | ✔ `flowStep: 'read_design'` |

0.21.4 的修法只读了 `item.flowStep`，所以从菜单点进来永远是空字符串 → 早退 →
"没输出、下游步骤没变"。之前那条集成测试之所以 PASS，是因为测试自己手搓了一个
**TreeItem 形状**的参数（`{flowStep:'read_design'}`），把真实现场的形状写错了 —— 这是本轮
最该记的教训：集成测试的入参必须用真实来源的形状。

修法（`FlowController.runStep`）：两种形状都认
（`step | node.step | flowStep | node.flowStep`，名字同理），并且只接受六个合法 id；
不合法时提示里写明"没有传 step"还是"传了不认识的 step"。

测试也一并改正：单测/集成测试都用**树节点形状**
（`{kind:'stage', step:'read_design', stepName:'Read Design', label:'Read Design · 1s'}`），
并补一条"点到了非步骤行"的用例。

真 VS Code 集成套件（节点形状参数，CorpusAlpha，15.9 s）：

```
[PASS] Reset and Run resolves after the flow finished                 {"ok":true}
[PASS] reset run re-ran the whole syn run (all three step marks rewritten)  15.9s
[PASS] FPGA Flow output shows the step run header   ▶ reset run Read Design (…) · fpga-lsp 0.21.7
[PASS] flow controller returns to idle after the run
[PASS] status bar reports done/idle
```

插件 45/45、集成套件除 3 条历史遗留（completion 标签数 / copy symbol path / doctor）外全 PASS。

### 第三十二轮：右键 Reset and Run 改成**真的只跑这一步**

用户反馈（0.21.7 之后，命令已经能跑起来了）：
1. 右键 Read Design → Reset and Run，运行中后面两步（Optimize RTL / Optimize Gate）还是 done；
2. 本来应该只跑 Read Design，但 syn 三步全跑了。

**根因**：`StartFromStep` 一直是"reset 整条 run + launch 整条 run"：

```
ResetDownstreamOf(run)        // 清掉 syn 三步 + phy 全部标记
Start({"syn"})                // launch_runs syn_1  -> 三步全跑
```

所以（1）下游两步在跑完后必然又变 done（运行中也被带着跑），（2）Read Design 一步的点击
实际跑了三步。TD 的 CLI 里确实支持单步：

```
launch_runs syn_1 -step read_design -jobs N     # 只跑这一步
```

但它要求这一步"还没跑过"，否则 `PRJ-8816 ERROR: Step read_design is already run.`
而且 —— 这轮踩到的坑 —— **TD 在 `open_project` 时就把步骤状态读进内存**：

| 顺序 | 结果 |
| --- | --- |
| 先 `file delete` 标记，再 `open_project` + `launch_runs -step` | ✅ 正常只跑那一步 |
| `open_project` 之后才 `file delete`，再 `launch_runs -step` | ❌ PRJ-8816 "already run" |

（两种顺序都在 CorpusAlpha 上用 TD 的 Tcl 实测过；`reset_runs` 不接受 `-step`（USR-8011），
`reset_run` 在 CLI 里根本不存在（invalid command name），所以标记只能由我们删。）

**新语义**（`fpga_flow_run_step` / 插件右键 "Reset and Run This Flow Step"）：

1. 在 **open_project 之前**清掉"这一步及其之后"的标记（前面的步骤产物保留）；
   syn 任一步都会让整个 phy run 作废（标记 + watcher 旁路 + 旧 bit）；
2. `launch_runs <run> -step <step>` —— 只有这一步跑；
3. 下游步骤保持 not run（插件树里显示 `not run`），不会再被带着跑；
4. phy 的 sidecar 只在跑到 bitgen 时写（place/route 单步时写会拿旧 bid 的几何）；
5. 单步 phy 不做 `save_best_bits`；单步模式也不再"探针清单比网表新就自动补 syn"。

**实测**：

```
corpus_delta:  step=read_design -> 只 read_design，10 s，opt_rtl/opt_gate 变 not run，phy 全部清空
corpus_delta:  step=opt_rtl     -> read_design 保留，只 opt_rtl，44 s，opt_gate 仍是 not run
CorpusAlpha 真 VS Code 集成套件：
  [PASS] reset and run re-ran ONLY the selected step            2.9s
  [PASS] downstream steps are cleared to "not run" (no re-run)  opt_rtl=0 opt_gate=0
  [PASS] FPGA Flow output shows the step run progress           ✓ Read Design finished after 2.9s
```

插件 45/45、`ctest` 64/64；集成套件只剩 3 条历史遗留（completion 标签数 / copy symbol path /
doctor，与本轮无关）。

### 第三十三轮：`launch_runs -jobs N` 到底是什么 —— 以及 GUI 的默认值

用户问：`-jobs N` 的语义是什么？TD GUI 默认多少？

**TD 手册（`doc/TD_User_Guide_V6.2.pdf` §9.3.14）给的是权威答案**：

| 消息号 | 说明 | 建议 |
| --- | --- | --- |
| PRJ-8802 | multirun 最大并发数为 6 | 设置一个 1~6 之间的 jobs 值 |
| PRJ-8803 | 单步执行时，最大并发数只能为 1 | 设置 jobs 为 1 |

即：`-jobs` 是 **multirun 同时并行跑的 run 数（并发上限）**，合法范围 1~6；
**单步执行（`-step`）时只能是 1**。它**不是**单个 run 内部的线程数 —— 后者由 TD 自己按
CPU 核数决定（run.log 里的 `TMR-1041 : Net delay calculation accelerates with 32 threads`、
`PHY-1001 ... (704.4%)` 都跟 `-jobs` 无关）。

**TD GUI 的默认值**（从 GUI 自己的会话日志实测，不是猜的）：

| GUI 动作 | 日志里的命令 | 默认 jobs |
| --- | --- | --- |
| 交互式跑整条/两个 run | `PRJ-1409 : launch_runs syn_1 phy_1 -jobs 6.` | **6** |
| GUI 单步（Run this step） | `PRJ-1411 : launch_runs syn_1 -step opt_gate.`（不带 `-jobs`） | 等价 **1**（PRJ-8803 要求） |
| GUI 生成的批处理脚本 `multirun_script.tcl` | `launch_runs { syn_1 phy_1 } -jobs 2` | **2**（模板值） |

**我们这边的三个问题，本轮一起修掉**：

1. 单步模式仍把用户给的 6/8 原样传给 `launch_runs -step`，违反 PRJ-8803（TD 目前容忍，
   但按手册就是错的）→ 单步固定 `-jobs 1`；
2. `fpga_flow_run{jobs}` 的 schema 允许 1~64（Python 参考实现也是 64），超出 TD 的 1~6
   → C++ 与 Python 参考实现都改成 1~6，文案写清楚"并发 run 数，不是线程数"；
   插件设置 `fpgaLsp.flowJobs` 同步改成 1~6（默认 6 = GUI 默认）；
3. 顺带修 `fpga_flow_log`：第二十九轮把 TD 会话日志归位到 `<flow 目录>/log/` 之后，
   它还在只看 flow 目录根 → e2e `mcp_flow_smoke` 的"应该有日志文件"失败。现在
   `<flow>/log`、`<al>/log`、flow 目录三处都看。

另外把 `flow.jobs` 透出的值改成**实际生效值**（钳到 1~6），不再回显用户请求的 64。

实测：CorpusAlpha `step=read_design,jobs=8` → 日志里 `launch_runs  syn_1  -step read_design -jobs 1`
→ ok / 2.9 s；`ctest` 64/64、插件 45/45。

---

## 第三十四轮（2026-11-22）：团队模式独立审计 —— 功能齐全度 + 文档同步度

**做法**：5 个子 agent 各自独立只读审计（互不共享上下文），lead 汇总、定级、修复、跑回归。
报告归档在 [audit/2026-11-22/](audit/2026-11-22/SUMMARY.md)（5 份 + 汇总）。基线是 0.21.9
（HEAD `36f5123`）；本轮交付 **0.22.0**。

**审计结论**：

| 维度 | 结论 | 关键数字 |
| --- | --- | --- |
| C++ 后端 vs Python 参考 | 契约层 100% 对等，C++ 是严格超集 | 39 工具 schema 逐字段一致；C++：50 MCP 工具 / 33 LSP handler / 34 executeCommand |
| 后端实现健康度 | 50 工具 × 17 组 LSP 能力**全部有真实实现**（0 空转） | `ctest` 64/64 |
| 插件 | 58 命令「声明 = 注册 = 实现」，0 死命令、0 空壳；4 视图 12 面板全绑真实数据 | `node test/run.js` 45/45 |
| 文档 | **不同步**：过时 25 / 缺失 13 / 多余 3 / 矛盾 5；4 blocker、11 major | 用户文档整体落后 2~3 个版本 |
| 真机运行期 | 能跑（LSP 18/20 方法、MCP 13 只读工具返回真实数据），但 2 个 major 缺陷 | 单步 3.2 s；退出码 0、无残留进程 |

**本轮修掉的代码缺陷**（每条都有复现脚本或单测钉住）：

1. **major：同一 MCP 会话第二次 `fpga_flow_run_step` 永久挂起**（后续 `flow_status` 一起卡死）。
   根因：心跳线程只看 `state.running`，而 `StartInternal` 在 join 上一个 worker **之前**就把状态
   重置成 `running=true / stage=starting` → 旧心跳被"复活"→ join 等心跳、心跳等状态，死锁。
   修法：心跳带 generation 代号（`heartbeat_generation` 每次 Start 递增）；新增单测
   `FlowRunner.SecondStartAfterCompletionDoesNotHang`。真机复现：修前第 2 次调用 40 s+ 无响应；
   修后 3 次调用全 ok、`exit: 0`。
2. **major：LSP 路径的 TD 短会话日志仍落工程根**（`fpgalsp.device` 每次 30 KB）。
   根因：`LspServer::CreateSession` 只传 `work_dir` 没传 `log_dir`（flow/probe/bridge 三条路径都传了）。
   修法：`log_dir = <al>/log`。真机 A/B：工程根 0 新增、`<al>\log` 新增 `td_*.log/.logw`。
3. **minor：单步 Reset and Run 后下游步骤仍显示 done**。根因：`CollectFlowSteps` 会用
   "统计表里有耗时"把 pending 升级成 done，而单步只删标记文件没清表。
   修法：`StripFlowStatusFromMarker()` + `ClearStepMarkersFrom` 同步过滤 `flow.status`/`*_flow.status`；
   真机：Read Design=done、其余 5 步 pending。
4. **minor：`fpgaLsp.verible.autofix` / `verible.highlight` 是空开关** → 服务端新增
   `Settings::verible_autofix/verible_highlight` 并在 code action / semanticTokens 处消费。
5. **nit：版本叙事分裂**（exe 自报 0.1.0 / 扩展 0.21.9）→ `src/platform/version.h` 单一来源，
   CMake、header、`package.json` 三处 0.22.0，新增 CTest `docs_version_consistency`（label `docs`）。
6. **jobs 钳位统一**：bridge 侧从 `clamp(1,64)`（死代码）改为与 `FlowRunner` 一致
   （>0 钳 1~6、0/负数 = 默认 6）；单步仍强制 1。
7. **插件**：`onDidChangeConfiguration` 首行 `if (!mcpEmitter) return;` 短路（默认配置下
   `dynamicProvider=false` → 改 `allowFlow/allowWrite` 后状态栏徽标永不更新）已去掉；
   `FPGA: Re-run Place && Route` 标题与实际（只跑 route）不符 → 改 `Re-run Routing`。

**文档同步**：`README.md`、`doc/README.md`、`doc/commands.md`、`doc/mcp-usage.md`、
`doc/protocol.md`、`doc/build.md`、`doc/design/README.md`、`doc/extension-development.md`、
`vscode-extension/README.md` 按代码事实重写：MCP 50 工具（`--allow-flow` 46 / 只读 38 /
`--allow-write` +3）、LSP 33 handler / 34 命令 / 广播 31、58 个 VS Code 命令、5 个 lm 工具、
`-jobs` 语义表、单步 Flow 语义、日志与状态目录表、`--mcp --mcp-port` 现在真的开端口
（`--mcp` 分支也起 loopback，每连接自建 Bridge）、插件 README 删掉两条不存在的命令。

**验证**：`ctest --preset windows-msvc-dev` 65/65（`e2e.mcp_concurrency` 首跑失败、重跑通过 ——
仍是已知的时序 flake，`-R` 重跑稳定通过）；插件 45/45；`tests/check_versions.py` 通过；
真机 CorpusAlpha：单步 3.2 s + 三次连续调用无挂起 + 日志落点正确 + 下游状态正确复位。

**本轮还修掉**（审计 A1）：`workspace/didChangeWorkspaceFolders` 之前只有声明、没有 handler
（dispatcher 静默丢弃，多根工作区增删文件夹后仍按旧根解析）→ 新增 handler 维护
`workspace_folders_`、切根、失效工程与报告缓存；真机探针：initialize 给两个文件夹
（CorpusAlpha + corpus_delta）→ `fpgalsp.info` 报 `demo_project` → 移除 CorpusAlpha 的文件夹 → 再 `info` 报
`corpus_delta`（PASS，`exit: 0`）。

**仍未做**（见 `doc/audit/2026-11-22/SUMMARY.md` §四）：LSP 合规项 A4–A12
（`signatureHelp`/`documentLink`/`codeLens`/`documentHighlight`/`selectionRange`/pull
diagnostics/`willSave`/`$/setTrace`/`completionItem/resolve`/`semanticTokens/range` —— 均未声明，
客户端按 capability 跳过）；`compare_reports.py` / `compare_constraints.py` 未进 CTest；插件 flow
无绝对超时；7 个 MCP 工具无人工 UI 入口；`fpgaLsp.mcp.allowFlow` 默认值是否改 `false` 待产品决定。

---

## 第三十五轮（2026-11-22，0.23.0）：LSP 可选合规 + 报表/约束门禁 + flow 超时 + 工程目录 .fpgalsp 收尾

本轮把第三十四轮审计留下的 backlog 全部做完，并单独根治了"工程目录反复长出 `.fpgalsp`"。

**1) LSP 3.17/3.18 可选合规项 A4–A12（10 项，全部真实语义）**

| 能力 | 方法 | 实现要点 |
| --- | --- | --- |
| signatureHelp | `textDocument/signatureHelp` | 例化括号内给模块端口签名 + `activeParameter`（顶层逗号；命名端口内层括号向外层升级） |
| documentLink | `textDocument/documentLink` | 仅 `.al` 的 `<File Path>`（`<Project Path>` 不链接），存在给 `file://`，缺失给 tooltip |
| codeLens | `textDocument/codeLens` | 每个 module 一条 `"N instantiations"` + `fpgalsp.searchHierarchy` |
| documentHighlight | `textDocument/documentHighlight` | 同文档 token 流匹配，注释/字符串不误命中 |
| selectionRange | `textDocument/selectionRange` | 单词 → 行 → `begin/end`/`{}` 块 → module → 整篇 |
| pull diagnostics | `textDocument/diagnostic` / `workspace/diagnostic` | 与 push 同源缓存（`latest_diagnostics_`），`previousResultId` 命中回 `unchanged`；push 行为不变 |
| willSave | `textDocument/willSave`(通知) / `willSaveWaitUntil`(请求) | 通知 no-op，请求立即 `[]` |
| `$/setTrace` | `$/setTrace` / `$/logTrace` | 记录 level；dispatcher 只对**已注册**的 `$/` 放行，未知仍静默忽略 |
| completionItem/resolve | `completionItem/resolve` | 模块项补 `detail` + markdown 文档（端口表 + 定义位置） |
| semanticTokens/range | `textDocument/semanticTokens/range` | 全量流水线 → 区间过滤 → **重新 5 元组编码** |

实现：`src/server/features_optional.{h,cpp}`（10 个纯函数 + handler 薄包装），
`lsp_server.cpp` 注册 13 个方法、能力声明补齐；`jsonrpc/dispatcher.cpp` 只放行已注册的 `$/`。
原生产物现在注册 **47 个 LSP handler**。单测 `tests/unit/features_optional_test.cpp` 10/10。

**2) 报表/约束对等门禁进 CTest**：`parity.reports`（`tests/compare_reports.py`，18 份真实报告）、
`parity.constraints`（`tests/run_constraints_parity.py` 先跑 `unit_adc/unit_sdc` 的 Parity 用例导出
JSON，再跑 `tests/compare_constraints.py`，93 个结构）。以前这两条数据通路只有"人记得跑"才跑。

**3) 插件 flow 绝对超时**：新增 `fpgaLsp.flowTimeoutMinutes`（默认 0 = syn 30 / phy 90 / all 120 分钟，
多阶段取最大；1–600 = 固定）；到点或 `flowStatus` 连续失败 12 次 → 停轮询、自动 cancel 一次、标记
失败（输出 `flow timed out after N min - cancelling` / `language server is not responding`），
不会永久 busy。新增纯函数 + 4 条单测，插件 **49/49**。

**4) `<工程>/.fpgalsp` 反复出现的根治**

* **根因**：不是"状态目录没做"，而是**一处硬编码**：`src/td/td_service.cpp` 的
  `options.work_dir = project.dir + "/.fpgalsp/run"` —— 每次 LSP 保存触发 TD 检查就重建
  `<project>/.fpgalsp/run`。真机复现：删掉 `.fpgalsp` → 一次 `didSave` → 立刻重建。
* **修复**：改为 `platform::state_dir(project.dir) + "/run"`；`state_dir()` 的 TEMP 不可用兜底也不再
  回落到工程目录（改 %LOCALAPPDATA%/%USERPROFILE%）。
* **防复发**：新增机械门禁 `docs_no_project_state`（`tools/audit/check_project_state_paths.py` 扫
  `src/` 的**代码**（剥注释）里写工程目录 `.fpgalsp` 的路径；白名单仅允许"扫描时跳过该目录/临时后缀"），
  已注册进 CTest；单测 `PathStateDir.NeverReturnsProjectLocalFpgalsp`、TD 集成测试断言 cwd 在状态目录下。
* **清理**：工程/仓库里的历史空 `.fpgalsp` 已删除；以后不会再生成。

**5) 真实 VS Code 集成测试重跑：SUITE PASS（全绿）** —— 顺手修掉 3 个长期红的真/假问题：
`doctor` 里 `${vSettings.args}` 引用了**不存在的变量**（ReferenceError 被 catch 吞掉 → 报告永远打不开）；
`completion returns project modules` 的测试位置在单词中间（服务端按前缀过滤，`top` 本就不该出现）；
`copy symbol path` 断言要的是叶子名，而命令语义是完整层次路径 `top/oc_chk_lock`。

**验证**：`ctest --preset windows-msvc-release` **69/69**（75 s）、插件 `node test/run.js` **49/49**、
真实 VS Code 集成套件 **PASS**、MCP schema 门禁 39 工具 × 2 可见性一致。
真机 CorpusAlpha：`textDocument/{signatureHelp,documentLink,codeLens,documentHighlight,selectionRange,
semanticTokens/range}`、`completionItem/resolve`、`willSaveWaitUntil`、`$/setTrace`、pull diagnostics
全部返回真实数据；`documentLink` 23 条无"假缺失"；删 `.fpgalsp` 后 `didSave` 不再重建。

**仍未做**：7 个 MCP 工具无人工 UI 入口；`fpgaLsp.mcp.allowFlow` 默认值是否改 `false` 待产品决定。

---

## 第三十六轮（2026-11-22，0.24.0）：统一写入服务（Write Service）

**用户反馈**：写缓存/日志/工程文件的代码散落在 flow / probe / analysis / server / mcp / td 各处，
以后要改"写哪、怎么写、写坏了怎么办"很容易漏改；应该统一成一个方法。
`.fpgalsp` 反复出现就是这类问题的症状（一处硬编码就绕过了所有约定）。

**后端（0.24.0）**

* 新增 `src/platform/write_service.{h,cpp}`：**唯一写入口** `writes::Write(project_dir, path, bytes,
  {.dry_run,.backup,.caller})` / `writes::AppendLine` / `writes::Classify`。
* 四类 root（按目标路径自动分类，越界带原因拒绝）：
  `kState`（`state_dir`，可建目录）、`kProject`（只允许 `*.al/*.adc/*.sdc`、`chip_debugger/*.mdn|*.cfg`；
  不替用户新建目录）、`kRun`（`<prj>_Runs/**`，可建目录）、`kTemp`（临时目录下含 fpgalsp 的 scratch）；
  任何 `.fpgalsp` 段 / `..` / NUL / 未知根一律拒绝。
* 原子写（`.tmp-<pid>-<seq>` → rename + `MoveFileEx` 兜底 + 3 次重试）、备份到 `<state>/backup/`、
  审计 `<state>/logs/writes.jsonl`（`FPGALSP_WRITE_AUDIT=0` 关）、never-throw。
* 已迁移 17 处写入：analysis 缓存、probe（mdn/config/cwc/缓存/trigger cwc）、flow_runner
  （run 文件/`.stop.f`/`flow.status`/`last_flow.json`）、flow_logic 与 ide_features 与 bridge 的
  `.al` 写、td_install 安装缓存、td_diagnostics 器件缓存、mcp_tcp 端点文件。
* 门禁 `docs_no_raw_writes`（`tools/audit/check_raw_file_writes.py`：`src/` 里的 `write_file_utf8(` 与
  `std::ofstream` 只允许出现在 write_service / encoding / log）+ 单测 `tests/unit/write_service_test.cpp`
  （8 条：`.fpgalsp`、白名单、越界、dry-run、备份、原子覆盖、AppendLine、run 目录）。

**插件（0.24.0）**

* 新增 `vscode-extension/src/writeService.js`：`writeStateFile` / `writeProjectFile`
  （白名单 `.vscode/mcp.json`、`.mcp.json`、`*.al`，可备份）/ `appendLog` / `appendText`；
  同样的 root 策略 + 原子写 + dryRun + never-throw。
* 迁移 `highlight.txt`、`.vscode/mcp.json` 安装、`launchCheck` 扩展日志；`test/run.js` 加门禁
  （裸写只允许 writeService.js），新增 9 条测试 → **58/58**。

**验证**：`ctest --preset windows-msvc-release` **71/71**（75 s）；插件 **58/58**；
真机 CorpusAlpha：单步 `read_design` 正常（3 s，下游 pending）、`didSave` 后**不再**出现 `<工程>/.fpgalsp`；
`<state>/logs/writes.jsonl` 可见 `td.install_cache`（temp）、`flow.file`（state）等真实条目。

**未做（可选项）**：Phase 4 —— 测试用内存 FS、写指标进 `fpgalsp.metrics`、MCP 写工具的 dry-run UI。

---

## 第三十七轮（2026-11-22，0.25.0）：写服务 Phase 4 + 目录/删除/改名全收口

第三十六轮的写服务只统一了"写文件"；本轮把 **目录创建 / 删除 / 改名**也收进去，并完成
Phase 4（内存 FS、写指标、MCP dry-run）。

**1) IO 全部收口**：新增 `writes::EnsureDir / Remove / Rename`（与 Write 同一套 root 策略、指标、
审计）。后端 **39 处**调用迁移：`flow_runner`（MakeDirs、步骤标记、cw、bit、user.cfg）、
`probe`（state/run 目录、capture cwd/csv）、`td_session`（scratch/work_dir）、`mcp_tcp`（端点目录/清理）、
`strategy`（复制/清理工作目录）、`analysis`（缓存目录/过期报告）、`main`（日志目录）、
`td_install`（清缓存）。策略边界：
* `kProject`：只允许在已存在的目录里写登记文件；`chip_debugger/` 允许创建、其 `*.mdn|*.cfg` 允许删除；
  用户 `.al/.adc/.sdc` 拒绝删除/改名；工程目录本身允许 EnsureDir（已存在语义）。
* TD 会话日志归位（`<al>/td_*.log → <al>/log/`）是平台维护动作，3 处保留裸调用并用
  `// fpgalsp-write-exempt: <理由>` 行内豁免；门禁识别该标记（必须带理由）。

**2) 门禁扩展**：`docs_no_raw_writes` 现在同时禁止 `write_file_utf8` / `std::ofstream` /
`create_directories` / `remove` / `remove_all` / `rename` / `platform::ensure_directory`；
白名单仅平台实现（write_service / encoding / log）+ 行内豁免。`test/run.js` 的插件门禁不变。

**3) Phase 4**：
* **内存 FS 后端**：`writes::MakeMemoryBackend()` + `SetBackendForTest/ResetBackendForTest`，单测
  `MemoryBackendKeepsDiskUntouched` 证明写走内存、磁盘无文件；
* **写指标进 `fpgalsp.metrics`**：`writes{ok,failed,denied,bytes,dirsCreated,removals,renames,backups,
  byRoot{state,project,run,temp}}`（真机 LSP 实测字段存在）；
* **MCP 写工具 dry-run**：`fpga_project_add_source` / `fpga_project_rename` / `fpga_project_update` /
  `fpga_probe_config` 的 `dry_run:true`（schema + description 已更新）。真机 CorpusAlpha 实测：
  四个调用都返回预览，`.al` 的 mtime 不变。

**验证**：`ctest --preset windows-msvc-release` **71/71**（75 s）、插件 **58/58**；
真机 CorpusAlpha 单步 `read_design` 正常（3 s、下游 pending）、`didSave` 后工程目录仍无 `.fpgalsp`、
`fpgalsp.metrics.writes` 可用；`unit_write_service` 13 条全绿（新增目录/删除/改名/指标/内存后端）。

---

## 第三十八轮（2026-11-22，0.26.0）：语法高亮错位根因 + copy_file 收口

**用户反馈**：右键格式化后语法高亮颜色错乱；编辑（保存与否都一样）也会偶发；切到别的文件再切回恢复。
用户判断"插件没及时感知未保存的修改"——方向对，但根因在**服务端**。

**根因（实测复现）**：`DocumentStore` 用 **uri 字符串**作为打开文档的键，而客户端发来的 uri
是 `file:///y%3A/CorpusAlpha/src/top.v`（小写盘符 + 百分号编码冒号），服务端内部
`platform::path_to_uri()` 产出 `file:///%WORKSPACE%/CorpusAlpha/src/top.v`。两者字符串不相等 →
`LspServer::LoadText` 查表永远 miss → 回落到**磁盘**。于是：
* 未保存的缓冲区对语义高亮（以及格式化基线）不可见；
* 格式化应用后 buffer 变了、磁盘没变 → token 仍按旧文本算 → 颜色错位；
* 切走再切回触发重新 didOpen/全量请求（此时缓冲区文本被重新发送）才恢复。
复现脚本 `build/_audit/rt/semantic_probe.py`（客户端风格 uri）：修复前 2094 个 token 有
**1000 个落在空白/行尾**（错位）；修复后 **0**。

**修法**：新增 `DocumentStore::GetTextByPath()`（按归一化路径查找：大小写/分隔符/URI 编码无关），
`LspServer::LoadText` 改用它；未打开时仍走磁盘缓存。单测
`DocumentStore.GetTextByPathFindsBufferForClientStyleUri`。

**顺带把 copy_file 也收进写服务**（0.25.0 唯一已知缺口）：新增 `writes::Copy`
（源任意 root；目标只允许 state/run/temp 或工程内登记目录 `chip_debugger/`、`log/`），
迁移 8 处（probe 4 处位流复制 + 1 处 user.cfg 备份、flow_runner user.cfg 备份、
strategy 工程副本、td_session 跨卷日志复制）；门禁新增 `copy_file(`；指标新增 `writes.copies`。

**验证**：`ctest --preset windows-msvc-release` **71/71**（75 s）、插件 **58/58**、真实 VS Code
集成套件 PASS；`unit_write_service` 14 条（+Copy）；真机 CorpusAlpha：客户端风格 uri 下 token 0 错位、
`.fpgalsp` 仍不出现。

---

## 第三十九轮（2026-11-22，0.27.0）：审计发现落地（安全 / 性能 / 现代化 / 引用）

按用户指定，修复 2026-11-22 静态审计中发现的问题，并补根 `.clang-tidy` 门禁。

**安全**：S-01 探针信号名白名单（`NormaliseSignals`/`RenderMdn`/`WriteMdn`/`ReadMdn`/`Configure`
全员校验，非法不落盘、不建目录）；S-02 4 处路径改 `TclQuoteGrouped`（正常路径命令逐字节不变）；
`check_tcl_invariant.py` 新增 R1/R2 + `--self-test`（9/9）。真机 MCP 注入信号 `a}; exec calc; #`
不写 user.mdn。S-03（loopback token 默认强制）与 T 类理论项未修，见 `static/FIXES-0.27.0.md`。

**性能**（全部确认项）：semanticTokens cache 改 `shared_ptr`（零深拷贝、resultId 直接 FNV、
未变时 delta 不 tokenize）；DocumentStore 文本 `shared_ptr` + `path_index_`（O(1) 查找）+
`LoadTextShared`；MCP `ResolveAlByNameCached`（5s TTL）；write_service `state_dir` memoize +
长驻审计 ofstream；flowStatus 复用 1s 缓存；tools/list 只构建一次；§2.7 小项。
**未做**：Classify 的 canonical root 缓存（安全边界，拒绝不安全缓存）。

**现代化**（A 类）：`atoi` 37 处清零（`platform/number.h`：`ParseDecimal`/`DigitsToIntSaturating`；
CLI 端口非法→退出）；`JoinOnExit`→`std::jthread`；`Result` 加 `[[nodiscard]]` + 36 处
`IgnoreResult`；2 处空 catch→`NeverThrow`；7 张静态 set→`constexpr array`；DocumentStore 透明
hash；`DiagnosticsOptions::text` 改 `std::string`。**A-7 巨型函数重构未做**（结构重构、JSON
契约敏感，单独立项）。

**wire 引用误报**：`HdlIndex::References` 对 signal 改 scope-aware（本文件 + module 范围；过滤
命名端口标签/端口/参数/实例名/模块名引用；保留 `.port(net)` 实参）。真机 CorpusAlpha `wire clk`
（多处 `.clk(clk)`）：引用 21 条、`.clk` 标签误报 **0**。

**新门禁**：根 `.clang-tidy`（缺陷优先 check 集）+ `tools/audit/clang_tidy_gate.py` 棘轮
（`ctest -L tidy`，`docs_clang_tidy`，~135 s）；基线 **281 条 / 67 TU**（修复前 320）。

**验证**：`ctest --preset windows-msvc-release` **72/72**；插件 **58/58**；真实 VS Code 套件
**PASS**；真机 CorpusAlpha 单步 flow 正常、`.fpgalsp` 不出现、wire 引用 0 误报；`check_tcl_invariant.py`
PASS（132 文件/45 调用点）。

---

## 第四十轮（2026-11-22，0.28.0）：现代化 B/C 批量 + 插件/日志英文化 + Update Project

**现代化 B/C（MODERNIZATION.md 19 类）**：
- 批次 1 机械 `--fix` **719 处**（scoped-lock/contains/trailing-comma 537、return-braced/raw-string/
  emplace/namespace 105、12 个零散 check 77）；fix 工具缺陷的人工修正：trailing-comma 误删实参逗号 2 处、
  `Json` braced-init 语义 28 处改回显式构造、use-string-view 破坏三元 1 处。
- 批次 2 **504 处**：designated 208（残留 0）、nodiscard 96（虚函数/`.cpp` 24 处按纪律跳过）、
  starts-ends 82（先补长度保护）、ranges 75、auto 20、std-print 5（仅 stderr，输出逐字不变）。
- 批次 3 人工项：43 个文本工具定义点 → `src/platform/text.h` 唯一实现；审计行 `std::format`；
  printf 17→0、拼接 23→0；sv 往返 9→0；`(const char*,size_t)` 2→span；u8path 11→0；
  anyofallof 27→0；C 数组 64→22（安全子集）。
- **未做**：A-7 巨型函数结构重构（JSON 契约敏感，单独立项）。
- 根因门禁：根 `.clang-tidy` 的 `Checks:` 会与命令行 `--checks=` 合并，批量修必须写 `-*,<check>`
  （过程记录见 `static/FIXES-0.28.0.md`）。

**插件界面/日志英文化**：插件代码本身自 `e57a273` 起已是英文（中文仅在注释）；
真正来源是后端 `Emit("log",…)`/`LogMessage`/logger/`--help`/CLI 错误——已翻译 **51 个字面量
（40 条消息）**、7 个源文件，结构字段零改动；注释/文档保留中文。扫描器结果 0（改前 51）。

**Update Project 修复**：根因 `ok = !HasError(output)` 把 `<CODE> CRITICAL-WARNING` 判失败，
而 TD 实际 `PRJ-1401 : Successfully analyzed…` → 改 `!HasFatal(output)`（只有 `<CODE> ERROR` 失败），
失败补 `error`；插件新增 `src/updateProject.js`：成功/失败写 "FPGA LSP" 输出通道 + extension.log，
失败弹 `Update Project failed: <reason> (see FPGA LSP output for details)`。真机 MCP
`fpga_project_update` 修复前 ok:false → 修复后 **ok:true**；插件单测 58→**63**。

**验证**：`ctest` release **72/72**（含 `docs_clang_tidy`，基线 281→**142**）；插件 **63/63**；
真实 VS Code 套件 **PASS**；e2e LSP 冒烟 **PASS**；真机 `didSave` 后 `.fpgalsp` 不出现；
`check_tcl_invariant`/`check_raw_file_writes`/`check_project_state_paths` 全 PASS。

---

## 第四十一轮（2026-11-22，0.28.1）：Update Project 在"移动过的工程"上必失败（真机 %WORKSPACE%\CorpusBeta）

**症状**：`FPGA: Update Project` 在 `%WORKSPACE%\CorpusBeta` 上必失败，`output` 里是一串
`HDL-8007 ERROR: cannot open Verilog file '/src/ADC.v'`，末尾却是
`PRJ-1401 : Successfully analyzed 52 source files.`。

**根因**：TD 解析 `.al` 里的相对源文件路径（`<File Path="src/ADC.v">`）时用的"工程根"是
`.al` 根节点的 `<Project Path="...">` 属性；该属性失效（工程被移动/拷贝过）时**退化成进程 cwd**。
我们的 TD 会话 cwd 是 `%TEMP%/fpgalsp/<prj>-<hash>/run`，于是所有相对路径都落到临时目录 →
`'/src/ADC.v'` 找不到。实测矩阵（`build/_audit/rt/mb210_update_probe.py`）：
cwd=%TEMP% 10 条 HDL-8007；cwd=工程目录 **0 条**；cwd=%TEMP% + 改写 Path 属性 **0 条**。

**修法**（3 处 TD 会话 cwd 统一成 `.al` 所在目录，与 TD GUI 一致）：
- `analysis::UpdateProject`（用户报的命令）：cwd = `project.dir`，`log_dir` = `<al>/log`；
- `td_service::TdSessionRunner`（LSP 保存触发的 TD 检查）：同上；
- `flow_runner` 的 TD 会话：cwd = `project.dir`（run 日志目录不变，仍在状态目录）。
  这同时修掉"移动过的工程跑 flow 会整片 HDL-8007"的同类问题（实测 CorpusBeta 副本单步
  `read_design`：run.log 0 条 HDL-8007、36 个源文件、marker 3 s 内出现）。
TD 自己的 `td_*.log/.logw`、`minidump-*.dmp` 仍由 `TdSession` 归位到 `<al>/log/`，工程根目录不留痕。

**新增可操作 warning**：`.al` 记录的 `Path` 与实际目录不一致时，`updateProject` 在 `ok:true` 的
同时返回 `warning`（插件弹 "Update Project completed with a warning: …" 并提供 Show Logs），
提示"在 TD GUI 里打开并保存一次即可重新记录该路径"。

**顺带修**：clang-tidy 棘轮门禁的"行号漂移"匹配现在会归一化**消息里内嵌的** `:line:col`
（`bugprone-exception-escape` 把 lambda 位置写进消息，纯漂移会一直被当成新问题）。

**验证**：真机 `%WORKSPACE%\CorpusBeta`：`fpga_project_update` **ok:true**、HDL-8007 **0**、
收到 warning；`ctest` **71/71**（**clang-tidy 门禁已改为默认不注册**；打开 `-DFPGA_LSP_ENABLE_TIDY_GATE=ON` 时 72/72）；插件 **64/64**（新增 warning 用例）；
`td.diagnostics` 集成用例改为断言"cwd = 工程目录 + 工程根无 TD 日志"；`%WORKSPACE%\CorpusBeta` 根目录无
残留（`log/` 收 TD 日志、无 `.fpgalsp`）。

> 2026-11-22 调整：clang-tidy 棘轮门禁（`docs_clang_tidy`，约 +145 s）**默认不再进 CTest**，
> 由 `-DFPGA_LSP_ENABLE_TIDY_GATE=ON` 显式注册（或直接跑 `tools/audit/clang_tidy_gate.py`）。
> 因此默认 `ctest` 回到 **71/71**；需要静态分析回归时按 `doc/quality.md` 的命令打开。

---

## 第四十二轮（2026-11-22/23，0.29.0）：端到端性能优化（团队模式，实测驱动）

三个子 agent 独立审计（`doc/audit/2026-11-22/perf2/`：LSP-HOTPATH / MCP-TD / PLUGIN-E2E，
全部实测、同机同构建），随后直接落地 10 项优化。汇总与改前/改后数字见
[`perf2/README.md`](audit/2026-11-22/perf2/README.md)。

**基线实测**：一次真实按键后端 **9.6 ms**（CorpusAlpha）/ **10.7 ms**（CorpusBeta，52 文件）；
MCP 传输层只有 0.075 ms；插件一次按键只发 1 条 didChange（239 B），但后端回 31.8–63.6 KB 相同诊断；
一次保存 24 条 / 78 KB；`fpga_flow_status` 20.91 ms；reports 全量 1.10 MB。

**落地项（改前 → 改后，实测）**
- 诊断"内容未变即不发"（`PublishDiagnostics` 在 sanitize 前用 `SameDiagnosticList` 短路）：
  didChange 1.73 ms/31.7 KB → **0.56 ms/0 B**；CorpusBeta 3.61 ms/62.5 KB 同类下降；保存不再整工程重播
- `fpga_flow_status` 改走 ReportStore（`kArtifacts` 掩码，输出逐字段不变）：**20.91 → 0.48 ms**
- `fpga_references` 32 项 generation-keyed LRU：**9.80 → 0.54 ms**
- `workspace_symbols` limit 下推：**5.86 → 4.16 ms**（剩余为索引 O(N) 扫描，记入待办）
- `WrapToolResult` 默认档去掉整树深拷贝
- documentSymbol 结果缓存（索引快照身份 + 文档版本）：同版本重复 **1.13 → 0.84 ms**
- 插件：删除 `debugHighlight` 调试遗留（每文件 1 RPC + 1 次写盘）；reports 走 `summary` 分节
  （**1.10 MB → 1.7 KB**）；reportsChanged 合并去抖 + 内容摘要短路（6 事件 → **1 请求/0 重绘**）；
  moduleTree 摘要短路（保留展开状态）

**未做（下一轮）**：semanticTokens 每键全量 tokenize（5.4 ms，占按键 51%，需行级增量）、
completion 候选缓存、TD 设备表落盘（冷调用 574 → ~170 ms）、保存后 stamp 校验、worker 池 2→4。

**验证**：dev/release `ctest` **71/71**；插件 `node test/run.js` **64/64**；
新测试：`BridgeTest.FlowStatusArtifactsSurviveTheReportStorePath`、
`WorkspaceSymbolsWindowMatchesFullList`、`ReferencesCacheIsGenerationKeyed`、
`McpResultRepresentationTest.DefaultTierTruncatesLargeArrayPayload / StructuredTierKeepsFullPayloadUnchanged`。

---

## 第四十三轮（2026-11-23，0.30.0）：性能优化第二轮 + clang-tidy 门禁改回默认开启

**性能（团队模式，子 agent 实现 + lead 实测验收；详见 [perf2/README.md](audit/2026-11-22/perf2/README.md)）**

| 项 | 改前 | 改后 |
| --- | --- | --- |
| semanticTokens 行级增量（缓存键 = path + **索引内容指纹**） | per-key full 5.51 ms / delta 5.43 ms | **1.95 / 1.88 ms**（-65%） |
| TD 设备摘要落盘缓存（`<state>/cache/td_device_summary.json`） | `fpga_constraint_audit` cold **558 ms** | **14.1 ms**（-97.5%，命中不建 TD 会话） |
| completion 常量候选预建 + reserve | 2.04 ms/键 | **1.81 ms** |
| LSP worker 池 2 → 4 | 多文档并发最坏 11.8 ms | 预期 ≈6.6 ms |

语义增量的正确性：2 个文件 × 200 次随机编辑逐 token 等价（含块注释/字符串/跨行删除），外加
"generation 抖动但索引内容不变必须继续命中""索引内容真变必须回退全量"两个专项用例（20/20 绿）。
第一版实现因为把 `generation` 放进缓存键而**一次都没命中**（每键都变），实测发现后改为索引内容指纹。

**顺带修掉 3 条静态分析真问题**：`old_text.substr()` 临时 `std::string` 绑到 `string_view`（悬垂）、
语义缓存全局 `std::map` 静态初始化期可抛（改函数内 static）、一处 `ReportsSnapshot` 命名空间写错。

**clang-tidy 门禁改回默认开启**：`FPGA_LSP_ENABLE_TIDY_GATE` 默认 `ON`，`ctest` 回到 **72 项**
（含 `docs_clang_tidy`，约 +145 s；想跑快的用 `ctest -E docs_clang_tidy`，约 90 s）。
基线重新生成：**142 条 / 67 TU**（与 0.28.0 持平 —— 本轮新增代码净新增 0 条，且修掉 3 条）。

**验证**：dev/release `ctest` **72/72**；插件 `node test/run.js` **64/64**；
`check_versions` / `check_raw_file_writes` / `check_project_state_paths` / `check_tcl_invariant` 全 PASS。

---

## 第四十四轮（2026-11-23，0.31.0）：性能第三轮（符号分页 / 单文件校验 / 语义健壮性）

| 项 | 改前（实测） | 改后（实测） |
| --- | --- | --- |
| workspace_symbols 真分页（`SearchSymbols(query,offset,limit,&total)` + MCP 接线） | limit=50 **4.16 ms** | **3.57 ms** |
| didClose / 无文本 didSave 只校验单文件（`ValidateOneLocked`） | 关闭后首个请求 +1~3 ms（最坏 22 ms） | 只重读该文件；watcher/InvalidateAll 仍全量 |
| 语义缓存健壮性：索引签名改行号感知记录表 + 修 2 个真 bug | 潜在错位/越界 | 已修；同键 1.95 → 2.07 ms |

**口径更正**：`HandleDidSave` 打开文档时总传缓冲区文本，`OnFileChanged(path,nullopt)` 实际几乎只由
**didClose** 触发 —— 前两轮"didSave 后 +1~3 ms"应理解为"关闭/无文本保存后"。

**未达成（诚实记录）**：回车/粘贴（行数变化）的 semantic 仍是全量 **5.58 ms**（目标 ≤2.5）。
第一版"行号整体平移"在等价性测试里撞到两个真问题：① `SymbolAt` 的"模块内同名回退"会枚举到平移后
错位的声明（kind 从 variable 变 property）；② 普通行插到 `define ... \` 之后会变成宏体，区域词法状态
必须跟着平移。结论：先建"索引与文本同源"契约（文本版本/哈希），再补宏体边界门禁，两步都做完才敢启用；
本轮按正确性优先**撤回启用**，只保留签名重构与两个 bug 修复。

**验证**：dev/release `ctest` **72/72**；插件 **64/64**；四个 python 门禁 PASS。
