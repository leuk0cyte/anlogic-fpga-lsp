# Python 参考实现（`Y:\fpga-lsp` 0.10.32）已知问题登记

> **编辑约定（避免多人重写）**
> 1. 新增条目一律**追加到文件末尾**，不要插入中间。
> 2. 编号只增不复用；发现冲突以**总览表**为准。
> 3. 不要重排、不要"整理"已有小节——并行编辑时任何重排都会吞掉别人的内容。
> 4. 改动已有条目时，在条目末尾注明"（YYYY-MM-DD 由 <成员> 修订）"——P-7 被推翻过一次，修订痕迹比悄悄改掉安全。
> 5. **编号有重复**（并行开发撞号，见「⚠ 重复编号对照」）——**引用时必须带标题**，不要只写 P-16。
>
> **顺带记一笔工具坑（本轮真实事故）**：用 PowerShell 做多行文本替换时，
> **单引号字符串里的「反引号 + n」不是转义**，会被原样写进文件。我就这样把两条
> `#include` 写成了一行带字面量的垃圾行 —— 编译错误指向几十行外
> （「ToolDispatcher 未定义基类」），极难归因。
> 安全写法：here-string（'@ ... @'）或双引号里的转义换行，替换后 grep 一遍确认。


> 本文档记录 **C++ 重写过程中实测发现的、Python 参考实现自身的问题**。
>
> 建立原因：移植的第一原则是**行为等价**，所以发现的 Python 缺陷不能顺手"修掉"——
> 那会让 C++ 版与 `tests/golden/` 基线、与现有用户的实际体验全部脱节。
> 正确做法是 **忠实复现 + 明确登记 + 用测试钉死**，等所有模块完成等价性验收后，
> 再作为一次独立的"契约修正"统一处理，并同步更新 golden 基线。
>
> 状态：持续更新。每发现一条，追加一节，并在总览表登记。

>
> 最后更新：2026-11-18

---

## 0. 总览

| 编号 | 问题 | 严重度 | 影响面 | C++ 处置 |
| --- | --- | --- | --- | --- |
| **P-1** | `ip_generated_hdl()` 把 `.vhd` 混进 Verilog 文件列表 | 🔴 **高** | 所有含 IP 的工程 → **elaborate 诊断整体失效** | 忠实复现（测试锚点 40 条 / 0 warning） |
| **P-2** | `td > ` 全局替换吃掉输出中的字面量 | 🟡 中 | 输出含 `td > ` 的少数场景 | 忠实复现（钉死测试） |
| **P-3** | `fpgalsp.info` 返回非确定性实时状态位 | 🟡 中 | **等价性验收**（会反复假红） | golden 归一化丢弃 + 登记为缺陷 |
| **P-4** | `.fpgalsp/` 与日志写入工程目录 | 🟡 中 | 只读工程被污染；采集污染用户工程 | 待改为可配置（后续任务） |
| **P-5** | 进程终止时不清理 TD 子进程 | 🟡 中 | 孤儿进程累积、文件句柄无法释放 | C++ 用 Job Object 已解决 |
| **P-6** | `read_hdl` 后 TD 关闭 stdout 管道 | ⚪ 约束 | 会话不能复用 | 已作为硬约束实现 |
| **P-7a** | `.al` 的 `Path` 是**读侧**回退源：TD 找不到文件时静默回退原路径 | ⚪ 约束 | 沙箱复制不能完全隔离**读取** | `resolved_via` 三态（必需语义，51 单测） |
| **P-7b** | `.al` 的 `Path` 决定 TD 的**工程根**，`*_Runs` / `.fpgalsp` 会写到那里 | 🟡 中 | **复制工程再跑 TD 会写进原工程**（可能写坏用户数据） | 复制后改写 `Path`（`flow::RewriteProjectPath`）+ 语料干净断言 |
| **P-8** | `fpgalsp.check` 把 `td_running` 置 False → 可并发起第二条 TD 检查线程 | 🔴 **高** | 诊断表乱序覆盖 / 管道交错 | **不复现**（数据损坏级，见原则 1 例外） |
| **P-9** | `hdl.py` 的 tokenizer 接受**非 ASCII 标识符** | 🟡 中 | 含中文模块名的文件两版结果不同 | 跟随 Verilog 规范（Verible 拒绝），差异已钉死 |
| **P-16** | ADC 补全推荐 `set_ippin_assignment`，但校验白名单不含它 | 🟡 中 | 用户照补全写 → 立刻报 ADC-CMD | 忠实复现（`AdcIoStandards.OnlySetPinAssignmentIsKnown`） |
| **P-17** | `device` 非空但引脚表为空时，每条约束都报 USR-8026 | 🟡 中 | 引脚表部分加载 → 全量误报 | 忠实复现（`AdcDiagnostics.DeviceWithEmptyPinTableFlagsEveryPin`） |
| **P-18** | `KNOWN_PULLTYPES` 定义了却从不校验 | 🟡 中 | PULLTYPE 拼错静默通过到 bitgen | 忠实复现（`AdcDiagnostics.PulltypeValueIsNeverValidated`） |
| **P-19** | `create_clock -period nan/inf` 通过合法性检查 | 🟡 中 | 荒谬周期静默通过，SDC-NOCLOCK 也被满足 | 忠实复现（`SdcDiagnostics.PeriodNaNIsSilentlyAccepted`） |
| **P-20** | `sdc.py` 完全不校验时钟名 | 🟡 中 | `-clock bogus` 静默通过 | Diagnostics 等价 + 额外 `UndefinedClockDiagnostics`（`SdcUndefinedClocks.*`） |
| **P-21** | `coverage(entries, ports, top)` 的 `top` 参数从未使用 | ⚪ 约束 | 无影响，但易被误解为「按顶层过滤」 | 保留参数并注明（`AdcCoverage.TopParameterIsIgnoredLikePython`） |
| **P-22** | MCP `fpga_project_list` 静默截断到 100 条且无 `truncated` 标志 | 🟡 中 | AI agent 无法区分「只有 1 个工程」与「被截断」 | 忠实复现（`BridgeTest.ProjectListTruncatesSilentlyWithoutFlag`） |
| **P-23** | 同一个 bridge 里两种行号约定并存（0-based 与 1-based） | 🟡 中 | 移植时「顺手 +1」统一 → 与 Python 不一致 | 两种约定分别断言（WorkspaceSymbols / SearchHierarchy 测试） |

### ⚠ 重复编号对照（**引用时必须带标题**）

并行开发时两个组各自从同一个号往后编，导致下列编号各出现两次。
按编辑约定第 2 条：编号**不重排**（重排要改散落在多个 agent 报告/代码注释/测试名里的引用，
成本与出错概率远高于收益），改用本表消歧。

| 编号 | 标题（引用时必须带上它） | 归属 |
| --- | --- | --- |
| P-9 | `artifacts.syn_db` 从未被填充 | 阶段 4-γ（reports） |
| P-9 | `hdl.py` 的 tokenizer 接受非 ASCII 标识符 | 阶段 3（hdl-index） |
| P-16 | 取消路径的异常/线程收尾缺陷：Worker 提前 return 漏 join → 进程 abort | 阶段 4-δ（flow） |
| P-16 | ADC 补全推荐 `set_ippin_assignment`，但校验白名单不含它 | 阶段 5-α（constraints） |
| P-17 | `letters_of()` 只取开头连续字母，「同族判定」比直觉更宽 | 阶段 4-δ（flow） |
| P-17 | `device` 非空但引脚表为空时，每条约束都报 USR-8026 | 阶段 5-α（constraints） |
| P-18 | PRJ-8806 容错只认含 `ERROR` 的行，`CRITICAL-WARNING` 不算 | 阶段 4-δ（flow） |
| P-18 | `KNOWN_PULLTYPES` 定义了却从不校验 | 阶段 5-α（constraints） |

