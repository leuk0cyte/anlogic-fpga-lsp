# fpga-lsp-cpp

Anlogic（安路）TangDynasty FPGA 语言服务器（LSP）的 **C++23 重写版**。

- 前身：`Y:\fpga-lsp` —— 约 11.8k 行 Python 实现的 TangDynasty FPGA 语言服务器后端。
- 本项目：用 C++23 重写，目标是在保持功能对齐的前提下获得数量级更好的解析/索引性能。
- SystemVerilog 的解析、lint、格式化直接复用 vendored 的 [Verible](thirdparty/verible)（Bazel 构建已改造为 CMake）。

## 与 Y:\fpga-lsp 的关系

| 项目 | 语言 | 角色 |
| --- | --- | --- |
| `Y:\fpga-lsp` | Python | 现有实现，作为**功能基准/参考实现**，逐步被本工程替代 |
| `Y:\fpga-lsp-cpp` | C++23 | 重写目标工程，行为以 Python 版为规格对照 |

Python 版中的解析、索引、诊断逻辑迁到本工程后，LSP 协议层、TD 工具链交互、ADC 引脚约束等能力保持语义一致。

## 环境要求

| 组件 | 版本 / 位置 |
| --- | --- |
| Visual Studio | **18 Community**（`C:\Program Files\Microsoft Visual Studio\18\Community`） |
| MSVC 工具集 | 14.5x（`VC\Tools\MSVC\14.52.37025`） |
| vcvars | `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"` |
| CMake | ≥ 3.24（本机实测 4.5.0） |
| Ninja | 1.13.0（在 PATH 里） |
| C++ 标准 | C++23（`/std:c++23preview`），静态 CRT（`/MT` / `/MTd`） |

> 只支持 MSVC + Ninja x64。**不支持** GCC / Clang / clang-cl。

## 构建

本工程不做「在 CMake 里自动调 vcvars」，请先在 MSVC 环境下再调 CMake：

```bat
:: 在 cmd 里
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d Y:\fpga-lsp-cpp

:: 配置（Release 为默认；也可以指定 debug / relwithdebinfo）
cmake --preset windows-msvc-release

:: 构建（-j 的并行度由 buildPresets 的 jobs:0 = 用满所有核心控制，本机 192 逻辑核）
cmake --build --preset windows-msvc-release

:: 测试
ctest --preset windows-msvc-release
```

PowerShell 里可以一条命令完成：

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset windows-msvc-release && cmake --build --preset windows-msvc-release'
```

### Preset 一览

| Configure / Build Preset | 构建类型 | 说明 |
| --- | --- | --- |
| `windows-msvc-base` | — | hidden 基础 preset（Ninja、`cl`、静态 CRT、C++23） |
| `windows-msvc` | Release | 默认，等于 `windows-msvc-release` |
| `windows-msvc-debug` | Debug | `/MTd`、`/Od`、PDB |
| `windows-msvc-release` | Release | `/MT`、`/O2` |
| `windows-msvc-relwithdebinfo` | RelWithDebInfo | `/MT`、`/O2`、PDB |

构建目录固定为 `build/`（已 `.gitignore`）。

## 依赖清单（全部 vendored 在 `thirdparty/`，不依赖网络）

| 依赖 | 版本 | 接入方式 | 使用方 |
| --- | --- | --- | --- |
| abseil-cpp | vendored | `add_subdirectory`（**必须先于 re2**） | Verible / re2 |
| re2 | vendored | `add_subdirectory` | Verible lexer 正则、本项目字符串匹配 |
| nlohmann json | 3.12.0 | header-only | LSP JSON-RPC 消息 |
| googletest | 1.18.0 | `add_subdirectory` | 单元测试 |
| spdlog | 1.17.0 | `add_subdirectory`（内置 fmt） | 日志 |
| simdutf | 9.4.0 | `add_subdirectory` | UTF-8/UTF-16 转换 |
| compile-time-regular-expressions (ctre) | 3.11.0 | header-only | 编译期正则 |
| Verible | vendored | Bazel → CMake 移植（`cmake/Verible.cmake`） | SystemVerilog 解析/lint/format |

**明确不引入** zlib 和 protobuf（原 Bazel 构建中的这两项依赖已从源码中剥离）。

### 可链接的 target 名

```
absl::*                       (abseil 自带)
re2::re2
nlohmann_json::nlohmann_json
GTest::gtest / GTest::gtest_main
spdlog::spdlog
simdutf::simdutf
ctre::ctre / fpga_lsp::ctre
fpga_lsp::verible             (Verible 聚合 facade，供后续任务 link)
```

## 目录结构

```
CMakeLists.txt              顶层构建脚本
CMakePresets.json           preset 定义
cmake/
  Dependencies.cmake        第三方依赖接入（顺序敏感）
  Verible.cmake             Verible 接入（阶段 0-B/0-D）
  CompilerWarnings.cmake    本项目自有代码的警告策略
  scripts/                  flex/bison 生成规则（阶段 0-D）
