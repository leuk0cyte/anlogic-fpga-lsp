// =============================================================================
//  tests/unit/reports_qor_test.cpp —— QoR 报告解析
// =============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "platform/encoding.h"
#include "reports/qor.h"
#include "reports/report_text.h"

namespace {

using fpgalsp::reports::ParseQor;
using fpgalsp::reports::ParseSuggestionsTcl;
using fpgalsp::reports::QorReport;
using fpgalsp::reports::ReadReportFile;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

const fpgalsp::reports::QorMetric* FindMetric(const QorReport& report, const std::string& key) {
    for (const auto& metric : report.metrics) {
        if (metric.key == key) return &metric;
    }
    return nullptr;
}

// 管道表的分组标题：value 为空的那一行是组名，后续行归入该组。
TEST(ReportsQor, GroupsMetricsUnderEmptyValueHeaders) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("eg4_route.qor")), "route");

    ASSERT_FALSE(report.metrics.empty());
    const auto* setup_wns = FindMetric(report, "Setup WNS");
    ASSERT_NE(setup_wns, nullptr);
    EXPECT_EQ(setup_wns->group, "Timing");
    EXPECT_EQ(setup_wns->value, "171");

    const auto* slices = FindMetric(report, "#slices");
    ASSERT_NE(slices, nullptr);
    EXPECT_EQ(slices->group, "Utilization");
    EXPECT_EQ(slices->value, "6029");

    const auto* percent = FindMetric(report, "slices percentage");
    ASSERT_NE(percent, nullptr);
    EXPECT_EQ(percent->value, "61.52%");
}

// "| QoR metrics | value |" 是表头，不能当成一条指标。
TEST(ReportsQor, SkipsHeaderRows) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("eg4_route.qor")), "route");
    EXPECT_EQ(FindMetric(report, "QoR metrics"), nullptr);
    EXPECT_EQ(FindMetric(report, "value"), nullptr);
    for (const auto& metric : report.metrics) {
        EXPECT_FALSE(metric.key.empty());
        EXPECT_FALSE(metric.value.empty());
    }
}

// 负值（综合阶段没收敛）必须原样保留，不能被当成"空"跳过。
TEST(ReportsQor, KeepsNegativeValues) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("eg4_gate.qor")), "gate");
    const auto* setup_wns = FindMetric(report, "Setup WNS");
    ASSERT_NE(setup_wns, nullptr);
    EXPECT_EQ(setup_wns->value, "-947");
    const auto* setup_tns = FindMetric(report, "Setup TNS");
    ASSERT_NE(setup_tns, nullptr);
    EXPECT_EQ(setup_tns->value, "-24474");
}

// EF2 的 gate.qor 会多出 #lut1 / #f7mux / kept_net / Rent 等分组；解析必须原样带上分组名。
TEST(ReportsQor, ToleratesFamilySpecificGroups) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("ef2_gate.qor")), "gate");
    const auto* luts = FindMetric(report, "#luts");
    ASSERT_NE(luts, nullptr);
    EXPECT_EQ(luts->group, "Utilization");
    const auto* keep = FindMetric(report, "#dont_touch");
    ASSERT_NE(keep, nullptr);
    EXPECT_EQ(keep->group, "kept_net");
    // 资源名里带大写、带括号、带空格都要照原样保留
    const auto* lut1 = FindMetric(report, "#lut1(~A)");
    ASSERT_NE(lut1, nullptr);
    EXPECT_EQ(lut1->group, "Utilization");
    const auto* fanout = FindMetric(report, "Top 1% datanet fanout#");
    ASSERT_NE(fanout, nullptr);
    EXPECT_EQ(fanout->group, "kept_net");
}

// ⚠ PYTHON-BUGS P-12：QoR 解析不区分表格。"Hierarchical Rent Exponent Report"
// 那张以 "+---+" 包边、以 "|" 起头的表，其表头行与数据行都会被当成指标收进来：
//     |Inst |Model |Rent |...  -> metric{group:"Rent**", key:"Inst",  value:"Model"}
//     |top  |top   |0.25 |...  -> metric{group:"Rent**", key:"top",   value:"top"}
// 忠实复现；若将来 reports.py 修掉，这里会变红。
TEST(ReportsQor, RentPipeTableLeaksIntoMetrics) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("ef2_gate.qor")), "gate");
    const auto* header = FindMetric(report, "Inst");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->group, "Rent**");
    EXPECT_EQ(header->value, "Model");

    const auto* top_row = FindMetric(report, "top");
    ASSERT_NE(top_row, nullptr);
    EXPECT_EQ(top_row->group, "Rent**");
    EXPECT_EQ(top_row->value, "top");
}

// 真实报告里没有建议项时必须是空表（不是"缺失"）。
TEST(ReportsQor, NoSuggestionsInCleanReports) {
    const QorReport report = ParseQor(ReadReportFile(Fixture("eg4_route.qor")), "route");
    EXPECT_TRUE(report.suggestions.empty());
}

