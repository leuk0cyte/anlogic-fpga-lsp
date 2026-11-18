// =============================================================================
//  tests/unit/flow_logic_test.cpp —— Flow 纯判定逻辑
// =============================================================================
//  重点：PRJ-8806/PRJ-5303 容错的**三种组合**（task-19 §1.4 的核心要求），
//  以及"Python 里那几个容易看错的判定"（stage 别名、同名 token、stall 两段）。
// =============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "flow/flow_logic.h"

namespace {

// Json 在 fpgalsp::jsonrpc 里；这些用例大量用到，起个别名
namespace jsonrpc = fpgalsp::jsonrpc;
using Json = fpgalsp::jsonrpc::Json;

using namespace fpgalsp::flow;  // NOLINT(google-build-using-namespace)

std::vector<std::string> Names(std::initializer_list<const char*> items) {
    std::vector<std::string> out;
    for (const char* item : items) out.emplace_back(item);
    return out;
}

// 一个"综合跑完"的 run 目录
std::vector<std::string> SynCompleteNames() {
    return Names({".read_design.begin.f", ".read_design.end.f", ".opt_rtl.begin.f",
                  ".opt_rtl.end.f", ".opt_gate.begin.f", ".opt_gate.end.f",
                  "demo_project_gate.db", "gate.qor", "gate.area"});
}

std::vector<std::string> PhyCompleteNames() {
    return Names({".bitgen.begin.f", ".bitgen.end.f", ".place.end.f", "main.bit",
                  "CorpusBeta_pr.db", "route.qor"});
}

}  // namespace

// -----------------------------------------------------------------------------
// 阶段归一化
// -----------------------------------------------------------------------------

TEST(FlowLogicStages, AllExpandsToSynAndPhy) {
    const std::vector<std::string> stages = NormaliseStages({"all"});
    ASSERT_EQ(stages.size(), 2u);
    EXPECT_EQ(stages[0], "syn");
    EXPECT_EQ(stages[1], "phy");
}

TEST(FlowLogicStages, RouteAliasesCollapseToRoute) {
    for (const char* alias : {"route", "reroute", "route_only", "incremental"}) {
        const std::vector<std::string> stages = NormaliseStages({alias});
        ASSERT_EQ(stages.size(), 1u) << alias;
        EXPECT_EQ(stages[0], "route") << alias;
    }
}

TEST(FlowLogicStages, SynAndPhyAliases) {
    EXPECT_EQ(NormaliseStages({"synthesis"}), std::vector<std::string>{"syn"});
    EXPECT_EQ(NormaliseStages({"synth"}), std::vector<std::string>{"syn"});
    EXPECT_EQ(NormaliseStages({"syn"}), std::vector<std::string>{"syn"});
    EXPECT_EQ(NormaliseStages({"pnr"}), std::vector<std::string>{"phy"});
    EXPECT_EQ(NormaliseStages({"place_route"}), std::vector<std::string>{"phy"});
    EXPECT_EQ(NormaliseStages({"phy"}), std::vector<std::string>{"phy"});
}

// "all" / "route" 是**提前返回**：出现在列表任何位置都会吃掉整个列表
TEST(FlowLogicStages, AllAndRouteShortCircuit) {
    EXPECT_EQ(NormaliseStages({"syn", "all"}), (std::vector<std::string>{"syn", "phy"}));
    EXPECT_EQ(NormaliseStages({"syn", "incremental"}), std::vector<std::string>{"route"});
}

TEST(FlowLogicStages, DedupesAndKeepsOrder) {
    EXPECT_EQ(NormaliseStages({"phy", "syn", "phy"}), (std::vector<std::string>{"phy", "syn"}));
}

TEST(FlowLogicStages, UnknownStagesYieldEmpty) {
    EXPECT_TRUE(NormaliseStages({}).empty());
    EXPECT_TRUE(NormaliseStages({"bitgen"}).empty());
    EXPECT_TRUE(NormaliseStages({"SYN "}).size() == 1u) << "大小写与空白要归一化";
}

