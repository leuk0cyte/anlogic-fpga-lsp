# TD 层与 Python 参考实现的逐字段核对记录

本文档记录 `src/td/` 与 `%PYREF%\fpgalsp\td.py` 的等价性核对进度。

原则：**先核对再实现，核对过程本身留档**。任何一次回归都可以对着本文档逐条复查。

- 参考实现：`%PYREF%\fpgalsp\td.py`（757 行）
- 实测环境：TD Release_2026.2，`C:\Anlogic\TD_Release_2026.2.210.850`
- 实测工程：`C:/work/CorpusAlpha`（`demo_project.al`，20 个 HDL 文件，器件 EF2L45LG144B）
- 夹具：`tests/td/fixtures/`

图例：✅ 已核对一致　🔶 部分核对　⬜ 未开始　❗已知偏差（有意保留）

> **发现的 Python 参考实现缺陷统一登记在 [`tests/PYTHON-BUGS.md`](../PYTHON-BUGS.md)。**
> 本文档只记录「TD 层逐字段核对」的过程与结论；缺陷的完整清单、严重度、
> 修复方向与 C++ 处置方式见那份登记表（P-1 ~ P-7）。

---

## 1. `version_key` / 安装发现

| 项目 | 状态 | 核对方式 |
| --- | --- | --- |
| `version_key` 标准形态 | ✅ | 实跑 Python：`TD_Release_2026.2.210.850 → (2026,2,0,210,850)`，C++ 相同 |
| `version_key` SP 形态 | ✅ | 实跑 Python：`2026.1_SP2.200.067 → (2026,1,2,200,67)`，C++ 相同 |
| `version_key` 退化（全数字） | ✅ | `v12.3 → [12,3]` |
| tuple 比较语义（长度决胜） | ✅ | `[2026,2] < [2026,2,0]` |
| `_install_roots` 三个浅层基目录 | ✅ | 注入式 FakeFs 单测 |
| `_install_roots` 深层 `<drive>/<x>/Anlogic` | ✅ | 注入式单测 |
| 去重（大小写不敏感） | ✅ | 注入式单测 |
| `_normalise_root`（exe/bin/root 三种形态） | ✅ | 单测 |
| `_release_of` | ✅ | 单测 |
| `installs()` 排序（key 降序 + root） | ✅ | 单测 |
| `find_td` 优先级 explicit > env > auto > PATH | ✅ | 单测（含无效 env 跳过） |
| **本机真实自动发现** | ✅ | `unit_td_install` 的 `TdInstallReal.FindsThisMachineInstall` 实测通过 |
| ❗`find_td` PATH 回退的 root 计算 | ❗ | Python 用 `dirname(dirname(entry))`，entry 是 `.../bin` 时得到 `<release>` 的上一级（如 `C:/custom`），**看起来像 Python 的 bug**，但按"等价优先"原则保持一致，已在单测中标注 |

---

## 2. `clean_lines`

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| `\r\n` → `\n` | ✅ | |
| 残余 `\r` → `\n` | ✅ | |
| 每行 rstrip | ✅ | |
| 丢弃空行 | ✅ | |

---

## 3. `run()` 的后处理（两层分离）

实测 body（探针抓取，原样）：

```
'\r\n\r\ntd > \r\ntd > 42\r\n\r\ntd > '
```

| 层 | Python 行为 | C++ 行为 | 状态 |
| --- | --- | --- | --- |
| 第一层：截取 `__B..__`/`__E..__` + **全局** `replace("td > ","")` | `'\r\n\r\n\r\n42\r\n\r\n'` | 完全相同 | ✅ |
| 第二层：`clean_lines` | `['42']` | 完全相同 | ✅ |

**关键结论：`Run()` 返回的是"脏文本"（含原始 `\r\n` 与空行），清理是调用方的事。**
不能在第一层就把换行规整掉，否则下游 `parse_message_lines` 的输入形态就变了。

### 3.1 已知缺陷：全局替换 `td > `（有意保留）

`td.py:237` 是 `text.replace("td > ", "")` —— **全局**替换。若 TD 输出本身含 `td > ` 字面
（HDL 注释、路径），会被静默吃掉。

