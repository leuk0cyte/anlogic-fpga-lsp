# 无上下文 agent 怎么用这套 MCP

> 面向"第一次接触本仓库、手里没有任何背景资料"的 agent（以及替 agent 设计提示词的人）。
> 目标是：**只看 MCP 本身**就能知道 50 个工具怎么串、哪些坑必踩。
> 可见性：`--allow-flow` 时 46 个，只读模式 38 个，`--allow-write` 再加 3 个（工程改名/加源文件/改工程）。

## 一、三条发现路径（按客户端能力从强到弱）

| 层 | 载体 | 谁会用 | 内容 |
| --- | --- | --- | --- |
| L1 | `initialize.instructions` | 所有 MCP 客户端（Claude Desktop / Copilot / Cline 都会带给模型） | 约 2200 字符的"作战手册"：三套流程、硬规则、成本与权限 |
| L2 | **`fpga_guide` 工具**（工具表第一位） | 只实现 `tools/list` 的客户端 | 7 个 topic 的按需文档，见下表 |
| L3 | `prompts/list` + `prompts/get` | 支持 prompts 的客户端（可一键把流程喂进来） | 4 个工作流模板 |
| L4 | 工具 `description` 里的 `Preconditions / Cost / Failure modes / Example` | 所有客户端 | `fpga_probe_config`、`fpga_probe_capture`、`fpga_flow_run` |

> 探针相关三件套：`fpga_probe_status`（现状 + `sampling{clock,depth,editable:false,reason}`）、
> `fpga_probe_verify`（逐位回答"哪几根被丢了、为什么"，`buses[].status/reason` + `hints`）、
> `fpga_probe_capture`（`buses:[{name,op,value}]` 表达"计数器回绕到 0 时触发"这类条件）。

**`fpga_guide` 的 7 个 topic**（每个 ≤ 2.4 KB，按需取，不占常驻上下文）：

| topic | 回答什么问题 |
| --- | --- |
| `overview` | 这套工具能干什么、先调什么、通用规则 |
| `project` | `.al` 工程结构、哪些文件真的参与编译 |
| `index` | 符号树、跨模块跳转、诊断、作用域 |
| `flow` | 综合/布局布线/位流：异步语义、stages、产物与报告 |
| `probe` | ChipWatcher：探针清单、重编 bit、抓取、触发（含 per-bus 值）、`fpga_probe_verify` 逐位核对、常见静默失败 |
| `program` | 烧录：MCP 能做什么、哪些必须走 LSP 命令 |
| `troubleshoot` | `KIT-*`／`PRJ-*`／`HDL-*` 的判读、假成功形态 |

调用方式：

```jsonc
// tools/call
{"name": "fpga_guide", "arguments": {"topic": "probe"}}
// → {"ok": true, "topic": "probe", "text": "# probe\n..."}
```

未知或省略 `topic` 一律回落 `overview`（不会报错，agent 不会因此卡住）。

## 二、四个工作流模板（`prompts/get`）

| name | 参数 | 用途 |
| --- | --- | --- |
| `onboard-project` | `project`（可选） | 第一次接触一个工程：设备/顶层/构建文件集/诊断 |
| `run-flow-to-bit` | `stages`（可选） | 从 RTL 到 bit：跑 flow、读报告、判成败 |
| `probe-on-board` | `signals`（必填）、`trigger`（可选） | 加探针 → 重编 → 上板抓取 |
| `debug-failed-flow` | `run`（可选） | flow 失败后的排查顺序 |

## 三、单步 flow（`fpga_flow_run_step`）与 `jobs` 语义

GUI 的 Flow 视图是**恰好 6 步**：`read_design` / `opt_rtl` / `opt_gate`（syn_1）与
`opt_place` / `opt_route` / `bitgen`（phy_1）。`fpga_flow_run_step` 就是右键那两个动作的 MCP 版：

| 调用 | 语义 |
| --- | --- |
| `{step:"read_design"}`（`force:false`，默认） | **Run**：这一步已有 `.end.f` 标记就直接返回 `{ok:true, skipped:true, reason}`，不重跑 |
| `{step:"read_design", force:true}` | **Reset and Run**：先清掉**该步及下游**的 `.begin/.end/.error.f` 标记与 `flow.status` 统计行，再 `launch_runs <run> -step <step>`，**只跑这一步**；上游产物保留 |
| 未知步骤 | `{ok:false, error:"unknown step; use read_design / opt_rtl / …"}`（不会静默跑全流程） |

副作用规则（与 GUI 一致，别惊讶）：

* 任何 **syn 步骤**重跑 → 整个 phy run 作废（phy 标记 + `phy_1/cw` + 旧 `.bit` 一起清）；
* 任何 **phy 步骤**重跑 → 删除 `phy_1/cw` 与旧 bit（bitgen 会重新生成）；
* 单步 phy **不做** `save_best_bits`，也不写 ChipWatcher 旁路（位流还不完整）。

`jobs` 语义（`fpga_flow_run` 与 `fpga_flow_run_step` 同一套）：

