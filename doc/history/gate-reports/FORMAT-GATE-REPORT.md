# 阶段 1 formatter 门禁报告：CMake 化 Verible 的格式化输出与官方二进制一致性

> **结论：✅ 通过（PASS）**
>
> 102 个语料文件，格式化输出**逐字节一致率 100.00%**，退出码一致。
> 变异测试证明对比脚本灵敏（不是"只报全一致"的空脚本）。
>
> 边界（见 §6）：证明的是**格式化行为等价**，未覆盖 `--lines` 局部格式化、
> `--inplace` 写回、`--verify` 只检模式、以及超长行的搜索上限触发。
>
> 本文件由 Lead 维护。测得数据时间：2026-11-18。

---

## 1. 目标

回答一个二元问题：

> 把 vendored Verible（commit `c6b29d62`）从 Bazel 构建改造成 CMake + MSVC 构建之后，
> 它的 **SystemVerilog 格式化行为**是否与官方发行的二进制完全一致？

为什么必须验证：formatter 的输出会作为整文档 `TextEdit` 直接进入用户的编辑器
（`textDocument/formatting`）。**任何差异都会在用户每次保存时改写他的代码**——
这是本项目里"错误代价最高"的一条链路。

---

## 2. 参与对比的两个端点

| 端点 | 路径 | 说明 |
| --- | --- | --- |
| 官方（基准） | `C:/tools/verible\verible-verilog-format.exe` | 官方 release 二进制 |
| 我方 | `build-fmt\bin\format_dump.exe` | 由 `src/tools/format_dump/main.cpp` 构建 |

---

## 3. 方法论：复用官方同一条路径，不重写输出

`format_dump` **没有重新实现任何输出逻辑**，而是逐项复用官方
`verilog/tools/formatter/verilog-format.cc` 的同一条调用链：

| 环节 | 官方 `verilog-format.cc` | `format_dump` |
| --- | --- | --- |
| stdout 二进制模式 | `_setmode(_fileno(stdout), _O_BINARY)` | 同 |
| 参数解析 | `verible::InitCommandLine` | 同 |
| 读文件 | `verible::file::GetContentAsString` | 同 |
| style 构造 | `InitializeFromFlags(&style)` | 同 |
| 诊断输出通道 | `control.stream = &std::cout` | 同 |
| 格式化 | `FormatVerilog(text, name, style, stream, lines, control)` | 同 |

因此「默认参数一致」与「输出字节一致」是**构造上保证**的，而不是靠人工逐项对齐。
**一旦有差异，那一定是移植本身的问题。**

这个设计决定与阶段 0（token 门禁）、阶段 1（lint 门禁）完全同构。
如果自己重写打印器，"格式一致"只能证明打印器写对了，证明不了 Verible 移植对了。

### 3.1 归一化只做路径前缀

输出里的文件名会出现在诊断消息中（出错场景）。对比脚本只把**语料根路径**折叠成
`<corpus>`，其余**一个字节都不动**——不做换行归一化、不做空白折叠。

理由：换行与空白正是 formatter 的输出本身。归一化它们等于把要验证的东西抹掉。

---

## 4. 语料

复用阶段 0 / 阶段 1 的同一份 `tests/gate-corpus/`：**102 个文件**

| 子集 | 文件数 | 说明 |
| --- | --- | --- |
| `corpus_alpha/` | 21 | 真实 Anlogic 工程（20 个 `.v` + IP 生成的 `pll_gen.v`） |
| `verible_lint_testdata/` | 73 | SystemVerilog 语法特性覆盖 |
| `error-cases/` | 8 | 故意语法错误（验证 failsafe 路径） |

`.vhd` 不参与（Verible 不支持 VHDL）。

---

## 5. 结果

