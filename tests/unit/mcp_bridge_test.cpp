// =============================================================================
//  tests/unit/mcp_bridge_test.cpp —— MCP 工具实现层（用真实工程副本做夹具）
// =============================================================================
//  语料：tests/gate-corpus/corpus_alpha（corpus_alpha 的只读副本）
//    · 20 个 HDL 文件、25 个顶层端口、47 bit
//    · constrains/pin.adc 覆盖全部 47 bit；constrains/top.sdc 有 create_clock + derive_pll_clocks
//
//  这里测的是"工具返回什么形状/数值"，不碰真实 TD（device/cables 类用例不做），
//  因此可以在 ctest 里稳定跑。协议层（isError/截断）在 mcp_server_test.cpp。
//
//  ⚠ 夹具工程**只读**：本层不写 .fpgalsp/、不改任何文件（PYTHON-BUGS P-4 的处置）。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mcp/bridge.h"
#include "platform/encoding.h"
#include "project/al_project.h"
#include <filesystem>
#include <process.h>

namespace {
std::string LowerAsciiForTest(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}
}  // namespace
#include "mcp/mcp_server.h"

namespace fs = std::filesystem;

namespace {

using fpgalsp::mcp::Json;
using fpgalsp::mcp::ToolError;

constexpr const char* kCorpusRoot = FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha";

struct BridgeTest : public ::testing::Test {
    std::unique_ptr<fpgalsp::mcp::Bridge> bridge;

    void SetUp() override {
        fpgalsp::mcp::BridgeOptions options;
        options.root = kCorpusRoot;
        bridge = std::make_unique<fpgalsp::mcp::Bridge>(options);
    }

    void TearDown() override {
        if (bridge) bridge->Shutdown();
    }

    Json Call(const char* handler, Json arguments = Json::object()) {
        return bridge->Call(handler, arguments);
    }
};

// -------------------------------------------------------------- 工程解析

TEST_F(BridgeTest, ProjectsDiscoverdUnderCorpus) {
    const Json result = Call("project_list");
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_GE(result.at("projects").size(), 1u);
    const Json& project = result.at("projects").at(0);
    EXPECT_EQ(project.at("name").get<std::string>(), "demo_project");
    EXPECT_EQ(project.at("top").get<std::string>(), "top");
    EXPECT_EQ(project.at("hdl").get<int>(), 20);
    EXPECT_FALSE(result.at("selected").get<std::string>().empty())
        << "构造时应自动选中工程";
}

TEST_F(BridgeTest, ProjectInfoCountsAndMetadata) {
    const Json result = Call("project_info");
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("project").get<std::string>(), "demo_project");
    EXPECT_EQ(result.at("top").get<std::string>(), "top");
    EXPECT_EQ(result.at("family").get<std::string>(), "EF2");
    EXPECT_EQ(result.at("device").get<std::string>(), "EF2L45LG144B");
    EXPECT_EQ(result.at("files").at("hdl").get<int>(), 20);
    EXPECT_EQ(result.at("files").at("adc").get<int>(), 1);
    EXPECT_EQ(result.at("files").at("sdc").get<int>(), 1);
    // PRJ-6619：工程被拷走后 .al 记录的原始路径与当前目录不一致 -> 一条 warning
    EXPECT_GE(result.at("warnings").size(), 1u);
    EXPECT_TRUE(result.at("runs").is_array());
    EXPECT_TRUE(result.at("td").is_object());
}

TEST_F(BridgeTest, ProjectFilesFiltersByKind) {
    {
        Json arguments = Json::object();
        arguments["kind"] = "hdl";
        const Json result = Call("project_files", arguments);
        ASSERT_TRUE(result.at("ok").get<bool>());
        EXPECT_EQ(result.at("files").size(), 20u);
        for (const Json& file : result.at("files")) {
            const std::string kind = file.at("kind").get<std::string>();
            EXPECT_TRUE(kind == "verilog" || kind == "vhdl") << kind;
            EXPECT_TRUE(file.at("inBuild").get<bool>());
        }
    }
    {
        Json arguments = Json::object();
        arguments["kind"] = "adc";
        const Json result = Call("project_files", arguments);
        ASSERT_EQ(result.at("files").size(), 1u);
        EXPECT_NE(result.at("files").at(0).at("relative").get<std::string>().find("pin.adc"),
                  std::string::npos);
    }
}

