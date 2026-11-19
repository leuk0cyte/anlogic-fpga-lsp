# tools/perf/profiler —— CPU 采样剖析（perf-profiler / task-27）

目标：回答"CPU 时间花在哪些函数、哪个线程、用户态还是内核态"，**不靠读代码猜**。

## 结论先行：为什么不用 ETW/xperf

本会话是 **Medium integrity（未提权）**，且授权弹窗被全局禁用。CPU 采样走 NT Kernel Logger，
需要 `SeSystemProfilePrivilege`（只授予 Administrators）：

```
xperf -on PROC_THREAD+LOADER+PROFILE   ->  NT Kernel Logger: 无效标志 (0x3ec)
wpr   -start CPU                       ->  Failed to enable the policy to profile system performance
```

`wpaexporter` 也确认必须配 `.wpaprofile`（没有 `-all`）。因此提供两条路：

| 路径 | 需要 | 产出 |
| --- | --- | --- |
| **A. ETW（collect_etw.ps1）** | 管理员 PowerShell | `<scenario>.etl` + wpaexporter 的 CPU 采样 CSV |
| **B. 自建 user-mode 采样器（sampler.exe）** | 无（本会话即可跑） | `functions.csv` / `threads.csv` / `sweeps.csv`（本次报告用的是这条） |

## 快速开始（Plan B，本会话可直接复现）

```powershell
cd Y:\fpga-lsp-cpp

# 1) 建带 PDB 的 RelWithDebInfo 构建（见下方"已知坑"）
. .\tools\perf\devshell.ps1
cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
      -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DBUILD_TESTING=OFF `
      -DFPGA_LSP_VERIBLE_ENABLED=ON
cmake --build build-prof --target fpgalsp-native -- -j 32
#    若在 verible 链接处失败：再 configure 一次再 build（见"已知坑"）

# 2) 编采样器（单文件，不需要 CMake）
. .\tools\perf\devshell.ps1
cl /nologo /std:c++20 /O2 /EHsc /W4 /Fe:tools\perf\profiler\sampler.exe `
   /Fo:"$env:TEMP\fpgalsp-perf\profiler\\" tools\perf\profiler\sampler.cpp dbghelp.lib

# 3) 生成合成工程（200 文件 + 10 MB timing 报告 + 10 份冷索引副本）
python tools\perf\profiler\make_project.py --files 200 --clones 40

# 4) 跑场景（有/无采样器各一次，用于量化采样开销）
python tools\perf\profiler\scenario_driver.py --exe build-prof\bin\fpgalsp-native.exe `
    --root "$env:TEMP\fpgalsp-perf\profiler\proj200" --scenario b --min-ms 15000 `
    --with-sampler --interval-ms 5 --out "$env:TEMP\fpgalsp-perf\profiler\runs"

# 5) 汇总（打印 markdown 表 + 写 out/*.csv）
python tools\perf\profiler\summarize.py --runs "$env:TEMP\fpgalsp-perf\profiler\runs" `
    --out tools\perf\profiler\out
```

## Plan A（ETW，需要管理员）

```powershell
# 管理员 PowerShell
cd Y:\fpga-lsp-cpp
.\tools\perf\profiler\collect_etw.ps1 -Scenario all -Exe build-prof\bin\fpgalsp-native.exe
# 产物：%TEMP%\fpgalsp-perf\profiler\etw\<scenario>.etl 与 <scenario>\*.csv
# 需要入库的 CSV 复制到 tools\perf\profiler\out\etw\（保持 <5 MB）
```

`collect_etw.ps1` 会：自检管理员 → 生成最小 `cpu-sampled.wpaprofile` → `xperf -on ... -stackwalk Profile`
→ 跑 `scenario_driver.py` 对应场景 → `xperf -d` → `wpaexporter -profile` → 记录前后系统负载到 `etw/load.csv`。

## sampler 的设计要点（以及踩过的三个坑）

```
每隔 interval：遍历目标进程线程
  ├─ OpenThread(THREAD_SUSPEND_RESUME|GET_CONTEXT|QUERY_INFORMATION)  仅成功的线程
  ├─ QueryThreadCycleTime 增量 == 0  -> 该线程在等待，跳过（不产生样本）
  ├─ SuspendThread -> GetThreadContext -> StackWalk64 只收集返回地址 -> ResumeThread
  └─ 权重 = 该线程这一拍消耗的 CPU cycles（最后用 GetThreadTimes 总 CPU 标定成 ms）
