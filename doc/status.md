# 交付状态（当前：2026-11-20）

## 构建与测试

| 项目 | 数据 |
| --- | --- |
| **全新目录干净构建** | configure 29 s → build 3.3 min → **844 个目标全部成功**（Release + 全量 LTO）；无 LTO 的 `windows-msvc-dev` 全新目录 **56 s** |
| **`ctest` 全量** | **63/63 通过**（`unit` 43 · `td` 4 · `e2e` 5 · `formatgate` 4 · `lintgate` 3 · `gate` 2 · `generator` 1 · `mcp` 1）；preset 默认 **8 路并行**，实测 **62 s**（串行 92–104 s） |
| **原生产物** | `build/bin/fpgalsp-native.exe`（**6,486,016 B**，全量 LTCG；SHA256 `78744ADE…88E25F`） |
| **单元测试规模** | 43 个 gtest target |

> 构建/测试的并行度分析（为什么 192 线程帮不上忙、时间精确花在哪、日常该用哪个 preset）见
> [doc/build.md](build.md) 的「并行度：192 线程用在了哪里」。

## 功能面

| 项目 | 数据 |
| --- | --- |
| LSP | 25 个方法（补全 / 悬停 / 定义 / 引用 / 文档与工作区符号 / 代码动作 / 格式化 ×2 / 重命名 / 调用层次 / 语义高亮 / 折叠 / inlayHint / reports …） |
| MCP | **43 个工具全部真实实现（0 占位）**（Python 对齐 39 + `job_status`/`job_cancel`/`run_diff`/`testbench`）；默认只读，flow / 写操作需显式开启；4 个只读 resources |
| TD 集成 | 会话与诊断、Flow + BitWizard、报告解析、ADC + SDC 符号、HDL 索引 |

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
