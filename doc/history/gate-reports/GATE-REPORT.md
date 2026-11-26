# 阶段 0 门禁报告：CMake 化 Verible 与官方二进制的 token 一致性

> **结论：✅ 通过（PASS）**
>
> 102 个语料文件全部一致，token 序列一致率 **100.00%**（18,815 token，0 不一致）。
> 附加验证：CST 结构（`--tree`）102 文件与官方**逐字节一致**；
> 错误恢复行为（12 条错误消息）逐条一致。
>
> 本文件由 gate-verify（任务 0-E）维护。测得数据时间：2026-11-18。
> 详细数据见 §5，方法论见 §2–§3，**未覆盖范围见 §8（请务必阅读，避免误读结论）**。

---

## 1. 门禁目标

阶段 0 是整个 C++ 重写方案的前提判定点。本门禁要回答一个二元问题：

> **把 vendored Verible（commit `c6b29d62`）从 Bazel 构建改造成 CMake + MSVC
> 构建之后，它的 Verilog/SystemVerilog 解析行为是否与官方发行的二进制完全一致？**

判定口径（来自任务书）：

| 条件 | 结论 |
| --- | --- |
| token 序列逐文件一致率 **100%** | 阶段 0 **通过**，方案可继续 |
| 存在**解析行为差异** | 阶段 0 **不通过**，转入止损评估 |
| 仅**输出格式差异** | 允许在脚本中显式过滤并说明，但必须在报告中透明列出 |

**为什么用 token 序列而不是 CST 树做判据**：token 序列是解析器的第一手输出，
它决定了 CST。lexer 是 flex 生成的（受生成器版本影响），parser 是 bison 生成的
（受语法文件和补丁影响），两者都最容易在移植中走样。token 一致是 CST 一致的必要条件，
且对比结果可精确到"第几个 token、哪个字段不同"，比整树 diff 更利于定位根因。

> 注：本门禁实际**额外**验证了 CST 结构一致（§5.3），但那是附加证据，
> 不改变上面的主判据口径。

---

## 2. 参与对比的两个端点

### 2.1 基准（Ground Truth）

| 项 | 值 |
| --- | --- |
| 可执行文件 | `C:/tools/verible\verible-verilog-syntax.exe` |
| 调用方式 | `verible-verilog-syntax --printrawtokens <file>` |
| 语义 | 输出**全部**词法 token（含被语法树过滤掉的空间/换行/注释） |

### 2.2 被测（本仓库 CMake 构建产物）

| 项 | 值 |
| --- | --- |
| 源码 | `src/tools/cst_dump/main.cpp` |
| 可执行文件 | `build/bin/cst_dump.exe` |
| 调用方式 | `cst_dump --rawtokens <file>` |
| 链接 | `verible_parser_libs`（`cmake/Verible.cmake` 定义的聚合 target） |

### 2.3 关键设计：对齐的是**代码路径**，不是输出格式

这一点决定了本门禁是否有意义。`cst_dump` **没有**自己重写一份 token 打印器，
而是逐项复用了官方工具 `verible/verilog/tools/syntax/verilog-syntax.cc` 的同一套实现：

| 环节 | 官方 `verilog-syntax.cc` | `cst_dump` | 是否相同 |
| --- | --- | --- | --- |
| 读文件 | `verible::file::GetContentAsMemBlock()` | 同左 | ✅ 同一函数 |
| 解析 | `VerilogAnalyzer::AnalyzeAutomaticMode()` | 同左 | ✅ 同一函数 |
| 预处理配置 | `VerilogPreprocess::Config{.filter_branches=true}` | 同左 | ✅ 逐字段相同 |
| token 枚举→名字 | `verilog::verilog_symbol_name()` | 同左 | ✅ 同一函数 |
| token 行渲染 | `TokenInfo::ToStream(stream, context)` | 同左 | ✅ 同一函数 |
| 错误消息 | `FileAnalyzer::LinterTokenErrorMessages()` | 同左 | ✅ 同一函数 |
| 语法树打印 | `verilog::PrettyPrintVerilogTree()` | 同左 | ✅ 同一函数 |

