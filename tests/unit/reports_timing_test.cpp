// =============================================================================
//  tests/unit/reports_timing_test.cpp —— 时序报告解析（最复杂的一个）
// =============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "reports/report_text.h"
#include "reports/timing.h"

namespace {

using fpgalsp::reports::CleanTimingPoint;
using fpgalsp::reports::ParseTiming;
using fpgalsp::reports::ReadReportFile;
using fpgalsp::reports::TimingReport;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

const std::string* FindMeta(const TimingReport& report, const std::string& key) {
    for (const auto& [field, value] : report.metadata) {
        if (field == key) return &value;
    }
    return nullptr;
}

}  // namespace

// -----------------------------------------------------------------------------
// Timing Status / 元数据 / 时钟
// -----------------------------------------------------------------------------

TEST(ReportsTiming, ParsesTimingStatusSummary) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");

    ASSERT_EQ(report.summary.size(), 5u);
    EXPECT_DOUBLE_EQ(report.summary.at("SWNS"), 0.171);
    EXPECT_DOUBLE_EQ(report.summary.at("STNS"), 0.0);
    EXPECT_DOUBLE_EQ(report.summary.at("HWNS"), 0.023);
    EXPECT_DOUBLE_EQ(report.summary.at("HTNS"), 0.0);
    // "Period Check WNS" 的空格被换成下划线
    EXPECT_DOUBLE_EQ(report.summary.at("Period_Check_WNS"), 0.0);
}

// 综合阶段的报告 WNS 是负的（没收敛），解析不能丢负号
TEST(ReportsTiming, ParsesNegativeSummary) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("ef2_gate.timing")), "ef2_gate.timing");
    EXPECT_DOUBLE_EQ(report.summary.at("SWNS"), -0.861);
    EXPECT_DOUBLE_EQ(report.summary.at("STNS"), -23.866);
}

TEST(ReportsTiming, ParsesMetadataWithContinuationLines) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");

    ASSERT_EQ(report.metadata.size(), 11u);
    const std::string* created = FindMeta(report, "Created by");
    ASSERT_NE(created, nullptr);
    // "                    : Copyright ..." 这一行是上一条的续行
    EXPECT_EQ(*created,
              "Tang Dynasty Release_2026.2 Copyright (c) 2012-2026 Anlogic Inc.");
    const std::string* device = FindMeta(report, "Device");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(*device, "EG4X20BG256");
    // 元数据在 "Check Status" 处截止，"Check Status" 自己不是一条元数据
    EXPECT_EQ(FindMeta(report, "Check Status"), nullptr);
}

TEST(ReportsTiming, ParsesCheckStatusCorners) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_EQ(report.check_status.size(), 2u);
    EXPECT_EQ(report.check_status[0].corner, "Slow");
    EXPECT_EQ(report.check_status[0].max, "yes");
    EXPECT_EQ(report.check_status[1].corner, "Fast");
}

TEST(ReportsTiming, ParsesClockSummary) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_EQ(report.clocks.size(), 3u);
    EXPECT_EQ(report.clocks[0].name, "clk0");
    EXPECT_DOUBLE_EQ(report.clocks[0].freq_mhz, 64.0);
    EXPECT_EQ(report.clocks[0].fanout, 4931);
    EXPECT_EQ(report.clocks[0].source, "pll_64m/pll_inst.clkc[0]");
    EXPECT_EQ(report.clocks[2].name, "clk2");
    EXPECT_EQ(report.clocks[2].fanout, 1);
}

