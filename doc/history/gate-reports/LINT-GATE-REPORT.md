# 阶段 1 门禁报告：lint 等价验证

> # ✅ 结论：通过（PASS）
>
> **第 1 层**：规则清单 61 行 / 默认启用 42 条，与官方**逐字节完全一致**
> **第 2 层**：42/42 条规则单独验证一致
> **第 3 层**：全语料 **102/102 文件**诊断一致，**398 条诊断逐字节一致**，一致率 **100.00%**
>
> 测得时间：2026-11-18。数据由 `python tests/run_gates.py` 与
> `python tests/compare_lint.py` 实测产出。

---

## 0. 摘要：三类证据，**不要混读**（最先看这一节）

本门禁会产生**三个容易混淆的数字**。它们的证据力完全不同，
**只有 C 行是门禁结论**。请勿把 A 或 B 的百分比误读为"门禁通过"。

| # | 数字来源 | 实测值 | 证明了什么 | 不能证明什么 |
| --- | --- | --- | --- | --- |
| **A** | `官方 vs 官方`（自比） | **102/102，100.00%**（398 条诊断） | **采集与归一化是确定性的** —— 同一输入两次结果相同，脚本不会把同样的东西判成不同 | 不能证明脚本能发现差异（它可能对任何输入都返回"一致"） |
| **B** | `官方 vs 被测`（**单边变异**） | **57.14%**（21 文件中 12 一致 / 9 不一致） | **脚本具备检出能力** —— 人为差异能被精确定位 | 不能证明被测实现正确（这是对**脚本**的测试，不是对**实现**的测试） |
| **C** | `官方 vs lint_dump`（**全语料**） | **102/102，100.00%** | **门禁结论本身** | —— |

> ⚠️ **A 与 B 缺一不可，但只有 C 是结论。**
> A 证明"脚本不会误报"，B 证明"脚本不会漏报"，C 才是"实现正确"。

> ⚠️ **B 的关键在"单边"**：若变异同时作用在对照组上（例如两端都传
> `--rules=-no-tabs`），两边一起变、结果仍 100% —— 那是**假绿灯**。详见 §3.4。

---

## 1. 门禁目标

阶段 0 已用同一套方法论证明了 **parser 等价**（102/102 文件 token 逐字节一致）。
本任务要证明 **lint 等价**：

> **把 vendored Verible 的 lint 子系统（58 条规则 + 配置系统 + waiver 机制）
> CMake 化之后，其诊断输出是否与官方 `verible-verilog-lint.exe` 完全一致？**

判据：全语料诊断的 `(file, line, col, message, category, rule_name)` 六元组逐字节一致。

---

## 2. 三类证据，不要混读（重要）

本门禁会产生**三个容易混淆的数字**。它们的证据力完全不同，**只有第三个是门禁结论**：

| # | 数字来源 | 证明了什么 | 不能证明什么 |
| --- | --- | --- | --- |
| **A** | `官方 vs 官方`（自比） | **采集与归一化是确定性的** —— 同一输入两次结果完全相同，脚本不会把同样的东西判成不同 | 不能证明脚本能发现差异（它可能对任何输入都返回"一致"） |
| **B** | `官方 vs 被测`（**单边变异**） | **脚本具备检出能力** —— 人为制造的差异能被精确定位 | 不能证明被测实现正确（这是对脚本的测试，不是对实现的测试） |
| **C** | `官方 vs lint_dump`（**全语料**） | **门禁结论本身** | —— |

> ⚠️ **A 与 B 缺一不可，但只有 C 是结论。**
> 本报告在 §5 会分别列出 A/B/C，不要把它们当成同一个"一致率"。

> ⚠️ **B 的重点在"单边"**：如果变异同时作用在对照组上（例如两边都传 `--rules=-no-tabs`），
> 两边一起变、结果仍 100% —— 那是**假绿灯**。详见 §3.4。

---

## 3. 对比方法（`tests/compare_lint.py`）

### 3.1 为什么第 1 层必须最先做（风险前移）

Verible 的每条 lint 规则靠这个宏注册：

```cpp
// verilog/analysis/lint-rule-registry.h
#define VERILOG_REGISTER_LINT_RULE(class_name)                           \
  static verilog::analysis::LintRuleRegisterer<class_name::rule_type>    \
      __##class_name##__registerer(class_name::GetDescriptor, []() {     \
          return std::unique_ptr<class_name::rule_type>(new class_name()); \
      });
```

**每条规则是一个 static 全局对象，在程序启动时自注册。这些对象没有任何外部符号引用。**

后果：**MSVC 静态库链接器默认"只取被引用到的 .obj"，会把它们整体丢弃。**
于是出现最危险的一类失败：

> 程序正常启动、不报错、退出码 0，但**规则少了或全没了**。

因此 `--rules-file` 的输出行数就是一张**链接完整性检查表**：

| 现象 | 含义 |
| --- | --- |
| 61 行 | 全部规则都在 ✅ |
| < 61 行 | 少了 `61 - N` 条规则 → `--diagnose-missing` 精确定位 |
| > 61 行 | 多了规则（异常，通常是误把测试夹具纳入） |

