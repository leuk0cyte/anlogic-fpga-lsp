# 片上探针（Anlogic ChipWatcher）

> 目标：把工程里的内部信号以"探针"形式嵌进 bitstream，上板后**在线读出真实数值**，
> 并且这件事既能在 IDE 里点，也能让 agent 通过 MCP 直接调。
>
> 本文记录的全是本机（TD Release_2026.2.210.850 / PH1A100SFG676 /
> `%WORKSPACE%\CorpusDelta\corpus_delta`）**实测**得到的结论，不是文档推演。

---

## 1. 一句话原理

```
chip_debugger/user.mdn         ← 你写：要 watch 哪些信号（mark_debug {...}）
        │  TD 的 insert_debugger 读它
        ▼
综合 / 布局时自动插一个 watcher 实例（auto_created_cwc1）+ debug hub
        │
        ▼
phy_1/cw/compiled.cwc  +  compiled.bit  +  corpus_delta_inst.bid
        │  探针→ERAM 的映射固化在 bit 里
        ▼
download -mode jtag_burst 灌进器件 → run_hw_cw → wait_on_hw_cw → write_hw_cw_data
        │
        ▼
capture_<时间戳>.csv（每个采样点一行，每条总线一列）
```

**关键点：探针不是"运行时挂上去"的，是编译期嵌进 bit 的。**
所以"改探针清单"必然要重跑综合/布局；而"改触发/采样条件"只是运行期寄存器，不用重编。

---

## 2. 探针清单放在哪

`<project>/chip_debugger/user.mdn`，只有一行：

```tcl
mark_debug { WHG/pwm_reg0_i[0] … WHG/pwm_reg7_i[15] }
```

* 这是 **TD 自己的约定位置**：phy run 的 `settings.cfg` 里有
  `set chipDebugger ../../chip_debugger`，生成的流程脚本调用 `insert_debugger $chipDebugger`；
  即使该变量不存在也会 `insert_debugger`（默认就是这个目录）。
* 名字用**综合后网表里的层次名**（`WHG/<实例层次>/<信号>[位]`），不是 RTL 里的局部名。
* `.al` 里的 `<CWC_FILE>`（本工程是 `cwc/1.cwc`，且 `Enable=false`）是**另一套**机制：
  手工图形式 ChipWatcher 实例。本工程实际生效的是 `user.mdn` 自动生成的
  `auto_created_cwc1`，那个 286 net 的 `1.cwc` 是历史遗留、未启用。

---

## 3. 编译期：探针怎么进 bit

TD 2026.2 生成的 phy 流程脚本（`corpus_delta_Runs/phy_1/corpus_delta.tcl`）里：

```tcl
opt_rtl_fun   →  insert_debugger $chipDebugger      ; 先把 mark 住的网保住
opt_place_fun →  insert_debugger $chipDebugger      ; 再插一次（布局前）
bitgen_fun    →  bitgen -bit $prj_name.bit
                 setup_debugger $chipDebugger -prj $prj_name
                                                 ; ^ 这一步产出 cw/compiled.cwc + cw/compiled.bit
```

`cw/` 下会多出：

| 文件 | 作用 |
| --- | --- |
| `compiled.cwc` | watcher 实例定义：时钟、采样深度、每条 bus、触发条件、ERAM 物理分配 |
| `compiled.bit` | 带调试逻辑的位流（本工程与 `best_result` 的 `.bit` 字节相同） |
| `compiled.data` | 波形视图配置（颜色/进制/顺序），GUI 用 |
| `auto_created_cwc1/` | watcher 的 OOC 综合产物（`*_watcherInst.sv` / `_ooc.db` / `_gate.area`） |
| `debug_hub/` | JTAG 到 watcher 的 hub wrapper |

**判断"探针有没有真的进 bit"**：看 `phy_1/cw/compiled.cwc` 是否存在、里面的 `<bus>` 是否
与 `user.mdn` 一致。`src/flow/probe.cpp::ReadCwc` 做的就是这个比对。

### 重跑一定要跑「完整）」的 Place & Route

实测（本机，同一个工程）：

| stages | TD 实际动作 | 耗时 | 会不会重新埋探针 |
| --- | --- | --- | --- |
| `["route"]` | `Incremental route`，`import_db place.db` 只重跑 route | 227 s | **不会** —— `insert_debugger` 在 `opt_place` 阶段，被跳过了；`compiled.cwc` 时间戳不变 |
| `["phy"]` | `reset_runs phy_1` + `launch_runs phy_1`（从 `opt_place` 起） | **401 s** | 会 |

