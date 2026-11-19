// =============================================================================
//  tests/unit/mcp_server_test.cpp —— MCP 协议层
// =============================================================================
//  用假 dispatcher 测协议语义，不碰 TD / 文件系统 / 工程：
//    · 版本协商、能力声明
//    · tools/list 的可见性过滤与内部字段剥离
//    · tools/call 的 required 校验、ToolError→isError 结果、未知工具、崩溃兜底
//    · 结果截断（800000 字节）
//    · 通知不应答 / 批量数组 / 未知方法 -32601
//
//  ⚠ 最容易被做错的一条：**工具失败是 isError 结果，不是 JSON-RPC error**。
//    客户端靠 isError 区分"工具说它失败了"和"协议层出错了"，混用会让 agent
//    把工具错误当成连接故障去重试。
// =============================================================================

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "mcp/mcp_server.h"
#include "mcp/tools.h"

namespace {

using fpgalsp::mcp::Json;
using fpgalsp::mcp::McpServer;
using fpgalsp::mcp::McpServerOptions;
using fpgalsp::mcp::ToolDispatcher;
using fpgalsp::mcp::ToolError;

class FakeDispatcher : public ToolDispatcher {
  public:
    Json Call(std::string_view handler, const Json& arguments) override {
        calls.emplace_back(handler);
        last_arguments = arguments;
        const std::string name(handler);
        if (name == "project_list") {
            Json out = Json::object();
            out["ok"] = true;
            out["projects"] = Json::array();
            return out;
        }
        if (name == "read_file") return std::string("plain-text");  // 非对象/数组
        if (name == "diagnostics") {
            Json out = Json::object();
            out["ok"] = true;
            out["blob"] = std::string(900000, 'x');  // > 800000 字节
            return out;
        }
        if (name == "search_text") throw ToolError("boom");
        if (name == "hover") throw std::runtime_error("kaboom");
        if (name == "flow_wait") {
            if (progress_) progress_(0.5, "half way");
            Json out = Json::object();
            out["ok"] = true;
            return out;
        }
        Json out = Json::object();
        out["ok"] = true;
        out["handler"] = name;
        return out;
    }

    std::vector<std::string> calls;
    Json last_arguments = nullptr;
};

Json Request(const char* method, const Json& params, const Json& id = Json(1)) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = method;
    message["params"] = params;
    return message;
}

Json CallRequest(const char* name, const Json& arguments, const Json& id = Json(1)) {
    Json params = Json::object();
    params["name"] = name;
    params["arguments"] = arguments;
    return Request("tools/call", params, id);
}

struct McpServerTest : public ::testing::Test {
    FakeDispatcher dispatcher;
    McpServerOptions options;
    std::unique_ptr<McpServer> server;

    void SetUp() override {
        options.allow_flow = true;
        options.allow_write = true;
        // P-B1：这些用例验的是"结构化结果"这条路径（旧行为），所以显式打开协商档；
        // 默认档（只发紧凑 text）由下面 DefaultTier* 三个用例单独覆盖。
        options.client_supports_structured = true;
        server = std::make_unique<McpServer>(dispatcher, options);
    }

    Json Call(const Json& message) {
        const auto response = server->Handle(message);
        EXPECT_TRUE(response.has_value());
        return *response;
    }
};

}  // namespace

// ---------------------------------------------------------------- initialize

TEST_F(McpServerTest, InitializeDefaultsToOldestSupportedVersion) {
    const Json response = Call(Request("initialize", Json::object()));
    EXPECT_EQ(response.at("result").at("protocolVersion").get<std::string>(), "2024-11-05");
}

TEST_F(McpServerTest, InitializeEchoesSupportedVersion) {
    Json params = Json::object();
    params["protocolVersion"] = "2025-06-18";
    const Json response = Call(Request("initialize", params));
    EXPECT_EQ(response.at("result").at("protocolVersion").get<std::string>(), "2025-06-18");
}

TEST_F(McpServerTest, InitializeUnsupportedVersionFallsBack) {
    Json params = Json::object();
    params["protocolVersion"] = "1999-01-01";
    const Json response = Call(Request("initialize", params));
    EXPECT_EQ(response.at("result").at("protocolVersion").get<std::string>(), "2024-11-05");
}

