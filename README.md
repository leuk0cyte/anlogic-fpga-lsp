# fpga-lsp-cpp

Anlogic（安路）TangDynasty FPGA 的 **LSP + MCP 语言服务器**，C++23 实现。

单一静态可执行文件 `fpgalsp-native.exe`，内嵌 [Verible](thirdparty/verible) 的
SystemVerilog 解析 / lint / 格式化；既是 LSP 服务端（供 VS Code 扩展使用），
也是 MCP 服务端（供 Copilot / Cline 等 agent 调用）。

本工程是 [`fpga-lsp`](../fpga-lsp)（约 11.8k 行 Python 实现）的 C++ 重写，行为以
Python 版为规格对照，等价性证据见 [`doc/audit/`](doc/audit/)。

## 特性

- **LSP**：补全、悬停、定义、引用、文档/工作区符号、代码动作、格式化（全文档 + 区间）、
  重命名、调用层次、语义高亮（full/delta/range）、折叠、inlayHint、签名帮助、文档链接、
  codeLens、文档高亮、selectionRange、pull diagnostics（textDocument/workspace）、
  报告视图 —— 注册 **47 个 handler**（标准 LSP 方法 + `fpgaLsp/*` 自定义方法 +
  `workspace/executeCommand`；后者广播 **31 条**命令，共实现 **34 条**）。
- **MCP**：**50 个工具**（`--allow-flow` 可见 46、只读 38；`--allow-write` 再加 4：`fpga_project_add_source` / `fpga_project_rename` / `fpga_project_update` / `fpga_probe_config`），覆盖工程结构、符号、诊断、报告、流程控制（含单步 `fpga_flow_run_step`）、约束审计、片上探针；自带面向无上下文 agent 的使用帮助（`initialize.instructions` + `fpga_guide` 工具 + 4 个工作流 prompt，见 [doc/mcp-usage.md](doc/mcp-usage.md)）；
  **服务端默认只读**，`--allow-flow` / `--allow-write` 显式开启副作用；VS Code 扩展默认
  `fpgaLsp.mcp.allowFlow=true`（因此装好扩展即有 flow 能力），写操作仍默认关闭。
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
> 增量构建 **32 s** vs LTO 版 **139 s**。LTO 版 83% 的时间花在约 60 个测试/工具可执行文件的
> `/LTCG` 链接上，那部分不随核数扩展 —— 详见 [doc/build.md](doc/build.md) 的
> 「并行度：192 线程用在了哪里」。

产物在 `build/bin/fpgalsp-native.exe`。运行：

```bat
fpgalsp-native.exe --stdio                        :: LSP（Content-Length framing）
fpgalsp-native.exe --mcp                          :: MCP over stdio（newline-delimited）
fpgalsp-native.exe --stdio --mcp-port 7788        :: LSP 进程内挂 loopback MCP（共享同一 Engine）
fpgalsp-native.exe --mcp --mcp-port 7788 --root <工程根>   :: 独立进程的 MCP over loopback TCP
fpgalsp-native.exe --mcp --attach-port 7788       :: 薄代理：接到上面那个 LSP 进程的 MCP
fpgalsp-native.exe --help                         :: 首行自报版本号（与扩展一致）
```

`--mcp-port` 在两种模式下都生效；`--mcp` 模式的 TCP 入口每个连接自建 Bridge（没有可共享的
LSP Engine），进程在 stdin 关闭时退出 —— 要当常驻服务用时从终端启动。
更多构建细节、预设与排错见 [doc/build.md](doc/build.md)。

## VS Code 扩展

```powershell
code --install-extension vscode-extension\fpga-lsp-anlogic-0.33.0.vsix
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
| [doc/status.md](doc/status.md) | 按轮记录的工程日志（当前 34 轮）+ 交付状态、已知差异、未验证项 |
| [doc/quality.md](doc/quality.md) | 静态分析流水线与审计报告索引 |
| [doc/commands.md](doc/commands.md) | `executeCommand` / `fpgaLsp/*` 落点索引 + initialize 能力对照 |
| [doc/mcp-usage.md](doc/mcp-usage.md) | MCP 工具怎么串：三条路线、jobs 语义、常见坑（agent 入口） |
| [doc/protocol.md](doc/protocol.md) | 可执行入口、两种传输模式、framing 与消息上限 |
| [doc/conventions.md](doc/conventions.md) | 编码约定与移植铁律 |
| [doc/extension-development.md](doc/extension-development.md) | VS Code 扩展的开发、测试与打包 |
| [doc/verible-cmake.md](doc/verible-cmake.md) | Verible CMake 移植的风险画像（踩坑记录） |
| [doc/audit/2026-11-22/SUMMARY.md](doc/audit/2026-11-22/SUMMARY.md) | 最近一次独立审计（功能齐全度 + 文档同步度）与修复记录 |

## 许可

AGPL-3.0-or-later —— 许可正文见 [vscode-extension/LICENSE](vscode-extension/LICENSE)。
Copyright (C) 2026 leuk0cyte.