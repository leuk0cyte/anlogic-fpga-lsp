# 质量门禁与静态审计

## 一键静态分析（`tools/audit/`）

| 工具 | 用途 |
| --- | --- |
| `run_audit.ps1` | 全流程：vcvars 自举 → 编译数据库新鲜度校验 → clang-tidy → MSVC `/analyze` → 汇总 |
| `make_clang_db.py` | cl.exe 命令行 → clang 可消费的编译数据库；`--verify <TU>` 做单 TU 语法检查 |
| `run_clang_tidy.py` | 逐 TU 跑 clang-tidy；`FPGA_LSP_AUDIT_DIR` 可指定输出目录；`--checks=-*,performance-*` 只跑某一维度（注意 `-*,` 前缀，否则会叠加默认集） |
| `run_msvc_analyze.ps1` | MSVC `/analyze`（含 analyzer control 自检段，证明分析器真的生效） |
| `summarize.py` | 去重口径聚合、模块密度、字面量感知的函数长度分布 |
| `check_tcl_invariant.py` | **Tcl 拼装不变量守卫**：禁止把不可信值裸拼进 `Run(...)` |

```powershell
pwsh -File tools\audit\run_audit.ps1 -Jobs 16        # 全流程
python tools\audit\check_tcl_invariant.py            # 只跑不变量守卫（适合 CI）
```

### 两个计数陷阱（不要用原始条数下结论）

1. **`--header-filter=src/` 让计数虚高约 30%**：同一头文件里的同一条诊断在每个包含它的
   TU 里各报一次。当前基线 **8 616 原始 → 6 092 唯一位置**。
2. **粗粒度花括号配对把「巨型函数」算大 10 倍**：字符串字面量里的 `{}` 会污染配对。
   用 `summarize.py` 的字面量感知扫描。

### 当前诊断基线

| 项 | 值 |
| --- | --- |
| clang-tidy | **67/67 TU 解析成功，11,154 原始 → 8,234 唯一位置**（2026-11-22，LLVM 23.1.5，0.26.0） |
| MSVC `/analyze` | 62 TU **0 条**（2026-11-19 口径；control 证明生效，但 `CppCoreCheck.dll` 未安装，C26xxx 未覆盖） |

## 审计证据归档（按日期）

| 目录 | 内容 |
| --- | --- |
| [`audit/2026-11-18/`](audit/2026-11-18/) | Phase B 前的基线、B1–B10 各阶段报告、Python↔C++ 对拍、性能基准 |
| [`audit/2026-11-19/perf/`](audit/2026-11-19/perf/) | 运行时性能剖析（CPU-PROFILE / INDEX / MCP / TD / VSCODE / REPORTS …） |
| [`audit/2026-11-19/fix/`](audit/2026-11-19/fix/) | 修复波次（F1–F6、W1–W4）的过程记录与协议门禁结果 |
| [`audit/2026-11-19/static/`](audit/2026-11-19/static/) | 静态分析总报告与三份维度报告（见下） |
| [`audit/2026-11-22/static/`](audit/2026-11-22/static/README.md) | **0.26.0 静态分析复审**：性能 / 安全 / 现代化三份独立报告 + 67 TU clang-tidy 全量证据 |

`audit/2026-11-19/static/` 的四份主报告：

| 报告 | 内容 |
| --- | --- |
| [REPORT.md](audit/2026-11-19/static/REPORT.md) | 三维度汇总、7 条必办项、方法论、修复路线图 |
| [SECURITY.md](audit/2026-11-19/static/SECURITY.md) | 威胁模型 + 15 条发现（全部带调用链）+ 16 项已正确防护 |
| [PERFORMANCE.md](audit/2026-11-19/static/PERFORMANCE.md) | 4 条确认 / 4 条可疑 / 76 条噪音逐条判定 |
| [MODERNIZATION.md](audit/2026-11-19/static/MODERNIZATION.md) | A/B/C 分级、10 主题记分卡、三批自动修复路线 |
| [LTCG.md](audit/2026-11-19/static/LTCG.md) | 5 组对照实验；推翻「flex 生成物与 `/GL` 不兼容」的旧归因 |
| [FIXES.md](audit/2026-11-19/static/FIXES.md) | 审计 §0.2 第 1–6 条的修复与验证记录 |

## 历史门禁报告

| 报告 | 内容 |
| --- | --- |
| [GATE-REPORT.md](history/gate-reports/GATE-REPORT.md) | 阶段 0 parser 门禁（token / CST / 错误消息对拍） |
| [LINT-GATE-REPORT.md](history/gate-reports/LINT-GATE-REPORT.md) | 阶段 1 lint 规则清单与诊断对拍 |
| [FORMAT-GATE-REPORT.md](history/gate-reports/FORMAT-GATE-REPORT.md) | formatter 全文档 / 局部区间对拍 |

## 建议接进 CI 的不变量

```powershell
python tools\audit\check_tcl_invariant.py     # Tcl 拼装：禁止裸拼不可信值
# 以及：MCP 工具的 file/path 入参必须经 SafeProjectFile / SafeProjectPath / ResolveFile
```

> 这两条对应的历史缺陷都属于「测试全绿但违反不变量」：良性输入下安全写法与裸拼写法
> 输出完全相同，对拍门禁抓不到。