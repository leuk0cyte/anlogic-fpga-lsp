// =============================================================================
//  src/flow/probe.h —— 片上调试探针（Anlogic ChipWatcher）后端
// =============================================================================
//  这是在 programmer.h（烧录）之外**第二条会碰真实硬件**的代码路径，但语义完全不同：
//  烧录是"改板子"，采集是"读板子"。所以安全门禁不复用 confirm，而是：
//    * 采集前必须存在 `<runs>/phy_1/cw/compiled.cwc` + `compiled.bit`（否则 bit 里没有探针）
//    * 探针信号清单只从 `<project>/chip_debugger/user.mdn` 读 —— 那是 TD 自己
//      `insert_debugger` 认的约定位置，**不要**另发明一份配置文件
//
//  ---- 实测链路（TD 2026.2，本机真机验证通过；全部走 td_commands_prompt.exe）----
//      import_device ph1_100.db -package PH1A100SFG676 -speed 2
//      open_project <abs>/<name>.al -single_run
//      config_chipwatcher <abs>/phy_1/cw/compiled.cwc
//      get_hw_cws auto_created_cwc1                     ; ⚠ 必须带实例名，否则 KIT-8414
//      connect_cable -allink
//      download -bit <abs>/phy_1/cw/compiled.bit -mode jtag_burst -spd 7 -cable 0 \
//               -total_dev 1 -cur_dev 1 -bypass 11111111
//      run_hw_cw auto_created_cwc1 -trigger_now -cable 0
//      wait_on_hw_cw auto_created_cwc1 -timeout 30 -cable 0
//      write_hw_cw_data auto_created_cwc1 -csv_file <abs>.csv
//
//  ---- 几个必须记住的坑 ----
//  1. `config_chipwatcher` 不带参数**返回成功但什么都不做**（实测无 KIT-1004 输出），
//     必须显式喂 `compiled.cwc`。
//  2. `download` 要用 `-mode jtag_burst`（GUI 的写法）；`jtag` 也能烧但 GUI 采集链
//     用的是 burst，保留回落。
//  3. 采集到的 CSV 表头形如 `Sample,Trigger,pwm_reg0_i[15:0],...`，数据列是**二进制
//     位串**（不是数值），位宽从 `[msb:lsb]` 推。
//  4. 触发条件默认是 Mark Debug 自动生成的 "所有 bit 为 low + basic_and"，对 PWM 这类
//     寄存器等于"永不/立即触发"。所以默认走 `-trigger_now`（立即触发，与条件无关）。
// =============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "td/td_install.h"