TEST_F(BridgeTest, ProjectSelectionByNameAndByDirectory) {
    {
        Json arguments = Json::object();
        arguments["project"] = "demo_project";
        const Json result = Call("project_info", arguments);
        EXPECT_TRUE(result.at("ok").get<bool>());
    }
    {
        Json arguments = Json::object();
        arguments["project"] = kCorpusRoot;
        const Json result = Call("project_info", arguments);
        EXPECT_TRUE(result.at("ok").get<bool>());
    }
    {
        Json arguments = Json::object();
        arguments["project"] = "no-such-project";
        EXPECT_THROW(Call("project_info", arguments), ToolError);
    }
}

// -------------------------------------------------------------- 约束（我的模块）

TEST_F(BridgeTest, ConstraintsStatusScansTopPortsFromVerilog) {
    const Json result = Call("constraints_status");
    ASSERT_TRUE(result.at("ok").get<bool>());
    // 文本扫描必须得到 CorpusAlpha top.v 的 25 个端口（含注释里的 "output signal" 不被误认）
    EXPECT_EQ(result.at("ports").get<int>(), 25) << result.dump(1);
    // 端口来自 HDL 索引（与 server 的符号工具同一数据源）；索引不可用时才回落 text-scan
    EXPECT_EQ(result.at("portsSource").get<std::string>(), "hdl-index");
    EXPECT_EQ(result.at("top").get<std::string>(), "top");
    // pin.adc 覆盖全部 47 bit -> 没有 missing
    EXPECT_EQ(result.at("missingCount").get<int>(), 0) << result.at("missing").dump(1);
    EXPECT_EQ(result.at("constrainedPorts").get<int>(), 25);
    ASSERT_EQ(result.at("sdc").size(), 1u);
    EXPECT_EQ(result.at("sdc").at(0).at("createClock").get<int>(), 1);
    EXPECT_EQ(result.at("sdc").at(0).at("derivePll").get<int>(), 1);
    EXPECT_EQ(result.at("sdc").at(0).at("generatedClock").get<int>(), 0);
}

TEST_F(BridgeTest, ConstraintAuditReportsFullCoverage) {
    Json arguments = Json::object();
    arguments["include_device"] = false;  // 不拉 TD
    const Json result = Call("constraint_audit", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("totalBits").get<int>(), 47);
    EXPECT_EQ(result.at("constrainedBits").get<int>(), 47);
    EXPECT_DOUBLE_EQ(result.at("coveragePercent").get<double>(), 100.0);
    EXPECT_EQ(result.at("issueCount").get<int>(), 0) << result.at("issues").dump(1);
    EXPECT_FALSE(result.at("deviceLoaded").get<bool>());
    EXPECT_EQ(result.at("portsSource").get<std::string>(), "hdl-index");
    ASSERT_EQ(result.at("adcFiles").size(), 1u);
    EXPECT_TRUE(result.at("duplicatePins").empty());
    EXPECT_TRUE(result.at("duplicatePorts").empty());
}

TEST_F(BridgeTest, ConstraintAuditWithoutSdc) {
    Json arguments = Json::object();
    arguments["include_device"] = false;
    arguments["include_sdc"] = false;
    const Json result = Call("constraint_audit", arguments);
    EXPECT_TRUE(result.at("sdc").empty());
    EXPECT_EQ(result.at("totalBits").get<int>(), 47);
}

// -------------------------------------------------------------- 诊断

