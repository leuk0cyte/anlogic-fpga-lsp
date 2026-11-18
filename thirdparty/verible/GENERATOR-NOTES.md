# Verible 代码生成（flex/bison）踩坑记录

本文记录把 Verible 的 Bazel `genrule` / `genlex` / `genyacc` 移植到 CMake 过程中
踩到的**全部真实 bug**。每条都附症状、根因、修法。

这些坑的共同特征：**CMake 的文本处理原语会静默出错** —— 脚本跑完没有报错，
产物看起来"有内容"，但内容是错的。所以在 Windows + CMake 下做文本变换，
**必须对着真实产物做断言验证**，不能相信"脚本跑完没报错"。

对应实现：
- `cmake/scripts/verible-patch.cmake` —— G4/G5/G6a/G6b 文本变换
- `cmake/scripts/verible-generators.cmake` —— G1–G6 编排（调 win_flex/win_bison）
- `cmake/scripts/verible-gen-check.cmake` —— 生成物回归断言
- `cmake/scripts/verible-gen-baseline.cmake` —— 计数基线

验证工具链：`win_flex 2.6.4` / `win_bison (GNU Bison) 3.8.2` / CMake 4.5.0 / Ninja 1.13.0。

---

## 1. `list(APPEND)` 吞分号：27137 行被压成 671 行，不报错

**症状**：生成的 `verilog-final.tab.cc` 只有 671 行（应为 27137 行），
但文件不为空、构建不报错，看起来"生成了"。

**根因**：CMake 的 `list` 以 `;` 作分隔符。用
```cmake
list(APPEND lines "${line}")
```
保存源代码行时，行内的每个 `;` 都会被当成分隔符，**一行被拆成多行**。
虽然可以写 `'\;'` 转义，但 bison 输出里含大量反斜杠
（`#line ... "C:\\\\..."`、行尾续行符 `\`），转义会失效。

**修法**：**全程不用 `list()` 处理代码行**。改用 ASCII 0x01 作哨兵做字符串缓冲：
```cmake
string(ASCII 1 _S)
string(APPEND _out_buf "${_S}${line}")   # 追加一行
```

**教训**：任何把"源代码行"放进 CMake list 的写法都是定时炸弹。

---

## 2. `\\2` 回填把语句本体带进插入行

**症状**：G6b 产出了
```cpp
++yynerrs;param->RecordSyntaxError(yylval);
```
（新语句被拼到 `++yynerrs;` 同一行末尾，而不是新起一行）

**根因**：正则捕获组范围写大了：
```cmake
"(^|\n)([ \t]*\\+\\+yynerrs;[^\n]*)"     # 组 2 把整条语句都捕获了
"\\1\\2\n\\2// Automatically patched..." # 回填时语句被复制到插入行开头
```

**修法**：捕获组只含"行边界 + 缩进"，语句本体用 `[^\n]*` 消费但不捕获，
在替换文本里重新写一遍：
```cmake
"(^|\n)([ \t]*)\\+\\+yynerrs;([^\n]*)"
"\\1\\2++yynerrs;\\3\n\\2// Automatically patched ..."
```

---

## 3. `strlen("++yynerrs;")` 是 10 不是 11 → 计数恒为 0

**症状**："hits=0 即失败"的断言误报，或者 `count > 0` 断言直接拦住构建，
但产物实际上是好的。

**根因**：用"删掉锚点后用长度差 ÷ 单位长度"来计数时，单位长度写成 11（错），
导致 `diff / 11 == 0` 恒成立。

**修法**：改用 `string(FIND)` + 偏移前进逐次计数（不复制大字符串，O(n)）：
```cmake
set(_hits 0)
set(_scan_from 0)
while(_scan_from LESS _text_len)
    string(SUBSTRING "${_text}" ${_scan_from} -1 _win)
    string(FIND "${_win}" "++yynerrs;" _rel)
    if(_rel EQUAL -1)
        break()
    endif()
    math(EXPR _hits "${_hits} + 1")
    math(EXPR _scan_from "${_scan_from} + ${_rel} + 10")   # 10 = strlen
