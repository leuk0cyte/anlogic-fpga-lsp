// =============================================================================
//  tests/unit/report_store_test.cpp —— Phase 2 / A3：ReportStore 缓存
// =============================================================================
//  覆盖（task-23 §5）：
//    * 重复请求命中缓存（同一 mask 返回同一快照、不再读盘/解析）；
//    * 文件变更失效（改报告文件 → 下一次 Get 重建、revision +1）；
//    * 分节请求：不同 mask 各自缓存，且与全量构建的对应段逐字节一致；
//    * 损坏文件不崩（never-throw）；
//    * InvalidatePaths / InvalidateAll / force 的语义；
//    * 并发读（4 线程 × N 次 Get）不会崩、不会重复构建。
//
//  夹具：用 tests/fixtures/reports 的真实报告在 %TEMP% 里搭一棵 *_Runs 树，
//  与 reports_aggregate_test 同源（这里不复制 20 份文件，只造最小必要集合）。
// =============================================================================

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "platform/encoding.h"
#include "project/al_project.h"
#include "reports/report_text.h"
#include "reports/reports_aggregate.h"
#include "server/report_store.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::reports::BuildReports;
using fpgalsp::reports::JoinPath;
using fpgalsp::server::ReportStore;

std::string Fixture(const std::string& name) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports/" + name;
}

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

class ScopedTempDir {
  public:
    explicit ScopedTempDir(const std::string& tag) {
        path_ = Slashes(fs::temp_directory_path().string());
        if (!path_.empty() && path_.back() != '/') path_.push_back('/');
        path_ += "fpgalsp_report_store_" + tag;
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

void Write(const std::string& to, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(to, text);
}

void Copy(const std::string& from, const std::string& to) {
    Write(to, fpgalsp::reports::ReadReportFile(from));
}

// 造一个"综合 + 布局布线都跑过"的最小工程（够覆盖 qor/timing/area/flow 四段）
std::string MakeProjectTree(const ScopedTempDir& temp, const std::string& name) {
    const std::string runs = JoinPath(temp.path(), name + "_Runs");
    const std::string syn = JoinPath(runs, "syn_1");
    const std::string phy = JoinPath(runs, "phy_1");

    Copy(Fixture("eg4_gate.qor"), JoinPath(syn, "gate.qor"));
    Copy(Fixture("eg4_gate.area"), JoinPath(syn, name + "_gate.area"));
    Copy(Fixture("eg4_gate.timing"), JoinPath(syn, name + "_gate.timing"));
    Copy(Fixture("eg4_syn_flow.status"), JoinPath(syn, "flow.status"));

    Copy(Fixture("eg4_route.qor"), JoinPath(phy, "route.qor"));
    Copy(Fixture("eg4_phy.area"), JoinPath(phy, name + "_phy.area"));
    Copy(Fixture("eg4_final_timing.rpt"), JoinPath(phy, "final_timing.rpt"));
    Copy(Fixture("eg4_route_flow.status"), JoinPath(phy, "route_flow.status"));
    Write(JoinPath(phy, name + ".bit"), "bit");
    Write(JoinPath(phy, name + "_pr.db"), "db");
    return runs;
}

fpgalsp::project::Project MakeProject(const std::string& dir, const std::string& name) {
    fpgalsp::project::Project project;
    project.dir = dir;
    project.name = name;
    project.top = "main";
    project.al_path = JoinPath(dir, name + ".al");
    return project;
}

}  // namespace

// -----------------------------------------------------------------------------
// 命中缓存
// -----------------------------------------------------------------------------

TEST(ReportStore, RepeatedGetHitsSnapshotWithoutRebuild) {
    ScopedTempDir temp("hit");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto first = store.Get(project);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->data.contains("timing"));
    EXPECT_EQ(first->mask, fpgalsp::reports::sections::kAll);