因此，`--rawtokens` 的输出理应与官方 `--printrawtokens` **逐字节相同**。
一旦出现差异，**只可能**来自下列可归因的来源，而这正是门禁要抓的东西：

1. CMake 化编译后代码本身的语义变化（MSVC vs 官方 clang 的 UB 暴露差异）；
2. 第三方依赖（abseil / re2 / nlohmann_json）版本差异；
3. flex / bison 生成器版本差异（官方 release 用的生成器版本与本机不一致）；
4. G6 文本补丁（`std_move_parser_symbols` / `record_recovered_syntax_errors`）失配。

> 这个设计是刻意的：如果 `cst_dump` 自己重新实现了一遍打印格式，
> 那么"格式一致"就只证明了我写的打印器没错，**证明不了 Verible 移植没错**。

### 2.4 关键技术说明：C skeleton + C++ 语义值类型（容易被误解）

**Bison 的 "skeleton"（解析器驱动代码的生成模板）与 "语义值类型" 是两件独立的事。**
这一点在本项目里非常重要，后人极容易误读：

```bison
%define api.value.type {::verible::SymbolPtr}   # 语义值类型 = C++ 的 unique_ptr
%define api.prefix {verilog_}
%param { ::verible::ParserParam* param }          # 把上下文以参数传入
```

| 事实 | 说明 |
| --- | --- |
| 生成的是 **C skeleton** | bison 生成传统 C 风格的 `yyparse(param)` 驱动 |
| 但 `YYSTYPE` = `verible::SymbolPtr` | 即 `std::unique_ptr<verible::Symbol>`，是**真正的 C++ 类**，不是 union |

因此 `yylval` / `yyval` 是 `unique_ptr` 类型的对象，而 C skeleton 生成的代码里
写的是**拷贝赋值**（`*++yyvsp = yylval;`）—— 这在 C++ 里是非法的，
因为 `unique_ptr` 的拷贝赋值已被 `delete`。

**这就是 G6a 补丁（`std_move_parser_symbols`）存在的原因**：把拷贝赋值改成移动赋值。
它不是"优化"或"风格问题"，而是**能不能编译的前提**。

> ⚠️ 常见误解：看到"C skeleton"就认为 `yylval` 是 union、`std::move` 多余。
> 这是错的。请以 `%define api.value.type` 为准。

**实测佐证**（用 win_bison 3.8.2 真实生成后对照）：

| 版本 | 编译结果 |
| --- | --- |
| 未打 G6a 补丁 | ❌ `error C2280`（`unique_ptr` 拷贝赋值被 delete）× 4 处 |
| 已打 G6a 补丁 | ✅ 0 error，产出 2.2 MB obj |

由此可得一个重要推论（已写入 §8 的风险表）：
**G6a 的正确性由编译器保证** —— 漏打任何一处必然编译失败，不可能静默产生错误行为。
相反 **G6b（`RecordSyntaxError`）漏打照样能编译**，只是错误恢复能力静默丢失，
这才是必须靠"对比官方错误消息"防护的风险点。

### 2.5 输入读取路径必须一致（已确认）

官方 `GetContentAsMemBlock()` 在 **Windows 上不做 mmap**（`AttemptMemMapFile`
的实现被 `#ifndef _WIN32` 包住），走的是 `fopen(filename, "rb")` 全量读入。
这一点很重要：二进制模式读入 → 文件里的 CRLF **原样保留** → lexer 的
`LineTerminator` 规则 `\r|\n|\r\n|\0` 把 `\r\n` 当成**一个** token。
若一边做了文本模式换行转换、另一边没有，token 边界会整体错位。
`cst_dump` 使用同一个 `GetContentAsMemBlock()`，因此不存在这个风险。

---

## 3. 对比方法（`tests/compare_cst.py`）

### 3.1 判定轴

脚本对每个文件做**两条独立的判定**：

1. **token 序列对比**（主要判据）
   逐条比较 `(kind, start_offset, end_offset, text)` 四元组，顺序敏感。
   **不做任何宽容匹配** —— offset 和转义文本都必须完全一致。