TEST(FlowLogicStages, LabelsMatchPython) {
    EXPECT_EQ(StageLabel("syn"), "Synthesis");
    EXPECT_EQ(StageLabel("phy"), "Place & Route");
    EXPECT_EQ(StageLabel("route"), "Incremental route");
    EXPECT_EQ(StageLabel("weird"), "weird");
}

// -----------------------------------------------------------------------------
// 阶段进度识别
// -----------------------------------------------------------------------------

TEST(FlowLogicPhase, RecognisesBothOptSpellings) {
    EXPECT_EQ(MatchPhaseToken("INFO: running optimize_rtl"), "Optimize RTL");
    EXPECT_EQ(MatchPhaseToken("INFO: running opt_rtl"), "Optimize RTL");
    EXPECT_EQ(MatchPhaseToken("run optimize_gate"), "Optimize Gate");
    EXPECT_EQ(MatchPhaseToken("run opt_gate"), "Optimize Gate");
}

TEST(FlowLogicPhase, TokenTableOrderWins) {
    // 同一行同时含两个 token 时取表里靠前的（opt_rtl 在 place/route 之前）
    const std::string line = "opt_rtl then place then route";
    EXPECT_EQ(MatchPhaseToken(line), "Optimize RTL");
    // read_design 最靠前
    EXPECT_EQ(MatchPhaseToken("read_design then bitgen"), "Read Design");
}

TEST(FlowLogicPhase, NoTokenYieldsEmpty) {
    EXPECT_TRUE(MatchPhaseToken("just some text").empty());
    EXPECT_TRUE(MatchPhaseToken("").empty());
}

TEST(FlowLogicPhase, DetectProgressTakesLastPhaseAndLastMessage) {
    const std::vector<std::string> tail = {
        "===", "some header", "running opt_rtl", "  stage: opt_rtl  ", "===",
        "last real line", "bw >",
    };
    const PhaseDetection detection = DetectProgress(tail);
    // 从后往前：第一条命中 token 的是 "  stage: opt_rtl  "
    EXPECT_EQ(detection.phase, "Optimize RTL");
    // 最后一条非空且不以 === 开头的是 "bw >"
    EXPECT_EQ(detection.message, "bw >");
}

TEST(FlowLogicPhase, DetectProgressTruncatesLongMessages) {
    const std::vector<std::string> tail = {std::string(400, 'x')};
    const PhaseDetection detection = DetectProgress(tail);
    EXPECT_EQ(detection.message.size(), 160u);
}

TEST(FlowLogicPhase, CompletedStepsFromMarkersFollowTableOrder) {
    const std::vector<std::string> names = {
        ".opt_gate.end.f", ".read_design.end.f", ".bitgen.end.f", "run.log"};
    const std::vector<std::string> steps = CompletedStepsFromMarkers(names);
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0], "read_design");
    EXPECT_EQ(steps[1], "opt_gate");
    EXPECT_EQ(steps[2], "bitgen");
}

// -----------------------------------------------------------------------------
// run 目录状态
// -----------------------------------------------------------------------------

TEST(FlowLogicRun, RunStartedNeedsATrace) {
    EXPECT_FALSE(RunStarted(Names({"gate.qor"})));
    EXPECT_TRUE(RunStarted(Names({".read_design.begin.f"})));
    EXPECT_TRUE(RunStarted(Names({".opt_gate.end.f"})));
    EXPECT_TRUE(RunStarted(Names({"run.log"})));
    EXPECT_FALSE(RunStarted({}));
}

TEST(FlowLogicRun, SynCompletedNeedsOptGateEndAndGateDb) {
    EXPECT_TRUE(RunCompleted("syn_1", SynCompleteNames(), false, "demo_project"));
    // 少 *_gate.db
    std::vector<std::string> names;
    for (const std::string& name : SynCompleteNames()) {
        if (name.find("_gate.db") != std::string::npos) continue;  // 去掉 db
        names.push_back(name);
    }
    EXPECT_FALSE(RunCompleted("syn_1", names, false, "demo_project"));
    // 少 .opt_gate.end.f
    EXPECT_FALSE(RunCompleted("syn_1", Names({".opt_gate.begin.f", "x_gate.db"}), false, "x"));
}

