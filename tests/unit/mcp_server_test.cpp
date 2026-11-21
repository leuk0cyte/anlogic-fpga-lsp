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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
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

TEST_F(McpServerTest, ToolsListAllModesReturns50) {
    const Json response = Call(Request("tools/list", Json::object()));
    EXPECT_EQ(response.at("result").at("tools").size(), 50u);
}

TEST_F(McpServerTest, ToolsListDefaultHidesFlowAndWriteTools) {
    McpServerOptions restricted;
    McpServer restricted_server(dispatcher, restricted);
    const Json response = *restricted_server.Handle(Request("tools/list", Json::object()));
    const Json& tools = response.at("result").at("tools");
    EXPECT_EQ(tools.size(), 38u);
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
    ASSERT_GE(tools.size(), 3u);
    // 表头刻意是"说明书"：agent 第一眼应该看到 fpga_guide，再是工程类工具。
    EXPECT_EQ(tools.at(0).at("name").get<std::string>(), "fpga_guide");
    EXPECT_EQ(tools.at(1).at("name").get<std::string>(), "fpga_project_list");
    EXPECT_EQ(tools.at(2).at("name").get<std::string>(), "fpga_project_info");
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

TEST_F(McpServerTest, ResourcesAreEmptyButPromptsAreProvided) {
    EXPECT_TRUE(Call(Request("resources/list", Json::object()))
                    .at("result")
                    .at("resources")
                    .empty());
    EXPECT_TRUE(Call(Request("resources/templates/list", Json::object()))
                    .at("result")
                    .at("resourceTemplates")
                    .empty());
    // prompts/list 不再是空数组：4 个工作流模板，供客户端一键把正确流程喂给模型
    const Json prompts =
        Call(Request("prompts/list", Json::object())).at("result").at("prompts");
    EXPECT_EQ(prompts.size(), 4u);
    EXPECT_NE(Call(Request("prompts/get", Json::object({{"name", "probe-on-board"}})))
                  .at("result")
                  .at("messages")
                  .size(),
              0u);
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

// ------------------------------------------------- P-B4：异步 tools/call + 取消

namespace {

// 会一直阻塞到被取消的假工具：模拟 flow_wait / 长扫描。
class BlockingDispatcher : public ToolDispatcher {
  public:
    Json Call(std::string_view handler, const Json& arguments) override {
        (void)handler;
        (void)arguments;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            started_ = true;
        }
        started_cv_.notify_all();
        for (int i = 0; i < 300; ++i) {  // 最多 3 s，避免测试挂死
            if (Cancelled()) throw ToolError("cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Json out = Json::object();
        out["ok"] = true;
        return out;
    }

    void WaitStarted() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait_for(lock, std::chrono::seconds(2), [this]() { return started_; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable started_cv_;
    bool started_ = false;
};

Json CancelRequest(const Json& request_id) {
    Json params = Json::object();
    params["requestId"] = request_id;
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = "notifications/cancelled";
    message["params"] = params;
    return message;
}

}  // namespace

TEST(McpCancelTest, RoutedCallIsAsyncAndCancellable) {
    BlockingDispatcher dispatcher;
    McpServerOptions options;
    options.allow_flow = true;
    McpServer server(dispatcher, options);

    std::mutex output_mutex;
    std::condition_variable output_cv;
    std::vector<Json> written;
    const auto writer = [&](const Json& message) {
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            written.push_back(message);
        }
        output_cv.notify_all();
    };

    const Json call = CallRequest("fpga_flow_wait", Json::object(), Json(7));
    const auto immediate = server.HandleRouted(call, writer);
    EXPECT_FALSE(immediate.has_value()) << "tools/call 必须交给 worker，读循环不能被占住";

    dispatcher.WaitStarted();
    // 读循环此刻仍在跑 —— 取消通知能被立刻处理（旧实现里这条消息永远进不来）
    const auto cancelled = server.HandleRouted(CancelRequest(Json(7)), writer);
    EXPECT_FALSE(cancelled.has_value());

    {
        std::unique_lock<std::mutex> lock(output_mutex);
        ASSERT_TRUE(output_cv.wait_for(lock, std::chrono::seconds(3),
                                       [&]() { return !written.empty(); }))
            << "取消后 worker 没有写出响应";
        ASSERT_EQ(written.size(), 1u);
        const Json& result = written[0].at("result");
        EXPECT_TRUE(result.at("isError").get<bool>());
        EXPECT_NE(result.at("content").at(0).at("text").get<std::string>().find("cancelled"),
                  std::string::npos);
    }
}

TEST(McpCancelTest, NonToolMethodsStaySynchronous) {
    BlockingDispatcher dispatcher;
    McpServerOptions options;
    McpServer server(dispatcher, options);
    const auto writer = [](const Json&) {};

    Json ping = Json::object();
    ping["jsonrpc"] = "2.0";
    ping["id"] = 1;
    ping["method"] = "ping";
    const auto response = server.HandleRouted(ping, writer);
    ASSERT_TRUE(response.has_value()) << "非 tools/call 仍然同步返回";
    EXPECT_EQ(response->at("result"), Json::object());
}

// ------------------------------------------------- P-B2：分页式裁剪

TEST(McpTruncationTest, ObjectArrayIsPagedInsteadOfDropped) {
    Json value = Json::object();
    value["ok"] = true;
    value["symbols"] = Json::array();
    for (int i = 0; i < 20000; ++i) {
        Json item = Json::object();
        item["name"] = "symbol_" + std::to_string(i);
        item["containerName"] = "some_container_name_to_make_items_bigger";
        value["symbols"].push_back(std::move(item));
    }
    ASSERT_GT(value.dump().size(), McpServer::kMaxToolResultBytes);

    const Json trimmed = McpServer::TruncatePayload(value, McpServer::kMaxToolResultBytes, 0);
    ASSERT_FALSE(trimmed.is_null());
    EXPECT_TRUE(trimmed.at("truncated").get<bool>());
    EXPECT_EQ(trimmed.at("total").get<std::size_t>(), 20000u);
    EXPECT_EQ(trimmed.at("itemsField").get<std::string>(), "symbols");
    const std::size_t kept = trimmed.at("symbols").size();
    EXPECT_GT(kept, 0u);
    EXPECT_LT(kept, 20000u);
    EXPECT_EQ(trimmed.at("nextOffset").get<std::size_t>(), kept);
    EXPECT_LT(trimmed.dump().size(), McpServer::kMaxToolResultBytes);

    // 偏移量是绝对的：第二次裁剪从 nextOffset 继续
    const Json again = McpServer::TruncatePayload(value, McpServer::kMaxToolResultBytes, kept);
    EXPECT_EQ(again.at("nextOffset").get<std::size_t>(), kept + again.at("symbols").size());
}

TEST(McpTruncationTest, RootArrayIsWrappedAndPaged) {
    Json value = Json::array();
    for (int i = 0; i < 20000; ++i) {
        Json item = Json::object();
        item["path"] = "src/some/rather/long/path/file_" + std::to_string(i) + ".v";
        value.push_back(std::move(item));
    }
    const Json trimmed = McpServer::TruncatePayload(value, McpServer::kMaxToolResultBytes, 0);
    ASSERT_FALSE(trimmed.is_null());
    EXPECT_TRUE(trimmed.at("ok").get<bool>());
    EXPECT_TRUE(trimmed.at("truncated").get<bool>());
    EXPECT_EQ(trimmed.at("total").get<std::size_t>(), 20000u);
    EXPECT_GT(trimmed.at("items").size(), 0u);
}

TEST(McpTruncationTest, PayloadWithoutArrayCannotBePaged) {
    Json value = Json::object();
    value["ok"] = true;
    value["blob"] = std::string(900000, 'x');
    EXPECT_TRUE(McpServer::TruncatePayload(value, McpServer::kMaxToolResultBytes, 0).is_null())
        << "没有数组可裁时应返回 null，让调用方回退到旧的整份丢弃";
}

// ------------------------------------------------- P-B6：MCP resources

namespace {

class ResourceDispatcher : public FakeDispatcher {
  public:
    Json ListResources() override {
        Json out = Json::array();
        Json item = Json::object();
        item["uri"] = "fpgalsp://project/info";
        item["name"] = "Project info";
        item["mimeType"] = "application/json";
        out.push_back(std::move(item));
        return out;
    }

    std::optional<std::string> ReadResource(std::string_view uri) override {
        if (uri != "fpgalsp://project/info") return std::nullopt;
        return std::string("{\"ok\":true}");
    }
};

}  // namespace

TEST(McpResourceTest, ListAndReadGoThroughTheDispatcher) {
    ResourceDispatcher dispatcher;
    McpServerOptions options;
    McpServer server(dispatcher, options);

    const auto listed = server.Handle(Request("resources/list", Json::object()));
    ASSERT_TRUE(listed.has_value());
    const Json& resources = listed->at("result").at("resources");
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].at("uri").get<std::string>(), "fpgalsp://project/info");

    Json read_params = Json::object();
    read_params["uri"] = "fpgalsp://project/info";
    const auto read = server.Handle(Request("resources/read", read_params));
    ASSERT_TRUE(read.has_value());
    const Json& contents = read->at("result").at("contents");
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(contents[0].at("text").get<std::string>(), "{\"ok\":true}");
    EXPECT_EQ(contents[0].at("mimeType").get<std::string>(), "application/json");
}

TEST(McpResourceTest, UnknownResourceIsInvalidParams) {
    ResourceDispatcher dispatcher;
    McpServerOptions options;
    McpServer server(dispatcher, options);

    Json params = Json::object();
    params["uri"] = "fpgalsp://nope";
    const auto response = server.Handle(Request("resources/read", params));
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->contains("error"));
    EXPECT_EQ(response->at("error").at("code").get<int>(), -32602);
}