TEST_F(McpServerTest, InitializeDeclaresToolsCapabilityAndServerInfo) {
    const Json result = Call(Request("initialize", Json::object())).at("result");
    EXPECT_FALSE(result.at("capabilities").at("tools").at("listChanged").get<bool>());
    EXPECT_EQ(result.at("serverInfo").at("name").get<std::string>(), "fpga-lsp-mcp");
    EXPECT_FALSE(result.at("serverInfo").at("version").get<std::string>().empty());
}

TEST_F(McpServerTest, InitializeCarriesInstructionsForAgents) {
    const Json result = Call(Request("initialize", Json::object())).at("result");
    const std::string instructions = result.at("instructions").get<std::string>();
    EXPECT_NE(instructions.find("1-based"), std::string::npos);
    EXPECT_NE(instructions.find("--allow-flow"), std::string::npos);
}

// ---------------------------------------------------------------------- ping

TEST_F(McpServerTest, PingReturnsEmptyResult) {
    const Json response = Call(Request("ping", Json::object()));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response.at("result").is_object());
    EXPECT_TRUE(response.at("result").empty());
}

// --------------------------------------------------------------- tools/list

TEST_F(McpServerTest, ToolsListAllModesReturns39) {
    const Json response = Call(Request("tools/list", Json::object()));
    EXPECT_EQ(response.at("result").at("tools").size(), 39u);
}

TEST_F(McpServerTest, ToolsListDefaultHidesFlowAndWriteTools) {
    McpServerOptions restricted;
    McpServer restricted_server(dispatcher, restricted);
    const Json response = *restricted_server.Handle(Request("tools/list", Json::object()));
    const Json& tools = response.at("result").at("tools");
    EXPECT_EQ(tools.size(), 32u);
    for (const Json& tool : tools) {
        const std::string name = tool.at("name").get<std::string>();
        EXPECT_NE(name, "fpga_flow_run") << "flow 工具默认不该出现";
        EXPECT_NE(name, "fpga_project_rename") << "write 工具默认不该出现";
    }
}

TEST_F(McpServerTest, ToolsListStripsInternalFields) {
    const Json tools = Call(Request("tools/list", Json::object())).at("result").at("tools");
    for (const Json& tool : tools) {
        EXPECT_FALSE(tool.contains("handler")) << tool.value("name", "");
        EXPECT_FALSE(tool.contains("requires")) << tool.value("name", "");
        EXPECT_TRUE(tool.contains("annotations")) << tool.value("name", "");
        EXPECT_TRUE(tool.contains("inputSchema")) << tool.value("name", "");
    }
}

TEST_F(McpServerTest, ToolsListKeepsTableOrder) {
    const Json tools = Call(Request("tools/list", Json::object())).at("result").at("tools");
    ASSERT_GE(tools.size(), 2u);
    EXPECT_EQ(tools.at(0).at("name").get<std::string>(), "fpga_project_list");
    EXPECT_EQ(tools.at(1).at("name").get<std::string>(), "fpga_project_info");
}

// ------------------------------------------------------------------ 通知

TEST_F(McpServerTest, NotificationWithoutIdIsIgnored) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = "notifications/initialized";
    EXPECT_FALSE(server->Handle(message).has_value());
}

TEST_F(McpServerTest, NullIdIsTreatedAsNotification) {
    Json message = Request("ping", Json::object(), Json(nullptr));
    EXPECT_FALSE(server->Handle(message).has_value());
}

TEST_F(McpServerTest, NonObjectMessageIsInvalidRequest) {
    const auto response = server->Handle(Json("not-an-object"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->at("error").at("code").get<int>(), -32600);
}

TEST_F(McpServerTest, UnknownMethodReturnsMethodNotFound) {
    const Json response = Call(Request("does/not/exist", Json::object()));
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response.at("error").at("code").get<int>(), -32601);
}

TEST_F(McpServerTest, ShutdownReturnsNullResult) {
    const Json response = Call(Request("shutdown", Json::object()));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response.at("result").is_null());
}

TEST_F(McpServerTest, ResourcesAndPromptsAreEmpty) {
    EXPECT_TRUE(Call(Request("resources/list", Json::object()))
                    .at("result")
                    .at("resources")
                    .empty());
    EXPECT_TRUE(Call(Request("resources/templates/list", Json::object()))
                    .at("result")
                    .at("resourceTemplates")
                    .empty());
    EXPECT_TRUE(Call(Request("prompts/list", Json::object())).at("result").at("prompts").empty());
    EXPECT_TRUE(Call(Request("logging/setLevel", Json::object())).at("result").is_object());
}

