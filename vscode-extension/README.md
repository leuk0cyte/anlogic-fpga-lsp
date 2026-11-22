# Anlogic FPGA LSP — TangDynasty / TD 工程语言支持与 IDE 视图

> 作者：**leuk0cyte** · 图标：带 “TD” 的蓝色芯片（`resources/icon.svg`、`resources/icon.png`）

这是 Anlogic TangDynasty（TD）FPGA 工程的 VS Code 扩展，配合内置的**原生语言服务器**
`fpgalsp-native.exe` 使用（不需要 Python，也不需要单独安装 Verible）。

扩展分两层：

1. **标准 LSP**：通过 `vscode-languageclient` 提供 `.v/.sv/.vhd`、`.adc`、`.sdc`、`.al` 文件的诊断、补全、悬停、定义、引用、重命名、符号、折叠、Inlay Hints、语义高亮等能力。诊断来源包括离线分析和 TD 的权威 `read_hdl` / `read_adc`。
2. **IDE 视图**：通过 fpga-lsp 的 v2 自定义协议提供模块层次树、Project & Flow 树、Synthesis / Place & Route 报告页，以及一键综合、布局布线、报告查看等操作。

所有界面文字、按钮、命令标题使用英文；本文档使用中文。

---

## 主要功能

- **模块层次树**：自动生成 `top` 的实例层次，点击模块跳定义，点击黑盒/IP 跳例化处；VHDL entity 和 IP 生成的同名 Verilog 模块会显示为已定义模块，而不是 blackbox。
- **导航与编辑**：定义、引用、补全、悬停、重命名、调用层次、workspace symbol、信号级符号、generate 作用域重命名。
- **TD 诊断**：保存时运行 TD 权威检查（`read_hdl` + `read_adc`），Problems 中显示 `HDL-xxxx` / `USR-xxxx` 等诊断；TD 的黑盒/ignored 警告也会显示。
- **一键 Flow**：Synthesis、Place & Route、Full Flow、取消、增量布线（只重跑 route）。`jobs` 默认 6（TD GUI 交互式默认），范围 **1~6**，单步强制 1。
- **单步 Flow**：Flow 树里六步右键 `Run This Flow Step` / `Reset and Run This Flow Step` —— Run 对已完成步骤直接跳过；Reset and Run **只跑这一步**（先清该步及下游标记），不会再把整个综合重跑一遍。
- **Flow 日志**：`Output → FPGA Flow` 实时显示 TD 输出；`run.log` 中的新 ERROR/WARNING 会加 `[run.log]` 前缀转发；Flow 的错误/警告同时进入 Problems（source `fpga-flow`）。
- **独立报告页**：**Reports** 是独立的活动栏视图，与 Project & Flow 平级；`Synthesis Reports` 和 `Place & Route Reports` 两个页面，各自包含 Dashboard、QoR、Timing、Area、Flow、DRC-CDC、Compare，并支持搜索。
- **资源利用率**：Area 页显示器件利用率，并计算每个模块的 LUT / SEQ / BRAM / DSP 使用率；兼容 EF2 以及 PH1 的 `#slice reg` / `#slice*` / `logic lut` / `eram` 格式。
- **Verible 集成**：格式化、lint/autofix、细粒度语法高亮全部由 exe 内嵌的 Verible 提供，无需外部二进制。
- **MCP server**：LSP 进程内挂 **loopback TCP** MCP（stdio 客户端经 `--attach-port` 薄代理接入），把工程、符号、诊断、报告、flow、探针等 **50 个工具**暴露给 Copilot / Cline / 其他 agent；服务端默认只读，扩展默认放行 flow（`allowWrite` 仍关闭）。
- **器件与约束**：TD 安装探测、器件引脚表缓存、ADC 引脚约束补全/校验、SDC 支持、多 TD 安装选择。
- **工程管理**：子目录 `.al` 自动发现、重命名工程、`open_project -update`、添加源文件、查重名 module、层次搜索。
- **代码片段**：Verilog / SystemVerilog / VHDL / ADC / SDC snippets；输入 `module`、`always_ff`、`entity`、`process`、`pin`、`clock` 等前缀即可展开。

---

## 快速开始

### 1. 前置条件

**无需安装 Python，也无需单独安装 Verible** —— 语言服务器是单一原生可执行文件
`fpgalsp-native.exe`，Verible 的解析 / lint / 格式化已编译进这个 exe。

- 安装 TangDynasty（TD）。默认自动探测 `C:\Anlogic\TD_Release_*` 等目录；探测不到时设置
  `fpgaLsp.tdPath`。