2. **错误消息对比**（辅助判据，默认开启 `--strict-errors`）
   对比词法/语法错误消息的行号、列范围和文本。
   作用：捕捉"token 序列一致但错误恢复行为不同"的隐蔽回退。
   典型场景 —— 若 G6b 的 `RecordSyntaxError` 补丁失效，
   token 仍然完全一致，但错误消息会消失，**只看 token 会漏掉**。

### 3.2 归一化规则（透明列出，均有依据）

| 归一化 | 做法 | 为什么要做 | 是否可能掩盖真实差异 |
| --- | --- | --- | --- |
| 行尾 CR | 去掉每行末尾的 `\r` | 子进程在 Windows 上可能以文本模式转发 stdout，产生 `\r\n` 甚至 `\r\r\n` | **不会**。Verible 把源文件里的 `\r` 用 `CEscape` 转义成字面字符序列 `"\\r"` 写在行中间，token 行正文从不以裸 `\r` 结尾。归一化只影响行分隔符 |
| 错误行路径前缀 | `Y:\path\file.v:1:8-13: msg` → `1:8-13: msg` | 两边可能以不同路径形式（相对/绝对）调用，路径不是解析行为 | **不会**。保留行号、列范围、消息全文，三者任一不同都会被抓到 |
| 空白行/标题行 | token 对比时跳过 `All lexed tokens:` 与空行 | 它们不是 token | **不会**。标题行本身在报告里单独核对（见 §2.3） |

**被刻意排除的归一化**（即"宁严勿宽"）：
- 不忽略大小写；
- 不忽略空白；
- 不重排 token；
- 不对 offset 做任何偏移修正；
- 不在 kind 名上做别名归一。

### 3.3 脚本自验证（重要：证明"一致率 100%"不是假象）

一个只会输出"全一致"的对比脚本毫无价值 —— 它可能只是坏在"什么差异都看不见"。
因此脚本经过三重验证，全部**实测**：

| # | 验证 | 做法 | 实测结果 |
| --- | --- | --- | --- |
| 1 | **自比测试** | 官方二进制 vs 官方二进制（`--selftest`） | **102 文件 / 18,815 token / 100.00% 一致** —— 证明归一化逻辑不会把不同内容误判为相同 |
| 2 | **变异测试 1：token 篡改** | 把某文件一个 `SymbolIdentifier` 改成 `SymbolIdentifierX` | 精确报出 `token mismatch at index 29` 并打印前后各 2 条上下文，**且只有该文件**被判不一致（其余 101 个仍判一致） |
| 3 | **变异测试 2：错误消息篡改** | 把 `lexical error` 改成 `parse error`（token 保持完全一致） | token 轴全过，但脚本仍在**错误消息轴**判不一致，并指出 `1:8-13: lexical error` vs `1:8-13: parse error` |

实测输出样例（变异测试 1）：

```
token mismatch at index 29 (official=3468 tokens, cst_dump=3468 tokens)
  [27] official: (#"module" @735-741: "module")
  [27] cst_dump: (#"module" @735-741: "module")
  [28] official: (#"<<space>>" @741-742: " ")
  [28] cst_dump: (#"<<space>>" @741-742: " ")
  [29] official: (#SymbolIdentifier @742-745: "top")
  [29] cst_dump: (#SymbolIdentifierX @742-745: "top")  <<<
  [30] official: (#'(' @745-746: "(")
  [30] cst_dump: (#'(' @745-746: "(")
```

**结论：脚本能发现单 token 级别、单条错误消息级别的差异，因此它的"通过"结论可信。**
且变异测试确认了它**不会**因为一个文件的差异而误报其它文件（无误报）。

---

## 4. 语料

### 4.1 语料构成

语料统一放在 `tests/gate-corpus/`（从原始位置**复制**而来，不污染原工程目录）。

