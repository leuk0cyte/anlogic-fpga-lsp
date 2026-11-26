// =============================================================================
//  tests/unit/flow_status_test.cpp —— GUI 6 步（含每步耗时）
// =============================================================================
//  语料全部来自**实测文件**，不是编的：
//   * kSynFlowStatus 是 %WORKSPACE%/corpus_delta\...\corpus_delta_Runs\syn_1\flow.status
//     的原文（TD 2026.2_SP1 写的 Flow Statistics 表）；
//   * 标记文件名来自同一个 run 目录里的 .read_design.* / .opt_rtl.* / .opt_gate.*
//     （phy_1 那次是手工 td_commands_prompt 跑的，没有 flow.status —— 正好当降级用例）。
//
//  重点断言：
//   1) 命令名 -> GUI 步骤的映射与顺序（analyze|elaborate|read_hdl|read_adc|read_sdc|
//      import_db|import_device -> Read Design，等等）；
//   2) "1m11s" -> 71 s（百分比后缀丢弃）、"1h2m3s" -> 3723 s；
//   3) 同一 GUI 步骤下多条命令的耗时**相加**；
//   4) 没有 flow.status 时仍然给出 6 个步骤、状态全 pending、wall 记 0。
// =============================================================================

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "flow/flow_status.h"

namespace {

namespace fs = std::filesystem;

using namespace fpgalsp::flow;  // NOLINT(google-build-using-namespace)

// -----------------------------------------------------------------------------
// 真实语料：syn_1/flow.status 原文
// -----------------------------------------------------------------------------
constexpr const char* kSynFlowStatus = R"(Location         : %WORKSPACE%/corpus_alpha/corpus_alpha_Runs/syn_1
Running with     : Tang Dynasty Release_2026.2_SP1
                   Copyright (c) 2012-2026 Anlogic Inc.
Top Model        : synth_top
Device           : PH1A100SFG676
Speed            : 2

Timing Mode      : basic
Setup            : WNS  1453ps  TNS        0ps  NUM_FEPS     0
Hold             : WNS 2147483647ps  TNS        0ps  NUM_FEPS     0

Flow Statistics  :
---------------------------------------------------------------------------------------------------
     command     |  #calls  |  wall time   |  cpu time(s)  |  used memory(mb)  |  peak memory(mb)  
---------------------------------------------------------------------------------------------------
  optimize_gate  |    1     |  1m11s(62%)  |   238(83%)    |       1125        |       1137        
  optimize_rtl   |    1     |   40s(35%)   |    46(16%)    |        907        |       1090        
    import_db    |    1     |    1s(0%)    |     0(0%)     |        576        |        576        
  import_device  |    1     |    1s(0%)    |     1(0%)     |        280        |        280        
    read_adc     |    1     |    0s(0%)    |     0(0%)     |        576        |        576        
    read_sdc     |    1     |    0s(0%)    |     0(0%)     |        577        |        577        
---------------------------------------------------------------------------------------------------
      total      |    6     |    1m53s     |      285      |       1125        |       1137        
---------------------------------------------------------------------------------------------------
)";

// 六条命令各归一步的合成表（只用来钉映射，不代表真实运行）
constexpr const char* kAllCommands = R"(Flow Statistics  :
------------------------------------------------------------
     command     |  #calls  |  wall time   |  cpu time(s)  |  used memory(mb)  |  peak memory(mb)
------------------------------------------------------------
       analyze   |    1     |    3s(1%)    |     2(1%)     |        100        |        110
     elaborate   |    1     |    4s(2%)    |     3(2%)     |        200        |        210
      read_hdl   |    1     |    5s(2%)    |     4(2%)     |        300        |        310
  optimize_rtl   |    1     |   40s(35%)   |    46(16%)    |        907        |       1090
  optimize_gate  |    1     |  1m11s(62%)  |   238(83%)    |       1125        |       1137
        place    |    1     |  1m2s(10%)   |   300(30%)    |        800        |        810
legalize_phy_inst|    1     |    5s(1%)    |     1(1%)     |        120        |        130
        route    |    1     |  1h2m3s(80%) |   500(50%)    |        600        |        700
       bitgen    |    1     |   12.5s(5%)  |     9(5%)     |        400        |        410
   setup_debugger|    1     |    2s(1%)    |     1(1%)     |        500        |        510