    const auto second = store.Get(project);
    EXPECT_EQ(first.get(), second.get());            // 同一份不可变快照
    EXPECT_EQ(first->revision, second->revision);    // revision 不变
    const auto stats = store.stats();
    EXPECT_EQ(stats.builds, 1u);
    EXPECT_EQ(stats.gets, 2u);
    EXPECT_EQ(stats.snapshot_hits, 1u);
    EXPECT_EQ(store.snapshot_count(), 1u);
}

TEST(ReportStore, ForceRebuildsAndBumpsRevision) {
    ScopedTempDir temp("force");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto first = store.Get(project);
    const std::uint64_t revision = first->revision;
    const std::uint64_t global = store.revision();

    const auto rebuilt = store.Get(project, fpgalsp::reports::sections::kAll, /*force=*/true);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_GT(rebuilt->revision, revision);
    EXPECT_GT(store.revision(), global);
    EXPECT_GE(store.stats().builds, 2u);
}

// -----------------------------------------------------------------------------
// 文件变更 -> 失效
// -----------------------------------------------------------------------------

TEST(ReportStore, FileChangeInvalidatesSnapshotOnNextGet) {
    ScopedTempDir temp("change");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto first = store.Get(project);
    const std::uint64_t revision = first->revision;

    // 改 route.qor：内容变长 -> size/mtime 都变
    const std::string qor = JoinPath(JoinPath(runs, "phy_1"), "route.qor");
    Write(qor, fpgalsp::reports::ReadReportFile(qor) + "\n| Extra | 1 |\n");

    // 请求时检查（快路径里的 AnyStaleUnlocked）应该发现过期
    const auto second = store.Get(project);
    ASSERT_NE(second, nullptr);
    EXPECT_GT(second->revision, revision);
    EXPECT_GE(store.stats().invalidations, 1u);
    EXPECT_GE(store.stats().builds, 2u);
}

TEST(ReportStore, CheckForChangesDetectsTouchAndIsQuietWhenUnchanged) {
    ScopedTempDir temp("check");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    (void)store.Get(project);  // 先建立缓存
    EXPECT_FALSE(store.CheckForChanges(project)) << "未变不应该报变化";

    const std::string area = JoinPath(JoinPath(runs, "phy_1"), name + "_phy.area");
    Write(area, fpgalsp::reports::ReadReportFile(area) + "\n# touched\n");
    EXPECT_TRUE(store.CheckForChanges(project));
    EXPECT_FALSE(store.CheckForChanges(project)) << "同一个变化只应报一次";
}

TEST(ReportStore, InvalidatePathsDropsOnlyThatFile) {
    ScopedTempDir temp("invalidate");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    (void)store.Get(project);
    const std::size_t cached = store.cached_file_count();
    ASSERT_GT(cached, 0u);

    const std::string timing = JoinPath(JoinPath(runs, "phy_1"), "final_timing.rpt");
    EXPECT_EQ(store.InvalidatePaths({timing}), 1u);
    EXPECT_EQ(store.cached_file_count(), cached - 1);
    // 快照被清掉 -> 下一次 Get 会重建
    EXPECT_EQ(store.snapshot_count(), 0u);
    const auto rebuilt = store.Get(project);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_TRUE(rebuilt->data.contains("timing"));
}

TEST(ReportStore, InvalidateAllClearsEverything) {
    ScopedTempDir temp("invalidate-all");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    (void)store.Get(project);
    ASSERT_GT(store.cached_file_count(), 0u);

    store.InvalidateAll();
    EXPECT_EQ(store.cached_file_count(), 0u);
    EXPECT_EQ(store.snapshot_count(), 0u);

    const auto again = store.Get(project);
    ASSERT_NE(again, nullptr);
    EXPECT_TRUE(again->data.contains("timing"));
}

// -----------------------------------------------------------------------------
// 分节
// -----------------------------------------------------------------------------

