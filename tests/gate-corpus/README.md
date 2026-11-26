# `tests/gate-corpus/` —— 门禁语料目录

> 阶段 0（解析）/ 阶段 1（lint、formatter）三条门禁、以及 HDL 索引 / TD / Flow 的对比测试都默认使用本目录。

## ⚠️ 最重要的一条：不要在本目录里直接跑 TD

`corpus_alpha/demo_project.al` 的根节点写着：

```xml
<Project Version="3" Minor="2" Path="C:/work/CorpusAlpha">
```

这个 `Path` **不是**"只是元数据"。实测（`reports-parse`，阶段 4-δ）：

- TD `open_project` 之后会拿 `Path` 指向的目录当**工程根**
- 于是 `*_Runs`、`.fpgalsp/` 会落在**那里**，而不是你复制出来的临时目录
- 也就是说：**在本目录里直接跑 `runFlow` 会写到 `C:/work/CorpusAlpha`（另一个真实工程）**

2026-11-18 曾发生过一次：`e2e.lsp_smoke` 的 `runFlow → cancelFlow` 把 `.stop.f` 与
半成品 run 写进了别处，导致 `td.flow` 的 `RunsASynthesisToCompletion` 读到被取消的
run、`bundle.files.area` 找不到而假失败。当时 `C:/work/CorpusAlpha` 侥幸没被写
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
| `corpus_alpha/` | `C:/work/CorpusAlpha` 的副本（20 个 `.v` + IP 生成的 `pll_gen.v` + `pll_gen.vhd`） | 真实工程语料 |
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