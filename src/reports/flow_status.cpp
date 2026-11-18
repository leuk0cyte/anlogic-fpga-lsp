// =============================================================================
//  src/reports/flow_status.cpp —— TD 流程状态报告解析
// =============================================================================
//  逐行对照 Y:\fpga-lsp\fpgalsp\reports.py::parse_flow_status_text（382-405 行）。
// =============================================================================

#include "reports/flow_status.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "reports/report_patterns_internal.h"
#include "reports/report_text.h"

namespace fpgalsp::reports {
namespace {

// 注意 `Setup`/`Hold` 的正则用 re.search（不是 match），且允许多个空格/换行
constexpr std::string_view kSetupPattern =
    R"(Setup\s*:\s*WNS\s+(-?\d+)ps\s+TNS\s+(-?\d+)ps\s+NUM_FEPS\s+(\d+))";
constexpr std::string_view kHoldPattern =
    R"(Hold\s*:\s*WNS\s+(-?\d+)ps\s+TNS\s+(-?\d+)ps\s+NUM_FEPS\s+(\d+))";
constexpr std::string_view kModePattern = R"(Timing Mode\s*:\s*(\S+))";
// 逐行 re.match：行首空白 + 小写步骤名 + 6 个管道列（后 4 列之后的内容不校验）
constexpr std::string_view kStepPattern =
    R"(^\s*([a-z_]+)\s*\|\s*(\d+)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*(\d+)\s*\|\s*(\d+))";

struct PatternEntry {
    std::string_view pattern;
};

constexpr PatternEntry kPatterns[] = {
    {kSetupPattern}, {kHoldPattern}, {kModePattern}, {kStepPattern},
};

}  // namespace

namespace detail {

bool ValidateFlowStatusPatterns(std::string* error) {
    for (const PatternEntry& entry : kPatterns) {
        const RE2 probe{std::string(entry.pattern)};
        if (!probe.ok()) {
            if (error != nullptr) {
                *error = "reports/flow_status: bad pattern '" + std::string(entry.pattern) +
                         "': " + probe.error();
            }
            return false;
        }
    }
    return true;
}

}  // namespace detail

jsonrpc::Json FlowStatusReport::ToJson() const {
    auto stats_json = [](const std::optional<FlowTimingStats>& stats) {
        if (!stats.has_value()) return jsonrpc::Json(nullptr);
        jsonrpc::Json item = jsonrpc::Json::object();
        item["wns_ps"] = stats->wns_ps;
        item["tns_ps"] = stats->tns_ps;
        item["feps"] = stats->feps;
        return item;
    };

    jsonrpc::Json out = jsonrpc::Json::object();
    out["setup"] = stats_json(setup);
    out["hold"] = stats_json(hold);
    if (mode.has_value()) {
        out["mode"] = *mode;
    } else {
        out["mode"] = nullptr;
    }

    jsonrpc::Json steps_json = jsonrpc::Json::array();
    for (const FlowStep& step : steps) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = step.name;
        item["calls"] = step.calls;
        item["wall"] = step.wall;
        item["cpu"] = step.cpu;
        item["mem_mb"] = step.mem_mb;
        item["peak_mb"] = step.peak_mb;
        // Python 里 stage 是聚合层回填的；单文件解析结果不带这个键
        if (!step.stage.empty()) item["stage"] = step.stage;
        steps_json.push_back(std::move(item));
    }
    out["steps"] = std::move(steps_json);
    return out;
}

bool LooksLikeFlowStatus(std::string_view text) {
    return text.find("Flow Statistics") != std::string_view::npos ||
           text.find("NUM_FEPS") != std::string_view::npos;
}

FlowStatusReport ParseFlowStatusText(std::string_view text) {
    FlowStatusReport report;
    if (text.empty()) {
        report.warnings.emplace_back("empty-report");
        return report;
    }

    static const RE2 kSetupRe{std::string(kSetupPattern)};
    static const RE2 kHoldRe{std::string(kHoldPattern)};
    static const RE2 kModeRe{std::string(kModePattern)};
    static const RE2 kStepRe{std::string(kStepPattern)};

    const re2::StringPiece whole(text.data(), text.size());

    std::string wns;
    std::string tns;
    std::string feps;
    if (RE2::PartialMatch(whole, kSetupRe, &wns, &tns, &feps)) {
        FlowTimingStats stats;
        (void)ParseInt32(wns, stats.wns_ps);
        (void)ParseInt32(tns, stats.tns_ps);
        (void)ParseInt32(feps, stats.feps);
        report.setup = stats;
    }
    if (RE2::PartialMatch(whole, kHoldRe, &wns, &tns, &feps)) {
        FlowTimingStats stats;
        (void)ParseInt32(wns, stats.wns_ps);
        (void)ParseInt32(tns, stats.tns_ps);
        (void)ParseInt32(feps, stats.feps);
        report.hold = stats;
    }
    std::string mode;
    if (RE2::PartialMatch(whole, kModeRe, &mode)) report.mode = mode;

    for (const std::string_view raw_line : SplitLines(text)) {
        std::string name;
        std::string calls;
        std::string wall;
        std::string cpu;
        std::string mem;
        std::string peak;
        re2::StringPiece input(raw_line.data(), raw_line.size());
        if (!RE2::Consume(&input, kStepRe, &name, &calls, &wall, &cpu, &mem, &peak)) continue;

        FlowStep step;
        step.name = std::move(name);
        (void)ParseInt32(calls, step.calls);
        step.wall = std::string(Strip(wall));
        step.cpu = std::string(Strip(cpu));
        (void)ParseInt32(mem, step.mem_mb);
        (void)ParseInt32(peak, step.peak_mb);
        report.steps.push_back(std::move(step));
    }

    if (!report.setup.has_value() && !report.hold.has_value()) {
        report.warnings.emplace_back("no-timing-status");
    }
    if (report.steps.empty()) report.warnings.emplace_back("no-flow-steps");
    return report;
}

}  // namespace fpgalsp::reports