------------------------------------------------------------
)";

// -----------------------------------------------------------------------------
// 临时目录（用例自己造 run 目录，绝不动仓库语料）
// -----------------------------------------------------------------------------
class TempRuns {
  public:
    explicit TempRuns(const std::string& name) {
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        root_ = (ec ? fs::path(".") : base) / ("fpgalsp-flow-status-" + name);
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    ~TempRuns() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempRuns(const TempRuns&) = delete;
    TempRuns& operator=(const TempRuns&) = delete;

    std::string root() const { return root_.string(); }

    std::string MakeRun(const std::string& run_name) const {
        const fs::path dir = root_ / run_name;
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.string();
    }

    static void Touch(const std::string& path) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "";
    }

    static void Write(const std::string& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

  private:
    fs::path root_;
};

const FlowStep* FindStep(const std::vector<FlowStep>& steps, const std::string& name) {
    for (const FlowStep& step : steps) {
        if (step.name == name) return &step;
    }
    return nullptr;
}

std::string NamesOf(const std::vector<FlowStep>& steps) {
    std::string out;
    for (const FlowStep& step : steps) {
        if (!out.empty()) out += ",";
        out += step.name;
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// 1) 耗时归一化
// -----------------------------------------------------------------------------

TEST(FlowStatus, ParsesWallTimeStringsIntoSeconds) {
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("1m11s(62%)"), 71.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("40s(35%)"), 40.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("0s(0%)"), 0.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("1m53s"), 113.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("1h2m3s"), 3723.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("12.5s"), 12.5);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("238(83%)"), 238.0);  // cpu 列没有单位
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds("  7s  "), 7.0);
    EXPECT_DOUBLE_EQ(ParseFlowStatusSeconds(""), 0.0);
}

// -----------------------------------------------------------------------------
// 2) 真实表 -> GUI 步骤（顺序 = GUI 顺序，不是表里的耗时降序）
// -----------------------------------------------------------------------------

TEST(FlowStatus, RealSynTableMapsToGuiStepsWithTimes) {
    const std::vector<FlowStep> steps = ParseFlowStatus(kSynFlowStatus);
    ASSERT_EQ(steps.size(), 3u) << NamesOf(steps);
    EXPECT_EQ(NamesOf(steps), "Read Design,Optimize RTL,Optimize Gate");

    // Read Design = import_device + import_db + read_adc + read_sdc（1+1+0+0 = 2 s）
    const FlowStep* design = FindStep(steps, "Read Design");
    ASSERT_NE(design, nullptr);
    EXPECT_EQ(design->stage, "design");
    EXPECT_DOUBLE_EQ(design->wall, 2.0);
    EXPECT_DOUBLE_EQ(design->cpu, 1.0);      // 1+0+0+0
    EXPECT_EQ(design->mem_mb, 577LL);        // used memory 取最大值，不是相加

    // Optimize RTL = optimize_rtl 一行
    const FlowStep* rtl = FindStep(steps, "Optimize RTL");
    ASSERT_NE(rtl, nullptr);
    EXPECT_EQ(rtl->stage, "rtl");
    EXPECT_DOUBLE_EQ(rtl->wall, 40.0);
    EXPECT_DOUBLE_EQ(rtl->cpu, 46.0);
    EXPECT_EQ(rtl->mem_mb, 907LL);

    // Optimize Gate = optimize_gate 一行，"1m11s(62%)" -> 71 s
    const FlowStep* gate = FindStep(steps, "Optimize Gate");
    ASSERT_NE(gate, nullptr);
    EXPECT_EQ(gate->stage, "gate");
    EXPECT_DOUBLE_EQ(gate->wall, 71.0);
    EXPECT_DOUBLE_EQ(gate->cpu, 238.0);
    EXPECT_EQ(gate->mem_mb, 1125LL);

    // total 行不能被当成一步
    EXPECT_EQ(FindStep(steps, "total"), nullptr);
}

