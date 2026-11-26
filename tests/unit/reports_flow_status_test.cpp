// =============================================================================
//  tests/unit/reports_flow_status_test.cpp —— *_flow.status 解析
// =============================================================================

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "reports/flow_status.h"
#include "reports/report_text.h"

namespace {

using fpgalsp::reports::FlowStatusReport;
using fpgalsp::reports::ParseFlowStatusText;
using fpgalsp::reports::ReadReportFile;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

}  // namespace

// phy 阶段：WNS/TNS 单位是 ps（整数），跟 timing 报告的 ns（浮点）不是一回事。
TEST(ReportsFlowStatus, ParsesPhyRouteStatus) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report =
        ParseFlowStatusText(ReadReportFile(Fixture("eg4_route_flow.status")));

    ASSERT_TRUE(report.setup.has_value());
    EXPECT_EQ(report.setup->wns_ps, 171);
    EXPECT_EQ(report.setup->tns_ps, 0);
    EXPECT_EQ(report.setup->feps, 0);

    ASSERT_TRUE(report.hold.has_value());
    EXPECT_EQ(report.hold->wns_ps, 23);

    ASSERT_TRUE(report.mode.has_value());
    EXPECT_EQ(*report.mode, "final");
}

// 负数 WNS 必须能解析（hold 违例时 TD 会打负值）
TEST(ReportsFlowStatus, ParsesNegativeWns) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const std::string text =
        "Timing Mode      : estimated\n"
        "Setup            : WNS  -1284ps  TNS    -9104ps  NUM_FEPS    47\n"
        "Hold             : WNS     -3ps  TNS       -6ps  NUM_FEPS     2\n";
    const FlowStatusReport report = ParseFlowStatusText(text);
    ASSERT_TRUE(report.setup.has_value());
    EXPECT_EQ(report.setup->wns_ps, -1284);
    EXPECT_EQ(report.setup->tns_ps, -9104);
    EXPECT_EQ(report.setup->feps, 47);
    ASSERT_TRUE(report.hold.has_value());
    EXPECT_EQ(report.hold->wns_ps, -3);
    EXPECT_EQ(report.hold->feps, 2);
    ASSERT_TRUE(report.mode.has_value());
    EXPECT_EQ(*report.mode, "estimated");
}

TEST(ReportsFlowStatus, ParsesFlowStepTable) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report =
        ParseFlowStatusText(ReadReportFile(Fixture("eg4_route_flow.status")));

    ASSERT_FALSE(report.steps.empty());
    const auto& first = report.steps.front();
    EXPECT_EQ(first.name, "route");
    EXPECT_EQ(first.calls, 1);
    EXPECT_EQ(first.wall, "48s(68%)");
    EXPECT_EQ(first.cpu, "31(73%)");
    EXPECT_EQ(first.mem_mb, 977);
    EXPECT_EQ(first.peak_mb, 977);

    // "total" 行名字是小写字母，也会被当成一条 step（Python 行为）
    bool saw_total = false;
    for (const auto& step : report.steps) {
        if (step.name == "total") {
            saw_total = true;
            EXPECT_EQ(step.calls, 8);
            EXPECT_EQ(step.peak_mb, 977);
        }
    }
    EXPECT_TRUE(saw_total);

    // 表头行 / 分隔线不能被当成 step
    for (const auto& step : report.steps) {
        EXPECT_NE(step.name, "command");
    }
}

// "_flow.status" 的文件名决定步骤归属哪个阶段；单文件解析时不打标签，
// 由聚合层（BuildReports）回填，避免同一个函数有两套语义。
TEST(ReportsFlowStatus, StageIsEmptyUntilAggregated) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report =
        ParseFlowStatusText(ReadReportFile(Fixture("ef2_syn_flow.status")));
    ASSERT_FALSE(report.steps.empty());
    for (const auto& step : report.steps) EXPECT_TRUE(step.stage.empty());

    const auto json = report.ToJson();
    EXPECT_FALSE(json["steps"][0].contains("stage"));
}

TEST(ReportsFlowStatus, Ef2SynthesisStatus) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report =
        ParseFlowStatusText(ReadReportFile(Fixture("ef2_syn_flow.status")));
    ASSERT_TRUE(report.setup.has_value());
    // EF2 综合阶段没收敛：Setup WNS 为负
    EXPECT_LT(report.setup->wns_ps, 0);
    ASSERT_TRUE(report.mode.has_value());
    ASSERT_TRUE(report.hold.has_value());
    EXPECT_GE(report.steps.size(), 4u);
}

TEST(ReportsFlowStatus, EmptyInput) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report = ParseFlowStatusText("");
    EXPECT_FALSE(report.setup.has_value());
    EXPECT_FALSE(report.hold.has_value());
    EXPECT_FALSE(report.mode.has_value());
    EXPECT_TRUE(report.steps.empty());
    EXPECT_FALSE(report.warnings.empty());

    const auto json = report.ToJson();
    EXPECT_TRUE(json["setup"].is_null());
    EXPECT_TRUE(json["hold"].is_null());
    EXPECT_TRUE(json["mode"].is_null());
    EXPECT_TRUE(json["steps"].is_array());
}

// 只有 Timing Mode、没有 Setup/Hold 的截断文件
TEST(ReportsFlowStatus, TruncatedFile) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const FlowStatusReport report = ParseFlowStatusText(
        "Top Model        : main\n"
        "Timing Mode      : final\n"
        "Setup            : WNS   171ps  TNS");
    EXPECT_FALSE(report.setup.has_value());
    ASSERT_TRUE(report.mode.has_value());
    EXPECT_EQ(*report.mode, "final");
    EXPECT_TRUE(report.steps.empty());
    EXPECT_FALSE(report.warnings.empty());
}

TEST(ReportsFlowStatus, BinaryGarbage) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    std::string garbage(1024, '\0');
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<char>(i * 13 + 5);
    }
    const FlowStatusReport report = ParseFlowStatusText(garbage);
    EXPECT_TRUE(report.steps.empty());
    EXPECT_FALSE(report.setup.has_value());
}

TEST(ReportsFlowStatus, LooksLikeFlowStatus) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    EXPECT_TRUE(fpgalsp::reports::LooksLikeFlowStatus(ReadReportFile(
        Fixture("eg4_route_flow.status"))));
    EXPECT_FALSE(fpgalsp::reports::LooksLikeFlowStatus("not a status file at all"));
    EXPECT_FALSE(fpgalsp::reports::LooksLikeFlowStatus(""));
}