TEST_F(McpServerTest, BatchMessagesProduceArrayResponse) {
    Json batch = Json::array();
    batch.push_back(Request("ping", Json::object(), Json(1)));
    batch.push_back(Request("ping", Json::object(), Json(2)));
    const auto response = server->Handle(batch);
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->is_array());
    EXPECT_EQ(response->size(), 2u);
    EXPECT_EQ(response->at(1).at("id").get<int>(), 2);
}

// --------------------------------------------------------------- tools/call

TEST_F(McpServerTest, ToolsCallSuccessWrapsContentAndStructuredContent) {
    const Json response = Call(CallRequest("fpga_project_list", Json::object()));
    const Json& result = response.at("result");
    EXPECT_FALSE(result.at("isError").get<bool>());
    ASSERT_EQ(result.at("content").size(), 1u);
    EXPECT_EQ(result.at("content").at(0).at("type").get<std::string>(), "text");
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_TRUE(result.at("structuredContent").at("ok").get<bool>());
}

TEST_F(McpServerTest, ToolsCallPassesArgumentsThrough) {
    Json arguments = Json::object();
    arguments["root"] = "Y:/somewhere";
    (void)Call(CallRequest("fpga_project_list", arguments));
    EXPECT_EQ(dispatcher.last_arguments.at("root").get<std::string>(), "Y:/somewhere");
}

TEST_F(McpServerTest, ToolsCallUnknownToolIsErrorResultNotProtocolError) {
    const Json response = Call(CallRequest("fpga_nope", Json::object()));
    ASSERT_FALSE(response.contains("error")) << "未知工具必须是 isError 结果，不是 -32601";
    const Json& result = response.at("result");
    EXPECT_TRUE(result.at("isError").get<bool>());
    EXPECT_EQ(result.at("structuredContent").at("ok").get<bool>(), false);
    EXPECT_NE(result.at("structuredContent").at("error").get<std::string>().find("Unknown tool"),
              std::string::npos);
    EXPECT_EQ(result.at("content").at(0).at("text").get<std::string>().rfind("Error: ", 0), 0u);
}

TEST_F(McpServerTest, ToolsCallMissingRequiredArgumentFails) {
    // fpga_read_file 要求 file
    const Json response = Call(CallRequest("fpga_read_file", Json::object()));
    const Json& result = response.at("result");
    EXPECT_TRUE(result.at("isError").get<bool>());
    EXPECT_EQ(result.at("structuredContent").at("error").get<std::string>(),
              "Missing required argument: file");
}

TEST_F(McpServerTest, ToolsCallEmptyStringCountsAsMissing) {
    Json arguments = Json::object();
    arguments["file"] = "";
    const Json response = Call(CallRequest("fpga_read_file", arguments));
    EXPECT_TRUE(response.at("result").at("isError").get<bool>());
}

TEST_F(McpServerTest, ToolsCallNullCountsAsMissing) {
    Json arguments = Json::object();
    arguments["file"] = nullptr;
    const Json response = Call(CallRequest("fpga_read_file", arguments));
    EXPECT_TRUE(response.at("result").at("isError").get<bool>());
}

TEST_F(McpServerTest, ToolsCallDisabledFlowToolExplainsTheFlag) {
    McpServerOptions restricted;  // allow_flow = false
    McpServer restricted_server(dispatcher, restricted);
    const auto response =
        restricted_server.Handle(CallRequest("fpga_flow_run", Json::object()));
    ASSERT_TRUE(response.has_value());
    const std::string message =
        response->at("result").at("structuredContent").at("error").get<std::string>();
    EXPECT_NE(message.find("--allow-flow"), std::string::npos) << message;
}

TEST_F(McpServerTest, ToolsCallDisabledWriteToolExplainsTheFlag) {
    McpServerOptions restricted;
    McpServer restricted_server(dispatcher, restricted);
    const auto response =
        restricted_server.Handle(CallRequest("fpga_project_rename", Json::object()));
    ASSERT_TRUE(response.has_value());
    const std::string message =
        response->at("result").at("structuredContent").at("error").get<std::string>();
    EXPECT_NE(message.find("--allow-write"), std::string::npos) << message;
}