TEST(FlowLogicRun, PhyCompletedNeedsBitgenEndAndBit) {
    EXPECT_TRUE(RunCompleted("phy_1", PhyCompleteNames(), true, "main"));
    // 目录里没有 *.bit 且工程 bit 不存在 -> 未完成
    EXPECT_FALSE(RunCompleted("phy_1", Names({".bitgen.end.f", "route.qor"}), false, "main"));
    // 目录里有任意 *.bit 也算
    EXPECT_TRUE(RunCompleted("phy_1", Names({".bitgen.end.f", "other.bit"}), false, "main"));
    // .bitgen.end.f 缺失
    EXPECT_FALSE(RunCompleted("phy_1", Names({"main.bit"}), true, "main"));
}

// 条件 3：任何 .error.f 都必须让 RunCompleted 直接否决
TEST(FlowLogicRun, AnyErrorMarkerVetoesCompletion) {
    std::vector<std::string> names = PhyCompleteNames();
    names.push_back(".route.error.f");
    EXPECT_FALSE(RunCompleted("phy_1", names, true, "main"));

    std::vector<std::string> syn = SynCompleteNames();
    syn.push_back(".opt_gate.error.f");
    EXPECT_FALSE(RunCompleted("syn_1", syn, false, "demo_project"));
}

TEST(FlowLogicRun, UnknownRunFallsBackToAnyEndMarker) {
    EXPECT_TRUE(RunCompleted("custom_run", Names({".foo.end.f"}), false, "x"));
    EXPECT_FALSE(RunCompleted("custom_run", Names({".foo.begin.f"}), false, "x"));
}

TEST(FlowLogicRun, ErrorMarkersAndSteps) {
    const std::vector<std::string> names = {".opt_gate.error.f", ".route.error.f", "gate.qor"};
    EXPECT_EQ(ErrorMarkers(names).size(), 2u);
    const std::vector<std::string> steps = ErrorMarkerSteps(names);
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0], "opt_gate");
    EXPECT_EQ(steps[1], "route");
}

// -----------------------------------------------------------------------------
// PRJ-8806 / PRJ-5303 容错：三种组合
// -----------------------------------------------------------------------------

TEST(FlowLogicPrj, OnlyToleratedCodesAreRecognised) {
    EXPECT_TRUE(OnlyToleratedPrjErrors("PRJ-8806 ERROR: bitgen exited\n"));
    EXPECT_TRUE(OnlyToleratedPrjErrors("PRJ-5303 ERROR: something\nPRJ-8806 ERROR: again\n"));
    EXPECT_FALSE(OnlyToleratedPrjErrors("PRJ-8806 ERROR: x\nPRJ-8001 ERROR: y\n"));
    EXPECT_FALSE(OnlyToleratedPrjErrors("no errors here"));
    EXPECT_FALSE(OnlyToleratedPrjErrors(""));
    // 其它错误码单独出现时不属于容错范围
    EXPECT_FALSE(OnlyToleratedPrjErrors("USR-8316 ERROR: invalid speed grade"));
}

// 组合 1：错误码是 PRJ-8806 + 完成标记在 + 没有 .error.f -> **成功**
TEST(FlowLogicPrj, ToleratedWhenArtifactsComplete) {
    RunSnapshot snapshot;
    snapshot.run_name = "phy_1";
    snapshot.run_dir_names = PhyCompleteNames();
    snapshot.project_name = "main";
    snapshot.project_bit_exists = true;
    snapshot.run_dir = "Y:/prj/main_Runs/phy_1";

    const RunOutcome outcome =
        DecideRunOutcome(snapshot, "PRJ-8806 ERROR: bitgen exited with code 1\n");
    EXPECT_TRUE(outcome.accepted);
    EXPECT_TRUE(outcome.tolerated_prj_error);
    EXPECT_TRUE(outcome.failure_detail.empty());
}