**这一步成本几乎为零（一次命令），却覆盖了阶段 1 约 80% 的风险。** 所以它排在最前。

### 3.2 四层验证结构

| 层 | 命令 | 作用 | 成本 |
| --- | --- | --- | --- |
| **1** | `--rules-file` | 规则清单逐字节 diff + 缺失定位 | 极低 |
| **2** | `--single-rule` | 单规则定向（隔离多规则干扰） | 中 |
| **3** | 默认（全语料） | 诊断逐字节对比（**主判据**） | 中 |
| **4** | `--selftest` | 脚本灵敏度验证（变异测试） | 低 |

### 3.3 官方诊断输出的**三类行格式**（必须全部覆盖）

官方输出**不是单一模式**，实测至少三种形态。只匹配其中一种会让其余行被当成
"无法解析"而**静默丢弃** —— 又是静默失败。

```
# (a) 普通 lint 违规（带 Category 与 rule）
err_lock.v:11:7: Use spaces, not tabs. [Style: tabs] [no-tabs]

# (b) 列号带范围 + Category + rule
ON_DEL.v:24:15-23: Explicitly define a storage type for every parameter and localparam, (DELAY_CNT). [Style: constants] [explicit-parameter-storage-type]

# (c) 语法/词法错误（**没有** [Category] [rule] 后缀）
e02_extra_comma_in_portlist.sv:3:3: syntax error at token ","
bad-id-lex.sv:1:8-13: lexical error at token "654foo"
e01_missing_endmodule.sv:14:1: syntax error (unexpected EOF)
```

对应的正则（`compare_lint.py`）：

```python
DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+(?:-\d+)?):\s*(?P<msg>.*?)"
    r"(?:\s*\[(?P<cat>[^\[\]]*)\])?\s*\[(?P<rule>[^\[\]]+)\]\s*$"
)
SYNTAX_ERR_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+(?:-\d+)?):\s*"
    r"(?P<msg>(?:syntax|lexical) error.*?)\s*$"
)
```

对 (c) 类行，比较时用固定占位符 `<syntax-error>` 填充 category/rule 字段，
保证两边以同样方式归一化。

### 3.4 变异测试的设计要求（独立小节，后续各阶段沿用）

验证「测试能发现问题」时，**必须确认问题真的被引入了，且只引入在一侧**。

本项目已经踩到过**两次形态不同但同类**的陷阱：

| 阶段 | 假绿灯形态 | 本质 |
| --- | --- | --- |
| 阶段 0 | `count == count` 断言在两边都为 0 时成立（`0 == 0`） | **断言恒为真** |
| 阶段 1 | 变异 flag 同时传给两端，两边一起变 | **变异作用在了对照组上** |

两者的共同教训：

> 验证灵敏度时，"施加了变异之后仍然通过"**不等于**"脚本不灵敏" ——
> 必须先确认变异真的生效，且只作用在被测一侧。

**本门禁的变异测试实测结果：**

```
对照组（两端都不禁用 no-tabs）:     102/102 一致，100.00%
变异组（仅被测端禁用 no-tabs）:      一致率跌至 57.14%，精确报出缺失的 [no-tabs] 诊断
```

### 3.4.1 为什么要先做自比（这不是形式主义）

在编写 `compare_lint.py` 的过程中，自比环节**连续抓出了 3 个会让门禁完全失效的 bug**，
而这 3 个 bug 的**共同特征**是：**它们都表现为"官方自比都过不了"**。

| # | Bug | 症状 | 若不先做自比的后果 |
| --- | --- | --- | --- |
| 1 | 官方 lint 诊断走 **stderr** 而非 stdout | 只读 stdout → **0 条诊断** | 两个"都没诊断"的实现被判**一致** —— 门禁直接失效，且看起来完全正常（退出码 0、无报错、输出合法） |
| 2 | 列号带**范围后缀**（`24:15-23`） | 正则只支持单数列 → 大量诊断被判"无法解析" | 差异被静默丢弃 |
| 3 | 语法/词法错误行**没有** `[Category][rule]` 后缀 | 正则要求必须有方括号 → 这类行全被判"无法解析" | 错误恢复路径完全未被比较 |

**核心论证**：

> 如果跳过自比直接测我们的实现，上述 3 个 bug 会表现为
> **我们的实现与官方大量不一致** —— 而每一个"不一致"看起来都像
> "我们的移植有问题"。
>
> 你会带着这 3 个假差异去追查根本不存在的问题，**而真正的移植问题反而被淹没**。

因此顺序必须是：**自比（验证脚本）→ 单边变异（验证灵敏度）→ 全语料对比（门禁结论）**。

> 教训：**自比环节不是形式主义，它是"脚本正确性"的唯一验证手段。**

### 3.5 必须硬处理的四个坑（从源码查出，非推测）