TEST_F(McpServerTest, ToolsCallNonObjectResultIsWrappedUnderResult) {
    Json arguments = Json::object();
    arguments["file"] = "top.v";
    const Json response = Call(CallRequest("fpga_read_file", arguments));
    const Json& structured = response.at("result").at("structuredContent");
    ASSERT_TRUE(structured.at("ok").get<bool>());
    EXPECT_EQ(structured.at("result").get<std::string>(), "plain-text");
}

TEST_F(McpServerTest, ToolsCallHugeResultIsTruncated) {
    const Json response = Call(CallRequest("fpga_diagnostics", Json::object()));
    const Json& structured = response.at("result").at("structuredContent");
    EXPECT_FALSE(structured.at("ok").get<bool>());
    EXPECT_TRUE(structured.at("truncated").get<bool>());
    EXPECT_EQ(structured.at("error").get<std::string>(),
              "Tool result too large; pass a smaller limit");
    // 截断后的结果本身必须很小
    const std::string text = response.at("result").at("content").at(0).at("text").get<std::string>();
    EXPECT_LT(text.size(), 2000u);
}

TEST_F(McpServerTest, ToolsCallToolErrorBecomesIsErrorResult) {
    Json arguments = Json::object();
    arguments["query"] = "x";
    const Json response = Call(CallRequest("fpga_search_text", arguments));
    ASSERT_FALSE(response.contains("error"));
    EXPECT_TRUE(response.at("result").at("isError").get<bool>());
    EXPECT_EQ(response.at("result").at("structuredContent").at("error").get<std::string>(), "boom");
}

TEST_F(McpServerTest, ToolsCallUnexpectedExceptionIsCaught) {
    Json arguments = Json::object();
    arguments["file"] = "top.v";
    arguments["line"] = 1;
    arguments["column"] = 1;
    const Json response = Call(CallRequest("fpga_hover", arguments));
    EXPECT_TRUE(response.at("result").at("isError").get<bool>());
    const std::string message =
        response.at("result").at("structuredContent").at("error").get<std::string>();
    EXPECT_NE(message.find("InternalError"), std::string::npos) << message;
    EXPECT_NE(message.find("kaboom"), std::string::npos) << message;
}

TEST_F(McpServerTest, WrapToolResultKeepsArrayShape) {
    Json value = Json::array();
    value.push_back(1);
    value.push_back(2);
    const Json wrapped = server->WrapToolResult(value);
    EXPECT_FALSE(wrapped.at("isError").get<bool>());
    EXPECT_FALSE(wrapped.contains("structuredContent")) << "数组结果不进 structuredContent";
    EXPECT_NE(wrapped.at("content").at(0).at("text").get<std::string>().find('['),
              std::string::npos);
}

TEST_F(McpServerTest, WrapToolErrorTextPrefix) {
    const Json wrapped = McpServer::WrapToolError("nope");
    EXPECT_TRUE(wrapped.at("isError").get<bool>());
    EXPECT_EQ(wrapped.at("content").at(0).at("text").get<std::string>(), "Error: nope");
    EXPECT_EQ(wrapped.at("structuredContent").at("error").get<std::string>(), "nope");
}

// ---------------------------------------------------------------- 进度通知

TEST_F(McpServerTest, ProgressNotificationIsEmittedForProgressToken) {
    Json notifications = Json::array();
    server->SetNotificationWriter(
        [&notifications](const Json& note) { notifications.push_back(note); });

    Json meta = Json::object();
    meta["progressToken"] = "tok-1";
    Json params = Json::object();
    params["name"] = "fpga_flow_wait";
    params["arguments"] = Json::object();
    params["_meta"] = meta;
    (void)Call(Request("tools/call", params));

    ASSERT_EQ(notifications.size(), 1u);
    const Json& note = notifications.at(0);
    EXPECT_EQ(note.at("method").get<std::string>(), "notifications/progress");
    EXPECT_EQ(note.at("params").at("progressToken").get<std::string>(), "tok-1");
    EXPECT_DOUBLE_EQ(note.at("params").at("progress").get<double>(), 0.5);
    EXPECT_EQ(note.at("params").at("message").get<std::string>(), "half way");
}

