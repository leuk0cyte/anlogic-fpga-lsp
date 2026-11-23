# 构建与预设

> 面向维护者。只想快速构建的话看仓库根 `README.md` 的「快速开始」。

## 环境要求

| 组件 | 版本 / 位置 |
| --- | --- |
| Visual Studio | **18 Community**（`C:\Program Files\Microsoft Visual Studio\18\Community`） |
| MSVC 工具集 | 14.5x（`VC\Tools\MSVC\14.52.37025`） |
| vcvars | `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"` |
| CMake | ≥ 3.24（本机实测 4.5.0） |
| Ninja | 1.13.0（在 PATH 里） |
| winflexbison | 2.6.4 / bison 3.8.2（生成 Verible 的 flex/bison 产物） |
| C++ 标准 | C++23（`/std:c++23`），静态 CRT（`/MT` / `/MTd`） |

> 只支持 MSVC + Ninja x64。**不支持** GCC / Clang / clang-cl。

本工程**不**在 CMake 里自动调 vcvars，请先进入 MSVC 环境再调 CMake：

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d Y:\fpga-lsp-cpp
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

PowerShell 一条命令：

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset windows-msvc-release && cmake --build --preset windows-msvc-release'
```

**全新目录一次 configure 就能构建成功**（无需 configure 两次）。原因见
[verible-cmake.md](verible-cmake.md) 的「生成物未挂接」：`cmake/scripts/verible-generators.cmake`
会在 configure 期把缺失的 flex/bison 产物补出来，使
`verible_mount_generated_sources()` 在同一次 configure 内即可挂接。

## Preset 一览

| Configure / Build Preset | 构建类型 | 说明 |
| --- | --- | --- |
| `windows-msvc-base` | — | hidden 基础 preset（Ninja、`cl`、静态 CRT、C++23） |
| `windows-msvc` | Release | 默认，等于 `windows-msvc-release` |
| `windows-msvc-debug` | Debug | `/MTd`、`/Od`、PDB，无 LTO |
| `windows-msvc-release` | Release | `/MT`、`/O2`、**全量 LTO**（自有代码 + Verible） |
| `windows-msvc-relwithdebinfo` | RelWithDebInfo | `/MT`、`/O2`、PDB，**无 LTO**（保留可调试性与快速迭代） |
| `windows-msvc-dev` | Release | 与 release 同优化等级但**关闭 IPO**，输出到 `build-dev/`（日常迭代用，见下节实测） |
| `windows-msvc-release-lto` | Release | 与 release 相同的 LTO 配置，但输出到 `build-lto/`，用于 A/B |

## Release 默认开启全量 LTO

| 项 | 实测值（本机 2×EPYC 7K62 = 96 物理核 / 192 逻辑核，512 GB） |
| --- | --- |
| 全新目录 configure | 约 29 s（含配置期预生成 flex/bison，占数秒） |
| 全新目录全量构建（844 目标） | 约 3.3 min（含 50+ 测试可执行文件） |
| **改一处 .cpp 后的增量构建** | **约 139 s** —— 不是 48 s。改一个 TU 会重编它、重链静态库，然后 **59 个可执行文件全部重链**（每个都跑 `/LTCG`：主程序 50 s、每个测试 ≈9.8 s）。只算"主程序重链"是 50 s，但没人只构建主程序 |
| 二进制体积 | 6,132,736 B（同源码非 LTO 为 6,248,960 B，**−1.9%**） |
| 性能 | lint 规则层 CPU **−11.9%**（361 KB 文件 / 1028 条诊断）；纯解析层基本不变 |

只改代码、想快速迭代时用 `windows-msvc-dev` 或 `windows-msvc-relwithdebinfo`（都不开 LTO）。

相关 CMake 开关（默认值保证不改变行为）：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION` | Release preset 里 ON | CMake 级 IPO（`/GL` + `/LTCG`） |
| `FPGA_LSP_VERIBLE_ALLOW_LTO` | OFF | 是否让 Verible 也参与 LTO；**Release preset 里为 ON** |
| `FPGA_LSP_LTO_OWN_CODE` | OFF | 只给 `src/` 自有代码开 `/GL`（对照配置，验证收益用） |
| `VERIBLE_PREGENERATE_AT_CONFIGURE` | ON | configure 期补生成缺失的 flex/bison 产物（全新目录必需） |

