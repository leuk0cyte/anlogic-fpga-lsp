// =============================================================================
//  src/flow/flow_status.h —— TD Flow Statistics 表 + 步骤标记 -> GUI 的 6 个步骤
// =============================================================================
//  背景（为什么需要这层）：
//    TD GUI 的 Flow 视图是**恰好 6 个步骤**，来自 TD 安装目录
//    scripts/DefaultFlow.tcl：stepList = {read_design opt_rtl opt_gate opt_place
//    opt_route bitgen}，每个 step 一个 *_fun proc，每步前后都有
//    .<step>.begin.f / .end.f / .error.f 标记（step_begin / step_end / step_error）
//    与 tcl_beforestep / tcl_afterstep 钩子：
//
//      run     GUI 名称              DefaultFlow proc   参数命名空间
//      syn_1   Read Design           read_design_fun    design
//      syn_1   Optimize RTL          opt_rtl_fun        rtl
//      syn_1   Optimize Gate         opt_gate_fun       gate
//      phy_1   Optimize Placement    opt_place_fun      place
//      phy_1   Optimize Routing      opt_route_fun      route
//      phy_1   Generate Bitstream    bitgen_fun         bitgen
//
//    但 flow.status 里的 "Flow Statistics" 表是**命令级**的（optimize_gate /
//    optimize_rtl / import_db / read_adc / ...），一个 GUI 步骤往往对应多条命令：
//    例如 Read Design = import_device + import_db + read_adc + read_sdc
//    （DefaultFlow 的 prepare proc 里做的那些 import_*/read_* 都算这一步）。
//    所以：
//      * 顺序与 done/running/failed/pending 从**标记文件**拿；
//      * 每一步的耗时从**表**里按 GUI 步骤聚合（同一 GUI 步骤下多条命令的 wall/cpu 相加）。
//
//  ---- 数据源（都实测过）----
//    <run_dir>/flow.status        syn run 的 opt_gate_fun 末尾 flow_status -file flow.status
//    <run_dir>/place_flow.status  opt_place_fun 末尾
//    <run_dir>/route_flow.status  opt_route_fun 末尾
//    表形态（行按耗时降序，末尾有 total 行）：
//        Flow Statistics  :
//        ---------------------------------------------------------------
//             command     |  #calls  |  wall time   |  cpu time(s)  | ...
//        ---------------------------------------------------------------
//          optimize_gate  |    1     |  1m11s(62%)  |   238(83%)    | ...
//            import_db    |    1     |    1s(0%)    |     0(0%)     | ...
//        ---------------------------------------------------------------
//              total      |    6     |    1m53s     |      285      | ...
//
//  ⚠ 单位：wall 是 "1m11s(62%)" 这种带百分比后缀的字符串，cpu 是 "238(83%)"，
//    解析后统一成**秒（double）**，百分比后缀丢弃；mem 取 used memory(mb) 列（整数）。
//    wall 还可能带小数（"12.5s"）或更长单位（"1h2m3s"），解析器都要吃。
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::flow {

// Flow 视图里的一步（对应 TD GUI 的一行）
struct FlowStep {
    std::string name;      // GUI 步骤名："Read Design" / "Optimize RTL" / ...
    std::string stage;     // 参数命名空间：design/rtl/gate/place/route/bitgen
    std::string status;    // pending | running | done | failed
    double wall = 0.0;     // 墙钟时间（秒）；no flow.status 时为 0
    double cpu = 0.0;      // CPU 时间（秒）
    long long mem_mb = 0;  // used memory(mb)
};

// GUI 的 6 步模板（顺序即 GUI 顺序，status=pending、耗时全 0）。
// 顺序与 DefaultFlow.tcl 的 stepList 完全一致，不要按字母重排。
const std::vector<FlowStep>& GuiFlowSteps();

// "1m11s(62%)" -> 71.0 ；"40s" -> 40 ；"1h2m3s" -> 3723 ；"12.5s" -> 12.5 ；
// "0s(0%)" -> 0 ；纯数字 "238" 按秒处理。百分比后缀与空白忽略。
double ParseFlowStatusSeconds(std::string_view text);

// 解析一张 Flow Statistics 表 -> 按 GUI 步骤聚合后的步骤列表。
//   * 只返回表里**真的出现过**的 GUI 步骤（syn 的 flow.status 通常只有 3 条）；
//   * 同一 GUI 步骤下多条命令的 wall/cpu 相加，mem 取最大值
//     （used memory 是占用快照，相加没有物理意义）；
//   * status 一律留空（表只能证明"这一步跑过、耗时多少"，完成/失败要看标记文件）。
std::vector<FlowStep> ParseFlowStatus(std::string_view text);

// 单步重跑（GUI 的 Reset and Run）前清理统计表：把 `marker` 这一步**及其下游**的
// 耗时行删掉，表头/分隔线/total 等其它行原样保留。为什么需要：CollectFlowSteps 会把
// "表里有耗时"的 pending 步骤升级成 done（表能证明跑过），如果只删标记文件不动表，
// 右键 Read Design 之后 Optimize RTL/Gate 仍会显示 done（2026-11-22 运行期审计实测）。
// marker 取 GUI 步骤标记名：read_design / opt_rtl / opt_gate / opt_place / opt_route / bitgen。
// 不认识的 marker 或空文本原样返回。
std::string StripFlowStatusFromMarker(std::string_view text, std::string_view marker);

// 一个 run 目录 -> 该 run 的那几步（syn_* 3 步 / phy_* 3 步 / route_* 2 步）。//   * status：.<step>.error.f -> failed；.<step>.end.f -> done；
//             .<step>.begin.f -> running；都没有 -> pending；
//   * 耗时：先读 flow.status，再合并 *_flow.status（place_flow.status /
//     route_flow.status）。同一命令在多个文件里出现时**取较大值**：这些文件是
//     同一 run 的**累计快照**，相加会把前面的步骤算两遍；
//   * 没有 flow.status 时不报错，wall/cpu/mem 全 0，status 仍按标记文件给出；
//   * 表里有耗时但标记文件已被清掉的旧 run：把 pending 升级成 done（表能证明跑过）。
// never-throw：目录不存在时返回全 pending 的步骤。
std::vector<FlowStep> CollectFlowSteps(const std::string& run_dir, const std::string& run_name);

// 一个 run（syn_1 / phy_1 / route_1）在 6 步里的那部分。
struct RunFlowSteps {
    std::string run_name;         // "syn_1" / "phy_1" / "route_1"；空表示没有可用的 run
    std::vector<FlowStep> steps;  // 只含这个 run 负责的步骤，按 GUI 顺序
};

// <project>_Runs 目录 -> {syn run, phy run} 两段（固定两条，缺的那条 steps 为空）。
//   run 选择：优先 syn_1 / phy_1，否则取第一个同前缀的目录；一个 syn/phy 都没有时
//   才回落到 route_*（增量布线，只有 route + bitgen 两步）。
std::vector<RunFlowSteps> CollectRunsFlowSteps(const std::string& runs_root);

// 上面两段摊平成固定的 6 条（缺的步骤补 pending、耗时 0），顺序即 GUI 顺序。
// 供 flowStatus 的 steps 数组与单测使用。
std::vector<FlowStep> CollectAllFlowSteps(const std::string& runs_root);

// 6 条步骤 -> JSON 数组（{name,stage,status,wall,cpu,mem_mb,run}）。
// 两个通道（LSP flowStatus / MCP fpga_flow_status）共用，避免两边字段写歪。
jsonrpc::Json StepsToJson(const std::vector<FlowStep>& steps);

}  // namespace fpgalsp::flow