- 现状：C++ 保持一致（等价优先，不修正）
- 钉死测试：`TdOutputKnownDefect.GlobalPromptReplaceEatsLiteralOccurrences`
- 该测试期望值是 `a b`（内容被吃掉），并注明"若将来要修，此处会变红"

---

## 4. `parse_message_lines`（核心）

### 4.1 正则逐字复现

| 正则 | 状态 | 备注 |
| --- | --- | --- |
| `MSG_RE` | ✅ | 用 re2，不用 std::regex（TD 日志可达数 MB） |
| `IN_FILE_RE`（绝对路径） | ✅ | |
| `REL_IN_FILE_RE`（相对路径） | ✅ | |
| `IGNORED_FILE_RE` | ✅ | Python 有 `re.I`，C++ 用 `(?i)` 内联标志 |
| `ERROR_RE` / `FATAL_RE` | ✅ | |
| `SEVERITY` 映射表 | ✅ | ERROR=1, CRITICAL-WARNING=2, WARNING=2, INFO=3 |

### 4.2 真实语料核对（`tests/gate-corpus` + `C:/work/CorpusAlpha`）

read_hdl 在 20 文件工程上的**真实输出**：

```
HDL-1007 : analyze verilog file C:/work/CorpusAlpha/src/trans/trans.v
HDL-1007 : elaborate module top in C:/work/CorpusAlpha/src/top.v(15)
HDL-7144 CRITICAL-WARNING: instantiate unknown module pll_gen in C:/work/CorpusAlpha/src/top.v(66)
HDL-1008 : device name is not set, so there is no need to read *macro.v
```

Python `parse_message_lines` 输出（21 条，节选）：

| code | severity | message | path | line |
| --- | --- | --- | --- | --- |
| HDL-1007 | 3 (INFO) | elaborate module top | `C:/work/CorpusAlpha/src/top.v` | 15 |
| HDL-7144 | 2 (WARNING) | instantiate unknown module pll_gen | `C:/work/CorpusAlpha/src/top.v` | 66 |
| HDL-1007 | 3 (INFO) | elaborate module sys_rstn | `.../sys_rstn.v` | 1 |

| 规则 | 状态 |
| --- | --- |
| 无 severity + 有位置 → **INFO(3)** | ✅ **最关键**：若误判成 warning，20 文件工程会刷 20+ 条噪音 |
| 无 severity + 无位置 → **丢弃**（`analyze verilog file`、`RUN-1002 start command`） | ✅ |
| `CRITICAL-WARNING` → 2 | ✅ |
| `WARNING` → 2 | ✅ |
| 行尾 `in/at/from/of` 被剥离 | ✅ 实跑 Python 确认 `'bad syntax in' → 'bad syntax'` |
| `ignored due to errors` 合成 warning（line=1） | ✅ 需以 `<CODE>` 开头，否则 MSG_RE 直接不匹配 |

### 4.3 实测纠正的错误预期（记录在此，避免重犯）

| 我曾以为 | 实测真值 |
| --- | --- |
| `HasFatal("XUSR-8001 ERROR")` 应为 false | **true**。Python 的 `\b` 在 `-` 与数字之间成立；真正不匹配的是 `1USR-8001`（前面是数字） |
| 无 `<CODE>` 前缀的 `... ignored due to errors` 也会产生警告 | **不会**。`MSG_RE` 要求行首是 `<CODE>`，否则整行丢弃 |

---

## 5. TD 会话行为（实测）

| 事实 | 状态 | 验证方式 |
| --- | --- | --- |
| 交互式 Tcl shell，启动即打印横幅 | ✅ | `TdSessionTest.StartsAndReportsBanner` |
| 提示符 `td > ` 会**出现多次**且粘在输出里 | ✅ | 实测 body |
| 一条命令报错不截断后续命令 | ✅ | `ErrorInOneCommandDoesNotTruncateNext` |
| 执行 `exit` 正常退出 | ✅ | 实测 |
| 超时能中断并清理 | ✅ | `TimeoutKillsSessionAndCleansUp`（2s 超时 + 30s 命令） |
| Job Object 清理整棵进程树 | ✅ | `KillLeavesNoProcess` / `CloseLeavesNoProcess` |