// Clock Summary 下面那张 17 列详表：列值必须全部按名字而不是位置取
TEST(ReportsTiming, ParsesClockDetailSeventeenColumns) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_EQ(report.clock_detail.size(), 3u);
    const auto& first = report.clock_detail[0].fields;
    EXPECT_EQ(first.size(), 17u);
    EXPECT_EQ(first.at(0).first, "name");
    EXPECT_EQ(first.at(0).second, "clk0");
    EXPECT_EQ(first.at(4).first, "c_period_ns");
    EXPECT_EQ(first.at(4).second, "15.625");
    EXPECT_EQ(first.at(8).first, "swns_ns");
    EXPECT_EQ(first.at(8).second, "0.171");
    EXPECT_EQ(first.at(16).first, "cc_to");
    EXPECT_EQ(first.at(16).second, "no");

    const auto json = report.clock_detail[0].ToJson();
    EXPECT_EQ(json["fanout"], "4931");
    EXPECT_EQ(json["max_level"], "14");
}

TEST(ReportsTiming, ParsesCoverageAndConfidence) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_TRUE(report.coverage.has_value());
    EXPECT_DOUBLE_EQ(*report.coverage, 99.64);
    ASSERT_TRUE(report.confidence.has_value());
    EXPECT_EQ(*report.confidence, "Routed");

    const TimingReport ef2 = ParseTiming(ReadReportFile(Fixture("ef2_gate.timing")), "x");
    ASSERT_TRUE(ef2.confidence.has_value());
    EXPECT_EQ(*ef2.confidence, "Synthesized");
}

// 数字与 % 之间有空白也要能解析（报表分隔符形态不该被赌死）。
// TD 2026.2_SP1 修了 "STA Coverage 不准确"：标签没变、只有数值会变，
// 但分隔符若变成 `98.7 %`，旧 pattern `([\d.]+)%` 会静默解析失败 ->
// coverage 变 nullopt，前端只看到"没有覆盖率"。
TEST(ReportsTiming, ParsesCoverageWithSpaceBeforePercent) {
    const TimingReport spaced = ParseTiming("STA coverage : 98.7 %\n", "x");
    ASSERT_TRUE(spaced.coverage.has_value());
    EXPECT_DOUBLE_EQ(*spaced.coverage, 98.7);

    // 无空白的老形态必须不回归
    const TimingReport tight = ParseTiming("STA coverage : 99.64%\n", "x");
    ASSERT_TRUE(tight.coverage.has_value());
    EXPECT_DOUBLE_EQ(*tight.coverage, 99.64);
}

// -----------------------------------------------------------------------------
// Path Group / 路径
// -----------------------------------------------------------------------------

TEST(ReportsTiming, ParsesPathGroups) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_EQ(report.groups.size(), 4u);
    EXPECT_EQ(report.groups[0].name, "pll_64m/pll_inst.clkc[0] -> pll_64m/pll_inst.clkc[0]");
    EXPECT_EQ(report.groups[0].type, "Self");
    EXPECT_EQ(report.groups[0].min_period, "15.454ns");
    EXPECT_EQ(report.groups[0].fmax, "64.708MHz");
    EXPECT_EQ(report.groups[0].paths.size(), 20u);
    EXPECT_EQ(report.paths.size(), 64u);
}

// 同一份报告里可以出现**同名但不同段**的 Path Group（intra clock / inter clock 各一段），
// 解析必须按出现顺序各算一组，不能按名字合并。
TEST(ReportsTiming, DuplicateGroupNamesAreNotMerged) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_EQ(report.groups.size(), 4u);
    EXPECT_EQ(report.groups[1].name, report.groups[2].name);
    EXPECT_EQ(report.groups[1].paths.size(), 20u);
    EXPECT_EQ(report.groups[2].paths.size(), 4u);
    EXPECT_EQ(report.groups[3].paths.size(), 20u);
}

TEST(ReportsTiming, Ef2GroupsHaveCrossDomainTypes) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("ef2_gate.timing")), "ef2_gate.timing");
    ASSERT_EQ(report.groups.size(), 4u);
    EXPECT_EQ(report.groups[0].type, "Self");
    EXPECT_EQ(report.groups[2].type, "Same Domain");
    EXPECT_EQ(report.paths.size(), 40u);
}