TEST(FlowStatus, EveryGuiStepHasItsCommandsMapped) {
    const std::vector<FlowStep> steps = ParseFlowStatus(kAllCommands);
    ASSERT_EQ(steps.size(), 6u) << NamesOf(steps);
    EXPECT_EQ(NamesOf(steps),
              "Read Design,Optimize RTL,Optimize Gate,Optimize Placement,Optimize Routing,"
              "Generate Bitstream");

    const FlowStep* design = FindStep(steps, "Read Design");
    ASSERT_NE(design, nullptr);
    EXPECT_DOUBLE_EQ(design->wall, 3.0 + 4.0 + 5.0);  // analyze + elaborate + read_hdl

    const FlowStep* place = FindStep(steps, "Optimize Placement");
    ASSERT_NE(place, nullptr);
    EXPECT_EQ(place->stage, "place");
    EXPECT_DOUBLE_EQ(place->wall, 62.0 + 5.0);        // place "1m2s" + legalize_phy_inst
    EXPECT_DOUBLE_EQ(place->cpu, 300.0 + 1.0);

    const FlowStep* route = FindStep(steps, "Optimize Routing");
    ASSERT_NE(route, nullptr);
    EXPECT_DOUBLE_EQ(route->wall, 3723.0);            // route "1h2m3s"

    const FlowStep* bitgen = FindStep(steps, "Generate Bitstream");
    ASSERT_NE(bitgen, nullptr);
    EXPECT_EQ(bitgen->stage, "bitgen");
    EXPECT_DOUBLE_EQ(bitgen->wall, 12.5 + 2.0);       // bitgen + setup_debugger
}

// -----------------------------------------------------------------------------
// 3) 没有 flow.status：6 步全 pending、wall 0（phy_1 实测就是这个状态）
// -----------------------------------------------------------------------------

TEST(FlowStatus, MissingFlowStatusStillYieldsSixPendingSteps) {
    TempRuns runs("no-status");
    const std::vector<FlowStep> steps = CollectAllFlowSteps(runs.root() + "/does-not-exist");
    ASSERT_EQ(steps.size(), 6u) << NamesOf(steps);
    EXPECT_EQ(NamesOf(steps),
              "Read Design,Optimize RTL,Optimize Gate,Optimize Placement,Optimize Routing,"
              "Generate Bitstream");
    for (const FlowStep& step : steps) {
        EXPECT_EQ(step.status, "pending") << step.name;
        EXPECT_DOUBLE_EQ(step.wall, 0.0) << step.name;
        EXPECT_DOUBLE_EQ(step.cpu, 0.0) << step.name;
        EXPECT_EQ(step.mem_mb, 0LL) << step.name;
    }
    EXPECT_EQ(steps[0].stage, "design");
    EXPECT_EQ(steps[3].stage, "place");
    EXPECT_EQ(steps[5].stage, "bitgen");
}

// -----------------------------------------------------------------------------
// 4) 标记文件给 status：pending / running / done / failed
// -----------------------------------------------------------------------------

TEST(FlowStatus, MarkersDecideStatusAndTableSuppliesTimes) {
    TempRuns runs("markers");
    const std::string syn = runs.MakeRun("syn_1");
    // 实测文件名：.<step>.begin.f / .<step>.end.f / .<step>.error.f
    TempRuns::Touch(syn + "/.read_design.begin.f");
    TempRuns::Touch(syn + "/.read_design.end.f");
    TempRuns::Touch(syn + "/.opt_rtl.error.f");  // 这一步失败了
    TempRuns::Write(syn + "/flow.status", kSynFlowStatus);

    const std::vector<FlowStep> steps = CollectFlowSteps(syn, "syn_1");
    ASSERT_EQ(steps.size(), 3u) << NamesOf(steps);
    EXPECT_EQ(steps[0].name, "Read Design");
    EXPECT_EQ(steps[0].status, "done");
    EXPECT_DOUBLE_EQ(steps[0].wall, 2.0);
    EXPECT_EQ(steps[1].name, "Optimize RTL");
    EXPECT_EQ(steps[1].status, "failed");
    EXPECT_DOUBLE_EQ(steps[1].wall, 40.0);
    // 表里有耗时、但没有 .begin.f/.end.f 的步骤：视为已经跑完
    EXPECT_EQ(steps[2].name, "Optimize Gate");
    EXPECT_EQ(steps[2].status, "done");
    EXPECT_DOUBLE_EQ(steps[2].wall, 71.0);
}

