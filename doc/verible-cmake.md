# Verible CMake 化的风险画像（写给后来者）

> 本节来自阶段 0（parser 门禁）与阶段 1（lint 门禁）的实战统计，目的是让后续维护者把
> 注意力放在**真正容易出事的地方**。

## 结论：风险不在「解析器行为漂移」，而在「构建装配」

阶段 0 已用 **102 个文件 / 18,815 token** 证明：CMake + MSVC 构建的 Verible 与官方
二进制在 **token 序列、CST 结构、错误消息** 三个维度上 **100% 一致**。也就是说，
**解析行为本身没有漂移**。

而累计发现的构建缺陷，**100% 集中在构建管线上，没有一个涉及解析/lint 逻辑**：

| 缺陷 | 所在层 | 是否「响亮」（报错明显） |
| --- | --- | --- |
| `verilog-y-cc` 被建成 INTERFACE 库 | target 类型 | ✅ 响亮（`LNK2001`） |
| `command-file-lexer.lib` 不在依赖图但被 `/WHOLEARCHIVE` 引用 | 依赖图 | ✅ 响亮（`LNK1356`） |
| G1/G2/G3 的 `VERBATIM` 引号把 `--outfile=...` 传成带字面引号的参数 | 参数传递 | ✅ 响亮（文件没生成） |
| **G4 锚点失效**（CMake 正则 `^` 只匹配整段开头，`(^\|\n)` 的 `^` 在位置 0 空匹配） | **文本处理** | ❌ **静默**（`REGEX MATCH` 返回非空、`hits=1` 看起来正常，产物却是坏的） |
| **G6a `std::move` 补丁未生效** | **文本处理** | ❌ **静默**（命令执行成功，产物未改到目标行） |
| **生成物未挂接到 target**（首次 configure 时 flex/bison 产物还不存在） | **生成物装配** | ❌ **静默**（库只含空占位源；`verilog-y-cc.lib` 只有 1 KB 而非 2 MB，链接期才报 35 个符号） |
| **Verible 被无条件排除在 LTO 之外**（`INTERPROCEDURAL_OPTIMIZATION OFF`） | **LTO 装配** | ❌ **静默**（构建成功、行为正常，只是没吃到优化；且让「LTO 失败」的归因长期错在 flex 上） |

## 关键规律：静默类缺陷全部在「文本处理」与「装配」层

这是 CMake 脚本化移植的固有风险，也是本项目建立 `ctest -L generator` 的原因：

> **对生成物做断言时，必须断言「结构性不变量」**
> （`#if` 与 `#endif` 数量相等、切片行数在期望区间、`std::move` 出现次数 = 锚点数），
> **而不是只断言「命令执行成功」。**

`tests/check_generated_parser.py` 就是按这个原则写的：它硬编码 bison 3.8.2 的基线计数，
逐项断言，并专门防 `0 == 0` 假通过。

### 2026-11-19 补充：两条「装配类」静默缺陷

它们不是文本处理，而是**目标源文件列表**与**生成时机**的错配：

- `verible_mount_generated_sources()` 只在 configure 期用 `EXISTS` 判断，而 G1–G6b 是
  构建期命令 → 全新目录首次 configure 时挂接被跳过 → 库缺 lexer/parser 源。症状要到
  **链接期**才出现（35 个符号），且**不开 LTO 也一样**，因此极易误判成 LTO 不兼容。
- 修法：`cmake/scripts/verible-generators.cmake` 增加**配置期预生成**
  （`VERIBLE_PREGENERATE_AT_CONFIGURE`，默认 ON，只补缺失产物），使挂接在同一次
  configure 内完成。`ctest -L generator` 的基线断言仍然保留——它防的是"产物内容错了"，
  而这次修的是"产物根本没进库"，两者互补。

**教训：「能编译」和「目标里真的有这些源文件」是两件事。** 判断后者要查库内容
（`dumpbin /linkermember` 看成员），不要只看构建是否成功。

## 两个补丁的风险等级不同（务必区分）

| 补丁 | 失效表现 | 防护手段 |
| --- | --- | --- |
| **G6a** `std_move_parser_symbols` | **编译失败**（`C2280`，`unique_ptr` 拷贝赋值已 delete） | **编译器本身**，不需要额外断言 |
| **G6b** `record_recovered_syntax_errors` | **静默**：能编译、token 完全一致，只是语法错误恢复能力丢失 | 必须靠**对比官方错误消息**（`tests/gate-corpus/error-cases/` 专项） |

## 关于 lint 规则

Verible 每条 lint 规则靠 `VERILOG_REGISTER_LINT_RULE` 产生一个 **static 全局对象**
在启动时自注册。这些对象**没有任何外部符号引用**，MSVC 静态库链接器默认会把它们丢弃
→ **程序正常运行、退出码 0、不报错，但规则静默消失**。

因此：

- 所有规则库必须用 `/WHOLEARCHIVE` 整库链入；
- 本项目封装为聚合 target **`verible_lint_rules_all`**，下游一行链接即可，不要自己列；
- 验收标准是「**61 条规则逐条 `dumpbin` 能找到符号**」，而不是「能编译」。

## 生成物自检命令

```bat
:: 生成物完整性（硬编码基线 + 防 0==0 假通过）
python tests\check_generated_parser.py ^
  --generated build\verible\gen\verible\verilog\parser\verilog-final.tab.cc ^
  --raw-cc    build\verible\gen\verible\verilog\parser\verilog.tab.cc

:: 双跑两套门禁（阶段0 回归 + 阶段1 规则清单）
python tests\run_gates.py --json build\gates.json
```

升级 bison / flex 版本的完整验证流程见
[verible-generator-upgrade.md](verible-generator-upgrade.md)。