### 阶段 4-γ（reports）与 4-δ（flow）的条目索引

详表在各章（4-γ 见「P-9 ~ P-15」，4-δ 见附录 B）。

| 编号 | 问题 | 归属 |
| --- | --- | --- |
| P-9 | `artifacts.syn_db` 从未被填充（协议 §2 要求）| 4-γ reports |
| P-10 | Timing `statistics["min"]` 永远缺失（正则要求 `Min : SWNS`，实际是 `HWNS`）| 4-γ reports |
| P-11 | `#le` 只进 `io` 不进 `totals` → PH1 层次表 `le` 百分比恒为 null；`#lut&reg` 整行被丢 | 4-γ reports |
| P-12 | QoR 解析不区分表格 → Rent 表被当成指标 | 4-γ reports |
| P-13 | `collect()` 输出混合路径分隔符（`Y:/a/b\c\d`）| 4-γ reports |
| P-14 | `_parse_area_hierarchy` 的 `len(cells) < 7` 丢掉 5 列 gates/macros 表 | 4-γ reports |
| P-15 | 无 HDL 索引时 `begin_at`/`end_at` 是 `null` 而不是带 `reason` 的降级对象 | 4-γ reports |
| P-16 | 取消路径：Worker 提前 return 漏 join → 进程 `0xC0000409` | 4-δ flow |
| P-17 | `letters_of()` 只取开头连续字母 | 4-δ flow |
| P-18 | PRJ-8806 容错不认 `CRITICAL-WARNING` | 4-δ flow |

图例：🔴 影响功能正确性 / 🟡 影响可测性或工程卫生 / ⚪ 不是 bug，是必须遵守的行为约束

---

## P-1 🔴 IP 生成 HDL 把 `.vhd` 混进 Verilog 文件列表

**位置**：`td.py:604 ip_generated_hdl()` + `run_read_hdl()`

```python
def ip_generated_hdl(project) -> list:
    """IP cores are registered as .ipc in the .al; TD's non-project flow needs the generated HDL.
    Look for a same-stem .v/.vhd next to each .ipc (that is what the IP generator writes)."""
    out = []
    for entry in project.ip_files():
        stem = os.path.splitext(entry.path)[0]
        for ext in (".v", ".sv", ".vhd", ".vhdl"):     # ← 全部扩展名都收
            candidate = stem + ext
            if os.path.isfile(candidate) and " " not in candidate:
                out.append(candidate)
    return out

def run_read_hdl(...):
    files = [f.path for f in project.hdl_files()]
    files = files + ip_generated_hdl(project)          # ← 合并成一个扁平列表
    tcl = "read_hdl -file {%s} -top %s" % (...)        # ← 统一用 -file（Verilog）
```

**问题**：`read_hdl -file` 按 **Verilog** 编译。当 IP 同时生成 `.v` 和 `.vhd`
（本机 `C:/work/CorpusAlpha/al_ip/pll_gen` 正是如此），`.vhd` 会被当 Verilog 喂进去。

**实测后果**（TD 2026.2）：

```
HDL-8007 ERROR: 'ef2_components' is not compiled in library 'ef2_macro' in <pll_gen.vhd>
HDL-8007 ERROR: ignore unit pll_gen due to previous errors in <pll_gen.v>
```

`elaborate` 阶段**整体失败**，于是：

| 现象 | 原因 |
| --- | --- |
| 20 条 `elaborate module ...` **全部消失** | 没有 elaborate 就没有这些行 |
| `HDL-7144 instantiate unknown module pll_gen` **消失** | pll_gen 变成"存在但编译失败"，不再是未知模块 |
| `HDL-8007 pll_gen is a black box` **消失** | 同上 |
| 只剩 40 条全 `HDL-1007` Info、**零 warning** | 真正的诊断信号全部丢失 |

**为什么这条最严重**：它让用户看到"这个工程完全没问题"（0 warning），
而实际上 elaborate 根本没跑。**假绿灯比报错危险**。

**对照验证**（判定"谁对"的关键证据）：

| 输入 | Python | C++ |
| --- | --- | --- |
| 只含 20 个工程 HDL（不含 IP 生成 HDL） | 22 条，**2 个 warning** | 22 条，2 个 warning ✅ |
| 含 IP 生成 HDL（`.v` + `.vhd`） | **40 条，0 warning** | **40 条，0 warning** ✅ |

两种输入下两边都逐字段一致 → C++ 没有引入偏差，40 条纯属 Python bug 的产物。

**C++ 处置**：忠实复现。
- 测试锚点：`tests/td/td_diagnostics_integration_test.cpp :: RunReadHdlProducesDiagnostics`
  断言 **40 条、0 warning**，注释里写明完整机理
- 修掉这个 bug 时，该测试会变红并提示需要更新

**修复方向**（未实施）：
1. Verilog 与 VHDL 分开传：Verilog 用 `-file`，VHDL 用 `-vhdl`
2. 或 `ip_generated_hdl` 只取与 IP 实际语言匹配的扩展名（`.ipc` 里可能记录了语言）
3. 修好后该工程应恢复为 22 条含 2 个 warning

---

## P-2 🟡 `td > ` 全局替换吃掉输出中的字面量

**位置**：`td.py:237`

```python
return text.replace("td > ", "")     # ← 全局替换
```

若 TD 的输出内容本身含 `td > ` 字面（例如 HDL 注释里写了 `td > `、或路径中含该串），
会被**静默删除**。

**C++ 处置**：忠实复现（等价优先）。
- 钉死测试：`TdOutputKnownDefect.GlobalPromptReplaceEatsLiteralOccurrences`
- 期望值故意写成"被吃掉后"的结果（`a b`），并注明"若将来要修，此处会变红"

**为什么保留**：这是唯一能证明"我们知道它会出问题、且有意保持一致"的方式。
只复现不记录，将来没人知道这是缺陷；修掉了不记录，等价性会被破坏。

---

## P-3 🟡 `fpgalsp.info` 返回非确定性实时状态位

**位置**：`server.py:2098-2100`

```python
"device_loaded": bool(device),
"td_session": bool(self.session and self.session.alive()),
```

**问题**：这两个字段是"TD 子进程此刻是否就绪"的**实时状态**，
取决于 `fpgalsp.check` 与 `fpgalsp.info` 两次调用之间的竞态，**与输入无关**。

**实测证据**（golden 采集，两次完全相同的全量采集）：

```
case:CorpusAlpha_self_pwm_gen/result/info/td_session   true -> false
```

两批快照唯一差异就是它。**诊断内容本身两次逐字节相同**。

**影响**：如果不处理，任何基于快照的等价性验收都会被它反复打红，
而每次红灯都是假阳性——**假阳性会让人习惯性忽略红灯，验收就废了**。

**C++ 处置**：
- `tests/golden/tools/normalize.py` 的 `GOLDEN_VOLATILE_KEYS` 丢弃这两个 key
- README 里明确写成"**被掩盖的参考实现缺陷**"，不是"正常的 volatile 字段"

**建议的正式修复**（属协议层，不是 C++ 能单方面解决的）：

```
把布尔实时状态改成状态机 + 单调 generation：
  { state: "starting" | "ready" | "failed", generation: <int> }
```

这样：
- 客户端可以判断"是否就绪"，而不是猜
- 同一次会话内 `generation` 单调递增，可做乐观并发控制
- 快照可比（`generation` 在同类操作序列下确定）

---

## P-4 🟡 `.fpgalsp/` 与日志写入工程目录

**位置**（多处，全部写死在 `project.dir` 下）：