```
官方端点 : C:/tools/verible\verible-verilog-format.exe
我方端点 : Y:\fpga-lsp-cpp\build-fmt\bin\format_dump.exe
语料     : Y:\fpga-lsp-cpp\tests\gate-corpus
文件数   : 102

==========================================================================
共 102 个文件，一致 102 个，不一致 0 个，一致率 100.00%
对比输出总字节: 82325
==========================================================================
```

`ctest -L formatgate` → **2/2 通过**（`formatgate_corpus` 4.49s、`formatgate_selftest` 0.43s）。

### 5.1 三类证据（不要混读）

| # | 来源 | 结果 | 证明什么 |
| --- | --- | --- | --- |
| **A** | 官方 vs 官方 | 一致 | 采集与归一化**确定性** |
| **B** | 官方 vs `format_dump` | **102/102 一致** | **门禁结论** |
| **C** | 只改我方 `--indentation_spaces=7` | **发现差异** | 脚本**灵敏**，不是宽容匹配 |

**A 单独存在时没有意义**（只证明两次跑同一工具的输出相同）；
**B 单独存在时也危险**（一个根本没在比较的脚本同样会报"全部一致"）。
只有 **C 通过**，B 才可信。

> ⚠ C 的变异必须**只作用于一侧**。两边同时改 flag 会让输出"一起变"、
> 一致率仍然 100%，那是**假绿灯**。阶段 1 lint 门禁踩过这个坑
> （见 `LINT-GATE-REPORT.md` §3.4），这里刻意避免。

### 5.2 局部（选区）格式化 —— 官方 `--lines N-M`

编辑器"格式化选区"走这条路径，与全文档格式化**是不同代码路径**，必须单独验证。

```
共 194 个区间用例，一致 194 个，不一致 0 个，一致率 100.00%
```

每个文件挑 3 个 1 基闭区间：开头 10 行 / 中间一段 / 末尾 10 行
（覆盖"区间贴文件起点""两端都不贴边""贴文件末行"三类边界）。

`ctest -L formatgate` → **4/4 通过**
（`formatgate_corpus` 4.69s、`formatgate_selftest` 0.41s、
`formatgate_range` 8.44s、`formatgate_range_selftest` 0.29s）。

#### 官方 `--lines` 的真实语义（容易误解）

实测：`--lines 5-10` 对一个 27 行的文件输出 **37 行（整个文件）**，不是只输出第 5-10 行。

官方实现是把 `--lines` 用 `verible::ParseInclusiveRanges(...)` 解析成 `LineNumberSet`，
再传给 `FormatVerilog(text, name, style, stream, lines_to_format, control)`——
**输出仍是全文，只是"仅这些行参与格式化决策"**。

`FormatVerilogRange`（只输出区间那一段、且不做收敛性检验）是**另一个 API**，
编辑器里的"格式化选区"才会用它。本门禁对齐的是**官方 CLI**，所以走
`lines_to_format` 这条。

> **因此本次通过的是「CLI `--lines` 语义的等价」，不是 `FormatVerilogRange` 的等价。**
> 后者仍列为 §8 未覆盖项。

#### 本节新增的第 5 个坑（见 §7 坑 5）

第一版手工 `lines_to_format.Add(Interval(range_start - 1, range_end))`，
194 个用例里 **58 个不一致（一致率仅 70.10%）**，症状是"单行区间下我方完全不格式化"。
改用官方的 `ParseInclusiveRanges` 后 100%。

---

## 6. 复现步骤

```powershell
# 1) 构建（私有目录，避免与其他人共享 build/ 冲突）
cmake --preset windows-msvc-release -B build-fmt
cmake --build build-fmt --target format_dump

# 2) 变异测试（先证脚本灵敏）
python tests\compare_format.py --selftest

# 3) 全语料门禁（主判据）
python tests\compare_format.py

# 4) 或走 CTest
ctest --test-dir build-fmt -L formatgate --output-on-failure
```

`compare_format.py` 自动探测 `build-fmt\bin` 与 `build\bin`；
也可用 `--ours <path>` 或环境变量 `FPGALSP_FORMAT_DUMP_EXE` 指定。

