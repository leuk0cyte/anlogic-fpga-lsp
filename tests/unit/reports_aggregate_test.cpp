// =============================================================================
//  tests/unit/reports_aggregate_test.cpp —— fpgaLsp/reports 汇总（BuildReports）
// =============================================================================
//  用真实报告夹具搭一棵与 TD 完全同构的 <name>_Runs 树，然后验证：
//    * 顶层挑文件的优先级
//    * syn / phy 两个分阶段段（v13）
//    * flowStatus 的双份语义（顶层带 stage，分阶段不带）
//    * 没有 runs 时的降级
// =============================================================================

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ctime>
#include <gtest/gtest.h>

#include "platform/encoding.h"
#include "reports/report_text.h"
#include "reports/reports_aggregate.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::reports::BuildReports;
using fpgalsp::reports::JoinPath;
using fpgalsp::reports::ReportsBundle;

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
        path_ += "fpgalsp_reports_agg_" + tag;
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

// 把夹具复制成 TD 的产物命名
void Write(const std::string& to, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(to, text);
}

void Copy(const std::string& from, const std::string& to) {
    Write(to, fpgalsp::reports::ReadReportFile(from));
}

// 造一个"综合 + 布局布线都跑过"的工程（用 EG4 的真实报告）
std::string MakeFullProject(const ScopedTempDir& temp, const std::string& name) {
    const std::string runs = JoinPath(temp.path(), name + "_Runs");
    const std::string syn = JoinPath(runs, "syn_1");
    const std::string phy = JoinPath(runs, "phy_1");
    const std::string best = JoinPath(runs, "best_result");

    Copy(Fixture("eg4_gate.qor"), JoinPath(syn, "gate.qor"));
    Copy(Fixture("eg4_gate.area"), JoinPath(syn, name + "_gate.area"));
    Copy(Fixture("eg4_gate.timing"), JoinPath(syn, name + "_gate.timing"));
    Copy(Fixture("eg4_syn_flow.status"), JoinPath(syn, "flow.status"));

    Copy(Fixture("eg4_route.qor"), JoinPath(phy, "route.qor"));
    Copy(Fixture("eg4_place.qor"), JoinPath(phy, "place.qor"));
    Copy(Fixture("eg4_phy.area"), JoinPath(phy, name + "_phy.area"));
    Copy(Fixture("eg4_final_timing.rpt"), JoinPath(phy, "final_timing.rpt"));
    Copy(Fixture("eg4_route_flow.status"), JoinPath(phy, "route_flow.status"));
    Copy(Fixture("eg4_place_flow.status"), JoinPath(phy, "place_flow.status"));
    Write(JoinPath(phy, name + ".bit"), "bit");
    Write(JoinPath(phy, name + "_pr.db"), "db");
    Write(JoinPath(best, name + ".bit"), "best-bit");
    return runs;
}

// JoinPath 复现 os.path.join 的反斜杠语义（PYTHON-BUGS P-13），
// 期望值里可能混着 '/'，比较前统一后比较
bool SamePath(const std::string& a, const std::string& b) {
    return fpgalsp::reports::ToForwardSlashes(a) == fpgalsp::reports::ToForwardSlashes(b);
}

fpgalsp::project::Project MakeProject(const std::string& dir, const std::string& name) {
    fpgalsp::project::Project project;
    project.dir = dir;
    project.name = name;
    project.top = "main";
    return project;
}

}  // namespace

// -----------------------------------------------------------------------------
// 全量工程
// -----------------------------------------------------------------------------

TEST(ReportsAggregate, BuildsFromFullFlow) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("full");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeFullProject(temp, name);
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));

    ASSERT_TRUE(bundle.ok);
    EXPECT_TRUE(bundle.error.empty());
    EXPECT_GT(bundle.generated_at, 1600000000.0);

    // 顶层挑最新一份：phy 的 route.qor / final_timing.rpt / *_phy.area / route_flow.status
    EXPECT_TRUE(SamePath(bundle.files.qor, JoinPath(runs, "phy_1/route.qor"))) << bundle.files.qor;
    EXPECT_TRUE(SamePath(bundle.files.timing, JoinPath(runs, "phy_1/final_timing.rpt"))) << bundle.files.timing;
    EXPECT_TRUE(SamePath(bundle.files.area, JoinPath(runs, "phy_1/CorpusBeta_phy.area"))) << bundle.files.area;
    EXPECT_TRUE(SamePath(bundle.files.flow_status, JoinPath(runs, "phy_1/route_flow.status"))) << bundle.files.flow_status;
    EXPECT_NE(bundle.files.bit.find("best_result"), std::string::npos);

    ASSERT_TRUE(bundle.qor.has_value());
    EXPECT_EQ(bundle.qor->step, "route");
    ASSERT_TRUE(bundle.timing.has_value());
    EXPECT_FALSE(bundle.timing->paths.empty());
    ASSERT_TRUE(bundle.area.has_value());
    EXPECT_FALSE(bundle.area->rows.empty());
    ASSERT_TRUE(bundle.flow_status.has_value());
}

