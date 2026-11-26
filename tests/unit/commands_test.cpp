// =============================================================================
//  tests/unit/commands_test.cpp —— B-A2：workspace/executeCommand 的声明与实现一致
// =============================================================================
//  背景：initialize 以前**没有**声明 executeCommandProvider，25 条命令对第三方
//  客户端不可见（只有本仓库扩展知道它们存在）。
//
//  这里守住三条不变量：
//    1. 命令表本身：非空、无重复、统一前缀（防止手滑写出半截名字）；
//    2. capability == 命令表（声明与表同源，不可能再漂移）；
//    3. 分发表真的认识表里的名字 —— 用无副作用的两条命令验证（metrics / 未知命令）。
//       其余命令会启动 TD/flow，不适合在单测里执行；对它们的保护来自第 2 条
//       （capability 由表生成）与 e2e 冒烟。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <ctime>
#include <string>
#include <vector>

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/transport.h"
#include "platform/encoding.h"
#include "platform/path.h"
#include "server/lsp_server.h"

namespace {

using Json = fpgalsp::server::Json;
using fpgalsp::jsonrpc::TransportBaseCommon;
using fpgalsp::jsonrpc::TransportMode;
using fpgalsp::server::LspServer;

const std::string kProject = std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";

class CommandsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        transport_ = fpgalsp::jsonrpc::MakeTransport(TransportMode::Lsp, input_, output_);
        server_ = std::make_unique<LspServer>(*transport_);
        Json params = Json::object();
        params["rootUri"] = fpgalsp::platform::path_to_uri(kProject);
        server_->FeatureInitialize(params);
    }

    void TearDown() override { server_.reset(); }

    LspServer& server() { return *server_; }

  protected:
    std::istringstream input_;
    std::ostringstream output_;
    std::unique_ptr<TransportBaseCommon> transport_;
    std::unique_ptr<LspServer> server_;
};

TEST_F(CommandsTest, CommandTableIsWellFormed) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::vector<std::string>& names = fpgalsp::server::ExecuteCommandNames();
    EXPECT_FALSE(names.empty());
    std::set<std::string> unique;
    for (const std::string& name : names) {
        EXPECT_EQ(name.rfind("fpgalsp.", 0), 0u) << "命令名必须以 fpgalsp. 开头: " << name;
        EXPECT_TRUE(unique.insert(name).second) << "命令名重复: " << name;
    }
    EXPECT_EQ(unique.size(), names.size());
}

TEST_F(CommandsTest, CapabilityMatchesCommandTable) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const Json init = server().FeatureInitialize(Json::object());
    ASSERT_TRUE(init.contains("capabilities"));
    const Json& capabilities = init["capabilities"];
    ASSERT_TRUE(capabilities.contains("executeCommandProvider"));
    const Json& commands = capabilities["executeCommandProvider"]["commands"];
    ASSERT_TRUE(commands.is_array());

    const std::vector<std::string>& expected = fpgalsp::server::AdvertisedCommandNames();
    ASSERT_EQ(commands.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(commands[i].get<std::string>(), expected[i]) << "第 " << i << " 项不一致";
    }
}

// 广播子集必须排除扩展自己 contributes 的 3 条历史命令 —— vscode-languageclient
// 会把广播的每条命令注册成 VS Code 命令，重复注册会让 initialize 直接失败。
TEST_F(CommandsTest, AdvertisedSubsetExcludesClientOwnedCommands) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::vector<std::string>& all = fpgalsp::server::ExecuteCommandNames();
    const std::vector<std::string>& advertised = fpgalsp::server::AdvertisedCommandNames();
    EXPECT_EQ(advertised.size() + 3, all.size());
    for (const std::string& name : {"fpgalsp.check", "fpgalsp.info", "fpgalsp.restart"}) {
        EXPECT_EQ(std::find(advertised.begin(), advertised.end(), name), advertised.end())
            << name << " 由扩展 contributes，不能再广播";
        EXPECT_NE(std::find(all.begin(), all.end(), name), all.end())
            << name << " 仍必须能被 executeCommand 分发";
    }
}

