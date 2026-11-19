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
| C++ 标准 | C++23（`/std:c++23`），静态 CRT（`/MT` / `/MTd`） |
| LTO | Release 默认开启全量 LTCG（`/GL` + `/LTCG`），见下文「构建」 |

> 只支持 MSVC + Ninja x64。**不支持** GCC / Clang / clang-cl。

## 构建

本工程不做「在 CMake 里自动调 vcvars」，请先在 MSVC 环境下再调 CMake：

```bat
:: 在 cmd 里
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d Y:\fpga-lsp-cpp

:: 配置（Release 为默认，含全量 LTO；也可以指定 debug / relwithdebinfo）
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

**全新目录一次 configure 就能构建成功**（无需 configure 两次）。原因见下节
「Verible CMake 化的风险画像」里的「生成物未挂接」一条：
`cmake/scripts/verible-generators.cmake` 会在 configure 期把缺失的 flex/bison 产物
补出来，使 `verible_mount_generated_sources()` 在同一次 configure 内就能挂接。

### Release 默认开启全量 LTO

| 项 | 实测值（本机 192 核，`-j24`） |
| --- | --- |
| 全新目录 configure | 约 29 s（含配置期预生成 flex/bison，占数秒） |
| 全新目录全量构建（844 目标） | 约 3.3 min（含 50+ 测试可执行文件） |
| **仅重链**（改一个 .cpp 后） | **约 48 s**（`/LTCG` 全程序分析，比普通链接慢很多） |
| 二进制体积 | 6,132,736 B（同源码非 LTO 为 6,248,960 B，**−1.9%**） |
| 性能 | lint 规则层 CPU **−11.9%**（361 KB 文件 / 1028 条诊断）；纯解析层基本不变 |

只改代码、想快速迭代时用无 LTO 的 `windows-msvc-relwithdebinfo`：

```powershell
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
```

相关 CMake 开关（默认值保证不改变行为）：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION` | Release preset 里 ON | CMake 级 IPO（`/GL` + `/LTCG`） |
| `FPGA_LSP_VERIBLE_ALLOW_LTO` | OFF | 是否让 Verible 也参与 LTO；**Release preset 里为 ON** |
| `FPGA_LSP_LTO_OWN_CODE` | OFF | 只给 `src/` 自有代码开 `/GL`（对照配置，验证收益用） |
| `VERIBLE_PREGENERATE_AT_CONFIGURE` | ON | configure 期补生成缺失的 flex/bison 产物（全新目录必需） |

> 历史背景：早期两次 LTO 尝试都失败并被错误归因为「flex 生成物与 `/GL` 不兼容」。
> 真实原因有两个叠加——生成物未挂接、以及 `cmake/Verible.cmake` 曾无条件把 Verible
> 的 IPO 设为 OFF（等于 Verible 从未进过 LTO 图）。完整实验记录见
> [`audit/2026-11-19/static/LTCG.md`](audit/2026-11-19/static/LTCG.md)。

### Preset 一览

| Configure / Build Preset | 构建类型 | 说明 |
| --- | --- | --- |
| `windows-msvc-base` | — | hidden 基础 preset（Ninja、`cl`、静态 CRT、C++23） |
| `windows-msvc` | Release | 默认，等于 `windows-msvc-release` |
| `windows-msvc-debug` | Debug | `/MTd`、`/Od`、PDB，无 LTO |
| `windows-msvc-release` | Release | `/MT`、`/O2`、**全量 LTO**（自有代码 + Verible） |
| `windows-msvc-relwithdebinfo` | RelWithDebInfo | `/MT`、`/O2`、PDB，**无 LTO**（保留可调试性与快速迭代） |
| `windows-msvc-release-lto` | Release | 与 release 相同的 LTO 配置，但输出到 `build-lto/`，用于与 `build/` 做 A/B |

构建目录：

| 目录 | 用途 |
| --- | --- |
| `build/` | 权威构建目录（Release，含 LTO），扩展打包默认从这里取 exe |
| `build-lto/` | `windows-msvc-release-lto` 的独立目录，仅用于 A/B 对照 |
| `build-<topic>/` | 各人/各任务的临时目录 |

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