| 子目录 | 来源 | 文件数 | 字节数 | 说明 |
| --- | --- | --- | --- | --- |
| `corpus_alpha/` | `C:/work/CorpusAlpha` | 21 (`.v`) | 75,064 | 真实 Anlogic TangDynasty 工程，Verilog-2001/2005 风格 |
| `verible_lint_testdata/` | `thirdparty/verible/verible/verilog/tools/lint/testdata` | 73 (`.sv`) | 8,865 | SystemVerilog 特性覆盖：class / covergroup / 约束 / 宏 / 属性 / UDP 等 |
| `error-cases/` | 本任务手工构造 | 8 (`.sv`) | 987 | **故意语法错误**，验证错误恢复（G6b） |
| **合计** | | **102** | **84,916** | token 总数 **18,815** |

### 4.2 语料覆盖度评估

- **真实工程**（21 文件）覆盖了 Verilog 的主干语法：module 声明、端口列表、
  always/assign、位宽、拼接、generate、参数化实例、case、函数任务等。
  这是"能不能用于 Anlogic 工程"的直接证据。
- **SystemVerilog 特性**（73 文件）提供了 SV 语法面的广度覆盖，
  包括错误文件 `bad-id-lex.sv`（故意的词法错误，用于验证错误路径一致性）。
- **错误恢复路径被覆盖**：`bad-id-lex.sv` 会产生
  `lexical error at token "654foo"`，官方仍输出已识别的 3 个 token 且**退出码为 1**。
  这验证了"解析失败时仍输出已识别 token + 非零退出码"的约定。

### 4.3 语料中含错误文件（非 bug，是特性）

| 文件 | 官方行为 |
| --- | --- |
| `verible_lint_testdata/bad-id-lex.sv` | 1 条 lexical error；输出 3 个 token（`module` / 空格 / `TK_OTHER "654foo"`）；退出码 1 |

该文件被**刻意保留**在语料中：它同时验证了
(1) lexer 对非法标识符的处理，(2) 出错后仍输出 token，(3) 退出码语义，
(4) 错误消息文本一致性。剔除它反而会降低门禁强度。

### 4.4 被明确排除的语料

| 排除项 | 原因 |
| --- | --- |
| `corpus_alpha/.fpgalsp/logs/`（server.log 等） | 语言服务器运行日志，与 CST/token 无关 |
| `corpus_alpha/**/pll_gen.vhd` | VHDL，不归 Verible 管，且不是 token 对比对象（保留在原工程，未纳入对比） |
| `corpus_alpha/*.al` / `*.sdc` / `*.adc` / `*.cfg` | TD 工程配置与约束文件，非 HDL 源码 |
| `%PYREF%\tests\fixtures` | **该目录不存在**（实测），已跳过 |
| `Y:\verible-src-probe\verible` | **该目录存在但为空**（实测：`Y:\verible-src-probe` 是空目录，其中没有 `verible/` 子目录、0 个文件），已跳过 |
| Verible 自带 `*_test.cc` 内嵌 testdata 字符串 | 不是独立文件；其覆盖的语法面已被 lint testdata 部分覆盖 |

> 语料清单与文件大小的机器可读版本见 `tests/gate-corpus/` 目录本身；
> 逐文件明细（token 数、一致与否）见对比脚本产出的 `build/gate-report.json`。

---

## 5. 对比结果

### 5.1 主结果（token 序列）

| 指标 | 值 |
| --- | --- |
| 语料文件数 | **102** |
| 一致文件数 | **102** |
| 不一致文件数 | **0** |
| **一致率** | **100.00%** |
| 对比 token 总数（以官方为准） | **18,815** |
| 行尾归一化生效行数 | 38,064（仅 CRLF→LF 的行文本，不改 token 内容） |

被测产物：`build/bin/cst_dump.exe`（1,208,832 B，MSVC 19.52 / Release / 静态 CRT）
基准产物：`C:/tools/verible\verible-verilog-syntax.exe`

> 说明：门禁结论在 `build/`（工程正式构建目录）与独立验证目录 `build_gate/` 两处
> **各跑一次，结果完全相同**（均为 102/102、100.00%），排除构建目录偶然性。

### 5.2 分类明细