// 走**已注册的 dispatcher**（而不是直接调私有 handler）：这样测的是真实的
// 注册表 → workspace/executeCommand → 分发表 这条链路。
TEST_F(CommandsTest, MetricsCommandDispatches) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    fpgalsp::jsonrpc::Dispatcher dispatcher(*transport_);
    server().RegisterAll(dispatcher);

    Json params = Json::object();
    params["command"] = "fpgalsp.metrics";
    params["arguments"] = Json::array();
    const auto result = dispatcher.Invoke("workspace/executeCommand", params);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error) << result->error_message;
    EXPECT_TRUE(result->result["ok"].get<bool>());
}

TEST_F(CommandsTest, UnknownCommandFails) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    fpgalsp::jsonrpc::Dispatcher dispatcher(*transport_);
    server().RegisterAll(dispatcher);

    Json params = Json::object();
    params["command"] = "fpgalsp.definitely_not_a_command";
    params["arguments"] = Json::array();
    const auto result = dispatcher.Invoke("workspace/executeCommand", params);
    ASSERT_TRUE(result.has_value());
    // 约定：未知命令不是 JSON-RPC error，而是 {ok:false, error:"unknown command ..."}
    // 的**结果**（与工具失败同一条语义，客户端据 ok 判断）。
    EXPECT_FALSE(result->is_error);
    EXPECT_FALSE(result->result.at("ok").get<bool>());
    EXPECT_NE(result->result.at("error").get<std::string>().find("unknown command"),
              std::string::npos);
}

// LSP 的 fpgalsp.probeConfig 必须和 MCP 的 fpga_probe_config 同语义：disabled 只有
// 经"自写 cwc -> insert_debugger -cwc"才进 bit，而那条路是 apply_build_config 门控的。
// 插件侧传 apply_build_config:false，所以这里守的是"传了就转发、不传默认关"。
// config.txt 用 FPGALSP_STATE_DIR 指到临时目录，绝不碰 gate-corpus 工程。
TEST_F(CommandsTest, ProbeConfigForwardsApplyBuildConfigOptIn) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
#if defined(_WIN32)
    namespace fs = std::filesystem;
    struct StateDirGuard {
        std::string dir;
        explicit StateDirGuard(std::string value) : dir(std::move(value)) {
            std::error_code ec;
            fs::remove_all(fs::path(dir), ec);
            fs::create_directories(fs::path(dir), ec);
            _putenv_s("FPGALSP_STATE_DIR", dir.c_str());
        }
        ~StateDirGuard() {
            _putenv_s("FPGALSP_STATE_DIR", "");
            std::error_code ec;
            fs::remove_all(fs::path(dir), ec);
        }
    } guard((fs::temp_directory_path() / "fpgalsp_cmd_probe_state").string());
    const std::string config_path = guard.dir + "/probe/config.txt";

    fpgalsp::jsonrpc::Dispatcher dispatcher(*transport_);
    server().RegisterAll(dispatcher);
    const auto invoke = [&dispatcher](Json options) {
        Json params = Json::object();
        params["command"] = "fpgalsp.probeConfig";
        params["arguments"] = Json::array();
        params["arguments"].push_back(std::move(options));
        return dispatcher.Invoke("workspace/executeCommand", params);
    };

    Json on = Json::object();
    on["write"] = false;  // 只验证参数转发，别改工程的 user.mdn
    on["apply_build_config"] = true;
    const auto applied = invoke(std::move(on));
    ASSERT_TRUE(applied.has_value());
    ASSERT_FALSE(applied->is_error) << applied->error_message;
    EXPECT_TRUE(applied->result.value("apply_build_config", false)) << applied->result.dump();
    const std::string text =
        fpgalsp::platform::try_read_file_utf8(config_path).value_or("");
    EXPECT_NE(text.find("apply=1"), std::string::npos) << text;

    // 不传 = 保持 opt-in 默认关闭，不写 apply=1
    std::error_code ec;
    fs::remove(fs::path(config_path), ec);
    Json off = Json::object();
    off["write"] = false;
    const auto plain = invoke(std::move(off));
    ASSERT_TRUE(plain.has_value());
    ASSERT_FALSE(plain->is_error) << plain->error_message;
    EXPECT_FALSE(plain->result.value("apply_build_config", false));
    EXPECT_FALSE(fs::exists(fs::path(config_path))) << "默认不能写 apply=1";
