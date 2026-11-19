# 生成器升级后的正确性验证手册（bison / flex 版本漂移）

> 本节回答一个问题：**如果将来有人把 win_bison 从 3.8.2 升级到 4.x（或任何其它版本），
> 我们怎么知道 Verible 移植是否还正确？**
>
> 这一节刻意写成"可执行步骤"，而不是"重新跑一遍测试"。因为版本升级引入的风险
> 是**静默失效**，不是报错 —— 走错流程会得到假的绿色。

---

## 0. 先理解风险在哪里

生成物里只有两处是"靠文本补丁改出来"的，其余都由 bison/flex 原生生成：

| 补丁 | 干什么 | **失效后的表现** | 风险等级 |
| --- | --- | --- | --- |
| **G6a** `std_move_parser_symbols` | 把 `= yylval;` 等变成 `std::move(...)`，并注释掉 `yyval = yyvsp[1-yylen];` | **编译失败**（`error C2280`，unique_ptr 拷贝赋值被 delete） | 🟢 **低**：响亮失败，不可能静默 |
| **G6b** `record_recovered_syntax_errors` | 在每处 `++yynerrs;` 后插入 `param->RecordSyntaxError(yylval);` | **能正常编译、token 完全一致**，但语法错误恢复能力静默丢失（错误消息变少或为空） | 🔴 **高**：静默失效 |

**结论：G6a 由编译器保证正确性，不需要额外断言。G6b 才是最需要防护的那个，
而且必须靠"对比官方错误消息"来抓，光比 token 抓不到。**

---

## 1. 升级后必须执行的验证步骤（按顺序）

> **2026-11-19 补充：生成时机有两个入口，别被它迷惑。**
>
> - **构建期**：G1–G6b 是 `add_custom_command`，`DEPENDS` 里包含 `win_flex` / `win_bison`
>   本身。所以**原地升级工具**后，ninja 会因工具 mtime 变化而重新生成——这条路径不受影响。
> - **配置期**：`VERIBLE_PREGENERATE_AT_CONFIGURE`（默认 ON）只在产物**缺失**时补生成，
>   用于让全新目录一次 configure 就能挂接（见 `doc/audit/2026-11-19/static/LTCG.md` §3）。
>   它**不会**因为工具升级而重新生成（产物已存在），所以升级后**不要**指望 configure 帮你刷新；
>   要么让 ninja 跑（推荐），要么删掉 `build/verible/gen` 再 configure。
>
> 判据：升级 bison/flex 后先跑下面步骤 1 的锚点核对。若数字不符，说明生成物还是旧工具产出的，
> 先强制重新生成再核对。

### 步骤 1：锚点计数核对（最先做，最便宜）

升级后**第一件事**，在打补丁**之前**，对原始 `verilog.tab.cc` 统计补丁锚点出现次数。

```powershell
python tests/check_generated_parser.py `
  --generated build/verible/gen/verible/verilog/parser/verilog-final.tab.cc `
  --raw-cc    build/verible/gen/verible/verilog/parser/verilog.tab.cc
```

脚本里 `EXPECTED_ANCHORS` 硬编码了 bison **3.8.2** 下的实测值：

| 锚点 | 期望次数 |
| --- | --- |
| `= yylval;` | 2 |
| `= yyval;` | 1 |
| `(= yyval_default)` | 1 |
| `yyval = yyvsp[1-yylen];` | 1 |
| **`++yynerrs;`** | **2** |

**若数字不符 → 立刻停止，不要继续构建。** 说明 bison 的代码生成格式变了，
现有补丁可能部分失配。

> ⚠️ **为什么必须硬编码而不是"只要补丁前后数量相等"**：
> 如果 bison 4.x 不再生成 `++yynerrs;`，那么"打补丁前 0 处、打补丁后 0 处"，
> 朴素的相等断言会得到 `0 == 0` → **假通过**。脚本里因此同时断言了
> `count > 0` 和 `count == 期望值`。这个场景已在 §4 用变异测试验证过确实会被抓到。

### 步骤 2：确认补丁实际生效

```powershell
python tests/check_generated_parser.py --generated build/.../verilog-final.tab.cc
```

必须全部 PASS，重点看：
- `无残留 '= yylval;'` —— 若失败，说明 G6a 漏改，**构建会失败**（但先看清楚是哪条没匹配）
- `++yynerrs; 数量 > 0` —— 若失败，说明 bison 改了输出格式，G6b 已失效
- `yynerrs 与 RecordSyntaxError 数量一致`

### 步骤 3：编译验证 G6a（编译器就是 G6a 的回归测试）

```powershell
cmake --build build --target cst_dump
```

**如果 G6a 有任一处理漏打，这一步必然失败**，报 `error C2280`
（`std::unique_ptr` 拷贝赋值被 delete），且错误位置精确指向未补丁的行号。
这是好消息：不需要为 G6a 写任何断言，编译器已经替我们验证了。

### 步骤 4：**门禁对比（最关键的一步）**

```powershell
cmake --build build --target cst_dump
python tests/compare_cst.py `
  --cst-dump build/bin/cst_dump.exe `
  --official %USERPROFILE%\verible\verible-verilog-syntax.exe `
  --corpus   tests/gate-corpus `
  --pattern  ".v,.sv" `
  --report   build/gate-report.json