endwhile()
```

**教训**：任何"除以单位长度"的计数写法，都要先用已知样本验证一次。

---

## 4. `string(FIND)` 会命中行中间的子串

**症状**：想定位 `#ifndef VERILOG_TOKENTYPE` 所在行，结果定位到了
**include guard 那一行**（因为 include guard 的名字里恰好含有该子串）。

**根因**：`string(FIND)` 是纯子串搜索，不认行边界。

**修法**：**行首锚定**，并且逐行判断（不要用整段正则的 `^`，见第 5 条）。

---

## 5. 整段正则里的 `^` / `(^|\n)` 会在位置 0 空匹配

**症状**（本条是**最隐蔽**的一个，直接导致了 C1070 编译错误）：

`verilog-token-enum.h` 变成 547 行、`#if` 7 个 / `#endif` 6 个的**失衡文件**。
下游所有 include 它的文件都报：
```
verilog.tab.hh(545): fatal error C1070: #if/#endif 对不匹配
```
报错位置指向**引用方**，很难定位到生成器。

**根因**：用
```cmake
string(REGEX MATCH "(^|\n)([ \t]*#ifndef +VERILOG_TOKENTYPE)" _begin_line "${_text}")
```
定位起点。

**技术细节（这是理解本 bug 的关键）**：
> CMake 的 `string(REGEX ...)` **没有多行模式**，`^` 在默认情况下只匹配字符串起点。
> 而 `"(^|\n)pattern"` 这种"想同时匹配文件首行和中间行"的写法，
> 在整段文本上会**先在位置 0 用 `^` 分支空匹配成功** ——
> 因为 `(^|\n)` 的第一个分支在位置 0 是**零宽匹配**，正则引擎立即接受，
> **根本不会尝试后面的 `\n` 分支和 `pattern`**。

所以这不是"匹配错了地方"，而是"**根本没尝试匹配 pattern**"。

**为什么最难查**：
- `REGEX MATCH` 返回了**非空结果**（确实"命中"了一次 —— 在位置 0）
- 日志里 `hits=1` 是**对的**
- 退出码 0
- **所有成功信号都在，产物是坏的**

**修法**：要么逐行扫描（行级 `MATCHES "^..."` 时 `^` 是行首），
要么用 `string(REPLACE "\n" ";" ...)` 切成行数组
—— **但后者有第 1 条的 `;` 陷阱**（C++ 代码必然含 `;`，会被 list 吞）。
所以**逐行扫描 + 行级 `MATCHES` 是唯一安全的路子**。

---

**修法**：**老老实实逐行扫描**（头文件只有 ~560 行，代价可忽略），
用行级正则 `^[ \t]*#[ \t]*ifndef[ \t]+VERILOG_TOKENTYPE[ \t]*$` 判断起始行。

**注意**：必须区分
```
第 63 行: #ifndef VERILOG_TOKENTYPE      ← guard（起始锚点）
第 64 行: # define VERILOG_TOKENTYPE     ← 宏定义（不是锚点）
```

**正确的切片**：tab.hh 第 63 行 ~ 第 543 行（481 行），
加上外层 3 行头 + 1 行尾 => **约 485 行**。
产物若是 547 行，就是本条 bug。

**已加防护**：`verible-gen-check.cmake` 的 `g4` 检查现在会断言
`#if` 数 == `#endif` 数，且行数 < 600。这类失衡在生成期就被抓住，
不用等编译时报一个指错位置的消息。

---

## 6. "取第一个 `#endif`" ≠ "同层 `#endif`"

**症状**：G5 删除了错误的区域，或者 G4 切片提前结束。

**根因**：bison 生成的头文件里，枚举段**之前**已有 include guard、
`VERILOG_DEBUG` 等 `#if`/`#endif` 块，枚举段**内部**也可能嵌套 `#ifndef`。
直接取"向后第一个 `#endif`"会停在 include guard 的 `#endif`（第 54 行），
而不是枚举段自己的（第 543 行）。

**修法**：**嵌套深度计数**：
```cmake
if(_line MATCHES "^[ \t]*#[ \t]*if")
    math(EXPR _depth "${_depth} + 1")
elseif(_line MATCHES "^[ \t]*#[ \t]*endif")
    if(_depth EQUAL 0)
        # 找到同层 endif
        break()
    endif()
    math(EXPR _depth "${_depth} - 1")
endif()
```

---