| 语料子集 | 文件数 | token 数 | 一致 | 说明 |
| --- | --- | --- | --- | --- |
| `corpus_alpha/` | 21 | 16,496 | 21 | 真实 Anlogic 工程（Verilog-2001/2005） |
| `verible_lint_testdata/` | 73 | 1,899 | 73 | SystemVerilog 特性覆盖面 |
| `error-cases/` | 8 | 420 | 8 | 故意语法错误，验证错误恢复 |
| **合计** | **102** | **18,815** | **102** | |

### 5.3 附加验证（超出主判据，均为 100%）

| 验证项 | 命令 | 结果 |
| --- | --- | --- |
| **CST 结构与官方一致** | `cst_dump --tree` vs 官方 `--printtree`，全 102 文件逐字节 diff | **102/102 一致** |
| **错误消息逐条一致** | 8 个错误用例，官方基线 12 条错误 | **8/8 一致，12 条消息全对** |

### 5.4 错误恢复专项明细（验证 G6b 补丁未静默失效）

这是最容易被忽略、也最致命的一层。若 `RecordSyntaxError` 补丁失效，
**token 序列仍然完全一致**，只有错误消息会消失。

| 文件 | 官方错误数 | cst_dump 错误数 | token 数 | 一致 |
| --- | --- | --- | --- | --- |
| `e01_missing_endmodule.sv` | 1 | 1 | 87 | ✅ |
| `e02_extra_comma_in_portlist.sv` | **3** | **3** | 52 | ✅ |
| `e03_missing_semicolon_in_always.sv` | 1 | 1 | 31 | ✅ |
| `e04_case_without_endcase.sv` | 1 | 1 | 87 | ✅ |
| `e05_incomplete_ternary.sv` | 1 | 1 | 47 | ✅ |
| `e06_unclosed_begin_end.sv` | 1 | 1 | 50 | ✅ |
| `e07_unterminated_string.sv` | 1 | 1 | 14 | ✅ |
| `e08_multiple_errors.sv` | **3** | **3** | 52 | ✅ |
| **合计** | **12** | **12** | 420 | ✅ |

**关键点：不是"0 == 0"的一致。** 实测样例（`e02`，3 条错误逐字相同）：

```
official : e02_...sv:3:3: syntax error at token ","
           e02_...sv:7:3-8: syntax error at token "assign"
           e02_...sv:8:1-9: syntax error at token "endmodule"
cst_dump : e02_...sv:3:3: syntax error at token ","
           e02_...sv:7:3-8: syntax error at token "assign"
           e02_...sv:8:1-9: syntax error at token "endmodule"
退出码   : official=1, cst_dump=1
```

### 5.5 CTest 集成

```
$ ctest -L gate
1/2 Test #1: gate_cst_dump_token_consistency ...   Passed    4.22 sec
2/2 Test #2: gate_cst_dump_error_recovery ......   Passed    0.45 sec
100% tests passed out of 2
```

---

## 6. 不一致明细与根因分析

### 6.1 对比中发现的不一致

**主判据（token 序列）与附加判据（CST / 错误消息）均为 0 处不一致。**

没有任何需要归因为"输出格式差异"或"调用路径差异"的项 —— 因为 `cst_dump`
从一开始就复用了官方的同一套代码路径（§2.3），所以连格式差异都不存在。

### 6.2 过程中发现并修复的**实现缺陷**（不是解析差异，但值得记录）

这些是在门禁准备期间发现的问题，均已修复，**不影响最终结论**：

| # | 问题 | 影响 | 状态 |
| --- | --- | --- | --- |
| 1 | `cst_dump --json` 的 `kind` 字段裸输出，遇到含双引号的 token 名（如 `"module"`）会破坏 JSON 合法性 | 仅影响 `--json`，不影响门禁判据 | ✅ 已修：改用 `TokenTypeToString()` 并做 JSON 转义，同时与官方 `--export_json` 的 `tag` 语义对齐（`TK_SPACE` 而非 `<<space>>`） |
| 2 | `GetTokenStreamView()` 元素类型是 `TokenSequence::const_iterator`，原代码按指针解引用 | 编译错误 | ✅ 已修 |
| 3 | CTest 门禁用例注册在 `src/tools/cst_dump/` 时 `ctest -N` 显示 0 个 | 用例不可见 | ✅ 已修：移到 `tests/CMakeLists.txt` 注册（根因见 §6.3） |

