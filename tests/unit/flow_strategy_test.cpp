// =============================================================================
//  tests/unit/flow_strategy_test.cpp —— 策略对比里的纯逻辑
// =============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "flow/strategy.h"
#include "platform/encoding.h"

namespace {

namespace fs = std::filesystem;
// Json 在 fpgalsp::jsonrpc 里；这些用例大量用到，起个别名
namespace jsonrpc = fpgalsp::jsonrpc;
using Json = fpgalsp::jsonrpc::Json;
using namespace fpgalsp::flow;  // NOLINT(google-build-using-namespace)

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
        path_ += "fpgalsp_strategy_" + tag;
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

void Touch(const std::string& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(path, content);
}

}  // namespace

// -----------------------------------------------------------------------------
// 策略文件选择
// -----------------------------------------------------------------------------

TEST(FlowStrategy, FilePerFamily) {
    EXPECT_EQ(StrategyFileForFamily("PH1"), "official_strategy_ph1.json");
    EXPECT_EQ(StrategyFileForFamily("ph1p"), "official_strategy_ph1.json");
    EXPECT_EQ(StrategyFileForFamily("PH2"), "official_strategy_ph2.json");
    EXPECT_EQ(StrategyFileForFamily("PH2A"), "official_strategy_ph2.json");
    // EF2 / EG4 等 CPLD 族走 cpld 文件
    EXPECT_EQ(StrategyFileForFamily("EF2"), "official_strategy_cpld.json");
    EXPECT_EQ(StrategyFileForFamily("EG4"), "official_strategy_cpld.json");
    EXPECT_EQ(StrategyFileForFamily(""), "official_strategy_cpld.json");
}

TEST(FlowStrategy, AvailableStrategiesReadsJsonKeys) {
    ScopedTempDir temp("avail");
    const std::string root = temp.path() + "/td";
    Touch(root + "/doc/strategies/official_strategy_cpld.json",
          R"({"TimingHigh":{}, "CongestionHigh":{}, "AreaOptimize":{}})");
    const std::vector<std::string> names = AvailableStrategies(root, "EF2");
    ASSERT_EQ(names.size(), 3u);
    // 排序后返回
    EXPECT_EQ(names[0], "AreaOptimize");
    EXPECT_EQ(names[1], "CongestionHigh");
    EXPECT_EQ(names[2], "TimingHigh");
}

TEST(FlowStrategy, AvailableStrategiesToleratesMissingOrBadFiles) {
    ScopedTempDir temp("avail_bad");
    const std::string root = temp.path() + "/td";
    EXPECT_TRUE(AvailableStrategies(root, "EF2").empty());
    Touch(root + "/doc/strategies/official_strategy_ph1.json", "not json at all");
    EXPECT_TRUE(AvailableStrategies(root, "PH1").empty());
    EXPECT_TRUE(AvailableStrategies("", "PH1").empty());
}

// -----------------------------------------------------------------------------
// 选策略
// -----------------------------------------------------------------------------

TEST(FlowStrategy, ChooseUsesRequestWhenProvided) {
    std::vector<std::string> unknown;
    const std::vector<std::string> wanted =
        ChooseStrategies({"TimingHigh"}, {"TimingHigh", "AreaOptimize"}, &unknown);
    EXPECT_EQ(wanted, std::vector<std::string>{"TimingHigh"});
    EXPECT_TRUE(unknown.empty());
}

TEST(FlowStrategy, ChooseFallsBackToDefaultsThatExist) {
    std::vector<std::string> unknown;
    const std::vector<std::string> wanted =
        ChooseStrategies({}, {"AreaOptimize", "TimingHigh", "Custom"}, &unknown);
    // DEFAULT_STRATEGIES 里存在的是 TimingHigh 与 AreaOptimize
    ASSERT_EQ(wanted.size(), 2u);
    EXPECT_EQ(wanted[0], "TimingHigh");
    EXPECT_EQ(wanted[1], "AreaOptimize");
    EXPECT_TRUE(unknown.empty());
}

TEST(FlowStrategy, ChooseFallsBackToFirstTwoWhenNoDefaultsExist) {
    std::vector<std::string> unknown;
    const std::vector<std::string> wanted =
        ChooseStrategies({}, {"Alpha", "Beta", "Gamma"}, &unknown);
    ASSERT_EQ(wanted.size(), 2u);
    EXPECT_EQ(wanted[0], "Alpha");
    EXPECT_EQ(wanted[1], "Beta");
}

TEST(FlowStrategy, ChooseReportsUnknownNames) {
    std::vector<std::string> unknown;
    const std::vector<std::string> wanted =
        ChooseStrategies({"Nope", "TimingHigh"}, {"TimingHigh"}, &unknown);
    EXPECT_EQ(wanted.size(), 2u);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0], "Nope");
}

// -----------------------------------------------------------------------------
// QoR 数字提取（EF2/PH1 的列名差异）
// -----------------------------------------------------------------------------