## 7. 数行数用 `FIND + SUBSTRING` 循环是 O(n²)：100s+ → 0.3s

**症状**：`cmake -P verible-patch.cmake` 处理 1.6 MB 的 `.tab.cc` 要 **100 秒以上**。

**根因**：逐行循环里每次都 `string(SUBSTRING "${_rest}" ${_next} -1 _rest)`，
每次都要复制剩余的全部文本 → 27000 行就是 O(n²)。
`--trace` 统计显示单个数行数的循环就发出了 27138 次 `string(SUBSTRING)`。

**修法**：一次 `REGEX REPLACE` 删掉所有换行，用长度差算行数：
```cmake
string(REGEX REPLACE "\n" "" _no_nl "${_text}")
string(LENGTH "${_no_nl}" _no_nl_len)
math(EXPR _nlines "${_text_len} - ${_no_nl_len}")
```
G6a/G6b 的替换也从逐行改成**一次整文本 REGEX REPLACE**。
四条规则合计从 140 s+ 降到 **~1.1 s**。

---

## 7b. `string(APPEND)` 在长循环里也是 O(n²)：22 秒

**症状**：为了修"贪婪正则改错位置"，把 G6a 改成逐行 `string(APPEND)` 累积，
结果 g6a 从 0.5 s 退化到 **66~120 s，甚至跑不完**。

**实测对比**（同一台机器）：

| 操作 | 耗时 |
|---|---|
| 27001 行循环里 `string(APPEND)` 累积 3 MB | **22 000 ms** |
| 一次 `string(REPLACE)` 处理 1.6 MB 全文件 | **1 ms** |
| 一次 `string(REGEX REPLACE "[^\x01]" ...)` 处理 1.6 MB | **2 200 ms** |

**结论**：
- **绝不要在逐行循环里 `string(APPEND)` 累积大文本**
- `string(REPLACE)` 是 O(n) 且常数极小，能一次做完就不要循环
- `string(REGEX REPLACE)` 也很慢（比 REPLACE 慢 3 个数量级），
  别用它做"逐字符筛选"这类本可以用 REPLACE 完成的工作

**G6a 的最终方案**（314 ms，语义与 sed 等价）：
1. 4 次整文本 `REGEX REPLACE`（顺序严格 1→4）
2. **前置校验**"同一行是否有重复锚点"，有则 FATAL_ERROR
   （因为全局替换改所有出现，而 sed 只改第一个；不满足前提就拒绝处理）
3. 校验用 `REPLACE + 长度差`，不是逐行 SUBSTRING

**取舍**：用"每行第一个的精确语义"换来了"全局替换 + 拒绝歧义输入"。
好处是 O(n) 且覆盖所有真实输入；代价是同行重复锚点会报错而非处理。
实测 bison 3.8.2 的 `.tab.cc` **0 行**有重复锚点，前提成立。

---

## 7c. 贪婪量词 vs 非贪婪：CMake 只支持贪婪

**症状**："每行只替换第一个"的 sed 语义，用
```cmake
string(REGEX REPLACE "(^|\n)([^\n]*)= yylval;" "\\1\\2= std::move(yylval);" ...)
```
会改到行内**最后一个**锚点（`[^\n]*` 贪婪）。

**想用非贪婪 `[^\n]*?`** —— **CMake 正则引擎不支持**，直接报：
```
RegularExpression::compile(): Nested *?+.
```

**结论**：CMake 的正则没有非贪婪量词、没有 lookahead、没有多行模式。
"sed 的每行第一个"这类语义**无法用单条 CMake 正则表达**，
只能用"逐行"或"全局替换 + 前提校验"（见 7b）。

---

## 8. `VERBATIM` + 参数内嵌引号 → 工具收到带引号的路径

**症状**：
```
win_flex.exe: could not create "Y:/.../verilog.yy.cc"
win_bison.exe: "Y:/.../verilog.output": cannot open: No such file or directory
```
但**同一命令在 cmd 里手工跑就成功**。

**根因**：
```cmake
COMMAND "${WIN_FLEX}" --outfile="${VG_VERILOG_YY_CC}" ...   # ← 错
VERBATIM
```
生成到 `build.ninja` 的是一层 `cmd.exe /C "... && win_flex --outfile=\"Y:/...\""`。
那层 `\"` 是给 cmd 的，但 **win_flex/win_bison 收到的 argv 里引号是内容的一部分**：
```
argv[1] == --outfile="Y:/.../verilog.yy.cc"    ← 含引号字符
```
于是它尝试创建文件名带引号的文件。