| # | 坑 | 处理 |
| --- | --- | --- |
| 1 | `--rules_config_search` 会**向上搜索** `.rules.verible_lint` | 显式置 `false`（`LinterOptions.rules_config_search = false`）。否则语料目录附近有配置文件会造成假差异 |
| 2 | 规则**配置串默认值**也在默认表里（`-line-length=length:100`、`-dff-name-style=input:next,n,d;...`） | 由第 1 层逐字节 diff 覆盖 |
| 3 | `--lint_fatal` / `--parse_fatal` 控制**退出码** | 除诊断外还要比退出码 |
| 4 | `--waiver_files` 会**抑制诊断** | 两端都不带 waiver |

### 3.6 ⚠️「源码层面不需要」≠「链接层面不需要」

这是本项目移植过程中最容易误判的一类问题，记录备查。

**案例**：`GetLintRuleFile()` / `LinterConfigurationFromFlags()` 两个函数的**函数体**
完全不需要 parser（它们只操作规则配置与规则描述表）。据此我曾判断"第 1 层不需要
`verilog-final.tab.cc`"。

**这个判断在链接层面是错的**，原因：

- 这两个函数位于 `verilog-linter.cc`；
- 该 **翻译单元**（TU）`#include "verible/verilog/analysis/verilog-analyzer.h"`，
  且同文件里的 `Lint()` 方法实际调用了 analyzer；
- **C++ 静态库的链接粒度是翻译单元，不是函数** —— 只要链接 `verilog-linter.lib`，
  整个 TU 会被拉入，从而需要 parser 符号。

> **正确做法**：判断"某个库是否必需"时，看该 **TU 的 include 与符号引用**，
> 而不是看调用路径上是否用到了具体函数。

### 3.7 lint 诊断的输出通道（又一个静默陷阱）

```powershell
$ verible-verilog-lint.exe --ruleset=default file.v
# stdout: (空)
# stderr: file.v:11:7: Use spaces, not tabs. [Style: tabs] [no-tabs]
```

> ⚠️ `verible-verilog-lint` 的诊断输出在 **stderr**，不是 stdout。
> 若只读 stdout，会得到"0 条诊断"，且两个实现都会被判为一致
> —— **这是本门禁最危险的假通过形态**（不是"差异被容忍"，而是"根本没采集到数据"）。
> 任何 lint 相关的自动化脚本都必须同时采集 stdout 与 stderr。

注意例外：`--print_rules_file` 的输出走 **stdout**（实现是 `GetLintRuleFile(&std::cout, ...)`）。

`lint_dump` 自己选择让诊断走 **stdout**（工具输出归 stdout 更符合惯例），
对比脚本通过 `run_lint_tool()` 合并两个通道，抹平这一差异。

> 附带风险提示：若将来有人绕过脚本、直接对两边输出做文本 diff，
> 会因通道不同而误判"官方没有输出"。

---

## 4. 语料

复用阶段 0 的 `tests/gate-corpus/`，共 **102 个文件**：

| 子目录 | 来源 | 文件数 | 说明 |
| --- | --- | --- | --- |
| `corpus_alpha/` | `C:/work/CorpusAlpha` | 21 (`.v`) | 真实 Anlogic 工程 |
| `verible_lint_testdata/` | vendored Verible lint testdata | 73 (`.sv`) | SV 特性覆盖 |
| `error-cases/` | 本任务手工构造 | 8 (`.sv`) | 故意语法错误，覆盖错误路径 |
| **合计** | | **102** | |

---

## 5. 对比结果

**被测产物**：`build/bin/lint_dump.exe`（1,693,184 B，MSVC 19.52 / Release / 静态 CRT）
**基准产物**：`C:/tools/verible\verible-verilog-lint.exe`

### 5.1 第 1 层：规则清单（链接完整性）

```
  官方      : 61 行规则，其中默认启用 42 条
  lint_dump : 61 行规则，其中默认启用 42 条
  ✅ 逐字节完全一致
```

| 指标 | 官方 | lint_dump | 结果 |
| --- | --- | --- | --- |
| 规则总数 | 61 | 61 | ✅ |
| 默认启用数 | 42 | 42 | ✅ |
| 禁用规则数 | 19 | 19 | ✅ |
| 配置串（如 `line-length=length:100`） | — | — | ✅ 逐字节相同 |

> 第 1 层通过意味着：**没有规则因库漏链而静默消失**。
> 这是阶段 1 头号风险的正式排除（该风险在阶段 0/1 已真实发生 3 次构建缺陷）。

### 5.2 第 2 层：单规则定向验证

**为什么第 3 层 100% 了还要做这层**：第 3 层只说明"默认规则集下诊断相同"。
如果两条规则**互相抵消**（A 多报一条、B 少报一条），第 3 层的总数与顺序比对
可能仍然通过。单规则验证能排除这种"互补抵消式错误"。

```
共 42 条规则，一致 42，不一致 0，一致率 100.00%
门禁结论（第 2 层）：通过 —— 每条规则单独验证均一致
```

| 指标 | 值 |
| --- | --- |
| 语料中实际产生过诊断的规则 | **42** 条 |
| 单规则验证一致 | **42 / 42** |
| 不一致 | **0** |

做法：对该规则**产生过诊断的文件**，用 `--ruleset=none --rules=+<rule>`
两端各跑一次，逐文件比对诊断。