namespace fpgalsp::probe {

// -----------------------------------------------------------------------------
// chip_debugger/user.mdn —— 探针信号清单的唯一来源
// -----------------------------------------------------------------------------

// 取出 `mark_debug { ... }` 里的大括号内容并按空白切词（原样保留层次名）。
std::vector<std::string> ParseMarkDebug(std::string_view text);

// `WHG/pwm_reg0_i[0]` -> 排序键 (WHG/pwm_reg0_i, 0)，让 `[2]` 排在 `[10]` 之前。
std::string NormaliseSignal(std::string_view signal);

// 去空、去重、按 (base, index) 升序
std::vector<std::string> NormaliseSignals(const std::vector<std::string>& signals);

struct MdnConfig {
    bool ok = false;
    std::string error;
    std::string path;
    bool exists = false;
    std::vector<std::string> signals;
    // 原文件里除 mark_debug 之外的行（写回时原样保留，避免把用户手写的东西吃掉）
    std::vector<std::string> other_lines;
};

MdnConfig ReadMdn(const std::string& path);

// 生成新内容：替换第一条 `mark_debug {...}`，其余行逐字保留；
// 没有 mark_debug 行时追加到末尾。传入空 signals 等价于删掉这条命令。
std::string RenderMdn(std::string_view original, const std::vector<std::string>& signals);

bool WriteMdn(const std::string& path, const std::vector<std::string>& signals,
              std::string* error);

// -----------------------------------------------------------------------------
// phy_1/cw/compiled.cwc —— ChipWatcher 编译产物（探针是否真的进了 bit 的证据）
// -----------------------------------------------------------------------------

struct CwcBus {
    std::string name;
    std::string clock;
    std::string instance;
    std::string type;  // MARK_DEBUG_PROBE / ...
    bool enabled = true;
};

struct CwcSnapshot {
    bool ok = false;
    std::string error;
    std::string path;
    std::string version;
    std::string project;
    std::string chip;
    std::string clock;
    std::string instance;
    int sample_depth = 0;
    int trigger_position = 0;
    int window_num = 0;
    int window_depth = 0;
    std::string trigger_type;
    bool trigger_enabled = false;
    std::vector<CwcBus> buses;
    int net_count = 0;
};

CwcSnapshot ParseCwc(std::string_view text);
CwcSnapshot ReadCwc(const std::string& path);

// -----------------------------------------------------------------------------
// write_hw_cw_data 的输出 CSV
// -----------------------------------------------------------------------------

struct CaptureColumn {
    std::string name;  // 表头原样（含 `[15:0]`）
    std::string bus;   // 去掉 `[..]` 后的总线名
    int msb = 0;
    int lsb = 0;
    int width = 0;
};

struct CaptureRow {
    int index = 0;
    bool trigger = false;
    std::vector<std::uint64_t> values;  // 与 columns 一一对应
};

struct CaptureData {
    bool ok = false;
    std::string error;
    std::vector<CaptureColumn> columns;
    std::vector<CaptureRow> rows;
    int trigger_index = -1;
};

CaptureData ParseCaptureCsv(std::string_view text);

// -----------------------------------------------------------------------------
// 路径约定
// -----------------------------------------------------------------------------

std::string RunsDir(const project::Project& project);
std::string ChipDebuggerDir(const project::Project& project);
std::string MdnPath(const project::Project& project);
std::string CompiledBit(const project::Project& project);
std::string CompiledCwc(const project::Project& project);
std::string CompiledData(const project::Project& project);

// -----------------------------------------------------------------------------
// 只读状态（不碰硬件，毫秒级）
// -----------------------------------------------------------------------------

// { ok, project_target, chip_debugger, mdn:{exists,signals,count}, watcher:{present,
//   compiled_cwc, compiled_bit, clock, sample_depth, buses, nets}, ready, reason }
jsonrpc::Json Status(const project::Project& project);

// 把请求的信号写成 user.mdn。signals 为空 = 读取当前清单（dry run，不落盘）。
// 返回 { ok, path, before, after, added, removed, written, reason }
jsonrpc::Json Configure(const project::Project& project, const std::vector<std::string>& signals,
                        bool write);

// -----------------------------------------------------------------------------
// 采集（碰硬件）
// -----------------------------------------------------------------------------

struct CaptureOptions {
    std::string cwd;             // TD 会话工作目录，空 = <project>/.fpgalsp/probe
    std::string instance;        // 空 = 从 compiled.cwc 读（通常是 auto_created_cwc1）
    std::string cable = "0";
    double wait_timeout = 30.0;  // wait_on_hw_cw -timeout
    bool download = true;        // 先 download -mode jtag_burst 把带探针的 bit 灌进去
    bool trigger_now = true;     // 立即触发（不依赖当前触发条件）
    double session_timeout = 900.0;
    bool keep_csv = true;
    std::string csv_path;  // 空 = <runs>/phy_1/cw/capture_<ts>.csv
};

// 返回 { ok, stage, reason, instance, clock, sample_depth, trigger_index,
//        columns:[{bus,width}], signals:{bus:[值…]}, samples, csv, log }
jsonrpc::Json Capture(const td::TdChoice& td, const project::Project& project,
                      const CaptureOptions& options);

}  // namespace fpgalsp::probe