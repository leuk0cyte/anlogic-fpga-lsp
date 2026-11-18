# Anlogic FPGA LSP — TangDynasty / TD 工程语言支持与 IDE 视图

> 作者：**leuk0cyte** · 图标：带 “TD” 的蓝色芯片（`resources/icon.svg`、`resources/icon.png`）

这是 Anlogic TangDynasty（TD）FPGA 工程的 VS Code 扩展，配合内置的 `fpga-lsp` Python 语言服务器使用。

扩展分两层：

1. **标准 LSP**：通过 `vscode-languageclient` 提供 `.v/.sv/.vhd`、`.adc`、`.sdc`、`.al` 文件的诊断、补全、悬停、定义、引用、重命名、符号、折叠、Inlay Hints、语义高亮等能力。诊断来源包括离线分析和 TD 的权威 `read_hdl` / `read_adc`。
2. **IDE 视图**：通过 fpga-lsp 的 v2 自定义协议提供模块层次树、Project & Flow 树、Synthesis / Place & Route 报告页，以及一键综合、布局布线、报告查看等操作。

所有界面文字、按钮、命令标题使用英文；本文档使用中文。

---

## 主要功能

- **模块层次树**：自动生成 `top` 的实例层次，点击模块跳定义，点击黑盒/IP 跳例化处；VHDL entity 和 IP 生成的同名 Verilog 模块会显示为已定义模块，而不是 blackbox。
- **导航与编辑**：定义、引用、补全、悬停、重命名、调用层次、workspace symbol、信号级符号、generate 作用域重命名。
- **TD 诊断**：保存时运行 TD 权威检查（`read_hdl` + `read_adc`），Problems 中显示 `HDL-xxxx` / `USR-xxxx` 等诊断；TD 的黑盒/ignored 警告也会显示。
- **一键 Flow**：Synthesis、Place & Route、Full Flow、取消、增量布线，默认 `jobs=6`，行为与 TD GUI 一致。
- **Flow 日志**：`Output → FPGA Flow` 实时显示 TD 输出；`run.log` 中的新 ERROR/WARNING 会加 `[run.log]` 前缀转发；Flow 的错误/警告同时进入 Problems（source `fpga-flow`）。
- **独立报告页**：**Reports** 是独立的活动栏视图，与 Project & Flow 平级；`Synthesis Reports` 和 `Place & Route Reports` 两个页面，各自包含 Dashboard、QoR、Timing、Area、Flow、DRC-CDC、Compare，并支持搜索。
- **资源利用率**：Area 页显示器件利用率，并计算每个模块的 LUT / SEQ / BRAM / DSP 使用率；兼容 EF2 以及 PH1 的 `#slice reg` / `#slice*` / `logic lut` / `eram` 格式。
- **Verible 集成**：格式化、lint/autofix、细粒度语法高亮；检测到 Verible 时自动使用，找不到时自动回退内置实现。
- **MCP server**：内置 MCP stdio server，把工程、符号、诊断、报告、flow 等能力暴露给 Copilot / Cline / 其他 agent；默认只读，可显式开启 flow/写操作。
- **器件与约束**：TD 安装探测、器件引脚表缓存、ADC 引脚约束补全/校验、SDC 支持、多 TD 安装选择。
- **工程管理**：子目录 `.al` 自动发现、重命名工程、`open_project -update`、添加源文件、查重名 module、层次搜索。
- **代码片段**：Verilog / SystemVerilog / VHDL / ADC / SDC snippets；输入 `module`、`always_ff`、`entity`、`process`、`pin`、`clock` 等前缀即可展开。

---

## 快速开始

### 1. 前置条件

```powershell
python --version        # 需要 Python 3.9+，且 python 在 PATH 上
```

- TD 默认自动探测 `C:\Anlogic\TD_Release_*` 等安装目录；探测不到时设置 `fpgaLsp.tdPath`。
- Verible 可选；未安装时格式化/高亮会降级，不影响 TD 流程。

### 2. 安装扩展并重载

```powershell
code --install-extension Y:\fpga_lsp\vscode-extension\fpga-lsp-anlogic-0.10.32.vsix
```

安装后按提示 **Reload**，或执行 `Developer: Reload Window`。

### 3. 打开工程文件夹

用 `File → Open Folder…` 打开包含 `.al` 文件的工程根目录，不要只打开单个文件。

找不到 `.al` 时没有模块树/诊断；可以用 `FPGA: Select Project…` 手动选择。

### 4. 打开 FPGA 活动栏