TEST(ReportsTiming, ParsesPathFields) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    const auto& path = report.paths[0];
    EXPECT_DOUBLE_EQ(path.slack, 0.171);
    EXPECT_EQ(path.check, "setup");
    EXPECT_EQ(path.process, "slow");
    EXPECT_NE(path.begin.find("reg_init/reg352_or_reg[2]_syn_4.clk"), std::string::npos);
    EXPECT_NE(path.end.find("SOUT[7]_syn_6.do[0]"), std::string::npos);
    ASSERT_TRUE(path.group.has_value());
    EXPECT_EQ(*path.group, report.groups[0].name);

    // details 里除 begin/end/check/process 外还保留 Budget / Data Path Delay 等
    ASSERT_EQ(path.details.size(), 10u);
    EXPECT_EQ(path.details[0].first, "Begin Point");
    bool saw_delay = false;
    for (const auto& [label, value] : path.details) {
        if (label == "Data Path Delay") {
            saw_delay = true;
            EXPECT_NE(value.find("14.914ns"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_delay);

    // 延迟表按原样收下（含分隔线），行数固定可数
    EXPECT_EQ(path.delay.size(), 63u);
    EXPECT_EQ(path.delay.front().substr(0, 4), std::string("----")) << "第一条是分隔线";
    EXPECT_NE(path.delay[1].find("source latency"), std::string::npos);
}

// ⚠ PYTHON-BUGS P-8：Statistics 的正则写成 `(Max|Min)\s*:\s*SWNS`，
// 而真实报告是 "Min : HWNS ..."，所以 min 统计**永远缺失**。
// 这条测试钉住当前（忠实复现）行为；若将来修 reports.py，这里会变红。
TEST(ReportsTiming, MinStatisticsIsMissingDueToPythonPattern) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_TRUE(report.groups[0].max_statistics.has_value());
    EXPECT_DOUBLE_EQ(report.groups[0].max_statistics->swns_ns, 0.171);
    EXPECT_EQ(report.groups[0].max_statistics->total_endpoints, 15840);
    EXPECT_FALSE(report.groups[0].min_statistics.has_value())
        << "reports.py 的 Min 行正则要求 SWNS，真实报告打的是 HWNS";

    const auto json = report.groups[0].ToJson();
    EXPECT_TRUE(json["statistics"].contains("max"));
    EXPECT_FALSE(json["statistics"].contains("min"));
}

// -----------------------------------------------------------------------------
// legacy 兜底：没有 Path Group 段（*_exception.timing / *_chk.timing）
// -----------------------------------------------------------------------------

TEST(ReportsTiming, LegacyFallbackFindsSlackRows) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_exception.timing")), "eg4_exception.timing");
    EXPECT_TRUE(report.groups.empty());
    ASSERT_EQ(report.paths.size(), 24u);
    const auto& path = report.paths[0];
    EXPECT_TRUE(path.legacy);
    EXPECT_DOUBLE_EQ(path.slack, 0.572);
    EXPECT_EQ(path.check, "setup");
    EXPECT_EQ(path.process, "slow");
    ASSERT_FALSE(path.begin.empty());
    EXPECT_FALSE(path.group.has_value());
    // legacy 条目只有 slack/begin/end/check/process（+ v3 的 begin_at/end_at）
    const auto json = path.ToJson(/*with_endpoints=*/true);
    EXPECT_FALSE(json.contains("details"));
    EXPECT_FALSE(json.contains("delay"));
    EXPECT_FALSE(json.contains("group"));
    EXPECT_TRUE(json.contains("begin_at"));
}

// *_chk.timing 是一种完全不同的报告（约束检查清单），解析必须是空结构而不是崩
TEST(ReportsTiming, CheckOnlyReportYieldsEmptyStructure) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_chk.timing")), "eg4_chk.timing");
    EXPECT_TRUE(report.summary.empty());
    EXPECT_TRUE(report.groups.empty());
    EXPECT_TRUE(report.paths.empty());
    EXPECT_TRUE(report.clocks.empty());
    EXPECT_TRUE(report.clock_detail.empty());
    EXPECT_TRUE(report.check_status.empty());
    // 意外收获：两行 "X : Y" 会被当作元数据（Python 同样如此）
    ASSERT_EQ(report.metadata.size(), 2u);
}