#else
    GTEST_SKIP() << "state dir override is Windows-only";
#endif
}

}  // namespace
// P-C3：revision 条件请求。带上"我看到的那一版"时，服务端不该再取快照/做装饰。
TEST_F(CommandsTest, ReportsHonourIfRevision) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const Json first = server().FeatureReports(Json::object());
    ASSERT_TRUE(first.contains("revision")) << "报告响应必须带 revision";
    const std::uint64_t revision = first.at("revision").get<std::uint64_t>();

    Json params = Json::object();
    params["ifRevision"] = revision;
    const Json second = server().FeatureReports(params);
    EXPECT_TRUE(second.contains("unchanged"));
    EXPECT_TRUE(second.at("unchanged").get<bool>());
    EXPECT_EQ(second.at("revision").get<std::uint64_t>(), revision);
    EXPECT_FALSE(second.contains("timing")) << "unchanged 应答不带任何报告数据";

    // 版本对不上时必须真的返回数据
    params["ifRevision"] = revision + 1;
    const Json third = server().FeatureReports(params);
    EXPECT_FALSE(third.contains("unchanged"));
}

// P-C4：flowStatus 的日志切片（窗口 / 增量 / 落后）
TEST(FlowLogSliceTest, WindowAndIncrementalCases) {
    fpgalsp::flow::FlowState state;
    state.log = {"a", "b", "c", "d", "e"};
    state.log_seq = 12;  // 7..11 的 5 行在窗口里，序号 = log_seq - (size - i)

    std::uint64_t start = 0;
    // 旧客户端：完整窗口
    Json all = fpgalsp::server::SliceFlowLog(state, std::nullopt, &start);
    EXPECT_EQ(start, 7u);
    EXPECT_EQ(all.size(), 5u);
    EXPECT_EQ(all[0].get<std::string>(), "a");

    // 增量：只回 since 之后的行（since=10 -> 第 11 行 = "e"）
    Json tail = fpgalsp::server::SliceFlowLog(state, std::uint64_t{10}, &start);
    ASSERT_EQ(tail.size(), 1u);
    EXPECT_EQ(tail[0].get<std::string>(), "e");

    // 无新行
    Json none = fpgalsp::server::SliceFlowLog(state, std::uint64_t{12}, &start);
    EXPECT_TRUE(none.empty());

    // 落后于窗口：先给 truncated 标记，再给整个窗口
    Json behind = fpgalsp::server::SliceFlowLog(state, std::uint64_t{1}, &start);
    ASSERT_EQ(behind.size(), 6u);
    EXPECT_NE(behind[0].get<std::string>().find("truncated"), std::string::npos);
    EXPECT_EQ(behind[5].get<std::string>(), "e");
}

// P-D5：语义高亮增量只覆盖变化区间（旧实现把整个新数组塞进 delta，比 full 还贵）
TEST(SemanticDeltaTest, SingleTokenEditKeepsPrefixAndSuffix) {
    // LSP 的 data 是**扁平**的 5 元组数组（不是"数组的数组"）
    const auto push_token = [](Json& arr, int line, int start, int len, int type) {
        arr.push_back(line);
        arr.push_back(start);
        arr.push_back(len);
        arr.push_back(type);
        arr.push_back(0);
    };
    Json before = Json::array();
    for (int i = 0; i < 10; ++i) push_token(before, 0, i * 4, 3, 1);
    Json after = Json::array();
    for (int i = 0; i < 10; ++i) push_token(after, 0, i * 4, 3, 1);
    after[3 * 5 + 3] = 7;  // 第 4 个 token 的类型变了

    const Json edits = fpgalsp::server::SemanticDeltaEdits(before, after);
    ASSERT_EQ(edits.size(), 1u);
    const Json& edit = edits[0];
    EXPECT_EQ(edit.at("start").get<int>(), 15);       // 3 个 token × 5
    EXPECT_EQ(edit.at("deleteCount").get<int>(), 5);  // 只删 1 个 token
    EXPECT_EQ(edit.at("data").size(), 5u);            // 只补 1 个 token

    // 完全相同 -> 空编辑（客户端复用旧 token）
    EXPECT_TRUE(fpgalsp::server::SemanticDeltaEdits(after, after).empty());

    // 纯追加 -> start 在末尾、deleteCount=0
    Json appended = after;
    push_token(appended, 0, 40, 3, 1);
    const Json tail_edits = fpgalsp::server::SemanticDeltaEdits(after, appended);
    ASSERT_EQ(tail_edits.size(), 1u);
    EXPECT_EQ(tail_edits[0].at("start").get<int>(), 50);
    EXPECT_EQ(tail_edits[0].at("deleteCount").get<int>(), 0);
}

