# Golden 快照：Python 参考实现 ↔ C++ 重写的等价性验收

本目录是 **Python 参考实现（`Y:\fpga-lsp`）与 C++ 重写（`Y:\fpga-lsp-cpp`）之间做等价性对比** 的基础设施。
思路很简单：用同一份固定语料，把 Python 后端的输出采集为 JSON 快照（golden），之后 C++ 实现跑同样的
采集，两份快照 diff 为空即通过。

对比侧只做"同一输入 → 两边结构化 diff"，不判断谁对谁错：**Python 的输出就是契约**。

---

## 1. 目录结构

```
tests/golden/
├── README.md                      本文件
├── corpus/
│   └── cases.json                 固定语料清单（只描述路径，不复制源文件）
├── snapshots/                     采集结果，入 git（collect.py 的默认输出目录）
│   ├── _meta.json                 本次采集的元信息（类别、用例数、skip 数、上限）
│   ├── corpus.manifest.json       采集时实际看到的语料快照（自描述）
│   ├── project/project.json       a) 工程信息
│   ├── hdl/hdl.json               b) 符号/定义/引用/悬停
│   ├── semantics/semantics.json   c) 语义高亮/折叠/内联提示
│   ├── hierarchy/hierarchy.json   d) 模块层次树
│   ├── diagnostics/diagnostics.json  e) TD 诊断（需要真实 TD）
│   └── mcp/mcp.json               f) MCP 只读工具
└── tools/
    ├── collect.py                 采集脚本（主入口）
    ├── compare.py                 对比脚本
    ├── rebuild_snapshots.py       从一次采集结果重建 snapshots/（不重启 TD）
    └── goldenlib/
        ├── client.py              LSP / MCP 的 stdio JSON-RPC 客户端
        └── normalize.py           归一化 + 序列化 + 结构截断
```

采集时的临时目录（服务日志、synthesized 语料）默认放 `tests/golden/_sandbox/`，
可用 `--sandbox` 指到别处；它不入 git，跑完可随时删。

---

## 2. 如何采集

依赖：Python 3（仅标准库）、`Y:\fpga-lsp` 参考仓库、可选的真实 TD。

```bat
cd Y:\fpga-lsp-cpp
python tests\golden\tools\collect.py
```

**耗时参考（本机实测）**：全量六类约 **12–13 分钟**（其中 `diagnostics` 主要为起 TD 会话）。
只跑轻量三类（`project` / `hdl` / `semantics` / `hierarchy`）约 1–2 分钟。

常用参数：

| 参数 | 作用 |
| --- | --- |
| `--out DIR` | 输出目录（默认 `tests/golden/snapshots`） |
| `--only CAT` | 只采集某类（可重复）；`CAT ∈ project hdl semantics hierarchy diagnostics mcp` |
| `--skip CAT` | 跳过某类（可重复） |
| `--clean` | 先清空输出目录 |
| `--no-td` | 绝不启动真实 TD；`diagnostics` 全部记 skipped |
| `--repo DIR` | 参考仓库路径（默认读 `cases.json` 的 `referenceRepository`） |
| `--corpus FILE` | 语料清单路径 |
| `--sandbox DIR` | 临时目录（日志 / synthesized 语料） |
| `--server {python,native}` | 采集对象：Python 参考实现（默认）或 `fpgalsp-native.exe` |
| `--native-exe FILE` | native 可执行文件路径（默认 `build/bin/fpgalsp-native.exe`，可用 `FPGALSP_NATIVE_EXE` 覆盖） |

### 采集 C++ 原生实现（`--server native`）

同一套语料与归一化规则也可以直接驱动 C++ 重写，用来做「同一输入 → 两侧快照结构 diff」：

```bat
python tests\golden\tools\collect.py --server native --no-td ^
    --only project --only hdl --only semantics --only hierarchy --only mcp ^
    --out tests\golden\runs\native
python tests\golden\tools\compare.py tests\golden\snapshots tests\golden\runs\native
```

- native 的 argv 是 `fpgalsp-native.exe --stdio` / `--mcp --root <project>`，协议与 Python 侧相同
  （`goldenlib.client.NativeLspClient` / `NativeMcpClient` 只换了 argv）。