// v13：分阶段段
TEST(ReportsAggregate, BuildsStageSections) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("stages");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeFullProject(temp, name);
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));

    ASSERT_TRUE(bundle.syn.has_value());
    EXPECT_TRUE(bundle.syn->ok);
    EXPECT_EQ(bundle.syn->label, "Synthesis");
    EXPECT_TRUE(SamePath(bundle.syn->files.qor, JoinPath(runs, "syn_1/gate.qor"))) << bundle.syn->files.qor;
    EXPECT_TRUE(SamePath(bundle.syn->files.timing, JoinPath(runs, "syn_1/CorpusBeta_gate.timing"))) << bundle.syn->files.timing;
    EXPECT_TRUE(SamePath(bundle.syn->files.area, JoinPath(runs, "syn_1/CorpusBeta_gate.area"))) << bundle.syn->files.area;
    EXPECT_TRUE(SamePath(bundle.syn->files.flow_status, JoinPath(runs, "syn_1/flow.status"))) << bundle.syn->files.flow_status;
    ASSERT_TRUE(bundle.syn->qor.has_value());
    // step 取 label 第一个词的小写 -> "synthesis"
    EXPECT_EQ(bundle.syn->qor->step, "synthesis");

    ASSERT_TRUE(bundle.phy.has_value());
    EXPECT_TRUE(bundle.phy->ok);
    EXPECT_EQ(bundle.phy->label, "Place & Route");
    EXPECT_TRUE(SamePath(bundle.phy->files.qor, JoinPath(runs, "phy_1/route.qor"))) << bundle.phy->files.qor;
    ASSERT_TRUE(bundle.phy->qor.has_value());
    EXPECT_EQ(bundle.phy->qor->step, "place");
    // phy 的 area 只靠 glob（没有写死的文件名候选）
    EXPECT_TRUE(SamePath(bundle.phy->files.area, JoinPath(runs, "phy_1/CorpusBeta_phy.area"))) << bundle.phy->files.area;
}

// flowStatus 有两份语义：顶层带 stage 标签（合并），分阶段不带
TEST(ReportsAggregate, FlowStatusHasTwoShapes) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("flow");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));

    ASSERT_TRUE(bundle.flow_status.has_value());
    // 顶层：phas 的 7 步 + syn 的 7 步
    ASSERT_FALSE(bundle.flow_status->steps.empty());
    int syn_steps = 0;
    int phy_steps = 0;
    for (const auto& step : bundle.flow_status->steps) {
        if (step.stage == "syn") ++syn_steps;
        if (step.stage == "phy") ++phy_steps;
        EXPECT_FALSE(step.stage.empty()) << "顶层合并后的每一步都必须带 stage";
    }
    EXPECT_GT(syn_steps, 0);
    EXPECT_GT(phy_steps, 0);

    // 分阶段：steps 不带 stage
    ASSERT_TRUE(bundle.syn->flow_status.has_value());
    for (const auto& step : bundle.syn->flow_status->steps) EXPECT_TRUE(step.stage.empty());
    ASSERT_TRUE(bundle.phy->flow_status.has_value());
    for (const auto& step : bundle.phy->flow_status->steps) EXPECT_TRUE(step.stage.empty());

    // flow_stages 里放的是各阶段原始 flowStatus
    ASSERT_EQ(bundle.flow_stages.size(), 2u);
    EXPECT_EQ(bundle.flow_stages[0].first, "syn");
    EXPECT_EQ(bundle.flow_stages[1].first, "phy");

    const auto json = bundle.ToJson();
    EXPECT_TRUE(json["flowStatus"]["stages"].contains("syn"));
    EXPECT_TRUE(json["flowStatus"]["stages"].contains("phy"));
    EXPECT_FALSE(json["syn"]["flowStatus"]["steps"][0].contains("stage"));
}