> 历史背景：早期两次 LTO 尝试都失败并被错误归因为「flex 生成物与 `/GL` 不兼容」。
> 真实原因有两个叠加——生成物未挂接、以及 `cmake/Verible.cmake` 曾无条件把 Verible 的
> IPO 设为 OFF（等于 Verible 从未进过 LTO 图）。完整实验记录见
> [doc/audit/2026-11-19/static/LTCG.md](audit/2026-11-19/static/LTCG.md)。

## 并行度：192 线程用在了哪里（2026-11-20 实测）

本机是 **2×AMD EPYC 7K62（96 物理核 / 192 逻辑核 / 512 GB）**，Windows 把它们分进多个
processor group。结论先说：**编译阶段早就吃满核数，瓶颈不在那里**。

### 1. 构建阶段拆分（`windows-msvc-release`，删掉自有代码 117 个 obj 后重编）

| 指标 | `-j` 默认 | `-j96` | `-j192` |
| --- | ---: | ---: | ---: |
| 总墙钟 | 141.8 s | 145.0 s | 138.6 s |
| 其中 **编译** 117 个 TU | **24.3 s** | 25.8 s | **23.2 s** |
| 其中 **链接** 72 个目标 | **117.4 s** | 119.2 s | **115.4 s** |

编译 117 个 TU 只花 24 s —— 并行度已经没问题（`-j` 默认 ≈ `-j192`，±2.3% 属噪声，
说明 ninja 认得出这 192 个逻辑核；**不需要**手动传 `--parallel`）。**83% 的时间在链接**。

### 2. 只做链接：加核几乎无效

删掉 60 个 exe 后重链（不动 obj）：

| 只重链 | `-j1` | `-j8` | `-j24` | `-j192` |
| --- | ---: | ---: | ---: | ---: |
| 墙钟 | 1017.2 s | 134.6 s | 138.4 s | 124.9 s |

**有效并行度只有 8.1×**（1017/125）。原因：**单条 `/LTCG` 链接自己就吃 ~5.6 核**
（实测一个 link.exe：CPU 168 s / 墙钟 30 s / 工作集 1.8 GB），58 条并发 ≈ **330 核的瞬时需求**，
已经超过 192 个逻辑核 —— 所以 8 核和 192 核几乎同一时间，加核没有用。

再看"这些链接工作有多少是交付需要的"：59 个 exe 串行链接合计 1009.4 s，其中
`fpgalsp-native.exe` 只占 **31.9 s（3.2%）**，另外 58 个（46 个 `unit_*` 643.4 s、
7 个 `perf_*` 206.0 s、5 个工具 128.2 s）占 **96.8%**。也就是说构建时间的绝大部分花在
**测试/探针可执行文件的全程序 LTO** 上，而那 58 个二进制不需要 LTO。

### 3. ctest 之前是**串行**的

`ctest` 默认不加 `-j`，63 个用例一个个跑：**92–104 s**。改成并行后（preset 里已设为 8）：
（下表是 2026-11-19 那次实验的原始数据；用例数已增长到 65，数字仅作并行收益的证据。）

| ctest | 串行 | `-j8` | `-j24` | `-j48` | `-j96` |
| --- | ---: | ---: | ---: | ---: | ---: |
| 墙钟 | 92.5 / 103.7 | **59.2 / 59.9** | 59.1 / 59.7 | 59.8 / 60.7 | 59.4 / 60.2 |
| 结果 | 63/63 | 63/63 | 63/63 | 63/63 | 63/63 |