- native 模式的 sandbox 自动加 `-native` 后缀，日志不会和 Python 采集混在一起。
- `_meta.json` 会记录 `server` 与 `nativeExe`，便于回溯本次快照来自哪个二进制。
- 注意：MCP 默认只暴露只读工具（`--allow-write` / `--allow-flow` 未开），与 Python 的
  `visible_tools` 规则一致；`tools/list` 数量少于 39 是预期行为。

退出码：采集器自身健康即 **0**。个别用例采不到（工程不存在、TD 不可用、超时、JSON-RPC 报错）不会让整次
采集失败，而是记进该类别快照的 `skipped` 数组并打印 `SKIP`/`ERROR` 行——环境抖动不能打断流水线。

### 只读保证

采集只发送 `initialize` / `initialized` / `textDocument/didOpen` 和只读请求；
**不发送** `didChange`、保存、`addSourceFile`，也不调用任何写类 MCP 工具。
四个语料工程的源文件不会被修改。

### `diagnostics` 类别在沙箱副本上跑（重要）

参考实现会往 `<project>/.fpgalsp/` 写东西——TD 设备引脚缓存和 TD 会话日志：

```python
# fpgalsp/server.py, fpgalsp/mcp_bridge.py
cache_dir = os.path.join(project.dir, ".fpgalsp", "cache")
# fpgalsp/server.py: TD 会话的 cwd
return os.path.join(os.path.join(project.dir, ".fpgalsp"), "run")
```

`project.dir` 由 `.al` 文件位置推出，所以**只要让服务对着副本跑，所有写入就落在副本里**。
因此 `diagnostics` 类别固定按下面流程走，这不是可选项：

```
tests/golden/_sandbox/diagnostics/projects/<corpus_id>/   ← 复制整个工程（排除 .fpgalsp/.git/日志）
  → 校验副本自洽（重新解析 .al，确认每个源文件都能在副本内解析）
  → 对着副本起 TD 会话、采集诊断
  → 清理副本（--keep-sandbox-projects 可保留以便复跑）
```

**用户工程目录永远不会被写入。** 两道保险：

1. 复制时排除 `.fpgalsp/`、`.git/`、`__pycache__/`、`node_modules/` 和 `.log/.logw/.bak/.tmp/.swp`；
2. 复制后调用 `verify_copied_project()` **重新解析 `.al`**，把记录的所有源文件路径逐个检查
   是否都落在副本内。只要有一个指向副本之外（`.al` 里写绝对路径就会这样），该工程直接记
   `skipped`、副本删除、**不对原工程做任何操作**，并在日志里报出具体是哪个字段。

实测三个语料工程复制后都能 100% 自洽（CorpusAlpha 22 个路径、self_pwm_gen 23 个、CorpusBeta 20 个，
全部在副本内解析）。

> 之所以必须这样：参考实现把 `.fpgalsp/` 写在 `project.dir` 下是它的**既定行为**，短期内不会改。
> 我们不能要求它别写，只能保证它写在一个我们可控、可丢弃的副本上。

---

## 3. 如何对比

```bat
python tests\golden\tools\compare.py <快照目录_a> <快照目录_b>
```

- `a` = 参考（Python 侧快照），`b` = 候选（C++ 侧快照）。
- 报告为 markdown，写到 stdout；按类别汇总一致率。
- 退出码：**全部一致 0，有任何差异 1**（目录不存在等用法错误为 2）。
- `--quiet` 只给退出码，不打印报告；`--max-per-file N` 控制单文件最多列多少条差异。

### 从一次采集结果重建 `snapshots/`

不想为改一行归一化规则就重跑 13 分钟（尤其 `diagnostics` 要起 TD）时：

```bat
python tests\golden\tools\collect.py --out tests\golden\runs\A      :: 采集一次
python tests\golden\tools\rebuild_snapshots.py tests\golden\runs\A  :: 反复重建 snapshots/
```