**修法**：参数整体作为一个 CMake 字符串，**不要在值两侧加引号**：
```cmake
COMMAND "${WIN_FLEX}" "--outfile=${VG_VERILOG_YY_CC}" ...
```
本工程路径不含空格，VERBATIM 最终不会加引号，工具拿到干净路径。

**附带**：每条命令第一个 `COMMAND` 加
`${CMAKE_COMMAND} -E make_directory "${_gp}"`，
防止并行构建/清理时目录短暂缺失（bison 一次写 4 个文件，缺一个目录就全失败）。

---

## 9. 生成物里嵌绝对路径 → 文件级哈希不可用

**实测**：
- `verilog.tab.hh` 的 include guard 里嵌着**输出文件的绝对路径**：
  `#ifndef YY_VERILOG_C_USERS_LEUKOC_1_APPDATA_LOCAL_TEMP_..._TAB_HH_INCLUDED`
- `verilog.tab.cc` 里有 **4090 处** `#line` 指向绝对路径
- `verilog.output`(4.8 MB) / `verilog.dot`(4.4 MB) 是 bison 状态机表

**结论**：换构建目录（`build/` → `build-debug/`）哈希就全变。
**任何跨目录/跨机器的文件级 sha256 都不可用** —— 会产生高频假阳性，
逼团队养成"直接更新基线"的习惯，门就废了。

**替代方案**（本项目采用）：
1. **差分断言**：不依赖固化常量，如"最终 std::move 数 == 原始锚点数"
2. **计数基线**：只固化"锚点数量"这类语义量
3. **行为层**：`ctest -L gate` 的 token 等价测试兜底

---

## 10. "基线跟着产物走"的滑坡 —— 完整版规则

任何要求人工确认的基线，都可能被人为省事而随手更新，门就失效了。

**可机械检查的规则**：

> 基线的每一次更新都必须能回答"为什么变了"。
> 如果某次提交里：
>   - 只改了 `verible-gen-baseline.cmake`，
>   - 而没有任何其他变化（bison/flex 版本、`verilog.y`、`verilog.lex`、
>     `GENERATOR-NOTES.md` 都没动），
> 那么这次基线更新**大概率是为了让红的构建变绿**，应当被 review 拒绝。

实现建议（接 CI 后）：
```bash
git diff --name-only | grep verible-gen-baseline.cmake
# 且同时不含 *.y / *.lex / GENERATOR-NOTES.md / 工具版本文件
#   => 自动标记为"需人工 review 基线变更"
```

**设计原则**：优先用差分断言（自动适应工具升级），
基线只用于抓"数量变了但没归零"——那是差分断言抓不到的。

---

## 11. CMake 脚本自身的语法错误必须表现为非零退出码

**实测**（CMake 4.5.0）：

| 情况 | exit code |
|---|---|
| 脚本有语法错误（未闭合字符串） | **1** |
| 运行时 `message(FATAL_ERROR)` | **1** |
| 正常结束 | **0** |

**结论**：基于 `cmake -P` 脚本的 CTest 断言**不会**因为脚本自身语法错误而假绿。
这个结论是安全的，可以放心依赖。

**配套坑**：`if(path STREQUAL "")` 里的 `path` 是**字面量**不是变量解引用，恒为假。
CMake 的 `if()` 字符串比较**不解引用变量名**，必须写 `"${path}"`。
写错时的表现是"文件明明存在却判定为不存在" —— 方向是"多报错"，
但如果写的是 `if(NOT path STREQUAL "")` 这类正向判断，就会变成**静默跳过**。

---

## 12. `verilog-parse-interface.h` 当前无人 include

全仓库 grep（`thirdparty/verible` 下所有 `*.h/*.cc/*.lex/*.y`）：
**引用数 0**。

它仍然被生成，原因是：
- (a) 与 Bazel 构建等价 —— 原 `parser/BUILD` 有这个 genrule；
- (b) 上游 `parser/BUILD` 里 `verilog-y-cc` 的 `hdrs` 声明了它，
  未来上游可能有人开始 include；