thirdparty/                 vendored 第三方源码（只读，不修改）
src/
  platform/                 平台层：UTF-16 位置换算 / 路径 / URI / 日志 / 编码
  jsonrpc/                  JSON-RPC 传输层（LSP Content-Length / MCP newline）+ 分派
  app/                      fpgalsp-native 可执行入口
tests/
  unit/                     平台层 + JSON-RPC 的 GoogleTest 单测
  gate-corpus/              阶段0 Verible 门禁语料
  golden/                   vendored 裁剪基准
build/                      构建目录（不提交）
```

> 测试根目录统一为 `tests/`（复数）。顶层 `CMakeLists.txt` 优先加载
> `tests/CMakeLists.txt`，并保留对旧 `test/` 的兼容（会给出废弃提示）。

## 平台层与传输层（阶段 2-α）

`fpgalsp-native.exe` 是传输层的可运行骨架，用于验证协议栈并驱动 e2e 测试：

```bat
:: LSP 模式（Content-Length framing）
build\bin\fpgalsp-native.exe --stdio

:: MCP 模式（newline-delimited JSON）
build\bin\fpgalsp-native.exe --mcp

:: 可选：日志写文件（stdout 永远归协议独占）
build\bin\fpgalsp-native.exe --stdio --log build\server.log --verbose
```

已实现方法：LSP 模式下 `initialize` / `shutdown` / `exit`；MCP 模式下
`initialize` / `tools/list` / `ping` / `shutdown`。其余方法返回 `-32601`。

### 两个容易踩的规范要点

1. **UTF-16 列落在代理对中间时的行为**（复现 `fpgalsp/lsp.py` 的 `utf16_to_py`）：
   返回**该字符的起始索引**，不做四舍五入。中文占 1 码元，emoji 占 2 码元。
2. **framing 差异不只是分隔符**：LSP 的 body 可以含换行；MCP 的 body **绝不能**
   含裸换行，因此两种模式使用不同的 JSON 序列化密度。Windows 上还必须把
   stdin/stdout 切到二进制模式，否则 CRT 会把 `\r\n` 再翻译成 `\r\r\n`。

### 单元测试

```bat
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
:: 或只跑单元测试
ctest -L unit --output-on-failure
```

## 约定

- 只支持 x64 + MSVC + 静态 CRT；`BUILD_SHARED_LIBS` 固定 OFF。
- 第三方代码统一 `/W0`（由 `cmake/Dependencies.cmake` 注入），自有代码用 `fpga_lsp_default_warnings()`。
- 源码一律 UTF-8（`/utf-8` 全局开启）。
- 构建产物不进版本库，见 `.gitignore`。

### 移植铁律：发现 Python 缺陷，**忠实复现 + 登记**，不顺手修

移植的第一原则是**行为等价**。顺手"修好"一个 Python 缺陷，会让 C++ 版与
`tests/golden/` 基线、与用户现有体验同时脱节，且无法再判断"是我们的移植错了还是修掉了"。

所有发现的参考实现问题统一登记在 **[`tests/PYTHON-BUGS.md`](tests/PYTHON-BUGS.md)**，
每条都注明：位置、实测后果、C++ 的处置方式（复现 / 修复）、钉死的测试锚点。

等所有模块完成等价性验收后，再作为一次独立的「契约修正」统一处理并同步 golden 基线。

### 构建目录纪律：共享 `build/` 只放已验证状态

**多人/多 agent 同时往一个构建目录写，必然互相踩。** 本项目已实际发生过两次：

- **`CMAKE_CXX_FLAGS` 被清空**：有人用裸 `cmake -S . -B build -DCMAKE_CXX_COMPILER=cl`
  绕过 preset 配置，CMake 的 MSVC 默认 flag 初始化没跑 → `/EHsc` 与 `/O2` 全丢。
  症状极具迷惑性：`C4530` 警告 + 凡是"抛-捕"的测试都 `std::terminate` abort
  （看起来像代码 bug，实际是编译选项问题）。
- **`LNK1104`**：一个构建删掉了另一个构建正在链接的 `.obj`。

因此：

| 目录 | 用途 | 谁维护 |
| --- | --- | --- |
| `build/` | **权威构建**，只放已验证状态 | Lead 统一维护 |
| `build-<topic>/` | 各人/各任务的迭代目录 | 各自维护 |

**永远不要用裸 `cmake -S . -B build` 配置共享目录** —— 必须走 preset：

```powershell
cmake --preset windows-msvc-release          # 正确
cmake --build --preset windows-msvc-release
```

如果确实需要自定义构建目录，**所有关键 cache 变量都要显式传**，尤其是
`CMAKE_C_COMPILER=cl` 与 `CMAKE_CXX_COMPILER=cl`（否则会探到 PATH 上的 mingw 而失败）：

```powershell
cmake -S . -B build-mytopic -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_STANDARD_REQUIRED=ON `
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded" `
  -DABSL_MSVC_STATIC_RUNTIME=ON -DBUILD_TESTING=ON `
  -DFPGA_LSP_VERIBLE_ENABLED=ON
```