采样结束：一次性 SymFromAddr 解析（build-prof/bin/*.pdb）-> functions.csv / threads.csv
```

三个必须记住的坑（都实测踩过，别重犯）：

1. **不要在采样循环里做符号解析**。第一版逐帧 `SymFromAddr`，单轮 157 ms、每轮只有 1 个样本，
   归因完全失真（"CPU 全花在 NtReadFile"）。改成只存返回地址、循环外一次性解析后，
   单轮降到 ~1 ms、样本数 ×20。
2. **`CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 是全机快照**（本机单次 ~95 ms）。
   线程集合几乎不变 → 缓存 200 ms 刷新一次，否则采样周期被拉到 100 ms。
3. **`Sleep(1)` 在默认 15.6 ms 定时器精度下真的睡 15 ms**。采样器启动时 `timeBeginPeriod(1)`，
   退出 `timeEndPeriod(1)`。

另外两个"物理限制"：
* `GetThreadTimes` 粒度 = 调度 tick（15.6 ms）→ 只能用来做 user/kernel 拆分与总量标定，
  **不能**用它做"这一拍在不在跑"的门控（否则采样率只剩 ~64/s）。
* 冷索引场景**必须**在一个 server 进程里跑 N 份工程副本：sampler 每个进程首次 StackWalk
  要加载 `fpgalsp-native.pdb`（~1 s），会把 50 ms 的索引完全淹没
  （这就是 `make_project.py --clones` 存在的原因）。

## 已知坑：build-prof 的 Verible 两遍构建

vendored Verible 的 flex/bison 生成源要**两遍 configure**：

```
cmake -S . -B build-prof ...        # 第 1 遍：生成规则尚未产出
cmake --build build-prof --target fpgalsp-native   # 跑到 verible 链接失败（此时 .c 已生成）
cmake -S . -B build-prof            # 第 2 遍：把生成的 .c 纳入编译
cmake --build build-prof --target fpgalsp-native   # 通过
```

`build-prof/verible/gen` 里出现 `verilog-final.tab.cc` / `command-file.yy.cc` / `verilog.yy.cc`
就说明第一遍生成已完成。**另外**：README §5.4 的 configure 命令缺
`-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`，在本机会因"cl 不在 PATH 的默认搜索里"而失败
（报 `could not find CMAKE_PROJECT_NAME in Cache`），上面"快速开始"里已补上。

## 文件清单

| 文件 | 作用 |
| --- | --- |
| `sampler.cpp` | user-mode 采样器（dbghelp StackWalk64 + QueryThreadCycleTime 加权） |
| `make_project.py` | 生成 200 文件合成工程 + 10 MB timing 报告 + N 份冷索引副本，并预热 page cache |
| `scenario_driver.py` | 4 个场景的 LSP 驱动（延迟 p50/p95、系统负载、可选挂采样器） |
| `summarize.py` | CSV+JSON → 报告表格（top-N self/inclusive、线程占比、内核态占比） |
| `collect_etw.ps1` | Plan A：管理员跑一次产出 ETW CSV |
| `out/` | 入库的聚合 CSV（<5 MB） |

## 证据等级（写报告时按这个标）

* **M** = 本机实测（sampler 的 cycles 加权样本；driver 的客户端延迟）
* **C** = 读代码确定（`文件:行号`）
* **E** = 由 M/C 推算
* 采样是**相对排序**工具：绝对延迟以 `scenario_driver` 的客户端计时为准（且已用"有/无采样器"
  对照量化偏置），不要把采样得到的 ms 当墙钟。