### 6.3 构建链路中发现的**移植缺陷**（已报给 0-B，不属门禁判据但影响可复现性）

| # | 问题 | 根因 | 状态 |
| --- | --- | --- | --- |
| A | `verilog-y-cc` 被定义为 `INTERFACE` 库 | Bazel 原定义为 `cc_library(srcs=["verilog-final.tab.cc"])`，转译时丢了源文件，导致 `verilog_parse` / `verilog_symbol_name` / `verilog_debug` 未定义 | ✅ 已由 verible-build 修为 STATIC |
| B | `ABSL_MSVC_STATIC_RUNTIME` 默认 `OFF` | absl 编成 `/MD`，项目是 `/MT` → `LNK2038` 大量不匹配 | ✅ 通过 `-DABSL_MSVC_STATIC_RUNTIME=ON` 解决；建议进 `Dependencies.cmake` 固化 |
| C | `VERIBLE_FLEX_LEXER_INCLUDE_DIR` 未自动填充 | `verilog-lexer.h` 找不到 `FlexLexer.h` | ⚠️ 依赖手工传入，建议 0-D 自动探测 |
| D | CTest 在 `src/` 分支不可见 | 顶层 `add_subdirectory(src)` 在 `include(CTest)` **之前**，`src/` 不生成 `CTestTestfile.cmake` | ✅ 已绕过（注册移到 `tests/`），但顶层顺序仍建议修正 |

### 6.4 已知"暂存差异"（Lead 要求显式标注）

| 项 | 现状 | 对门禁的影响 |
| --- | --- | --- |
| **G5 `verilog-parse-interface.h`** | 锚点 `YYTOKENTYPE` 在 bison 3.8.2 下命中 **0 次**（实际宏名是 `VERILOG_TOKENTYPE`，出现 4 次），因此该文件目前**未生成** | **无影响**。实测：全仓库**没有任何源码** `#include` 这个文件（grep 结果 0 处），它是纯 Bazel 中间产物。`cst_dump` 不依赖它 |
| **`command-file.yy.cc`** | 本门禁运行前手工生成（`win_flex`），产物 50,195 B | **无影响**。它只服务于 `common/analysis` 包，通过 `/WHOLEARCHIVE` 参与链接，不改变 token 序列 |

> 结论：这两项都不阻塞门禁。但**正式生成器（`cmake/scripts/verible-generators.cmake`）
> 交付后应复跑一次门禁**，以证明"正式生成物"与"本次手工产物"等价。

---

## 7. 门禁结论

### 7.1 判定

> ## ✅ **阶段 0 通过（PASS）**

依据：

1. **token 序列逐文件一致率 100.00%**（102/102 文件，18,815 token，0 不一致）—— 满足任务书"100% 一致"的通过条件；
2. **错误恢复行为一致**（12/12 条错误消息逐字相同，含行号与列范围）；
3. **CST 结构一致**（102/102 文件 `--tree` 输出逐字节相同，属附加证据）；
4. 无任何归因为"输出格式差异"或"调用路径差异"的项。

### 7.2 根因分类（任务书要求的"不通过时的根因"）

| 类别 | 本次出现次数 |
| --- | --- |
| 输出格式差异 | **0** |
| 调用路径差异 | **0** |
| 真实解析行为差异（lexer / parser 语义） | **0** |

### 7.3 对"止损评估"的回答

任务书规定"存在解析行为差异 → 不通过，转入止损评估"。
**本次未发现任何解析行为差异，因此不需要触发止损评估，C++ 重写方案可以继续进入阶段 1。**

### 7.4 结论的适用范围（强烈建议连同 §8 一起读）

"通过"的准确含义是：

> 在 **102 个语料文件**上，**raw token 序列**、**错误消息**、**CST 结构**
> 三个维度与官方发行二进制**完全一致**。