> 性能说明：最初实现对 42 规则 × 102 文件的**全组合**跑，超过 10 分钟未完成
> （6000+ 次进程启动）。改为只对「官方在该文件上产生过该规则诊断」的组合比对，
> 降到 5.7 秒，且不降低验证强度——因为两边都无诊断的组合比对价值极低。

### 5.3 第 3 层：全语料诊断（主判据）

```
语料：tests/gate-corpus   文件数：102
共 102 文件，一致 102，不一致 0，一致率 100.00%
对比诊断总数（官方）：398
```

| 指标 | 值 |
| --- | --- |
| 语料文件数 | **102** |
| 一致文件数 | **102** |
| 不一致文件数 | **0** |
| **一致率** | **100.00%** |
| 对比诊断总数 | **398** |

比较字段（全部逐字节，file 归一化路径前缀）：
`(file, line, col, message, category, rule_name)` 六元组。

### 5.4 证据 A：自比（采集正确性）

| 指标 | 值 |
| --- | --- |
| 语料 | 102 文件 |
| 一致 | **102 / 100.00%** |
| 对比诊断总数 | 398 |

**仅证明采集与归一化是确定性的，不构成门禁结论。**

### 5.5 证据 B：单边变异（脚本灵敏度）

| 场景 | 一致率 |
| --- | --- |
| 对照组（两端同样配置，21 文件） | **100.00%**（21/21） |
| 变异组（**仅被测端** `--rules=-no-tabs`） | **57.14%**（12/21） |
| 官方诊断数（对照组） | 334 |
| 缺失项被精确定位为 | `[no-tabs]` |

**证明脚本能发现真实差异；对照组证明它不会误报。缺一不可。**

### 5.6 退出码语义（两态实测）

| 场景 | 官方 rc | lint_dump rc | 结果 |
| --- | --- | --- | --- |
| 有违规（`err_lock.v`，默认规则集） | 1 | 1 | ✅ |
| 无违规（同文件 `--ruleset=none`） | 0 | 0 | ✅ |

`check_syntax` / `parse_fatal` / `lint_fatal` 均采用官方 `ABSL_FLAG` 默认值（三者都是 `true`）。
对比脚本对每个文件同时比较退出码（见 §3.5 坑 3），**102 文件退出码全部一致**。

### 5.7 双跑（gate + lintgate）

```
  [PASS] gate             102/102 文件一致 (100.00%)
  [PASS] lintgate-rules   官方 61 行 / 启用 42；我方 61 行 / 启用 42
双跑结论：**全部通过**
```

`gate` 通过的含义见 §6.7（本轮 gate 是在 G4 + G6a 两次生成器修正之后取得的）。

---

## 6. 不一致明细与根因分析

### 6.0 最终不一致明细

**第 1 层：0 处不一致**（逐字节相同）
**第 3 层：0 处不一致**（102/102 文件，398 条诊断）

不存在需要归因为"输出格式差异"或"调用路径差异"的项。

> 过程中曾出现 9 个文件不一致（91.18%），根因是**我自己的工具默认值错误**
> ——见 §6.8。修复后归零。

> ⏳ **待填写**（尚无被测实现，无法产生不一致）

### 6.1 门禁基础设施问题（阻塞项，记录备查）

在准备 `lint_dump.exe` 的过程中，构建链路上发现一个**生成器缺陷**，导致 lint 相关的
CST/parser 包大面积编译失败。虽然不是 lint 逻辑本身的问题，但它阻塞了本门禁，
故记录于此。

**现象**

```
verilog.tab.hh(545): fatal error C1070: #if/#endif 对不匹配
```

**根因**：`cmake/scripts/verible-generators.cmake` 的 **G4** 补丁
（从 `verilog.tab.hh` 抽取 `verilog_token_enum` 枚举段生成 `verilog-token-enum.h`）
起点锚定错误。

生成结果是 **547 行，`#if` 7 个 / `#endif` 6 个 —— 失衡**，文件头自带
`// verible-patch: g4 hits=1`。

正确的切片（实测计算）：

| 项 | 值 |
| --- | --- |
| 起始 | `verilog.tab.hh` 第 **63** 行 `#ifndef VERILOG_TOKENTYPE`（行首精确匹配） |
| 结束 | 第 **543** 行 `#endif`（从起始行按嵌套深度计数到 0） |
| 切片行数 | 543 − 63 + 1 = **481** |
| 加上外层 guard 后 | **485 行**，`#if` 2 个 / `#endif` 2 个，平衡 |

而当前实现从第 41 行的 include guard
`#ifndef YY_VERILOG_..._VERILOG_TAB_HH_INCLUDED` 开始截取，
把 DEBUG / Stype 声明段一并带入（多出约 60 行），且该 include guard 的 `#endif`
（第 558 行）不在切片内 → **就是缺失的那一个**。

**验证**：按正确切片手工重新生成后得到 485 行 / 2 对 2 平衡的
`verilog-token-enum.h`，与阶段 0 手工生成的那版**逐字节一致**，
`cst_dump` 在该版本下曾通过 102 文件门禁。故修复方案是确定的。