- (c) 上游注释明确写了拆分意图：token-enum 只含枚举、parse-interface 只含
  prototype，以切断循环依赖。

**若它缺失，构建不会失败** —— 这正是必须用 `files` 断言守住它的原因。
将来有人看到"这个头文件没人用"想删掉时，请先读这一段。

---

## 13. 断言必须覆盖结构完整性，不只是内容存在性

**真实事故**：G4 的断言检查了"含 VERILOG_TOKENTYPE / 不含 YYTOKENTYPE /
含 enum + typedef"，**全部通过**，但产出的文件 `#if` 7 个 / `#endif` 6 个，
是**失衡**的 —— 下游编译报 `C1070: #if/#endif 对不匹配`，
而且报错位置指向**引用方**（`verilog.tab.hh(545)`），极难定位到生成器。

**教训**：只证明"该有的在"，不证明"不该有的不在"、"结构是否自洽"。
**结构失衡（`#if/#endif` 不配对）是典型的"存在但不能用"**。

已加断言（`verible-gen-check.cmake` g4）：
- `#if` 数 == `#endif` 数
- 行数 < 600（超过即判定切片错位）
- 内容存在性断言保留

---

## 14. 变异测试的覆盖面 = 你设想过的失效模式集合

在 task-4 里我做了 7 个变异测试全部通过，但 G4 的结构失衡 bug **没被抓到** ——
因为那 7 个变异**全部针对 G6a/G6b**，没有一个针对 G4 的切片起点。

**这说明了变异测试的局限**：它只能证伪你**想得到**的错误。
"全部变异被捕获"不等于"覆盖完整"。

**改进规则：每条生成规则至少要覆盖三类变异**

| 类型 | 含义 | G4 的例子 |
|---|---|---|
| **缺失** | 该有的没有 | 锚点不存在 → 必须 FATAL_ERROR |
| **多余** | 不该有的有 | include guard / DEBUG 段被误切进来 |
| **错位/失衡** | 内容对但结构/顺序/位置错 | `#if`/`#endif` 不配对 |

**第三类最容易漏**，因为它需要"下游能感知到"才能被发现 ——
G4 这次正是靠门禁侧的独立编译才暴露的。

`tests/generator-e2e/unit/*.cmake` 已按这三类重写。

---

## 15. 产物的两行头部：一行可直接对比，一行需归一化

| 行 | 内容 | 是否含路径 | 对比时的处理 |
|---|---|---|---|
| 第 1 行 | `// verible-patch: g4 hits=1 lines=558` | **否**（纯数字） | **可直接逐字节对比** |
| 第 2 行 | `// DO NOT EDIT -- generated from <绝对路径>` | **是** | 需把路径归一化为 `<INPUT>` |

- **第 1 行是本工具加的**，刻意只含 `mode/hits/lines` 三个纯数字，
  跨机器、跨构建目录完全稳定。**请勿往里加路径/时间戳/主机名** ——
  一旦加了，整个诊断头就没法参与对比了。
- **第 2 行是原 Bazel 规则本来就有**（`echo '// DO NOT EDIT -- generated from $<'`），
  含绝对路径是官方规则固有的，**不影响与 Bazel 的等价性**，只影响跨目录对比。

G6 的产物（`.tab.cc`）不产生第 2 行（原规则没有 echo），只有第 1 行诊断头。

---

## 附：G5 的锚点为什么会变（bison 版本差异）

原 Bazel 的 sed 写的是：
```
sed -e '/#ifndef YYTOKENTYPE/,/#endif/d'
```
而当前 Verible 的 bison 调用带了 `%define api.prefix {verilog_}`，
生成的头文件里是 `#ifndef VERILOG_TOKENTYPE`（`YYTOKENTYPE` 出现 **0** 次）。

若只按字面匹配 `YYTOKENTYPE`，**命中 0 次、静默空操作** ——
`verilog-parse-interface.h` 会退化成 `verilog.tab.hh` 的原样拷贝，
把整段 `enum verilog_tokentype` 一起带进来，违背拆分意图，
只是靠 include guard 的巧合才没立刻炸。

修法：锚点同时认两个名字，且**命中 0 次即 FATAL_ERROR**。