它**不**代表：
- 所有 Verilog/SystemVerilog 语法都已覆盖（语料有限，见 §8）；
- lint / formatter / preprocessor 深展开已验证（那是阶段 1）；
- 构建编排已完全自动化（见 §6.3、§6.4）。

### 7.5 遗留事项（不阻塞本门禁，建议跟进）

| 优先级 | 事项 | 责任 |
| --- | --- | --- |
| 高 | 正式生成器交付后**复跑门禁**，交叉验证手工产物 ≡ 正式产物 | flexbison-gen + gate-verify |
| 中 | 把 `ABSL_MSVC_STATIC_RUNTIME=ON`、`VERIBLE_FLEX_LEXER_INCLUDE_DIR` 固化进 CMake，去掉手工参数 | verible-build / flexbison-gen |
| 低 | 顶层 `add_subdirectory(src)` 移到 `include(CTest)` 之后 | cmake-scaffold |

---

## 8. 本门禁**没有**验证的范围（重要，防止误读）

"阶段 0 通过"只意味着**CMake 化后的 Verible 在下列已测维度上与官方二进制一致**。
它**不**意味着"Verible 全功能已验证"。明确划出边界：

### 8.1 未验证项

| 未验证 | 说明 | 归属阶段 |
| --- | --- | --- |
| **CST 节点类型序列逐节点一致** | ⚠️ **本项已部分覆盖**：门禁主判据是 token+错误消息，但附加验证里用 `cst_dump --tree` 对全 102 文件与官方 `--printtree` 做了**逐字节 diff，结果 102/102 完全一致**（§5.3）。因此"CST 一致"实际上已被强证据支持。剩余未覆盖的是：**未对超大规模/未入库的工程做 CST 对比** | 阶段 0 **已覆盖（102 文件）**，建议阶段 1 扩到更多真实工程 |
| **lint 规则行为** | `verible-verilog-lint` 的 200+ 条规则、规则配置、豁免 —— 完全未验证 | 阶段 1 |
| **formatter 行为** | `verible-verilog-format` 的排版算法与对齐 —— 完全未验证 | 阶段 1 |
| **preprocessor 展开后的结果** | 官方 `--printrawtokens` 输出的是**原始 token**。`Config{.filter_branches=true}` 已对齐，语料中的宏也走了同一路径，但没有专门构造复杂宏展开 / `` `include `` 展开 / `` `ifdef `` 嵌套裁剪的用例 | 阶段 0 **部分覆盖** |
| **命令行工具级行为** | 多文件处理、`--error_limit`、`--verifytree` 等官方工具特性。`cst_dump --json` 是**近似**官方 `--export_json`（字段名不同），**不作为门禁判据** | 不需要 |
| **性能/内存** | 解析速度、大文件/超大文件表现 | 阶段 1+ |
| **非 Verilog 输入** | VHDL（.vhd）、TD 约束文件（.sdc/.adc/.al） | 不属于 Verible |
| **MSVC 与官方编译器的潜在 UB 差异** | 官方 release 用 clang 构建，本仓库用 MSVC。理论上 UB 暴露差异可能在边界输入上分叉。102 文件语料未触发任何差异，但**不能证明在最极端输入下必然不分叉** | 无法完全排除 |
| **错误恢复的极端场景** | 已用 8 个手写错误用例覆盖常见错误类型（12 条消息）。未覆盖：极深嵌套导致的栈溢出、超长错误级联、lexer 状态机的罕见分支 | 阶段 0 部分覆盖 |

### 8.2 语料规模的真实局限（必须诚实说明）

本结论基于 **102 个文件 / 18,815 token / 83,929 字节**。其中：
- **真实工程只有 21 个文件**（CorpusAlpha，共 75 KB）—— 这是唯一代表"实际使用场景"的部分；
- 其余 73 个是**很小的** SystemVerilog 测试片段（平均 121 字节/文件）。

**因此"100% 一致"的强度主要来自 21 个真实文件和 18,815 个 token 的规模，
而非文件数量。** 若要更强的保证，阶段 1 应把语料扩到千行级、多模块、
含复杂宏与 generate 的真实工程。

### 8.2 关于"token 一致 ⇒ CST 一致"的推理边界