- 8 路就到地板，再加核没有收益：下界 = **10 个 `RUN_SERIAL` 用例之和 49.9–51.7 s**
  （其中 `td.bench` 17.6 + `td.flow` 9.4 + `td.diagnostics` 7.7 + `td.session` 7.7 ≈ 42 s，
  它们必须串行，否则 `td-process` 资源锁互斥），其余 53 个并行后只剩约 9 s。
- 用例本身有并发防护（`td.*` 与 5 条 e2e 都带 `RUN_SERIAL TRUE` + `RESOURCE_LOCK "td-process"`），
  5 组 × 2 次共 **10 次 63/63**，无 flake。
- **测试临时目录也按构建目录隔离**：preset 里把 `TMP`/`TEMP` 指向 `<build>/Testing/tmp`
  （`tests/CMakeLists.txt` 里 `file(MAKE_DIRECTORY ...)` 建好）。这样 `build/` 与
  `build-dev/` 两棵树**可以同时跑单元测试**（实测两边各 43/43）。
  没有这层隔离时，两棵树会在系统 `%TEMP%` 里抢同名夹具目录，表现为"FileCount 只有 1/2、
  `file_updates==0`"这种看着像产品 bug 的假失败。
  ⚠ 但两棵树**不要同时跑 `td.*` / `e2e.*`**：`RESOURCE_LOCK` 只在单个 ctest 进程内生效，
  跨进程仍会抢 TD 会话。
- ⚠ 唯一会"假失败"的情形：**一边构建一边跑 ctest**。构建会删/重链 `build/bin/*.exe`，
  此时 ctest 报 `***Not Run`（找不到可执行文件），不是用例本身的问题。

### 4. 建议

| 场景 | 命令 | 实测 |
| --- | --- | --- |
| 日常改代码 + 跑测试 | `cmake --preset windows-msvc-dev` → `cmake --build --preset windows-msvc-dev` → `ctest --preset windows-msvc-dev` | 增量 **32 s** + 测试 **62 s** |
| 发版 / 性能数字 / 交付二进制 | `cmake --preset windows-msvc-release` → `cmake --build --preset windows-msvc-release` → `ctest --preset windows-msvc-release` | 增量 **139 s** + 测试 **62 s** |
| 全新目录 | 同上 | 无 LTO 全量 **57 s**（491 个 TU）；LTO 全量约 **3.3 min** |

`build-dev/` 与 `build/` 互不干扰（`.gitignore` 已覆盖 `build-*/`），可以同时存在。

### 5. 还能再快，但要改 CMake 结构（未做）

1. **只给 58 个测试/探针目标关 IPO**（保留 `fpgalsp-native.exe` 的 LTO）：按上面的账，链接工作
   的 96.8% 与交付物无关，理论上可把构建从 142 s 压到 62–68 s。
   ⚠ 有一处**需要先验证**：静态库本身是 `/GL` 对象，链接器看到 IL 仍可能触发 LTCG，
   那样收益就没有估算的那么大 —— 得先做一次"只关测试目标 IPO"的实验再定。
2. **合并测试可执行文件**：58 个 exe 合成少数几个（每个原用例仍注册成独立 ctest，用
   `--gtest_filter=<Suite>.*` 选择），把 58 次 LTCG 降到 1–2 次。
   代价：ctest 名字与 `ctest -R` 过滤表达式要跟着改（现有文档、`tools/perf/gate/verify.ps1`、
   多份审计记录都按 `unit.<name>` 点名），所以先记录不实施。
3. `td.bench`（17.6 s）如果移出默认集合，ctest 还能从 ~60 s 降到 ~45 s。

> 完整测量数据与脚本见 `build/_audit_sp1/parallel/REPORT.md`（构建目录内，未提交）。

## 依赖

全部 vendored 在 `thirdparty/`，构建不依赖网络。