### 5.1 重要：未知命令不产生 `<CODE> ERROR`

实测 `this_command_does_not_exist_xyz` 的输出：

```
'this_command_does_not_exist_xyz' is not recognized as an internal or external command,
operable program or batch file.
```

**没有 `<CODE> ERROR` 前缀 → `HasError()` 返回 false**（Python 版一致，不是 bug）。

**含义：不能靠"有没有 ERROR 标记"判断命令是否失败。** 要测错误路径必须用
TD 认识但会失败的命令，例如：

- `import_device no_such_db_xyz` → `USR-8001 ERROR: Invalid device_db name`
- `import_device ef2_1.db -package EF2L45LG144B` → `USR-8001 ERROR: device ef2_1 not support package ...`
- `query_current_package -fio`（未 import） → `USR-8001 ERROR: Unknown Device ...`

### 5.2 ⚠ TD 行为约束：read_hdl 会关闭 stdout 管道

**这不是我们的 bug，是 TD 的真实行为约束。**

实测现象（**Python 版完全相同**）：

```
read_hdl -file {...} -top top
  → 收到 __B 哨兵，但 __E 哨兵收不到
  → 进程仍存活（proc alive: True）
  → 下一条命令写入时 Errno 22 / Invalid argument
```

**为什么 Python 版每次都新建会话**（这曾经看起来像"没做优化"）：

| 位置 | 做法 |
| --- | --- |
| `server.py:856` | 每次新建 `TdSession` |
| `flow.py:199` | `with TdSession(...)`，每流程一个会话 |

**结论：`read_hdl` 必须是会话的最后一条命令。**
任何在同一会话里 `read_hdl` 之后继续发命令的代码路径都是 bug。
需要多次 `read_hdl` 时，必须每次新建会话。

> 将来若有人想"优化"成复用会话做多次 read_hdl —— **那做不到**，先读这一节。

**C++ 侧的落地**（`td_session.cpp :: ReadUntil`）：

| 情况 | 行为 |
| --- | --- |
| 管道断开 + **进程已退出** | 抛 `TdError("TD process exited unexpectedly")` |
| 管道断开 + **进程仍存活** | 返回已缓冲的内容，标记会话不可再写（不抛） |

这区分了「TD 真的挂了」和「TD 只是关了自己那侧的 stdout」。
约束同时写在 `td_session.h` 的 `Run()` 契约注释里。

### 5.3 测试间污染（已修复，值得记录）

首次 8 个 TD 测试一起跑 → 6 个失败；**逐个单跑全部 PASS**。

根因不是 Job Object 失效，而是**测试间互相污染**：超时类用例故意让 TD 处于"被 kill"状态，
后续用例的 `TotalTdProcesses()` 断言会看到前一个用例的残留。

修法：`SetUp`/`TearDown` 里等待所有 TD 相关进程归零（枚举
`td_commands_prompt / td_commands / td / td_client` 四种名字）。

---

## 6. 设备库与引脚查询

### 6.1 `import_device` 参数形态（实测纠正）

**这是 `import_device` 的完整实测矩阵，新器件/新封装接入时直接对照使用：**

| 传给 `import_device` 的参数 | 实测结果 | 判定 |
| --- | --- | --- |
| `ef2`（family 名） | `USR-8001 ERROR: Invalid device_db name` | ❌ |
| `ef2_4.db` 的**完整路径** | `USR-8316 ERROR: invalid speed grade setting "speed -2"` | ❌ |
| `ef2_1.db` | `USR-8001 ERROR: device ef2_1 not support package EF2L45LG144B` | ❌ 该 db 不支持此封装 |
| `ef2_2.db` | `USR-8001 ERROR: device ef2_2 not support package EF2L45LG144B` | ❌ 该 db 不支持此封装 |
| **`ef2_4.db`（裸文件名）** | ✅ 成功，`has_error=False` | ✅ **正确形态** |

**关键结论**：
1. 参数是**裸数据库文件名**（`<name>.db`），不是 family 名、也不是路径——与 `_db_candidates` 返回值直接对应；
2. 同一个 family 下**多个 db 支持的封装不同**，必须逐个试；
3. 带 `-speed -2` 会报 `USR-8316`，**必须去掉正负号**（`-speed 2`）。