```
analysis.py:41       os.path.join(project.dir, ".fpgalsp", "cache")
flow.py:191          os.path.join(project.dir, ".fpgalsp", "flow", run_id)
flow.py:205          os.path.join(project.dir, ".fpgalsp", "cache")
mcp_bridge.py:887    os.path.join(proj.dir, ".fpgalsp", "cache")
mcp_bridge.py:1296   os.path.join(proj.dir, ".fpgalsp", "cache")
__main__.py:23       os.path.join(os.getcwd(), ".fpgalsp", "logs")
```

**问题**：所有缓存、flow 日志、LSP 服务器日志都写进**工程目录**，不可配置。

**实际造成的麻烦**（本项目实证）：

1. **golden 采集污染用户真实工程**：第一次采集直接往
   三个测试工程（`C:/work` 下的三个样例工程）的 `.fpgalsp/` 写文件，
   必须手工清理
2. **只读工程无法使用**：网络盘、版本控制只读检出、CI 里的挂载镜像都会失败
3. **`.al` 同级目录被污染**，用户会误以为工程被改动

**处置**：
- golden 采集已改为**先复制工程到 `tests/golden/_sandbox/` 再跑诊断**（已实测有效：
  TD 的 `.fpgalsp/run` 落在沙箱内，真实工程未被写入）
- C++ 版**应提供配置项**（如 `fpgaLsp.cacheDir`），默认仍在工程目录（保持兼容），
  但允许指向 `%LOCALAPPDATA%` 或临时目录

**注意**：这不是严格意义的 bug，而是一个**有代价的设计选择**。
但它的代价在只读场景和自动化场景下是致命的，建议正式修复。

---

## P-5 🟡 进程终止时不清理 TD 子进程

**实测**：取消一次 golden 采集后：

```
td_commands_prompt.exe   PID 45276   StartTime 16:44:39   ← 存活
```

它**持有 `.fpgalsp/run/td_*.log` 的文件句柄**，导致目录无法删除：

```
Remove-Item: The process cannot access the file
  '...\td_20261118_164439.log' because it is being used by another process.
```

**后果**：
- 孤儿进程累积（每次异常退出留一个）
- 文件句柄泄漏，缓存目录清不掉
- 下次启动可能因为文件被占用而失败

**C++ 处置**：已用 **Windows Job Object** 解决——父进程退出/被杀时整棵进程树一起终止。
- 验收断言：`tests/e2e/lsp_smoke.py` 必须在 `exit` 后检查 **0 个残留 `td_commands_prompt.exe`**

**建议 Python 版修复**：`subprocess.Popen(..., creationflags=CREATE_NEW_PROCESS_GROUP)` +
Job Object，或退出时 `taskkill /T /F /PID <pid>`。

---

## P-6 ⚪ TD 行为约束：`read_hdl` 会关闭 stdout 管道

**不是 Python 的 bug，是必须遵守的 TD 行为。**

实测：执行 `read_hdl` 后，TD **关闭 stdout 管道**——
收到 `__B` 标记但**收不到 `__E`**，进程仍存活，后续写入报 `Errno 22`。

`td.py` 的真实约束是：**`read_hdl` 必须是会话的最后一条命令。**

Python 版三处都遵守了这一点：
- `server.py:856` 每次新建会话
- `flow.py` 用 `with` 上下文
- 从不复用同一会话做 `read_hdl` + 后续命令

**C++ 处置**：作为 `td_session.h` 的 `Run()` 契约，写成显式注释块；
`PYTHON-PARITY.md §5.2` 单独立节，注明"将来想优化成复用会话，**做不到**，先读这节"。

---

## P-7a ⚪ `.al` 的 `Path` 是读侧回退源；TD 找不到文件时静默回退原路径

**不是 Python 的 bug，是 TD 的工程语义，但影响我们的测试方案。**

事实（读侧）：
- TD 在工程目录里找不到某个文件时，会**静默回退到 `.al` 记录的 `Path` 下的相对路径**
- 终端提示是 `PRJ-6619`：
  "Project was created at <原路径> but the current directory is <当前路径>"

**对本项目的两个影响**：

1. **`resolved_via` 三态是必需语义**，不能简化成 `path_join(dir, rel)`。
   `al_project` 已实现 `resolved_via ∈ {project, declared, absolute}`（51 单测覆盖）。
   任何人想"简化"这段代码，都会破坏"工程被拷走后仍能正确定位"的能力。

2. **⚠ 沙箱复制不能完全隔离读取**：
   把工程复制到 `tests/golden/_sandbox/` 后，TD 读取源文件时**仍可能回退到原工程的路径**。
   - 结论：golden 快照反映的仍是"原工程内容"，这在语义上可接受（内容相同），
     但**如果原工程内容变了，快照会静默改变**——所以快照必须记录源工程的哈希

## P-7b 🟡 `.al` 的 `Path` 决定 TD 的**工程根**，产物会写到那里（不只是"读回退"）

**位置**：`.al` 根节点 `<Project Path="...">`（**写侧**语义，2026-11-18 新增）

**实测**（阶段 4-δ，`tests/gate-corpus/corpus_alpha` 副本上跑 flow）：
把工程复制到临时目录后 `open_project`，TD **仍然把 `*_Runs` / `.fpgalsp/` 写到 `Path`
指向的目录**，而不是副本目录。

现成语料里就有一个活的例子：

```xml
tests/gate-corpus/corpus_alpha/demo_project.al
<Project Version="3" Minor="2" Path="C:/work/CorpusAlpha">
```

这个 `Path` 指向**另一个真实工程**。于是在副本里跑 flow 会：

1. 把 `*_Runs` 写到 `C:/work/CorpusAlpha`（**写坏用户工程**）；
2. 让"按副本目录找产物"的断言全部假失败（产物在别处）。

2026-11-18 真的差点发生：`td.flow` 的 area 断言因为找不到产物而红了，
核查 `C:/work/CorpusAlpha` 后发现最新文件时间仍是 11/9、没有实际写入 —— 侥幸而已。

**影响**：任何"复制工程再跑 TD"的隔离方案**不能只复制，必须改写 `Path`**。

**C++ 处置**（通用三步，已落到代码里）：
1. 复制时**排除** `*_Runs` 与 `.fpgalsp/`（被取消的 run 里有 `.stop.f`，
   TD 每一步都检查它，`reset_runs` 行为不确定）；
2. 复制后**改写 `.al` 根节点的 `Path`** 为副本目录；
3. 跑完**断言原语料目录**没有新增 `*_Runs` / `.fpgalsp`。

参考实现：
- `src/flow/flow_logic.cpp::RewriteProjectPath()`（公用工具）
- `src/flow/strategy.cpp::CompareStrategies()`（策略对比必须复用，否则
  `phy_cmp_N` 会写回用户工程 —— 那样"在副本上跑"就完全失去意义）
- `tests/td/flow_integration_test.cpp`：`Prepared` 里改写 + `SharedCorpusStaysClean` 用例

**（2026-11-18 由 reports-parse 修订：原文把 `Path` 描述成"只是元数据、不决定读哪个文件"，
不完整 —— 它在写侧决定 TD 的工程根。 ）**

---
## P-8 🔴 `fpgalsp.check` 清掉 `td_running` 标志 → 可并发起第二条 TD 检查线程

**位置**：`server.py:2077-2085`

```python
        if command == "fpgalsp.check":
            project = self._project(self._root_path())
            ...
            self.td_running = False                     # ← 不加锁地清掉"有人在跑"标志
            self._schedule_td_check(project, why="command")
            return {"ok": True, "project": project.name, "queued": True}
```

`_schedule_td_check` 的并发保护全靠 `td_running` 这一个布尔量：