// P-C2：懒加载子节点——只回一层、带 hasChildren、可翻页
TEST_F(CommandsTest, ModuleChildrenReturnsOneLevelAndPaginates) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    Json params = Json::object();
    params["module"] = "top";
    params["limit"] = 2;
    const Json first = server().FeatureModuleChildren(params);
    ASSERT_TRUE(first.at("ok").get<bool>()) << first.dump();
    EXPECT_EQ(first.at("parent").get<std::string>(), "top");
    ASSERT_FALSE(first.at("children").empty());
    ASSERT_LE(first.at("children").size(), 2u);
    for (const Json& child : first.at("children")) {
        EXPECT_FALSE(child.contains("children"))
            << "懒加载只回一层，子节点不能再带 children";
        EXPECT_TRUE(child.contains("hasChildren"));
    }

    if (first.at("hasMore").get<bool>()) {
        params["offset"] = first.at("nextOffset");
        const Json second = server().FeatureModuleChildren(params);
        ASSERT_TRUE(second.at("ok").get<bool>());
        EXPECT_FALSE(second.at("children").empty());
        EXPECT_NE(second.at("children").at(0).at("instance").get<std::string>(),
                  first.at("children").at(0).at("instance").get<std::string>());
    }

    // 未知模块要明确失败，而不是空 children
    params["module"] = "definitely_not_a_module";
    const Json unknown = server().FeatureModuleChildren(params);
    EXPECT_FALSE(unknown.at("ok").get<bool>());
}

// =============================================================================
//  B-B8：工程级诊断（.al 里记了但磁盘上没有的文件 / 解析告警）
// =============================================================================
TEST(ProjectDiagnosticsTest, ReportsMissingFilesAndWarnings) {
    fpgalsp::project::Project project;
    project.dir = "Y:/fake";
    project.al_path = "Y:/fake/p.al";
    project.name = "fake";

    fpgalsp::project::AlFile missing;
    missing.path = "Y:/fake/src/missing.v";
    missing.rel = "src/missing.v";
    missing.kind = fpgalsp::project::FileKind::Verilog;
    missing.exists = false;

    fpgalsp::project::AlFile disabled;
    disabled.path = "Y:/fake/src/disabled.v";
    disabled.rel = "src/disabled.v";
    disabled.kind = fpgalsp::project::FileKind::Verilog;
    disabled.exists = false;
    disabled.enabled = false;  // 用户显式关掉了：不该报

    project.files = {missing, disabled};
    project.warnings = {"td_version missing"};

    const auto diagnostics = fpgalsp::server::ProjectDiagnostics(project);
    ASSERT_EQ(diagnostics.size(), 2u);
    bool has_missing = false;
    bool has_warning = false;
    for (const auto& diagnostic : diagnostics) {
        EXPECT_EQ(diagnostic.range.start.line, 0);
        if (diagnostic.code == "AL-MISSING-FILE") {
            has_missing = true;
            EXPECT_EQ(diagnostic.severity, fpgalsp::lsp::kError);
            EXPECT_NE(diagnostic.message.find("src/missing.v"), std::string::npos);
        }
        if (diagnostic.code == "AL-WARNING") {
            has_warning = true;
            EXPECT_EQ(diagnostic.severity, fpgalsp::lsp::kWarning);
        }
    }
    EXPECT_TRUE(has_missing);
    EXPECT_TRUE(has_warning);
}