// 顶层 flowStatus 的 setup/hold 来自 status 文件本身（route_flow.status 是 phy 的）
TEST(ReportsAggregate, TopLevelFlowStatusKeepsTimingNumbers) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("flow2");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));

    ASSERT_TRUE(bundle.flow_status.has_value());
    ASSERT_TRUE(bundle.flow_status->setup.has_value());
    EXPECT_EQ(bundle.flow_status->setup->wns_ps, 171);
}

// 只有综合阶段跑过：没有 phy 段，syn 段仍然完整
TEST(ReportsAggregate, SynOnlyProject) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("synonly");
    const std::string name = "demo_project";
    const std::string runs = JoinPath(temp.path(), name + "_Runs");
    const std::string syn = JoinPath(runs, "syn_1");
    Copy(Fixture("ef2_gate.qor"), JoinPath(syn, "gate.qor"));
    Copy(Fixture("ef2_gate.area"), JoinPath(syn, name + "_gate.area"));
    Copy(Fixture("ef2_gate.timing"), JoinPath(syn, name + "_gate.timing"));
    Copy(Fixture("ef2_syn_flow.status"), JoinPath(syn, "flow.status"));

    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));
    ASSERT_TRUE(bundle.ok);
    EXPECT_FALSE(bundle.phy.has_value());
    ASSERT_TRUE(bundle.syn.has_value());
    EXPECT_TRUE(bundle.syn->ok);
    EXPECT_TRUE(SamePath(bundle.syn->files.area, JoinPath(syn, name + "_gate.area"))) << bundle.syn->files.area;
    EXPECT_NE(bundle.files.timing.find("_gate.timing"), std::string::npos);

    // 顶层 flowStatus 只有 syn 的步骤
    ASSERT_TRUE(bundle.flow_status.has_value());
    for (const auto& step : bundle.flow_status->steps) EXPECT_EQ(step.stage, "syn");
    ASSERT_EQ(bundle.flow_stages.size(), 1u);
    EXPECT_EQ(bundle.flow_stages[0].first, "syn");
}

// 没有任何 runs 目录：ok=false + error，且 artifacts 只有 runs: []
TEST(ReportsAggregate, NoRunsDirectory) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("noruns");
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), "ghost"));

    EXPECT_FALSE(bundle.ok);
    EXPECT_NE(bundle.error.find("No flow has run yet (cannot find ghost_Runs)"),
              std::string::npos);

    const auto json = bundle.ToJson();
    EXPECT_FALSE(json["ok"]);
    EXPECT_TRUE(json.contains("error"));
    EXPECT_TRUE(json["artifacts"]["runs"].is_array());
    EXPECT_TRUE(json["artifacts"]["runs"].empty());
    // 降级形态不应再给 bit/route_db/files/timing
    EXPECT_FALSE(json["artifacts"].contains("bit"));
    EXPECT_FALSE(json.contains("files"));
    EXPECT_FALSE(json.contains("timing"));
    EXPECT_FALSE(json.contains("generatedAt"));
}

// runs 目录存在但里面是空的：ok=true，所有报告段缺失，但不崩
TEST(ReportsAggregate, EmptyRunsDirectory) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("emptyruns");
    {
        std::error_code ec;
        fs::create_directories(fs::path(JoinPath(temp.path(), "ghost_Runs")), ec);
    }
    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), "ghost"));

    EXPECT_TRUE(bundle.ok);
    EXPECT_FALSE(bundle.qor.has_value());
    EXPECT_FALSE(bundle.timing.has_value());
    EXPECT_FALSE(bundle.area.has_value());
    EXPECT_FALSE(bundle.flow_status.has_value());
    EXPECT_FALSE(bundle.syn.has_value());
    EXPECT_FALSE(bundle.phy.has_value());
    EXPECT_FALSE(bundle.warnings.empty());

    const auto json = bundle.ToJson();
    EXPECT_TRUE(json["ok"]);
    EXPECT_TRUE(json["files"]["qor"].get<std::string>().empty());
    EXPECT_TRUE(json.contains("generatedAt"));
}

