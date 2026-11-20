# 交付状态（当前：2026-11-19）

## 构建与测试

| 项目 | 数据 |
| --- | --- |
| **全新目录干净构建** | configure 29 s → build 3.3 min → **844 个目标全部成功**（Release + 全量 LTO） |
| **`ctest` 全量** | **58/58 通过**（`unit` 40 · `e2e` 3 · `formatgate` 4 · `lintgate` 3 · `gate` 2 · `generator` 1 · `mcp` 1 · `td` 3 · `bench` 1） |
| **原生产物** | `build/bin/fpgalsp-native.exe`（**6,132,736 B**，全量 LTCG；SHA256 `1E5B207B…B1595A`） |
| **单元测试规模** | 40 个 gtest target |

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
> 工程：`%WORKSPACE%\CorpusDelta\corpus_delta`（PH1A100SFG676，TD 2026.2.210850），真机在环。

| 项 | 数据 |
| --- | --- |
| 新增后端模块 | `src/flow/probe.{h,cpp}`（mdn / compiled.cwc / 采集 CSV 三套纯解析 + TD 会话编排）、`src/flow/hardware_lease.h`（进程级 JTAG 租约） |
| 新增 LSP 命令 | `fpgalsp.probeStatus` / `fpgalsp.probeConfig` / `fpgalsp.probeCapture`（广播进 `executeCommandProvider`） |
| 新增 MCP 工具 | `fpga_probe_status`（只读）· `fpga_probe_config`（`write` 门控）· `fpga_probe_capture`（`flow` 门控）→ 工具总数 **43 → 46** |
| 新增插件命令 | `fpgaLsp.probeStatus` / `fpgaLsp.probeCapture` / `fpgaLsp.probeConfig`（采集前有模态确认） |
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

**未完成的实验（下期）**：为拿到"非 0 且随时间变化"的读数，把
`WHG/ANPC_init0/ANPC_SVPWM_init/cnt[15:0]`（16 位三角载波，`synthesis keep="1"`，
与 `reg_init` 同 `clk` 域）加进 `user.mdn` 后**必须跑完整 flow**——实测
`stages:["route"]` 会走增量（`import_db place.db`，只重跑 route），
`insert_debugger` 在 `opt_place` 阶段不会重新执行，`compiled.cwc` 时间戳不变、探针不会更新。
本次已验证到这一层并已把 `user.mdn` 还原成原来那 64 个信号（1192 B，与原始文件同尺寸）。