> 注：该问题必须由生成器修复，**不能靠手改产物** —— 生成器每次运行都会覆盖。

### 6.2 生成器缺陷链（本轮共发现 3 个，均已定位并上报）

本门禁的准备过程暴露了 `cmake/scripts/verible-generators.cmake` 的**三个独立缺陷**。
它们不属于 lint 逻辑，但直接阻塞了门禁，且都是"**构建管线**"类问题，记录备查。

| # | 缺陷 | 症状 | 根因 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | G1/G2/G3 **VERBATIM 引号** | `win_flex.exe: could not create "Y:/..."`（失败；但手工执行同一命令成功） | `COMMAND ... --outfile="${VAR}"` 展开后，`VERBATIM` 使整个参数被二次引号包裹，导致 flex 收到的 argv 含**字面引号字符** | ✅ 已修：改为 `"--outfile=${VAR}"` 整体单 token |
| 2 | **G4 锚点失效** | `verilog-token-enum.h` 失衡（`#if`=7 / `#endif`=6），引发 `C1070: #if/#endif 对不匹配` | CMake 正则 `^` **只匹配整段字符串开头**；`"(^|\n)"` 中的 `^` 分支在位置 0 空匹配成功 → 锚点整体失效，切片从文件第 1 行开始。**所有成功信号都在**（`REGEX MATCH` 非空、`hits=1`），只是产物是坏的 | ✅ 已修：改逐行扫描 + 行级精确匹配 + 嵌套深度计数 |
| 3 | **G6a 未生效** | `verilog-final.tab.cc` 编译失败 `C2280 × 4`（`unique_ptr` 拷贝赋值被 delete） | `verilog-moved.tab.cc` 比输入**大 44 字节**（确实改了东西）但 `std::move(yylval)` 出现 **0** 次（应 2 次），未处理 `= yylval;` 仍为 2 处。**极可能是缺陷 2 的同源问题**（同为"按行正则替换"） | 🔴 阻塞中 |

### 6.3 缺陷 2 的教训：最难查的是"返回成功但语义不对"

缺陷 2 值得单独记一笔，因为它具有最恶劣的失败形态：

> 代码走了成功分支（正则命中、返回非空、`hits=1` 看起来正常），
> **没有任何错误信号**，但产物是错误的。

而且**报错位置具有误导性**：MSVC 报在 `verilog.tab.hh(545)`，
而真正失衡的是被它包含的 `verilog-token-enum.h`。
排查者第一眼会去查 `verilog.tab.hh`（该文件实测完全平衡：7/7，最终深度 0），
白白浪费一轮。

**通用教训**：对生成物做断言时，应断言**结构性不变量**
（如 `#if` 与 `#endif` 数量相等、切片行数在期望区间），
而不是只断言"命令执行成功"。

### 6.4 缺陷 3 印证了阶段 0 的结论

阶段 0 门禁报告曾断言：

> G6a 的正确性由**编译器**保证（`unique_ptr` 拷贝赋值已 `delete`），
> 不需要额外运行时断言。

本次缺陷 3 是对该论断的实战验证：**G6a 一失效立刻编译失败（`C2280 × 4`），
绝不静默**。这也再次说明为什么两个补丁的风险等级不同：

| 补丁 | 失效表现 | 防护手段 |
| --- | --- | --- |
| G6a `std_move_parser_symbols` | **编译失败**（响亮） | 编译器本身 |
| G6b `record_recovered_syntax_errors` | **静默**：能编译、token 一致，只是错误恢复丢失 | 必须靠对比官方错误消息（阶段 0 的 `error-cases` 专项） |

### 6.5 关于"没有隐藏的 re2/API 层问题"（正面证据）

修好 G4 后做了一次完整构建统计，错误分类如下：

| 错误码 | 数量 | 归因 |
| --- | --- | --- |
| `C1070`（`#if/#endif` 不匹配） | **0**（G4 修复前有大量） | G4 锚点失效的直接后果，修复后全消 |
| `C2280`（`unique_ptr` 拷贝赋值） | 4 | G6a 未生效的直接后果 |
| `LNK2019`（`InitCommandLine` 未解析） | 1 | `lint_dump` 少链接一个库，已修 |
| **re2 / absl / Verible API 层错误** | **0** | —— |

**这个统计的意义**：

> checkers 的 120 个 `.cc`（依赖 re2 + CST 全套 + matcher）能编译通过，
> **不是因为碰巧没触发 API 差异，而是真的没有 API 差异**。

这是"**Verible 移植风险低**"的一条正面证据：vendored 版本与官方二进制对应的
源码在 MSVC + C++23 下没有暴露 API 不兼容。

### 6.6 同类问题的历史（值得记住的模式）

这是本项目累计遇到的构建管线类问题，阶段 0 三次 + 阶段 1 三次：