// 手工构造：真实 TD 在某些失败收敛上会打 QOR-xxxx 建议表。
// 这段文本不是从 TD 抓的，是按 reports.py 的解析契约构造的。
TEST(ReportsQor, ParsesSuggestionTable) {
    const std::string text =
        "-----------------------------------------------------------------\n"
        "| QoR metrics               |         value                   |\n"
        "-----------------------------------------------------------------\n"
        "| Timing                    |                                 |\n"
        "|     Setup WNS             |         -2649                   |\n"
        "-----------------------------------------------------------------\n"
        "| QOR-1302                  | MEDIUM  | - Route HWNS -2649ps smaller than 0, do not meet timing. |\n"
        "|                           |         |   Try TimingHigh strategy or relax the clock.            |\n"
        "| QOR-2077                  | HIGH    | - Slice utilization 91.2% is above 90%.                  |\n"
        "-----------------------------------------------------------------\n";

    const QorReport report = ParseQor(text, "route");
    ASSERT_EQ(report.suggestions.size(), 2u);

    EXPECT_EQ(report.suggestions[0].id, "QOR-1302");
    EXPECT_EQ(report.suggestions[0].priority, "MEDIUM");
    EXPECT_NE(report.suggestions[0].text.find("Route HWNS"), std::string::npos);
    // 续行用 "\n" 连接，且续行只取最后一个单元格
    EXPECT_NE(report.suggestions[0].text.find("\nTry TimingHigh"), std::string::npos);

    EXPECT_EQ(report.suggestions[1].id, "QOR-2077");
    EXPECT_EQ(report.suggestions[1].priority, "HIGH");
    EXPECT_NE(report.suggestions[1].text.find("Slice utilization"), std::string::npos);
}

// 没有优先级列时 priority 是空串，而不是漏字段。
// 注意：至少要有 3 个单元格，否则整行被 len(cells) < 3 跳过（Python 行为）。
TEST(ReportsQor, SuggestionWithoutPriority) {
    const std::string text =
        "| QOR-5001 | - Check the false path on clk_a. | net1 |\n";
    const QorReport report = ParseQor(text, "gate");
    ASSERT_EQ(report.suggestions.size(), 1u);
    EXPECT_EQ(report.suggestions[0].id, "QOR-5001");
    EXPECT_EQ(report.suggestions[0].priority, "");
    EXPECT_EQ(report.suggestions[0].text, "- Check the false path on clk_a. net1");
}

// 只有 2 个单元格的管道行连建议候选都不算（Python 的 len(cells) < 3 规则）
TEST(ReportsQor, TwoCellRowIsNotASuggestion) {
    const std::string text = "| QOR-5001 | - Check the false path on clk_a. |\n";
    const QorReport report = ParseQor(text, "gate");
    EXPECT_TRUE(report.suggestions.empty());
}

TEST(ReportsQor, EmptyInputIsNotACrash) {
    const QorReport report = ParseQor("", "route");
    EXPECT_TRUE(report.metrics.empty());
    EXPECT_TRUE(report.suggestions.empty());
    EXPECT_EQ(report.step, "route");
    EXPECT_FALSE(report.warnings.empty());

    const auto json = report.ToJson();
    EXPECT_EQ(json["step"], "route");
    EXPECT_TRUE(json["metrics"].is_array());
    EXPECT_TRUE(json["suggestions"].is_array());
}

TEST(ReportsQor, BinaryGarbageIsNotACrash) {
    std::string garbage;
    for (int i = 0; i < 4096; ++i) garbage.push_back(static_cast<char>((i * 37) & 0xFF));
    const QorReport report = ParseQor(garbage, "gate");
    EXPECT_TRUE(report.metrics.empty());
    const auto json = report.ToJson();
    EXPECT_TRUE(json["metrics"].empty());
}

// 管道行里 key 为空（"| | value |"）必须跳过，否则会造出空 key 的指标。
TEST(ReportsQor, SkipsRowsWithEmptyKey) {
    const std::string text =
        "| Timing | |\n"
        "|   | value |\n"
        "|   Setup WNS | 12 |\n";
    const QorReport report = ParseQor(text, "route");
    ASSERT_EQ(report.metrics.size(), 1u);
    EXPECT_EQ(report.metrics[0].key, "Setup WNS");
    EXPECT_EQ(report.metrics[0].group, "Timing");
}

TEST(ReportsQor, SuggestionTclSkipsCommentsAndBlankLines) {
    const std::string path =
        std::string(::testing::TempDir()) + "/reports_qor_route_qor_suggestion.tcl";
    ASSERT_TRUE(fpgalsp::platform::write_file_utf8(
        path,
        "# TD generated suggestion\n"
        "\n"
        "set_param -name route_effort -value high\n"
        "  set_param -name max_fanout -value 64  \n"));

    const std::vector<fpgalsp::reports::QorSuggestion> suggestions = ParseSuggestionsTcl(path);
    ASSERT_EQ(suggestions.size(), 2u);
    EXPECT_EQ(suggestions[0].id, "TD-SUGGESTION");
    EXPECT_EQ(suggestions[0].text, "set_param -name route_effort -value high");
    EXPECT_EQ(suggestions[1].text, "set_param -name max_fanout -value 64");
    EXPECT_EQ(suggestions[0].file, path);

    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path(path), ignored);
}

TEST(ReportsQor, SuggestTclMissingFileIsEmpty) {
    EXPECT_TRUE(ParseSuggestionsTcl("Y:/definitely/not/here.tcl").empty());
}

}  // namespace
