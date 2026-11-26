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
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mcp/bridge.h"
#include "platform/encoding.h"
#include "project/al_project.h"
#include "td/td_device.h"
#include <filesystem>
#include <fstream>
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

// 语料需自备：本仓库不附带工程语料。默认取源码树下的 tests/gate-corpus/corpus_alpha；
// 设置环境变量 FPGA_LSP_TEST_PROJECT 可指向任意自备工程（不改代码即可换语料）。
std::string CorpusRoot() {
    const char* from_env = std::getenv("FPGA_LSP_TEST_PROJECT");
    if (from_env != nullptr && *from_env != '\0') return from_env;
    return FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha";
}
const std::string kCorpusRoot = CorpusRoot();

struct BridgeTest : public ::testing::Test {
    std::unique_ptr<fpgalsp::mcp::Bridge> bridge;

    void SetUp() override {
        if (!std::filesystem::exists(kCorpusRoot)) {
            GTEST_SKIP() << "corpus missing (release tree): " << kCorpusRoot;
        }
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
    // D-2：Python 的 constraints_status 没有 portsSource（C++ 早期追加、已移除）
    EXPECT_FALSE(result.contains("portsSource")) << result.dump(1);
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
    // D-2：Python 的 constraint_audit 没有 device / portsSource（C++ 早期追加、已移除）
    EXPECT_FALSE(result.contains("portsSource")) << result.dump(1);
    EXPECT_FALSE(result.contains("device")) << result.dump(1);
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
    // D-2：键集合必须与 Python 的 {ok, project, file, diagnostics, truncated} 完全一致
    // （C++ 早期追加的 portsSource / unavailable 已移除）
    std::set<std::string> keys;
    for (auto it = result.begin(); it != result.end(); ++it) keys.insert(it.key());
    EXPECT_EQ(keys, (std::set<std::string>{"ok", "project", "file", "diagnostics", "truncated"}))
        << result.dump(1);
}

TEST_F(BridgeTest, DiagnosticsForUnimplementedTiersStaysSilentAndPythonShaped) {
    // hdl / verible 两个来源在 MCP 层不可用；Python 用空数组 + 无附加键表达同一件事。
    Json arguments = Json::object();
    arguments["sources"] = Json::array({"hdl", "verible"});
    const Json result = Call("diagnostics", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>());
    EXPECT_TRUE(result.at("diagnostics").empty());
    EXPECT_FALSE(result.contains("unavailable")) << result.dump(1);
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

TEST_F(BridgeTest, DocumentSymbolsRejectsAbsoluteFileOutsideRoot) {
    // SEC-02：ResolveFile 在边界判定这一层统一拦住越界读（11 个只读工具共用），
    // 绝对路径不再"原样通过"。用例选**真实存在**的工程外文件，否则
    // "File not found" 也会抛 ToolError，测不出包含校验是否真的生效。
    Json arguments = Json::object();
    arguments["file"] = std::string(FPGA_LSP_SOURCE_DIR) + "/CMakeLists.txt";
    EXPECT_THROW(Call("document_symbols", arguments), ToolError);
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
TEST_F(BridgeTest, FormerPlaceholdersAreGone) {
    // B6/B6c 之后：format / code_actions / rename_edits / analysis_run 都不再返回
    // `not implemented:`（project_update 会起真实 TD，单元测试不跑，见探针）。
    struct Probe {
        const char* handler;
        Json arguments;
    };
    std::vector<Probe> probes;
    {
        Json arguments = Json::object();
        arguments["text"] = "module m(input a);\nendmodule\n";
        probes.push_back({"format", arguments});
    }
    {
        Json arguments = Json::object();
        arguments["file"] = "src/top.v";
        arguments["start_line"] = 15;
        arguments["start_column"] = 8;
        probes.push_back({"code_actions", arguments});
    }
    {
        Json arguments = Json::object();
        arguments["file"] = "src/trans/trans.v";
        arguments["line"] = 27;
        arguments["column"] = 8;
        arguments["new_name"] = "trans_x";
        probes.push_back({"rename_edits", arguments});
    }
    probes.push_back({"analysis_run", Json::object()});

    for (const Probe& probe : probes) {
        Json result;
        try {
            result = Call(probe.handler, probe.arguments);
        } catch (const ToolError& error) {
            ADD_FAILURE() << probe.handler << " threw: " << error.what();
            continue;
        }
        const std::string error_text =
            result.contains("error") && result["error"].is_string()
                ? result["error"].get<std::string>()
                : std::string();
        EXPECT_EQ(error_text.rfind("not implemented:", 0), std::string::npos)
            << probe.handler << ": " << error_text;
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
// -------------------------------------------------------------- B6c：code actions / rename

TEST_F(BridgeTest, CodeActionsReturnInstantiationRefactor) {
    // corpus top.v：module 头在 1-based 第 15 行，模块名从第 8 列开始
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["start_line"] = 15;
    arguments["start_column"] = 8;
    const Json result = Call("code_actions", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    ASSERT_TRUE(result.at("actions").is_array()) << result.dump(1);
    ASSERT_FALSE(result.at("actions").empty()) << result.dump(1);
    const Json& first = result.at("actions").at(0);
    EXPECT_NE(first.at("title").get<std::string>().find("Insert instantiation of"),
              std::string::npos) << first.dump(1);
    EXPECT_EQ(first.at("kind").get<std::string>(), "refactor");
    EXPECT_TRUE(first.at("isPreferred").get<bool>());
    // 未请求 edits 时不应带 edits 字段（与 Python 一致）
    EXPECT_FALSE(first.contains("edits")) << first.dump(1);
}

TEST_F(BridgeTest, CodeActionsIncludeEditsWhenAsked) {
    Json arguments = Json::object();
    arguments["file"] = "src/top.v";
    arguments["start_line"] = 15;
    arguments["start_column"] = 8;
    arguments["include_edits"] = true;
    const Json result = Call("code_actions", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    ASSERT_FALSE(result.at("actions").empty()) << result.dump(1);
    const Json& first = result.at("actions").at(0);
    ASSERT_TRUE(first.contains("edits")) << first.dump(1);
    EXPECT_TRUE(first.at("edits").is_object());
    EXPECT_FALSE(first.at("edits").empty());
    // key 必须是本地路径（Python: uri_to_path(uri)），不是 file:// URI
    for (auto it = first.at("edits").begin(); it != first.at("edits").end(); ++it) {
        EXPECT_EQ(it.key().rfind("file://", 0), std::string::npos) << it.key();
        EXPECT_NE(it.key().find("top.v"), std::string::npos) << it.key();
    }
}

TEST_F(BridgeTest, RenameEditsUseOneBasedRanges) {
    // trans.v 的 module 名（1-based line 27, column 8）
    Json arguments = Json::object();
    arguments["file"] = "src/trans/trans.v";
    arguments["line"] = 27;
    arguments["column"] = 8;
    arguments["new_name"] = "trans_renamed";
    const Json result = Call("rename_edits", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    ASSERT_TRUE(result.at("changes").is_object()) << result.dump(1);
    EXPECT_FALSE(result.at("changes").empty()) << result.dump(1);
    for (auto file = result.at("changes").begin(); file != result.at("changes").end(); ++file) {
        EXPECT_EQ(file.key().rfind("file://", 0), std::string::npos) << file.key();
        ASSERT_FALSE(file.value().empty()) << file.key();
        for (const Json& edit : file.value()) {
            EXPECT_EQ(edit.at("newText").get<std::string>(), "trans_renamed");
            const Json& range = edit.at("range");
            // MCP 侧是 1-based：行/列都必须 ≥ 1（Python 用 _one_based_range）
            EXPECT_GE(range.at("start").at("line").get<int>(), 1) << edit.dump(1);
            EXPECT_GE(range.at("start").at("column").get<int>(), 1) << edit.dump(1);
            EXPECT_GE(range.at("end").at("line").get<int>(), 1) << edit.dump(1);
            EXPECT_GE(range.at("end").at("column").get<int>(), 1) << edit.dump(1);
        }
    }
}

TEST_F(BridgeTest, RenameEditsRejectInvalidIdentifier) {
    Json arguments = Json::object();
    arguments["file"] = "src/trans/trans.v";
    arguments["line"] = 27;
    arguments["column"] = 8;
    arguments["new_name"] = "1bad-name";
    EXPECT_THROW(Call("rename_edits", arguments), ToolError);
}

// -------------------------------------------------------------- Phase B / B6

TEST_F(BridgeTest, FormatTextReturnsVeribleFormattedText) {
    Json arguments = Json::object();
    arguments["text"] = "module m(input a,output b);\nassign b=a;\nendmodule\n";
    const Json result = Call("format", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_TRUE(result.at("changed").get<bool>()) << result.dump(1);
    const std::string text = result.at("text").get<std::string>();
    EXPECT_NE(text.find("module m ("), std::string::npos) << text;
    EXPECT_NE(text.find("endmodule"), std::string::npos) << text;
}

TEST_F(BridgeTest, FormatFileUsesProjectResolution) {
    Json arguments = Json::object();
    arguments["file"] = "src/trans/trans.v";  // 相对工程目录
    const Json result = Call("format", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    const std::string text = result.at("text").get<std::string>();
    EXPECT_NE(text.find("module trans"), std::string::npos) << text.substr(0, 200);
}

TEST_F(BridgeTest, FormatRejectsFilesOutsideProjectAndRoot) {
    // SEC-02：format 过去在"绝对路径"和"工程解析失败"两条分支上绕过了 ResolveFile，
    // 是整个越界读的全文读取原语。这里两条逃逸路径都必须抛 ToolError，且错误必须
    // 来自包含校验而不是"文件不存在"（所以用例选真实存在的工程外文件）。
    Json escaping = Json::object();
    escaping["file"] = "../../../CMakeLists.txt";  // 仓库根，语料工程之外
    try {
        Call("format", escaping);
        ADD_FAILURE() << "format 不应读出工程外的文件";
    } catch (const ToolError& error) {
        EXPECT_NE(std::string(error.what()).find("outside"), std::string::npos) << error.what();
    }

    Json absolute = Json::object();
    absolute["file"] = std::string(FPGA_LSP_SOURCE_DIR) + "/CMakeLists.txt";
    EXPECT_THROW(Call("format", absolute), ToolError);

    // 边界是「工程目录或 --root」：落在 root 内的绝对路径必须仍然可用（否则会
    // 把"工程在 root 下但不同目录"的既有用法一起改坏）。
    Json inside = Json::object();
    inside["file"] = std::string(kCorpusRoot) + "/src/top.v";
    const Json in_root = Call("format", inside);
    ASSERT_TRUE(in_root.at("ok").get<bool>()) << in_root.dump(1);
    EXPECT_NE(in_root.at("text").get<std::string>().find("module top"), std::string::npos);
}

TEST_F(BridgeTest, FormatRequiresFileOrText) {
    EXPECT_THROW(Call("format"), ToolError);
}

TEST_F(BridgeTest, FormatRangeReturnsEdits) {
    Json arguments = Json::object();
    arguments["text"] =
        "module m(input a,output b);\nassign b=a;\nendmodule\n";
    arguments["start_line"] = 1;
    arguments["end_line"] = 3;
    const Json result = Call("format", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    ASSERT_TRUE(result.at("edits").is_array()) << result.dump(1);
    EXPECT_TRUE(result.at("changed").get<bool>());
    ASSERT_FALSE(result.at("edits").empty());
    const Json& edit = result.at("edits").at(0);
    EXPECT_TRUE(edit.contains("range"));
    EXPECT_TRUE(edit.contains("newText"));
}

TEST_F(BridgeTest, AnalysisRunWithoutRoutedDbReturnsActionableError) {
    // 语料没有 *_Runs -> 不启动 TD，直接返回与 Python 逐字相同的错误
    const Json result = Call("analysis_run");
    ASSERT_TRUE(result.at("ok").is_boolean()) << result.dump(1);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("error").get<std::string>(),
              "No placed/routed result yet (run Place & Route / Full Flow first)");
    EXPECT_EQ(result.at("device").get<std::string>(), "EF2L45LG144B");
    EXPECT_TRUE(result.at("drc").at("rules").empty());
    EXPECT_TRUE(result.at("cdc").at("rows").empty());
}

// ---- B7 安全补丁（P1 / P1b / P3）的回归 ----

TEST_F(BridgeTest, ProjectOutsideRootIsRejected) {
    // S-03：--root 是安全边界；绝对路径同样必须落在 root 内
    Json arguments = Json::object();
    arguments["project"] = "C:/work/CorpusAlpha/demo_project.al";
    EXPECT_THROW(Call("project_info", arguments), ToolError);
}

TEST_F(BridgeTest, ProjectListRootOutsideRootIsRejected) {
    Json arguments = Json::object();
    arguments["root"] = "C:/work";
    EXPECT_THROW(Call("project_list", arguments), ToolError);
}

TEST_F(BridgeTest, ReadFileRejectsOversizedFile) {
    // S-07：16 MiB 上限，先看大小再读
    TempProjectCopy copy;
    const fs::path big = fs::path(copy.Root()) / "big.v";
    {
        std::string chunk(1024 * 1024, 'x');  // 1 MiB
        std::ofstream handle(big, std::ios::binary | std::ios::trunc);
        for (int index = 0; index < 17; ++index) handle << chunk;
    }
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);
    Json arguments = Json::object();
    arguments["file"] = "big.v";
    const Json result = bridge.Call("read_file", arguments);
    ASSERT_TRUE(result.contains("ok")) << result.dump(1);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_NE(result.at("error").get<std::string>().find("too large"), std::string::npos)
        << result.dump(1);
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

// P-B2：分页偏移。裁剪后的 nextOffset 作为下一次的 offset，结果不重叠也不丢。
TEST_F(BridgeTest, WorkspaceSymbolsPagesWithOffset) {
    Json all_args = Json::object();
    all_args["query"] = "";
    all_args["limit"] = 1000;
    const Json all = Call("workspace_symbols", all_args);
    ASSERT_TRUE(all.at("ok").get<bool>());
    const std::size_t total = all.at("symbols").size();
    ASSERT_GT(total, 4u) << "语料里的符号数必须足够多，才能验证分页";

    Json first_args = Json::object();
    first_args["query"] = "";
    first_args["limit"] = 3;
    const Json first = Call("workspace_symbols", first_args);
    ASSERT_TRUE(first.at("ok").get<bool>());
    ASSERT_EQ(first.at("symbols").size(), 3u);
    ASSERT_EQ(first.at("total").get<std::size_t>(), total);
    const std::size_t next = first.at("nextOffset").get<std::size_t>();
    EXPECT_EQ(next, 3u);

    Json second_args = Json::object();
    second_args["query"] = "";
    second_args["limit"] = 3;
    second_args["offset"] = next;
    const Json second = Call("workspace_symbols", second_args);
    ASSERT_TRUE(second.at("ok").get<bool>());
    ASSERT_EQ(second.at("symbols").size(), 3u);

    // 两页必须完全等于全量结果的前 6 项（说明偏移与顺序都对）
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(second.at("symbols").at(i).at("name").get<std::string>(),
                  all.at("symbols").at(i + 3).at("name").get<std::string>());
    }
}

// P-B5：job 工具在"没有 flow 在跑"时必须明确返回 found:false / cancelled:false，
// 不能让 agent 把空闲误读成"已完成"。
TEST_F(BridgeTest, JobToolsReportIdleState) {
    Json status_args = Json::object();
    status_args["job"] = "flow_deadbeef";
    const Json status = Call("job_status", status_args);
    ASSERT_TRUE(status.at("ok").get<bool>());
    EXPECT_FALSE(status.at("found").get<bool>());
    EXPECT_FALSE(status.at("running").get<bool>());

    Json cancel_args = Json::object();
    cancel_args["job"] = "flow_deadbeef";
    const Json cancelled = Call("job_cancel", cancel_args);
    ASSERT_TRUE(cancelled.at("ok").get<bool>());
    EXPECT_FALSE(cancelled.at("cancelled").get<bool>());
}

// P-B6：只读资源目录（工程信息 / 文件 / 报告摘要 / flow）
TEST_F(BridgeTest, ResourcesExposeReadOnlyProjectViews) {
    const Json resources = bridge->ListResources();
    ASSERT_GE(resources.size(), 4u);
    bool has_files = false;
    for (const Json& item : resources) {
        if (item.at("uri").get<std::string>() == "fpgalsp://project/files") has_files = true;
        EXPECT_EQ(item.at("mimeType").get<std::string>(), "application/json");
    }
    EXPECT_TRUE(has_files);

    const auto files = bridge->ReadResource("fpgalsp://project/files");
    ASSERT_TRUE(files.has_value());
    EXPECT_NE(files->find("\"ok\""), std::string::npos);
    EXPECT_FALSE(bridge->ReadResource("fpgalsp://project/unknown").has_value());
}

// ------------------------------------------------- P2-B：MCP 侧性能优化的行为锁

// ①`flow_status` 改走 ReportStore 的 kArtifacts 快照（不再每次全量 BuildReports）。
// 掩码收窄必须是**纯性能**改动：在临时副本上造一个 runs 目录后，artifacts 段仍要给出
// runs/bit/route_db，而且快照复用不能改内容（两次调用逐字节一致）。
TEST_F(BridgeTest, FlowStatusArtifactsSurviveTheReportStorePath) {
    TempProjectCopy copy;
    const fs::path runs = fs::path(copy.Root()) / "demo_project_Runs";
    std::error_code ec;
    fs::create_directories(runs / "syn_1", ec);
    fs::create_directories(runs / "phy_1", ec);
    {
        std::ofstream bit(runs / "phy_1" / "demo_project_pr.bit", std::ios::binary);
        bit << "BIT";
        std::ofstream db(runs / "phy_1" / "demo_project_pr.db", std::ios::binary);
        db << "DB";
    }

    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);

    const Json first = bridge.Call("flow_status", Json::object());
    ASSERT_TRUE(first.at("ok").get<bool>()) << first.dump(1);
    const Json& artifacts = first.at("artifacts");
    ASSERT_TRUE(artifacts.is_object());
    ASSERT_TRUE(artifacts.at("runs").is_array()) << artifacts.dump(1);
    ASSERT_EQ(artifacts.at("runs").size(), 2u) << "syn_1 / phy_1 都该被列出来";
    // ListSubdirectories 已排序 -> phy_1 在前
    EXPECT_EQ(artifacts.at("runs").at(0).at("name").get<std::string>(), "phy_1");
    EXPECT_TRUE(artifacts.at("bit").at("exists").get<bool>());
    EXPECT_NE(artifacts.at("bit").at("path").get<std::string>().find(".bit"), std::string::npos);
    EXPECT_TRUE(artifacts.at("route_db").at("exists").get<bool>());

    // 第二次调用命中 ReportStore 快照：artifacts 必须逐字节一致（快照不可变）
    const Json second = bridge.Call("flow_status", Json::object());
    EXPECT_EQ(second.at("artifacts").dump(), artifacts.dump());
}

// ②`workspace_symbols` 把 offset/limit 下推到扫描层之后，分页窗口必须与"全量结果的对应
// 区间"逐字段相同，`total` 仍是命中总数（不是本页条数），越界 offset 给空页但保留
// total/nextOffset（分页契约）。
TEST_F(BridgeTest, WorkspaceSymbolsWindowMatchesFullList) {
    Json all_args = Json::object();
    all_args["query"] = "";
    all_args["limit"] = 1000;
    const Json all = Call("workspace_symbols", all_args);
    ASSERT_TRUE(all.at("ok").get<bool>());
    const Json& full = all.at("symbols");
    ASSERT_GT(full.size(), 12u) << "语料符号数太少，验证不了窗口";

    struct Window {
        int limit;
        int offset;
    };
    for (const Window window : {Window{1, 0}, Window{3, 2}, Window{7, 5}, Window{40, 9}}) {
        Json args = Json::object();
        args["query"] = "";
        args["limit"] = window.limit;
        args["offset"] = window.offset;
        const Json page = Call("workspace_symbols", args);
        ASSERT_TRUE(page.at("ok").get<bool>()) << page.dump(1);

        const std::size_t begin = static_cast<std::size_t>(window.offset);
        const std::size_t expected =
            begin >= full.size()
                ? 0u
                : std::min<std::size_t>(static_cast<std::size_t>(window.limit), full.size() - begin);
        ASSERT_EQ(page.at("symbols").size(), expected);
        EXPECT_EQ(page.at("total").get<std::size_t>(), full.size());
        EXPECT_EQ(page.at("nextOffset").get<std::size_t>(), begin + expected);
        for (std::size_t i = 0; i < expected; ++i) {
            EXPECT_EQ(page.at("symbols").at(i).dump(), full.at(begin + i).dump())
                << "offset=" << window.offset << " i=" << i;
        }
    }

    Json beyond = Json::object();
    beyond["query"] = "";
    beyond["limit"] = 5;
    beyond["offset"] = 100000;
    const Json empty_page = Call("workspace_symbols", beyond);
    ASSERT_TRUE(empty_page.at("ok").get<bool>());
    EXPECT_TRUE(empty_page.at("symbols").empty());
    EXPECT_EQ(empty_page.at("total").get<std::size_t>(), full.size());
    EXPECT_EQ(empty_page.at("nextOffset").get<std::size_t>(), 100000u);
}

// ③`references` 32 项 LRU 缓存。必须：
//   (a) 命中路径与冷路径逐字节一致；
//   (b) include_declaration 不同的请求不串味；
//   (c) 索引 generation 变化后立刻失效（否则会返回旧引用 —— 这是这条缓存唯一的真风险）。
TEST_F(BridgeTest, ReferencesCacheIsGenerationKeyed) {
    TempProjectCopy copy;
    fpgalsp::mcp::BridgeOptions options;
    options.root = copy.Root();
    fpgalsp::mcp::Bridge bridge(options);

    Json arguments = Json::object();
    arguments["file"] = "src/system/pwm_gen.v";
    arguments["line"] = 30;    // `module pwm_gen #(`（1-based）
    arguments["column"] = 8;   // 指向模块名
    const Json cold = bridge.Call("references", arguments);
    ASSERT_TRUE(cold.at("ok").get<bool>()) << cold.dump(1);
    const std::size_t cold_count = cold.at("references").size();
    ASSERT_GE(cold_count, 2u) << "定义 + top.v 里的例化，至少 2 条";

    // (a) 同一请求第二次 = 缓存命中，结果必须逐字节一致
    const Json hit = bridge.Call("references", arguments);
    EXPECT_EQ(hit.dump(), cold.dump());

    // (b) 不同 include_declaration 必须是不同条目
    Json without_decl = arguments;
    without_decl["include_declaration"] = false;
    const Json no_decl = bridge.Call("references", without_decl);
    EXPECT_LE(no_decl.at("references").size(), cold_count);
    EXPECT_EQ(bridge.Call("references", without_decl).dump(), no_decl.dump());
    // 回到 true：仍必须是原来那份（说明没被 false 那条覆盖）
    EXPECT_EQ(bridge.Call("references", arguments).dump(), cold.dump());

    // (c) 索引变化 -> generation 推进 -> 缓存失效。
    // 在 top.v 末尾追加一个例化 pwm_gen 的新模块（合法 Verilog；临时副本，不影响别人）。
    {
        const fs::path top_v = fs::path(copy.Root()) / "src" / "top.v";
        std::ofstream append(top_v, std::ios::app);
        append << "\nmodule pwm_ref_probe(input clk);\n"
                  "  pwm_gen u_pwm_ref_probe (.clk(clk));\n"
                  "endmodule\n";
    }
    // 没有 watcher 时 IndexService 按 250 ms TTL 做 stamp 懒校验，要跨过一个窗口。
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const Json refreshed = bridge.Call("references", arguments);
    EXPECT_GT(refreshed.at("references").size(), cold_count)
        << "文件变了以后 references 必须重算，不能命中旧 generation 的缓存";
}
// =============================================================================
//  P2-B⑤：TD 设备摘要**跨进程**落盘缓存
// =============================================================================
//  实测依据 doc/audit/2026-11-22/perf2/MCP-TD.md：MCP 冷调用 510–760 ms，其中
//  `import_device + 4×query_current_package ≈ 350 ms` 是"每个新进程都重来"的重复工作。
//
//  这里用 BridgeOptions::device_probe（测试注入，生产为空）替掉真实 TD 往返：
//  probe 的调用次数就是"是否走了 TD"的计数器，所以三条契约都能在没装 TD 的机器上跑：
//    1) 缓存命中 -> 一次 probe 都不调（等价于不 import_device / 不 query）；
//    2) 缓存损坏 / 键不匹配 -> 回落 probe 一次，并把结果重写回盘（下一个进程能命中）；
//    3) state_dir 不可写（写服务拒绝/目录建不出来）-> 不崩，仍返回 probe 的结果。
namespace {

using fpgalsp::td::DedicatedPin;
using fpgalsp::td::DeviceInfo;
using fpgalsp::td::Pin;

constexpr const char* kProbeTdRoot = "C:/FakeTd/TD_Release_2099.1";
constexpr const char* kProbeTdVersion = "2099.1";

std::string UniqueDeviceCacheStateDir(const char* tag) {
    static int counter = 0;
    ++counter;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
                         ("fpgalsp-mcp-devicecache-" + std::string(tag) + "-" +
                          std::to_string(static_cast<long long>(_getpid())) + "-" +
                          std::to_string(counter));
    return dir.generic_string();
}

// FPGALSP_STATE_DIR 的作用域守卫（写缓存只允许落 state_dir；用完恢复环境变量）。
//   create=false 时**只设环境变量**，不建目录 —— 用来模拟"state_dir 不可写"。
class DeviceCacheStateDir {
  public:
    DeviceCacheStateDir(std::string dir, bool create) : dir_(std::move(dir)) {
        const char* previous = std::getenv("FPGALSP_STATE_DIR");
        saved_ = previous == nullptr ? std::string() : std::string(previous);
        std::error_code ec;
        if (create) {
            fs::remove_all(fs::path(dir_), ec);
            fs::create_directories(fs::path(dir_), ec);
        }
        _putenv_s("FPGALSP_STATE_DIR", dir_.c_str());
    }
    ~DeviceCacheStateDir() {
        _putenv_s("FPGALSP_STATE_DIR", saved_.c_str());
        std::error_code ec;
        fs::remove_all(fs::path(dir_), ec);
    }
    DeviceCacheStateDir(const DeviceCacheStateDir&) = delete;
    DeviceCacheStateDir& operator=(const DeviceCacheStateDir&) = delete;

    const std::string& dir() const { return dir_; }

  private:
    std::string dir_;
    std::string saved_;
};

void WriteRawTextFile(const std::string& path, std::string_view text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// 设备摘要样本：两个引脚（覆盖 intBank / extBank+lvds）与一条专用引脚。
// 值本身是假的（不会拿去碰 TD），用来验证"命中路径与现查路径逐字段一致"。
DeviceInfo FakeDeviceSummary(const std::string& package) {
    DeviceInfo info;
    info.device = "EF2L45LG144B";
    info.family = "EF2";
    info.package = package;
    info.db = "ef2_fake.db";
    Pin p1;
    p1.pe = "P1";
    p1.pe_raw = "p1";
    p1.location = "pb1a";
    p1.type = "fio";
    p1.int_bank = 1;
    p1.clock_capable = true;
    p1.clock_kind = "dpclkio_1";
    info.pins["p1"] = p1;
    Pin p2;
    p2.pe = "P2";
    p2.pe_raw = "p2";
    p2.location = "pb1b";
    p2.type = "fio";
    p2.ext_bank = 3;
    p2.lvds = true;
    p2.lvds_type = "LVDS";
    info.pins["p2"] = p2;
    info.dedicated.push_back(DedicatedPin{"programn", "P119"});
    return info;
}

std::string CorrelationKey() {
    return fpgalsp::td::DeviceSummaryCacheKey(kProbeTdRoot, kProbeTdVersion, "EF2L45LG144B",
                                              "EF2", "");
}

struct ProbeSetup {
    fpgalsp::mcp::BridgeOptions options;
    int* calls = nullptr;

    explicit ProbeSetup(int* counter, const char* td_version = kProbeTdVersion,
                        const std::string& live_package = "live_pkg") : calls(counter) {
        options.root = kCorpusRoot;
        fpgalsp::mcp::BridgeOptions::DeviceProbe probe;
        probe.td_root = kProbeTdRoot;
        probe.td_version = td_version;
        probe.load = [counter, live_package](const fpgalsp::project::Project&)
            -> std::optional<DeviceInfo> {
            ++(*counter);
            return FakeDeviceSummary(live_package);
        };
        options.device_probe = probe;
    }
};

}  // namespace

// (1) 命中：一次 probe 都不调 = 不 import_device、不 query_current_package。
TEST_F(BridgeTest, DeviceSummaryCacheHitSkipsTdProbe) {
    DeviceCacheStateDir state(UniqueDeviceCacheStateDir("hit"), /*create=*/true);
    const std::string key = CorrelationKey();
    const DeviceInfo cached = FakeDeviceSummary("cached_pkg");
    WriteRawTextFile(fpgalsp::td::DeviceSummaryCachePath(state.dir()),
                     fpgalsp::td::DeviceSummaryCacheFileText(key, cached));

    int probe_calls = 0;
    const ProbeSetup setup(&probe_calls);
    fpgalsp::mcp::Bridge bridge(setup.options);

    Json arguments = Json::object();
    arguments["include_pins"] = true;
    arguments["limit"] = 5000;
    const Json result = bridge.Call("device_info", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_EQ(probe_calls, 0)
        << "缓存命中不能走 TD（import_device / query_current_package）";

    // 与现查逐字段一致：device/family 取 .al，package/db/pinCount/引脚字段取摘要缓存。
    EXPECT_EQ(result.at("device").get<std::string>(), "EF2L45LG144B");
    EXPECT_EQ(result.at("family").get<std::string>(), "EF2");
    EXPECT_EQ(result.at("package").get<std::string>(), "cached_pkg");
    EXPECT_EQ(result.at("db").get<std::string>(), "ef2_fake.db");
    EXPECT_EQ(result.at("pinCount").get<int>(), 2);
    bool saw_p1 = false;
    bool saw_p2 = false;
    for (const Json& pin : result.at("pins")) {
        const std::string name = pin.at("pin").get<std::string>();
        if (name == "P1") {
            saw_p1 = true;
            EXPECT_EQ(pin.at("location").get<std::string>(), "pb1a");
            EXPECT_EQ(pin.at("intBank").get<int>(), 1);
            EXPECT_TRUE(pin.at("clockCapable").get<bool>());
            EXPECT_FALSE(pin.at("lvds").get<bool>());
        } else if (name == "P2") {
            saw_p2 = true;
            EXPECT_EQ(pin.at("location").get<std::string>(), "pb1b");
            ASSERT_TRUE(pin.at("extBank").is_number());
            EXPECT_EQ(pin.at("extBank").get<int>(), 3);
            EXPECT_FALSE(pin.at("clockCapable").get<bool>());
            EXPECT_TRUE(pin.at("lvds").get<bool>());
        }
    }
    EXPECT_TRUE(saw_p1);
    EXPECT_TRUE(saw_p2);
}

// (2) 缓存损坏：回落现查一次，并把结果重写回盘（下一个"进程"能直接命中）。
TEST_F(BridgeTest, DeviceSummaryCacheCorruptFallsBackAndRewrites) {
    DeviceCacheStateDir state(UniqueDeviceCacheStateDir("corrupt"), /*create=*/true);
    const std::string key = CorrelationKey();
    WriteRawTextFile(fpgalsp::td::DeviceSummaryCachePath(state.dir()),
                     "{ this is not valid json !!!");

    int probe_calls = 0;
    const ProbeSetup first(&probe_calls);
    fpgalsp::mcp::Bridge bridge(first.options);
    Json arguments = Json::object();
    arguments["include_pins"] = true;
    arguments["limit"] = 5000;
    const Json result = bridge.Call("device_info", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_EQ(probe_calls, 1) << "损坏 = 未命中，必须回落现查";
    EXPECT_EQ(result.at("package").get<std::string>(), "live_pkg");
    EXPECT_EQ(result.at("pinCount").get<int>(), 2);

    // 重写：盘上现在能读回同一份摘要（含引脚与专用引脚）
    const std::optional<DeviceInfo> stored =
        fpgalsp::td::LoadDeviceSummaryCache(state.dir(), key);
    ASSERT_TRUE(stored.has_value()) << "现查之后必须重写缓存";
    EXPECT_EQ(stored->package, "live_pkg");
    EXPECT_EQ(stored->pins.size(), 2u);
    ASSERT_EQ(stored->dedicated.size(), 1u);
    EXPECT_EQ(stored->dedicated.front().role, "programn");

    // 模拟"进程重启"：新 Bridge + 同一个 state_dir，必须命中且不再调 probe。
    int second_calls = 0;
    const ProbeSetup second(&second_calls, kProbeTdVersion, "should_not_be_used");
    fpgalsp::mcp::Bridge bridge2(second.options);
    const Json hit = bridge2.Call("device_info", arguments);
    ASSERT_TRUE(hit.at("ok").get<bool>()) << hit.dump(1);
    EXPECT_EQ(second_calls, 0);
    EXPECT_EQ(hit.at("package").get<std::string>(), "live_pkg");
}

// (3) state_dir 不可写：写缓存失败被 IgnoreResult 吞掉，调用不崩、结果照常。
TEST_F(BridgeTest, DeviceSummaryCacheUnwritableStateDirStillReturns) {
    // 把 state_dir 指到一个**文件**上：<state>/cache 目录建不出来，写必失败。
    const fs::path blocker =
        fs::temp_directory_path() /
        ("fpgalsp-mcp-devicecache-blocker-" +
         std::to_string(static_cast<long long>(_getpid())));
    std::error_code ec;
    fs::remove_all(blocker, ec);
    {
        std::ofstream out(blocker, std::ios::binary | std::ios::trunc);
        out << "not a directory";
    }
    DeviceCacheStateDir state(blocker.generic_string(), /*create=*/false);

    int probe_calls = 0;
    const ProbeSetup setup(&probe_calls);
    fpgalsp::mcp::Bridge bridge(setup.options);
    Json arguments = Json::object();
    arguments["include_pins"] = true;
    arguments["limit"] = 5000;
    const Json result = bridge.Call("device_info", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_EQ(probe_calls, 1);
    EXPECT_EQ(result.at("package").get<std::string>(), "live_pkg");
    EXPECT_EQ(result.at("pinCount").get<int>(), 2);

    // 直接钉住"写失败"的语义：返回 false 且不抛、不留残文件。
    EXPECT_FALSE(fpgalsp::td::StoreDeviceSummaryCache(kCorpusRoot, state.dir(), CorrelationKey(),
                                                      FakeDeviceSummary("x")));
    EXPECT_FALSE(fs::exists(fs::path(fpgalsp::td::DeviceSummaryCachePath(state.dir()))));
}

// (4) 键覆盖 TD 身份：TD 升级（版本变化）后旧缓存不能命中。
TEST_F(BridgeTest, DeviceSummaryCacheMissesWhenTdVersionChanges) {
    DeviceCacheStateDir state(UniqueDeviceCacheStateDir("version"), /*create=*/true);
    const DeviceInfo cached = FakeDeviceSummary("old_td_pkg");
    WriteRawTextFile(fpgalsp::td::DeviceSummaryCachePath(state.dir()),
                     fpgalsp::td::DeviceSummaryCacheFileText(CorrelationKey(), cached));

    int probe_calls = 0;
    const ProbeSetup setup(&probe_calls, "2099.2", "new_td_pkg");
    fpgalsp::mcp::Bridge bridge(setup.options);
    Json arguments = Json::object();
    arguments["include_pins"] = true;
    const Json result = bridge.Call("device_info", arguments);
    ASSERT_TRUE(result.at("ok").get<bool>()) << result.dump(1);
    EXPECT_EQ(probe_calls, 1) << "TD 版本变了 -> 旧条目必须失效";
    EXPECT_EQ(result.at("package").get<std::string>(), "new_td_pkg");
}