`runs/` 是临时目录，不要提交；只有 `snapshots/` 入 git。
若那次采集的某个类别已经在采集期被折叠成 digest，重建会直接报错退出，而不是悄悄产出空文件。

会判定为差异的情况：

1. 两边文件集合不同（缺文件 / 多文件）；
2. 同一快照里 case id 不同（缺用例 / 多用例）；
3. 树里任意位置的值不同（含类型不同，例如 `1` vs `"1"`、`true` vs `1`）；
4. **列表顺序不同**——顺序本身是契约；
5. 一侧有截断标记而另一侧没有，或截断标记的内容（digest / droppedItems）不同。

### C++ 实现的通过标准

> **`compare.py` 对 Python 快照与 C++ 快照的比对结果 diff 为空、退出码 0，即通过。**

C++ 侧只需实现同样的请求，并把输出按本文件的归一化规则写成同名 JSON，即可直接比对。
如果某处是有意不实现（例如不打算做语义高亮），请在对比时用 `--skip` 不采该类，并在评审记录里写明，
不要用"放宽 diff"的方式通过。

---

## 4. 归一化规则

快照能用作验收前提是**稳定**：同一输入连跑两次必须字节相同。以下规则由 `normalize.py` 统一执行，
采集时已生效，C++ 侧不需要重复实现（只要输出与 Python 侧同样结构化，就可被同一套规则处理）。

### 4.1 路径统一

| 原样 | 归一化后 |
| --- | --- |
| `%WORKSPACE%\CorpusAlpha\src\top.v` | `<corpus:corpus_alpha>/src/top.v` |
| `Y:/fpga-lsp/tests/e2e_lsp.py` | `<repo>/tests/e2e_lsp.py` |
| `C:\Anlogic\TD_Release_2026.2.210.850\bin\td_commands_prompt.exe` | `<abs:>.../bin/td_commands_prompt.exe` |
| `%WORKSPACE%/CorpusAlpha`（非语料根，出现在诊断文案里） | `<abs:>.../sic/CorpusAlpha` |

- 所有分隔符统一为正斜杠。
- 语料根 → `<corpus:<id>>`，参考仓库 → `<repo>`，sandbox → `<sandbox>`；匹配时按最长根优先、忽略大小写
  （Windows 路径大小写不敏感，服务端两种写法都会出现）。
- 认不出的绝对路径折叠为 `<abs:>.../<父目录>/<末名>`，只保留尾部两级，避免不同机器/检出位置造成 diff。
- **为什么要保留尾巴而不是只留文件名**：只留文件名会让不同目录下的同名文件（各工程的 `top.v`）互相混淆。

### 4.2 丢弃的时间类字段

这些 key **整个删掉**（值本身就是随机的）：

```
generatedAt mtime startedAt finishedAt elapsed elapsedMs elapsedSeconds runId run_id
sessionId session_id timestamp time duration durationMs durationSeconds pid
lastUpdate updatedAt createdAt bootTime uptimeMs
td_session device_loaded
```

最后两个 `td_session` / `device_loaded` 值得单独说明——**它们是实测发现的不稳定字段**，见第 11 节。

### 4.3 遮罩的时间类片段

有些时间戳嵌在**语义字符串**里（例如 TD 运行目录 `td_20261109_141838`），整个丢掉会损失信息，
所以只把时钟部分替换成 `<TS>`：

- `YYYYMMDD_HHMMSS`、ISO-8601、`MM-DD HH:MM:SS`、`YYYY/MM/DD HH:MM:SS`。

### 4.4 排序

- **字典 key 一律排序**后再序列化（`sort_keys=True`），消除插入顺序差异。
- **列表保持原顺序**——顺序是契约的一部分，不排序。

### 4.5 数值

浮点数统一 `round(…, 6)`，吸收无意义的末位差异。结构化数据里基本都是坐标和计数，6 位远超契约所需精度。

### 4.6 序列化

`indent=2`、`sort_keys=True`、`ensure_ascii=False`、UTF-8 无 BOM、换行 `\n`、结尾带一个换行。

---

## 5. 快照截断约定

单文件上限 **2 MB / 2000 行**。超限时**不做文本截断**（那会产生非法 JSON，还会在某个巨大 case 中间
一刀切掉后面所有 case），而是**结构化收缩**：