| 依赖 | 版本 | 接入方式 | 使用方 |
| --- | --- | --- | --- |
| abseil-cpp | vendored（无版本锚点） | `add_subdirectory`（**必须先于 re2**） | Verible / re2 |
| re2 | `2025-11-05`（SONAME 11） | `add_subdirectory` | Verible lexer 正则、本项目字符串匹配 |
| nlohmann json | 3.12.0 | header-only | LSP JSON-RPC 消息 |
| googletest | 1.18.0 | `add_subdirectory` | 单元测试 |
| spdlog | 1.17.0 | `add_subdirectory`（内置 fmt） | 日志 |
| simdutf | 9.4.0 | `add_subdirectory` | UTF-8/UTF-16 转换 |
| compile-time-regular-expressions (ctre) | 3.11.0 | header-only | 编译期正则 |
| Verible | vendored（未打标签） | Bazel → CMake 移植（`cmake/Verible.cmake`） | SystemVerilog 解析/lint/format |

**明确不引入** zlib 和 protobuf（原 Bazel 构建中的这两项依赖已从源码中剥离）。

可链接的 target 名：

```
absl::*                       (abseil 自带)
re2::re2
nlohmann_json::nlohmann_json
GTest::gtest / GTest::gtest_main
spdlog::spdlog
simdutf::simdutf
ctre::ctre / fpga_lsp::ctre
fpga_lsp::verible             (Verible 聚合 facade)
verible_lint_rules_all        (lint 规则聚合库，必须 /WHOLEARCHIVE 链入)
```

> **供应链现状（2026-11-19 审计结论）**：`thirdparty/` 全部入库（7,338 个文件），构建
> 完全封闭、不存在"拉错版本"的风险；代价是上游修复要手工合入。其中 **abseil 没有版本
> 锚点**（`MODULE.bazel` 里 `version = "head"`、CMake `project(absl)` 无 VERSION），
> Verible 也未打标签——因此无法自动回答"落后哪些 CVE"。建议补
> `thirdparty/*/UPSTREAM-VERSION.txt`（上游 tag + commit + 同步日期）。

## 用 Visual Studio 打开这个工程

两种方式，**都不要再让 VS 用默认的 `build/` 覆盖发布构建**：

1. **推荐：用 preset（Ninja）**
   VS 的「打开文件夹」会读 `CMakePresets.json`，直接选 `windows-msvc-dev`（日常）
   或 `windows-msvc-release`（发布）。这两条 preset 已经把 `cl` / 静态 CRT / C++23 / LTO 都写齐了。

2. **用 VS 自己的生成器（MSBuild，`Visual Studio 18 2026`）**
   也可以，已修到能 configure + 生成 + 编译出 `fpgalsp-native.exe`（0.36.0 实测）。
   代价：MSBuild 下没有 Ninja 的 target 级并行，且 `/MP` 会额外开线程，比 Ninja 慢不少；
   构建目录请**显式指定到别处**（例如 `build-vs/`），不要指向 `build/`。

> ⚠ **VS 默认会往 `build/` 里 configure**（并把它改成 Debug、关掉 LTO）。真发生过：
> 共享 `build/` 的 cache 变成 `CMAKE_BUILD_TYPE=Debug`，随后发布构建产出一个 21 MB 的
> 非 LTO 二进制（正常是 ~7.1 MB）。如果发现 `build/bin/fpgalsp-native.exe` 突然变大，
> 用 `cmake --preset windows-msvc-release` 重新配置后重编即可恢复。

### 打开 VS 时曾经报的两个错（已修）

| 报错 | 原因 | 修法 |
| --- | --- | --- |
| `set_target_properties can not be used on an ALIAS target`（`cmake/Dependencies.cmake:185`） | `ctre::ctre` 是 `add_library(ctre::ctre ALIAS ctre)`，CMake 不允许对 ALIAS 设属性 | 用 `get_target_property(... ALIASED_TARGET)` 找到真实 target 再设 |
| `未指定编译器。本项目仅支持 MSVC...`（`CMakeLists.txt:21`） | VS 生成器在 configure 前**不会**设置 `CMAKE_C/CXX_COMPILER`（它自己挑 MSVC），被这条守卫拦下 | 守卫放行 `Visual Studio` 生成器 |

