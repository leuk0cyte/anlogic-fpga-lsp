// =============================================================================
//  tests/unit/reports_area_test.cpp —— Area 报告解析（重点是跨器件族宽容性）
// =============================================================================
//  这个文件的核心目的不是"能解析"，而是"换成别的器件族也还能解析"：
//    * EF2 综合阶段打 #lut6 / Gate Statistics
//    * EF2 RTL 阶段打 gates/seq/macros 表（5 列，Python 的 <7 列规则会整表跳过）
//    * EG4/PH1 打 le/logic lut/eram，多一列 le
// =============================================================================

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "reports/area.h"
#include "reports/report_text.h"

namespace {

using fpgalsp::reports::AreaReport;
using fpgalsp::reports::AreaTotal;
using fpgalsp::reports::NormAreaResource;
using fpgalsp::reports::ParseArea;
using fpgalsp::reports::ReadReportFile;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

const fpgalsp::reports::AreaModule* FindModule(const AreaReport& report,
                                               const std::string& name) {
    for (const auto& module : report.modules) {
        if (module.name == name) return &module;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// 资源名归一化 / 别名
// -----------------------------------------------------------------------------

TEST(ReportsArea, NormResourceFoldsPunctuation) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    EXPECT_EQ(NormAreaResource("#lut"), "lut");
    EXPECT_EQ(NormAreaResource("logic lut"), "logic_lut");
    EXPECT_EQ(NormAreaResource("  LUT6 "), "lut6");
    EXPECT_EQ(NormAreaResource("#lut only"), "lut_only");
    EXPECT_EQ(NormAreaResource("slice_reg"), "slice_reg");
    EXPECT_EQ(NormAreaResource(""), "");
    EXPECT_EQ(NormAreaResource("---"), "");
}

TEST(ReportsArea, AreaTotalUsesAliases) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const std::map<std::string, int> totals = {
        {"lut", 19600}, {"reg", 19600}, {"bram", 64}, {"dsp", 29}, {"io", 188}};
    // lut / logic_lut / ripple 都指向 lut（PH1 打 logic lut，EF2 打 lut）
    EXPECT_EQ(AreaTotal(totals, "lut"), std::optional<int>(19600));
    EXPECT_EQ(AreaTotal(totals, "logic_lut"), std::optional<int>(19600));
    EXPECT_EQ(AreaTotal(totals, "ripple"), std::optional<int>(19600));
    EXPECT_EQ(AreaTotal(totals, "seq"), std::optional<int>(19600));
    EXPECT_EQ(AreaTotal(totals, "bram"), std::optional<int>(64));
    EXPECT_EQ(AreaTotal(totals, "dsp"), std::optional<int>(29));
    // 没有别名且不在表里 -> nullopt（不是 0）
    EXPECT_EQ(AreaTotal(totals, "gates"), std::nullopt);
}

TEST(ReportsArea, AreaTotalTreatsZeroAsMissing) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    // 复现 Python 的 `if totals.get(key):` —— 0 也算"没有"
    const std::map<std::string, int> totals = {{"slice", 0}, {"lut", 4480}};
    EXPECT_EQ(AreaTotal(totals, "lut"), std::optional<int>(4480));
}

// -----------------------------------------------------------------------------
// EF2（#lut6 那一族）
// -----------------------------------------------------------------------------

TEST(ReportsArea, Ef2UtilizationRows) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("ef2_gate.area")));

    ASSERT_FALSE(report.rows.empty());
    bool saw_lut = false;
    for (const auto& row : report.rows) {
        if (row.resource == "#lut") {
            saw_lut = true;
            EXPECT_EQ(row.used, 1064);
            EXPECT_EQ(row.total, 4480);
            EXPECT_EQ(row.utilization, "23.75%");
        }
        // 关键：不能假设固定资源名。EF2 有 bram128k/bram256k，PH1 没有。
        if (row.resource == "#bram128k") {
            EXPECT_EQ(row.used, 0);
            EXPECT_EQ(row.total, 1);
        }
    }
    EXPECT_TRUE(saw_lut);
    EXPECT_EQ(report.totals.at("lut"), 4480);
    EXPECT_EQ(report.totals.at("bram128k"), 1);
}

TEST(ReportsArea, Ef2IoCounts) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("ef2_gate.area")));
    EXPECT_EQ(report.io.at("input"), 26);
    EXPECT_EQ(report.io.at("output"), 21);
    EXPECT_EQ(report.io.at("inout"), 0);
}