// -----------------------------------------------------------------------------
// 端点映射（v3）
// -----------------------------------------------------------------------------

TEST(ReportsTiming, CleanTimingPointStripsTdSuffixes) {
    const std::vector<std::string> parts = CleanTimingPoint(
        "ad_7924/AD_ACQ/ncs_cnt_reg[6]_syn_4.clk (falling edge triggered by clock clk)");
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "ad_7924");
    EXPECT_EQ(parts[1], "AD_ACQ");
    // .clk 引脚后缀与 _syn_4 内部后缀都要去掉
    EXPECT_EQ(parts[2], "ncs_cnt_reg[6]");
}

TEST(ReportsTiming, CleanTimingPointHandlesEmptyAndOddInput) {
    EXPECT_TRUE(CleanTimingPoint("").empty());
    EXPECT_TRUE(CleanTimingPoint("(only a comment)").empty());
    const std::vector<std::string> parts = CleanTimingPoint("a//b/");
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
}

// HDL 索引还没实现：begin_at / end_at 一律降级为 resolved=false + 明确 reason，
// 但 signal / hierarchy 仍要算出来，前端至少能显示端点名字。
TEST(ReportsTiming, EndpointLocationsDegradeWhenHdlIndexMissing) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_TRUE(report.paths[0].begin_at.has_value());
    const auto& begin_at = *report.paths[0].begin_at;
    EXPECT_FALSE(begin_at.resolved);
    ASSERT_TRUE(begin_at.reason.has_value());
    EXPECT_EQ(*begin_at.reason, "hdl-index-unavailable");
    EXPECT_FALSE(begin_at.file.has_value());
    EXPECT_FALSE(begin_at.line.has_value());
    EXPECT_FALSE(begin_at.module.has_value());
    EXPECT_EQ(begin_at.signal, "reg352_or_reg");
    ASSERT_EQ(begin_at.hierarchy.size(), 1u);
    EXPECT_EQ(begin_at.hierarchy[0], "reg_init");

    const auto json = report.paths[0].ToJson(/*with_endpoints=*/true);
    EXPECT_EQ(json["begin_at"]["resolved"], false);
    EXPECT_TRUE(json["begin_at"]["file"].is_null());
    EXPECT_TRUE(json["begin_at"]["line"].is_null());
}

// v3 的 begin_at / end_at 只挂在展平后的顶层 paths 上；groups[i].paths[j] 里没有这两个键。
// 这是 Python server.py::_decorate_reports 的实际形状，webview 按这个形状读。
TEST(ReportsTiming, GroupPathsDoNotCarryEndpointKeys) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_FALSE(report.groups.empty());
    ASSERT_FALSE(report.groups[0].paths.empty());

    const auto group_json = report.groups[0].paths[0].ToJson();
    EXPECT_FALSE(group_json.contains("begin_at"));
    EXPECT_FALSE(group_json.contains("end_at"));

    const auto top_json = report.paths[0].ToJson(/*with_endpoints=*/true);
    EXPECT_TRUE(top_json.contains("begin_at"));
    EXPECT_TRUE(top_json.contains("end_at"));

    // TimingReport::ToJson 里两边各自正确
    const auto whole = report.ToJson();
    EXPECT_TRUE(whole["paths"][0].contains("begin_at"));
    EXPECT_FALSE(whole["groups"][0]["paths"][0].contains("begin_at"));
}

TEST(ReportsTiming, BeginEndResolvedCounters) {
    const TimingReport report =
        ParseTiming(ReadReportFile(Fixture("eg4_final_timing.rpt")), "final_timing.rpt");
    ASSERT_TRUE(report.begin_end_resolved.has_value());
    // 每条路径两个端点，全都没解析出来
    EXPECT_EQ(report.begin_end_resolved->first, 2 * static_cast<int>(report.paths.size()));
    EXPECT_EQ(report.begin_end_resolved->second, 0);

    const TimingReport empty = ParseTiming("", "");
    EXPECT_FALSE(empty.begin_end_resolved.has_value());
}

