// =============================================================================
//  tests/unit/analysis_test.cpp —— B5：DRC/CDC 分析 + updateProject
// =============================================================================
//  覆盖（任务书 §4）：
//    * 无 routed db      -> Run() 立刻返回可操作的错误，且**不启动 TD**、不写缓存
//    * DRC 解析          -> ParseDrc：summary 表 / details 段 / clean / 200 条上限
//    * 不支持 CDC        -> CdcUnsupportedByDevice + reason 文案（analysis.py:105-107）
//    * updateProject 失败 -> %TEMP% 副本（文件名与 <Name> 不一致 -> PRJ-8643），
//                            断言 ok=false 且**保留 TD 输出**；没有 TD 时 GTEST_SKIP
//  另外覆盖纯函数：NewestRoutedDb（mtime 选择 / 目录缺失）、FirstError、TailLines。
//
//  ⚠ 纪律：真实 TD 只在 %TEMP% 副本上跑（P-4/P-7b）。本文件的 integration 用例
//     自己造一个最小 .al（Path 指向自己的临时目录），绝不碰 Y:\ 下的真实工程。
// =============================================================================

#include "analysis/analysis.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "project/al_project.h"
#include "td/td_install.h"

namespace {

namespace fs = std::filesystem;
namespace project = fpgalsp::project;
namespace analysis = fpgalsp::analysis;
namespace td = fpgalsp::td;
using Json = fpgalsp::jsonrpc::Json;

std::string Slashes(const fs::path& path) {
    std::string text = path.generic_string();
    return text;
}

std::string Slashes(const std::string& text) { return text; }

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("fpgalsp_analysis_test_" + std::to_string(stamp) + "_" +
                 std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string str() const { return Slashes(path_); }

    std::string Write(const std::string& relative, const std::string& content) const {
        const fs::path target = path_ / fs::path(relative);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream handle(target, std::ios::binary | std::ios::trunc);
        handle << content;
        handle.close();
        return Slashes(target);
    }

private:
    fs::path path_;
};

project::Project MakeProject(const std::string& dir, const std::string& name,
                             const std::string& device = "EF2L45LG144B") {
    project::Project project;
    project.dir = dir;
    project.name = name;
    project.al_path = dir + "/" + name + ".al";
    project.device = device;
    project.family = "EF2";
    return project;
}

void SetMtime(const std::string& path, std::chrono::hours offset) {
    std::error_code ec;
    fs::last_write_time(fs::path(path), fs::file_time_type::clock::now() - offset, ec);
    ASSERT_FALSE(ec) << "cannot set mtime: " << path;
}

constexpr const char* kDrcReport =
    "Report DRC Summary\n"
    "REPORT SUMMARY\n"
    "--------------------------------------------\n"
    "| USR-8026  | ERROR             | 3  |\n"
    "| USR-7311  | WARNING           | 1  |\n"
    "| PRJ-1007  | INFO              | 5  |\n"
    "--------------------------------------------\n"
    "\n"
    "REPORT DETAILS\n"
    "USR-8026 ERROR: something is wrong at (top.v:12)\n"
    "  further explanation line\n"
    "USR-7311 WARNING: mild issue at (top.v:30)\n";

constexpr const char* kCdcReport =
    "CDC report\n"
    "---------------------------------------------\n"
    "| From   | To      | Type    | Safe |\n"
    "| reg_a  | reg_b   | async   | No   |\n"
    "---------------------------------------------\n"
    "Safe: 4  Unsafe: 2  Unknown: 1  No ASYNC_REG: 3\n";

}  // namespace

// -----------------------------------------------------------------------------
// NewestRoutedDb
// -----------------------------------------------------------------------------

TEST(Analysis, NewestRoutedDbPicksLatestMtime) {
    TempDir temp;
    const std::string phy = temp.str() + "/demo_Runs/phy_1";
    const std::string old_db = temp.Write("demo_Runs/phy_1/old_pr.db", "old");
    const std::string new_db = temp.Write("demo_Runs/phy_1/new_pr.db", "new");
    const std::string route_db = temp.Write("demo_Runs/phy_1/x_route.db", "route");
    // 非 db 文件必须被忽略
    temp.Write("demo_Runs/phy_1/readme.txt", "nope");
    SetMtime(old_db, std::chrono::hours(5));
    SetMtime(route_db, std::chrono::hours(2));
    SetMtime(new_db, std::chrono::hours(1));
    ASSERT_TRUE(fs::is_directory(fs::path(phy)));

    // 前置条件：new_pr.db 必须真的比 route 新（否则测的是目录顺序，不是 mtime 选择）
    std::error_code mtime_ec;
    const auto mtime_new = fs::last_write_time(fs::path(new_db), mtime_ec);
    ASSERT_FALSE(mtime_ec);
    const auto mtime_route = fs::last_write_time(fs::path(route_db), mtime_ec);
    ASSERT_FALSE(mtime_ec);
    ASSERT_TRUE(mtime_route < mtime_new) << "mtime precondition failed";

    const std::string newest = analysis::NewestRoutedDb(MakeProject(temp.str(), "demo"));
    EXPECT_EQ(newest, new_db);
}