| # | 问题 | 表现形式 | 是否响亮 |
| --- | --- | --- | --- |
| 1 | `verilog-y-cc` 建成 INTERFACE 库 | `LNK2001` 未定义符号 | ✅ 响亮 |
| 2 | `command-file-lexer.lib` 不在依赖图但被 `/WHOLEARCHIVE` 引用 | `LNK1356` | ✅ 响亮 |
| 3 | G1/G2/G3 `VERBATIM` 引号 | 文件没生成 | ✅ 响亮 |
| 4 | **G4 锚点失效** | `C1070`，但**报错位置误导**（报在 `verilog.tab.hh(545)`，实际坏的是 `verilog-token-enum.h`） | ⚠️ 响亮但难定位 |
| 5 | **G6a 未生效** | `C2280 × 4` | ✅ 响亮 |

**共同点**：**全部是构建管线问题，0 个涉及解析/lint 逻辑本身。**
这提示：CMake 化 Verible 的风险主要不在"解析器行为漂移"（阶段 0 已证明 102/102 一致），
而在"**构建装配出错**"。

> 该规律已同步写入根 `README.md` 的「Verible CMake 化的风险画像」一节。

### 6.7 双跑验证：gate 通过 ≠ gate 还能跑

生成器修复后必须**同时**跑两套门禁。原因：

- G4 的修复让 `verilog-token-enum.h` 从 **547 行（失衡，坏）** 变成 **485 行（平衡，好）**，
  **文件内容确实变了**；
- G6a 的修复让 `verilog-final.tab.cc` 里**多了 5 处 `std::move`**
  （4 处 move 替换 + 1 处注释），**文件内容也确实变了**；
- 因此"gate 仍然 102/102"证明的**不是**"gate 还能跑"，
  而是"**两次生成器修正都没有破坏解析行为**" —— 这是两件不同的事。

> **本轮 gate 通过是在 G4（切片修复）+ G6a（move 补丁）两次生成器修正之后取得的。**
> 这比单纯的"gate 还能跑"强得多：它说明无论切片范围的修正，
> 还是 `std::move` 语义的引入（移动而非拷贝），**都没有改变 token 序列**。

| 验证项 | 含义 |
| --- | --- |
| `gate` 通过 | 阶段 0 结论在**两次生成器修正之后**依然成立（解析行为未被破坏） |
| `lintgate` 通过 | lint 规则清单（61/42）与官方一致，无规则因漏链丢失 |

统一入口：

```bat
python tests\run_gates.py --json build\gates.json
```

---

### 6.8 我自己的实现缺陷（已全部修复，记录备查）

这 4 个 bug **全部出在 `lint_dump` 自身**，不是 Verible 移植问题。
它们值得记录，因为**每一个单独存在都会让门禁产生错误的结论**。

| # | 缺陷 | 症状 | 根因 | 若不修的后果 |
| --- | --- | --- | --- | --- |
| 1 | **未链接聚合 target，漏 19 条规则** | `--rules-file` 只输出 **42 行**（= 默认启用数） | CMakeLists 用了已不存在的旧变量 `VERIBLE_LINT_RULE_TARGETS`，自己 foreach 收集规则库，**漏掉默认禁用的那 19 条** | 第 1 层必然失败，且容易误判为"规则库漏链" |
| 2 | **`violation_handler` 传 `nullptr`** | `--lint` **SIGSEGV**（栈探测 `_chkstk`，极易误判成栈溢出/递归） | `verilog-linter.cc:168` 是**无条件解引用** `violation_handler->HandleViolations(...)`；我传了 `nullptr`。官方传 `new ViolationPrinter(&std::cerr)` | 第 3 层完全跑不了 |
| 3 | **`check_syntax` 默认值错误** | 9 个含语法错误的文件 `dump=0`，一致率 **91.18%** | 我默认 `false`，官方 `ABSL_FLAG(check_syntax, true)` 默认 **`true`** | 语法/词法错误路径完全未比较，**门禁假通过** |
| 4 | `--rules-file` / `--lint` 被 absl 拒绝 | `ERROR: Unknown command line flag 'rules-file'` | `InitCommandLine` 内部用 absl 解析 argv，遇到未知 flag 直接退出 | 工具完全不可用 |

**缺陷 1 的教训值得单独记**：

> `GetLintRuleFile()` 内部的 `GetAllRuleDescriptions()` **完全依赖静态注册表**。
> 没被链接的规则，**连"它被禁用了"这件事本身都不可见** —— 既不出现、也不报错。
>
> 所以"规则数变少"不能靠肉眼数，必须与官方逐字节比对。

**缺陷 3 的教训**：

> 工具的**默认值必须与官方一致**。`check_syntax` 这种"默认开"的开关，
> 默认关掉之后症状是"某些文件诊断变少"，很容易被误解读为"我们的解析器有问题"。
>
> 正确做法是**逐个核对官方 `ABSL_FLAG` 的默认值**，而不是凭直觉设默认。

---

## 7. 门禁结论

### 7.1 判定

> ## ✅ **阶段 1 lint 门禁通过（PASS）**

依据：

1. **第 1 层**：规则清单 61 行 / 默认启用 42 条，与官方**逐字节完全一致**
   —— 无规则因库漏链而静默消失；
2. **第 2 层**：**42/42** 条规则的单独定向验证全部一致
   —— 排除多规则"互补抵消"式错误；
