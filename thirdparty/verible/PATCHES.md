# PATCHES.md —— 对 vendored Verible 的上游补丁清单

本文件记录**对 `thirdparty/verible/` 下上游 Verible 源码做过的所有修改**。
原则：非必要不改上游；确需修改时必须是**最小补丁**，并在此登记（文件 / 行 / 原因 / 补丁内容）。

Verible vendored commit：`c6b29d62`（详见同目录 `VENDOR-INFO.md`）。

---

## 一、上游 `.cc` / `.h` 源码补丁（业务逻辑）

### 结论：**34 处 / 17 个文件**（全部是同一类修复）

> ⚠ 本表在 2026-11-26 之前写的是"0 条" —— 那是**错误陈述**。当时确实没改上游，
> 但后来为了在 **MSVC Debug**（`/MTd`，`_ITERATOR_DEBUG_LEVEL=2`）下能解析 Verible，
> 对上游做了语义等价的必要修改（见下）。Apache-2.0 §4(b) 要求再分发者说明修改，
> 因此必须如实登记。

**修改主题：跨 `std::string_view` 的迭代器比较/相减 → 指针运算**

Verible 大量用 `std::string_view::const_iterator` 表示"文本中的位置"，但比较/相减的
两端常常来自**不同的 `string_view` 对象**（`contents_` 一个 view，`token.text()` 另一个，
`line.text()` 又一个）。标准上这属于不同 range 的迭代器比较，是 UB；libstdc++ 不检查，
所以 Linux/Release 一直没暴露，而 MSVC 的受检迭代器会直接断言：

```
cannot compare incompatible string_view iterators for equality   (__msvc_string_view.hpp:1384)
cannot subtract incompatible string_view iterators               (__msvc_string_view.hpp:1370)
cannot compare incompatible string_view iterators                (__msvc_string_view.hpp:1395)
```

修复方式统一为 `data()` 指针 + 长度，语义与输出**完全等价**（本项目的
`compare_cst` / `compare_format` / `compare_lint` 三个门禁把输出与官方 Verible 二进制
逐字节对比，改动后仍全过）。

另新增一个重载：

```cpp
// common/strings/range.h
std::string_view make_string_view_range(const char *begin, const char *end);
```

以及 `InterTokenInfo::preserved_space_start` / `InterTokenDecision::preserved_space_start`
从 `std::string_view::const_iterator`（用默认构造的"空迭代器"当哨兵）改成
`const char *` + `nullptr`（这正是 `format-token.h` 里原有 TODO 建议的方向）。

### 逐文件清单

| 文件（相对 `thirdparty/verible/`） | 补丁处数 |
| --- | --- |
| `common/formatting/align.h` | 1 |
| `common/formatting/format-token.cc` | 1 |
| `common/formatting/format-token.h` | 2 |
| `common/formatting/token-partition-tree.cc` | 4 |
| `common/formatting/tree-annotator.cc` | 1 |
| `common/strings/range.cc` | 1 |
| `common/strings/range.h` | 1 |
| `common/text/text-structure.cc` | 7 |
| `common/text/text-structure.h` | 1 |
| `common/text/token-info.h` | 1 |
| `common/text/token-stream-view.cc` | 1 |
| `common/text/tree-utils.cc` | 2 |
| `common/util/range.h` | 3 |
| `verilog/formatting/comment-controls.cc` | 1 |
| `verilog/formatting/formatter.cc` | 1 |
| `verilog/formatting/token-annotator.cc` | 2 |
| `verilog/formatting/tree-unwrapper.cc` | 4 |
| **合计** | **34** |

每个补丁点都有 `[fpgalsp patch]` 注释，可用：

```powershell
Select-String -Path thirdparty\verible\verible\**\*.cc,thirdparty\verible\verible\**\*.h -Pattern '\[fpgalsp patch\]'
```

复核。上游已报：<https://github.com/chipsalliance/verible/issues/2366>
（含完整 patch：<https://gist.github.com/leuk0cyte/b6f65ed707d3cbe411be08ddde1>）。

