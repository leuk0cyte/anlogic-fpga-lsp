// =============================================================================
//  tests/unit/reports_artifacts_test.cpp —— 产物探测（bit / db / runs / stat）
// =============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "platform/encoding.h"
#include "reports/artifacts.h"
#include "reports/report_text.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::reports::ArtifactsReport;
using fpgalsp::reports::CollectArtifacts;
using fpgalsp::reports::FirstExisting;
using fpgalsp::reports::GlobReportFiles;
using fpgalsp::reports::JoinPath;
using fpgalsp::reports::RunsDirectory;
using fpgalsp::reports::StatFile;
using fpgalsp::reports::ToForwardSlashes;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

// 每个用例一个独立的临时目录，析构时清掉
class ScopedTempDir {
  public:
    explicit ScopedTempDir(const std::string& tag) {
        path_ = Slashes(fs::temp_directory_path().string());
        if (!path_.empty() && path_.back() != '/') path_.push_back('/');
        path_ += "fpgalsp_reports_" + tag;
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
        fs::create_directories(fs::path(path_), ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::string& path() const { return path_; }

  private:
    std::string path_;
};

void MakeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
}

void Touch(const std::string& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(path, content);
}

}  // namespace

// -----------------------------------------------------------------------------
// 路径工具
// -----------------------------------------------------------------------------

// ⚠ 刻意复现 os.path.join 在 Windows 上的行为（用反斜杠拼接，见 PYTHON-BUGS P-13）。
// 想改成正斜杠前先看那条登记：改了 collection 输出的 JSON 路径字段就和 Python 对不上。
TEST(ReportsArtifacts, JoinPathFollowsOsPathJoinSemantics) {
    EXPECT_EQ(JoinPath("Y:/a/b", "c.txt"), "Y:/a/b\\c.txt");
    // dir 已经带分隔符时不再补一个
    EXPECT_EQ(JoinPath("Y:/a/b/", "c.txt"), "Y:/a/b/c.txt");
    EXPECT_EQ(JoinPath("", "c.txt"), "c.txt");
    // 混合形态就是 Python 的实际输出
    EXPECT_EQ(ToForwardSlashes(JoinPath("Y:/a/b", "c/d.txt")), "Y:/a/b/c/d.txt");
}

TEST(ReportsArtifacts, StatFileReportsSizeAndMtime) {
    ScopedTempDir temp("stat");
    const std::string path = JoinPath(temp.path(), "some.bit");
    Touch(path, "0123456789");

    const auto stat = StatFile(path);
    EXPECT_TRUE(stat.exists);
    EXPECT_EQ(stat.size, std::optional<int64_t>(10));
    ASSERT_TRUE(stat.mtime.has_value());
    // 2020-01-01 之后（TD 报告的 mtime 一定是近代时间戳）
    EXPECT_GT(*stat.mtime, 1577836800.0);

    const auto json = stat.ToJson();
    EXPECT_EQ(json["path"], path);
    EXPECT_TRUE(json["exists"]);
    EXPECT_EQ(json["size"], 10);
}

// 缺失文件必须是 {path, exists:false}，**不能**带 size/mtime 键（Python 的 _stat 行为）
TEST(ReportsArtifacts, StatFileOnMissingFileHasNoSizeKeys) {
    const auto stat = StatFile("Y:/definitely/not/here/qor.qor");
    EXPECT_FALSE(stat.exists);
    const auto json = stat.ToJson();
    EXPECT_TRUE(json.contains("path"));
    EXPECT_FALSE(json["exists"]);
    EXPECT_FALSE(json.contains("size"));
    EXPECT_FALSE(json.contains("mtime"));
}

TEST(ReportsArtifacts, StatFileIgnoresDirectories) {
    ScopedTempDir temp("statdir");
    const auto stat = StatFile(temp.path());
    EXPECT_FALSE(stat.exists) << "目录不能被当成产物文件";
}

