// =============================================================================
//  src/mcp/mcp_server.h —— MCP 协议层（newline-delimited JSON-RPC）
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\mcp.py 的 McpServer（第 290-470 行）
//
//  ---- 分层 ----
//    这一层只管**协议**：消息分派、版本协商、required 参数校验、结果包装与截断、
//    错误语义。具体工具实现在 ToolDispatcher（生产实现是 mcp::Bridge）。
//    这样协议的正确性可以脱离 TD / 工程 / 文件系统单独测试（tests/unit/mcp_server_test.cpp）。
//
//  ---- 三条容易搞错的协议语义 ----
//  1. **工具失败不是 JSON-RPC error**。
//     Python 的 ToolError 会被包成 `isError: true` 的**结果**（带 content 和
//     structuredContent），而不是 `{"error": {...}}`。客户端（Claude Desktop 等）
//     依赖这个区分"工具返回了错误"和"协议层出错"。
//     未知工具名同样是 ToolError（`Unknown tool: X`），不是 -32601。
//  2. **通知不应答**：没有 id（或 id 为 null）的消息一律返回 nullopt。
//  3. **结果截断**：序列化后 UTF-8 字节数 > 800000 时，整个结果替换成
//     `{ok:false, error:"Tool result too large; pass a smaller limit", truncated:true}`。
//     不截断会让客户端解析器直接崩（MCP 客户端普遍有帧大小上限）。
// =============================================================================

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fpgalsp::mcp {

using Json = nlohmann::json;

// 工具实现侧的**可预期**失败（对应 Python 的 ToolError）：
// 会被包成 isError 结果，不打 traceback。
class ToolError : public std::runtime_error {
  public:
    explicit ToolError(const std::string& message) : std::runtime_error(message) {}
};

// 工具实现接口。抛 ToolError = 可预期错误；抛其它异常 = 实现缺陷（也会被兜住）。
class ToolDispatcher {
  public:
    virtual ~ToolDispatcher() = default;

    virtual Json Call(std::string_view handler, const Json& arguments) = 0;

    // 长任务进度上报（对应 Python 的 progress 参数）。
    // emitter(progress, message)；没有 progressToken 时 server 传空函数。
    virtual void SetProgressEmitter(std::function<void(double, const std::string&)> emitter) {
        progress_ = std::move(emitter);
    }

  protected:
    std::function<void(double, const std::string&)> progress_;
};

struct McpServerOptions {
    bool allow_flow = false;
    bool allow_write = false;
    std::string server_version = "0.1.0";
    // P-B1：结果表示协商。
    //   false（默认）= 只发 `content[0].text`，且是**紧凑** JSON（无缩进）——
    //                  对未知客户端零回归，同时省掉 structuredContent 那一份。
    //   true          = 发 `structuredContent` + 一行摘要 text（由
    //                  initialize.params.capabilities.experimental.structuredContent 打开）。
    // 两种档位都不会把同一份数据发两遍。
    bool client_supports_structured = false;
};

class McpServer {
  public:
    // 与 Python mcp.MAX_TOOL_RESULT_BYTES 一致
    static constexpr std::size_t kMaxToolResultBytes = 800000;
    // 支持的协议版本（第一个是回落默认值）
    static constexpr const char* kSupportedVersions[3] = {"2024-11-05", "2025-03-26",
                                                          "2025-06-18"};

    McpServer(ToolDispatcher& dispatcher, McpServerOptions options);

    // 通知外发通道（notifications/progress 用）。生产环境接 transport.WriteMessage。
    void SetNotificationWriter(std::function<void(const Json&)> writer);

    // 处理一条已解码消息；nullopt = 无需应答（通知）。
    // 输入可以是单个对象，也可以是批量数组（MCP 允许）。
    std::optional<Json> Handle(const Json& message);

    bool allow_flow() const { return options_.allow_flow; }
    bool allow_write() const { return options_.allow_write; }

    // 结果包装（供单测直接验证截断/错误语义）
    Json WrapToolResult(const Json& value) const;
    static Json WrapToolError(const std::string& message);

    // 协商档是否开启（单测/诊断用）
    bool structured_results() const { return options_.client_supports_structured; }

  private:
    std::optional<Json> HandleSingle(const Json& message);
    Json CallTool(const Json& params);

    ToolDispatcher& dispatcher_;
    McpServerOptions options_;
    std::function<void(const Json&)> notification_writer_;
};

}  // namespace fpgalsp::mcp