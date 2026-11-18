// =============================================================================
//  tests/unit/reports_parity_test.cpp —— 与 Python 参考实现逐字段对比的导出钩子
// =============================================================================
//  这个 target 有两个作用：
//
//  1) 普通 ctest 运行时：验证 tests/fixtures/reports/ 里的夹具齐全、每个解析器
//     都能在真实报告上跑出非空结果、并且对畸形输入不崩。
//  2) 被 tests/compare_reports.py 调用时（设置 FPGALSP_REPORTS_DUMP=<dir>）：
//     把每个夹具文件的 C++ 解析结果写成 JSON，供 Python 侧做逐字段 diff。
//
//  为什么把导出钩子放在测试里而不是 src/tools/：写作用域只到 src/reports/ 与
//  tests/unit/，而"能导出 JSON 的开关"本身就是一个需要长期保留的回归工具。
// =============================================================================

//  getenv 在 MSVC 上是 C4996 弃用项；这里读的是测试自己设的环境变量，风险为零。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "reports/area.h"
#include "reports/artifacts.h"
#include "reports/flow_status.h"
#include "reports/qor.h"
#include "reports/report_text.h"
#include "reports/reports_aggregate.h"
#include "reports/timing.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::jsonrpc::Json;

std::string FixtureDir() { return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports"; }

std::vector<std::string> ListFixtures(const std::string& suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(fs::path(FixtureDir()), ec)) return out;
    for (const auto& entry : fs::directory_iterator(fs::path(FixtureDir()), ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Stem(const std::string& path) {
    const std::string name = fs::path(path).filename().string();
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void DumpJson(const std::string& dir, const std::string& name, const Json& value) {
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    std::ofstream out(fs::path(dir) / (name + ".json"), std::ios::binary);
    if (!out) return;
    out << fpgalsp::jsonrpc::Serialize(value, /*compact=*/false);
}

}  // namespace

// -----------------------------------------------------------------------------
// 夹具与解析器可用性自检（不需要环境变量）
// -----------------------------------------------------------------------------

TEST(ReportsParity, FixtureCorpusIsComplete) {
    // 两个器件族都必须有真实报告：只用一族做夹具，PH1/EF2 的资源名差异永远测不出来。
    EXPECT_FALSE(ListFixtures(".qor").empty());
    EXPECT_FALSE(ListFixtures(".area").empty());
    EXPECT_FALSE(ListFixtures(".status").empty());
    EXPECT_FALSE(ListFixtures(".timing").empty());
    EXPECT_FALSE(ListFixtures(".rpt").empty());

    int family_markers = 0;
    for (const std::string& path : ListFixtures(".area")) {
        const std::string text = fpgalsp::reports::ReadReportFile(path);
        if (text.find("EF2L45LG144B") != std::string::npos) ++family_markers;
        if (text.find("EG4X20BG256") != std::string::npos) ++family_markers;
    }
    EXPECT_GE(family_markers, 2) << "area 夹具必须同时覆盖 EF2 与 EG4 两个器件族";
}

TEST(ReportsParity, EveryPatternCompiles) {
    std::string error;
    EXPECT_TRUE(fpgalsp::reports::AllReportPatternsValid(&error)) << error;
}

// 每个夹具都必须能解析（不崩、输出合法 JSON）；至于"是否解析出了内容"取决于报告本身：
// ef2_rtl.area 没有 Utilization 段、*_chk.timing / *_exception.timing 没有 Clock Summary，
// 这些都只有部分字段。所以这里只要求"整批夹具里有内容的占多数"。
TEST(ReportsParity, EveryFixtureParsesWithoutThrowing) {
    using namespace fpgalsp::reports;

    int qor_with_metrics = 0;
    for (const std::string& path : ListFixtures(".qor")) {
        const QorReport parsed = ParseQor(ReadReportFile(path), "step");
        if (!parsed.metrics.empty()) ++qor_with_metrics;
        EXPECT_TRUE(parsed.ToJson()["metrics"].is_array()) << path;
    }
    EXPECT_EQ(qor_with_metrics, static_cast<int>(ListFixtures(".qor").size()));

    int area_with_rows = 0;
    for (const std::string& path : ListFixtures(".area")) {
        const AreaReport parsed = ParseArea(ReadReportFile(path));
        if (!parsed.rows.empty()) ++area_with_rows;
        EXPECT_TRUE(parsed.ToJson()["rows"].is_array()) << path;
        // 无论解析到什么，io 里的计数都必须自洽（input/output/inout 非负）
        for (const auto& [key, value] : parsed.io) EXPECT_GE(value, 0) << path << ":" << key;
    }
    EXPECT_GE(area_with_rows, 3) << "至少三份 .area 夹具应解析出 Utilization 行";

    int status_with_steps = 0;
    for (const std::string& path : ListFixtures(".status")) {
        const FlowStatusReport parsed = ParseFlowStatusText(ReadReportFile(path));
        if (!parsed.steps.empty()) ++status_with_steps;
        EXPECT_TRUE(parsed.setup.has_value()) << path;
    }
    EXPECT_EQ(status_with_steps, static_cast<int>(ListFixtures(".status").size()));

    int timing_with_clocks = 0;
    int timing_with_paths = 0;
    for (const std::string& path : ListFixtures(".timing")) {
        const TimingReport parsed = ParseTiming(ReadReportFile(path), path);
        if (!parsed.clocks.empty()) ++timing_with_clocks;
        if (!parsed.paths.empty()) ++timing_with_paths;
        EXPECT_TRUE(parsed.ToJson()["groups"].is_array()) << path;
    }
    EXPECT_GE(timing_with_clocks, 2) << "chk/exception 两类报告没有 Clock Summary";
    EXPECT_GE(timing_with_paths, 3);
}

// -----------------------------------------------------------------------------
// JSON 导出（由 tests/compare_reports.py 驱动）
// -----------------------------------------------------------------------------

TEST(ReportsParity, DumpJsonWhenRequested) {
    const char* dump_dir = std::getenv("FPGALSP_REPORTS_DUMP");
    if (dump_dir == nullptr || *dump_dir == '\0') {
        GTEST_SKIP() << "FPGALSP_REPORTS_DUMP 未设置（正常的 ctest 运行不需要导出）";
    }
    using namespace fpgalsp::reports;
    const std::string dir(dump_dir);

    for (const std::string& path : ListFixtures(".qor")) {
        DumpJson(dir, "qor_" + Stem(path),
                 ParseQor(ReadReportFile(path), Stem(path)).ToJson());
    }
    for (const std::string& path : ListFixtures(".area")) {
        DumpJson(dir, "area_" + Stem(path), ParseArea(ReadReportFile(path)).ToJson());
    }
    for (const std::string& path : ListFixtures(".status")) {
        DumpJson(dir, "flow_" + Stem(path), ParseFlowStatusText(ReadReportFile(path)).ToJson());
    }
    for (const std::string& path : ListFixtures(".timing")) {
        DumpJson(dir, "timing_" + Stem(path),
                 ParseTiming(ReadReportFile(path), path).ToJson());
    }
    for (const std::string& path : ListFixtures(".rpt")) {
        DumpJson(dir, "timing_" + Stem(path),
                 ParseTiming(ReadReportFile(path), path).ToJson());
    }

    // 汇总：由 compare_reports.py 造好一棵 <name>_Runs 树后，用这两个变量告诉我们
    // 工程目录与工程名（与 Python 侧伪造的 project 对象同构：只用到 .dir / .name）。
    const char* project_dir = std::getenv("FPGALSP_REPORTS_PROJECT_DIR");
    const char* project_name = std::getenv("FPGALSP_REPORTS_PROJECT_NAME");
    if (project_dir != nullptr && *project_dir != '\0') {
        fpgalsp::project::Project project;
        project.dir = project_dir;
        project.name = project_name == nullptr ? std::string() : std::string(project_name);
        DumpJson(dir, "aggregate", BuildReports(project).ToJson());
    }
    SUCCEED();
}