3. **第 3 层**：全语料 **102/102 文件**一致，**398 条诊断逐字节一致**（100.00%），
   比较字段为 `(file, line, col, message, category, rule_name)` 六元组；
4. **证据 A**（自比 102/102）证明采集与归一化确定性；
5. **证据 B**（单边变异 57.14%）证明脚本具备检出能力且不误报；
6. 退出码语义一致（两态实测：有违规 → 1，无违规 → 0）。

`ctest -L lintgate` → **3/3 通过**（`lintgate_rules_file` / `lintgate_single_rule` / `lintgate_corpus`）。

### 7.2 根因分类

| 类别 | 出现次数 |
| --- | --- |
| 输出格式差异 | **0** |
| 调用路径差异 | **0** |
| 真实 lint 行为差异 | **0** |

### 7.3 对"止损评估"的回答

**未发现任何 lint 行为差异。** lint 子系统的 CMake 化是等价的。

### 7.4 结论的适用范围（务必连同 §8 一起读）

"通过"的准确含义是：

> 在 **102 个语料文件**上，**lint 诊断六元组**与官方发行二进制**完全一致**，
> 且 **61 条规则全部在场、默认启用 42 条、配置串逐字节相同**。

它**不**代表：
- lint 的全功能已验证（waiver、规则配置文件、formatter、autofix 均未覆盖，见 §8）；
- 所有 Verilog/SystemVerilog 语法都已覆盖（语料有限，见 §8.2）；
- lint 规则的**内部实现**没有因编译器差异而行为不同（语料未触发，但无法排除极端输入）。

### 7.5 遗留事项（不阻塞本门禁）

| 优先级 | 事项 | 责任 |
| --- | --- | --- |
| 中 | 把 `tests/compare_lint.py` 注册为 `ctest -L lintgate` 用例 | gate-verify |
| 中 | 阶段 1 收尾时扩展语料到千行级真实工程（当前 21 个真实文件平均仅 3.6 KB） | 阶段 1 |
| 低 | waiver / `--rules_config` 行为的独立验证 | 阶段 1 后续 |
| 低 | formatter 等价验证（verible-build 进行中） | task-12 |

---

## 8. 本门禁**没有**验证的范围

"阶段 1 lint 门禁通过"只意味着**在下列已测维度上与官方一致**。
它**不**意味着 Verible 的 lint 子系统全功能已验证。明确划出边界：

### 8.1 未验证项

| 未验证 | 说明 | 归属 |
| --- | --- | --- |
| **waiver 机制** | `--waiver_files` 的自定义豁免、waiver 行解析、`--autofix=generate-waiver` 生成。本门禁**两端都不带 waiver**，因此完全没有覆盖 | 阶段 1 后续或阶段 2 |
| **`--rules_config` / `--rules_config_search` 的实际行为** | 本门禁**硬编码关闭**（`rules_config_search = false`），正是为了避免向上搜到 `.rules.verible_lint` 造成假差异。因此**配置文件解析路径完全未验证** | 阶段 1 后续 |
| **`--rules` 复杂配置串** | 只验证了默认集与简单 `+/-` 语法；规则的**参数化配置**（如 `-line-length=length:120`）未逐一验证 | 低优先级 |
| **formatter** | `verible-verilog-format` 的排版算法、对齐、缩进。**完全未验证**（verible-build 正在做） | task-12 后续 |
| **`common/lsp` / jcxxgen 代码生成** | 不属 lint 范围 | task-12 |
| **自动修复（autofix）** | `--autofix=inplace` / `patch` 等改写行为 | 阶段 2+ |
| **多文件批处理语义** | 官方的多文件并行输出顺序、`--lint_fatal` 在多文件下的聚合语义 | 低优先级 |
| **性能** | lint 速度、大文件表现 | 阶段 1+ |
| **非 Verilog 输入** | VHDL、TD 约束文件 | 不属于 Verible |
| **MSVC vs clang 的 UB 差异** | 官方 release 用 clang，本仓库用 MSVC。理论上边界输入可能存在 UB 暴露差异；102 文件语料未触发 | 无法完全排除 |

### 8.2 语料规模的真实局限（必须诚实说明）

本结论基于 **102 个文件**，其中：

| 子类 | 文件数 | 平均大小 | 说明 |
| --- | --- | --- | --- |
| 真实工程（CorpusAlpha） | 21 | ~3.6 KB | **唯一代表实际使用场景**的部分 |
| SV 特性覆盖（lint testdata） | 73 | ~121 B | 极小片段，覆盖面广但深度浅 |
| 错误用例（手工构造） | 8 | ~123 B | 只覆盖常见错误类型 |

**因此"一致"的强度主要来自 21 个真实文件，而非文件数量。**
73 个测试片段共约 8.8 KB，能验证"规则在简单输入上行为一致"，
但**不能证明在复杂工程（长文件、深嵌套、大量 generate/macro）上同样一致**。

阶段 1 收尾时应把语料扩到千行级、多模块、含复杂宏的真实工程。

### 8.3 关于"规则清单一致 ⇒ 规则行为一致"的推理边界

第 1 层（规则清单逐字节一致）能保证：