---

## 7. 实施过程中踩到的 6 个坑（全部是"报告成功但结果错"类）

这四条是本次移植最有价值的副产品，全部记在
`src/tools/format_dump/main.cpp` 的头部注释里（代码旁边比文档更可能被读到）。

### 坑 1（★ 最关键）stdout 必须是二进制模式

Windows CRT 默认把 stdout 当文本流，**写 `\n` 会自动变成 `\r\n`**。

| 版本 | 输出字节 |
| --- | --- |
| 不设 `_O_BINARY` | 510 B（CRLF） |
| 设了 `_O_BINARY` | 478 B（LF，与官方一致） |

首个差异在 offset 18：官方 `0x0A`，我方 `0x0D`。

**这与本项目 LSP 传输层踩到的 `\r\r\n` 是同一类问题**：CRT 文本模式会静默改写字节，
不报任何错。**任何向 stdout 写协议/精确文本的原生程序都必须先关掉这个转换。**

### 坑 2：`ExecutionControl::max_search_states` 的结构体默认值 ≠ CLI 默认值

| 来源 | 值 |
| --- | --- |
| `ExecutionControl` 结构体默认 | `10000` |
| 官方 CLI `--max_search_states` 默认 | **`100000`** |

官方在 `formatter_control` 里显式从 flag 赋值。我们必须照做，否则遇到需要大量搜索的
换行决策时会提前报 "search space exceeded"。

**这类差异看起来像"格式化算法不同"，极难归因。**

### 坑 3：`--failsafe_success` 默认 `true`，出错也返回 0

第一版在格式化失败时 `return 1`。结果：stdout **完全相同**（225 B），但退出码
官方 0 / 我方 1。

官方失败路径是 `return absl::GetFlag(FLAGS_failsafe_success);`（默认 true）。

**教训：门禁不只要比 stdout，还要比退出码。** 只比 stdout 会漏掉这类差异。

### 坑 4a（★ 局部格式化）手工构造 `LineNumberSet` 与 `ParseInclusiveRanges` 不等价

做 §5.2 时第一版这样写：

```cpp
lines_to_format.Add(verible::Interval<int>(range_start - 1, range_end));
```

自认为"1 基闭区间 → 0 基半开"换算对了。结果：

```
共 194 个区间用例，一致 136 个，不一致 58 个，一致率 70.10%
```

症状（单个用例）：

```
输入   `    a <= b; // [Style: ...]`
官方   `    a <= b;  // [Style: ...]`   ← 规范化了空格
我方   `    a <= b; // [Style: ...]`    ← 原样不动
```

**单行区间（`--lines 3-3`）下我方完全不格式化，而多行区间碰巧一致**——
这让问题看起来像"偶发"，其实是最简单的边界条件错了。

改成调用官方**同一个**解析函数后 100%：

```cpp
std::vector<std::string> line_ranges;
line_ranges.push_back(std::to_string(range_start) + "-" +
                      std::to_string(range_end));
verible::ParseInclusiveRanges(&lines_to_format, line_ranges.begin(),
                              line_ranges.end(), &std::cerr, '-');
```

**教训（比这条 bug 本身重要）：上游提供了"解析这个参数"的函数，就一定用它。**
自己重新推导边界语义是 off-by-one 的温床，而且症状往往只在极端输入下暴露
（单个元素、首尾、空集）。

这与本项目另外两条同类教训一致：
* 坑 1 的 `\r\n`（不自己推导换行处理，关掉 CRT 转换）
* 阶段 0 的 G5（不自己推导 `#endif` 配对，用深度计数）

### 坑 4b：变异测试的"变异"必须作用到有效内容上

`formatgate_range_selftest` 的 [C] 项，第一版用 `range_end + 3` 做变异 →
"没发现差异"。改用 `--indentation_spaces=7` → 仍然"没发现差异"。