// LUT Statistics 段的 #Total_luts / #lut6 只在段名以 LUT 开头时采集
TEST(ReportsArea, Ef2LutStatisticsCounts) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("ef2_gate.area")));
    ASSERT_FALSE(report.counts.empty());
    EXPECT_EQ(report.counts.at("Total_luts"), 678);
    EXPECT_EQ(report.counts.at("lut4"), 529);
    EXPECT_EQ(report.counts.at("lut5"), 134);
    // #lut6 是 EF2 综合阶段特有的资源名；解析器必须原样收下（0 也要收）
    ASSERT_TRUE(report.counts.count("lut6") == 1);
    EXPECT_EQ(report.counts.at("lut6"), 0);
}

TEST(ReportsArea, Ef2HierarchyModules) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("ef2_gate.area")));
    ASSERT_FALSE(report.modules.empty());

    const auto* top = FindModule(report, "top");
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->module, "top");
    EXPECT_EQ(top->level, 0);
    EXPECT_EQ(top->path, "top");
    EXPECT_EQ(top->resources.at("lut"), 871);
    EXPECT_EQ(top->resources.at("ripple"), 193);
    EXPECT_EQ(top->resources.at("seq"), 914);

    // 缩进 2 空格 = level 1，路径 = top/<name>
    const auto* child = FindModule(report, "trans");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->level, 1);
    EXPECT_EQ(child->path, "top/trans");

    // 缩进 4 空格 = level 2
    const auto* grand = FindModule(report, "rx_crc_inst");
    ASSERT_NE(grand, nullptr);
    EXPECT_EQ(grand->level, 2);
    EXPECT_EQ(grand->path, "top/trans/rx_crc_inst");
    EXPECT_EQ(grand->module, "crc_8_8_15");
}

// EF2 RTL 阶段的表只有 5 列（gates/seq/macros），Python 的 "< 7 列就跳过" 规则
// 会把整张表丢掉；而且这一阶段连 Utilization 段都没有（只有 Gate Statistics），
// 所以 rows 也是空的。这里钉住该行为，若将来放宽列数限制，这条会变红（P-10）。
TEST(ReportsArea, Ef2RtlHierarchyTableIsSkipped) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("ef2_rtl.area")));
    EXPECT_TRUE(report.rows.empty()) << "RTL 阶段没有 Utilization 段";
    EXPECT_TRUE(report.totals.empty());
    EXPECT_TRUE(report.modules.empty()) << "5 列的 gates 表应被 <7 列规则跳过（Python 行为）";
    // IO 计数仍然能拿到（#input/#output 走的是独立的 IO_COUNT_RE）
    EXPECT_EQ(report.io.at("input"), 17);
    EXPECT_EQ(report.io.at("output"), 15);
}

// -----------------------------------------------------------------------------
// EG4 / PH1（le / logic lut 那一族）
// -----------------------------------------------------------------------------

TEST(ReportsArea, Eg4UtilizationRows) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("eg4_phy.area")));
    ASSERT_FALSE(report.rows.empty());
    EXPECT_EQ(report.totals.at("lut"), 19600);
    EXPECT_EQ(report.totals.at("bram"), 64);
    EXPECT_EQ(report.totals.at("bram32k"), 16);
    // "#lut only" 里的空格要归一化成 lut_only，且 total 是 "#le 10122"
    EXPECT_EQ(report.totals.at("lut_only"), 10122);
    // #le 走的是 LE_RE，进 io 而不是 totals
    EXPECT_EQ(report.io.at("le"), 10122);
    EXPECT_EQ(report.io.at("input"), 44);
    EXPECT_EQ(report.io.at("inout"), 16);
}

TEST(ReportsArea, Eg4HierarchyHasExtraLeColumn) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("eg4_phy.area")));
    const auto* top = FindModule(report, "top");
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->resources.at("le"), 10122);
    EXPECT_EQ(top->resources.at("lut"), 7406);
    EXPECT_EQ(top->resources.at("seq"), 7659);

    // lut 的百分比用 #lut 的总数（19600）
    ASSERT_TRUE(top->utilization.count("lut") == 1);
    ASSERT_TRUE(top->utilization.at("lut").has_value());
    EXPECT_DOUBLE_EQ(*top->utilization.at("lut"),
                     fpgalsp::reports::Round2(7406.0 * 100.0 / 19600.0));

    // ⚠ PYTHON-BUGS P-11：`#le` 走的是 LE_RE，只写进 io 而**不**写进 totals，
    // 于是 _area_total(totals,"le") 找不到总数 -> le 的百分比永远是 null。
    // 忠实复现，不顺手修。
    ASSERT_TRUE(top->utilization.count("le") == 1);
    EXPECT_FALSE(top->utilization.at("le").has_value())
        << "PH1 的 le 百分比因 Python 把 #le 记进 io 而不是 totals 而恒为 null";
}