```python
        with self.state_lock:
            if self.td_running:
                self._td_pending[project.al_path] = why   # 只排队一次
                return
            self.td_running = True
        thread = threading.Thread(target=self._td_check_worker, ...).start()
```

**问题**：`fpgalsp.check` 在**锁外**把 `td_running` 强制改成 `False`。若此刻正好有一个
（save 触发的）检查在跑，`_schedule_td_check` 会认为空闲，于是**再起一个线程**。

**实测后果**（推演自代码路径，非臆测）：

1. 两个线程同时对**同一个** `TdSession` 调 `run()`。`TdSession.run()` 会写同一个 stdin 管道、
   读同一个 stdout 管道，没有会话级互斥 → 两路 Tcl 输出在同一个管道里交错，
   `__B__/__E__` 哨兵会互相串台 → 诊断内容错乱，或一方超时 `kill` 掉另一方正在用的进程。
2. 两个线程都会执行 `self.td_diags[project.al_path] = table`：**后完成的覆盖先完成的**，
   而"先完成的"可能是**较新**的那一轮修订 → Problems 显示的是旧结果。
3. `_td_pending` 的"只排队一次"语义失效（原意是"当前这轮结束后再跑一轮"）。
4. 第一个线程的 `finally` 里 `self.td_running = False`，把第二个线程的存在抹掉 →
   后续 save 可能起第三个。

**严重度**：🔴 高。它不是"显示不准"而是**并发写同一个进程管道 + 诊断表**，
最坏情况是 TD 进程被另一条线程 `kill` 掉，后续检查全部失败。

**C++ 处置：不复现。**

> **这是本项目唯一一条「刻意不复现」的缺陷。**
> 理由：复现会导致两条 TD 会话并发执行 `read_hdl`，而 `read_hdl` 会关闭 stdout 管道（见 P-6），
> 并发时故障不可预测（可能死锁、可能互相读错输出）。
> 该缺陷属于"复现成本高于修复成本"的例外，因此 C++ 版加互斥保护，
> 并用 e2e 断言日志条数 ≤2 来证明没有并发。

这同时落在 README「移植铁律」原则 1 的**例外条款**（"缺陷会导致数据损坏或无法安全测试时才修"）：

* `commands.cpp` 的 `fpgalsp.check` **直接**调用 `ScheduleTdCheck`，不再预置 `td_running`
* `td_running_` / `td_pending_` 全部在 `state_mutex_` 保护下读写
* 同时最多一个 TD 线程；期间的触发只置 `td_pending_`，当前轮结束后**再跑一轮**
* 每次检查新建会话、结束立即 `Kill()`，不存在"两个线程共用一个会话"的可能

行为契约对客户端**完全不变**：响应仍是 `{ok:true, queued:true}`，
只是"并发时不再起第二条线程"。

**钉死测试锚点**：`tests/e2e/lsp_smoke.py` 连续两次 `fpgalsp.check`，
断言两次都返回 `ok:true`，且 `TD check complete` 日志**不超过 2 次**
（= 1 次运行 + 至多 1 次排队重跑）。若将来有人把 `td_running = False` 那行"忠实"搬过来，
这个计数会变成 3 次或出现诊断错乱而变红。

---

## P-9 🟡 `hdl.py` 的 tokenizer 接受非 ASCII 标识符（Verilog 规范不允许）

**位置**：`hdl.py:286`（`_tokenize` 的标识符分支）

```python
        # -- identifier -------------------------------------------------------
        if ch.isalpha() or ch == "_" or ch == "$":     # ← str.isalpha() 对中文返回 True
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "_$"):
```

`str.isalpha()` / `str.isalnum()` 对 Unicode 字母返回 `True`，所以 `module 中文模块(...)`
会被 Python 版当成合法模块定义。

**为什么是缺陷**：IEEE 1364 / 1800 的 identifier 只允许
`[A-Za-z_][A-Za-z0-9_$]*`（以及 `\` 开头的 escaped identifier）。非 ASCII 标识符
在所有主流工具（含 TD 的 Verilog 前端）里都是**语法错误**。

**实测后果**：含中文标识符的文件，Python 版索引出模块、C++ 版索引不出 →
**同一个文件在编辑器里"大纲有空、层次树没有"**，属于会让用户困惑的不一致。

**C++ 处置：跟随规范（不复现）。**

token 来自 Verible，它的 lexer 按规范拒绝非 ASCII 标识符；我们如实把错误记进
`parse_errors`，并保证 never-raise（不崩、不误判成别的符号）。

**钉死测试锚点**：`tests/unit/hdl_index_test.cpp`
* `HdlUtf16.NonAsciiIdentifierIsNotAcceptedAsVerilog` —— 断言不产生模块**且不抛异常**
* `HdlUtf16.ColumnsAreUtf16NotBytes` —— 中文**注释/字符串**里的 UTF-16 列换算必须正确
  （这是真正需要中文支持的地方：注释可以有中文，标识符不行）

> 相关但不算缺陷的两条**有意差异**（都只会更准）：
> 1. Python 的 tokenizer 会保留 `ifdef 的**两个分支**，同一模块可能被统计两次；
>    Verible 用 `filter_branches=true` 只留激活分支。
> 2. Python 把宏调用当普通标识符；Verible 在 filtered 流里已展开。

---

## 附：对 C++ 重写的三条处置原则

这三条是从上面 9 个问题里提炼出来的，适用于所有后续模块：

### 原则 1：发现缺陷 → 忠实复现 + 登记 + 钉死测试，不顺手修

理由：等价性是阶段 2-5 的验收标准。顺手修会让 C++ 与 golden 基线、与用户现有体验脱节。

例外：缺陷会导致**数据损坏**或**无法安全测试**时，才考虑修（并同步基线）。

### 原则 2：新增的行为必须"显式失败"，不能"隐式正确"

例：
- 占位命令返回 `{ok:false, error:"not implemented"}`，而不是静默不返回
- 生成器遇到歧义输入 `FATAL_ERROR`，而不是猜一个合理值
- 配置缺失时报错给出正确命令，而不是猜一个默认路径

### 原则 3：每一项"MVP 简化"都要标注为**有意的**，并写明恢复条件

例：`fpgaLsp/moduleTree` 暂时用文件级正则扫模块名，必须写
`TODO(hdl-index): 用 Verible CST 替换`，而不是留一个看起来完整的实现。

否则将来审计时，无法区分"有意留待后续"和"漏掉了"。

---

## 复现与验证

```bash
# P-1 复现（40 条 0 warning）
ctest -R td.diagnostics --output-on-failure

# P-2 复现（钉死测试）
ctest -R unit.td_output --output-on-failure

# P-3 复现（两次采集对比）
python tests/golden/tools/collect.py --only project
python tests/golden/tools/collect.py --only project --out /tmp/run2
python tests/golden/tools/compare.py <snapshots> /tmp/run2

# P-5 验证（exit 后无残留）
python tests/e2e/lsp_smoke.py     # 末尾断言 0 个 td_commands_prompt.exe
Get-Process td_commands_prompt    # 应为空

# P-8 验证（连续两次 check 不叠加线程）
ctest -R e2e.lsp_smoke --output-on-failure   # 断言 "TD check complete" 日志 ≤ 2 次
```
---

# 附录 A：阶段 4-γ（TD 报告解析）新增登记

> 登记人：reports-parse。全部通过 `python tests/compare_reports.py` 逐字段对比实测得出。
> 夹具：`tests/fixtures/reports/`（EF2/EF2L45LG144B + EG4X20BG256 两个器件族的真实报告）。

## P-9 🟡 `artifacts.syn_db` 从未被填充