---

## 二、构建系统的等价实现（不是源码补丁，但替代了上游 Bazel 构建步骤）

上游有两类"由 Bazel 在构建期生成"的输入，CMake 侧必须等价实现。它们不碰源码，
但**产出物必须是上游期望的形状**，否则编译失败。这里登记的目的是一旦上游头文件
结构变化，能快速定位。

### 2.1 `verilog_nonterminals_foreach-gen.inc`（原 genrule）

- **上游定义**：`verible/verilog/CST/BUILD`
  ```python
  genrule(
      name = "verilog-nonterminals-foreach-gen",
      srcs = ["verilog-nonterminals.h"],
      outs = ["verilog_nonterminals_foreach-gen.inc"],
      cmd = "sed -n -e '/BEGIN GENERATE/,/END GENERATE/p' $< | grep -v GENERATE | "
            "sed -e 's|^ *|CONSIDER(|' -e 's| =.*,|,|' -e 's|,|)|' > $@",
  )
  ```
- **CMake 等价实现**：`cmake/scripts/verible-nonterminals.cmake`（`cmake -P` 脚本）
- **原因**：Windows 上没有 `sed` / `grep`，不能照搬 shell 管道。
- **实现要点**：
  - 取 `// BEGIN GENERATE` 与 `// END GENERATE` 之间的枚举项
  - 去掉行首空白、**去掉行尾 `//` 注释**（源文件里有
    `kBaseDigits,  // e.g. 'd 1234`，不处理会生成 `CONSIDER(kBaseDigits,  // e.g. 'd 1234)`）
  - 去掉 ` = 值`、去掉末尾逗号
  - 输出 `CONSIDER(<name>)`（**不带**额外逗号：sed 的 `s|,|)|` 吃的正是那个分隔逗号）
- **实测产出**：474 项，`CONSIDER(kUntagged)` … `CONSIDER(kFilePathSpecList)`
- **挂接**：`cmake/Verible.cmake` 中的 `add_custom_command`

### 2.2 `generated-verible-build-version.h`（原 version-header genrule）

- **上游定义**：`verible/common/util/BUILD`
  ```python
  genrule(name = "version-header", outs = ["generated-verible-build-version.h"],
          cmd = "sed -e 's/^/#define VERIBLE_/' bazel-out/volatile-status.txt > $@", stamp = 1)
  ```
- **CMake 等价实现**：`cmake/Verible.cmake` 里用 `file(WRITE)` 直接生成，
  内容 `#define VERIBLE_GIT_COMMIT "<commit>"` / `#define VERIBLE_MODULE_VERSION "<commit>"`，
  commit 从 `VENDOR-INFO.md` 抽取。
- **原因**：Bazel 的 `volatile-status.txt` 依赖 `bazel/build-version.py` 和 git 仓库，
  vendored 副本既没有 `.git` 也没有 Bazel，无法复现。
- **注意点（踩过的坑）**：`init-command-line.cc` 里的 include 写的是
  `#include "verible/common/util/generated-verible-build-version.h"`（带 `verible/` 前缀），
  而文件本身在构建目录根下。单纯 `-I<gen>` 拼不出这个路径，所以在
  `<gen>/verible/common/util/` 下放了一个**转发头**（绝对路径 include + include guard）。

### 2.3 flex / bison 生成物（G1–G6）

- 由任务 0-D（成员 `flexbison-gen`）在 `cmake/scripts/` 实现，本任务只负责：
  - 生成 `CMakeLists.txt` 里把生成物的「源」和「头」登记成 target 属性
    （`verible_generated_sources` / `verible_generated_headers`）
  - `cmake/Verible.cmake` 负责在生成规则就绪后把它们挂回 target
  - 预留 include 点：`include(cmake/scripts/verible-generators.cmake)`（存在即接）
- 这部分**不是**上游补丁，只是构建系统对接。

---