> **供应链现状（2026-11-19 审计结论）**：`thirdparty/` 全部入库（7,338 个文件），构建完全
> 封闭、不存在"拉错版本"的风险；代价是上游修复要手工合入。其中 **abseil 没有版本锚点**
> （`MODULE.bazel` 里 `version = "head"`、CMake `project(absl)` 无 VERSION），Verible 也
> 未打标签——因此无法自动回答"落后哪些 CVE"。建议补 `thirdparty/*/UPSTREAM-VERSION.txt`
> （上游 tag + commit + 同步日期）。其余依赖版本明确：re2 `2025-11-05`（SONAME 11）、
> nlohmann 3.12.0、spdlog 1.17.0、simdutf 9.4.0、googletest 1.18.0。

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
CMakePresets.json           preset 定义（Release 含全量 LTO）
cmake/
  Dependencies.cmake        第三方依赖接入（顺序敏感）
  Verible.cmake             Verible 接入 + 配置期/构建期生成物挂接
  CompilerWarnings.cmake    本项目自有代码的警告策略
  scripts/                  flex/bison 生成规则（G1–G6b）+ 配置期预生成
thirdparty/                 vendored 第三方源码（只读，不修改）
src/
  platform/                 平台层：UTF-16 位置换算 / 路径 / URI / 日志 / 编码
  jsonrpc/                  JSON-RPC 传输层（LSP Content-Length / MCP newline）+ 分派
  lsp/                      文档存储 / 诊断
  server/                   LSP 服务端：方法分发、索引服务、报告存储、各 feature
  mcp/                      MCP 服务端与工具实现（bridge / tools）
  hdl/                      HDL 索引（模块/实例/端口）
  semantics/                语义高亮、折叠等
  constraints/              ADC / SDC 解析、诊断、补全
  reports/                  综合/布线报告解析（QoR / timing / area / DRC-CDC）
  flow/                     Flow 运行器、策略比较、BitWizard 编程
  td/                       TangDynasty 会话、器件库、诊断、Tcl 安全拼装
  project/                  .al 工程解析、XML
  analysis/                 DRC/CDC 分析与 project_update
  verible_bridge/           内嵌 Verible 的 lint / format 封装
  tools/                    cst_dump / lint_dump / format_dump 调试工具
  app/                       fpgalsp-native 可执行入口（含 MCP TCP 模式）
tests/
  unit/                     GoogleTest 单测（40 个 target）
  td/                       TD 集成测试
  perf/                     性能探针（index / latency / reports / mcp / td）
  gate-corpus/              阶段0 Verible 门禁语料
  golden/                   vendored 裁剪基准
tools/
  audit/                    静态分析流水线（run_audit.ps1 / summarize.py /
                            check_tcl_invariant.py）与探针
  perf/                     性能剖析与压测脚手架
audit/                      各轮审计报告与证据（2026-11-18 / 2026-11-19）
vscode-extension/           VS Code 扩展（打包时把 build/bin 的 exe 塞进 server/bin）
build/                      构建目录（不提交）
```

> 测试根目录统一为 `tests/`（复数）。顶层 `CMakeLists.txt` 优先加载
> `tests/CMakeLists.txt`，并保留对旧 `test/` 的兼容（会给出废弃提示）。

## 可执行入口与两种传输模式

`fpgalsp-native.exe` 既是 LSP 服务端，也是 MCP 服务端；两种模式共用同一套协议栈与
业务层，仅 framing 与序列化密度不同：

```bat
:: LSP 模式（Content-Length framing）
build\bin\fpgalsp-native.exe --stdio

:: MCP 模式（newline-delimited JSON）
build\bin\fpgalsp-native.exe --mcp

:: MCP over loopback TCP（供扩展/agent 以 client 身份附着）
build\bin\fpgalsp-native.exe --mcp --mcp-port <n> --root <dir>