所以 `probeConfig` 之后必须按 **Run Place & Route（`stages: ["phy"]`）**，
插件里那条「Re-run Place & Route（增量布线）」不够。

### `probeStatus.stale`：专门防这个坑

`probeStatus` 会把 `user.mdn` 的逐位清单聚合成 bus，与 `compiled.cwc` 的 `<bus>` 逐位比对：

```
mdn        = 80 signals
bit buses  = [pwm_reg0_i[15:0], pwm_reg1_i[15:0], pwm_reg6_i[15:0], pwm_reg7_i[15:0]]
stale      = true
missing    = [ANPC_init0/ANPC_SVPWM_init/cnt]
ready      = false
```

`stale:true` 时 `probeCapture` **默认拒绝**（采回来的不是你写的清单，静默给数据比报错更糟），
要读当前那份 bit 必须显式 `allow_stale:true`。

---

## 4. 运行期：无 GUI 采集

全部走 `td_commands_prompt.exe`（**不是** BitWizard），实测序列：

```tcl
import_device ph1_100.db -package PH1A100SFG676 -speed 2
open_project {<abs>/corpus_delta.al} -single_run
config_chipwatcher {<abs>/phy_1/cw/compiled.cwc}      ; 必须带文件！
set fpgalsp_hw [get_hw_cws auto_created_cwc1]          ; 必须带实例名
connect_cable -allink
download -bit {<abs>/phy_1/cw/compiled.bit} -mode jtag_burst -spd 7 -cable 0 \
         -total_dev 1 -cur_dev 1 -bypass 11111111
run_hw_cw $fpgalsp_hw -trigger_now -cable 0
wait_on_hw_cw $fpgalsp_hw -timeout 30 -cable 0
write_hw_cw_data $fpgalsp_hw -csv_file {<abs>.csv}
```

### 四个必须踩过的坑

1. **`config_chipwatcher` 不带参数会"成功但什么都不做"**
   实测没有任何 `KIT-1004` 输出，后续 `get_hw_cws` 拿不到实例。必须显式喂
   `compile.cwc` 的绝对路径。
2. **`get_hw_cws` 必须带实例名**，否则 `KIT-8414 ERROR: please specify a name of watcher instance`。
3. **拿到的是 Tcl 对象，不是字符串**：`run_hw_cw auto_created_cwc1` 会报
   `KIT-8000 ERROR: No watcher instance matched.`，必须
   `set h [get_hw_cws auto_created_cwc1]` 然后 `run_hw_cw $h`。
4. **`open_project` 不能用 `HasError` 判失败**：本工程 RTL 自带
   `HDL-7225 CRITICAL-WARNING: 'busclk' is already implicitly declared`，
   它跟 open_project 成不成功无关。判断要用"只看 `ERROR`"（`td::HasFatal`）。

### 触发条件的默认值是"没用"的

Mark Debug 自动生成的 `compiled.cwc` 里，64 个 net 全是 `cond="low"`，bus 是 `dont_care`，
组合是 `basic_and` —— 等价于"64 位同时为 0 才触发"。对 PWM 占空比寄存器来说，
这等于永远立刻触发或永远不触发。

所以**采集默认走 `-trigger_now`（立即触发，与条件无关）**，返回值里标
`trigger_mode: "manual"`。想改成条件触发需要 `set_net_cond` / `set_bus_cond` / `set_cwc`
这些未公开的内部命令（help 里没有），本期不做，先保证"能拿到数据"。

### CSV 格式

```csv
Sample,Trigger,pwm_reg0_i[15:0],pwm_reg1_i[15:0],pwm_reg6_i[15:0],pwm_reg7_i[15:0]
0,0,0000000000000000,0000000000000000,0000000000000000,0000000000000000
…
```

* 每条总线一列，值对是**二进制位串**（不是十进制），位宽由 `[msb:lsb]` 推。
* `Trigger` 列只有一个采样点为 1（= 触发位置，本工程 `position=127`）。
* 采样数 = `compiled.cwc` 的 `sample_depth`（本工程 1024，占 4 块 ERAM，余量 4/280）。

---

## 5. 后端实现