TEST(Analysis, NewestRoutedDbMissingDirectoryIsEmpty) {
    TempDir temp;
    EXPECT_TRUE(analysis::NewestRoutedDb(MakeProject(temp.str(), "demo")).empty());
    // .al 目录本身都不存在时也不能抛
    EXPECT_TRUE(analysis::NewestRoutedDb(MakeProject(temp.str() + "/nope", "demo")).empty());
}

// -----------------------------------------------------------------------------
// ParseDrc / ParseCdc
// -----------------------------------------------------------------------------

TEST(Analysis, ParseDrcSummaryDetailsAndCleanFlag) {
    const Json drc = analysis::ParseDrc(kDrcReport);
    ASSERT_TRUE(drc.is_object());
    EXPECT_EQ(drc.value("violations", -1), 9);  // 3 + 1 + 5
    ASSERT_TRUE(drc.contains("rules"));
    ASSERT_EQ(drc["rules"].size(), 3u);
    EXPECT_EQ(drc["rules"][0].value("rule", ""), "USR-8026");
    EXPECT_EQ(drc["rules"][0].value("severity", ""), "ERROR");
    EXPECT_EQ(drc["rules"][0].value("count", 0), 3);
    EXPECT_EQ(drc["rules"][1].value("severity", ""), "WARNING");

    ASSERT_TRUE(drc.contains("details"));
    // 第一条明细：匹配规则号；第二条：不匹配但 details 已非空 -> 仍收
    ASSERT_EQ(drc["details"].size(), 3u);
    EXPECT_NE(drc["details"][0].get<std::string>().find("USR-8026 ERROR"), std::string::npos);
    EXPECT_NE(drc["details"][1].get<std::string>().find("further explanation"),
              std::string::npos);
    EXPECT_FALSE(drc.value("clean", true));
}

TEST(Analysis, ParseDrcCleanReportHasZeroViolations) {
    const Json drc = analysis::ParseDrc(
        "REPORT SUMMARY\n"
        "--------------------------------------------\n"
        "REPORT DETAILS\n");
    EXPECT_EQ(drc.value("violations", -1), 0);
    EXPECT_TRUE(drc.value("clean", false));
    EXPECT_TRUE(drc["rules"].empty());
    EXPECT_TRUE(drc["details"].empty());
}

TEST(Analysis, ParseCdcRowsAndCounts) {
    const Json cdc = analysis::ParseCdc(kCdcReport);
    ASSERT_TRUE(cdc["rows"].is_array());
    // 表头 + 1 条数据行；分隔线被排除
    ASSERT_EQ(cdc["rows"].size(), 2u);
    EXPECT_EQ(cdc["rows"][0][0].get<std::string>(), "From");
    EXPECT_EQ(cdc["rows"][1][0].get<std::string>(), "reg_a");
    EXPECT_EQ(cdc["rows"][1][3].get<std::string>(), "No");

    ASSERT_TRUE(cdc["counts"].is_object());
    EXPECT_EQ(cdc["counts"].value("safe", 0), 4);
    EXPECT_EQ(cdc["counts"].value("unsafe", 0), 2);
    EXPECT_EQ(cdc["counts"].value("unknown", 0), 1);
    // "No ASYNC_REG" -> no_async_reg（Python re.sub(r"\s+", "_", …)）
    EXPECT_EQ(cdc["counts"].value("no_async_reg", 0), 3);
    EXPECT_FALSE(cdc.value("clean", true));
}

TEST(Analysis, ParseCdcCleanWhenNoUnsafeOrUnknown) {
    const Json cdc = analysis::ParseCdc("Safe: 7\nUnsafe: 0\n");
    EXPECT_EQ(cdc["counts"].value("safe", 0), 7);
    EXPECT_TRUE(cdc.value("clean", false));
    EXPECT_TRUE(cdc["rows"].empty());
}

TEST(Analysis, CdcUnsupportedDeviceContract) {
    EXPECT_TRUE(analysis::CdcUnsupportedByDevice(
        "USR-8001 ERROR: The current device does not support generating check_cdc reports"));
    EXPECT_FALSE(analysis::CdcUnsupportedByDevice("CDC report\nSafe: 3\n"));
    EXPECT_EQ(analysis::CdcUnsupportedReason("EF2L45LG144B"),
              "Device (EF2L45LG144B) does not support check_cdc reports");
}

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