**位置**：`reports.py:504-507 collect()`

```python
export["artifacts"].update({
    "bit": _stat(bit_path) if bit_path else {"path": "", "exists": False},
    "route_db": _stat(db_path) if db_path else {"path": "", "exists": False},
})
```

**问题**：协议 §2 明写 `artifacts` 里有 `bit` / `syn_db` / `route_db` 三项，
但 `collect()` 只填了 `bit` 和 `route_db`，`syn_db` 从头到尾没有出现。
`db_path` 本身还会"回落"——`_glob(phy, .*_pr\.db)` 找不到时退到 `_glob(syn, .*_gate\.db)`，
于是"布线 db"字段里可能装的是综合 db，两者语义混淆。

**实测后果**：`fpgaLsp/reports` 的 `artifacts.syn_db` 在前端永远是 `undefined`。

**C++ 处置**：**补齐** `artifacts.syn_db`（= syn 目录下的 `*_gate.db`），并保留 Python 的
`route_db` 回落逻辑不动（那是另一回事，见 P-13 说明）。
这是本任务里唯一一处"故意不忠实复现"，因为缺字段属于协议违约而不是行为选择。
测试锚点：`ReportsArtifacts.CollectArtifactsFromRealLayout` / `ReportsArtifacts.CollectArtifactsSynOnly`。

**修复方向**：Python 侧补一行
`"syn_db": _stat(_first_existing(_glob(syn, r".*_gate\.db")) or "")`。

## P-10 🟡 Timing 报告的 `Min` 统计行永远解析不到

**位置**：`reports.py:192-198 _parse_timing_groups()`

```python
stat = re.match(r"^(Max|Min)\s*:\s*SWNS\s+(-?[\d.]+)ns,\s*STNS\s+(-?[\d.]+)ns,"
                r"\s*(\d+)\s+Viol Endpoints,\s*(\d+)\s+Total Endpoints", stripped)
```

**问题**：正则要求 `Max`/`Min` 之后紧跟 `SWNS`。但 TD 的报告里
**Max 行打 SWNS/STNS，Min 行打 HWNS/HTNS**：

```
Max            : SWNS      0.171ns, STNS      0.000ns,         0 Viol Endpoints,     15840 Total Endpoints
Min            : HWNS      0.023ns, HTNS      0.000ns,         0 Viol Endpoints,     15840 Total Endpoints
```

所以 `statistics["min"]` 在所有真实报告里都**不存在**，只有 `statistics["max"]`。

**实测后果**：`eg4_final_timing.rpt` 解析出的 `groups[i].statistics` 只有 `max` 键；
前端如果想展示 hold 统计，拿到的是空。

**C++ 处置**：忠实复现——`TimingGroup::min_statistics` 保持 `nullopt`。
测试锚点：`ReportsTiming.MinStatisticsIsMissingDueToPythonPattern`（若 repairs.py 修掉，此测试会变红）。

**修复方向**：把两个模式拆开，Min 用 `HWNS|HTNS`：
`^(Max)\s*:\s*SWNS...` / `^(Min)\s*:\s*HWNS...`，输出字段名相应改成 `swns/stns` → `hwns/htns`。

## P-11 🟡 Area 的 `#le` 只进 `io` 不进 `totals` → PH1 层次表 `le` 百分比恒为 null

**位置**：`reports.py:18`（`UTIL_RE` 的字符类）+ `reports.py:424-431 parse_area()`

```python
UTIL_RE = re.compile(r"^#([A-Za-z][A-Za-z0-9_ *]*?)\s+(\d+)\s+out of\s+(\d+)\s+([\d.]+)\s*%")
...
m = LE_RE.match(stripped)          # ^#le\s+(\d+)\s*$
if m:
    io["le"] = int(m.group(1))     # ← 只写 io，不写 totals
```

**问题**：两个独立缺陷叠在一起。

1. `#le 10122`（PH1 的 `Utilization Statistics` 段里没有 `out of`）走的是 `LE_RE`，
   值被写进 `io["le"]`，`totals` 里**没有** `le` 键。于是 `_area_total(totals, "le")`
   返回 `None`，层次表里 `le` 列的百分比恒为 `null`。
   （实测 `eg4_phy.area` 的 `modules[0].utilization` 是
   `{"le": None, "lut": 37.79, "ripple": 1.4, "seq": 39.08, "bram": 1.56, "dsp": 0.0}`。）
2. `UTIL_RE` 的字符类 `[A-Za-z0-9_ *]` 不含 `&`，所以 PH1 的
   `#lut&reg  5146   out of  10122   50.84%` **整行被丢弃**（`totals` 里没有 `lut_reg`，
   `rows` 里也没有这一行）。

**实测后果**：PH1/EG4 工程里"逻辑单元总量占比"这一列是空的（`le` 是 PH1 的 slice 计数）。

**C++ 处置**：忠实复现两处。测试锚点：
`ReportsArea.Eg4HierarchyHasExtraLeColumn`（断言 `le` 的 utilization 为 null）。

**修复方向**：`#le` 同时写 `io` 与 `totals`（键 `le`）；字符类放宽到
`[A-Za-z0-9_ *&%+-]` 或改成"贪婪到第一个数字串之前"。

## P-12 🟡 QoR 解析不区分表格，Rent 表被当成指标收进 `metrics`

**位置**：`reports.py:44-59 parse_qor()`

```python
for raw in text.split("\n"):
    line = raw.strip()
    if not line.startswith("|"): continue
    cells = [c.strip() for c in line.strip("|").split("|")]
    key, value = cells[0], cells[1]
```

**问题**：任何以 `|` 起头、且第 2 个单元格非空的行都会被当成一条 QoR 指标。
`.qor` 里最后一张 `Hierarchical Rent Exponent Report` 表恰好也是 `|` 分隔：

```
+--------------------------------------------------------+
|Inst     |Model  |Rent     |Cell     |Pin     |Term     |
+--------------------------------------------------------+
|top      |top    |0.25     |1815     |6972    |25       |
+--------------------------------------------------------+
```

于是 `metrics` 里混进两条假指标：`{group:"Rent**", key:"Inst", value:"Model"}`
和 `{group:"Rent**", key:"top", value:"top"}`。

**实测后果**：`ef2_gate.qor` 的 metrics 从 26 条变成 28 条（实测 Python 输出 28 条）。
前端指标表里会多出两行 `Inst=Model`、`top=top`。

**C++ 处置**：忠实复现。测试锚点：`ReportsQor.RentPipeTableLeaksIntoMetrics`。

**修复方向**：遇到 `Report <stage> stage QoR done` 就停止解析；
或只接受 `|` 包围且上一行是 `---` 分隔线的表格。

## P-13 🟡 `collect()` 输出的路径是混合分隔符（`Y:/a/b\c\d`）

**位置**：`reports.py:442-506 collect()` + `_runs_dir()` / `_first_existing()` / `_glob()`

**问题**：`project.dir` 来自 `project.py::norm()`，是**正斜杠**；
而 `_runs_dir()` 用 `os.path.join(project.dir, ...)` 拼接，`os.sep` 在 Windows 上是 `\`：

```python
base = os.path.join(project.dir, "%s_Runs" % project.name)
# "C:/work/CorpusBeta" + "\\" + "CorpusBeta_Runs"
```

**实测后果**：`fpgaLsp/reports` 的 `files.*` / `artifacts.*.path` / `artifacts.runs[].dir`
全部是 `C:/work/CorpusBeta\CorpusBeta_Runs\phy_1\route.qor` 这种混合形态
（GitHub 上的协议文档 §2 示例写的是纯正斜杠，与实际输出不一致）。

**C++ 处置**：忠实复现——`reports::JoinPath` 按 `os.sep` 语义插入 `\`
（`src/reports/artifacts.h` 有说明）。测试锚点：
`ReportsArtifacts.JoinPathFollowsOsPathJoinSemantics`。

**修复方向**：Python 侧统一用 `posixpath.join` 或最后统一 `norm()` 一次。
注意：Windows 上两种分隔符等价，改动只影响 JSON 字符串外观，不影响功能。

## P-14 🟡 `_parse_area_hierarchy` 的 `len(cells) < 7` 丢掉 5 列的 gates/macros 表

**位置**：`reports.py:341-345 _parse_area_hierarchy()`

```python
if len(cells) < 7:
    continue
