// =============================================================================
//  src/reports/reports_aggregate.h —— fpgaLsp/reports 的汇总入口
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\reports.py::collect()
//  协议：Y:\fpga-lsp\PROTOCOL-V2.md §2 + v13 附录（分阶段 syn/phy）
//
//  输出骨架（结构见 BuildReports 的说明与 ToJson）：
//      {
//        ok, error?, generatedAt,
//        artifacts: {bit, syn_db, route_db, runs[]},
//        files:     {qor, timing, area, flow_status, bit},
//        qor?, timing?, area?, flowStatus?,
//        syn?: {ok, label, files, qor?, timing?, area?, flowStatus?},
//        phy?: {ok, label, files, qor?, timing?, area?, flowStatus?}
//      }
//
//  ⚠ flowStatus 有**两份**，语义不同（v13 的关键点）：
//    * 顶层 flowStatus.steps：syn 与 phy 两阶段的步骤**合并**，每步带 stage="syn"/"phy"；
//      另带 stages={syn:..., phy:...} 供前端分页展示。
//    * syn.flowStatus / phy.flowStatus：只有本阶段的步骤，且**不带** stage 标签。
//    这是 Python 版的实际行为（server 端的 webview 依赖它），不要"顺手统一"。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "reports/area.h"
#include "reports/artifacts.h"
#include "reports/flow_status.h"
#include "reports/qor.h"
#include "reports/timing.h"

namespace fpgalsp::reports {

// 用到的报告文件路径（找不到的字段是空串）
struct ReportsFiles {
    std::string qor;
    std::string timing;
    std::string area;
    std::string flow_status;
    std::string bit;

    jsonrpc::Json ToJson(bool include_bit) const;
};

// 一个阶段（Synthesis / Place & Route）的报告
struct StageSection {
    bool ok = false;
    std::string label;  // "Synthesis" / "Place & Route"
    ReportsFiles files;
    std::optional<QorReport> qor;
    std::optional<TimingReport> timing;
    std::optional<AreaReport> area;
    std::optional<FlowStatusReport> flow_status;

    jsonrpc::Json ToJson() const;
};

struct ReportsBundle {
    bool ok = false;
    std::string error;
    ArtifactsReport artifacts;
    ReportsFiles files;
    std::optional<QorReport> qor;
    std::optional<TimingReport> timing;
    std::optional<AreaReport> area;
    std::optional<FlowStatusReport> flow_status;
    std::optional<StageSection> syn;
    std::optional<StageSection> phy;
    double generated_at = 0.0;

    // v13：顶层 flowStatus.stages = {"syn": <syn.flowStatus 的 JSON>, "phy": ...}
    // 只包含**实际存在**的阶段（与 Python 的 `if export.get(key)` 一致），保序。
    std::vector<std::pair<std::string, jsonrpc::Json>> flow_stages;

    // 解析过程中所有子解析器的 warnings（不进 ToJson，供日志/测试）
    std::vector<std::string> warnings;

    jsonrpc::Json ToJson() const;
};

// 汇总一个工程的最新一次 flow 的所有报告。
// never-throw：没有任何 runs 目录时返回 ok=false + error，不抛异常。
ReportsBundle BuildReports(const project::Project& project);

}  // namespace fpgalsp::reports