```

**必须 94 文件全部一致、一致率 100%，且退出码 0。**

注意 `--strict-errors` 默认开启，所以这一步同时校验了：
- token 序列（覆盖 G6a 语义、lexer 行为）
- **错误消息列表（覆盖 G6b）**

### 步骤 5：错误恢复专项（G6b 的定向验证）

```powershell
python tests/compare_cst.py `
  --cst-dump build/bin/cst_dump.exe `
  --official %USERPROFILE%\verible\verible-verilog-syntax.exe `
  --corpus   tests/gate-corpus/error-cases `
  --pattern  ".sv"
```

`tests/gate-corpus/error-cases/` 下有 8 个**故意有语法错误**的文件，
官方基线为 **12 条错误消息**：

| 文件 | 官方错误数 | 触发的问题 |
| --- | --- | --- |
| `e01_missing_endmodule.sv` | 1 | 模块未闭合（unexpected EOF） |
| `e02_extra_comma_in_portlist.sv` | **3** | 端口列表多余逗号（多错误恢复） |
| `e03_missing_semicolon_in_always.sv` | 1 | always 块漏分号 |
| `e04_case_without_endcase.sv` | 1 | case 缺 endcase |
| `e05_incomplete_ternary.sv` | 1 | 不完整三目运算 |
| `e06_unclosed_begin_end.sv` | 1 | begin/end 不配对 |
| `e07_unterminated_string.sv` | 1 | 字符串未闭合（词法错误） |
| `e08_multiple_errors.sv` | **3** | 多个独立错误 |

**为什么这条最关键**：如果 G6b 失效，这一步会出现
`error-message mismatch (official=3, cst_dump=0)` —— 而步骤 4 的 token 对比
**仍然会通过**。这正是为什么必须单独跑这一步。

### 步骤 6：人工 diff 生成的 parser

如果步骤 1–5 全部通过，**理论上可以不看代码**。但建议对升级做一次人工抽查：

```powershell
# 用旧版本生成一份 verilog.tab.cc 存档，与新版本 diff
# 重点关注：yyparse 主体、错误恢复段、yyval/yylval 的赋值处
```

---

## 2. 如果失败了，怎么定位

| 现象 | 含义 | 处理 |
| --- | --- | --- |
| 步骤1 锚点计数不符 | bison 输出格式变了 | 对照新版本 `verilog.tab.cc` 找到等价模式，**更新 `cmake/scripts/verible-patch.cmake` 与 `EXPECTED_ANCHORS`**，然后在报告里记录 |
| 步骤3 编译 `C2280` | G6a 漏改某处 | 看错误行号，对照未打补丁的原文件，补上 G6a 规则 |
| 步骤4 token 不一致 | 真·解析行为差异 | 用 `--report` 输出的 JSON 找到具体文件与 token 索引；区分是 lexer（flex 版本）还是 parser（bison 版本）差异 |
| 步骤4 通过但**步骤5 失败** | **G6b 失效**（最危险） | 说明 `RecordSyntaxError` 没插入。检查 patch 规则是否匹配到新的 `++yynerrs;` 形式 |
| 步骤4/5 都过，但 lint/formatter 阶段出问题 | 与生成器无关 | 属于阶段 1，查 lint 规则本身 |

---

## 3. 升级时的决策原则

1. **不要为了让测试变绿而修改期望值。** `EXPECTED_ANCHORS` 里的数字是"事实记录"，
   不是"可调参数"。改它之前必须能解释清楚**为什么**新版本的模式数量不同，
   并且步骤 4/5 仍然通过。
2. **升级后必须重跑官方基准对比，并留存报告。** 把 `build/gate-report.json`
   与 `tests/GATE-REPORT.md` 一起归档，作为该版本组合的"已验证"证据。
3. **flex 与 bison 要一起记录版本。** lexer 是 flex 生成的，
   token 边界直接受 flex 版本影响（见 §4 未覆盖项）。
4. **官方基准二进制版本要固定。** 本门禁的 ground truth 是
   `%USERPROFILE%\verible\verible-verilog-syntax.exe`；
   如果更换基准二进制，整个基线都要重采。

---

## 4. 本手册中的断言已被验证是"真能抓到问题"的

为防止"守护脚本本身是坏的"，对 `tests/check_generated_parser.py` 做了两组变异测试：

| 变异 | 模拟的场景 | 实测结果 |
| --- | --- | --- |
| 删掉所有 `RecordSyntaxError` 调用 | G6b 完全没执行 | ✅ 报 4 项 FAIL，退出码 1 |
| 把 `++yynerrs;` 全部删掉（两边都变 0） | bison 升级后模式消失 → `0 == 0` 假通过 | ✅ 报 5 项 FAIL，退出码 1，含 `++yynerrs; 数量 > 0: got 0` |

结论：这两个守护是**活的**，不是摆设。

---

## 5. 一句话总结

> 升级 bison 后：**先看锚点计数，再看 G6b 断言，编译过（G6a 自动保证），
> 然后跑两遍门禁 —— 全语料比 token + 错误用例比错误消息。**
> 其中最容易被忽略、也最致命的是**错误用例那一遍**：它挂了而全语料过了，
> 就说明 G6b 失效了，此时解析器的错误恢复已经静默降级。