// 组合 2：错误码对、但没有完成标记 -> **失败**（不能把真失败当成功）
TEST(FlowLogicPrj, NotToleratedWithoutCompletionMarker) {
    RunSnapshot snapshot;
    snapshot.run_name = "phy_1";
    snapshot.run_dir_names = Names({".bitgen.begin.f", "route.qor"});  // 没有 .end.f / .bit
    snapshot.project_name = "main";
    snapshot.project_bit_exists = false;
    snapshot.run_dir = "Y:/prj/main_Runs/phy_1";

    const RunOutcome outcome = DecideRunOutcome(snapshot, "PRJ-8806 ERROR: died\n");
    EXPECT_FALSE(outcome.accepted);
    EXPECT_FALSE(outcome.tolerated_prj_error);
    EXPECT_NE(outcome.failure_detail.find("PRJ-8806"), std::string::npos);
}

// 组合 3：错误码对、完成标记也在，但存在 .error.f -> **失败**
TEST(FlowLogicPrj, NotToleratedWhenErrorMarkerPresent) {
    std::vector<std::string> names = PhyCompleteNames();
    names.push_back(".bitgen.error.f");
    RunSnapshot snapshot;
    snapshot.run_name = "phy_1";
    snapshot.run_dir_names = names;
    snapshot.project_name = "main";
    snapshot.project_bit_exists = true;
    snapshot.run_dir = "Y:/prj/main_Runs/phy_1";

    const RunOutcome outcome = DecideRunOutcome(snapshot, "PRJ-8806 ERROR: died\n");
    EXPECT_FALSE(outcome.accepted);
    EXPECT_NE(outcome.failure_detail.find("failed run step: bitgen"), std::string::npos);
}

// 不在容错清单里的错误码：即使产物齐全也判失败
TEST(FlowLogicPrj, OtherErrorCodesAreNeverTolerated) {
    RunSnapshot snapshot;
    snapshot.run_name = "phy_1";
    snapshot.run_dir_names = PhyCompleteNames();
    snapshot.project_name = "main";
    snapshot.project_bit_exists = true;
    snapshot.run_dir = "Y:/prj/main_Runs/phy_1";

    const RunOutcome outcome = DecideRunOutcome(snapshot, "PRJ-8001 ERROR: empty device_name\n");
    EXPECT_FALSE(outcome.accepted);
    EXPECT_NE(outcome.failure_detail.find("PRJ-8001"), std::string::npos);
}

// 干净输出（无 ERROR）永远是成功，且不带"容错"标记
TEST(FlowLogicPrj, CleanOutputIsPlainSuccess) {
    RunSnapshot snapshot;
    snapshot.run_name = "syn_1";
    snapshot.run_dir_names = SynCompleteNames();
    snapshot.project_name = "demo_project";
    const RunOutcome outcome = DecideRunOutcome(snapshot, "run complete\n");
    EXPECT_TRUE(outcome.accepted);
    EXPECT_FALSE(outcome.tolerated_prj_error);
}

// 只有 CRITICAL-WARNING、没有 ERROR：HasError 为真但 OnlyToleratedPrjErrors 为假
// （Python 的判定基于 "ERROR" 子串，CRITICAL-WARNING 不带 ERROR -> errors 为空 -> 不容错）
TEST(FlowLogicPrj, CriticalWarningIsNotTolerated) {
    RunSnapshot snapshot;
    snapshot.run_name = "phy_1";
    snapshot.run_dir_names = PhyCompleteNames();
    snapshot.project_name = "main";
    snapshot.project_bit_exists = true;

    const RunOutcome outcome = DecideRunOutcome(snapshot, "PRJ-8806 CRITICAL-WARNING: odd\n");
    EXPECT_FALSE(outcome.accepted);
}

TEST(FlowLogicPrj, ErrorLinesAndFirstError) {
    const std::string text = "INFO: ok\nPRJ-8806 ERROR: a\nHDL-1007 : b\nUSR-1 CRITICAL-WARNING: c\n";
    EXPECT_EQ(ErrorLines(text, 1), "PRJ-8806 ERROR: a");
    EXPECT_EQ(ErrorLines(text, 3), "PRJ-8806 ERROR: a / USR-1 CRITICAL-WARNING: c");
    EXPECT_EQ(FirstErrorLine(text), "PRJ-8806 ERROR: a");
    EXPECT_TRUE(FirstErrorLine("nothing").empty());
}

