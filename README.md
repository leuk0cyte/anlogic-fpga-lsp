# fpga-lsp-cpp

Anlogic（安路）TangDynasty FPGA 的 **LSP + MCP 语言服务器**，C++23 实现。

单一静态可执行文件 `fpgalsp-native.exe`，内嵌 [Verible](thirdparty/verible) 的
SystemVerilog 解析 / lint / 格式化；既是 LSP 服务端（供 VS Code 扩展使用），
也是 MCP 服务端（供 Copilot / Cline 等 agent 调用）。

本工程是 [`fpga-lsp`](../fpga-lsp)（约 11.8k 行 Python 实现）的 C++ 重写，行为以
Python 版为规格对照，等价性证据见 [`doc/audit/`](doc/audit/)。

## 特性

- **LSP**：补全、悬停、定义、引用、文档/工作区符号、代码动作、格式化（全文档 + 区间）、
  重命名、调用层次、语义高亮、折叠、inlayHint、报告视图 —— 共 25 个方法。
- **MCP**：47 个工具，覆盖工程结构、符号、诊断、报告、流程控制、约束审计、片上探针；自带面向无上下文 agent 的使用帮助（`initialize.instructions` + `fpga_guide` 工具 + 4 个工作流 prompt，见 [doc/mcp-usage.md](doc/mcp-usage.md)）；
  默认只读，`--allow-flow` / `--allow-write` 显式开启副作用。
- **TD 集成**：TangDynasty 会话与诊断、Synthesis / Place&Route / BitWizard、
  报告解析（QoR / timing / area / DRC-CDC）。
- **约束与索引**：ADC 引脚约束、SDC 语法与补全、HDL 模块/实例/端口索引。

## 快速开始

**要求**：Windows x64、Visual Studio 18（MSVC 14.5x）、CMake ≥ 3.24、Ninja、
[winflexbison](https://github.com/lexxmark/winflexbison)。第三方依赖全部 vendored，不需要网络。

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d Y:\fpga-lsp-cpp

cmake --preset windows-msvc-release      :: Release，含全量 LTO
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release      :: 8 路并行
```

> 只是改代码、跑测试的话用 **`windows-msvc-dev`**（同优化等级、关 LTO，输出 `build-dev/`）：
> 增量构建 **32 s** vs LTO 版 **139 s**。LTO 版 83% 的时间花在 58 个测试可执行文件的
> `/LTCG` 链接上，那部分不随核数扩展 —— 详见 [doc/build.md](doc/build.md) 的
> 「并行度：192 线程用在了哪里」。

产物在 `build/bin/fpgalsp-native.exe`。运行：

```bat
fpgalsp-native.exe --stdio                                   :: LSP
fpgalsp-native.exe --mcp                                     :: MCP over stdio
fpgalsp-native.exe --mcp --mcp-port 7788 --root <工程根>      :: MCP over loopback TCP
fpgalsp-native.exe --help
```

改代码想快速迭代时用无 LTO 的 `windows-msvc-dev`（增量 32 s；Release 的 LTO 版要 142 s，因为
58 个测试可执行文件每个都做一次全程序 LTCG）。
更多构建细节、预设与排错见 [doc/build.md](doc/build.md)。

## VS Code 扩展

```powershell
code --install-extension vscode-extension\fpga-lsp-anlogic-0.18.3.vsix
```

扩展会优先使用随包内置的服务端。开发、测试与打包见
[doc/extension-development.md](doc/extension-development.md)。

## 目录结构

```
cmake/               构建脚本：依赖接入、Verible 移植、flex/bison 生成
src/
  platform/          平台层：UTF-16 位置换算 / 路径 / 日志 / 编码
  jsonrpc/           JSON-RPC 传输与分派（LSP Content-Length / MCP newline）
  lsp/ server/       LSP 服务端：文档、诊断、索引服务、各 feature
  mcp/               MCP 服务端与工具
  hdl/ semantics/    HDL 索引、语义高亮
  constraints/       ADC / SDC 解析、诊断、补全
  reports/ flow/ td/ 报告解析、流程运行、TangDynasty 会话
  project/ analysis/ .al 工程解析、DRC/CDC 分析
  verible_bridge/    内嵌 Verible 的 lint / format 封装
  app/               fpgalsp-native 入口
tests/               unit / td / perf / gate-corpus / golden
tools/               audit（静态分析流水线）、perf（性能脚手架）
doc/                 项目文档（见下）
thirdparty/          vendored 依赖（只读）
vscode-extension/    VS Code 扩展
```

## 依赖

全部 vendored 在 `thirdparty/`（abseil、re2、nlohmann/json、spdlog、simdutf、ctre、
googletest、Verible）。版本与供应链说明见 [doc/build.md](doc/build.md#依赖)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [doc/README.md](doc/README.md) | 文档索引 |
| [doc/build.md](doc/build.md) | 构建、预设、LTO、排错 |
| [doc/status.md](doc/status.md) | 当前交付状态、已知差异、未验证项 |
| [doc/quality.md](doc/quality.md) | 静态分析流水线与审计报告索引 |
| [doc/verible-cmake.md](doc/verible-cmake.md) | Verible CMake 移植的风险画像（踩坑记录） |
| [doc/commands.md](doc/commands.md) | LSP 命令清单 |

## 许可

AGPL-3.0-or-later —— 许可正文见 [vscode-extension/LICENSE](vscode-extension/LICENSE)。
Copyright (C) 2026 leuk0cyte.