1. 先按"最大块优先"收缩，列表从尾部缩短；
2. 被丢弃的部分替换成带 digest 的标记：
   `{"__goldenTruncated": true, "_note": "...", "bytes": N, "sha256": "..."}`；
   被缩短的列表末尾追加 `{"__goldenTruncated": true, "droppedItems": N, ...}`；
3. 每层按剩余预算**平均分配**，避免单个超大元素把其他元素挤掉；
4. 极端情况下（2000 行连骨架都放不下）整体折叠为单个标记，并在 `caseIds` 里保留全部 case id。

**任何时候文件都是合法 JSON 且在上限内。** 收缩只删内容、从不臆造内容，所以对截断过的快照，
C++ 实现无法靠"猜"通过；`compare.py` 会为这类文件打印 **Truncation warnings**，提示"相等"只是
"两边同样不完整"，不构成完整等价性证明。

> 当前真实语料的体量（`hdl` / `hierarchy` 尤其大）会触发截断。这些类别改成了"结构 + digest"的比对：
> 内容一旦变化，digest 立刻不同，仍能可靠发现回归；代价是看不到具体差异字段。
> 需要逐字段精确定位时，用 `cases.json` 里的 `syntheticCases` 增补小体量用例。
>
> 说明：同一份 Python 后端既产生基线也产生候选，两侧的收缩是确定性且等价的，所以真实基线里
> 保留的是**部分收缩**的顶层数组（截断标记只挂在被裁掉的尾部），`compare.py` 判定为一致。
> 只有满足"对侧多出一个尾部元素且多出来的部分可摘要"这种形状时，收缩会产生**扁平标记**
> （顶层 `caseIds` + `identity`）；C++ 侧若完全不产出该文件内容，就会命中这种形状，
> 此时 `compare.py` 按差异报告——这是有意为之，宁可报差异也不给假通过。

---

## 6. 语料清单

见 `corpus/cases.json`。规则：

- 只记录路径，**不复制**工程源文件；
- 路径不存在 → 该工程记号 `skipped`，采集继续；
- 只读打开，不做任何写操作。

当前登记：

| id | 路径 | 说明 |
| --- | --- | --- |
| `CorpusAlpha` | `%WORKSPACE%\CorpusAlpha` | EF2L45LG144B，top=`top`，21 个 HDL 文件 |
| `CorpusAlpha_self_pwm_gen` | `%WORKSPACE%\CorpusAlpha_self_pwm_gen` | 同系列变体，23 个 HDL 文件 |
| `CorpusBeta` | `Y:\sic\unit_test\CorpusBeta` | EG4X20BG256，top=`main`，跨器件族 |
| `CorpusGamma` | `Y:\CorpusGamma` | **本机不存在** → skipped |
| `CorpusDelta` | `Y:\CorpusDelta` | **本机不存在** → skipped |
| `CorpusEpsilon` | `Y:\CorpusEpsilon` | **本机不存在** → skipped |

后三个是任务书要求登记但本机没有的工程；保留登记是为了将来这些工程出现时自动纳入，
不需要改脚本。

> `Y:\fpga-lsp\tests\fixtures` 不存在，原计划的"fixtures 下的 Verilog 文件"改用
> `syntheticCases`（写入 `_sandbox/`，不触碰参考仓库）。需要固定边界用例时往那里加即可。

### 采集上限（写死在 `collect.py`，并记录在 `_meta.json` 的 `limits`）

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `MAX_PROJECT_HDL_FILES` | 10 | 每个工程最多采多少个 HDL 文件的 hdl/diagnostics |
| `MAX_POSITIONS_PER_KIND` | 3 | module/port/instance 每类最多几个探针位置 |
| `MAX_REFERENCES_CASES` | 6 | 每个文件最多几次 references 请求 |
| `MAX_SEMANTIC_FILES` | 8 | semantics 类别总共采几个文件 |
| `MAX_MCP_POSITIONS` | 4 | MCP 位置类工具的候选探针数 |

