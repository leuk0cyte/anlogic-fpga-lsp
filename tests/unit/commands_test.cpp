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
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/transport.h"
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

}  // namespace
// P-C3：revision 条件请求。带上"我看到的那一版"时，服务端不该再取快照/做装饰。
TEST_F(CommandsTest, ReportsHonourIfRevision) {
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