### 生成物自检（G1/G3）

`win_flex` 的两份输出（`verilog.yy.cc` / `command-file.yy.cc`）曾经**互相覆盖**过：`verilog.yy.cc`
里出现了 `#define yyFlexLexer veribleCommandFileFlexLexer`，症状是一串看不懂的
`LNK2005: veribleCommandFilealloc/free/... 已在 ... 中定义`。

现在生成后立刻校验前缀（`verible-patch.cmake` 的 `verify_flex` 模式），失败会直接说清楚：

```
代码生成校验失败：<build>/verible/gen/verible/verilog/parser/verilog.yy.cc
  期望 '#define yyFlexLexer verilogFlexLexer'，实际 '... veribleCommandFileFlexLexer'
  修法：删除该构建目录下的 verible/gen 后重新构建
```

## 构建目录纪律：共享 `build/` 只放已验证状态

**多人/多 agent 同时往一个构建目录写，必然互相踩。** 本项目已实际发生过两次：

- **`CMAKE_CXX_FLAGS` 被清空**：有人用裸 `cmake -S . -B build -DCMAKE_CXX_COMPILER=cl`
  绕过 preset 配置，CMake 的 MSVC 默认 flag 初始化没跑 → `/EHsc` 与 `/O2` 全丢。
  症状极具迷惑性：`C4530` 警告 + 凡是"抛-捕"的测试都 `std::terminate` abort
  （看起来像代码 bug，实际是编译选项问题）。
- **`LNK1104`**：一个构建删掉了另一个构建正在链接的 `.obj`。

| 目录 | 用途 | 谁维护 |
| --- | --- | --- |
| `build/` | **权威构建**，只放已验证状态（Release + 全量 LTO） | Lead 统一维护 |
| `build-lto/` | `windows-msvc-release-lto` 的 A/B 对照目录 | Lead 统一维护 |
| `build-<topic>/` | 各人/各任务的迭代目录 | 各自维护 |

**永远不要用裸 `cmake -S . -B build` 配置共享目录** —— 必须走 preset。

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

## 恢复被污染的共享构建目录

症状：`CMAKE_CXX_FLAGS` 为空、`C4530` 增多、抛-捕测试 abort。

```powershell
Remove-Item build\CMakeCache.txt, build\CMakeFiles -Recurse -Force
cmake --preset windows-msvc-release
```

## 测试

```bat
ctest --preset windows-msvc-release          :: 8 路并行（preset 里 execution.jobs=8）
ctest -L unit --output-on-failure          :: 只跑单元测试
ctest -L docs                              :: 版本一致性 / 工程状态目录门禁
ctest -L parity                            :: 报表 / 约束解析对等门禁（需要 Y:\fpga-lsp 参考实现）
ctest -j 1                                  :: 想复现问题/看干净时序时用串行
```

当前基线（2026-11-23，扩展 0.30.0）：`ctest` **72/72**（并行 8 路约 **230 s**，含 clang-tidy 门禁 `docs_clang_tidy` 约 145 s）；只想跑快的用 `ctest -E docs_clang_tidy`（71 项，约 90 s）。
其中 `unit` 46 个 target，另有 `e2e.*` 5 条、`td.*` 3 条、门禁若干（`gate`/`lintgate`/`formatgate`/`docs`）。
并行之所以安全：`td.*` 与 5 条 e2e 都带 `RUN_SERIAL TRUE` + `RESOURCE_LOCK "td-process"`。
详见上文「并行度：192 线程用在了哪里」。

> ⚠ **不要一边构建一边跑 ctest**：构建会删除并重链 `build/bin/*.exe`，此时 ctest 会报
> `***Not Run`（找不到可执行文件），看起来像用例失败，其实是构建在改文件。