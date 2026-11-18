// =============================================================================
//  src/reports/flow_status.h —— TD 流程状态报告（*_flow.status / flow.status）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\reports.py::parse_flow_status_text()
//
//  报告形态（TD 2026.2 phy_1/route_flow.status）：
//      Location         : D:\...\CorpusBeta_Runs\phy_1
//      Top Model        : main
//      Device           : EG4X20BG256
//      Speed            : NA
//
//      Timing Mode      : final
//      Setup            : WNS   171ps  TNS        0ps  NUM_FEPS     0
//      Hold             : WNS    23ps  TNS        0ps  NUM_FEPS     0
//
//      Flow Statistics  :
//      ------------------------------------------------------------
//           command     |  #calls  |  wall time  |  cpu time(s)  | ...
//      ------------------------------------------------------------
//            route      |    1     |  48s(68%)   |    31(73%)    | ...
//      ------------------------------------------------------------
//
//  ⚠ 注意 WNS/TNS 的单位是 **ps（整数）**，与 timing 报告的 ns（浮点）不是一回事，
//    不要混用；协议 §2 里 timing.summary 的 SWNS 是 ns。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::reports {

// Setup / Hold 时序统计（单位 ps）
struct FlowTimingStats {
    int wns_ps = 0;
    int tns_ps = 0;
    int feps = 0;  // NUM_FEPS：被违反的端点数量
};

// Flow Statistics 表的一行
struct FlowStep {
    std::string name;   // route / place / fix_hold / import_db / ... / total
    int calls = 0;
    std::string wall;   // 原样字符串 "48s(68%)"
    std::string cpu;    // 原样字符串 "31(73%)"
    int mem_mb = 0;
    int peak_mb = 0;
    std::string stage;  // "syn" / "phy"：聚合阶段才回填，单文件解析时为空
};

struct FlowStatusReport {
    std::optional<FlowTimingStats> setup;
    std::optional<FlowTimingStats> hold;
    std::optional<std::string> mode;  // "final" / "estimated" / ...
    std::vector<FlowStep> steps;
    std::vector<std::string> warnings;

    jsonrpc::Json ToJson() const;
};

// 解析一个 *_flow.status。never-throw：找不到任何字段时返回空结构 + warnings。
FlowStatusReport ParseFlowStatusText(std::string_view text);

// 该文本像不像一份 flow status（给聚合层判断用）
bool LooksLikeFlowStatus(std::string_view text);

}  // namespace fpgalsp::reports