// 单步 Reset and Run 的配套清理：标记文件删了还不够，统计表里"该步及下游"的行
// 也必须删 —— 否则 CollectFlowSteps 的"表里有耗时 -> done"会把下游重新标成已完成。
TEST(FlowStatus, StripFromMarkerClearsThisStepAndDownstreamRows) {
    const std::string stripped = StripFlowStatusFromMarker(kSynFlowStatus, "opt_rtl");
    ASSERT_FALSE(stripped.empty());
    EXPECT_NE(stripped, std::string(kSynFlowStatus));

    TempRuns runs("strip_marker");
    const std::string syn = runs.MakeRun("syn_1");
    TempRuns::Write(syn + "/flow.status", stripped);

    const std::vector<FlowStep> steps = CollectFlowSteps(syn, "syn_1");
    ASSERT_EQ(steps.size(), 3u) << NamesOf(steps);
    EXPECT_DOUBLE_EQ(steps[0].wall, 2.0);  // Read Design（上游）保留
    EXPECT_EQ(steps[0].status, "done");    // 表里有耗时 -> 视为跑过（上游产物仍在）
    EXPECT_DOUBLE_EQ(steps[1].wall, 0.0);  // Optimize RTL 的行已删
    EXPECT_EQ(steps[1].status, "pending");
    EXPECT_DOUBLE_EQ(steps[2].wall, 0.0);  // Optimize Gate 的行已删
    EXPECT_EQ(steps[2].status, "pending");

    // 不认识的 marker：原样返回（不要静默清表）
    EXPECT_EQ(StripFlowStatusFromMarker(kSynFlowStatus, "nope"), std::string(kSynFlowStatus));
    // 空文本：原样返回
    EXPECT_TRUE(StripFlowStatusFromMarker("", "opt_rtl").empty());
}

TEST(FlowStatus, BeginMarkerWithoutEndMeansRunning) {    TempRuns runs("running");
    const std::string phy = runs.MakeRun("phy_1");
    TempRuns::Touch(phy + "/.opt_place.end.f");
    TempRuns::Touch(phy + "/.opt_route.begin.f");

    const std::vector<FlowStep> steps = CollectFlowSteps(phy, "phy_1");
    ASSERT_EQ(steps.size(), 3u) << NamesOf(steps);
    EXPECT_EQ(NamesOf(steps), "Optimize Placement,Optimize Routing,Generate Bitstream");
    EXPECT_EQ(steps[0].status, "done");
    EXPECT_EQ(steps[1].status, "running");
    EXPECT_EQ(steps[2].status, "pending");
    EXPECT_DOUBLE_EQ(steps[1].wall, 0.0);  // 没有 flow.status，耗时只能是 0
}

// -----------------------------------------------------------------------------
// 5) 两个 run 合并成 6 条：syn 有表、phy 没有表（本仓库实测的 syn_1 + phy_1）
// -----------------------------------------------------------------------------

TEST(FlowStatus, MergesSynAndPhyRunsIntoGuiOrder) {
    TempRuns runs("merge");
    const std::string syn = runs.MakeRun("syn_1");
    TempRuns::Touch(syn + "/.read_design.end.f");
    TempRuns::Touch(syn + "/.opt_rtl.end.f");
    TempRuns::Touch(syn + "/.opt_gate.end.f");
    TempRuns::Write(syn + "/flow.status", kSynFlowStatus);
    runs.MakeRun("phy_1");  // 空目录：没有 flow.status、没有标记
    runs.MakeRun("best_result");

    const std::vector<FlowStep> steps = CollectAllFlowSteps(runs.root());
    ASSERT_EQ(steps.size(), 6u) << NamesOf(steps);
    EXPECT_EQ(NamesOf(steps),
              "Read Design,Optimize RTL,Optimize Gate,Optimize Placement,Optimize Routing,"
              "Generate Bitstream");

    EXPECT_EQ(steps[0].status, "done");
    EXPECT_DOUBLE_EQ(steps[0].wall, 2.0);
    EXPECT_DOUBLE_EQ(steps[2].wall, 71.0);
    for (std::size_t i = 3; i < 6; ++i) {
        EXPECT_EQ(steps[i].status, "pending") << steps[i].name;
        EXPECT_DOUBLE_EQ(steps[i].wall, 0.0) << steps[i].name;
    }

    const std::vector<RunFlowSteps> runs_steps = CollectRunsFlowSteps(runs.root());
    ASSERT_EQ(runs_steps.size(), 2u);
    EXPECT_EQ(runs_steps[0].run_name, "syn_1");
    EXPECT_EQ(runs_steps[1].run_name, "phy_1");
    EXPECT_EQ(runs_steps[0].steps.size(), 3u);
    EXPECT_EQ(runs_steps[1].steps.size(), 3u);
}

