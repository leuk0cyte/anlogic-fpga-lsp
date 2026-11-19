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
| MCP | **39 个工具全部真实实现（0 占位）**；默认只读，flow / 写操作需显式开启 |
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

## VS Code 扩展（0.14.0）

```
vscode-extension/fpga-lsp-anlogic-0.14.0.vsix   224 files / 2.86 MB
  └─ extension/server/bin/fpgalsp-native.exe    6,132,736 B（全量 LTCG）
```

打包：`npm run package`（= `node scripts/bundle-server.js && vsce package …`）。
已核对包内 exe 与 `build/bin` 哈希一致。

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