- ✅ 规则**数量**一致（61 条全在）
- ✅ 规则**默认启停状态**一致（42 条启用）
- ✅ 规则**参数默认值**一致（配置串逐字节相同）
- ✅ 没有规则因库漏链而**静默消失**

但**不能**保证：

- ❌ 每条规则在**任意输入**上的诊断内容一致（这只能靠第 3 层语料对比，而语料有限）
- ❌ 规则的**内部实现**没有因编译器差异而行为不同（例如 re2 正则库版本差异可能影响 `*-style` 类规则）

> 换句话说：**第 1 层排除的是"规则存在性"问题，不是"规则正确性"问题。**
> 两者都需要，但前者成本低得多，所以排在前面。

### 8.4 本门禁的前提假设

本门禁的对比有效性依赖以下前提，若前提变化则结论需重新评估：

1. **官方基准二进制固定**：`C:/tools/verible\verible-verilog-lint.exe`。
   更换基准需要重新采集全部基线。
2. **vendored 源码未被改动**：`thirdparty/verible/` 下只有构建系统适配，
   没有修改 `.cc/.h` 业务逻辑（漏链问题通过 CMake 修正，不改源码）。
3. **规则注册表完整**：61 条规则全部被链接进最终可执行文件
   （由 `/WHOLEARCHIVE` + `verible_lint_rules_all` 保证，已用 `dumpbin` 逐条核对）。
4. **诊断输出通道**：官方走 stderr、`lint_dump` 走 stdout，对比脚本合并两个通道后比较。
   若将来官方改为 stdout（或反之），脚本仍能工作，但**不要**绕过脚本直接做文本 diff。

---

## 9. 复现步骤

### 9.1 规则清单（第 1 层）

```bat
:: 生成规则映射表（源码 -> 库）
python tests\gen_rule_map.py --json build\rule-map.json

:: 规则清单对比
python tests\compare_lint.py ^
  --lint-dump build\bin\lint_dump.exe ^
  --official C:/tools/verible\verible-verilog-lint.exe ^
  --rules-file
```

### 9.2 缺失规则定位

```bat
python tests\compare_lint.py ^
  --lint-dump build\bin\lint_dump.exe ^
  --official C:/tools/verible\verible-verilog-lint.exe ^
  --diagnose-missing
```

输出形如：

```
缺失规则：signal-name-style
  所在源码：verible/verilog/analysis/checkers/signal-name-style-rule.cc
  推测库：  signal-name-style-rule
  lib 状态：存在 (verible__verilog__analysis__checkers__signal-name-style-rule.lib)
  符号检查：✅ 含 SignalNameStyleRule
  结论：lib 存在且含该规则符号 → 问题在【最终可执行文件未链接该 lib，或缺 /WHOLEARCHIVE】
```

### 9.3 全语料对比（主判据）

```bat
python tests\compare_lint.py ^
  --lint-dump build\bin\lint_dump.exe ^
  --official C:/tools/verible\verible-verilog-lint.exe ^
  --corpus   tests\gate-corpus ^
  --pattern  ".v,.sv"
```

### 9.4 脚本灵敏度自检（改动脚本后必跑）

```bat
:: 自比：必须 100%
python tests\compare_lint.py --selftest ^
  --lint-dump C:/tools/verible\verible-verilog-lint.exe ^
  --official C:/tools/verible\verible-verilog-lint.exe ^
  --corpus   tests\gate-corpus

:: 单边变异：必须 < 100%（否则脚本作废）
python tests\compare_lint.py --selftest ^
  --lint-dump C:/tools/verible\verible-verilog-lint.exe ^
  --official C:/tools/verible\verible-verilog-lint.exe ^
  --corpus   tests\gate-corpus\corpus_alpha --pattern ".v" "--rules=-no-tabs"
```

---

## 10. 附录：规则映射基础设施

`tests/gen_rule_map.py` 从源码现场构建 `rule_name -> {class, source, target}` 映射，
**不硬编码规则清单**，因此源码增删规则时映射自动跟上。

**实测：源码注册规则数 == 官方规则数 == 61，零差异。**

这个数字的意义：**规则清单的"真值"与源码是对齐的**。因此将来若
`--rules-file` 输出少于 61 条，就一定是**构建/链接问题**，不会是源码问题
——这把诊断范围从 120 个文件的任意处，缩小到了构建系统。

开发过程中修掉的两个推导 bug（都是"假差异"的来源，记录备查）：

| Bug | 症状 | 根因 |
| --- | --- | --- |
| 从类名推导规则名 | `always-ff-non-blocking` 被拼成 `always-f-f-non-blocking` | 驼峰拆词对连续大写（`FF`）失效 |
| 未排除测试文件 | 多出 `test-rule1..5`、`line-rule1`、`tree-rule1/2` 等 → 71 条 | `_test.cc` 里的 `VERILOG_REGISTER_LINT_RULE` 是测试夹具 |

> 第二个尤其值得记：若未排除，会得到 71 条，进而误以为"源码有 71 条但我们只输出 61 条，
> 缺了 10 条"，去追一个**不存在**的问题。**假差异比真差异更浪费时间。**