TEST(Analysis, FirstErrorPicksFirstErrorLine) {
    EXPECT_EQ(analysis::FirstError("hello\n  PRJ-1234 ERROR: boom  \nworld"),
              "PRJ-1234 ERROR: boom");
    EXPECT_EQ(analysis::FirstError("X-1 CRITICAL-WARNING: careful"), "X-1 CRITICAL-WARNING: careful");
    EXPECT_TRUE(analysis::FirstError("nothing here").empty());
}

TEST(Analysis, TailLinesKeepsLastLinesAndBlankLines) {
    std::string text;
    for (int index = 1; index <= 40; ++index) text += "line" + std::to_string(index) + "\n";
    const std::string tail = analysis::TailLines(text, 30);
    const std::vector<std::string> lines = [&tail] {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (true) {
            const std::size_t pos = tail.find('\n', start);
            if (pos == std::string::npos) {
                out.push_back(tail.substr(start));
                break;
            }
            out.push_back(tail.substr(start, pos - start));
            start = pos + 1;
        }
        return out;
    }();
    ASSERT_EQ(lines.size(), 30u);
    EXPECT_EQ(lines.front(), "line11");
    EXPECT_EQ(lines.back(), "line40");
    // 结尾换行不产生多余空串（Python splitlines 语义）
    EXPECT_TRUE(analysis::TailLines("a\n\n\nRUN-1002\n", 30) == "a\n\n\nRUN-1002");
}

// -----------------------------------------------------------------------------
// Run：无 routed db（不启动 TD，不写缓存）
// -----------------------------------------------------------------------------

TEST(Analysis, RunWithoutRoutedDbReportsActionableError) {
    TempDir temp;
    const project::Project project = MakeProject(temp.str(), "demo");
    const Json result = analysis::Run(project);

    ASSERT_TRUE(result.is_object());
    EXPECT_FALSE(result.value("ok", true));
    EXPECT_EQ(result.value("error", std::string()),
              "No placed/routed result yet (run Place & Route / Full Flow first)");
    EXPECT_EQ(result.value("device", std::string()), "EF2L45LG144B");
    EXPECT_TRUE(result["generatedAt"].is_number());
    EXPECT_EQ(result["drc"].value("supported", false), true);
    EXPECT_EQ(result["drc"].value("violations", -1), 0);
    EXPECT_TRUE(result["drc"]["rules"].empty());
    EXPECT_TRUE(result["drc"]["details"].empty());
    EXPECT_EQ(result["cdc"].value("supported", false), true);
    EXPECT_TRUE(result["cdc"]["rows"].empty());
    EXPECT_TRUE(result["cdc"]["counts"].empty());
    EXPECT_TRUE(result["files"].empty());
    // 没有布线结果时 Python 也不写缓存
    EXPECT_FALSE(fs::exists(fs::path(temp.str() + "/.fpgalsp/cache/analysis.json")));
}

// -----------------------------------------------------------------------------
// updateProject：真实 TD 的失败路径（只在 %TEMP% 副本上）
// -----------------------------------------------------------------------------

TEST(Analysis, UpdateProjectFailureKeepsTdOutput) {
    if (!td::FindTd().has_value()) {
        GTEST_SKIP() << "no TangDynasty installation found";
    }
    TempDir temp;
    // 最小 .al：Path 指向自己所在的临时目录（不触碰真实工程），
    // <Name> 与文件名不一致 -> TD 报 PRJ-8643（Python 探针里同样的失败）
    const std::string al = temp.Write(
        "wrong_name.al",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project Version=\"3\" Minor=\"2\" Path=\"" + temp.str() + "\">\n"
        "    <Project_Created_Time></Project_Created_Time>\n"
        "    <TD_Version>6.2.3.210850</TD_Version>\n"
        "    <Name>totally_different_name</Name>\n"
        "    <HardWare>\n"
        "        <Family>EF2</Family>\n"
        "        <Device>EF2L45LG144B</Device>\n"
        "        <Speed></Speed>\n"
        "    </HardWare>\n"
        "    <Source_Files>\n"
        "    </Source_Files>\n"
        "</Project>\n");

    const project::Project project = project::ParseAl(al);
    ASSERT_EQ(project.al_path.substr(project.al_path.size() - 13), "wrong_name.al");

    const Json result = analysis::UpdateProject(project);
    ASSERT_TRUE(result.is_object());
    EXPECT_FALSE(result.value("ok", true));
    ASSERT_TRUE(result.contains("output"));
    const std::string output = result.value("output", std::string());
    EXPECT_FALSE(output.empty());                       // 失败也保留 TD 输出
    EXPECT_NE(output.find("check_project_version"), std::string::npos);
    // 文件名校验失败是 TD 的既有行为（与 Python 探针一致）
    EXPECT_NE(output.find("PRJ-"), std::string::npos);
    EXPECT_EQ(result.value("al", std::string()), al);
    EXPECT_TRUE(fs::exists(fs::path(al)));              // 原文件仍在（失败不删文件）
}