左侧出现 FPGA 图标后，可以看到：

- **Module Hierarchy**
- **Project & Flow**
- **Reports**

### 5. 运行 Flow

命令面板输入 `FPGA:`，或使用视图标题栏按钮：

| 操作 | 命令 |
| --- | --- |
| 只综合 | `FPGA: Run Synthesis` |
| 只布局布线 | `FPGA: Run Place & Route` |
| 综合 + 布局布线 + bit | `FPGA: Run Full Flow` |
| 增量布线 | `FPGA: Re-run Place && Route` |
| 取消 | `FPGA: Cancel Flow` |
| 查看报告 | `FPGA: Show Reports` 或打开 **Reports** 视图 |

默认 jobs 数为 **6**，与 TD GUI 默认一致。运行时会显示可取消的进度通知，状态栏显示当前阶段和计时。

### 6. 查看报告

**Reports** 视图标题栏提供两个独立页面：

- **Synthesis Reports**
- **Place & Route Reports**

每页都有 Dashboard / QoR / Timing / Area / Flow / DRC-CDC / Compare，并带搜索框。数据来自 TD 生成的报告文件，不是估算。

---

## 功能说明

### 编辑与导航

- `F12` / Ctrl+Click：跳转定义。
- `Shift+Alt+F12`：查找引用（模块、端口、信号）。
- `F2`：重命名；generate 块内只改当前作用域。
- `Show Call Hierarchy`：查看谁例化了我、我例化了谁。
- `Ctrl+T`：搜索模块、端口、wire/reg/parameter。
- `Format Document` / `Format Selection`：优先 Verible，未配置时回退内置缩进器。
- `Ctrl+.`：插入例化、ADC 约束快速修复等 code action。
- 编辑器标题栏按钮：**Check Read Design**，单独跑 TD `read_design` 检查并把 HDL/TD 错误写入 Problems；复制例化文本在命令面板。

### TD 诊断

- 保存 `.v/.sv` 时运行 TD 权威检查，Problems 同时显示 TD 和 Verible 诊断。
- TD 的 `HDL-1007` 等无 severity、相对路径形式也会解析并映射回源文件。
- TD 因 error 把文件 ignored 时，会提示该模块变成了黑盒，TD lint 在修复前不可用。

### Module Hierarchy

- 节点显示 `module` + 实例名；黑盒/IP、原语、递归例化有不同图标。
- 点击已定义模块打开定义；未定义节点打开例化处。
- 右键：Go to Definition / Go to Instantiation / Copy File Path。
- 标题栏：刷新、批量添加源文件到 `.al`（文件对话框支持多选）、检索层次、查重名 module。

### Project & Flow

- **Project**：工程名、器件、top、`.al` 路径；**Project file** 行的切换按钮可在多个 `.al` 之间切换。
- **Flow**：当前 run/stage、耗时、最后一条消息、各阶段状态。
- **Artifacts**：bit、数据库、run 目录。
- **Reports**：报告文件列表。
- 标题栏：综合、布局布线、全流程、取消、报告、刷新。

### Flow 失败与日志

- Flow 运行过程中会读取 TD 报告的实际 `run.log` 路径（来自 `PRJ-1404`）。
- 失败时输出会包含失败步骤（例如 `.read_design.error.f`）和 run.log 路径/缺失状态；有 run.log 时会读取最后的 ERROR/WARNING。
- 所有问题同时进入 **Problems**，source 为 `fpga-flow`；无法定位文件的消息挂到 `.al`。
- 完整日志在 `.fpgalsp/flow/<runId>/td_*.log`，以及 `<project>_Runs/<run>/run.log`（TD 不一定生成）。

### Reports

- **Synthesis Reports** / **Place & Route Reports** 是独立页面，互不影响。
- Timing 页结构化展示 metadata、Check Status、Clock Summary、Path Group、每条路径的详细字段、delay 表和完整 raw 报告。
- Area 页展示器件利用率、LUT 统计和每模块资源利用率；支持 PH1 的 slice/slice reg/eram 字段。
- 页面顶部搜索框可过滤表格行、路径和 raw 行。

### Verible

Verible 是可选的，但推荐安装：

```powershell
# 示例：C:\Users\<you>\verible
```