// 层次表里 Module 列为空（CWC / 黑盒实例）时必须保留空串，且不影响 level 计算。
TEST(ReportsArea, Eg4EmptyModuleCellIsPreserved) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea(ReadReportFile(Fixture("eg4_phy.area")));
    const auto* wrapper = FindModule(report, "wrapper_cwc_top");
    ASSERT_NE(wrapper, nullptr);
    EXPECT_EQ(wrapper->module, "");
    // 缩进 4 空格 -> level 2，路径是 top/auto_created_cwc1/wrapper_cwc_top
    EXPECT_EQ(wrapper->level, 2);
    EXPECT_EQ(wrapper->path, "top/auto_created_cwc1/wrapper_cwc_top");
}

// 资源名不在别名表里时，utilization 必须是 null 而不是 0 或 100。
TEST(ReportsArea, UnknownResourceGetsNullUtilization) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const std::string text =
        "Utilization Statistics\n"
        "#foo                      3   out of      0    0.00%\n"
        "Report Hierarchy Area:\n"
        "+-----------------------------------------+\n"
        "|Instance |Module |foo  |lut  |ripple |seq  |bram |dsp |\n"
        "+-----------------------------------------+\n"
        "|top      |top    |3    |1    |0      |0    |0    |0   |\n"
        "+-----------------------------------------+\n";
    const AreaReport report = ParseArea(text);
    ASSERT_EQ(report.modules.size(), 1u);
    // totals["foo"] 是 0 -> 当作没有 -> null
    EXPECT_FALSE(report.modules[0].utilization.at("foo").has_value());
    // lut 也没有总量 -> null
    EXPECT_FALSE(report.modules[0].utilization.at("lut").has_value());
}

// -----------------------------------------------------------------------------
// never-throw
// -----------------------------------------------------------------------------

TEST(ReportsArea, EmptyInput) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const AreaReport report = ParseArea("");
    EXPECT_TRUE(report.rows.empty());
    EXPECT_TRUE(report.modules.empty());
    EXPECT_TRUE(report.io.empty());
    EXPECT_FALSE(report.warnings.empty());

    const auto json = report.ToJson();
    EXPECT_TRUE(json["rows"].is_array());
    EXPECT_TRUE(json["modules"].is_array());
    EXPECT_EQ(json["raw"], "");
}

TEST(ReportsArea, TruncatedHierarchyTable) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    // 表头后面直接断掉（真实场景：TD 被 Ctrl-C 打断）
    const std::string text =
        "IO Statistics\n"
        "#IO                        47   out of    109   43.12%\n"
        "Utilization Statistics\n"
        "#lut                     1064   out of   4480   23.75%\n"
        "Report Hierarchy Area:\n"
        "+----------------------------------+\n"
        "|Instance |Module |lut |ripple |seq |bram |dsp |\n";
    const AreaReport report = ParseArea(text);
    EXPECT_EQ(report.totals.at("lut"), 4480);
    EXPECT_FALSE(report.rows.empty());
    EXPECT_TRUE(report.modules.empty());
}

TEST(ReportsArea, BinaryGarbage) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    std::string garbage(2048, '\0');
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<char>(i * 7 + 3);
    }
    const AreaReport report = ParseArea(garbage);
    EXPECT_TRUE(report.rows.empty());
    EXPECT_TRUE(report.modules.empty());
}

// 表头/数据行的空格对齐不能靠固定列宽：列宽随实例名长度变化。
TEST(ReportsArea, AlignmentIsNotFixedWidth) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const std::string text =
        "Utilization Statistics\n"
        "#lut                     100   out of   1000   10.00%\n"
        "Report Hierarchy Area:\n"
        "+-----------------------+\n"
        "|Instance|Module|lut|ripple|seq|bram|dsp|\n"
        "+-----------------------+\n"
        "|top|top|100|0|0|0|0|\n"
        "|  averyveryverylonginstancename|m|50|0|0|0|0|\n"
        "+-----------------------+\n";
    const AreaReport report = ParseArea(text);
    ASSERT_EQ(report.modules.size(), 2u);
    EXPECT_EQ(report.modules[1].name, "averyveryverylonginstancename");
    EXPECT_EQ(report.modules[1].level, 1);
    EXPECT_EQ(report.modules[1].path, "top/averyveryverylonginstancename");
    ASSERT_TRUE(report.modules[0].utilization.at("lut").has_value());
    EXPECT_DOUBLE_EQ(*report.modules[0].utilization.at("lut"), 10.0);
}

// 同一资源在 Utilization 段重复出现时，totals 保留最后一次（Python dict 覆盖语义）
TEST(ReportsArea, DuplicateUtilizationRowsKeepLastTotal) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    const std::string text =
        "Utilization Statistics\n"
        "#lut                     100   out of   1000   10.00%\n"
        "#lut                     200   out of   2000   10.00%\n";
    const AreaReport report = ParseArea(text);
    EXPECT_EQ(report.rows.size(), 2u);
    EXPECT_EQ(report.totals.at("lut"), 2000);
}

}  // namespace