// -----------------------------------------------------------------------------
// 6) *_flow.status（place_flow.status / route_flow.status）也要读，
//    而且同一命令在多个快照里出现时取较大值（否则会重复计数）
// -----------------------------------------------------------------------------

TEST(FlowStatus, MergesPlaceAndRouteSnapshotsWithoutDoubleCounting) {
    TempRuns runs("snapshots");
    const std::string phy = runs.MakeRun("phy_1");
    TempRuns::Write(phy + "/place_flow.status", R"(Flow Statistics  :
------------------------------------------------------------
     command     |  #calls  |  wall time   |  cpu time(s)  |  used memory(mb)  |  peak memory(mb)
------------------------------------------------------------
        place    |    1     |  1m2s(10%)   |   300(30%)    |        800        |        810
   update_timing |    1     |    5s(1%)    |     2(1%)     |        300        |        310
------------------------------------------------------------
)");
    TempRuns::Write(phy + "/route_flow.status", R"(Flow Statistics  :
------------------------------------------------------------
     command     |  #calls  |  wall time   |  cpu time(s)  |  used memory(mb)  |  peak memory(mb)
------------------------------------------------------------
        place    |    1     |  1m2s(10%)   |   300(30%)    |        800        |        810
        route    |    1     |  2m0s(50%)   |   500(50%)    |        600        |        700
       bitgen    |    1     |    9s(5%)    |     9(5%)     |        400        |        410
------------------------------------------------------------
)");
    TempRuns::Touch(phy + "/.opt_place.end.f");
    TempRuns::Touch(phy + "/.opt_route.end.f");
    TempRuns::Touch(phy + "/.bitgen.end.f");

    const std::vector<FlowStep> steps = CollectFlowSteps(phy, "phy_1");
    ASSERT_EQ(steps.size(), 3u) << NamesOf(steps);
    const FlowStep* place = FindStep(steps, "Optimize Placement");
    ASSERT_NE(place, nullptr);
    // place 在 place_flow.status 和 route_flow.status 里都出现：取 62 s，不是 124 s；
    // update_timing 故意不映射（它同时出现在 gate/place/route 三步里）
    EXPECT_DOUBLE_EQ(place->wall, 62.0);
    EXPECT_DOUBLE_EQ(place->cpu, 300.0);
    const FlowStep* route = FindStep(steps, "Optimize Routing");
    ASSERT_NE(route, nullptr);
    EXPECT_DOUBLE_EQ(route->wall, 120.0);
    const FlowStep* bitgen = FindStep(steps, "Generate Bitstream");
    ASSERT_NE(bitgen, nullptr);
    EXPECT_DOUBLE_EQ(bitgen->wall, 9.0);
}

// -----------------------------------------------------------------------------
// 7) 增量布线 run（route_*）：只有 route + bitgen 两步
// -----------------------------------------------------------------------------

TEST(FlowStatus, RouteRunContributesRouteAndBitgenOnly) {
    TempRuns runs("route-run");
    const std::string route = runs.MakeRun("route_1");
    TempRuns::Touch(route + "/.opt_route.end.f");
    TempRuns::Touch(route + "/.bitgen.end.f");

    const std::vector<FlowStep> steps = CollectAllFlowSteps(runs.root());
    ASSERT_EQ(steps.size(), 6u) << NamesOf(steps);
    EXPECT_EQ(steps[4].name, "Optimize Routing");
    EXPECT_EQ(steps[4].status, "done");
    EXPECT_EQ(steps[5].name, "Generate Bitstream");
    EXPECT_EQ(steps[5].status, "done");
    // 前面 4 步没跑过
    for (std::size_t i = 0; i < 4; ++i) EXPECT_EQ(steps[i].status, "pending") << steps[i].name;
}