TEST(ReportStore, DifferentMasksAreCachedSeparatelyAndMatchFullBuild) {
    ScopedTempDir temp("mask");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto full = store.Get(project);
    ASSERT_NE(full, nullptr);

    const auto timing = store.Get(project, fpgalsp::reports::sections::kTiming);
    ASSERT_NE(timing, nullptr);
    EXPECT_EQ(timing->mask, fpgalsp::reports::sections::kTiming);
    EXPECT_TRUE(timing->data.contains("timing"));
    EXPECT_FALSE(timing->data.contains("qor"));
    // 分节结果必须与全量构建里的同一段逐字节一致
    EXPECT_EQ(timing->data["timing"].dump(), full->data["timing"].dump());
    EXPECT_EQ(store.snapshot_count(), 2u);

    // 再取一次：命中，不重建
    const auto builds_before = store.stats().builds;
    const auto again = store.Get(project, fpgalsp::reports::sections::kTiming);
    EXPECT_EQ(again.get(), timing.get());
    EXPECT_EQ(store.stats().builds, builds_before);
}

TEST(ReportStore, MaskedRequestDoesNotParseOtherSections) {
    ScopedTempDir temp("lazy");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto timing_only = store.Get(project, fpgalsp::reports::sections::kTiming);
    ASSERT_NE(timing_only, nullptr);
    const auto after_timing = store.stats();
    EXPECT_GT(after_timing.parses, 0u);
    EXPECT_TRUE(timing_only->data.contains("timing"));
    EXPECT_FALSE(timing_only->data.contains("qor"));

    // 再要 area：timing 的解析结果应被复用，解析增量只等于 area 文件数（<=2）
    (void)store.Get(project, fpgalsp::reports::sections::kArea);
    const auto after_area = store.stats();
    EXPECT_GE(after_area.parses, after_timing.parses);
    EXPECT_LE(after_area.parses - after_timing.parses, 2u);
}

// -----------------------------------------------------------------------------
// 健壮性
// -----------------------------------------------------------------------------

TEST(ReportStore, CorruptReportFilesDoNotThrow) {
    ScopedTempDir temp("corrupt");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeProjectTree(temp, name);
    // 把三个报告写成二进制垃圾 / 半截内容
    std::string binary;
    binary.push_back('\0');
    binary += "\x01\x02 not a qor";
    Write(JoinPath(JoinPath(runs, "phy_1"), "route.qor"), binary);
    Write(JoinPath(JoinPath(runs, "phy_1"), name + "_phy.area"), "garbage area");
    Write(JoinPath(JoinPath(runs, "phy_1"), "final_timing.rpt"), std::string(4096, 'x'));
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    const auto snapshot = store.Get(project);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->data.contains("ok"));
    // 解析器 never-throw：坏文件要么给空结构，要么给 warnings，但服务不能挂
    EXPECT_TRUE(snapshot->data.is_object());
}

TEST(ReportStore, NoRunsDirectoryStillReturnsSnapshot) {
    ScopedTempDir temp("no-runs");
    const auto project = MakeProject(temp.path(), "ghost");

    ReportStore store;
    const auto snapshot = store.Get(project);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_FALSE(snapshot->data.value("ok", true));
    EXPECT_TRUE(snapshot->data.contains("error"));
}

// -----------------------------------------------------------------------------
// 并发读
// -----------------------------------------------------------------------------

