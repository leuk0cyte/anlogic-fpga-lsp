# `tests/gate-corpus/` —— 门禁语料目录

> 阶段 0（解析）/ 阶段 1（lint、formatter）三条门禁、以及 HDL 索引 / TD / Flow 的对比测试都默认使用本目录。


> **公开版本说明（重要）**
>
> 本目录**不再包含真实工程语料**。上游的 `corpus_alpha/`（一份企业内部 FPGA 工程的副本，
> 含他人版权头、员工工号与产品参数）已从公开版本中移除 —— 未经授权不能分发。
>
> 保留下来的只有：
> * `verible_lint_testdata/` —— vendored Verible 自带的 lint 测试集（Apache-2.0）；
> * `error-cases/` —— 手工构造的语法错误样例（本仓库原创）。
>
> 依赖"真实工程语料"的用例（`unit.hdl_index` 的 `HdlCorpus.*`、`unit.constraint_symbols`、
> `e2e.*`、`td.*`、`parity.*` 等）在缺语料时会 **skip / Not Run**，而不是失败。
> 想跑它们：把自己的一份 `.al` 工程放到 `%WORKSPACE%/corpus_alpha/`，或设置
> `FPGA_LSP_TEST_PROJECT=<工程目录>`（详见 `tests/fixtures/README.md`）。

## ⚠️ 最重要的一条：不要在本目录里直接跑 TD

`corpus_alpha/demo_project.al` 的根节点写着：

```xml
<Project Version="3" Minor="2" Path="C:/work/CorpusAlpha">  <!-- 示例：自备语料的 .al 根节点写法 -->
```

这个 `Path` **不是**"只是元数据"。实测（`reports-parse`，阶段 4-δ）：

- TD `open_project` 之后会拿 `Path` 指向的目录当**工程根**
- 于是 `*_Runs`、`.fpgalsp/` 会落在**那里**，而不是你复制出来的临时目录
- 也就是说：**在本目录里直接跑 `runFlow` 会写到 `Path` 指向的那个目录**（自备语料时请自行确认它不是你的真实工程）

2026-11-18 曾发生过一次：`e2e.lsp_smoke` 的 `runFlow → cancelFlow` 把 `.stop.f` 与
半成品 run 写进了别处，导致 `td.flow` 的 `RunsASynthesisToCompletion` 读到被取消的
run、`bundle.files.area` 找不到而假失败。当时那个 `Path` 目录侥幸没被写
（最新文件时间仍是 2026/11/9），但那是运气，不是设计。

### 因此：任何要跑 TD 的测试必须做三件事

1. `copytree` 到 `tempfile.mkdtemp()`，**排除** `*_Runs` 与 `.fpgalsp`
   （被取消过的 run 目录里有 `.stop.f`，TD 每一步都会检查它，`reset_runs` 行为不确定）
2. **改写副本 `.al` 根节点的 `Path` 为副本目录** —— 这一步才是隔离的关键
3. 跑完断言**原语料目录没有被新增** `*_Runs` / `.fpgalsp`

参考实现：`tests/td/flow_integration_test.cpp` 的 `RewriteProjectPath()` 与
`SharedCorpusStaysClean` 用例。

## 本目录的内容来源

| 子目录 | 来源 | 用途 |
| --- | --- | --- |
| `corpus_alpha/` | **本仓库不提供**（上游曾放一份企业工程副本，已按合规要求移除）。需要时自备：`FPGA_LSP_TEST_PROJECT` 或放到 `%WORKSPACE%/corpus_alpha/` | gate/compare 类用例的可选输入 |
| `verible_lint_testdata/` | vendored Verible 的 lint testdata（73 个 `.sv`） | SystemVerilog 特性覆盖 |
| `error-cases/` | 手工构造（8 个故意语法错误的 `.sv`） | 验证 failsafe 与错误恢复 |

`.vhd` 不参与 Verible 门禁（Verible 不支持 VHDL），但保留用于 HDL 索引的 entity 提取测试。

## 已知的"故意保留"的陷阱

| 现象 | 为什么保留 |
| --- | --- |
| `.al` 的 `Path` 指向别处 | 这是 **PRJ-6619** 场景的真实样本："工程被拷走后 TD 回退到原路径"。`tests/unit/al_project_test.cpp` 依赖它验证 `resolved_via="declared"` 的三态语义 |
| `corpus_alpha` 里有 IP 生成的 `.v` 与 `.vhd` | 用于验证 `tests/PYTHON-BUGS.md` **P-1**（Python 把 `.vhd` 混进 Verilog 列表导致 elaborate 失败） |
| 同名模块 `pwm_gen` 出现两次 | 用于验证 `duplicate_modules` 与重名检测 |

**因此不要"清理"这些看似异常的地方**——它们是测试样本，不是遗留问题。