## 三、MSVC 编译问题处置记录

阶段 0-B 实际遇到并解决的问题**全部在构建系统层面**，没有一条需要改上游源码。
按类别统计：

| # | 类别 | 现象 | 处置 |
|---|---|---|---|
| 1 | 生成物缺失（bison） | `C1083: 无法打开 "verible/verilog/parser/verilog-token-enum.h"` | 构建期生成，由 0-D 提供；生成器把这些 hdrs 记为 `verible_generated_headers`，不写进 `add_library`（否则 configure 期报 "Cannot find source file"） |
| 2 | 生成物缺失（flex） | `C1083: 无法打开 "FlexLexer.h"` | 路径由 0-D 通过 `VERIBLE_FLEX_LEXER_INCLUDE_DIR` 提供；`Verible.cmake` 用**目录级** `include_directories` 保证在本文件之后创建的 target 也能看到 |
| 3 | 生成物缺失（include 前缀） | `C1083: "verible/common/util/generated-verible-build-version.h"` | 见 2.2 的转发头 |
| 4 | 生成物缺失（自身 include） | `C1083: "verible/verilog/CST/verilog_nonterminals_foreach-gen.inc"` | 生成到 `<gen>/verible/verilog/CST/` 并按该前缀能被找到 |
| 5 | 生成脚本语义偏差 | `C4002: CONSIDER 参数过多` / `C2816: 缺少 ")"` | 见 2.1 的逗号与行尾注释处理 |
| 6 | Bazel→CMake 名称不匹配 | configure 期 `target not found: absl::flag / absl::globals / absl::random` | abseil 的 CMake 导入名与 Bazel target 名不同，生成器里维护 `ABSL_NAME_OVERRIDES` 映射表 |
| 7 | 阶段 1 包未纳入 | `target not found: verible::verilog::analysis::checkers::verilog-lint-rules` | 生成器把指向 `EXCLUDED_PREFIXES`（formatting/lsp/tools/checkers）的依赖**丢弃并输出注释**，而不是生成死引用 |
| 8 | `//external_libs:editscript` 命名空间 | 依赖名 `external_libs::editscript` 与 `verible::...` 风格不一致 | 额外生成 `verible::external_libs::editscript` 别名 |
| 9 | C1128（节数超限） | **未出现** | `/bigobj` 已全局生效（顶层 + `verible_common_opts` 都带） |
| 10 | `/permissive-` 两阶段查找 | **未出现** | 上游代码本身符合规范，无需 `/permissive` 回退 |
| 11 | **源文件全是生成物的库被建成 INTERFACE** | 链接报 `LNK2001: verilog::verilog_parse / verilog_debug / verilog_symbol_name` | `verilog-y-cc` 的 srcs 只有 `verilog-final.tab.cc`（configure 期不存在），生成器曾据此建 INTERFACE 库 → 永远没有 `.lib`。修法：**仍建 STATIC**，先用 `${VERIBLE_PLACEHOLDER_SRC}` 空占位源，生成物就绪后 `target_sources` 补挂。**修在 `tools/gen_verible_cmake.py`，不是改产物** |
| 12 | **alwayslink 语义** | 规则/全局构造被 MSVC 链接器丢弃（Bazel `alwayslink = 1`） | CMake 无对应属性 → 用 `/WHOLEARCHIVE:$<TARGET_FILE:...>`。**坑**：`/WHOLEARCHIVE:<file>` 要求 .lib 已存在，否则 `LNK1356`；必须同时把库加进 `target_link_libraries` 建立构建依赖 |
| 13 | **静态 CRT 被 abseil 覆盖** | 517 条 `LNK2038: MDd_DynamicDebug 不匹配 MTd_StaticDebug` | `thirdparty/abseil-cpp/CMakeLists.txt:61-68` 在自己作用域内**无条件**改写 `CMAKE_MSVC_RUNTIME_LIBRARY`。修法：`set(ABSL_MSVC_STATIC_RUNTIME ON)`。编译静态库时完全看不出来，**一链接可执行文件才爆** |
| 14 | **C 编译器被第三方重新探测** | `Check for working C compiler: mingw/g++.exe - broken` | 顶层 `LANGUAGES CXX`，但 googletest 的 CMakeLists 有 `project(... LANGUAGES C CXX)`，会自行探测 C 编译器。修法：顶层改 `LANGUAGES C CXX` + `Dependencies.cmake` 加 MSVC 断言 |
| 15 | **生成头无法用 -I 拼出** | `C1083: "verible/common/util/generated-verible-build-version.h"` | 文件在构建目录根，但 include 带 `verible/` 前缀。修法：在 `<gen>/verible/common/util/` 放**转发头**，且必须用绝对路径 include + include guard，否则与 `<gen>` 里的同名文件互相包含 → `C1014: 包含文件太多 深度=1024` |
| 16 | **configure 期挂接顺序** | `target_include_directories` 对生成物目录不生效 | 生成物目录必须用**目录级** `include_directories()`（在 `add_subdirectory` 之前），因为 `target_include_directories` 只能作用于已定义的 target |
| 17 | **CMake `if(EXISTS "a;b")` 列表陷阱** | 生成物明明存在却报"还缺生成源文件" | `get_target_property` 取回的是 list，`if(EXISTS "${x}")` 会拿整串去判断。必须 `foreach(IN LISTS)` 逐个检查 |