TEST_F(BridgeTest, DiagnosticsForConstraintSourcesIsCleanOnCorpus) {
    Json arguments = Json::object();
    arguments["sources"] = Json::array({"adc", "sdc"});
    const Json result = Call("diagnostics", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_TRUE(result.at("diagnostics").empty()) << result.at("diagnostics").dump(1);
    // 只请求了 adc/sdc：只有**请求了但做不到**的来源才进 unavailable
    EXPECT_TRUE(result.at("unavailable").empty()) << result.at("unavailable").dump(1);
}

TEST_F(BridgeTest, DiagnosticsReportsUnavailableTiers) {
    Json arguments = Json::object();
    arguments["sources"] = Json::array({"hdl", "verible"});
    const Json result = Call("diagnostics", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_TRUE(result.at("unavailable").contains("hdl"));
    EXPECT_TRUE(result.at("unavailable").contains("verible"));
    EXPECT_TRUE(result.at("diagnostics").empty());
}

// -------------------------------------------------------------- 文件 / 搜索

TEST_F(BridgeTest, ReadFileRangeAndMetadata) {
    // top.v 前 12 行是注释，module 头在 1-based 第 15 行
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["offset"] = 15;
    arguments["limit"] = 3;
    const Json result = Call("read_file", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("lineStart").get<int>(), 15);
    // Python: end = start + limit -> lineEnd 是返回的最后一行的 1-based 行号
    EXPECT_EQ(result.at("lineEnd").get<int>(), 17);
    EXPECT_GT(result.at("totalLines").get<int>(), 40);
    EXPECT_NE(result.at("text").get<std::string>().find("module top"), std::string::npos);
    EXPECT_EQ(result.at("relative").get<std::string>(), "src/top.v");
}

TEST_F(BridgeTest, ReadFileRejectsEscapingProject) {
    Json arguments = Json::object();
    arguments["file"] = "../../../CONTRACT.md";
    EXPECT_THROW(Call("read_file", arguments), ToolError);
}

TEST_F(BridgeTest, ReadFileMissingFileFails) {
    Json arguments = Json::object();
    arguments["file"] = "src/does_not_exist.v";
    EXPECT_THROW(Call("read_file", arguments), ToolError);
}

TEST_F(BridgeTest, ReadFileTruncatesByMaxChars) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["offset"] = 1;
    arguments["limit"] = 5000;
    arguments["max_chars"] = 1000;
    const Json result = Call("read_file", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("text").get<std::string>().size(), 1000u);
    EXPECT_TRUE(result.at("truncated").get<bool>());
}

TEST_F(BridgeTest, SearchTextFindsPortNamesWithOneBasedLines) {
    Json arguments = Json::object();
    arguments["query"] = "pwmG";
    arguments["limit"] = 20;
    const Json result = Call("search_text", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_GT(result.at("count").get<int>(), 0);
    EXPECT_GT(result.at("files").get<int>(), 0);
    for (const Json& match : result.at("matches")) {
        EXPECT_GE(match.at("line").get<int>(), 1) << "行号必须 1-based";
        EXPECT_GE(match.at("column").get<int>(), 1) << "列号必须 1-based";
    }
}

TEST_F(BridgeTest, SearchTextRegexCaseInsensitiveByDefault) {
    Json arguments = Json::object();
    arguments["query"] = "PWMG\\[[0-9]+\\]";
    arguments["regex"] = true;
    arguments["limit"] = 5;
    const Json result = Call("search_text", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_GT(result.at("count").get<int>(), 0) << "默认应大小写不敏感";

    arguments["case_sensitive"] = true;
    const Json strict = Call("search_text", arguments);
    EXPECT_EQ(strict.at("count").get<int>(), 0) << "区分大小写时 PWMG 不应命中 pwmG";
}

TEST_F(BridgeTest, SearchTextGlobFilter) {
    Json arguments = Json::object();
    arguments["query"] = "module";
    arguments["glob"] = "*.adc";
    arguments["limit"] = 10;
    const Json result = Call("search_text", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("count").get<int>(), 0) << "*.adc 里不会出现 module";
}

TEST_F(BridgeTest, SearchTextContextLines) {
    Json arguments = Json::object();
    arguments["query"] = "module top";
    arguments["context"] = 1;
    arguments["limit"] = 3;
    const Json result = Call("search_text", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    if (result.at("count").get<int>() > 0) {
        const Json& first = result.at("matches").at(0);
        ASSERT_TRUE(first.contains("context"));
        EXPECT_GE(first.at("context").size(), 2u);
    }
}

TEST_F(BridgeTest, SearchTextEmptyQueryFails) {
    Json arguments = Json::object();
    arguments["query"] = "";
    EXPECT_THROW(Call("search_text", arguments), ToolError);
}

// -------------------------------------------------------------- flow / TD / 报告

TEST_F(BridgeTest, FlowStatusIsIdleWithArtifacts) {
    const Json result = Call("flow_status");
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_TRUE(result.at("flow").is_object());
    EXPECT_FALSE(result.at("flow").at("running").get<bool>());
    EXPECT_EQ(result.at("flow").at("stage").get<std::string>(), "idle");
    EXPECT_TRUE(result.at("artifacts").is_object());
}

TEST_F(BridgeTest, FlowWaitReturnsImmediatelyWithoutActiveFlow) {
    const Json result = Call("flow_wait");
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_FALSE(result.at("running").get<bool>());
    EXPECT_EQ(result.at("note").get<std::string>(), "no active flow for this project");
}

TEST_F(BridgeTest, FlowLogWithoutRunDirectoryIsExplicitFailure) {
    const Json result = Call("flow_log");
    EXPECT_FALSE(result.at("ok").get<bool>()) << "没有 flow 目录时必须显式失败";
    EXPECT_EQ(result.at("error").get<std::string>(), "No flow run directory found");
    EXPECT_TRUE(result.at("files").is_array());
}

TEST_F(BridgeTest, FlowIssuesWithoutRunDirectoryStillReturnsShape) {
    const Json result = Call("flow_issues");
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("runId").get<std::string>(), "");
    EXPECT_TRUE(result.at("issues").is_array());
    EXPECT_EQ(result.at("counts").at("error").get<int>(), 0);
    EXPECT_TRUE(result.at("markers").is_array());
    EXPECT_TRUE(result.at("logs").is_array());
}

TEST_F(BridgeTest, TdInfoReturnsSummaryShape) {
    const Json result = Call("td_info");
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_TRUE(result.at("td").is_object());
    // 无论本机是否装了 TD，都必须有 ok 字段与（失败时的）error
    EXPECT_TRUE(result.at("td").contains("ok"));
    if (!result.at("td").at("ok").get<bool>()) {
        EXPECT_TRUE(result.at("td").contains("error"));
    } else {
        EXPECT_TRUE(result.at("td").contains("root"));
        EXPECT_TRUE(result.at("td").contains("alternatives"));
    }
}

TEST_F(BridgeTest, ProjectReportsShapeWithoutRuns) {
    const Json result = Call("project_reports");
    ASSERT_TRUE(result.contains("ok"));
    EXPECT_EQ(result.at("project").get<std::string>(), "demo_project");
    EXPECT_TRUE(result.at("files").is_object());
    // 语料没有 <name>_Runs 目录 -> ok=false 且带 error（不抛异常）
    if (!result.at("ok").get<bool>()) {
        EXPECT_TRUE(result.contains("error"));
    }
}

// -------------------------------------------------------------- 占位与错误

// -------------------------------------------------------------- 符号类（阶段二 A 批）

namespace {
Json CallWith(const std::function<Json()>& fn) { return fn(); }
}  // namespace

TEST_F(BridgeTest, DocumentSymbolsReturnsModuleTreeWithOneBasedRanges) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    const Json result = Call("document_symbols", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_FALSE(result.at("symbols").empty()) << "top.v 应该至少有一个模块符号";
    const Json& module = result.at("symbols").at(0);
    EXPECT_EQ(module.at("name").get<std::string>(), "top");
    EXPECT_EQ(module.at("kind").get<std::string>(), "module");
    EXPECT_GE(module.at("range").at("start").at("line").get<int>(), 1) << "MCP 位置必须 1-based";
    EXPECT_EQ(module.at("file").get<std::string>().find("top.v") != std::string::npos, true);
    EXPECT_EQ(module.at("relative").get<std::string>(), "src/top.v");
    ASSERT_TRUE(module.contains("children"));
    EXPECT_FALSE(module.at("children").empty()) << "模块下应有端口子符号";
}

TEST_F(BridgeTest, WorkspaceSymbolsFindsModulesByName) {
    Json arguments = Json::object();
    arguments["query"] = "pwm";
    arguments["limit"] = 50;
    const Json result = Call("workspace_symbols", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_FALSE(result.at("symbols").empty());
    for (const Json& symbol : result.at("symbols")) {
        EXPECT_FALSE(symbol.at("name").get<std::string>().empty());
        EXPECT_FALSE(symbol.at("kind").get<std::string>().empty());
        if (symbol.at("location").is_object()) {
            EXPECT_GE(symbol.at("location").at("line").get<int>(), 1);
            EXPECT_GE(symbol.at("location").at("column").get<int>(), 1);
        }
    }
}

TEST_F(BridgeTest, HoverOnPortDeclarationDescribesDirectionAndWidth) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 15;    // `module top(` 的 1-based 行
    arguments["column"] = 8;   // 指向模块名
    const Json result = Call("hover", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_FALSE(result.at("hover").get<std::string>().empty())
        << "模块名上应该有 hover 内容";
}

TEST_F(BridgeTest, DefinitionOfInstanceResolvesToItsModuleFile) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 66;   // pll_gen 例化（1-based）
    arguments["column"] = 2;
    const Json result = Call("definition", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_FALSE(result.at("locations").empty())
        << "pll_gen 应该有定义（al_ip/pll_gen.v）";
    const std::string file = result.at("locations").at(0).at("file").get<std::string>();
    EXPECT_NE(file.find("pll_gen"), std::string::npos) << file;
}

TEST_F(BridgeTest, ReferencesOfAModuleNameAreReported) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 15;
    arguments["column"] = 8;
    const Json result = Call("references", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_TRUE(result.at("references").is_array());
    for (const Json& reference : result.at("references")) {
        EXPECT_GE(reference.at("line").get<int>(), 1);
    }
}

TEST_F(BridgeTest, ProjectStructureRootIsTheTopModule) {
    Json arguments = Json::object();
    arguments["project"] = kCorpusRoot;
    const Json result = Call("project_structure", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    const Json& root = result.at("root");
    EXPECT_EQ(root.at("module").get<std::string>(), "top");
    EXPECT_TRUE(root.at("defined").get<bool>());
    EXPECT_EQ(root.at("portCount").get<int>(), 25);
    EXPECT_GE(root.at("line").get<int>(), 1);
    ASSERT_TRUE(root.contains("children"));
    EXPECT_FALSE(root.at("children").empty()) << "top 应该例化了子模块";
    // 子节点同样 1-based
    const Json& child = root.at("children").at(0);
    EXPECT_GE(child.at("line").get<int>(), 1);
    EXPECT_FALSE(result.at("modulesSeen").get<int>() == 0);
}

TEST_F(BridgeTest, ProjectStructureWithPortsAttachesPortLists) {
    Json arguments = Json::object();
    arguments["include_ports"] = true;
    arguments["max_depth"] = 1;
    const Json result = Call("project_structure", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    const Json& root = result.at("root");
    ASSERT_TRUE(root.contains("ports"));
    EXPECT_EQ(root.at("ports").size(), 25u);
    EXPECT_TRUE(root.contains("parameters"));
}

TEST_F(BridgeTest, ModuleInfoReportsPortsAndInstantiationSites) {
    Json arguments = Json::object();
    arguments["module"] = "pwm_gen";
    const Json result = Call("module_info", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("module").get<std::string>(), "pwm_gen");
    EXPECT_NE(result.at("file").get<std::string>().find("pwm_gen.v"), std::string::npos);
    EXPECT_GE(result.at("line").get<int>(), 1);
    ASSERT_FALSE(result.at("ports").empty());
    const Json& port = result.at("ports").at(0);
    EXPECT_TRUE(port.contains("direction"));
    EXPECT_TRUE(port.contains("width"));
    EXPECT_TRUE(result.at("instances").is_array());
}

TEST_F(BridgeTest, ModuleInfoUnknownModuleFails) {
    Json arguments = Json::object();
    arguments["module"] = "no_such_module";
    EXPECT_THROW(Call("module_info", arguments), ToolError);
}

TEST_F(BridgeTest, DuplicateModulesReturnsSortedGroups) {
    const Json result = Call("duplicate_modules");
    ASSERT_TRUE(result.at("ok").get<bool>());
    ASSERT_TRUE(result.at("duplicates").is_array());
    // 语料里没有重名模块 -> 空数组；字段形状仍然要对
    for (const Json& entry : result.at("duplicates")) {
        EXPECT_TRUE(entry.contains("name"));
        ASSERT_TRUE(entry.at("definitions").is_array());
        EXPECT_GE(entry.at("definitions").size(), 2u);
    }
}

TEST_F(BridgeTest, SearchHierarchyFindsModulesAndInstances) {
    {
        Json arguments = Json::object();
        arguments["query"] = "pwm";
        arguments["kind"] = "module";
        const Json result = Call("search_hierarchy", arguments);
        ASSERT_TRUE(result.at("ok").get<bool>());
        ASSERT_FALSE(result.at("matches").empty());
        for (const Json& match : result.at("matches")) {
            EXPECT_EQ(match.at("kind").get<std::string>(), "module");
            // Python 原样透传 0-based line；这里必须保持一致（不要"顺手" +1）
            EXPECT_GE(match.at("line").get<int>(), 0);
        }
    }
    {
        Json arguments = Json::object();
        arguments["query"] = "pll";
        arguments["kind"] = "instance";
        const Json result = Call("search_hierarchy", arguments);
        ASSERT_TRUE(result.at("ok").get<bool>());
        for (const Json& match : result.at("matches")) {
            EXPECT_EQ(match.at("kind").get<std::string>(), "instance");
            EXPECT_TRUE(match.contains("parent"));
        }
    }
}

TEST_F(BridgeTest, SearchHierarchyEmptyQueryReturnsNothing) {
    Json arguments = Json::object();
    arguments["query"] = "";
    const Json result = Call("search_hierarchy", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_TRUE(result.at("matches").empty());
}

TEST_F(BridgeTest, CallHierarchyForModuleListsCallers) {
    Json arguments = Json::object();
    arguments["module"] = "pwm_gen";
    arguments["direction"] = "incoming";
    const Json result = Call("call_hierarchy", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("module").get<std::string>(), "pwm_gen");
    ASSERT_TRUE(result.at("definition").is_object());
    EXPECT_GE(result.at("definition").at("line").get<int>(), 1);
    EXPECT_FALSE(result.at("incoming").empty()) << "pwm_gen 被 top 例化，应有 caller";
    const Json& caller = result.at("incoming").at(0);
    EXPECT_EQ(caller.at("module").get<std::string>(), "pwm_gen");
    EXPECT_GE(caller.at("line").get<int>(), 1);
}

TEST_F(BridgeTest, CallHierarchyUnknownModuleIsEmptyNotError) {
    Json arguments = Json::object();
    arguments["module"] = "no_such_module";
    const Json result = Call("call_hierarchy", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("module").get<std::string>(), "");
    EXPECT_TRUE(result.at("incoming").empty());
    EXPECT_TRUE(result.at("outgoing").empty());
}

TEST_F(BridgeTest, SymbolPathOfModuleName) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 15;
    arguments["column"] = 8;
    const Json result = Call("symbol_path", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_FALSE(result.at("name").get<std::string>().empty());
    EXPECT_TRUE(result.contains("instancePath"));
    EXPECT_TRUE(result.contains("path"));
}

TEST_F(BridgeTest, CompletionReturnsItemsForAnInstantiationPosition) {
    // top.v 的模块体内部（例化区域）：应能拿到模块名补全
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 70;
    arguments["column"] = 1;
    arguments["limit"] = 20;
    const Json result = Call("completion", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_TRUE(result.at("items").is_array());
    for (const Json& item : result.at("items")) {
        EXPECT_TRUE(item.contains("label"));
        EXPECT_TRUE(item.contains("kind"));
        EXPECT_TRUE(item.contains("documentation"));
    }
}

TEST_F(BridgeTest, CompletionPrefixFilterAndSnippetToggle) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["line"] = 70;
    arguments["column"] = 1;
    arguments["prefix"] = "pwm";
    arguments["limit"] = 50;
    const Json filtered = Call("completion", arguments);
    ASSERT_TRUE(filtered.at("ok").get<bool>());
    for (const Json& item : filtered.at("items")) {
        const std::string label = item.at("label").get<std::string>();
        EXPECT_EQ(LowerAsciiForTest(label).rfind("pwm", 0), 0u) << label;
    }
    arguments["include_snippets"] = false;
    const Json no_snippets = Call("completion", arguments);
    for (const Json& item : no_snippets.at("items")) {
        // insertTextFormat 可能是 null（普通补全项没有该字段）——不能直接 value<int>
        if (item.contains("insertTextFormat") && item["insertTextFormat"].is_number_integer()) {
            EXPECT_NE(item["insertTextFormat"].get<int>(), 2)
                << "include_snippets=false 时不该有 snippet";
        }
    }
}

TEST_F(BridgeTest, InstantiationTemplateUsesPortNames) {
    Json arguments = Json::object();
    arguments["module"] = "pwm_gen";
    const Json result = Call("instantiation", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_EQ(result.at("module").get<std::string>(), "pwm_gen");
    EXPECT_EQ(result.at("instance").get<std::string>(), "u_pwm_gen");
    const std::string text = result.at("text").get<std::string>();
    // pwm_gen 带参数（DEADTIME），所以是 `pwm_gen #( ... ) u_pwm_gen (` 形式
    EXPECT_NE(text.find("pwm_gen"), std::string::npos) << text;
    EXPECT_NE(text.find("u_pwm_gen ("), std::string::npos) << text;
    EXPECT_NE(text.find(".clk"), std::string::npos) << text;
    EXPECT_NE(text.find(");"), std::string::npos) << text;
    EXPECT_FALSE(result.at("ports").empty());
}

TEST_F(BridgeTest, InstantiationHonoursInstanceNameAndIndent) {
    Json arguments = Json::object();
    arguments["module"] = "pwm_gen";
    arguments["instance_name"] = "u_my_pwm";
    arguments["indent"] = "  ";
    const Json result = Call("instantiation", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("instance").get<std::string>(), "u_my_pwm");
    const std::string text = result.at("text").get<std::string>();
    EXPECT_EQ(text.find("u_pwm_gen"), std::string::npos) << "默认实例名应被替换掉";
    EXPECT_NE(text.find("u_my_pwm"), std::string::npos);
    EXPECT_NE(text.find("\n      ."), std::string::npos) << "缩进应为 indent + 4 空格";
}

TEST_F(BridgeTest, InstantiationUnknownModuleFails) {
    Json arguments = Json::object();
    arguments["module"] = "no_such_module";
    EXPECT_THROW(Call("instantiation", arguments), ToolError);
}
TEST_F(BridgeTest, OnlyEditingToolsRemainExplicitPlaceholders) {
    // completion / instantiation 已实现（features::Completion + 纯文本模板）；
    // 只剩"需要编辑构造层"的两个
    for (const char* handler : {"code_actions", "rename_edits"}) {
        Json arguments = Json::object();
        arguments["file"] = "src/top.v";
        arguments["module"] = "top";
        const Json result = Call(handler, arguments);
        EXPECT_FALSE(result.at("ok").get<bool>()) << handler << " 不该假装成功";
        EXPECT_EQ(result.at("error").get<std::string>().rfind("not implemented:", 0), 0u)
            << handler << ": " << result.at("error").get<std::string>();
    }
}

TEST_F(BridgeTest, FlowCancelWithoutRunnerIsIdempotent) {
    // 没有 flow 在跑时必须返回 {ok:true, note:"not running"}（客户端契约），而不是报错
    const Json result = Call("flow_cancel");
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("note").get<std::string>(), "not running");
}

TEST_F(BridgeTest, FlowStatusShapeIsStableWithoutRunner) {
    const Json result = Call("flow_status");
    ASSERT_TRUE(result.at("ok").get<bool>());
    const Json& flow = result.at("flow");
    EXPECT_FALSE(flow.at("running").get<bool>());
    EXPECT_EQ(flow.at("stage").get<std::string>(), "idle");
    EXPECT_TRUE(flow.contains("lastMessage"));
    EXPECT_TRUE(flow.contains("log"));
}

TEST_F(BridgeTest, FlowWaitReturnsImmediatelyWithoutRunner) {
    Json arguments = Json::object();
    arguments["timeout"] = 1;
    arguments["interval"] = 1;
    const auto started = std::chrono::steady_clock::now();
    const Json result = Call("flow_wait", arguments);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_FALSE(result.at("running").get<bool>());
    EXPECT_LT(elapsed, 0.5) << "没有 flow 时必须立即返回，不能等 timeout";
}

// 说明：`strategy_compare` / `cables` **不在单测里调用**。
// 前者会真的跑多次综合（实测 40s+），并且会往工程目录写 `.fpgalsp/compare/`
// —— 既慢又污染夹具（本轮真实踩过：gate-corpus 出现了 .fpgalsp，已清理）。
// 它们的真实行为由 e2e 在 **临时副本** 上验证（tests/e2e/mcp_smoke.py）。
// -------------------------------------------------------------- 写工程（临时副本）

namespace {

// 把语料复制到临时目录再改——**绝不动 tests/gate-corpus**
class TempProjectCopy {
  public:
    TempProjectCopy() {
        std::error_code ec;
        root = fs::temp_directory_path(ec) / ("fpgalsp-mcp-write-" + std::to_string(static_cast<long long>(_getpid())));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        const fs::path source(kCorpusRoot);
        for (const fs::directory_entry& entry : fs::directory_iterator(source, ec)) {
            const std::string name = entry.path().filename().string();
            if (name == "demo_project_Runs" || name == ".fpgalsp") continue;
            fs::copy(entry.path(), root / entry.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        al = root / "demo_project.al";
    }
    ~TempProjectCopy() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string Root() const { return root.generic_string(); }
    std::string Al() const { return al.generic_string(); }

  private:
    fs::path root;
    fs::path al;
};

}  // namespace

TEST_F(BridgeTest, ProjectRenameRewritesOnlyTheNameOnATempCopy) {
    TempProjectCopy copy;
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);

    const auto before = fpgalsp::project::ParseAl(copy.Al());
    Json arguments = Json::object();
    arguments["name"] = "renamed_prj";
    const Json result = bridge.Call("project_rename", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_TRUE(result.at("changed").get<bool>());

    const auto after = fpgalsp::project::ParseAl(copy.Al());
    EXPECT_EQ(after.name, "renamed_prj");
    // 只允许名字变：文件列表与 top 必须原样
    EXPECT_EQ(after.files.size(), before.files.size());
    EXPECT_EQ(after.top, before.top);
    EXPECT_EQ(after.device, before.device);
}

TEST_F(BridgeTest, ProjectRenameRejectsInvalidNames) {
    TempProjectCopy copy;
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);
    for (const char* name : {"", "  ", "bad<name>", "bad&name"}) {
        Json arguments = Json::object();
        arguments["name"] = name;
        EXPECT_THROW(bridge.Call("project_rename", arguments), ToolError) << name;
    }
}

TEST_F(BridgeTest, ProjectAddSourceAppendsToSourceFiles) {
    TempProjectCopy copy;
    // 在副本里造一个新的源文件
    const fs::path new_source = fs::path(copy.Root()) / "src" / "brand_new.v";
    ASSERT_TRUE(fpgalsp::platform::write_file_utf8(
        new_source.string(), "module brand_new(input a); endmodule\n"));

    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);

    const auto before = fpgalsp::project::ParseAl(copy.Al());
    Json arguments = Json::object();
    arguments["file"] = "src/brand_new.v";
    const Json result = bridge.Call("project_add_source", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_TRUE(result.at("added").get<bool>());
    EXPECT_EQ(result.at("rel").get<std::string>(), "src/brand_new.v");
    EXPECT_GE(result.at("compileOrder").get<int>(), 1);

    const auto after = fpgalsp::project::ParseAl(copy.Al());
    EXPECT_EQ(after.files.size(), before.files.size() + 1);
    bool found = false;
    for (const auto& item : after.files) {
        if (item.rel.find("brand_new.v") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "新文件没有进入 .al";
}

TEST_F(BridgeTest, ProjectAddSourceIsIdempotentForExistingFiles) {
    TempProjectCopy copy;
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);

    Json arguments = Json::object();
    arguments["file"] = "src/top.v";  // 已在 .al 里
    const Json result = bridge.Call("project_add_source", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_FALSE(result.at("added").get<bool>());
    EXPECT_TRUE(result.at("compileOrder").is_null());
}

TEST_F(BridgeTest, ProjectAddSourceRejectsPathOutsideProject) {
    TempProjectCopy copy;
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);
    Json arguments = Json::object();
    arguments["file"] = "../../../../CONTRACT.md";
    EXPECT_THROW(bridge.Call("project_add_source", arguments), ToolError);
}
TEST_F(BridgeTest, RemainingActionsAreExplicitPlaceholders) {
    for (const char* handler : {"project_update", "analysis_run", "format"}) {
        const Json result = Call(handler);
        EXPECT_FALSE(result.at("ok").get<bool>()) << handler;
        EXPECT_EQ(result.at("error").get<std::string>().rfind("not implemented:", 0), 0u)
            << handler << ": " << result.at("error").get<std::string>();
    }
}

TEST_F(BridgeTest, UnknownHandlerThrows) {
    EXPECT_THROW(Call("no_such_handler"), ToolError);
}

TEST_F(BridgeTest, BridgeRejectsMissingRoot) {
    fpgalsp::mcp::BridgeOptions options;
    options.root = "Y:/definitely/not/a/real/directory";
    EXPECT_THROW(fpgalsp::mcp::Bridge bridge(options), ToolError);
}

TEST_F(BridgeTest, ProjectListTruncatesSilentlyWithoutFlag) {
    // ⚠ P-22（见 PYTHON-BUGS.md）：Python 的 project_list 把结果静默截到 100 条，
    //   且**没有** truncated / total 字段；而同一个 bridge 的 project_files 是带
    //   `truncated` 的。agent 无法区分"只有 1 个工程"和"被截断了"。
    //   忠实复现（C++ 同样 100 条 + 无标志）；修掉 Python 缺陷后本测试应改为断言有标志。
    Json arguments = Json::object();
    arguments["root"] = kCorpusRoot;
    const Json result = Call("project_list", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_FALSE(result.contains("truncated"));
    EXPECT_FALSE(result.contains("total"));

    Json files_arguments = Json::object();
    files_arguments["kind"] = "hdl";
    EXPECT_TRUE(Call("project_files", files_arguments).contains("truncated"));
}

}  // namespace