- Windows x64，VS Code ≥ 1.80。
- 扩展自带 `server/bin/fpgalsp-native.exe`；想用自建的 exe 时设 `fpgaLsp.serverPath`。

### 2. 安装扩展并重载

```powershell
code --install-extension Y:\fpga-lsp-cpp\vscode-extension\fpga-lsp-anlogic-0.30.0.vsix
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
- **超时**：每次 flow 都有绝对上限 —— `fpgaLsp.flowTimeoutMinutes` 默认 `0` 表示按阶段自动（综合 30 分钟 / 布局布线 90 分钟 / 全流程 120 分钟，多阶段取最大），非 `0` 为固定分钟数（1~600）；到点或 `flowStatus` 连续失败 12 次（约 2 分钟）都会停止轮询、自动 `cancel` 并把 flow 标记失败（输出 `flow timed out after N min - cancelling` / `language server is not responding`），不会永远停在 busy。
- 完整日志：本次 flow 的运行目录在 `<状态目录>/flow/<runId>/`（`<状态目录>` = `%TEMP%\fpgalsp\<工程名>-<hash8>`，可用 `FPGALSP_STATE_DIR` 覆盖）；TD **会话**日志在 `<.al 目录>/log/td_*.log`（与 TD GUI 一致，不再落工程根目录）；TD 自己的 run 日志在 `<project>_Runs/<run>/run.log`（TD 不一定生成）。

### 单步 Flow（Flow 树六步右键）

Flow 树固定渲染 TD GUI 的六步：Read Design / Optimize RTL / Optimize Gate（syn_1）、
Optimize Placement / Optimize Routing / Generate Bitstream（phy_1）。

| 动作 | 行为 |
| --- | --- |
| `Run This Flow Step` | 该步已有完成标记就直接跳过（提示 "skipped"）；否则跑这一步 |
| `Reset and Run This Flow Step` | **只跑这一步**：先清掉该步及下游的标记与 `flow.status` 统计行，再 `launch_runs <run> -step <step> -jobs 1`；上游产物保留 |

副作用（与 TD GUI 一致）：任何 syn 步骤重跑都会作废整个 phy run（标记 + `phy_1/cw` + 旧 `.bit`）；
任何 phy 步骤重跑都会删 `phy_1/cw` 与旧 bit。单步 phy 不做 `save_best_bits`。

### Reports

- **Synthesis Reports** / **Place & Route Reports** 是独立页面，互不影响。
- Timing 页结构化展示 metadata、Check Status、Clock Summary、Path Group、每条路径的详细字段、delay 表和完整 raw 报告。
- Area 页展示器件利用率、LUT 统计和每模块资源利用率；支持 PH1 的 slice/slice reg/eram 字段。
- 页面顶部搜索框可过滤表格行、路径和 raw 行。

### Verible

**Verible 已内嵌在 `fpgalsp-native.exe` 里，不需要另外安装**（解析、lint、格式化都由
服务端提供）：

- 格式化：由内嵌 Verible 完成，通过语言服务器调用。团队排版风格（160 列 / 4 空格缩进 /
  对齐组）与 `fpgaLsp.verible.args` 的默认值逐条一致，后者会在启动后端时发过去覆盖
  服务端内置的那份（整体替换语义，见下）。`fpgaLsp.verible.format=false` 可关掉。
- Lint / quick fix：`fpgaLsp.verible.autofix=true`（默认）时，quick fix 由内嵌 lint
  直接算出来，**不需要外部 verible 二进制**。规则参数见 `fpgaLsp.verible.lintArgs`
  （默认 `--rules=+line-length=length:160`，与格式化列宽对齐）。
- 语法高亮：基础着色由 TextMate 语法（`syntaxes/*.tmLanguage.json`）提供，语义高亮由
  服务端的 `semanticTokens`（内嵌 Verible CST 驱动）叠加；不依赖外部 `verible-verilog-syntax`。
- 因此不再有「找不到 Verible 就降级」的行为——历史上那套外部 Verible 探测是在
  Python 后端时期，已随原生化移除。

### MCP（Copilot / Cline / 其他 agent）

内置 MCP 服务端：LSP 进程在 127.0.0.1 上开 loopback MCP（共享同一个 Engine，agent 看到的工程状态
= 编辑器看到的状态）；默认暴露只读工具，扩展默认允许 flow，`allowWrite` 仍关闭。

- 默认不注册动态 provider（`fpgaLsp.mcp.dynamicProvider=false`），避免 Copilot 无法关联 workspace。
- 安装工作区配置：

```text
FPGA: Install MCP Configuration
```

选择 **VS Code workspace**（推荐第一个选项）或 portable `.mcp.json`。

- `fpgaLsp.mcp.allowFlow` 默认 `true`；`allowWrite` 默认 `false`。
- 常用新工具：`fpga_flow_run_step`（单步）、`fpga_flow_log`、`fpga_flow_issues`、`fpga_flow_wait`（带 progress 通知）、`fpga_read_file`、`fpga_search_text`、`fpga_constraint_audit`。
- 全部 **50 个工具**（`--allow-flow` 可见 46、只读 38、`--allow-write` 再加 4）与安全限制见仓库的
  `src/mcp/tools.cpp`；工具清单文档见 `doc/mcp-usage.md`。
  （`doc/audit/2026-11-18/parity-mcp/tools-diff.md` 是 2026-11-18 的归档快照，不含后来新增的 9 个工具。）

### ADC / SDC

- `.adc` 引脚约束支持补全、诊断、一键补 `IOSTANDARD`、删除分配。
- 可校验可用 IO、专用/SerDes 引脚、重复/冲突。
- `.sdc` 支持语法诊断、补全和定义跳转。

---

## 设置

| 设置 | 默认值 | 说明 |
| --- | --- | --- |
| `fpgaLsp.serverPath` | `""` | 原生语言服务器 `fpgalsp-native.exe` 路径；留空自动查找（vsix 内置 `server/bin` → 同仓库 `build*/bin` → PATH） |
| `fpgaLsp.tdPath` | `""` | TD 安装根目录，空 = 自动探测 |
| `fpgaLsp.tdCheckOnSave` | `true` | 保存时运行 TD 权威检查（`read_hdl` + `read_adc`，约 1–2 s） |
| `fpgaLsp.projectFile` | `""` | 显式指定 `.al`；空 = 用最近的上级工程 |
| `fpgaLsp.flowJobs` | `6` | TD multirun 的并发 run 数（**1~6**，不是线程数；单步始终用 1）。超出范围会被钳到 6 |
| `fpgaLsp.flowTimeoutMinutes` | `0` | 单次 flow 的绝对超时（分钟）：`0` = 按阶段自动（综合 30 / 布局布线 90 / 全流程 120，多阶段取最大），非 `0` = 固定值（1~600）。到点停止轮询、自动取消并标记失败 |
| `fpgaLsp.logFile` | `""` | 服务器日志，空 = `<状态目录>/logs/server.log`（`<状态目录>` = `%TEMP%\fpgalsp\<工程名>-<hash8>`；工程目录下不再有 `.fpgalsp`） |
| `fpgaLsp.verible.format` | `true` | 用内嵌 Verible 格式化（Format Document） |
| `fpgaLsp.verible.args` | 160 列 + 4 空格缩进 + 一组对齐参数 | 内嵌 `verible-verilog-format` 的 style 参数；与服务端内置默认逐条一致，非空即**整体替换**，见下 |
| `fpgaLsp.verible.lintArgs` | `["--rules=+line-length=length:160"]` | 传给内嵌 lint 的参数：`--rules=` / `--ruleset=` / `--rules_config=` / `--rules_config_search` / `--waiver_files=` |
| `fpgaLsp.verible.autofix` | `true` | quick fix 由内嵌 Verible linter 计算（**不需要外部 verible 二进制**） |
| `fpgaLsp.mcp.allowFlow` | `true` | 向 agent 暴露 flow / analysis / strategy 工具（会真的启动 TD flow） |
| `fpgaLsp.mcp.allowWrite` | `false` | 向 agent 暴露工程写工具（更新 / 添加源文件 / 重命名 `.al`） |
| `fpgaLsp.mcp.attachOnStart` | `true` | 启动时附着 MCP server；`false` 则完全跳过附着与注册 |
| `fpgaLsp.mcp.dynamicProvider` | `false` | 注册动态 MCP server provider。默认关闭：Copilot 无法把动态 server 关联到 workspace，建议用 `FPGA: Install MCP Configuration` 写工作区 `.vscode/mcp.json` |

### 排版风格从哪来

默认排版风格**同时存在两处**，且逐条一致：

| 位置 | 作用 |
| --- | --- |
| `fpgaLsp.verible.args`（本插件 package.json 默认值） | VS Code 启动后端时作为初始化选项发过去 → **替换**服务端内置默认 |
| `src/verible_bridge/format_text.cpp::DefaultStyleFlags()`（服务端内置） | MCP、裸 LSP、或客户端不传配置时使用 |

两份不许漂移：`tests/unit/verible_style_test.cpp::PluginDefaultsMirrorServerDefaults`
直接读本清单与服务端列表逐条比对，改了一边忘了另一边，单测立刻红。

当前默认风格（与旧 Python 插件 `fpgaLsp.verible.args` 的默认值一致）：

```text
--column_limit=160                                    # 行宽 160
--indentation_spaces=4  --wrap_spaces=4               # 4 空格缩进
--try_wrap_long_lines=true                            # 长行主动折断
--assignment_statement_alignment=align                # 12 组对齐
--case_items_alignment=align
--distribution_items_alignment=align
--enum_assignment_statement_alignment=align
--module_net_variable_alignment=align
--class_member_variable_alignment=align
--struct_union_members_alignment=align
--parameter_declaration_alignment=align
--port_declarations_alignment=align
--formal_parameters_alignment=align
--named_parameter_alignment=align
--named_port_alignment=align
--port_declarations_right_align_packed_dimensions=true
--port_declarations_right_align_unpacked_dimensions=true
--formal_parameters_indentation=indent                # 4 组换行缩进
--named_parameter_indentation=indent
--named_port_indentation=indent
--port_declarations_indentation=indent
--alignment_group_boundary=blank-lines-and-separator-comments
```

**覆盖语义是"整体替换"，不是逐条合并**：写了非空数组，上面这些内置默认就全部不再
生效，没写的字段回落到 Verible 原生默认（缩进 2 空格、对齐 `infer`）。所以想改单项
又不想丢掉团队风格，要把整串抄下来再改那一项，例如：

```jsonc
"fpgaLsp.verible.args": [
  "--column_limit=120",          // 只改行宽
  "--indentation_spaces=4",      // 其余照抄
  "--wrap_spaces=4",
  "--try_wrap_long_lines=true",
  // … 其余 19 条 alignment / indentation / alignment_group_boundary
]
```

---

## 常用命令

| 命令 | 作用 |
| --- | --- |
| `FPGA: Run Synthesis` | 只综合 |
| `FPGA: Run Place & Route` | 只布局布线 |
| `FPGA: Run Full Flow` | 综合 + 布局布线 + bit |
| `FPGA: Re-run Routing` | 增量布线（复用综合 + 布局结果，只重跑 route + bitgen） |
| `FPGA: Cancel Flow` | 取消 flow |
| `FPGA: Show Reports` | 打开报告 |
| `FPGA: Show Flow Log` | 打开 FPGA Flow 输出 |
| `FPGA: Run TD Check` | 立即运行 TD 检查 |
| `FPGA: Select Project…` | 选择 `.al` |
| `FPGA: TD Installations` | 查看/选择 TD 版本 |
| `FPGA: Check Read Design` | 单独跑 TD read_design 检查，错误进入 Problems |
| `FPGA: Run This Flow Step` | 右键 Flow 六步之一：跑这一步（已完成则跳过） |
| `FPGA: Reset and Run This Flow Step` | 右键 Flow 六步之一：清该步及下游后只跑这一步 |
| `FPGA: Doctor` | 汇总扩展/LSP/Verible/TD/MCP 状态 |
| `FPGA: Show Logs` | 打开扩展/服务器日志 |
| `FPGA: Install MCP Configuration` | 安装 MCP 用户/工作区配置 |
| `FPGA: Copy MCP Configuration` | 复制 MCP 配置到剪贴板 |
| `FPGA: Show Cable / Device` | 查看下载器和在线器件 |
| `FPGA: Program SRAM (JTAG)` | JTAG 下载 bit |
| `FPGA: Program Flash (SPI)` | SPI Flash 烧录 |
| `FPGA: Generate SVF…` | 生成 SVF |
| `FPGA: ChipWatcher Probe Status` | 看探针清单 / 采样深度 / 时钟 / 编译产物是否就绪（只读） |
| `FPGA: Capture On-Chip Probe Data` | 通过 JTAG 读回片上探针的 1024 个采样点（默认先直接用器件里现有的 bit 抓，抓不动才下载） |
| `FPGA: ChipWatcher (signals, trigger, capture)` | 打开探针面板（信号增删 / watcher 现状 / 采样时钟与深度 / 一键采集） |

> 上表只是常用项；扩展共 **58 条命令**（命令面板搜 `FPGA:` 可看全部），完整 id/标题以
> `package.json` 的 `contributes.commands` 为准。
>
> 插件界面（命令标题、提示、面板、设置说明）一律英文；只有 `doc/` 与本 README 是中文。

### 片上探针（ChipWatcher）

工程里 `<工程>/chip_debugger/user.mdn` 的 `mark_debug { … }` 决定**哪些内部信号被埋进 bit**：

```tcl
mark_debug { WHG/pwm_reg0_i[0] … WHG/pwm_reg7_i[15] }
```

流程是「改清单 → 重跑 Place & Route（`insert_debugger` 在 `opt_place` 阶段执行）→
得到 `phy_1/cw/compiled.cwc` → 采集」。采集回 1024 个采样点（`sample_depth` 决定），
每条总线一列，落地成 `phy_1/cw/capture_<时间戳>.csv`。

三个已经踩过的坑（写进 [doc/design/on-chip-probe.md](../doc/design/on-chip-probe.md)）：

* **只跑 route 不会更新探针** —— 走的是增量（复用 `place.db`），`compiled.cwc` 时间戳不变。
* 触发条件默认是「所有 bit 为 low」，对寄存器类信号等于没用；采集默认用 `-trigger_now`。
* 信号名必须是**综合后网表**的层次名，写错会被 TD 静默忽略。

---

## 常见问题

| 现象 | 处理 |
| --- | --- |
| 看不到 FPGA 图标 | VS Code ≥ 1.80；扩展是否启用；Reload Window |
| 模块树空 | 确认打开的是包含 `.al` 的**文件夹**，或设置 `fpgaLsp.projectFile` |
| 语言服务器没起来 | 看 `FPGA: Show Logs`；确认包内 `server/bin/fpgalsp-native.exe` 存在，或用 `fpgaLsp.serverPath` 指向自建 exe |
| `.v` 没有补全/跳转 | 确认语言模式为 Verilog/SystemVerilog；本扩展按后缀匹配，不依赖其他 Verilog 插件 |
| `.al` 被识别成其他语言 | 右键 → `Reopen Editor With…` → **Anlogic Project** |
| Flow 报 `PRJ-8001` / `PRJ-8806` | 检查是否有残留 `td_commands_prompt.exe` / `run.exe`；看 Problems 的 `fpga-flow` 诊断和 `FPGA Flow` 输出 |
| Flow 失败但只有 `PRJ-8806` | TD 子进程可能没有写 `run.log`；插件会显示失败步骤和 run.log 路径。详情见 `FPGA Flow` 和 run 目录里的 `.error.f` 标记 |
| Reports 没有 Area 数据 | 需要成功的 phy 运行生成 `*_phy.area`；仅有 OOC `cw/*.area` 时不会当顶层面积报告 |
| MCP 不在 `MCP: List Servers` | 执行 `FPGA: Install MCP Configuration` 选 **VS Code workspace** 后 Reload |
| 注释里的 tab 被 lint 报出 | 插件会过滤注释内的 `no-tabs`；代码中的 tab 仍会提示。line-length 默认 160 |
| 格式化没有任何变化 | 检查 `fpgaLsp.verible.format` 是否为 `true`；格式化由内嵌 Verible 完成，不需要外部工具 |

---

## 开发与测试

扩展的开发、测试与打包流程见 [../doc/extension-development.md](../doc/extension-development.md)。

要点：扩展只对接原生 `fpgalsp-native.exe`（由 `npm run bundle:server` 从
`../build/bin` 塞进 `server/bin`），打包前需先构建出该 exe。

### 文件写入统一走 writeService

插件的**所有文件写入**都必须经过 `src/writeService.js`（与后端 `platform::write_service` 对称），
业务代码里不再直接 `fs.writeFileSync` / `fs.appendFileSync` / `createWriteStream`：

- **路径策略**：状态类写入只允许落在 `stateRootFor(workspaceRoot)`（即 `%TEMP%\fpgalsp\<工程名>-<hash8>`，
  与 `src/stateDir.js` / 后端 `platform::state_dir()` 同一个目录）之下；工程文件只允许显式白名单
  （`.vscode/mcp.json`、`.mcp.json`、`*.al`）。绝对路径、`..`、NUL 以及任何含 `.fpgalsp` 的路径段
  一律拒绝。
- **原子写**：同目录 `.tmp-<pid>-<seq>` → `rename`，失败重试 3 次并清理临时文件；`opts.backup`
  覆盖工程文件时先把旧内容写成 `<file>.fpgalsp-bak`。
- **never-throw**：所有 API 返回 `{ ok, path, error }`，失败只回报原因；`opts.dryRun` 只校验不落盘。

门禁：`node test/run.js` 的 `write gate` 测试会扫描 `extension.js` 与 `src/*.js`，
`fs.*` 裸写只允许出现在 `src/writeService.js`，新增写入绕过服务会直接测试失败。

---

## 许可

License: **AGPL-3.0-or-later** — 见 [LICENSE](LICENSE)。Copyright (C) 2026 leuk0cyte.