### 关于 `verible-nonterminals.cmake` 的两个实现 bug（用 sed 的上游 genrule 不会有）

上游 `verilog/parser/BUILD` 的 genrule 用 sed 三连做变换，CMake 等价实现时踩了两个坑，
两者都会产生**语法合法但语义错误**的 `.inc`，报错信息完全指不到根因：

| bug | 错误现象 | 原因 |
|---|---|---|
| 逗号多一个 | `C4002: 类函数宏的调用"CONSIDER"参数过多` | 生成成了 `CONSIDER(kX,)`。上游 `sed 's\|,\|)\|'` 替换的是行内**第一个**逗号，而那个逗号正是枚举项末尾的分隔逗号；正确输出是 `CONSIDER(kX)` |
| 行尾注释未剔除 | `C2816: 类函数宏"CONSIDER"的调用缺少终止 ")"` | 源文件里有 `kBaseDigits,  // e.g. 'd 1234`，注释里的 `1234` 被当成参数，且 `//` 吞掉了行尾的 `)` |

同类隐患：G4 抽取 `#ifndef VERILOG_TOKENTYPE ... #endif` 时，若用 `"\n#endif"` 之类的模糊锚点会
截到**外层**的 `#endif`（实测截出 `#if/#endif 对不匹配` → `C1070`）。bison 3.8.2 生成的
`verilog.tab.hh` 里 `YYTOKENTYPE` 出现 0 次，只有 `VERILOG_TOKENTYPE`（L63-65/L542），
锚点必须双名兼容。

### 关于 `/permissive-`

`/permissive-` 由顶层 `CMakeLists.txt` 全局开启，并且 `verible_common_opts`
也显式带着它。**阶段 0-B 没有对任何 verible_* target 加 `/permissive` 覆盖**——
上游代码在 MSVC 19.52 + C++23 + `/permissive-` 下全部通过，因此不需要降级。
如果将来某个阶段 1 的包（formatting / lsp / tools）在 `/permissive-` 下失败，
处置顺序应为：

1. 先确认是否是 `/permissive-` 引起（临时在单个 target 上加 `/permissive` 对比）
2. 若是，**优先只对该 target** 用 `/permissive` 覆盖，并在本文件登记原因
3. 只有在无法局部规避时，才考虑最小源码补丁（并在此登记）

---

## 四、变更历史

| 日期 | 阶段 | 内容 |
|---|---|---|
| 2026-11-18 | 0-B | 建立本文件；上游源码补丁 0 条；构建等价实现 2 项（2.1、2.2） |