TEST(ReportsArtifacts, FirstExistingPicksFirstHit) {
    ScopedTempDir temp("first");
    const std::string a = JoinPath(temp.path(), "a.qor");
    const std::string b = JoinPath(temp.path(), "b.qor");
    Touch(b, "x");
    EXPECT_EQ(FirstExisting({a, b}), b);
    EXPECT_EQ(FirstExisting({a}), "");
    EXPECT_EQ(FirstExisting({}), "");
}

// -----------------------------------------------------------------------------
// Glob —— 模式按**文件名**整串匹配
// -----------------------------------------------------------------------------

TEST(ReportsArtifacts, GlobMatchesFileNamesNotPaths) {
    ScopedTempDir temp("glob");
    Touch(JoinPath(temp.path(), "CorpusBeta_pr.db"), "x");
    Touch(JoinPath(temp.path(), "CorpusBeta_place.db"), "x");
    Touch(JoinPath(temp.path(), "CorpusBeta_pr.timing"), "x");

    const auto dbs = GlobReportFiles(temp.path(), R"(.*_pr\.db)");
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_NE(dbs[0].find("CorpusBeta_pr.db"), std::string::npos);

    // 模式里没有 .* 前缀时，名字前面的部分必须也能匹配——否则会漏
    const auto strict = GlobReportFiles(temp.path(), R"(_pr\.db)");
    EXPECT_TRUE(strict.empty());
}

TEST(ReportsArtifacts, GlobIsSortedAndTolerant) {
    ScopedTempDir temp("glob2");
    Touch(JoinPath(temp.path(), "b_phy.area"), "x");
    Touch(JoinPath(temp.path(), "a_phy.area"), "x");

    const auto areas = GlobReportFiles(temp.path(), R"(.*_phy\.area)");
    ASSERT_EQ(areas.size(), 2u);
    EXPECT_LT(areas[0], areas[1]);

    // 目录不存在 / 坏模式都只是空结果，不抛
    EXPECT_TRUE(GlobReportFiles(JoinPath(temp.path(), "nope"), R"(.*\.area)").empty());
    EXPECT_TRUE(GlobReportFiles(temp.path(), R"([unclosed)").empty());
}

// -----------------------------------------------------------------------------
// runs 目录
// -----------------------------------------------------------------------------

TEST(ReportsArtifacts, RunsDirectoryPrefersProjectNamedDir) {
    ScopedTempDir temp("runs1");
    MakeDirs(JoinPath(temp.path(), "proj_Runs"));
    MakeDirs(JoinPath(temp.path(), "other_Runs"));
    EXPECT_EQ(RunsDirectory(temp.path(), "proj"), JoinPath(temp.path(), "proj_Runs"));
}

// 改了 <Name> 之后旧 runs 目录还叫老名字：必须回落到第一个 *_Runs
TEST(ReportsArtifacts, RunsDirectoryFallsBackToAnyRunsDir) {
    ScopedTempDir temp("runs2");
    MakeDirs(JoinPath(temp.path(), "oldname_Runs"));
    EXPECT_EQ(RunsDirectory(temp.path(), "newname"), JoinPath(temp.path(), "oldname_Runs"));
}

TEST(ReportsArtifacts, RunsDirectoryReturnsCandidateWhenNothingExists) {
    ScopedTempDir temp("runs3");
    EXPECT_EQ(RunsDirectory(temp.path(), "proj"), JoinPath(temp.path(), "proj_Runs"));
}

// -----------------------------------------------------------------------------
// CollectArtifacts
// -----------------------------------------------------------------------------