// =============================================================================
//  B-A3：workspace/willRenameFiles 必须改写 .al 里的路径
// =============================================================================
TEST(RenameNotificationTest, WillRenameRewritesAlFileEntry) {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / ("fpgalsp-rename-" + std::to_string(::time(nullptr)));
    fs::create_directories(dir / "src");
    {
        std::ofstream out(dir / "src" / "old.v");
        out << "module old (input clk, output q);\n  assign q = clk;\nendmodule\n";
    }
    const std::string al_text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project Version=\"3\" Minor=\"2\" Path=\"" + dir.generic_string() + "\">\n"
        "    <Name>rename</Name>\n"
        "    <HardWare><Family>EF2</Family><Device>EF2L45LG144B</Device></HardWare>\n"
        "    <Source_Files><Verilog>\n"
        "        <File Path=\"src/old.v\"><FileInfo><Attr Name=\"CompileOrder\" Val=\"1\"/></FileInfo></File>\n"
        "    </Verilog></Source_Files>\n"
        "    <Top_Module>old</Top_Module>\n"
        "</Project>\n";
    {
        std::ofstream out(dir / "p.al");
        out << al_text;
    }

    std::istringstream input;
    std::ostringstream output;
    auto transport = fpgalsp::jsonrpc::MakeTransport(fpgalsp::jsonrpc::TransportMode::Lsp, input,
                                                     output);
    LspServer server(*transport);
    Json init = Json::object();
    init["rootUri"] = fpgalsp::platform::path_to_uri(dir.generic_string());
    server.FeatureInitialize(init);

    Json params = Json::object();
    Json files = Json::array();
    Json rename = Json::object();
    rename["oldUri"] = fpgalsp::platform::path_to_uri((dir / "src" / "old.v").generic_string());
    rename["newUri"] = fpgalsp::platform::path_to_uri((dir / "src" / "renamed.v").generic_string());
    files.push_back(std::move(rename));
    params["files"] = std::move(files);

    const Json edit = server.FeatureWillRenameFiles(params);
    ASSERT_TRUE(edit.contains("changes")) << edit.dump();
    const Json& changes = edit.at("changes");
    ASSERT_EQ(changes.size(), 1u);
    const auto list = changes.begin().value();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list.at(0).at("newText").get<std::string>(), "src/renamed.v");
    // 编辑必须落在 .al 里那一行的路径上
    const Json& range = list.at(0).at("range");
    EXPECT_EQ(range.at("start").at("line").get<int>(), 5);
    EXPECT_EQ(range.at("start").at("character").get<int>(),
              static_cast<int>(std::string("        <File Path=\"").size()));
}

// B-A3：必须声明 fileOperations，否则客户端根本不会发 willRenameFiles。
TEST_F(CommandsTest, DeclaresFileOperationFilters) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const Json init = server().FeatureInitialize(Json::object());
    const Json& operations = init.at("capabilities").at("workspace").at("fileOperations");
    ASSERT_TRUE(operations.contains("willRename"));
    ASSERT_TRUE(operations.contains("didRename"));
    const std::string glob =
        operations.at("willRename").at("filters").at(0).at("pattern").at("glob").get<std::string>();
    EXPECT_NE(glob.find("{v,sv"), std::string::npos) << glob;
}

// B-B10：testbench 骨架（端口声明/例化/自检）
TEST_F(CommandsTest, TestbenchSkeletonIsParseable) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    Json params = Json::object();
    params["module"] = "top";
    const Json out = server().FeatureTestbench(params);
    ASSERT_TRUE(out.at("ok").get<bool>()) << out.dump();
    EXPECT_EQ(out.at("module").get<std::string>(), "top");
    const std::string text = out.at("text").get<std::string>();
    EXPECT_NE(text.find("module top_tb;"), std::string::npos);
    EXPECT_NE(text.find("u_dut"), std::string::npos);
    EXPECT_NE(text.find("endmodule"), std::string::npos);
    EXPECT_TRUE(out.at("parseOk").get<bool>()) << out.at("parseErrors").dump();

    // 猜不到时钟时也有可编译的骨架（不生成 always 块）
    const bool has_clock = !out.at("clock").get<std::string>().empty();
    EXPECT_EQ(text.find("always #5"), has_clock ? text.find("always #5") : std::string::npos);

    Json unknown = Json::object();
    unknown["module"] = "definitely_not_a_module";
    const Json missing = server().FeatureTestbench(unknown);
    EXPECT_FALSE(missing.at("ok").get<bool>());
}
