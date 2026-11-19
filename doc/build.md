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
| `windows-msvc-release-lto` | Release | 与 release 相同的 LTO 配置，但输出到 `build-lto/`，用于 A/B |

## Release 默认开启全量 LTO

| 项 | 实测值（本机 192 核，`-j24`） |
| --- | --- |
| 全新目录 configure | 约 29 s（含配置期预生成 flex/bison，占数秒） |
| 全新目录全量构建（844 目标） | 约 3.3 min（含 50+ 测试可执行文件） |
| **仅重链**（改一个 .cpp 后） | **约 48 s**（`/LTCG` 全程序分析，比普通链接慢很多） |
| 二进制体积 | 6,132,736 B（同源码非 LTO 为 6,248,960 B，**−1.9%**） |
| 性能 | lint 规则层 CPU **−11.9%**（361 KB 文件 / 1028 条诊断）；纯解析层基本不变 |

只改代码、想快速迭代时用无 LTO 的 `windows-msvc-relwithdebinfo`。

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
ctest --preset windows-msvc-release --output-on-failure
ctest -L unit --output-on-failure          :: 只跑单元测试
```

当前基线：`ctest` **58/58**，其中 `unit` 40 个 target。