// qor_suggestion.tcl 里的 set_param 处方要并进 suggestions（带 file 字段）
TEST(ReportsAggregate, MergesQorSuggestionTcl) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("tcl");
    const std::string name = "CorpusBeta";
    const std::string runs = MakeFullProject(temp, name);
    Write(JoinPath(runs, "phy_1/route_qor_suggestion.tcl"),
          "# generated by TD\nset_param -name route_effort -value high\n");

    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));
    ASSERT_TRUE(bundle.qor.has_value());
    ASSERT_FALSE(bundle.qor->suggestions.empty());
    const auto& suggestion = bundle.qor->suggestions.back();
    EXPECT_EQ(suggestion.id, "TD-SUGGESTION");
    EXPECT_EQ(suggestion.text, "set_param -name route_effort -value high");
    EXPECT_NE(suggestion.file.find("route_qor_suggestion.tcl"), std::string::npos);

    const auto json = bundle.qor->ToJson();
    EXPECT_TRUE(json["suggestions"].back().contains("file"));
}

// 畸形报告（二进制垃圾）不能拖垮整包：仍然是 ok=true，只是对应段为空
TEST(ReportsAggregate, GarbageReportsDoNotBreakTheBundle) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("garbage");
    const std::string name = "broken";
    const std::string runs = JoinPath(temp.path(), name + "_Runs");
    const std::string phy = JoinPath(runs, "phy_1");
    std::string garbage;
    for (int i = 0; i < 8192; ++i) garbage.push_back(static_cast<char>((i * 71) & 0xFF));
    Write(JoinPath(phy, "route.qor"), garbage);
    Write(JoinPath(phy, "final_timing.rpt"), garbage);
    Write(JoinPath(phy, name + "_phy.area"), garbage);
    Write(JoinPath(phy, "route_flow.status"), garbage);

    const ReportsBundle bundle = BuildReports(MakeProject(temp.path(), name));
    EXPECT_TRUE(bundle.ok);
    ASSERT_TRUE(bundle.qor.has_value());
    EXPECT_TRUE(bundle.qor->metrics.empty());
    ASSERT_TRUE(bundle.timing.has_value());
    EXPECT_TRUE(bundle.timing->paths.empty());
    ASSERT_TRUE(bundle.area.has_value());
    EXPECT_TRUE(bundle.area->rows.empty());
    ASSERT_TRUE(bundle.flow_status.has_value());
    EXPECT_TRUE(bundle.flow_status->steps.empty());

    const auto json = bundle.ToJson();
    EXPECT_TRUE(json["ok"]);
}

// JSON 顶层键形状：ok/artifacts/files/generatedAt 必在，未解析的段不出现
TEST(ReportsAggregate, JsonKeyShape) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("shape");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);
    const auto json = BuildReports(MakeProject(temp.path(), name)).ToJson();

    EXPECT_TRUE(json.contains("ok"));
    EXPECT_TRUE(json.contains("artifacts"));
    EXPECT_TRUE(json.contains("files"));
    EXPECT_TRUE(json.contains("generatedAt"));
    EXPECT_TRUE(json.contains("qor"));
    EXPECT_TRUE(json.contains("timing"));
    EXPECT_TRUE(json.contains("area"));
    EXPECT_TRUE(json.contains("flowStatus"));
    EXPECT_TRUE(json.contains("syn"));
    EXPECT_TRUE(json.contains("phy"));
    // files 里有 bit，但没有 syn_db（产物在 artifacts 里）
    EXPECT_TRUE(json["files"].contains("bit"));
    EXPECT_TRUE(json["artifacts"].contains("syn_db"));
    // timing.paths 里带 v3 的端点字段
    EXPECT_TRUE(json["timing"]["paths"][0].contains("begin_at"));
    EXPECT_TRUE(json["timing"].contains("begin_end_resolved"));
}

// =============================================================================
//  Phase 2 / A3：section 掩码 + 每文件解析缓存
// =============================================================================
//  这里只测 reports 层（不需要 server）：掩码是否真的少解析、分节结果是否与全量
//  一致、同一个 ReportParseCache 在第二次构建时是否全命中。ReportStore 自己的
//  快照/失效/revision/线程安全在 tests/unit/report_store_test.cpp。
// =============================================================================