:: 可选：日志写文件（stdout 永远归协议独占）
build\bin\fpgalsp-native.exe --stdio --log build\server.log --verbose
```

`--mcp-port` / `--attach-port` / `--allow-flow` / `--allow-write` / `--verible` 等开关见
`-h`。各条路径都有单条消息上限：LSP 的 `Content-Length` 帧与 MCP 的 loopback TCP 共用
`kMaxBodyBytes`（64 MiB，先判上限再分配），MCP stdio 使用有界读行。

已实现方法：LSP 模式覆盖 25 个方法（`initialize` / `shutdown` / `exit` / 补全 / 悬停 /
定义 / 引用 / 文档与工作区符号 / 代码动作 / 格式化 ×2 / 重命名 / 调用层次 / 语义高亮 /
折叠 / inlayHint / reports 等）；MCP 模式暴露 **39 个工具**（`initialize` / `tools/list` /
`ping` / `shutdown` + 工程、符号、诊断、报告、flow、约束等能力）。未实现的方法返回 `-32601`。

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
| `build/` | **权威构建**，只放已验证状态（Release + 全量 LTO） | Lead 统一维护 |
| `build-lto/` | `windows-msvc-release-lto` 的 A/B 对照目录 | Lead 统一维护 |
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

> 要复刻 preset 的 **LTO** 行为，再补 `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
> -DFPGA_LSP_VERIBLE_ALLOW_LTO=ON`；不要用裸 `cmake` 改共享 `build/`。

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
| **生成物未挂接到 target**（首次 configure 时 flex/bison 产物还不存在） | **生成物装配** | ❌ **静默**（库只含空占位源；`verilog-y-cc.lib` 只有 1 KB 而非 2 MB，链接期才报 35 个符号） |
| **Verible 被无条件排除在 LTO 之外**（`INTERPROCEDURAL_OPTIMIZATION OFF`） | **LTO 装配** | ❌ **静默**（构建成功、行为正常，只是没吃到优化；且让「LTO 失败」的归因长期错在 flex 上） |

### 关键规律：**静默类缺陷全部在"文本处理"层**

这是 CMake 脚本化移植的固有风险，也是本项目建立 `ctest -L generator` 的原因：

> **对生成物做断言时，必须断言"结构性不变量"**
> （`#if` 与 `#endif` 数量相等、切片行数在期望区间、`std::move` 出现次数 = 锚点数），
> **而不是只断言"命令执行成功"。**

`tests/check_generated_parser.py` 就是按这个原则写的：它硬编码 bison 3.8.2 的
基线计数，逐项断言，并专门防 `0 == 0` 假通过。

**2026-11-19 补充：还多了一条"装配类"静默缺陷。** 上面两条挂接相关的问题都不是
文本处理，而是**目标源文件列表**与**生成时机**的错配：

- `verible_mount_generated_sources()` 只在 configure 期用 `EXISTS` 判断，而 G1–G6b
  是构建期命令 → 全新目录首次 configure 时挂接被跳过 → 库缺 lexer/parser 源。
  症状要到**链接期**才出现（35 个符号），且**不开 LTO 也一样**，因此极易误判成
  LTO 不兼容。
- 修法：`cmake/scripts/verible-generators.cmake` 增加**配置期预生成**
  （`VERIBLE_PREGENERATE_AT_CONFIGURE`，默认 ON，只补缺失产物），使挂接在同一次
  configure 内完成。`ctest -L generator` 的基线断言仍然保留——它防的是"产物内容错了"，
  而这次修的是"产物根本没进库"，两者互补。

教训：**"能编译"和"目标里真的有这些源文件"是两件事。** 判断后者要查库内容
（`dumpbin /linkermember` 看成员），不要只看构建是否成功。

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

## 交付状态（当前：2026-11-19）

### 构建与测试

| 项目 | 数据 |
| --- | --- |
| **全新目录干净构建** | configure 29 s → build 3.3 min → **844 个目标全部成功**（Release + 全量 LTO） |
| **`ctest` 全量** | **58/58 通过**（`unit` 40 · `e2e` 3 · `formatgate` 4 · `lintgate` 3 · `gate` 2 · `generator` 1 · `mcp` 1 · `td` 3 · `bench` 1） |
| **原生产物** | `build/bin/fpgalsp-native.exe`（**6,132,736 B**，全量 LTCG；SHA256 `1E5B207B…B1595A`） |
| **单元测试规模** | 40 个 gtest target，含本轮新增的 5 条回归断言 |

### 本轮修复内容（审计 `REPORT.md` §0.2 的 1–6 条）