// -----------------------------------------------------------------------------
// 健壮性
// -----------------------------------------------------------------------------

TEST(ReportsTiming, EmptyInput) {
    const TimingReport report = ParseTiming("", "");
    EXPECT_TRUE(report.summary.empty());
    EXPECT_TRUE(report.paths.empty());
    EXPECT_TRUE(report.groups.empty());
    EXPECT_EQ(report.raw, "");
    EXPECT_FALSE(report.warnings.empty());

    const auto json = report.ToJson();
    EXPECT_TRUE(json["summary"].is_object());
    EXPECT_TRUE(json["coverage"].is_null());
    EXPECT_TRUE(json["confidence"].is_null());
}

TEST(ReportsTiming, TruncatedReport) {
    // 截断在 Path Group 中间：前面的 summary 保住了，路径只能拿到不收尾的那条
    const std::string text =
        "Created by          : Tang Dynasty Release_2026.2\n"
        "\n"
        "Timing Status\n"
        "--------------------\n"
        "\tSWNS: 1.234ns, STNS: 0.000ns\n"
        "\n"
        "Path Group     :     clk_a -> clk_a\n"
        "Type           :     Self\n"
        "Max Paths\n"
        "------------------------------------\n"
        "Slack               : 1.234ns\n"
        "Begin Point         : a_reg[0].clk (rising edge";
    const TimingReport report = ParseTiming(text, "truncated.rpt");
    EXPECT_DOUBLE_EQ(report.summary.at("SWNS"), 1.234);
    ASSERT_EQ(report.groups.size(), 1u);
    ASSERT_EQ(report.groups[0].paths.size(), 1u);
    EXPECT_DOUBLE_EQ(report.groups[0].paths[0].slack, 1.234);
    EXPECT_NE(report.groups[0].paths[0].begin.find("a_reg[0]"), std::string::npos);
}

TEST(ReportsTiming, BinaryGarbage) {
    std::string garbage(4096, '\0');
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<char>((i * 31) & 0xFF);
    }
    const TimingReport report = ParseTiming(garbage, "garbage.timing");
    EXPECT_TRUE(report.summary.empty());
    EXPECT_TRUE(report.paths.empty());
    const auto json = report.ToJson();
    EXPECT_TRUE(json["groups"].is_array());
}

// begin/end 只截到 240 字符；details 里的原文不截。超长行必须被截断而不是撑爆 JSON。
TEST(ReportsTiming, LongEndpointIsTruncated) {
    std::string begin = "top/" + std::string(400, 'a') + ".clk";
    const std::string text =
        "Path Group     :     clk_a -> clk_a\n"
        "Slack               : 1.000ns\n"
        "Begin Point         : " + begin + "\n"
        "End Point           : b_reg[1].d\n";
    const TimingReport report = ParseTiming(text, "long.rpt");
    ASSERT_EQ(report.paths.size(), 1u);
    EXPECT_EQ(report.paths[0].begin.size(), 240u);
    // details 保留完整值
    for (const auto& [label, value] : report.paths[0].details) {
        if (label == "Begin Point") EXPECT_EQ(value.size(), begin.size());
    }
}

// Delay 表在遇到空行时结束，之后的行不能再被吞进 delay
TEST(ReportsTiming, DelayTableStopsAtBlankLine) {
    const std::string text =
        "Path Group     :     clk_a -> clk_a\n"
        "Slack               : 1.000ns\n"
        "Delay Type          Location   Incr   Path\n"
        "----------------------------------------\n"
        "source latency                 0.000  0.000\n"
        "\n"
        "Check Type          : setup\n";
    const TimingReport report = ParseTiming(text, "x");
    ASSERT_EQ(report.paths.size(), 1u);
    EXPECT_EQ(report.paths[0].delay.size(), 2u);
    EXPECT_EQ(report.paths[0].check, "setup");
}