本报告主张 token 序列一致是 CST 一致的**必要条件**，这是成立的：
bison 解析器的每一步动作都由 `(state, token)` 决定，token 流相同且语法文件相同
（都是 vendored 的同一份 `verilog.y`），则产生的语法树必然相同。

但这个推理有一个前提：**语法文件本身没有被改动**。本仓库对 `verilog.y`
只做构建系统适配（不改 `%token` / 产生式）。若将来有人改动语法，
上述推理失效，必须补逐节点 CST 对比。

### 8.3 建议的阶段 1 补充验证

1. 用 `cst_dump --tree` 与官方 `--printtree` 做全语料逐行 diff（本工具已支持，只是未纳入门禁判据）；
2. 专门验证 preprocessor 展开路径（例如对含复杂宏的真实工程跑 `--printpreprocessed` 类对比）；
3. 用更大的真实工程语料扩展（当前 21 个真实文件，建议阶段 1 增到 100+ 文件）。

---

## 9. 复现步骤

任何人可以按下列步骤重现本报告的结论。

### 9.1 前置条件

| 项 | 路径 / 版本 |
| --- | --- |
| 官方基准 | `C:/tools/verible\verible-verilog-syntax.exe` |
| win_bison | 3.8.2（`C:/Users/example\scoop\apps\winflexbison\current\win_bison.exe`） |
| win_flex | 2.6.4（同目录 `win_flex.exe`） |
| MSVC | Visual Studio 18 Community，`cl` 19.52 |
| Python | 3.x（脚本用 stdlib，无第三方依赖） |

### 9.2 构建 cst_dump

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

cmake -S . -B build -G Ninja ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DFPGA_LSP_VERIBLE_ENABLED=ON ^
  -DFPGA_LSP_BUILD_THIRDPARTY_IN_ALL=OFF ^
  -DABSL_MSVC_STATIC_RUNTIME=ON ^
  -DVERIBLE_FLEX_LEXER_INCLUDE_DIR="C:/Users/example/scoop/apps/winflexbison/current"

cmake --build build --target cst_dump
:: 产物: build/bin/cst_dump.exe
```

> ⚠️ 在上述 `cmake --build` 之前，需要 `build/verible/gen/` 下已存在
> flex/bison 生成物（正式生成器就绪后由 CMake 自动产出）。
> 本报告测得结果时的生成物来源见 §6.4。

### 9.3 跑门禁

```bat
:: 方式一：CTest（推荐）
ctest --test-dir build -L gate --output-on-failure

:: 方式二：直接调用脚本（可拿到 JSON 报告）
python tests\compare_cst.py ^
  --cst-dump build\bin\cst_dump.exe ^
  --official C:/tools/verible\verible-verilog-syntax.exe ^
  --corpus   tests\gate-corpus ^
  --pattern  ".v,.sv" ^
  --report   build\gate-report.json
```

**通过判据：输出 `一致率 100.00%` 且退出码为 0。**

### 9.4 单独跑错误恢复专项

```bat
python tests\compare_cst.py ^
  --cst-dump build\bin\cst_dump.exe ^
  --official C:/tools/verible\verible-verilog-syntax.exe ^
  --corpus   tests\gate-corpus\error-cases ^
  --pattern  ".sv"
```

### 9.5 验证对比脚本本身没坏（每次改动脚本后都该跑）

```bat
:: 自比：官方 vs 官方，必须 100%
python tests\compare_cst.py --selftest ^
  --cst-dump C:/tools/verible\verible-verilog-syntax.exe ^
  --official C:/tools/verible\verible-verilog-syntax.exe ^
  --corpus   tests\gate-corpus --pattern ".v,.sv"
```

---

## 10. 一句话总结

> CMake + MSVC 构建的 Verible 在 102 个语料文件（18,815 token）上与官方发行二进制
> **token、CST、错误消息三个维度完全一致**，**阶段 0 门禁通过**，C++ 重写方案可以进入阶段 1。
> 该结论的边界见 §8 —— 它验证的是**解析行为等价**，不是 Verible 全功能等价。