namespace {

// 记忆化 + 计数：用真实解析器，但记录"每个文件被解析了几次"。
class CountingCache final : public fpgalsp::reports::ReportParseCache {
  public:
    std::shared_ptr<const fpgalsp::reports::QorReport> GetQor(
        const std::string& path, std::string_view step) override {
        requested.insert("qor:" + path);
        ++calls_qor;
        if (auto it = qor.find(path); it != qor.end()) return it->second;
        auto value = std::make_shared<const fpgalsp::reports::QorReport>(
            fpgalsp::reports::ParseQor(fpgalsp::reports::ReadReportFile(path), step));
        qor.emplace(path, value);
        ++parses;
        return value;
    }
    std::shared_ptr<const fpgalsp::reports::TimingReport> GetTiming(
        const std::string& path, std::string_view filename) override {
        requested.insert("timing:" + path);
        ++calls_timing;
        if (auto it = timing.find(path); it != timing.end()) return it->second;
        auto value = std::make_shared<const fpgalsp::reports::TimingReport>(
            fpgalsp::reports::ParseTiming(fpgalsp::reports::ReadReportFile(path), filename));
        timing.emplace(path, value);
        ++parses;
        return value;
    }
    std::shared_ptr<const fpgalsp::reports::AreaReport> GetArea(const std::string& path) override {
        requested.insert("area:" + path);
        ++calls_area;
        if (auto it = area.find(path); it != area.end()) return it->second;
        auto value = std::make_shared<const fpgalsp::reports::AreaReport>(
            fpgalsp::reports::ParseArea(fpgalsp::reports::ReadReportFile(path)));
        area.emplace(path, value);
        ++parses;
        return value;
    }
    std::shared_ptr<const fpgalsp::reports::FlowStatusReport> GetFlowStatus(
        const std::string& path) override {
        requested.insert("flow:" + path);
        ++calls_flow;
        if (auto it = flow.find(path); it != flow.end()) return it->second;
        auto value = std::make_shared<const fpgalsp::reports::FlowStatusReport>(
            fpgalsp::reports::ParseFlowStatusText(fpgalsp::reports::ReadReportFile(path)));
        flow.emplace(path, value);
        ++parses;
        return value;
    }
    std::shared_ptr<const std::vector<fpgalsp::reports::QorSuggestion>> GetSuggestions(
        const std::string& path) override {
        requested.insert("sug:" + path);
        ++calls_sug;
        if (auto it = suggestions.find(path); it != suggestions.end()) return it->second;
        auto value = std::make_shared<const std::vector<fpgalsp::reports::QorSuggestion>>(
            fpgalsp::reports::ParseSuggestionsTcl(path));
        suggestions.emplace(path, value);
        ++parses;
        return value;
    }

    int parses = 0;
    int calls_qor = 0;
    int calls_timing = 0;
    int calls_area = 0;
    int calls_flow = 0;
    int calls_sug = 0;
    std::set<std::string> requested;

  private:
    std::map<std::string, std::shared_ptr<const fpgalsp::reports::QorReport>> qor;
    std::map<std::string, std::shared_ptr<const fpgalsp::reports::TimingReport>> timing;
    std::map<std::string, std::shared_ptr<const fpgalsp::reports::AreaReport>> area;
    std::map<std::string, std::shared_ptr<const fpgalsp::reports::FlowStatusReport>> flow;
    std::map<std::string, std::shared_ptr<const std::vector<fpgalsp::reports::QorSuggestion>>>
        suggestions;
};

std::string Canon(const fpgalsp::jsonrpc::Json& value) {
    return value.dump();
}

}  // namespace

TEST(ReportsAggregate, SectionMaskSkipsUnrequestedSections) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("mask");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);

    CountingCache cache;
    fpgalsp::reports::ReportsBuildOptions options;
    options.sections = fpgalsp::reports::sections::kTiming;
    options.cache = &cache;
    const auto json = BuildReports(MakeProject(temp.path(), name), options).ToJson();

    // 只解析了 timing；qor/area/flow 一次都没问过
    EXPECT_GT(cache.calls_timing, 0);
    EXPECT_EQ(cache.calls_qor, 0);
    EXPECT_EQ(cache.calls_area, 0);
    EXPECT_EQ(cache.calls_flow, 0);
    // 输出里只有被请求的段（files/artifacts 是廉价元数据，始终在）
    EXPECT_TRUE(json.contains("timing"));
    EXPECT_FALSE(json.contains("qor"));
    EXPECT_FALSE(json.contains("area"));
    EXPECT_FALSE(json.contains("flowStatus"));
    EXPECT_TRUE(json.contains("files"));
    EXPECT_TRUE(json.contains("artifacts"));
}