TEST_F(McpServerTest, ProgressWithoutTokenEmitsNothing) {
    Json notifications = Json::array();
    server->SetNotificationWriter(
        [&notifications](const Json& note) { notifications.push_back(note); });
    (void)Call(CallRequest("fpga_flow_wait", Json::object()));
    EXPECT_TRUE(notifications.empty());
}

TEST_F(McpServerTest, ErrorResultTextCarriesMessage) {
    const Json result = McpServer::WrapToolError("disk full");
    EXPECT_EQ(result.at("content").at(0).at("type").get<std::string>(), "text");
    EXPECT_EQ(result.at("content").at(0).at("text").get<std::string>(), "Error: disk full");
}
// ------------------------------------------------- P-B1：结果表示协商（单表示）

// 默认档：不发 structuredContent，text 是**紧凑** JSON（无缩进、无换行）。
// 这是绝大多数客户端的路径；旧实现会把同一份数据同时写进 text 与 structuredContent，
// 实测占响应 58–64%（doc/design/protocol-optimization.md §B1）。
TEST(McpResultRepresentationTest, DefaultTierIsCompactTextOnly) {
    FakeDispatcher dispatcher;
    McpServerOptions options;  // client_supports_structured = false
    options.allow_flow = true;
    McpServer server(dispatcher, options);

    const auto response = server.Handle(CallRequest("fpga_project_list", Json::object()));
    ASSERT_TRUE(response.has_value());
    const Json& result = response->at("result");
    EXPECT_FALSE(result.contains("structuredContent")) << "默认档不能重复发一份";

    const Json parsed = Json::parse(result.at("content").at(0).at("text").get<std::string>());
    EXPECT_TRUE(parsed.at("ok").get<bool>());
    EXPECT_TRUE(parsed.contains("projects"));

    // 紧凑：除字符串内容外不应出现格式化换行/缩进
    const std::string text = result.at("content").at(0).at("text").get<std::string>();
    EXPECT_EQ(text.find('\n'), std::string::npos);
    EXPECT_EQ(text.find("\n "), std::string::npos);
}

// 协商档：数据进 structuredContent，text 只留一行摘要（不重复数据）。
TEST(McpResultRepresentationTest, StructuredTierSendsSummaryText) {
    FakeDispatcher dispatcher;
    McpServerOptions options;
    options.allow_flow = true;
    options.client_supports_structured = true;
    McpServer server(dispatcher, options);

    const auto response = server.Handle(CallRequest("fpga_project_list", Json::object()));
    ASSERT_TRUE(response.has_value());
    const Json& result = response->at("result");
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_TRUE(result.at("structuredContent").at("ok").get<bool>());
    EXPECT_EQ(result.at("content").at(0).at("text").get<std::string>(), "ok");
}

// initialize 里声明的能力决定档位；没声明（或声明 false）就保持默认档。
TEST(McpResultRepresentationTest, InitializeNegotiatesStructuredCapability) {
    FakeDispatcher dispatcher;
    McpServerOptions options;
    options.allow_flow = true;
    McpServer server(dispatcher, options);

    Json params = Json::object();
    params["protocolVersion"] = "2025-06-18";
    Json capabilities = Json::object();
    Json experimental = Json::object();
    experimental["structuredContent"] = true;
    capabilities["experimental"] = experimental;
    params["capabilities"] = capabilities;

    EXPECT_TRUE(server.Handle(Request("initialize", params)).has_value());
    EXPECT_TRUE(server.structured_results());

    // 同一连接后续调用走结构化档
    const auto response = server.Handle(CallRequest("fpga_project_list", Json::object()));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->at("result").contains("structuredContent"));
}

TEST(McpResultRepresentationTest, InitializeWithoutCapabilityKeepsCompactTier) {
    FakeDispatcher dispatcher;
    McpServerOptions options;
    options.allow_flow = true;
    options.client_supports_structured = true;  // 先打开，验证 initialize 会按声明重置
    McpServer server(dispatcher, options);

    Json params = Json::object();
    params["protocolVersion"] = "2025-06-18";
    params["capabilities"] = Json::object();

    EXPECT_TRUE(server.Handle(Request("initialize", params)).has_value());
    EXPECT_FALSE(server.structured_results());
}