实测成功序列（TD 2026.2，`arch/` 下 35 个 db 文件）：

```
import_device ef2_1.db -package EF2L45LG144B     → USR-8001 ERROR: device ef2_1 not support package ...
import_device ef2_2.db -package EF2L45LG144B     → USR-8001 ERROR: device ef2_2 not support package ...
import_device ef2_4.db -package EF2L45LG144B     → 成功（has_error=False）
```

**这验证了 `DB_PREFIXES` 降级遍历的必要性**：必须按候选顺序逐个试，直到 `has_error=False`。
工程器件是 `EF2L45LG144B`，而**前缀最靠前的 `ef2_1.db`/`ef2_2.db` 都不支持这个封装**——
只有 `ef2_4.db` 才行。若只试第一个候选，设备导入必然失败。

`_db_candidates("EF2")` 的返回顺序（前 4 个）：

```
ef2_1.db, ef2_2.db, ef2_4.db, ef2_s4.db, ef3_45.db, ...
```

规则：family 前缀（EF2 → `ef2`,`ef3`,`ef4`）优先，其余按字典序补全，
候选总数 = `arch/*.db` 的文件数（**不写死**——TD 升级会增删 db 文件）。

> **新增器件族时该做什么**：从这里就能直接看出——在 `DB_PREFIXES` 里加一条
> `"EF5": ("ef5","ef4")` 这样的降级链（先本族前缀，再退到兼容族），
> 然后确认 `arch/` 下确实存在对应前缀的 db 文件。

### 6.2 `query_current_package` 真实表格格式

夹具：`tests/td/fixtures/qp_*.txt`

**`-fio`**（122 行）：`Idx | PE | Location | Type | IntBank | ExtBank`

```
RUN-1001 :    1   |   p1   |    pb1b    |  fio   |     2     |     0
RUN-1001 :    2   |  p10   |    pb9b    |  fio   |     3     |     0
```

**`-clock_io`**（37 行）：`Sector | Location | Name | Config | IntBank | ExtBank`

```
RUN-1001 :     0     |   pl18b    |  P121  |   dpclkio_1    |     1     |     3
```

**`-cfg_io`**（12 行）：`OPTION | IO | SETTING`（SETTING 含 `dedicate` 的进黑名单）

```
ARC-1001 :      programn      |         P119          |  dedicate
ARC-1001 :   tdi/tms/tck/tdo  |  P136/P130/P131/P137  |  dedicate
```

**`-lvds`**（169 行）：`# | Type | IntBank | ExtBank | Name <[P],[N]> | Location <[P],[N]>`

```
RUN-1001 :   1   |  LVDS  |     7     |     2     |   <P100 , P99>   |    <PT8A , PT8B>
```

⚠ `-lvds` 输出里夹着一条 `USR-8192 ERROR: Differential location ...`，但**表格仍然正常输出**。
这是 `has_error()` 会误报的典型场景——查询类命令不能只看 `has_error`，要看表格行数。

| 解析项 | 状态 |
| --- | --- |
| `_parse_pipe_table`（前缀单元剔除、`\|` 计数） | ✅ 实现并单测 |
| `parse_fio` | ✅ **真实夹具 115 个引脚，与 Python 逐一一致** |
| `parse_clock_io` | ✅ **28 个 clock_capable，与 Python 一致** |
| `parse_lvds` | ✅ **100 个 lvds，与 Python 一致** |
| `parse_cfg_io` | ✅ **5 条 dedicated，与 Python 一致** |
| `_db_candidates` | ✅ **与 Python 同序**（ef2_1/ef2_2/ef2_4/ef2_s4 打头，总数 = `arch/*.db` 文件数） |

`parse_fio` 的过滤规则（`IsAllDigits(cells[0])`）实测有效：`qp_fio.txt` 共 117 行表格，
其中 2 行是表头（`Idx|PE|...`）与 `List of IO Banks: 0|1|2|3.`，被正确排除，剩 115 个引脚。

---