- 格式化：`verible-verilog-format`，通过语言服务器调用。
- 高亮：检测到 `verible-verilog-syntax` 时使用细粒度 token，否则回退内置高亮。
- Lint/quick fix：`fpgaLsp.verible.enable=true` 时启动 `verible-verilog-ls`，诊断来源标记为 `verible`。
- Autofix：检测到 `verible-verilog-lint` 时提供 `Verible: apply lint autofixes`。
- 找不到 Verible：基础功能正常，格式化回退内置缩进器，不会阻止插件启动。
- `FPGA: Verible Status` 查看当前探测结果。

### MCP（Copilot / Cline / 其他 agent）

内置 MCP stdio server，默认暴露只读工具；扩展默认允许 flow，`allowWrite` 仍关闭。

- 默认不注册动态 provider（`fpgaLsp.mcp.dynamicProvider=false`），避免 Copilot 无法关联 workspace。
- 安装工作区配置：

```text
FPGA: Install MCP Configuration
```

选择 **VS Code workspace**（推荐第一个选项）或 portable `.mcp.json`。

- `fpgaLsp.mcp.allowFlow` 默认 `true`；`allowWrite` 默认 `false`。
- 新工具：`fpga_flow_log`、`fpga_flow_issues`、`fpga_flow_wait`（带 progress 通知）、`fpga_read_file`、`fpga_search_text`、`fpga_constraint_audit`。
- 设置 `fpgaLsp.mcp.allowFlow` / `fpgaLsp.mcp.allowWrite` 可开启 flow/工程写操作。
- 完整工具目录与安全限制见仓库根目录 `MCP.md`。

### ADC / SDC

- `.adc` 引脚约束支持补全、诊断、一键补 `IOSTANDARD`、删除分配。
- 可校验可用 IO、专用/SerDes 引脚、重复/冲突。
- `.sdc` 支持语法诊断、补全和定义跳转。

---

## 设置

| 设置 | 默认值 | 说明 |
| --- | --- | --- |
| `fpgaLsp.pythonPath` | `python` | 运行语言服务器的 Python |
| `fpgaLsp.serverRoot` | `""` | 语言服务器目录；vsix 自动使用内置 `server/` |
| `fpgaLsp.tdPath` | `""` | TD 安装根目录，空 = 自动探测 |
| `fpgaLsp.tdCheckOnSave` | `true` | 保存时运行 TD 权威检查 |
| `fpgaLsp.projectFile` | `""` | 显式指定 `.al` |
| `fpgaLsp.flowJobs` | `6` | flow 并行 jobs（1–64） |
| `fpgaLsp.logFile` | `""` | 服务器日志，空 = `<workspace>/.fpgalsp/logs/server.log` |
| `fpgaLsp.verible.path` | `""` | Verible bin 目录或可执行文件；空 = PATH/常见目录 |
| `fpgaLsp.verible.enable` | `true` | 启动 `verible-verilog-ls` 诊断/quick-fix 客户端 |
| `fpgaLsp.verible.format` | `true` | 使用 Verible 格式化 |
| `fpgaLsp.verible.args` | 160 列 + 对齐参数 | 传给 `verible-verilog-format` |
| `fpgaLsp.verible.highlight` | `true` | 使用 Verible 语法 token |
| `fpgaLsp.verible.autofix` | `true` | Verible lint autofix |
| `fpgaLsp.verible.lintArgs` | `--rules=+line-length=length:160` | 传给 `verible-verilog-lint` |
| `fpgaLsp.verible.command` | `""` | 自定义 Verible LS 命令 |
| `fpgaLsp.verible.lsArgs` | `--rules=+line-length=length:160` | 传给 Verible LS；与格式化 160 列一致 |
| `fpgaLsp.mcp.allowFlow` | `true` | 允许 MCP 执行 flow |
| `fpgaLsp.mcp.allowWrite` | `false` | 允许 MCP 修改工程 |

默认的 `fpgaLsp.verible.args`：

```json
[
  "--column_limit=160",
  "--indentation_spaces=4",
  "--wrap_spaces=4",
  "--try_wrap_long_lines=true",
  "--assignment_statement_alignment=align",
  "--case_items_alignment=align",
  "--distribution_items_alignment=align",
  "--enum_assignment_statement_alignment=align",
  "--module_net_variable_alignment=align",
  "--class_member_variable_alignment=align",
  "--struct_union_members_alignment=align",
  "--parameter_declaration_alignment=align",
  "--port_declarations_alignment=align",
  "--formal_parameters_alignment=align",
  "--named_parameter_alignment=align",
  "--named_port_alignment=align",
  "--port_declarations_right_align_packed_dimensions=true",
  "--port_declarations_right_align_unpacked_dimensions=true",
  "--formal_parameters_indentation=indent",
  "--named_parameter_indentation=indent",
  "--named_port_indentation=indent",
  "--port_declarations_indentation=indent",
  "--alignment_group_boundary=blank-lines-and-separator-comments"
]
```