```

**问题**：EF2（CorpusAlpha/demo_project）**综合 RTL 阶段**的 `.area` 里层次表只有 5 列：

```
|Instance |Module |gates |seq |macros |
```

于是整张表被跳过，`modules` 为空。同一份文件也没有 `Utilization Statistics` 段
（只有 `Gate Statistics`），所以 `rows`/`totals` 也是空的 —— **整个 area 报告解析成
一个只有 `io` 的空壳**。

**实测后果**：`demo_project_rtl.area` → `rows=0, totals={}, modules=0, counts={}`，
只有 `io={"input":17,"output":15,"inout":0}` 有内容。

**C++ 处置**：忠实复现。测试锚点：`ReportsArea.Ef2RtlHierarchyTableIsSkipped`。

**修复方向**：列数下限应该按"至少 Instance + Module + 1 个资源列"判断（即 `< 3`），
并把 `gates/macros` 也纳入资源名；`Gate Statistics` / `LUT Statistics` 都应参与
`counts`（现在 `section` 只认 `IO|LUT|Utilization` 前缀，`Gate` 被漏掉）。

## P-15 🟡 无 HDL 索引时 `begin_at`/`end_at` 是 `null` 而不是带 `reason` 的降级对象

**位置**：`timing_map.py:58-69 resolve()` + `server.py:2003-2010 _decorate_reports()`

```python
def resolve(project, index, text):
    components = clean_point(text)
    if not components or index is None:
        return None          # ← index 缺失时直接给 None
```

**问题**：协议 v3 规定 `timing.paths[i].begin_at` 的形状是
`{signal, hierarchy, file, line, character, resolved, module, reason}`，
语义上"没解析出来"应当是 `resolved:false` + `reason`。
但 `index is None`（阶段 5 之前一直如此）时返回 `None`，客户端拿到 `null`
就无法区分"没有 HDL 索引"和"这个端点本来就没有"。

**实测后果**：`begin_end_resolved = {total: 128, resolved: 0}`，但每条 path 的
`begin_at` / `end_at` 都是 `null`，前端只能回退显示原始文本。

**C++ 处置**：**有意不同**。`AttachEndpointLocations` 在无索引时仍然算出
`signal` / `hierarchy`（纯文本可算），并返回
`{resolved:false, reason:"hdl-index-unavailable", file:null, line:null, ...}`。
这是 task-15 明确要求的行为，也是协议 v3 的字面语义。
测试锚点：`ReportsTiming.EndpointLocationsDegradeWhenHdlIndexMissing`。

**修复方向（阶段 5）**：接入 HDL 索引后按 `timing_map.resolve()` 逐字段对齐；
同时 Python 侧应把 `index is None` 分支改成返回降级对象而不是 `None`。

## 阶段 4-γ 的复现与验证

```powershell
# 逐字段对比（会自己造 <name>_Runs 树、驱动 C++ 导出 JSON、调 Python 参考实现）
python tests/compare_reports.py

# 单测（含"畸形/截断/二进制垃圾不崩"）
ctest --test-dir build-reports -R unit.reports --output-on-failure
```

## P-16 🟡 取消路径的异常/线程收尾缺陷：Worker 提前 return 漏 join -> 进程 abort

**位置**：`src/flow/flow_runner.cpp::Worker()`（移植时的实现缺陷，非 Python 缺陷）

**问题**：`heartbeat` 线程是 `Worker()` 的**局部** `std::thread`，而 `Worker()` 里有 7 处
提前 `return`（`import_device` 失败 / 取消 / `run` 失败 / …）。提前 return 会跳过函数末尾的
`heartbeat_thread.join()`，于是析构一个仍然 `joinable()` 的 `std::thread` ——
按标准直接 `std::terminate()`，Windows 上走 `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`，
**整个进程以 `0xC0000409` 崩掉**，而不是把 flow 标记为失败。

补充两个容易误判的点：

* **与 `/EHsc` 无关**：本工程所有 TU 都开着 `/EHsc`（`CMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc`）。
  早前构建日志里的 `C4530` 来自共享 `build/` 被裸 `cmake -S . -B build -DCMAKE_CXX_COMPILER=cl`
  污染过一次（`CMAKE_CXX_FLAGS` 被清空），那次之后已修好。`std::terminate` 在任何 EH 模式下都由 fastfail 结束进程。
* **另一条同类陷阱**：取消时另一个线程 `Kill()` 掉会话，阻塞中的 `td::TdSession::ReadUntil`
  会 `throw TdError`。若该异常逃出 worker 线程入口，同样 `std::terminate()`。

**实测后果**：`fpgalsp.runFlow` 之后取消（或任何走提前 return 的失败路径），server 进程
直接崩（退出码 `3221226505` = `0xC0000409`），日志最后一行恰好是 `flow cancelled`。

**C++ 处置**（两层都要，缺一不可）：
1. **RAII join**：`Worker()` 里 `JoinOnExit` 守卫，保证任何提前 return 都会 join 心跳线程；
2. **线程入口 catch-all**：worker 与 heartbeat 的线程函数体整体包 `try/catch(...)`，
   异常一律转成 `ok=false, error=...`（`SafeFinishError` 自身也 noexcept）。
3. `FlowOptions::cancel_kill_grace_seconds`（默认 10 s，先写 `.stop.f` 让 TD 自己收尾）
   仍然保留 —— 它减少"强杀正在阻塞的会话"的频率，但不能替代上面两条。

测试锚点：`tests/td/flow_integration_test.cpp::CancelLeavesNoOrphanProcesses`（真实 TD，
修好前会让 gtest 进程 abort）与 `FlowRunner.EarlyReturnWithHeartbeatDoesNotAbort` /
`FlowRunner.CancelWithHeartbeatDoesNotAbort`（单测，打开心跳走提前 return 路径）。

> **编号冲突说明**：本文档里 P-9 / P-16 / P-17 / P-18 各出现两次（本组成员并行追加时撞号）。
> 本节的 P-16/17/18 属于**阶段 4-δ（flow/烧录）**；constraints 组另有 P-16/17/18（ADC/SDC）。
> 按编辑约定第 2 条，编号不复用，**引用时请带标题**；合并时以总览表为准。

**教训（一般性）**：任何"后台线程 + 外部中断"的场景都要同时做到
「线程对象有 RAII join」+「线程入口有 catch-all」。缺一条就是一个进程级崩溃点。

## P-17 🟡 `letters_of()` 只取开头连续字母，"同族判定"比直觉更宽

**位置**：`program.py:143 letters_of()`

```python
def letters_of(part):
    match = re.match(r"^([A-Za-z]+)", part or "")