| 文件 | 内容 |
| --- | --- |
| 后端实现 | `src/flow/probe.h/.cpp` | `ParseMarkDebug` / `RenderMdn` / `ReadMdn` / `WriteMdn`、`ParseCwc`、`ParseCaptureCsv`、`Status`、`Configure`、`Capture` |
| `src/flow/hardware_lease.h` | 进程级"谁在占 JTAG"租约；`Program` 与 `Capture` 互斥，撞了返回 `{refused:true, stage:"busy"}` |
| `src/server/commands.cpp` | `fpgalsp.probeStatus` / `probeConfig` / `probeCapture` |
| `src/mcp/tools.cpp` + `bridge.cpp` | `fpga_probe_status`（只读）/ `fpga_probe_config`（`write` 门控）/ `fpga_probe_capture`（`flow` 门控） |
| `vscode-extension` | **ChipWatcher 探针面板**（`src/probePanel.js` + `src/probeHtml.js`）：按总线分组的信号芯片、点一下移除、整组移除、粘贴清单、watcher 现状卡片（时钟/深度/触发/stale 徽章）、一键采集；另有 3 条命令走快捷路径 |

### 安全模型（比烧录松一档，但不是没有）

| 操作 | 门禁 |
| --- | --- |
| `probeStatus` | 无（纯读文件） |
| `probeConfig` | `write:true` 才落盘；**空清单拒绝写**（否则等于把 watcher 删了） |
| `probeCapture` | 需 `compiled.cwc` + `compiled.bit` 存在；`download:true` 时必须 `confirm:true`；与 `Program` 抢同一把硬件租约 |

---

## 6. 已知限制

1. **采集要重灌 bit**：`-mode jtag_burst` 会把 FPGA 停 ~15 s。想只读不重灌可传
   `download:false`，但前提是器件里跑的就是带探针的那份 bit。
2. **触发条件不可配**（见上文），只能 `-trigger_now`。
3. **采样深度改动要重编**：`sample_depth` 决定了 ERAM 深度并固化在 bit 里。
4. **同一 `compiled.cwc` 与 bit 的 ucode 必须匹配**：混用旧 bit 会报
   `ChipWatcher code mismatch`。
5. **VCD 导出没接**：TD 的 GUI 能导 VCD，但 CLI 只找到 `write_hw_cw_data -csv_file`。
6. 探针名必须是**综合后**的层次名；写错不会报错，只会被 TD 忽略（`signal %s is not present
   in the netlist`），所以 `probeConfig` 之后务必用 `probeStatus` 比对
   `compiled.cwc` 里的 bus 列表。

---

## 7. 本机验证记录（2026-11-20）

| 项 | 结果 |
| --- | --- |
| `fpga_cables` 预检 | `cables:[{index:0,device:"PH1A100"}]`、`match:true`、`Ready to program` |
| `fpgalsp.program {mode:"sram",confirm:true}` | 14.2 s，`Chip validation success: PH1A100`，`ok:true` |
| `fpgalsp.program {mode:"flash",confirm:true}` | 96.0 s，`SPI flash sector erase` + `Page program and verify`，`ok:true` |
| `fpga_probe_status` | `ready:true`，`clock:reg_init/clk`，`sample_depth:1024`，4 条 bus / 64 net |
| `fpga_probe_capture {confirm:true}` | 17.4 s，**1024 个采样点**，4 条 16 位总线，CSV 落盘 |
| 采集值 | 全部 0 —— 与 RTL 一致：`pwm_reg*_i` 来自 `bus_reg`，`reg_bus` 复位为 `'d0` 且只有主机写 Modbus 才改变，板上无主机 → 真值为 0 |
| stale 守卫 | 把 `user.mdn` 改成 80 个信号但 bit 仍是 64 位后，`stale:true` / `missing:[ANPC_init0/ANPC_SVPWM_init/cnt]`，`probeCapture` 在 `stage:"stale"` 拒绝 |

### 下载速度 `-spd`（本机 GUI 实测映射）

`-spd` 不是 MHz，是**档位号**（1-based，2 起）。官方文档（SWUG110 §5.2.1）写的是
"可配置范围 200kHz～16MHz，默认 1MHz"，但界面上的默认档是 5MHz：

| `-spd` | 2 | 3 | 4 | 5 | 6 | **7** | 8 | 9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 时钟 | 200 kHz | 500 kHz | 1 MHz | 2 MHz | 3 MHz | **5 MHz** | 8 MHz | 16 MHz |

**本后端默认 `-spd 7` = 5 MHz**，与 GUI 下载对话框的默认值一致。
`fpgalsp.program` / `fpgalsp.probeCapture` 都接受 `speed`（2–9），越界值会被忽略并回落默认。
注意 TD **不校验** `-spd`（实测 `-spd 0` / `-spd 99` 都直接放行，只是稍后才失败），
所以范围检查必须在我们这层做。

### 采集不一定要先下载

GUI 的 ChipWatcher 不是每次抓取都先下 bit —— 只要器件里跑的就是那份带探针的 bit 就能直接抓。
所以 `probeCapture` 的 `download` 是三态，默认 `"auto"`：