| 编号 | 问题 | 位置 |
| --- | --- | --- |
| SEC-01（High） | `.al` 的 `<Device>` 裸拼进 TD 的 Tcl → 任意命令执行（上轮修复的漏网点） | `server/commands.cpp` |
| SEC-02（Medium） | `ResolveFile` 缺包含校验 → `fpga_format` 可读任意文件；另堵掉 `FormatTool` 两条绕过路径 | `mcp/bridge.cpp` |
| SEC-03/04/05 | SDC 无界递归、`analysis.cpp` 五处裸 `{...}`、MCP TCP 接收缓冲无上限 | `constraints/sdc.cpp`、`analysis/analysis.cpp`、`app/mcp_tcp.cpp` |
| C1/C2（性能 P0） | 每请求两份 `Project` 深拷贝（读路径 31 处调用点）、报告快照整份深拷贝 | `server/*`、`server/commands.cpp` |
| 现代化 | 4 处 `snprintf` 定长缓冲 → `std::format`（截断会切断 UTF-8 多字节序列） | `constraints/sdc.cpp`、`flow/flow_runner.cpp` |

### 静态审计（详见 [`audit/2026-11-19/static/`](audit/2026-11-19/static/)）

| 报告 | 内容 |
| --- | --- |
| [`REPORT.md`](audit/2026-11-19/static/REPORT.md) | 三维度汇总、7 条必办项、方法论与两个计数陷阱、修复路线图 |
| [`SECURITY.md`](audit/2026-11-19/static/SECURITY.md) | 威胁模型 + 15 条发现（全部带调用链）+ 16 项已正确防护 |
| [`PERFORMANCE.md`](audit/2026-11-19/static/PERFORMANCE.md) | 4 条确认 / 4 条可疑 / 76 条噪音逐条判定 |
| [`MODERNIZATION.md`](audit/2026-11-19/static/MODERNIZATION.md) | A/B/C 分级、10 主题记分卡、三批自动修复路线 |
| [`LTCG.md`](audit/2026-11-19/static/LTCG.md) | 5 组对照实验；推翻「flex 生成物与 `/GL` 不兼容」的旧归因 |

### 质量门禁工具（`tools/audit/`）

| 工具 | 用途 |
| --- | --- |
| `run_audit.ps1` | 一键全流程：vcvars 自举 → 编译数据库新鲜度校验 → clang-tidy → MSVC `/analyze` → 汇总 |
| `summarize.py` | 去重口径聚合（`--header-filter` 会让原始条数虚高约 30%）、模块密度、函数长度分布 |
| `check_tcl_invariant.py` | **Tcl 拼装不变量守卫**：禁止把不可信值裸拼进 `Run(...)`。已做阳性对照——对修复前代码报 6 条违规，对当前树 PASS。建议接进 CI |
| `make_clang_db.py` / `run_clang_tidy.py` | cl.exe 命令行 → clang 可消费的编译数据库；单 TU 语法检查（`--verify`） |

> 诊断基线（当前代码）：clang-tidy **8 616 原始 → 6 092 唯一位置**，62/62 TU 全部解析成功；
> MSVC `/analyze` 62 TU **0 条**（control 证明分析器生效，但 `CppCoreCheck.dll` 未安装，
> C26xxx 未覆盖）。

### VS Code 扩展（0.14.0）

```
Y:\fpga-lsp-cpp\vscode-extension\fpga-lsp-anlogic-0.14.0.vsix   224 files / 2.86 MB
  └─ extension/server/bin/fpgalsp-native.exe                 6,132,736 B（全量 LTCG）
```

打包命令：`npm run package`（= `node scripts/bundle-server.js && vsce package …`，
`bundle-server.js` 会从 `build/bin/` 取 exe 塞进 `server/bin/`）。
已核对包内 exe 与 `build/bin` 哈希一致。

---

## 交付状态（历史快照：2026-11-18，Phase B 后）

> ⚠ 以下是当日快照，其中 `ctest 53/53`、exe `5,945,344 B`、扩展 `0.13.0` 等数字**已被
> 后续提交刷新**，当前数字见上一节。保留原文是为了留存 Phase B 的证据链。

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
