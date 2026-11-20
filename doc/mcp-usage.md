# 无上下文 agent 怎么用这套 MCP

> 面向"第一次接触本仓库、手里没有任何背景资料"的 agent（以及替 agent 设计提示词的人）。
> 目标是：**只看 MCP 本身**就能知道 47 个工具怎么串、哪些坑必踩。

## 一、三条发现路径（按客户端能力从强到弱）

| 层 | 载体 | 谁会用 | 内容 |
| --- | --- | --- | --- |
| L1 | `initialize.instructions` | 所有 MCP 客户端（Claude Desktop / Copilot / Cline 都会带给模型） | 1613 字符的"作战手册"：三套流程、硬规则、成本与权限 |
| L2 | **`fpga_guide` 工具**（工具表第一位） | 只实现 `tools/list` 的客户端 | 7 个 topic 的按需文档，见下表 |
| L3 | `prompts/list` + `prompts/get` | 支持 prompts 的客户端（可一键把流程喂进来） | 4 个工作流模板 |
| L4 | 工具 `description` 里的 `Preconditions / Cost / Failure modes / Example` | 所有客户端 | `fpga_probe_config`、`fpga_probe_capture`、`fpga_flow_run` |

**`fpga_guide` 的 7 个 topic**（每个 ≤ 2.4 KB，按需取，不占常驻上下文）：

| topic | 回答什么问题 |
| --- | --- |
| `overview` | 这套工具能干什么、先调什么、通用规则 |
| `project` | `.al` 工程结构、哪些文件真的参与编译 |
| `index` | 符号树、跨模块跳转、诊断、作用域 |
| `flow` | 综合/布局布线/位流：异步语义、stages、产物与报告 |
| `probe` | ChipWatcher：探针清单、重编 bit、抓取、触发、常见静默失败 |
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

## 三、agent 最常踩的 7 个坑（都写进了 L1/L2，这里给维护者留档）

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
7. **每次新建 MCP 进程约 20 s**（启动时扫盘找 TD 安装），单次工具调用本身通常 < 1 s；
   phy 5–15 分钟。长跑任务必须留在同一个会话里。

## 四、和后端实现的关系

| 帮助内容 | 对应实现 |
| --- | --- |
| L1 手册 | `src/mcp/mcp_server.cpp`（`kInstructions`） |
| L2 指南 | `src/mcp/guide.cpp`（正文）+ `src/mcp/tools.cpp`（工具表头）+ `src/mcp/bridge.cpp`（handler） |
| L3 模板 | `src/mcp/mcp_server.cpp`（`prompts/list`、`prompts/get`、`capabilities.prompts`） |
| L4 描述 | `src/mcp/tools.cpp`（三个易错工具的 description 追加段） |

门禁：`ctest -L mcp`（`mcp_schema_consistency` 会逐字段比对 Python 参考实现；
description **允许在 Python 原文之后追加**说明，反向缺失仍算漂移）。