# 文档索引

仓库的文档分三类：**本目录（活文档）**、**审计证据归档**、**代码旁的局部 README**。

## 活文档（会随实现更新）

| 文档 | 内容 |
| --- | --- |
| [build.md](build.md) | 环境要求、构建与 preset、Release 全量 LTO、构建目录纪律、恢复被污染的目录 |
| [conventions.md](conventions.md) | 编码约定、「发现 Python 缺陷忠实复现 + 登记」的移植铁律、有意保留的差异策略 |
| [verible-cmake.md](verible-cmake.md) | Verible CMake 化的风险画像：哪些缺陷是静默的、为什么、如何自检 |
| [verible-generator-upgrade.md](verible-generator-upgrade.md) | 升级 bison / flex 后的正确性验证流程（锚点核对、补丁生效、官方错误消息对拍） |
| [protocol.md](protocol.md) | 可执行入口、LSP/MCP 两种传输模式、framing 与单条消息上限 |
| [commands.md](commands.md) | `workspace/executeCommand` 34 条命令 + `fpgaLsp/*` 7 个方法的落点索引，及 initialize 能力声明对照 |
| [mcp-usage.md](mcp-usage.md) | MCP 工具怎么串：三条路线（读工程 / 跑 flow / 上板探针）、jobs 语义、常见坑 —— agent 的主要入口文档 |
| [quality.md](quality.md) | 静态分析流水线、两个计数陷阱、诊断基线、审计报告索引 |
| [status.md](status.md) | **按轮记录的工程日志**（当前 41 轮）：每轮改了什么、实测证据、仍未验证项；最新的交付数字以本文件末尾几轮为准 |
| [extension-development.md](extension-development.md) | VS Code 扩展的开发、测试与打包 |
| [design/](design/README.md) | **设计提案 + 落地状态**：端到端协议优化 32 条、后端功能缺口 25 条、插件功能 28 条；大部分已落地，剩余 backlog 见其 §3.5 |
| [design/write-service.md](design/write-service.md) | **统一写入服务（已实现 0.24.0，0.25 扩到目录/删除/改名 + 内存 FS/指标/dry-run，0.26 补 `Copy`）**：后端 `platform::write_service` + 插件 `writeService.js`、四类 root 策略、原子写/审计/dry-run、门禁与迁移清单 |

## 审计证据归档（按日期，只增不改）

| 目录 | 内容 |
| --- | --- |
| [doc/audit/2026-11-22/](audit/2026-11-22/SUMMARY.md) | **独立审计（团队模式，2026-11-22）**：5 份子审计（后端对等性 / 后端健康度 / 插件 / 文档 / 真机运行期）+ 汇总与 0.22.0 修复记录 |
| [doc/audit/2026-11-22/static/](audit/2026-11-22/static/README.md) | **静态分析复审（0.26.0，团队模式）**：性能 / 安全 / 现代化三份独立报告（clang-tidy 67 TU 全量证据） |
| [doc/audit/2026-11-22/perf2/](audit/2026-11-22/perf2/README.md) | **端到端性能审计（0.28.1 → 0.29.0，团队模式）**：LSP 热路径 / MCP+TD / 插件三份实测报告 + 10 项优化前后对比 |
| [doc/audit/2026-11-18/](audit/2026-11-18/) | Phase B 前基线、B1–B10 阶段报告、Python↔C++ 对拍、性能基准 |
| [doc/audit/2026-11-19/perf/](audit/2026-11-19/perf/) | 运行时性能剖析（CPU-PROFILE / INDEX / MCP / TD / VSCODE / REPORTS …） |
| [doc/audit/2026-11-19/fix/](audit/2026-11-19/fix/) | 修复波次（F1–F6、W1–W4）过程记录与协议门禁结果 |
| [doc/audit/2026-11-19/static/](audit/2026-11-19/static/) | 静态分析总报告与三份维度报告、LTCG 实验、修复记录 |

## 历史快照

| 文档 | 内容 |
| --- | --- |
| [history/delivery-2026-11-18.md](history/delivery-2026-11-18.md) | Phase B 交付快照（数字已被后续提交刷新，仅作证据链） |
| [history/gate-reports/](history/gate-reports/) | 阶段 0/1 的 parser、lint、formatter 门禁报告 |

## 留在原地的文档

这些文档与其数据/测试强绑定，因此不搬进 `doc/`：

| 位置 | 内容 |
| --- | --- |
| [tests/PYTHON-BUGS.md](../tests/PYTHON-BUGS.md) | Python 参考实现的缺陷登记（含 C++ 处置方式与测试锚点） |
| [tests/golden/README.md](../tests/golden/README.md) | golden 基线的组织与用法 |
| [tests/gate-corpus/README.md](../tests/gate-corpus/README.md) | 阶段 0 门禁语料说明 |
| [tests/td/PYTHON-PARITY.md](../tests/td/PYTHON-PARITY.md) | TD 相关行为的 Python 等价性记录 |
| [tests/fixtures/reports/README.md](../tests/fixtures/reports/README.md) | 报告解析测试夹具说明 |