```

**问题**：`letters_of("EF2L45LG144B")` 得到的是 **`"EF"`** 而不是 `"EF2"`。
`compare_devices()` 用它判断"是否同一 family"，于是 `EF2L` 与 `EF2M` 算同族（可告警），
而 `EF2` 与 `EG4` 算不同族（硬拒绝）。

**实测后果**：没有行为问题，但移植时极易"按直觉"写成 `EF2`，从而把
`EF2L` vs `EF2M` 误判成不同族 —— 那样会拒绝掉一个本来只需要告警的场景。

**C++ 处置**：忠实复现（`flow::LettersOf`），测试
`FlowProgrammerParse.SeriesAndLetters` 写明了这条语义。

## P-18 🟡 PRJ-8806 容错只认含 `ERROR` 的行，`CRITICAL-WARNING` 不算

**位置**：`flow.py:460 _only_run_exit_errors()`

```python
errors = [line.strip() for line in out.replace("\r","\n").split("\n") if "ERROR" in line]
return bool(errors) and all(("PRJ-8806" in line or "PRJ-5303" in line) for line in errors)
```

**问题**：判定只看含 `ERROR` 子串的行。若 TD 打的是 `PRJ-8806 CRITICAL-WARNING`，
`errors` 为空 → `_only_run_exit_errors` 为 False → 即使产物齐全也判失败。
（`has_error()` 是认 `CRITICAL-WARNING` 的，两处标准不一致。）

**实测后果**：手上没有"只报 CRITICAL-WARNING 的 PRJ-8806"样本，属于潜在风险而非现网 bug。

**C++ 处置**：忠实复现（`flow::OnlyToleratedPrjErrors`），测试
`FlowLogicPrj.CriticalWarningIsNotTolerated` 钉住该行为。

**修复方向**：改成和 `has_error()` 同一套判定（`ERROR|CRITICAL-WARNING`），
但先要拿到真实样本确认 TD 会不会那么打。

## 阶段 4-δ 的复现与验证

```powershell
# 单测（90 例：flow_logic 35 / flow_runner 17 / flow_programmer 25 / flow_strategy 15）
ctest --test-dir build-flow -R "unit\.flow" --output-on-failure

# 真实 TD 集成（含取消 / 失败两条无孤儿断言）
ctest --test-dir build-flow -R "td\.flow" --output-on-failure
```

---

# 阶段 5-α / 5-β 的缺陷登记（constraints + MCP）

> 说明：本轮已把总览表的 P-16 ~ P-22 补登记完成。以下每条的节点在
> `src/constraints/` 与 `src/mcp/`，**全部是"忠实复现 + 测试钉死"**，
> 只有 P-20 是有意的"等价 + 独立新增 API"。
>
> ⚠ 本轮发生过一次共享文档重写（我的小节被覆盖过一次），所以这些条目集中放在文件末尾，
> 不改动上面各阶段的结构。将来若再合并，请以总览表为准逐条核对小节是否存在。

## P-16 🟡 ADC 补全推荐 `set_ippin_assignment`，但校验白名单不含它

**位置**：`adc.py:31`（`KNOWN_COMMANDS`）+ `server.py:1050`（`_complete_adc`）

```python
KNOWN_COMMANDS = frozenset({"set_pin_assignment"})            # 校验白名单
for value in ("set_pin_assignment", "set_ippin_assignment"):   # 补全候选
```

**问题**：补全列表与校验白名单不一致。用户接受补全写下的
`set_ippin_assignment` 会被同一份代码判成"未知命令"（`ADC-CMD`，severity 3）。
**工具教用户写一个它自己认为非法的命令。**

**C++ 处置**：忠实复现（白名单只含 `set_pin_assignment`）。
锚点：`tests/unit/adc_test.cpp :: AdcIoStandards.OnlySetPinAssignmentIsKnown`。

**修复方向**：先在 TD 里实测 `set_ippin_assignment` 的语义；支持就进白名单，
不支持就从补全删掉。**两者必须一致**。

## P-17 🟡 `device` 非空但引脚表为空时，每条约束都报 USR-8026

**位置**：`adc.py:674`（`elif device is not None:`）+ `_pin_maps()`

**问题**：只判断 `device is not None`，不判断引脚表是否真的加载成功；
`pins` 为空时每个 LOCATION 都解析成 `missing` → 一个 47 引脚的工程刷 **47 条 error**。
与 `device is None` 时"正确跳过检查"自相矛盾。

**C++ 处置**：忠实复现。锚点：
`tests/unit/adc_test.cpp :: AdcDiagnostics.DeviceWithEmptyPinTableFlagsEveryPin`（3 条约束 → 3 条 USR-8026）。

**修复方向**：`not pins and not dedicated` 时按 `device is None` 处理。

## P-18 🟡 `KNOWN_PULLTYPES` 定义了却从不校验

**位置**：`adc.py:116` + `diagnostics()`

**问题**：注释写"用于补全/校验"，但 `diagnostics()` 里没有任何 PULLTYPE 分支，
常量全库无人引用。`PULLTYPE = PULLUPX` 静默通过，直到 bitgen 才报错。
同类：`DRIVESTRENGTH` / `SLEWRATE` / `VREF` 等只校验**键**、从不校验**值**。

**C++ 处置**：忠实复现（不新增诊断），但导出 `adc::KnownPulltypes()` 供补全用。
锚点：`AdcDiagnostics.PulltypeValueIsNeverValidated`。

## P-19 🟡 `create_clock -period nan/inf` 通过合法性检查

**位置**：`sdc.py:893-906`

```python
if number is None or number <= 0:   # nan <= 0 是 False；inf <= 0 也是 False
```

**C++ 处置**：忠实复现。锚点：`SdcDiagnostics.PeriodNaNIsSilentlyAccepted`。
⚠ **实现陷阱（已踩过）**：C++ 必须写 `*number <= 0`；写成 `!(number > 0)` 会把 NaN
判成非法，与 Python 不一致——这是"看起来等价、边界不同"的典型，也是
`tests/compare_constraints.py` 存在的理由。

**修复方向**：`not math.isfinite(number) or number <= 0`。

## P-20 🟡 `sdc.py` 完全不校验时钟名

**位置**：`sdc.py:824-949`（`CONTRACT.md` 的诊断表也没有这一项）

**问题**：只校验 `get_ports` 的端口名；`-clock` / `-master_clock` / `get_clocks` 的名字从不校验。
`set_input_delay 1.0 -clock clk_typo ...` 静默通过。

**C++ 处置**：**等价 + 有意新增**（分离成两个 API，互不干扰等价性）：
`Diagnostics()` 与 Python 逐字段一致；`UndefinedClockDiagnostics()` 产出 `SDC-CLOCK`（severity 2），
且刻意保守——文件里一条 `create_clock`/`create_generated_clock` 都没有时**不判断**
（时钟可能来自 `derive_pll_clocks` 或被 source 的 sdc），通配符与 `[]` 一律跳过。
锚点：`tests/unit/sdc_test.cpp :: SdcUndefinedClocks.*`（5 例）。

## P-21 ⚪ `coverage(entries, ports, top)` 的 `top` 参数从未使用

**位置**：`adc.py:436`。不是缺陷，是死参数；但调用方传了 `project.top`，
读代码的人会以为覆盖率按顶层过滤。

**C++ 处置**：保留同名参数并在头文件注明。
锚点：`AdcCoverage.TopParameterIsIgnoredLikePython`（传不传 top 输出相同）。

## P-22 🟡 MCP `fpga_project_list` 静默截断到 100 条且无 `truncated` 标志

**位置**：`mcp_bridge.py:305-317`（`for al in found[:100]`）

**问题**：同一 bridge 的 `project_files` 是带 `truncated` 的；两个工具对"结果被截断"
处理不一致。agent 拿到 100 个工程时无法判断是"正好 100 个"还是"还有更多"。

**C++ 处置**：忠实复现。锚点：
`tests/unit/mcp_bridge_test.cpp :: BridgeTest.ProjectListTruncatesSilentlyWithoutFlag`
（断言 project_list **没有** truncated、project_files **有**）。

**修复方向**：`{"truncated": len(found) > 100, "total": len(found)}`，并统一所有 `[:N]` 工具。

---

## 阶段 5-α 的复现与验证（ADC / SDC）

```powershell
# 私有构建目录（共享 build/ 由 Lead 维护）
cmake --preset windows-msvc-release -B build-mcp
cmake --build build-mcp --target unit_adc unit_sdc