改这些常量会改变 golden 集合：**改了就要重新采集**，并在提交信息里说明。

---

## 7. 类别与用例

| 类别 | 请求 | 用例粒度 |
| --- | --- | --- |
| `project` | `workspace/executeCommand` 的 `fpgalsp.info` / `fpgalsp.projects` / `fpgalsp.device` | 每工程一个 case |
| `hdl` | `textDocument/documentSymbol`、`workspace/symbol`、`definition`、`references`、`hover` | 每个 HDL 文件一个 case；探针位置按 module/port/instance 各取最多 3 处 |
| `semantics` | `semanticTokens/full`、`foldingRange`、`inlayHint`、`documentSymbol` | 每个 HDL 文件一个 case |
| `hierarchy` | `fpgaLsp/moduleTree` | 每工程一个 case |
| `diagnostics` | `fpgalsp.check` → 收集 `textDocument/publishDiagnostics` | 每工程一个 case，含每个文件的诊断 |
| `mcp` | MCP `tools/call` 的只读工具 | 每工程一个 case，内含各工具的结果 |

### 探针位置怎么定的

`hdl` / MCP 的位置类请求需要"某个具体位置"。位置**不从服务端问**，而是用 `DocPositions`
从文本里按正则确定性地挑（先剥掉 `//` 与 `/* */` 注释，避免采到注释里的假代码），
每类按出现顺序最多取 N 个。这样同一份语料永远得到同一组探针，快照才稳定。

### MCP 只读工具

采 13 个只读工具（`annotations.readOnlyHint = true` 且不依赖运行中的流程）：
`fpga_project_list`、`fpga_project_info`、`fpga_project_files`、`fpga_project_structure`、
`fpga_document_symbols`、`fpga_workspace_symbols`、`fpga_definition`、`fpga_references`、
`fpga_hover`、`fpga_completion`、`fpga_duplicate_modules`、`fpga_diagnostics`、`fpga_device_info`。

排除的写类 / 副作用类工具及理由记在 `cases.json` 的 `mcpTools.excluded`。

**MCP 结果按工具逐个压缩**：单个工具结果常常比整个快照预算还大（`fpga_document_symbols` 对
`top.v` 约 47 KB、整份 mcp 结果约 400 KB，光排版就远超 2000 行）。如果交给通用的结构收缩，
一个巨型 case 会把整类折叠成 digest，文件就失去可 diff 性。所以在**采集时**就按工具做：

- 结果 ≤ 40 行且 ≤ 4 KB → 原样保留（实测 13 个工具里 7 个能原样放下）；
- 否则保留 `arguments` + `resultKeys`（顶层字段名），结果本体换成 digest，放在 `resultShape` 里。

这样任一工具的输出变化都会让对应 `resultShape.sha256` 改变，diff 能**精确指到是哪个工具**。

---

## 8. 快照稳定性检查（每次改了采集器都要跑）

**连跑两次，快照目录必须字节相同。** 这是所有 diff 的前提，也是本任务最核心的验收项。

```bat
cd Y:\fpga-lsp-cpp
python tests\golden\tools\collect.py --out tests\golden\runs\A
python tests\golden\tools\collect.py --out tests\golden\runs\B
python tests\golden\tools\compare.py tests\golden\runs\A tests\golden\runs\B   :: 期望退出码 0
```

想更严格地排除"compare 把两边解析成同样结构"的可能，可以直接比字节：

```bat
fc /b tests\golden\runs\A\project\project.json tests\golden\runs\B\project\project.json
```

---

## 9. 在 CI / 评审中使用

采集成本差别很大，所以分两个标签跑（CMake 里注册成 `ctest -L <label>`）：

| 命令 | 覆盖 | 耗时 |
| --- | --- | --- |
| `ctest -L golden-light` | `project` / `hdl` / `semantics` / `hierarchy` | 约 1–2 分钟 |
| `ctest -L golden-full` | 全部六类（含 `diagnostics` + `mcp`，需要真实 TD） | 约 13 分钟 |

**CI 每次提交只跑 `golden-light`；`golden-full` 放 nightly。**