TEST(ReportsAggregate, SectionSubsetEqualsFullBuild) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("subset");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);
    const auto project = MakeProject(temp.path(), name);

    const auto full = BuildReports(project).ToJson();
    ASSERT_TRUE(full.contains("qor"));

    const std::pair<std::uint32_t, const char*> cases[] = {
        {fpgalsp::reports::sections::kQor, "qor"},
        {fpgalsp::reports::sections::kTiming, "timing"},
        {fpgalsp::reports::sections::kArea, "area"},
        {fpgalsp::reports::sections::kFlow, "flowStatus"},
        {fpgalsp::reports::sections::kFiles, "files"},
        {fpgalsp::reports::sections::kArtifacts, "artifacts"},
    };
    for (const auto& [mask, key] : cases) {
        fpgalsp::reports::ReportsBuildOptions options;
        options.sections = mask;
        const auto part = BuildReports(project, options).ToJson();
        ASSERT_TRUE(part.contains(key)) << key;
        EXPECT_EQ(Canon(part[key]), Canon(full[key])) << "section=" << key;
    }
}

TEST(ReportsAggregate, SameCacheParsesEachFileOnlyOnce) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    ScopedTempDir temp("once");
    const std::string name = "CorpusBeta";
    MakeFullProject(temp, name);
    const auto project = MakeProject(temp.path(), name);

    CountingCache cache;
    fpgalsp::reports::ReportsBuildOptions options;
    options.sections = fpgalsp::reports::sections::kAll;
    options.cache = &cache;

    const auto first = BuildReports(project, options).ToJson();
    const int parses_after_first = cache.parses;
    EXPECT_GT(parses_after_first, 0);

    const auto second = BuildReports(project, options).ToJson();
    // 第二次构建：同一组文件全部命中缓存，解析次数不涨
    EXPECT_EQ(cache.parses, parses_after_first);
    // generatedAt 是时间戳，逐字节比较前先去掉
    fpgalsp::jsonrpc::Json first_cmp = first;
    fpgalsp::jsonrpc::Json second_cmp = second;
    first_cmp.erase("generatedAt");
    second_cmp.erase("generatedAt");
    EXPECT_EQ(Canon(first_cmp), Canon(second_cmp));
}

// =============================================================================
//  B-B6：两次 run 的差异（QoR 指标 + 方向 + 汇总）
// =============================================================================
TEST(CompareRunsTest, DiffsTwoRunsAndExplainsDirection) {
    if (!std::filesystem::is_directory(std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports")) {
        GTEST_SKIP() << "tests/fixtures/reports is not shipped with this repository "
                        "(real TD reports removed for licensing); "
                        "drop your own fixtures there to run this test";
    }
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("fpgalsp-runs-" + std::to_string(::time(nullptr)));
    const fs::path runs = root / "demo_Runs";
    fs::create_directories(runs / "phy_1");
    fs::create_directories(runs / "phy_2");
    // 用真实夹具当两次 run 的 route.qor（指标不同）
    const std::string fixtures = std::string(FPGA_LSP_SOURCE_DIR) + "/tests/fixtures/reports";
    fs::copy_file(fixtures + "/ef2_gate.qor", runs / "phy_1" / "route.qor",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(fixtures + "/eg4_gate.qor", runs / "phy_2" / "route.qor",
                  fs::copy_options::overwrite_existing);

    fpgalsp::project::Project project;
    project.dir = root.generic_string();
    project.name = "demo";
    project.al_path = (root / "demo.al").generic_string();

    const auto runs_list = fpgalsp::reports::ListRuns(project);
    ASSERT_EQ(runs_list.size(), 2u);
    EXPECT_EQ(runs_list[0].name, "phy_1");
    EXPECT_EQ(runs_list[1].name, "phy_2");

    const nlohmann::json diff = fpgalsp::reports::CompareRuns(project, "", "");
    ASSERT_TRUE(diff.at("ok").get<bool>()) << diff.dump();
    EXPECT_EQ(diff.at("from").get<std::string>(), "phy_1");
    EXPECT_EQ(diff.at("to").get<std::string>(), "phy_2");
    ASSERT_FALSE(diff.at("metrics").empty());
    bool saw_setup_wns = false;
    for (const auto& metric : diff.at("metrics")) {
        if (metric.at("name").get<std::string>() != "Setup WNS") continue;
        saw_setup_wns = true;
        // 夹具里 phy_1 = -861，phy_2 = -947 -> 变差
        EXPECT_NEAR(metric.at("from").get<double>(), -861.0, 1e-6);
        EXPECT_NEAR(metric.at("to").get<double>(), -947.0, 1e-6);
        EXPECT_EQ(metric.at("direction").get<std::string>(), "higher-is-better");
        EXPECT_FALSE(metric.at("improved").get<bool>());
    }
    EXPECT_TRUE(saw_setup_wns) << "Setup WNS 必须在对比结果里";
    EXPECT_GE(diff.at("summary").at("compared").get<int>(), 1);
}