# 单测：adc 67 例 + sdc 52 例
ctest --test-dir build-mcp -R "unit\.(adc|sdc)$" --output-on-failure

# 与 Python 逐字段 diff：93 个结构（entries/coverage/diagnostics/commands，
# 含 18+17 个畸形输入用例）——结果 0 差异
$env:FPGA_LSP_ADC_DUMP = "$env:TEMP\adc_cpp.json"
$env:FPGA_LSP_SDC_DUMP = "$env:TEMP\sdc_cpp.json"
build-mcp\bin\unit_adc.exe --gtest_filter=AdcParity.*
build-mcp\bin\unit_sdc.exe --gtest_filter=SdcParity.*
python tests/compare_constraints.py
```

## 阶段 5-β 的复现与验证（MCP server）

```powershell
cmake --preset windows-msvc-release -B build-mcp
cmake --build build-mcp

# 1) schema 门禁（第一验收项）：39 个工具 × 2 种可见性，全字段一致
python tests/compare_mcp_schema.py --exe build-mcp\bin\fpgalsp-native.exe
ctest --test-dir build-mcp -L mcp

# 2) 单元测试：109 例（协议层 34 / 工具表 18 / bridge 57）
ctest --test-dir build-mcp -R "unit\.mcp" --output-on-failure

# 3) 端到端冒烟：mcp_smoke 28 项 + mcp_flow_smoke 8 项（后者只在临时副本上跑）
python tests/e2e/mcp_smoke.py --server build-mcp\bin\fpgalsp-native.exe
ctest --test-dir build-mcp -R e2e.mcp --output-on-failure
```

**34 个真实实现 / 5 个显式占位**（占位统一 `{ok:false, error:"not implemented: <缺什么>"}`）：

| 分组 | 数量 | 状态 |
| --- | --- | --- |
| 工程 / 报告 / 诊断 / 器件 / 约束 / 通用读写 / flow 日志 | 15 | ✅ 真实 |
| 符号类（documentSymbol、workspaceSymbol、definition、references、hover、moduleTree、module_info、duplicate_modules、search_hierarchy、symbol_path、call_hierarchy） | 11 | ✅ 真实 |
| flow 动作（run / cancel / strategy_compare） | 3 | ✅ 真实（`flow::FlowRunner` / `CompareStrategies`，与 LSP 同一实现） |
| cables（只读探测，`flow::Inspect`） | 1 | ✅ 真实 |
| 补全 / 实例化（completion / instantiation） | 2 | ✅ 真实（`features::Completion` + Python `_instantiation_text` 的纯文本格式） |
| 写工程（project_rename / project_add_source） | 2 | ✅ 真实（原子写 + 重解析验证 + 回滚） |
| analysis_run（TD DRC/CDC） | 1 | ⛔ C++ 里 `fpgalsp.runAnalysis` 本身未实现 |
| project_update（open_project -update） | 1 | ⛔ TD 工程版本同步未移植 |
| code_actions / rename_edits（编辑构造层） | 2 | ⛔ 需要编辑构造 API |
| format（Verible formatter 桥） | 1 | ⛔ 未接 |

**写工程两条的额外约定**（`project_rename` / `project_add_source` 会改用户文件）：
1. 先算新内容（纯字符串，不碰文件）→ 2. 同目录临时文件 + `rename` **原子替换** →
3. **重新解析 .al 验证**（只允许预期字段变化）→ 4. 验证失败用原文原子写回并返回 `{ok:false, error}`。
测试只在 `tempfile` 副本上跑（`TempProjectCopy`），**绝不碰 `tests/gate-corpus`**。

**flow 类工具只在临时副本上验证**：`tests/e2e/mcp_flow_smoke.py` 用
`tempfile.mkdtemp()` + `shutil.copytree(ignore=("*_Runs",".fpgalsp"))`，跑完断言原语料无新增。
本轮真实发生过两次污染（`.stop.f` 残留导致 td.flow 假失败、`.fpgalsp/compare` 来自
strategy_compare 单测），都由这条规矩防住 / 清理干净。

**符号类为什么分两个数据源**（不要"统一"掉）：

| 来源 | 工具 | 理由 |
| --- | --- | --- |
| `server::features::*`（LSP handler 与 MCP 共用的无状态层） | document_symbols / workspace_symbols / definition / references / hover / project_structure | 转换**只有一份实现**；features 不持有 transport，MCP 直接调它没有"LSP 通知写进 MCP stdout"的风险 |
| `hdl::HdlIndex` 直接查询 | module_info / duplicate_modules / search_hierarchy / symbol_path / call_hierarchy | C++ 里没有对应 feature API（Python 的 ide.py / nav.py 未上移），按 Python 算法移植；将来应上移到 features |

⚠ **位置约定不一致是这一批最大的坑**：`features`/`hdl` 输出 0-based，MCP 对外 1-based；
但 `duplicate_modules` 与 `search_hierarchy` 的 `line` 在 Python 里是**原样透传 0-based**，
那两处**必须保持 0-based**（"顺手 +1"就与 Python 不一致）。测试里已分别钉死。

**顶层端口来源**：优先 HDL 索引（`"portsSource": "hdl-index"`，与符号工具同一数据源）；
索引里没有 top 模块时才回落文本扫描（`"text-scan"`）。文本扫描必须**先把注释替换成空格**
（保留字节偏移）——`input SUPSTS, // driver enable output signal` 这类行会把注释里的
"output signal" 当成端口（真实踩到：25 个端口被扫成 26 个）。

## P-23 🟡 同一个 bridge 里两种行号约定并存（0-based 与 1-based）

**位置**：`mcp_bridge.py` 全文件（`_one_based_range` 与 `ide.duplicate_modules` / `ide.search_hierarchy`）

**问题**：MCP 对外约定是 **1-based**（`_one_based_range` 把 LSP 的 0-based 统一 +1），
但有两处是**原样透传 hdl 索引的 0-based 行号**：

```python
# ide.py:86 duplicate_modules -> module["line"] 是 0-based，直接进响应
duplicates.append({"name": name, "definitions": definitions})
# ide.py:104 search_hierarchy -> 同样是 0-based
add({"kind": "module", "name": name, "file": ..., "line": module.get("line", 0)})
```

也就是说 agent 拿到同一套工具时，`fpga_workspace_symbols.line` 是 1-based，
而 `fpga_duplicate_modules.definitions[i].line` 与 `fpga_search_hierarchy.matches[i].line`
是 0-based，**没有任何字段说明这件事**。

**实测后果**：agent 用 `search_hierarchy` 的 line 去请求 `fpga_read_file(offset=line)`
会整体错一行（read_file 的 offset 是 1-based）。这类偏差在 agent 侧表现为
「工具返回的位置不对」，很难归因到「两种约定并存」。

**C++ 处置**：忠实复现两种约定，并**分别**用测试钉死：
- `WorkspaceSymbolsFindsModulesByName`（1-based）
- `SearchHierarchyFindsModulesAndInstances`（断言 **0-based**，注释写明「不要顺手 +1」）
- `DuplicateModulesReturnsSortedGroups`（`line` 透传 0-based）

**修复方向**：统一成 1-based，并在两个工具的 description 里写明行号基准；
但这会改变 agent 已依赖的输出，属于契约变更 —— 需与 `MCP.md` 同步。

---