**恢复被污染的共享目录**（症状：`CMAKE_CXX_FLAGS` 为空、`C4530` 增多、抛-捕测试 abort）：

```powershell
Remove-Item build\CMakeCache.txt, build\CMakeFiles -Recurse -Force
cmake --preset windows-msvc-release
```

---

## Verible CMake 化的风险画像（重要，写给后来者）

> 本节来自阶段 0（parser 门禁）与阶段 1（lint 门禁）的实战统计，
> 目的是让后续维护者把注意力放在**真正容易出事的地方**。

### 结论：风险不在"解析器行为漂移"，而在"构建装配"

阶段 0 已用 **102 个文件 / 18,815 token** 证明：CMake + MSVC 构建的 Verible
与官方二进制在 **token 序列、CST 结构、错误消息**三个维度上 **100% 一致**。
也就是说，**解析行为本身没有漂移**。

而两个阶段累计发现的构建缺陷，**100% 集中在构建管线上，没有一个涉及解析/lint 逻辑**：

| 缺陷 | 所在层 | 是否"响亮"（报错明显） |
| --- | --- | --- |
| `verilog-y-cc` 被建成 INTERFACE 库 | target 类型 | ✅ 响亮（`LNK2001`） |
| `command-file-lexer.lib` 不在依赖图但被 `/WHOLEARCHIVE` 引用 | 依赖图 | ✅ 响亮（`LNK1356`） |
| G1/G2/G3 的 `VERBATIM` 引号把 `--outfile=...` 传成带字面引号的参数 | 参数传递 | ✅ 响亮（文件没生成） |
| **G4 锚点失效**（CMake 正则 `^` 只匹配整段开头，`(^\|\n)` 的 `^` 在位置 0 空匹配） | **文本处理** | ❌ **静默**（`REGEX MATCH` 返回非空、`hits=1` 看起来正常，产物却是坏的） |
| **G6a `std::move` 补丁未生效** | **文本处理** | ❌ **静默**（命令执行成功，产物未改到目标行） |

### 关键规律：**静默类缺陷全部在"文本处理"层**

这是 CMake 脚本化移植的固有风险，也是本项目建立 `ctest -L generator` 的原因：

> **对生成物做断言时，必须断言"结构性不变量"**
> （`#if` 与 `#endif` 数量相等、切片行数在期望区间、`std::move` 出现次数 = 锚点数），
> **而不是只断言"命令执行成功"。**