1. 采 Python 侧快照（本目录 `snapshots/` 即基线，或现场重采到临时目录）；
2. 采 C++ 侧快照到另一个临时目录；
3. `python tests/golden/tools/compare.py <python> <cpp>` → 退出码 0 才算通过；
4. 报告贴进 PR，重点看"Per-category agreement"表里非 100% 的行。

对某一类暂时不要求等价（例如尚未实现 `inlayHint`），用 `--skip` 在采集阶段就不采该类，
保持"采了就要求一致"的强约束。

---

## 10. 已知限制

- **依赖本机环境**：语料工程、TD 安装位置、参考仓库路径都会影响结果；换机器要重采基线，
  不能直接拿本机快照去比别人的机器。
- **快照体积**：`hdl` / `hierarchy` 体量大，会触发第 5 节的截断。
- **`diagnostics` 依赖真实 TD**，首次跑很慢（TD 要起会话、导入器件）。超时记为 skipped。
- **`mcp` 每个工程起一个独立 MCP 进程**，比 LSP 类别慢。
- 参考实现的行为若本身不稳定（同输入两次不同），`diffs` 里会直接暴露出来——这属于**参考实现的缺陷**，
  应在开始 C++ 重写前先修 Python 或明确契约，而不是去放宽归一化规则。

---

## 11. 实测的参考实现不稳定行为

采集过程本身就是一个"参考实现是否确定"的探针。本机实测结论如下。

### 11.1 已确认不稳定：`fpgalsp.info` 的 `td_session` / `device_loaded`

> ⚠️ **这是参考实现的契约缺陷，不是正常的 volatile 字段。**
> `td_session` / `device_loaded` 反映 TD 子进程此刻是否就绪，取决于 `check` 与 `info`
> 轮询之间的竞态，与输入无关。
> 我们在归一化中丢弃它们，是为了让基线可复现；**这不代表它们是合理设计**。
> C++ 实现应提供确定性替代（如 `starting/ready/failed` 状态机 + 单调 generation），
> 届时这两个字段应被移除或重新定义。

两次参数完全相同的全量采集，`diagnostics` 快照里出现且仅出现这一处差异：

```
case:CorpusAlpha_self_pwm_gen/result/info/td_session   true -> false
```

其余 3 个工程的 `files[*]` 诊断**逐字节相同**，说明诊断内容本身是确定的；不稳定的只是
"此刻 TD 子进程是否已经就绪"这个**实时状态位**：

- `td_session`：TD 子进程当前是否活着；
- `device_loaded`：设备引脚表是否已经加载完。

它们取决于 `fpgalsp.check` 与 `fpgalsp.info` 轮询之间的竞态——同一个工程上一轮拿到
`true`，下一轮拿到 `false`，与输入无关。

**当前处置（临时、有代价）**：在归一化规则里丢弃这两个 key（`GOLDEN_VOLATILE_KEYS`），
会话**产出**（pins、device 表、诊断）仍然比对。这样做**掩盖了一个真实缺陷**：

- 基线因此变得可复现 ✅
- 但代价是：如果 C++ 实现的会话状态机真有 bug（比如永远不进入 ready），
  这个 diff **抓不到** ❌

所以它只能作为**过渡措施**。等 C++ 侧的确定性状态机落地后，应把这两个字段从
`GOLDEN_VOLATILE_KEYS` 里移除，并重新定义断言（例如"最终必须到达 `ready`"）。

> 注意：这两个字段属于"进程实况"，不适合作为对外契约。在此之前，等价性验收不应把它们算作差异；
> 但也**不能**把它们当作"设计上就该随机"来长期豁免。

### 11.2 已验证稳定

同一输入连跑两次（各自独立起进程、独立起 TD），以下内容**逐字节相同**：

`_meta.json`、`corpus.manifest.json`、`project`、`hdl`、`semantics`、`hierarchy`、`mcp`
七个文件；`diagnostics` 在剔除 11.1 的两个字段后亦相同（第二次对照重采时连这两处也一致，
说明是偶发竞态而非固定偏移）。

这证明：`generate → 归一化 → 结构收缩` 整条链路没有引入随机性，快照可直接当契约用。