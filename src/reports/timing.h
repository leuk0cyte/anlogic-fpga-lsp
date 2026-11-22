// =============================================================================
//  src/reports/timing.h —— TD 时序报告解析（final_timing.rpt / *_pr.timing / *_gate.timing）
// =============================================================================
//  对应参考实现：
//    Y:\fpga-lsp\fpgalsp\reports.py::parse_timing()（102-294 行）
//    Y:\fpga-lsp\fpgalsp\timing_map.py（begin_at / end_at 端点映射）
//
//  报告结构（一份真实报告的分段，见 tests/fixtures/reports/eg4_final_timing.rpt）：
//
//      Created by          : Tang Dynasty Release_2026.2       <-- metadata
//                          : Copyright (c) ...                   <-- 续行（行首冒号）
//      STA coverage        : 99.64%
//      Confidence          : Routed
//
//      Check Status                                             <-- check_status
//      Slow     | yes      yes
//
//      Timing Status                                            <-- summary（ns）
//          SWNS: 0.171ns, STNS: 0.000ns
//          HWNS: 0.023ns, HTNS: 0.000ns
//          Period Check WNS: 0.000ns
//
//      Clock Summary                                            <-- clocks
//                 clk0:       64.000         4931   pll_64m/...
//                 Name  Type  Rise(ns) ...                     <-- clock_detail（17 列）
//
//      Path Group     :     A -> B                              <-- groups[i]
//      Type           :     Self
//      Statistics:
//      Max            : SWNS 0.171ns, STNS 0.000ns, 0 Viol Endpoints, 15840 Total Endpoints
//      Max Paths
//      Slack               : 0.171ns                            <-- groups[i].paths[j]
//      Begin Point         : reg_init/reg352_or_reg[2]_syn_4.clk (rising edge ...)
//      End Point           : SOUT[7]_syn_6.do[0] (...)
//      Delay Type  Location  Incr  Path  Level  Netlist Info     <-- 延迟表（原样保存）
//
//  ⚠ 器件族/阶段差异
//    * syn 阶段的报告（*_gate.timing）与 route 阶段（final_timing.rpt）分段结构相同；
//      EF2/PH1 之间也只差数值，格式一致。
//    * 但 *_chk.timing / *_exception.timing 是**另一种报告**（检查清单 / 时序例外表），
//      里面没有 Path Group，必须有兜底路径（legacy Slack 扫描）且不能崩。
// =============================================================================

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::reports {

// -----------------------------------------------------------------------------
// begin_at / end_at：路径端点 -> 源码位置
// -----------------------------------------------------------------------------
//  协议 v3 要求每个 timing path 带 begin_at / end_at：
//      {signal, hierarchy, file, line, character, resolved, module, reason}
//
//  ⚠ 当前 HDL 索引尚未实现（阶段 5 才有），所以**一律降级**为：
//      resolved=false, reason="hdl-index-unavailable"，其余定位字段为 null。
//      signal / hierarchy 仍然做（纯文本就能算出来），这样 webview 还能显示端点名字。
//  TODO(阶段5)：接入 hdl 索引后在这里查实例链 -> 模块 -> 模块行范围内的信号名，
//      逐字段对齐 timing_map.py::resolve 的返回（含 raw_signal / walked 语义）。
struct TimingEndpoint {
    std::string signal;                 // 去掉 [n] 位选后的信号名
    std::vector<std::string> hierarchy;  // 从 top 起的实例链（不含信号本身）
    std::optional<std::string> file;     // 未解析 -> nullopt
    std::optional<int> line;             // 0-based；未解析 -> nullopt
    std::optional<int> character;
    bool resolved = false;
    std::optional<std::string> module;
    std::optional<std::string> reason;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// 一条时序路径
struct TimingPath {
    double slack = 0.0;
    std::string begin;
    std::string end;
    std::string check;
    std::string process;
    // details 是保序的 label -> value（重复 label 后者覆盖前者）
    std::vector<std::pair<std::string, std::string>> details;
    std::vector<std::string> delay;  // 延迟表原文（已 rstrip）
    // 只在"从 groups 展平"的副本上有 group；groups 内的 paths 没有
    std::optional<std::string> group;

    // legacy 兜底路径（报告里没有 Path Group 段时）只有 slack/begin/end/check/process，
    // 没有 details / delay / group，JSON 必须与之完全一致。
    bool legacy = false;

    std::optional<TimingEndpoint> begin_at;
    std::optional<TimingEndpoint> end_at;

    // v3 的 begin_at / end_at 只挂在**展平后**的顶层 paths 上（与 server.py 的
    // _decorate_reports 一致）；groups[i].paths[j] 里不带这两个键，形状对齐 Python。
    [[nodiscard]] jsonrpc::Json ToJson(bool with_endpoints = false) const;
};

// Statistics: 段里的 Max/Min 行
struct TimingStatistics {
    double swns_ns = 0.0;
    double stns_ns = 0.0;
    int viol_endpoints = 0;
    int total_endpoints = 0;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

struct TimingGroup {
    std::string name;
    std::string type;
    std::string from_clock;
    std::string to_clock;
    std::string min_period;
    std::string fmax;
    std::optional<TimingStatistics> max_statistics;  // statistics["max"]
    std::optional<TimingStatistics> min_statistics;  // statistics["min"]
    std::vector<TimingPath> paths;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

struct TimingClock {
    std::string name;      // clk0 / clk1 / ...
    double freq_mhz = 0.0;  // Clock Summary 表里的 C-Freq
    int fanout = 0;
    std::string source;  // 时钟源网络名

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// Clock Summary 下面那张 17 列详表的一行
struct TimingClockDetail {
    std::vector<std::pair<std::string, std::string>> fields;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

struct TimingCheckStatusRow {
    std::string corner;  // Slow / Fast
    std::string max;     // yes / no
    std::string min;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

struct TimingReport {
    std::map<std::string, double> summary;  // SWNS / STNS / HWNS / HTNS / Period_Check_WNS
    std::optional<double> coverage;         // STA coverage (%)
    std::optional<std::string> confidence;  // Routed / Synthesized
    std::vector<TimingClock> clocks;
    std::vector<TimingClockDetail> clock_detail;
    std::vector<TimingPath> paths;  // 展平后的路径（带 group）
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<TimingCheckStatusRow> check_status;
    std::vector<TimingGroup> groups;
    std::string raw;
    std::string file;

    // v3：端点解析统计。只在 paths 非空时给出（与 server.py 一致）。
    std::optional<std::pair<int, int>> begin_end_resolved;  // {total, resolved}

    std::vector<std::string> warnings;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// 解析时序报告。never-throw。
// 解析完成后自动做一次端点降级映射（见 TimingEndpoint 的说明）。
TimingReport ParseTiming(std::string_view text, std::string_view file = {});

// 只做端点映射这一步（ParseTiming 内部调用；也为将来接入 HDL 索引留出入口）。
void AttachEndpointLocations(TimingReport& report);

// 等价 timing_map.clean_point：
//   'a/b/c_reg[0]_syn_4.c[0] (rising edge ...)' -> {"a","b","c_reg[0]"}
std::vector<std::string> CleanTimingPoint(std::string_view text);

}  // namespace fpgalsp::reports