| 值 | 行为 |
| --- | --- |
| `"auto"`（默认） | 先直接抓；抓不动（`run_hw_cw`/`wait_on_hw_cw` 报错，多半是器件里那份 bit 没探针）才下载并重抓一次 —— **同一会话内重试**，不重跑 `open_project` |
| `"true"` | 总是先 `download -mode jtag_burst` |
| `"false"` | 纯只读，绝不下载（这种调用不需要 `confirm`） |

真机验证：器件里已经是最新 bit 时，`download:"auto"` 返回 `downloaded:false`、
仍然拿到 1024 个采样点。

### 采样时钟 / 深度为什么还不能在面板里改

实测三件事，决定了这一步现在做不到：

1. `insert_debugger <dir> -cwc <file>` **认手写的 `.cwc`** —— 把 `sample_depth` 从 1024 改成
   2048 后 `report_cwc_status` 立刻显示 `2048`，文件头那行过期的
   `<!-- HMAC is: … -->` 也不拦（`KIT-1004 : chipwatcher: sanity check successfully`）。
2. 但 `insert_debugger <dir>`（**不带** `-cwc`）既不扫 `<dir>/*.cwc`（实测放进去也没生效，
   depth 仍 1024），又会**冲掉**前一次 `-cwc` 的配置（实测：先 `-cwc` 得到 2048，
   再跑一次不带参的 `insert_debugger` 后 watcher 列表直接变空）。
3. TD 生成的流程脚本调用的是不带 `-cwc` 的 `insert_debugger $chipDebugger`（而且
   `opt_rtl` 与 `opt_place` 各调一次），所以 `tcl_beforestep` 钩子也留不住配置。

结论：要让面板改时钟/深度，必须让流程里的 `insert_debugger` 带 `-cwc`。可行路径是改
`phy_1/corpus_delta.tcl` 的 "Editable zone"（该文件跨 run 不变，实测 mtime 停在 8 月），
或让 `.al` 的 `<CWC_FILE>` 先加载实例。两条都**还没验证**，属于下一期。
所以面板里这两项目前是**只读展示** + 上面这段说明，不写盘。

### 未完成：非 0 的动态读数（已定位到 TD 侧，不是我们的 bug）

想证明"采到的是随时间变化的活动数据"，计划加 16 位三角载波
`WHG/ANPC_init0/ANPC_SVPWM_init/cnt[15:0]`（`synthesis keep="1"`，与 `reg_init` 同 `clk` 域）。
实测三轮，结论是 **TD 会静默丢弃这 16 个网**：

| 实验 | 结果 |
| --- | --- |
| `get_nets -hier ANPC_init0/ANPC_SVPWM_init/cnt\[0\]`（对 `phy_1/…_pr.db`） | **1 个对象、名字完全对得上** → 不是"名字写错" |
| `import_db syn_1/…_gate.db` + `insert_debugger … -mdn probe80.mdn` | TD 展开成 `mark_debug <80 个网>`（回显里 80 个都在），但 `config_chipwatcher` 报 **4 view node / 64 trigger net / 64 data net** |
| 完整 `["phy"]` 流程（401 s） | `auto_created_cwc1_watcherInst.sv` 仍是 `CWC_BUS_NUM = 4` / `CWC_BUS_DIN_NUM = 64` |

所以 `mark_debug` **收到了**这 16 个名字、网表里**也有**这些网，最终却没进 watcher。
最可能的原因是**推断不出触发时钟**（TD 的错误串里正好有
`failed to get clock for signal %s, please specify the clock in chipwatcher`）——
这类需要"手工指定触发时钟"的实例在 GUI 里可以点，而 CLI 侧的
`set_net_cond` / `set_bus_cond` / `set_cwc` 都没有公开用法。

排查障碍：**后端驱动 `launch_runs` 时 TD 不把 run 输出写成 `phy_1/run.log`**
（只有 GUI / `run.bat` 驱动才写），所以 `insert_debugger` 内部打的告警看不到。
`--mdn` 选项（`insert_debugger <dir> -mdn <file>`）可以脱离工程文件做隔离实验，
但它只在 gate db 上有效，elaborate db（RTL 级）上不会生成 watcher。

下期两件事：① 给 run 加 `tcl_afterstep` 把 `report_cwc_status` / `insert_debugger` 的输出落盘；
② 摸清 `set_net_cond` / `set_cwc` 的参数，让后端能给单个 bus 指定触发时钟。

已把 `user.mdn` 还原成原来那 64 个信号（1192 B，与原始文件同尺寸），
并用 `probeStatus` 确认 `stale:false / ready:true`，随后一次真实采集通过。