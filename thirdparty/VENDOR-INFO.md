# `thirdparty/` —— vendored 依赖清单

本目录下的所有依赖都以**源码形式随仓库提交**（不是 git submodule，也没有 `FetchContent` 网络拉取）。
`cmake/Dependencies.cmake` 用 `add_subdirectory()` 接入，因此**完整 clone 即可构建，无需联网**。

为做到这一点，各依赖自带的 `.git` 目录已删除（否则 git 会把它们记为 gitlink/submodule，
克隆下来是空目录）。来源与版本记录如下，便于将来升级时对照。

## 版本与来源

| 目录 | 上游 | 冻结 commit | 说明 |
| --- | --- | --- | --- |
| `verible/` | https://github.com/chipsalliance/verible | `c6b29d62` | SystemVerilog 解析器 / linter / formatter。已裁剪（见下） |
| `abseil-cpp/` | https://github.com/abseil/abseil-cpp | `b1fc75d2` | Verible 与 re2 的基础库（`absl::Status` 等） |
| `re2/` | https://github.com/google/re2 | `2da00568` | 正则（TD 日志/报告解析用，禁止 `std::regex`） |
| `json/` | https://github.com/nlohmann/json | `abbc2996` | `nlohmann_json` 3.12.0，LSP/MCP/缓存全用它 |
| `spdlog/` | https://github.com/gabime/spdlog | `6d9805df` | 日志（1.17.0），绝不写 stdout |
| `simdutf/` | https://github.com/simdutf/simdutf | `82f457ff` | UTF-8 ↔ UTF-16（LSP 位置换算）9.4.0 |
| `googletest/` | https://github.com/google/googletest | `a46a6012` | 单测框架 1.18.0 |
| `compile-time-regular-expressions/` | https://github.com/hanickadot/compile-time-regular-expressions | `62b06068` | 编译期正则（仅字面量模式） |

## Verible 的裁剪（`verible/`）

按需求**不引入 zlib 与 protobuf**，因此删除了相关代码：

| 删除项 | 原因 |
| --- | --- |
| `verible/verilog/tools/kythe/`（111 文件） | 唯一的 protobuf 使用者 |
| `third_party/proto/kythe/`（5 文件） | Kythe 的 `.proto` |
| `verible/common/util/simple-zip.{cc,h}` + `_test.cc` | 唯一的 zlib 使用者 |
| `third_party/portable_endian/` | 仅 `simple-zip.cc` 使用 |
| `bazel/`、`MODULE.bazel`、`.bazelrc`、`.github/`、`.devcontainer/` | Bazel 构建体系，已由 CMake 取代 |
| `verilog/tools/project/` 中的 kzip 相关引用 | 随 kythe 一并移除 |

**保留了全部 27 个 `BUILD` 文件**——它们是 `tools/gen_verible_cmake.py` 的转译输入，
不要删（虽然其中的 `load("//bazel:...")` 已指向不存在的目录，但脚本会忽略 `load()`）。

详细记录见 `verible/VENDOR-INFO.md`；构建适配的补丁记录见 `verible/PATCHES.md`；
生成器踩坑记录见 `verible/GENERATOR-NOTES.md`。

## 升级注意

1. **不要 `git submodule add`**：本仓库的约定是源码直存。加了 submodule 会让 CI 需要联网。
2. **升级 Verible 后必须重跑三条门禁**：
   ```
   ctest -L "gate|lintgate|formatgate"
   ```
   它们分别验证 解析 / lint / formatter 与官方二进制**逐字节一致**。
   生成器层面的回归还要跑 `ctest -L generator`（14 条断言，含锚点计数基线）。
3. **升级 abseil 要与 Verible 对齐**：Verible 的 shim 只兼容特定 `absl` 版本区间，
   跨大版本容易触发 ODR 问题（详见 `README.md` 的风险画像一节）。

## Removed before publication (2026-11-26)

为了满足公开发布时的许可证与合规要求，以下**不参与编译**的子目录被删除：

| 目录 | 原因 |
| --- | --- |
| `simdutf/benchmarks/` | 内含 **OSL-3.0**（FSF 认定与 GPL 不兼容）与 LGPL-2.1-only 的对比实现，且有 "research purposes only" 与无授权文件（H1） |
| `json/tests/` | 内含 **GPL-3.0-only** 文件（`imapdl/filterbr.py`，H2），与 AGPL-3.0-**or-later** 的表述冲突 |
| `json/docs/` | 内含 4 个 `SIL OFL-1.1` 字体（.woff2），本仓库无 OFL 声明（H2） |
| `asio/` | **未被任何 target 使用**（构建实际用的是 Verible 自带的 asio 用法），保留只会产生 BSL-1.0 的署名义务（M4） |
| `jcxxgen/` | **未被使用**（构建用 `verible/common/lsp/jcxxgen.cc`），且原记录来源写错（M4） |

以上删除都不影响构建：`simdutf` 的 `SIMDUTF_BENCHMARKS`、`json` 的 `JSON_BuildTests` 在 `cmake/Dependencies.cmake` 里本来就是 OFF。