## 7. read_hdl 与 LSP 诊断的映射

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| `read_hdl -file {...} -top <top>` 参数形态 | ✅ | 必须显式给文件列表；裸 `read_hdl` 只报 top 警告 |
| 诊断 severity 映射 | ✅ | 见 §4.2 |
| **与 Python 逐字段对比（read_hdl 真实输出）** | ✅ | **21 条消息全部字段一致**（见下方 §7.1） |
| **TD 诊断没有列号** | ✅ | `character` 恒为 0，TD 粒度是「模块级 + 文件行号」 |
| `HDL-8007` 无行号时回落到 `.al` 路径 | ✅ task-13 完成 | Python: `path = project.al_path` |
| `black box` 消息升级为 warning | ✅ task-13 完成 | 含 "black box" 且非 "TD ignored this file" → severity=2 + 追加说明 |
| 文件名含空格 → `LSP-PATH-SPACE` | ✅ task-13 完成 | 唯一一条"TD 不报错但我们主动提示"的诊断 |
| 路径解析三态（绝对/相对/declared 回落） | ✅ task-13 完成 | 复用 `al_project`，逐 base 做 isfile 判定 |
| 行号 1-based → 0-based | ✅ task-13 完成 | `line-1`；`character` 恒 0；range 宽 1 列 |
| **IP 生成 HDL 把 `.vhd` 混进 Verilog 列表** | ⚠ **Python 版 bug，已复现并记录** | 见 §7.2 |

### 7.2 ⚠ 已发现的 Python 版 bug：IP 生成 HDL 把 `.vhd` 混进 Verilog 文件列表

**这是 task-13 在做逐字段对比时发现的、Python 版自身的 bug。C++ 版忠实复现了它**（移植第一原则是等价），但必须记录在案。

**位置**：`td.py :: ip_generated_hdl()` + `run_read_hdl()`

```python
def ip_generated_hdl(project):
    for entry in project.ip_files():
        stem = os.path.splitext(entry.path)[0]
        for ext in (".v", ".sv", ".vhd", ".vhdl"):     # <-- 全部扩展名都收
            candidate = stem + ext
            if os.path.isfile(candidate) and " " not in candidate:
                out.append(candidate)

def run_read_hdl(...):
    files = [f.path for f in project.hdl_files()]
    files = files + ip_generated_hdl(project)          # <-- 合并成一个列表
    tcl = "read_hdl -file {%s} -top %s" % (...)        # <-- 统一用 -file（Verilog）
```

**问题**：`read_hdl -file` 是按 **Verilog** 编译的。当 IP 同时生成了 `.v` 和 `.vhd`
（本机 `C:/work/CorpusAlpha/al_ip/` 下的 `pll_gen` 就是如此），`.vhd` 会被塞进 Verilog 文件列表。

**实测后果**（TD 2026.2，`C:/work/CorpusAlpha`）：

```
HDL-1008 : device name is not set, so there is no need to read *macro.v
HDL-1009 : device name is not set, so there is no need to read *macro.vhd
...
HDL-8007 ERROR: 'ef2_components' is not compiled in library 'ef2_macro' in <pll_gen.vhd>
HDL-8007 ERROR: ignore unit pll_gen due to previous errors in <pll_gen.v>
```

`elaborate` 阶段整体失败，于是：

| 现象 | 说明 |
| --- | --- |
| 20 条 `elaborate module ...` **全部消失** | 没有 elaborate 就没有这些行 |
| `HDL-7144 instantiate unknown module pll_gen` **消失** | pll_gen 已被当作"存在但编译失败"，不再是未知模块 |
| `HDL-8007 pll_gen is a black box` **消失** | 同上 |
| 剩下 40 条全是 `HDL-1007` Info | 21 条 `analyze verilog file`（无行号→丢弃后的可见项）+ 15 条 `undeclared symbol pll_gen_openN` + 其他 |

**对照验证**（关键证据）：

| 输入 | Python 结果 | C++ 结果 |
| --- | --- | --- |
| 只含 20 个工程 HDL（不含 IP 生成 HDL） | 22 条，2 个 warning | 22 条，2 个 warning ✅ |
| **含 IP 生成 HDL（`.v` + `.vhd`）** | **40 条，0 个 warning** | **40 条，0 个 warning** ✅ |