`tests/check_generated_parser.py` 就是按这个原则写的：它硬编码 bison 3.8.2 的
基线计数，逐项断言，并专门防 `0 == 0` 假通过。

### 两个补丁的风险等级不同（务必区分）

| 补丁 | 失效表现 | 防护手段 |
| --- | --- | --- |
| **G6a** `std_move_parser_symbols` | **编译失败**（`C2280`，`unique_ptr` 拷贝赋值已 delete） | **编译器本身**，不需要额外断言 |
| **G6b** `record_recovered_syntax_errors` | **静默**：能编译、token 完全一致，只是语法错误恢复能力丢失 | 必须靠**对比官方错误消息**（`tests/gate-corpus/error-cases/` 专项） |

### 关于 lint 规则（阶段 1 补充）

Verible 每条 lint 规则靠 `VERILOG_REGISTER_LINT_RULE` 产生一个 **static 全局对象**
在启动时自注册。这些对象**没有任何外部符号引用**，MSVC 静态库链接器默认会把它们丢弃
→ **程序正常运行、退出码 0、不报错，但规则静默消失**。

因此：
- 所有规则库必须用 `/WHOLEARCHIVE` 整库链入；
- 本项目封装为聚合 target **`verible_lint_rules_all`**，下游一行链接即可，不要自己列；
- 验收标准是"**61 条规则逐条 `dumpbin` 能找到符号**"，而不是"能编译"。

### 生成器改动后的自检命令

```bat
:: 生成物完整性（硬编码基线 + 防 0==0 假通过）
python tests\check_generated_parser.py ^
  --generated build\verible\gen\verible\verilog\parser\verilog-final.tab.cc ^
  --raw-cc    build\verible\gen\verible\verilog\parser\verilog.tab.cc

:: 双跑两套门禁（阶段0 回归 + 阶段1 规则清单）
python tests\run_gates.py --json build\gates.json
```
---

## 交付状态（2026-11-18，Phase B 后）

### 已通过验证

| 项目 | 数据 |
| --- | --- |
| **三条 Verible 门禁** | 解析 token/CST 102/102、lint 规则 61-42 + 诊断 398 条、formatter 全文档 102/102 + 局部 194/194 区间 |
| **`ctest` 全量** | **53/53 通过**（`gate` `lintgate` `formatgate` `generator` `mcp` `unit` `td` `e2e`；含 Phase B 新增的语义/代码动作/rename/call hierarchy/analysis/约束符号 6 个 target） |
| **功能模块** | LSP 全能力（25 方法，含语义高亮/折叠/代码动作/rename/callHierarchy/rangeFormatting/reports）/ MCP **39 个工具全部真实实现（0 占位）** / TD 会话与诊断 / Flow + BitWizard / 报告解析 / ADC+SDC 符号 / HDL 索引（与 Python 1214 项 0 差异） |
| **原生产物** | `build/bin/fpgalsp-native.exe`（5,945,344 B，SHA256 `7EDC9A67…32B03`；单一静态 exe，内嵌 Verible 解析/lint/格式化） |

### Phase B 关闭的 S0/S1（详见 `audit/2026-11-18/FIX-REPORT.md`）

- LSP：semanticTokens（2094 token 逐元素一致）、foldingRange（24）、codeAction（实例化 + 内嵌 lint autofix）、
  prepareRename/rename（4 处编辑逐字段一致）、callHierarchy in/out（1/12 逐字段一致）、rangeFormatting、
  `fpgaLsp/reports`（真实报告 + strategies/routed_db）
- executeCommand：instantiate / copyInstantiation / symbolPath / duplicateModules / searchHierarchy /
  debugHighlight / addSourceFile / setProjectName / runAnalysis / updateProject 全部真实实现
- MCP：format / code_actions / rename_edits / analysis_run / project_update 全部真实实现；pins 大小写与键集合对齐
- ADC/SDC：documentSymbol 47/2、hover、completion 逐字段一致
- completion：814 → **814**（21 模块 + 745 器件原语 + 48 关键字）
- moduleTree：`includePrimitives=false` 时 34 节点、0 黑盒
- 性能：200 文件 documentSymbol 29.5 → **0.84 ms**；project_structure 0.87× → **1.73×**；峰值内存 −23.7%