TEST(FlowStrategy, QorNumbersFromPh1Report) {
    const jsonrpc::Json qor = jsonrpc::Json::parse(R"({
      "metrics": [
        {"group":"Timing","key":"Setup WNS","value":"171"},
        {"group":"Timing","key":"Setup TNS","value":"0"},
        {"group":"Timing","key":"Hold WNS","value":"23"},
        {"group":"Utilization","key":"#registers","value":"7588"},
        {"group":"Utilization","key":"#slices","value":"6029"},
        {"group":"Utilization","key":"slices percentage","value":"61.52%"}
      ]})");
    const QorNumbers numbers = QorNumbersFromReport(qor);
    ASSERT_TRUE(numbers.setup_wns_ps.has_value());
    EXPECT_EQ(*numbers.setup_wns_ps, 171);
    ASSERT_TRUE(numbers.setup_tns_ps.has_value());
    EXPECT_EQ(*numbers.setup_tns_ps, 0);
    ASSERT_TRUE(numbers.hold_wns_ps.has_value());
    EXPECT_EQ(*numbers.hold_wns_ps, 23);
    ASSERT_TRUE(numbers.registers.has_value());
    EXPECT_EQ(*numbers.registers, 7588);
    ASSERT_TRUE(numbers.slices.has_value());
    EXPECT_EQ(*numbers.slices, 6029);
    EXPECT_EQ(numbers.slice_pct, "61.52%");
    // PH1 报告里没有 #luts / #lut
    EXPECT_FALSE(numbers.luts.has_value());
}

TEST(FlowStrategy, QorNumbersPrefersLutsOverLut) {
    const jsonrpc::Json qor = jsonrpc::Json::parse(R"({
      "metrics": [
        {"group":"Utilization","key":"#luts","value":"4923"},
        {"group":"Utilization","key":"#lut","value":"9999"}
      ]})");
    const QorNumbers numbers = QorNumbersFromReport(qor);
    ASSERT_TRUE(numbers.luts.has_value());
    EXPECT_EQ(*numbers.luts, 4923);
}

TEST(FlowStrategy, QorNumbersFallsBackToLut) {
    const jsonrpc::Json qor = jsonrpc::Json::parse(R"({
      "metrics": [{"group":"Utilization","key":"#lut","value":"7680"}]})");
    const QorNumbers numbers = QorNumbersFromReport(qor);
    ASSERT_TRUE(numbers.luts.has_value());
    EXPECT_EQ(*numbers.luts, 7680);
}

TEST(FlowStrategy, QorNumbersHandlesThousandSeparators) {
    const jsonrpc::Json qor = jsonrpc::Json::parse(R"({
      "metrics": [{"group":"Timing","key":"Setup WNS","value":"1,234"}]})");
    const QorNumbers numbers = QorNumbersFromReport(qor);
    ASSERT_TRUE(numbers.setup_wns_ps.has_value());
    EXPECT_EQ(*numbers.setup_wns_ps, 1234);
}

TEST(FlowStrategy, QorNumbersToleratesEmptyOrOddInput) {
    const QorNumbers empty = QorNumbersFromReport(jsonrpc::Json::object());
    EXPECT_FALSE(empty.setup_wns_ps.has_value());
    EXPECT_FALSE(empty.luts.has_value());

    const jsonrpc::Json odd = jsonrpc::Json::parse(R"({"metrics": [{"key":"Setup WNS"}]})");
    EXPECT_FALSE(QorNumbersFromReport(odd).setup_wns_ps.has_value());
}

TEST(FlowStrategy, QorNumbersToJsonShape) {
    QorNumbers numbers;
    numbers.setup_wns_ps = 171;
    numbers.slice_pct = "61.52%";
    const jsonrpc::Json json = numbers.ToJson();
    EXPECT_EQ(json["setup_wns_ps"], 171);
    EXPECT_TRUE(json["hold_wns_ps"].is_null());
    EXPECT_EQ(json["slice_pct"], "61.52%");
}

// -----------------------------------------------------------------------------
// 约束集
// -----------------------------------------------------------------------------

TEST(FlowStrategy, ConstraintSetFromAl) {
    ScopedTempDir temp("constr");
    const std::string al = temp.path() + "/proj.al";
    Touch(al, R"(<Project><FileSet Name="my_constraints" Type="ConstrainFiles"/></Project>)");
    EXPECT_EQ(ConstraintSetOf(al), "my_constraints");

    const std::string plain = temp.path() + "/plain.al";
    Touch(plain, "<Project></Project>");
    EXPECT_EQ(ConstraintSetOf(plain), "constraint_1");

    EXPECT_EQ(ConstraintSetOf(temp.path() + "/missing.al"), "constraint_1");
}

TEST(FlowStrategy, CompareWithoutTdRootReportsNoStrategies) {
    fpgalsp::project::Project project;
    project.dir = "Y:/definitely/not/here";
    project.name = "x";
    project.family = "EF2";
    const jsonrpc::Json result =
        CompareStrategies(project, fpgalsp::td::TdChoice{}, {}, CompareOptions{});
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_NE(result.value("error", std::string()).find("no strategies available"),
              std::string::npos);
}
