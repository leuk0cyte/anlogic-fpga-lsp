# 质量门禁与静态审计

## 一键静态分析（`tools/audit/`）

| 工具 | 用途 |
| --- | --- |
| `run_audit.ps1` | 全流程：vcvars 自举 → 编译数据库新鲜度校验 → clang-tidy → MSVC `/analyze` → 汇总 |
| `make_clang_db.py` | cl.exe 命令行 → clang 可消费的编译数据库；`--verify <TU>` 做单 TU 语法检查 |
| `run_clang_tidy.py` | 逐 TU 跑 clang-tidy；`FPGA_LSP_AUDIT_DIR` 可指定输出目录；`--checks=-*,performance-*` 只跑某一维度（注意 `-*,` 前缀，否则会叠加默认集） |
| `clang_tidy_gate.py` | **clang-tidy 棘轮门禁**（CTest `docs_clang_tidy`，**默认不注册**：需 `-DFPGA_LSP_ENABLE_TIDY_GATE=ON`；退出码 0/1）：根 `.clang-tidy` 的缺陷优先 check 集 + `clang_tidy_baseline.json` 基线比对——**基线外新诊断即失败**，基线项消失只提示收紧；`--only/--jobs/--timeout` 可调。`--update-baseline` 重写基线，**只能人工确认后使用**（见下节纪律）。必须经 `with_vcvars.cmd` 运行 |
| `run_msvc_analyze.ps1` | MSVC `/analyze`（含 analyzer control 自检段，证明分析器真的生效） |
| `summarize.py` | 去重口径聚合、模块密度、字面量感知的函数长度分布 |
| `check_tcl_invariant.py` | **Tcl 拼装不变量守卫**：禁止把不可信值裸拼进 `Run(...)` |

```powershell
pwsh -File tools\audit\run_audit.ps1 -Jobs 16        # 全流程
python tools\audit\check_tcl_invariant.py            # 只跑不变量守卫（适合 CI）
cmake --preset windows-msvc-dev -DFPGA_LSP_ENABLE_TIDY_GATE=ON   # 先注册（默认关闭）
ctest -L tidy                                        # clang-tidy 棘轮门禁（~145 s）
```

### clang-tidy 棘轮门禁（`ctest -L tidy`，**默认关闭**）

> ⚠ **默认不进 `ctest`**：逐 TU 跑 clang-tidy 要 ~145 s，放在日常回归里太慢。
> 需要时显式打开（默认全量 `ctest` 仍是 71 项、~90 s）：
>
> ```powershell
> cmake --preset windows-msvc-dev -DFPGA_LSP_ENABLE_TIDY_GATE=ON   # 注册 docs_clang_tidy
> ctest --preset windows-msvc-dev -L tidy
> # 或者不碰 CTest，直接跑脚本（脚本会自己经 with_vcvars 自举 MSVC 环境）：
> cmd /c tools\audit\with_vcvars.cmd python tools\audit\clang_tidy_gate.py
> ```
>
> `FPGA_LSP_ENABLE_TIDY_GATE` 在顶层 `CMakeLists.txt` 里定义，默认 `OFF`。

**为什么是棘轮而不是零告警**：审计口径的全量 check 集在本仓库是 1.1 万条诊断量级，
那种规模只能出报告、不能当门禁（永远红 = 训练人去关掉它）。门禁要回答的是
"这次改动有没有变差"，所以既有诊断全部记入基线，只拦新增。

* 根 [`.clang-tidy`](../.clang-tidy) 只收**缺陷优先**的 check：`bugprone-*`、
  `clang-analyzer-*`、`performance-*`、`cert-err33-c`/`cert-err34-c`（`atoi` 家族）等
  高信号项、`modernize` 的 avoid-c-arrays / use-nullptr / use-override / use-emplace。
  风格类（`readability-*`、`modernize-use-nodiscard` 1598 条、`performance-enum-size`
  166 条 …）**不进 gate**；每个关闭项的理由写在文件头的 OFF 列表里。
  `WarningsAsErrors` 刻意不开——门禁靠基线，不靠"一键全红"。
* 基线 `tools/audit/clang_tidy_baseline.json` 以 `file|line|check` 唯一化（头文件诊断会在
  每个包含它的 TU 各报一次，只算一条）。
* 判定：出现基线外诊断 → **exit 1**（打印 `file:line:col` + check + 消息）；基线项本次
  已不存在 → 只提示"可收紧基线"，不算失败（`--strict-baseline` 可要求基线精确）；
  TU 解析失败/超时 → 失败（**无法验证 ≠ 通过**），且此时拒绝 `--update-baseline`。
* **`--update-baseline` 纪律**：只在两种情况下用 —— ①修掉一批诊断后收紧基线；
  ②人工逐条确认新增诊断是"接受的技术债"。**禁止为了让门禁变绿而盲目 update**，
  那等于把棘轮拆掉，正是"连续两轮缺门禁"的同一个病根。
* 必须经 `with_vcvars.cmd` 运行（clang 前端需要 MSVC STL/CRT 的 include 路径）：

```powershell
cmd /c tools\audit\with_vcvars.cmd python tools\audit\clang_tidy_gate.py          # 门禁
cmd /c tools\audit\with_vcvars.cmd python tools\audit\clang_tidy_gate.py --only src/hdl --jobs 4
```

### 两个计数陷阱（不要用原始条数下结论）

1. **`--header-filter=src/` 让计数虚高约 30%**：同一头文件里的同一条诊断在每个包含它的
   TU 里各报一次。当前基线 **8 616 原始 → 6 092 唯一位置**。
2. **粗粒度花括号配对把「巨型函数」算大 10 倍**：字符串字面量里的 `{}` 会污染配对。
   用 `summarize.py` 的字面量感知扫描。

### 当前诊断基线

| 项 | 值 |
| --- | --- |
| clang-tidy（**审计口径**：`tools/audit/run_clang_tidy.py` 的全 check 集） | **67/67 TU 解析成功，11,154 原始 → 8,234 唯一位置**（2026-11-22，LLVM 23.1.5，0.26.0） |
| clang-tidy（**门禁口径**：只跑根 `.clang-tidy` 的缺陷优先子集，默认不进 ctest） | **142 条唯一位置 / 67 TU**（2026-11-22 21:45，LLVM 23.1.5，0.28.0 B/C 批量修复后；0.27.0 为 281、修复前 320）。此数只许下降 |
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