即：**C++ 与 Python 在两种输入下都逐字段一致**，差异全部来自 Python 的这个 bug。

**如何判断"谁对"**：
- `PYTHON-PARITY.md` 的对比脚本用的是**只含工程 HDL 的夹具**（不含 IP 生成 HDL），
  因为那个夹具是 task-9 抓的 `read_hdl`（当时未传 IP HDL）——那份对比是 **C++ vs Python 完全一致**。
- 改成含 IP HDL 后，两边**同样都**变成 40 条，说明 C++ 没有引入新偏差。
- 结论：**Python 版有 bug，C++ 忠实复现；修复需要改 Python 版的语义（vlog/vhdl 分开传），超出"移植"范围。**

**修复方向（留给后续任务，未实施）**：
1. `read_hdl` 的 Verilog 与 VHDL 文件应分开：Verilog 用 `-file`，VHDL 用 `-vhdl`；
2. 或者 `ip_generated_hdl` 只取与 IP 实际语言匹配的扩展名（`.ipc` 里可能记录了语言）；
3. 修好后本工程的诊断会恢复成 22 条含 2 个 warning 的形态。

**当前 C++ 行为的测试锚点**：
`tests/td/td_diagnostics_integration_test.cpp :: RunReadHdlProducesDiagnostics`
断言 **40 条、0 warning**，并在注释里完整写明上述机理。
若将来修掉这个 bug，这条测试会变红并提示需要更新。

### 7.3 逐字段对比结果（read_hdl 夹具，不含 IP 生成 HDL）

夹具：`tests/td/fixtures/read_hdl_corpus_alpha.txt`（TD 2026.2 真实输出，20 个 HDL 文件）

```
消息条数 : Python 21 / C++ 21
has_error: Python True / C++ True
has_fatal: Python False / C++ False
clean行数: Python 44 / C++ 44
✅ 完全一致：全部字段逐值相同（code/severity/message/path/line）
```

节选（C++ 侧输出，与 Python 逐值相同）：

```
[ 0] HDL-1007  INFO  elaborate module top                  C:/work/CorpusAlpha/src/top.v(15)
[ 1] HDL-7144  WARN  instantiate unknown module pll_gen    C:/work/CorpusAlpha/src/top.v(66)
[ 2] HDL-1007  INFO  elaborate module sys_rstn             C:/work/CorpusAlpha/src/system/sys_rstn.v(1)
...
[20] HDL-1007  INFO  elaborate module timeout_check(timeout_T=4400)  Y:/.../timeout_check.v(1)
```

**20 个文件 → 21 条消息，其中只有 1 条 WARN**，其余全是 INFO。
若把无 severity 的行误判成 warning，Problems 面板会被 20 条噪音淹没。

**列号的影响**：LSP `Range` 在 TD 诊断上是 `{line,0}-{line,0}`（零宽），VS Code 会在行首显示波浪线而不是 token 下划线。后续若要精确到 token，只能用本地 HDL 索引补充列号。

---

## 8. 待办

已完成（task-9）：
- [x] `td_install` / `td_output` / `td_session` / `td_device`

已完成（task-13）：
- [x] `src/lsp/diagnostic`：所有诊断来源的唯一形状
- [x] `src/td/td_diagnostics`：`TdMessage` → `Diagnostic` 映射
- [x] `ResolveTdFile`（绝对/相对/declared 回落）
- [x] `LSP-PATH-SPACE`
- [x] `EnsureDevice` / `RunReadHdl` 入口
- [x] 与 Python 逐字段对比（夹具 22 条，全字段一致；集成 40 条，全字段一致）

明确不在本轮范围（有意留给后续任务，非遗漏）：
- [ ] `run_read_adc`：ADC 引脚约束诊断（需 ADC 文件解析就位）
- [ ] 多来源诊断合并去重（Verible / ADC / SDC）—— 等各来源就绪后再设计
- [ ] `publishDiagnostics` 推送与版本管理
- [ ] 缓存/失效策略（`_invalidate_index`）
- [ ] 清空队列与重跑逻辑（0.10.28 那条）
- [ ] **修复 §7.2 的 IP 生成 HDL 混合语言 bug**（属语义变更，需单独决策）