TEST(ReportsArtifacts, CollectArtifactsFromRealLayout) {
    ScopedTempDir temp("collect");
    const std::string name = "CorpusBeta";
    const std::string runs = JoinPath(temp.path(), name + "_Runs");
    MakeDirs(JoinPath(runs, "syn_1"));
    MakeDirs(JoinPath(runs, "phy_1"));
    MakeDirs(JoinPath(runs, "best_result"));
    Touch(JoinPath(runs, "syn_1/CorpusBeta_gate.db"), "gate-db");
    Touch(JoinPath(runs, "phy_1/CorpusBeta_pr.db"), "pr-db-content");
    Touch(JoinPath(runs, "phy_1/CorpusBeta.bit"), "bit-from-phy");
    Touch(JoinPath(runs, "best_result/CorpusBeta.bit"), "bit-from-best");

    fpgalsp::project::Project project;
    project.dir = temp.path();
    project.name = name;

    const ArtifactsReport artifacts = CollectArtifacts(project);
    ASSERT_TRUE(artifacts.runs_found);
    EXPECT_EQ(artifacts.runs_dir, runs);

    // runs 列表排除 best_result
    ASSERT_EQ(artifacts.runs.size(), 2u);
    for (const auto& run : artifacts.runs) EXPECT_NE(run.name, "best_result");
    EXPECT_EQ(artifacts.runs[0].name, "phy_1");
    EXPECT_EQ(artifacts.runs[1].name, "syn_1");

    // bit 优先取 best_result 里的
    EXPECT_TRUE(artifacts.bit.exists);
    EXPECT_NE(artifacts.bit.path.find("best_result"), std::string::npos);
    EXPECT_EQ(artifacts.bit.size, std::optional<int64_t>(13));

    // route_db 取 phy 的 *_pr.db；syn_db 取 syn 的 *_gate.db
    EXPECT_TRUE(artifacts.route_db.exists);
    EXPECT_NE(artifacts.route_db.path.find("CorpusBeta_pr.db"), std::string::npos);
    EXPECT_TRUE(artifacts.syn_db.exists);
    EXPECT_NE(artifacts.syn_db.path.find("CorpusBeta_gate.db"), std::string::npos);

    const auto json = artifacts.ToJson();
    EXPECT_TRUE(json["runs"].is_array());
    EXPECT_TRUE(json["bit"]["exists"]);
    EXPECT_TRUE(json["syn_db"]["exists"]);
    EXPECT_TRUE(json["route_db"]["exists"]);
}

// 没有 runs 目录：runs_found=false，且不抛
TEST(ReportsArtifacts, CollectArtifactsWithoutRunsDirectory) {
    ScopedTempDir temp("collect2");
    fpgalsp::project::Project project;
    project.dir = temp.path();
    project.name = "nothing";

    const ArtifactsReport artifacts = CollectArtifacts(project);
    EXPECT_FALSE(artifacts.runs_found);
    EXPECT_TRUE(artifacts.runs.empty());
    EXPECT_FALSE(artifacts.bit.exists);
    EXPECT_FALSE(artifacts.syn_db.exists);
    EXPECT_FALSE(artifacts.route_db.exists);
}

// 只有 syn 阶段跑过：bit/route_db 不存在，但 syn_db 必须能找到
TEST(ReportsArtifacts, CollectArtifactsSynOnly) {
    ScopedTempDir temp("collect3");
    const std::string runs = JoinPath(temp.path(), "ef2_Runs");
    MakeDirs(JoinPath(runs, "syn_1"));
    Touch(JoinPath(runs, "syn_1/demo_project_gate.db"), "db");

    fpgalsp::project::Project project;
    project.dir = temp.path();
    project.name = "ef2";

    const ArtifactsReport artifacts = CollectArtifacts(project);
    ASSERT_TRUE(artifacts.runs_found);
    ASSERT_EQ(artifacts.runs.size(), 1u);
    EXPECT_EQ(artifacts.runs[0].name, "syn_1");
    EXPECT_TRUE(artifacts.syn_db.exists);
    EXPECT_FALSE(artifacts.bit.exists);
    // route_db 会回落到 syn 的 *_gate.db（Python 行为）
    EXPECT_TRUE(artifacts.route_db.exists);
    EXPECT_NE(artifacts.route_db.path.find("_gate.db"), std::string::npos);
}

TEST(ReportsArtifacts, CollectArtifactsOnMissingProjectDir) {
    fpgalsp::project::Project project;
    project.dir = "Y:/definitely/not/here";
    project.name = "x";
    const ArtifactsReport artifacts = CollectArtifacts(project);
    EXPECT_FALSE(artifacts.runs_found);
}