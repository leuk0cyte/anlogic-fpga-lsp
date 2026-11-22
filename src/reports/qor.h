// =============================================================================
//  src/reports/qor.h —— TD QoR 报告解析（*_Runs/<run>/{route,place,gate}.qor）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\reports.py
//      parse_qor() / parse_suggestions_tcl()
//
//  报告形态（TD 2026.2，`|` 分隔的管道表）：
//      -----------------------------------------------------------
//      | QoR metrics               |         value               |
//      -----------------------------------------------------------
//      | Timing                    |                             |   <- 空 value = 分组标题
//      |     Setup WNS             |          171                |
//      |     Setup TNS             |            0                |
//      -----------------------------------------------------------
//      | Utilization               |                             |
//      |     #slices               |         6029                |
//
//  ⚠ 解析要点
//    * value 为空的行是**分组标题**，后续行都归入该组（group 字段）。
//    * "| QoR metrics | value |" 这类表头必须跳过（大小写不敏感）。
//    * 建议项是另一张表（通常没有），格式 `| QOR-1302 | MEDIUM | - Route ... |`；
//      QOR-\\d{3,5} 起头开始一条，后续同表行追加到这条的 text 里（换行分隔）。
//    * 器件族差异不体现在 .qor 上（PH1/EF2 字段集一致），但仍不假设字段名：
//      metrics 原样保留 key/value 字符串，不做语义映射。
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::reports {

// 一条 QoR 指标（未做任何语义解释）
struct QorMetric {
    std::string group;  // 所属分组标题；第一条分组标题之前的行是 ""
    std::string key;    // 如 "Setup WNS"（保留 TD 的前导空格已被 strip 掉）
    std::string value;  // 如 "171" / "61.52%"，原样字符串
};

// 一条 QoR 建议
struct QorSuggestion {
    std::string id;        // "QOR-1302"
    std::string priority;  // CRITICAL / HIGH / MEDIUM / LOW；没有时为空
    std::string text;      // 建议正文，多行用 "\n" 连接
    std::string file;      // 仅 parse_suggestions_tcl 产生的条目会带
};

struct QorReport {
    std::string step;  // "route" / "place" / "gate" / "synthesis" ...
    std::vector<QorMetric> metrics;
    std::vector<QorSuggestion> suggestions;
    // 解析过程中的问题（畸形/空报告）。不进 ToJson，见完成报告里的说明。
    std::vector<std::string> warnings;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// 解析 .qor 文本。never-throw：空文本/二进制垃圾返回空 metrics + warnings。
// step 只用于回填输出的 step 字段（Python 由调用方按文件名判定）。
QorReport ParseQor(std::string_view text, std::string_view step = {});

// 解析 `<step>_qor_suggestion.tcl`：TD 写出的纯 set_param 处方。
// 每行一条，跳过空行与 # 注释。never-throw（文件打不开返回空表）。
std::vector<QorSuggestion> ParseSuggestionsTcl(std::string_view path);

}  // namespace fpgalsp::reports