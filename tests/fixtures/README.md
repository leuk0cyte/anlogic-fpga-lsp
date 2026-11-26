# release 分支：测试夹具说明（公开版）

> 本文件只存在于 `release` 分支。私有 `main` 分支保留了来自真实工程的完整语料与报告
> 夹具，公开版不含任何真实工程内容。

## 为什么有些测试会 skip / fail

公开版删掉了两类测试输入：

| 输入 | 为什么删 | 影响 |
| --- | --- | --- |
| `tests/gate-corpus/corpus_alpha/` | 原目录来自一份**企业内部** FPGA 工程（含他人版权头、员工工号与产品参数），不可公开 | 依赖它的单测与 e2e 用例会 **skip** |
| `tests/fixtures/reports/*.{qor,area,timing,status,rpt}` | 真实 TD 报告里有厂商 "All Rights Reserved" 版权头与内部绝对工程路径 | `unit.reports_*` / `unit.report_store` 等会 **fail**（找不到夹具） |
| `tests/golden/snapshots/` + `corpus/cases.json` | 快照里逐文件列了内部工程的源码文件名 | golden 对比不可用 |
| `tests/td/fixtures/read_hdl_corpus_alpha.txt` | 22 条内部绝对路径 | 两个 TD 用例改为读 `FPGA_LSP_TD_FIXTURE`，缺失则 skip |

## 想要全绿怎么办

**方案 A（推荐，任何贡献者都能做）**：生成**合成**夹具。

1. 在任意一份自己的 Verilog 工程上跑一遍 TD flow，得到 `*_Runs/<syn_1|phy_1>/` 下的
   `gate.qor` / `gate_timing.rpt` / `*_phy.area` / `flow.status`。
2. 把文件名对齐 `tests/fixtures/reports/` 期望的名字（见各测试的 `Fixture("…")` 调用）。
3. 放进本目录（注意：`release` 分支 `.gitignore` 排除了它，**本地跑测试可以，不会进仓库**）。

**方案 B**：用环境变量指向你自己的真实工程与报告目录（各脚本已支持）：

| 环境变量 | 用途 |
| --- | --- |
| `FPGA_LSP_TEST_PROJECT` | e2e / 真机测试要用的 `.al` 工程目录 |
| `FPGA_LSP_TD_FIXTURE` | `read_hdl` 夹具路径 |
| `FPGA_LSP_PYTHON_ROOT` | 若做与旧实现的对比，指向该实现 |
| `WINFLEXBISON_DIR` | 非 scoop 安装的 winflexbison |

## 面向上游贡献

公开仓库的目标是**可构建、可测试核心逻辑**：

* ✅ 能全过：`unit.hdl_index`、`unit.semantics`、`unit.mcp_*`、`unit.commands`、
  `formatgate*`、`unit.utf16`、`unit.probe` 等纯逻辑测试。
* ⚠️ 需自备数据：`unit.reports_*`（报告解析）、`td.*` / `e2e.*`（需要本机 TD 安装与工程）。

这两类都写成了"数据缺失时给出明确错误或跳过"，而不是悄悄通过 —— 这样外部贡献者知道
自己缺什么。