**两次都不是脚本不灵敏，而是变异本身没改变输出**：
* 多出来的 3 行本来就是规范格式
* 挑的区间是端口列表，其缩进由 `wrap_spaces` / `*_indentation` 控制，
  `indentation_spaces` 对它无效

最终改成"range 覆盖整个文件 + `--indentation_spaces=8`"——嵌套块体一定受影响，
输出必然不同。

**教训：变异测试要证明的是"脚本能发现差异"，所以必须保证变异确实产生了差异。**
否则会把"测试设计错误"误判成"门禁失效"，甚至可能反过来去削弱断言。

### 坑 5：`InitCommandLine` 返回的 vector **包含程序名**

`file_args[0] == argv[0]`，与官方 `if (file_args.size() == 1)` → 打印 usage 的写法一致。
第一版按"不含程序名"处理，导致把文件路径当成程序名、报"收到 2 个文件"并输出 0 字节。

---

## 8. 本门禁**没有**验证的范围（重要，防止误读）

以下**未验证**，不要因为本报告写着"通过"就认为 formatter 全功能等价：

1. ~~**`--lines` 局部格式化**~~ → **已补，见 §5.2**（194 个区间用例 100% 一致）。
   ⚠ 但要注意区分两个东西：
   * **CLI `--lines N-M`**（= `FormatVerilog` + 非空 `LineNumberSet`，输出全文）→ **已验证** ✅
   * **`FormatVerilogRange`**（只输出区间那一段、不做收敛性检验）→ **仍未验证** ❌
   编辑器的"格式化选区"可能走后者，需单独确认（作为后续任务）
2. **`--inplace` 文件写回** —— 我们不写文件，只输出到 stdout
3. **`--verify` 只检模式**（返回码语义：0 表示无需改动、1 表示需要）
4. **`max_search_states` 触发的错误路径** —— 语料里没有需要超大搜索空间的极端长行
5. **`line_terminator=CR / CRLF` 显式模式** —— 只验证了默认 `auto`
6. **非 UTF-8 编码的输入文件**
7. **`--format_debug` 家族**（`show_token_partition_tree` 等调试输出）
8. **语料规模**：102 个文件里 73 个是平均 121 字节的测试片段；真实工程只有 21 个
9. **Windows 以外的平台**

第 1 条现在拆成两半：CLI `--lines` 已验证，`FormatVerilogRange` 未验证。
后者才是 VS Code "Format Selection" 可能调用的 API，**不要因为 §5.2 通过就认为选区格式化已完全覆盖**。

---

## 9. 顺带发现的一个构建问题（已在代码里记录）

`format_dump` **不能只依赖 `verible_formatter_libs` 的 INTERFACE 传递**：

`verible_parser_libs` 已经带了 `/WHOLEARCHIVE` 选项，但 `format_dump` 额外引入了
formatting 归档后，链接顺序变化，MSVC 在 `/WHOLEARCHIVE` 生效前就按"无人引用"
丢掉了 `.tab.cc` / flex 生成的成员，报：

```
LNK2001: verilog::verilog_parse / verilog_debug / verilog_symbol_name
LNK2019: verilogFlexLexer::yy_pop_state / VerilogLexer::yylex
```

处置：在 `src/tools/format_dump/CMakeLists.txt` 里对本 target **再声明一次**
"link 一次（建立构建依赖，避免 LNK1356）+ `/WHOLEARCHIVE`"。

**只 link `verible_parser_libs` 的 `cst_dump` 不触发这个问题**——
这个坑只在引入 formatter 后才出现，所以必须记在这里。

---

## 10. 一句话总结

**formatter 链路与 parser / lint 一样，CMake 化后与官方二进制逐字节一致（102/102）。
所有麻烦都出在"装配"层（stdout 文本模式、默认值不一致、链接顺序、退出码语义），
没有一处是 Verible 的格式化逻辑本身。**

这与阶段 0 / 阶段 1 的结论完全一致，进一步印证了项目 README 里的风险画像：
**风险不在语义漂移，在构建装配。**