TEST(ReportStore, ConcurrentGetsShareOneSnapshot) {
    ScopedTempDir temp("concurrent");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    (void)store.Get(project);  // 先建好，避免 4 个线程同时冷启动（那是构建串行化，不是本用例目标）
    const auto builds_before = store.stats().builds;

    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([&store, &project, &ok] {
            for (int round = 0; round < 25; ++round) {
                const auto snapshot = store.Get(project);
                if (snapshot != nullptr && snapshot->data.contains("timing")) ++ok;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(ok.load(), 100);
    // 快照存在时读路径不应触发新构建
    EXPECT_EQ(store.stats().builds, builds_before);
}

TEST(ReportStore, ConcurrentColdStartBuildsOnce) {
    ScopedTempDir temp("concurrent-cold");
    const std::string name = "CorpusBeta";
    MakeProjectTree(temp, name);
    const auto project = MakeProject(temp.path(), name);

    ReportStore store;
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([&store, &project, &ok] {
            const auto snapshot = store.Get(project);
            if (snapshot != nullptr) ++ok;
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(ok.load(), 4);
    // 唯一构建：后到的线程要么命中快照，要么在写锁内发现快照已存在
    EXPECT_EQ(store.stats().builds, 1u);
}
// -----------------------------------------------------------------------------
// 性能：10 MB 报告的首解析 vs 缓存命中（记录数值，不做脆弱的硬断言）
// -----------------------------------------------------------------------------

TEST(ReportStore, LargeReportFirstParseVersusCached) {
    ScopedTempDir temp("large");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeProjectTree(temp, name);

    // 把 final_timing.rpt 膨胀到 ~10 MB（真实 366 KB 夹具重复 ~28 次）
    const std::string timing = JoinPath(JoinPath(runs, "phy_1"), "final_timing.rpt");
    const std::string unit = fpgalsp::reports::ReadReportFile(timing);
    ASSERT_FALSE(unit.empty());
    std::string large;
    large.reserve(unit.size() * 30);
    while (large.size() < 10u * 1024u * 1024u) large += unit;
    Write(timing, large);

    const auto project = MakeProject(temp.path(), name);
    ReportStore store;

    const auto start_first = std::chrono::steady_clock::now();
    const auto first = store.Get(project, fpgalsp::reports::sections::kTiming);
    const double first_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_first)
            .count();
    ASSERT_NE(first, nullptr);

    const auto start_cached = std::chrono::steady_clock::now();
    const auto second = store.Get(project, fpgalsp::reports::sections::kTiming);
    const double cached_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_cached)
            .count();
    ASSERT_NE(second, nullptr);

    const auto stats = store.stats();
    EXPECT_EQ(stats.builds, 1u) << "第二次必须是快照命中，不能重建";
    EXPECT_EQ(stats.snapshot_hits, 1u);
    EXPECT_GT(stats.parses, 0u);
    // 记录实测值（ctest 输出里可见）；不做"必须快 N 倍"的硬断言以免 CI 抖动。
    ::testing::Test::RecordProperty("first_build_ms", std::to_string(first_ms));
    ::testing::Test::RecordProperty("cached_build_ms", std::to_string(cached_ms));
    ::testing::Test::RecordProperty("report_mb", std::to_string(large.size() / (1024 * 1024)));
    EXPECT_LT(cached_ms, first_ms + 5.0);
}

// =============================================================================
//  D9：由文件名推断受影响 section（`reportsChanged` 不再构建快照）
// =============================================================================
TEST(SectionsForPaths, MapsReportFilesToSectionBits) {
    using fpgalsp::server::SectionsForPaths;
    using namespace fpgalsp::reports::sections;

    EXPECT_EQ(SectionsForPaths({}), 0u);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj_Runs/phy_1/route.qor"}), kQor);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj_Runs/phy_1/x_phy.area"}), kArea);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj_Runs/phy_1/final_timing.rpt"}), kTiming);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj_Runs/syn_1/x_gate.timing"}), kTiming);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj_Runs/phy_1/flow.status"}), kFlow);
    EXPECT_EQ(SectionsForPaths({"C:/p/proj/proj.bit"}), kArtifacts);
    // 混合输入按位或；未知扩展名不贡献任何段
    EXPECT_EQ(SectionsForPaths({"a.qor", "b.area", "top.v", "readme.md"}), kQor | kArea);
    // 大小写不敏感（TD 在 Windows 上可能给出混合大小写）
    EXPECT_EQ(SectionsForPaths({"C:/P/X_RUNS/PHY_1/ROUTE.QOR"}), kQor);
}