| 事实 | 值 | 依据 |
| --- | --- | --- |
| `-jobs N` 是什么 | **multirun 的并发 run 数**（同时并行跑几条 run），**不是**单 run 内的线程数 | TD 手册 §9.3.14 / PRJ-8802 |
| 合法范围 | **1~6**（超界被钳到 6；0 或负数按默认 6） | PRJ-8802；`flow_runner.cpp` / `bridge.cpp` |
| 单步 | **强制 1**（`launch_runs … -step … -jobs 1`） | PRJ-8803「单步执行时最大并发数只能为 1」 |
| 默认 | 6（= TD GUI 交互式跑整条的默认值 `launch_runs syn_1 phy_1 -jobs 6`） | GUI 会话日志 PRJ-1409 |
| 单 run 内线程数 | TD 自己按 CPU 核数开（run.log 里 `TMR-1041 … 32 threads`），与 `-jobs` 无关 | 实测 |

> 我们一次只 launch 一个 run（先 syn_1、再 phy_1），所以 `-jobs` 除了"合法范围"之外并不改变调度；
> 真正有意义的是单步必须传 1，否则 TD 直接报错。

### 日志在哪、`fpga_flow_log` 读哪里

| 位置 | 内容 |
| --- | --- |
| `<工程根>/<prj>_Runs/<run>/run.log` | TD 的 run 日志（每步耗时、`*-ERROR/WARNING`） |
| `<.al 目录>/log/td_*.log(.logw)` | TD **会话**日志（GUI 自己也放这里）；短会话不再落工程根 |
| `<状态目录>/flow/<runId>/`（+ `log/`） | 本次 flow 的运行目录；状态目录 = `%TEMP%\fpgalsp\<工程名>-<hash8>`（可用 `FPGALSP_STATE_DIR` 覆盖） |

`fpga_flow_log` 会同时扫 run 目录与 `<.al>/log`（按 mtime 取最近 12 份，带 `source` 标记），
所以"flow 失败的 TD 原文"通常都能拿到；`fpga_flow_issues` 给的是结构化判读（markers/logs/counts）。

## 四、agent 最常踩的 7 个坑（都写进了 L1/L2，这里给维护者留档）

1. **磁盘上的 `*.v` ≠ 参与编译的文件**：先看 `fpga_project_files`（`include_excluded:true` 看被排除的），
   别假设 `src/` 就是构建树。
2. **flow 是异步的**：`flow_run` 返回 `runId`，必须在**同一个 MCP 会话**里 `flow_wait`；
   进程退出会带走 TD 子进程（换个进程再查会看到"成功但没有 flow"）。
3. **改 RTL 必须重跑 `syn`**：`stages:["phy"]` 会用旧网表。最稳是 `["all"]`。
4. **写 `chip_debugger/*.mdn` 不会改板上任何东西**：要重跑 phy 才编进 bit；
   而且 **flow 成功 ≠ 探针进 bit**——用 `fpga_probe_status` 的
   `present / net_count / missing_in_bit` 确认（后端现在会在"配了探针但没产出 watcher"时直接失败并带出 TD 的 `KIT-*` 原文）。
5. **采样时钟与采样深度是编译期量**，MCP 改不了（TD 限制）；运行期只有
   `trigger_position / window_num / window_depth / trigger_*` 可改。
6. **MCP 没有独立烧录工具**：SRAM/Flash 用 LSP 命令 `fpgalsp.program`；
   MCP 里只有 `fpga_probe_capture(download:true)` 会重下载 SRAM。
7. **进程启动很快（~0.1 s）**：TD 发现是一次 40 ms 的目录扫描，结果缓存在
   `%TEMP%/fpgalsp/td-install-cache.txt`。之前记的"每次 20 s"是**客户端脚本的错**：
   发完 `exit` 通知没关 stdin，服务端按规范等 EOF，客户端于是耗到自己的超时。
   ⚠ 客户端务必在 `exit` 之后**关闭 stdin**（或直接 kill）。phy 仍要 5–15 分钟，
   长跑任务必须留在同一个会话里。

## 五、写工具的 dry-run（预览）

四个会**改工程**的工具有 `dry_run:true`：`fpga_project_add_source`、`fpga_project_rename`、
`fpga_project_update`、`fpga_probe_config`。dry_run 时只读、返回将要发生的变化
（`insertAtLine` / `from`,`to` / `note` / `before,after,added,removed`），**不写任何文件、不启动 TD**；
`.al` 的 mtime 不变可用来验证。需要 `--allow-write`（工具可见性与真实写一致）。

## 六、和后端实现的关系

| 帮助内容 | 对应实现 |
| --- | --- |
| L1 手册 | `src/mcp/mcp_server.cpp`（`kInstructions`） |
| L2 指南 | `src/mcp/guide.cpp`（正文）+ `src/mcp/tools.cpp`（工具表头）+ `src/mcp/bridge.cpp`（handler） |
| L3 模板 | `src/mcp/mcp_server.cpp`（`prompts/list`、`prompts/get`、`capabilities.prompts`） |
| L4 描述 | `src/mcp/tools.cpp`（三个易错工具的 description 追加段） |

门禁：`ctest -L mcp`（`mcp_schema_consistency` 会逐字段比对 Python 参考实现；
description **允许在 Python 原文之后追加**说明，反向缺失仍算漂移）。