### 已知差异（有意保留 / 版本基线）

1. **Verible 版本基线**：内嵌 `c6b29d62` vs Python 侧外部 `0.0-4296`。`formatting`/`rangeFormatting` 字节与
   autofix `no-trailing-spaces` 一行差异属此族（LSP-18）；单一静态 exe 优先，不为对齐外部版本改内嵌规则。
2. **`ifdef` 语义**：C++ 只索引活动分支，Python 两分支都收（LSP-14，`COMMANDS.md` 已登记为有意差异）。
3. **inlayHint additive**：C++ 比 Python 多 8 条实例类型提示（Python 是 C++ 的真子集）。
4. **非 BMP 列**：Python 语义高亮用码点列（其 docstring 写 UTF-16 但实现不是），C++ 按协议用 UTF-16；
   仅合成 emoji 语料有 7/102 token 差异，真实 HDL 零差异。
5. **Python 格式 fallback**：`format_fallback.py` 未移植；C++ 始终有内嵌 Verible，不需要 fallback。
6. **Python 参考实现的 28 条缺陷**：见 `tests/PYTHON-BUGS.md`，除安全 High（B7 已修）外保持"忠实复现 + 登记"。

### VS Code 扩展（已打包并在本机安装验证）

```
Y:\fpga-lsp-cpp\vscode-extension\fpga-lsp-anlogic-0.13.0.vsix   225 files / 2.75 MB
  └─ extension/server/bin/fpgalsp-native.exe                 5,945,344 B（SHA256 7EDC9A67…32B03）
```

安装后 `resolveServer()` 返回 `{kind:"native", source:"bundled", exe:"<ext>/server/bin/fpgalsp-native.exe"}`。

对**打包后的** exe 做 LSP 握手实测：

```
serverInfo      : fpga-lsp 0.1.0
capabilities    : 16 组（textDocumentSync / completion / hover / definition /
                  documentSymbol / workspaceSymbol / references / codeAction /
                  formatting ×2 / rename / callHierarchy / semanticTokens /
                  foldingRange / inlayHint / positionEncoding）
fpgalsp.info    : ok, EF2 / EF2L45LG144B / top
fpgaLsp/moduleTree: ok, root=top, portCount=25, children=19
exit            : 0，无残留 td_commands_prompt.exe
```

> 仍未由人眼确认的：GUI 内的渲染（Problems 波浪线、大纲树、InlayHint 显示）。
> 服务端协议层已逐项验证。

### 仍未验证 / 有意保留

1. **真实 DRC/CDC 端到端**：本机语料没有 `*_Runs/phy_1/*_pr.db`，`runAnalysis` 只验证了
   "无布线产物"降级路径与解析层单测；有 db 时的 `report_drc`/`check_cdc` 需要真实布线结果才能复跑。
2. **GUI 渲染**：Problems 波浪线、大纲树、InlayHint、reports webview 未由人眼确认；
   服务端协议层已逐项验证（`e2e.lsp_smoke` + `tools/audit/probe_lsp.py`）。
3. **`ifdef` / inlayHint / 非 BMP 列**：见上节"已知差异"，均为有意差异或版本基线。
4. **`tests/golden/snapshots/`**：仓库内快照保留为历史基线；Phase B 的等价性证据在
   `audit/2026-11-18/golden-final/`（Python/native 双采 + compare）。
5. **Python 参考实现的 28 条缺陷**：见 `tests/PYTHON-BUGS.md`，除安全 High（B7 已修）外保持
   "忠实复现 + 登记 + 测试钉死"。

### 参考实现缺陷里最该知道的两条

- **P-1**：`ip_generated_hdl` 把 `.vhd` 混进 Verilog 列表 → elaborate 整体失败 → 用户看到"0 warning"而 elaborate 根本没跑。
- **P-7b**：`.al` 的 `Path` 决定 TD 的**工程根**，产物写到那里。复制工程再跑 TD 必须改写 `Path`，否则会写进用户真实工程（本项目中差点发生）。