TEST(FlowLogicPrj, FailureDetailFormatting) {
    const std::string detail =
        BuildFailureDetail("PRJ-8001 ERROR: boom\n", Names({".opt_gate.error.f"}),
                           "run.log: ERROR something", "Y:/r/syn_1");
    EXPECT_NE(detail.find("PRJ-8001 ERROR: boom"), std::string::npos);
    EXPECT_NE(detail.find("failed run step: opt_gate"), std::string::npos);
    EXPECT_NE(detail.find("run.log: run.log: ERROR something"), std::string::npos);

    // 什么都没有时退化成 run dir
    EXPECT_EQ(BuildFailureDetail("", {}, "", "Y:/r/syn_1"), "run dir: Y:/r/syn_1");
}

// -----------------------------------------------------------------------------
// stall 看门狗
// -----------------------------------------------------------------------------

TEST(FlowLogicStall, NoActivityForTwoMinutesIsAStall) {
    // 完全没有活动痕迹 + 已经等了 130 s -> 子进程可能没起来
    const std::string reason = StallReason(0.0, 1130.0, 1000.0, 600.0);
    EXPECT_NE(reason.find("no run trace"), std::string::npos);
    // 只等了 100 s 还不算
    EXPECT_TRUE(StallReason(0.0, 1100.0, 1000.0, 600.0).empty());
}

TEST(FlowLogicStall, OldActivityIsAStall) {
    const std::string reason = StallReason(1000.0, 1700.0, 900.0, 600.0);
    EXPECT_NE(reason.find("no new artifacts for 700 s"), std::string::npos);
    // 在阈值内不算
    EXPECT_TRUE(StallReason(1000.0, 1500.0, 900.0, 600.0).empty());
}

// -----------------------------------------------------------------------------
// 日志与摘要
// -----------------------------------------------------------------------------

TEST(FlowLogicLog, FormatMatchesPython) {
    EXPECT_EQ(FormatLogLine(12.34, "syn", "hello"), "  12.3s [syn] hello");
    EXPECT_EQ(FormatLogLine(0.0, "", "x"), "   0.0s [-] x");
}

TEST(FlowLogicLog, TrimKeepsLastLines) {
    std::vector<std::string> log;
    for (int i = 0; i < 300; ++i) log.push_back("line " + std::to_string(i));
    const std::vector<std::string> trimmed = TrimLog(log, 200);
    ASSERT_EQ(trimmed.size(), 200u);
    EXPECT_EQ(trimmed.front(), "line 100");
    EXPECT_EQ(trimmed.back(), "line 299");
}

TEST(FlowLogicLog, ReportedRunLogPath) {
    EXPECT_EQ(ReportedRunLogPath("Run log file is D:/x/run.log.\n"),
              "D:/x/run.log");
    EXPECT_EQ(ReportedRunLogPath("nothing here"), "");
    EXPECT_EQ(ReportedRunLogPath("run log file is E:/y/run.log"), "E:/y/run.log");
}

TEST(FlowLogicSummary, PicksQorTimingAreaBit) {
    const jsonrpc::Json reports = jsonrpc::Json::parse(R"({
      "qor": {"metrics": [
        {"group":"Timing","key":"Setup WNS","value":"171"},
        {"group":"Timing","key":"Hold WNS","value":"23"}]},
      "timing": {"summary": {"SWNS": 0.171}},
      "area": {"rows": [{"resource":"#lut","used":6029,"total":19600}]},
      "artifacts": {"bit": {"exists": true, "size": 629628}}
    })");
    const std::string summary = SummaryFromReports(reports);
    EXPECT_EQ(summary,
              "flow complete: Setup WNS 171ps, Hold WNS 23ps, SWNS 0.171ns, "
              "LUT 6029/19600, bit 614 KB");
}

TEST(FlowLogicSummary, EmptyReportsStillReadable) {
    EXPECT_EQ(SummaryFromReports(jsonrpc::Json::object()), "flow complete");
}