---

## 常用命令

| 命令 | 作用 |
| --- | --- |
| `FPGA: Run Synthesis` | 只综合 |
| `FPGA: Run Place & Route` | 只布局布线 |
| `FPGA: Run Full Flow` | 综合 + 布局布线 + bit |
| `FPGA: Re-run Place && Route` | 增量布线 |
| `FPGA: Cancel Flow` | 取消 flow |
| `FPGA: Show Reports` | 打开报告 |
| `FPGA: Show Flow Log` | 打开 FPGA Flow 输出 |
| `FPGA: Run TD Check` | 立即运行 TD 检查 |
| `FPGA: Select Project…` | 选择 `.al` |
| `FPGA: TD Installations` | 查看/选择 TD 版本 |
| `FPGA: Verible Status` | 查看 Verible 探测结果 |
| `FPGA: Check Read Design` | 单独跑 TD read_design 检查，错误进入 Problems |
| `FPGA: Doctor` | 汇总扩展/LSP/Verible/TD/MCP 状态 |
| `FPGA: Show Logs` | 打开扩展/服务器日志 |
| `FPGA: Install MCP Configuration` | 安装 MCP 用户/工作区配置 |
| `FPGA: Copy MCP Configuration` | 复制 MCP 配置到剪贴板 |
| `FPGA: Show Cable / Device` | 查看下载器和在线器件 |
| `FPGA: Program SRAM (JTAG)` | JTAG 下载 bit |
| `FPGA: Program Flash (SPI)` | SPI Flash 烧录 |
| `FPGA: Generate SVF…` | 生成 SVF |

---

## 常见问题

| 现象 | 处理 |
| --- | --- |
| 看不到 FPGA 图标 | VS Code ≥ 1.80；扩展是否启用；Reload Window |
| 模块树空 | 确认打开的是包含 `.al` 的**文件夹**，或设置 `fpgaLsp.projectFile` |
| 找不到 python | 把 `fpgaLsp.pythonPath` 设为绝对路径 |
| `.v` 没有补全/跳转 | 确认语言模式为 Verilog/SystemVerilog；本扩展按后缀匹配，不依赖其他 Verilog 插件 |
| `.al` 被识别成其他语言 | 右键 → `Reopen Editor With…` → **Anlogic Project** |
| Flow 报 `PRJ-8001` / `PRJ-8806` | 检查是否有残留 `td_commands_prompt.exe` / `run.exe`；看 Problems 的 `fpga-flow` 诊断和 `FPGA Flow` 输出 |
| Flow 失败但只有 `PRJ-8806` | TD 子进程可能没有写 `run.log`；插件会显示失败步骤和 run.log 路径。详情见 `FPGA Flow` 和 run 目录里的 `.error.f` 标记 |
| Reports 没有 Area 数据 | 需要成功的 phy 运行生成 `*_phy.area`；仅有 OOC `cw/*.area` 时不会当顶层面积报告 |
| MCP 不在 `MCP: List Servers` | 执行 `FPGA: Install MCP Configuration` 选 **VS Code workspace** 后 Reload |
| Verible 未找到 | 设置 `fpgaLsp.verible.path`，或安装 Verible；不影响 TD 功能 |
| Verible 报注释 tab | 插件会过滤注释内的 `no-tabs`；代码中的 tab 仍会提示。`Verible: apply lint autofixes` 同时会把注释内的 tab 展开为空格。line-length 默认 160 |
| 格式化没有任何变化 | 检查 `fpgaLsp.verible.format` / `verible-format` 是否可用；无 Verible 时使用内置缩进回退 |

---

## 开发与测试

```powershell
cd Y:\fpga_lsp\vscode-extension
npm install
npm test
npm run test:locate
npm run test:integration
npm run test:vscode
npm run bundle:server
npm run package
code --extensionDevelopmentPath="Y:\fpga_lsp\vscode-extension" %WORKSPACE%\CorpusAlpha
```

开发模式下扩展使用仓库里的 `../fpgalsp`；vsix 模式使用打包进 `server/` 的副本。两种布局由 `src/locateServer.js` 解析；`fpgaLsp.serverRoot` 可强制覆盖。

---

## 许可

License: **AGPL-3.0-or-later** — 见 [LICENSE](LICENSE)。Copyright (C) 2026 leuk0cyte.
