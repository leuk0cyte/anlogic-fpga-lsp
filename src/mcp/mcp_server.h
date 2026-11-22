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

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <array>
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

    // ---- P-B6：MCP resources ----
    // 只读数据（工程信息 / 文件清单 / 报告摘要 / flow 状态）以 resource 形式暴露，
    // agent 可以直接 resources/read，不必为每个查询定义一个工具、也不必把大 JSON
    // 塞进 tools/call 的 800 KB 信封里。
    // 默认空实现：不知道 resources 的 dispatcher 保持旧行为（resources/list 为空）。
    virtual Json ListResources() { return Json::array(); }
    // 找不到返回 nullopt（服务端回 -32602）。
    virtual std::optional<std::string> ReadResource(std::string_view uri) {
        (void)uri;
        return std::nullopt;
    }

    // P-B4：取消令牌。服务端收到 `notifications/cancelled` 时置位；
    // 长任务（flow_wait / search_text / rename_edits）在循环边界自查。
    virtual void SetCancelToken(std::shared_ptr<std::atomic<bool>> token) {
        cancel_ = std::move(token);
    }

  protected:
    std::function<void(double, const std::string&)> progress_;
    // 协作式取消：为空时永不取消（同步单测/旧行为）。
    [[nodiscard]] bool Cancelled() const { return cancel_ != nullptr && cancel_->load(); }
    std::shared_ptr<std::atomic<bool>> cancel_;
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
    static constexpr std::array<const char*, 3> kSupportedVersions = {"2024-11-05",
                                                                      "2025-03-26",
                                                                      "2025-06-18"};

    McpServer(ToolDispatcher& dispatcher, McpServerOptions options);
    ~McpServer();

    // 通知外发通道（notifications/progress 用）。生产环境接 transport.WriteMessage。
    void SetNotificationWriter(std::function<void(const Json&)> writer);

    // 处理一条已解码消息；nullopt = 无需应答（通知）。
    // 输入可以是单个对象，也可以是批量数组（MCP 允许）。
    std::optional<Json> Handle(const Json& message);

    // P-B4：**给 serve 循环用**的入口。
    // `tools/call` 交给内部单 worker 执行，读循环继续跑 —— 否则客户端根本没机会
    // 发 `notifications/cancelled`（读循环被工具占着，取消消息永远进不来）。
    // 通知（含取消）在这里直接处理；其余方法等价于 Handle()。
    // 异步执行的结果通过 writer 写出（transport 写是线程安全的）。
    std::optional<Json> HandleRouted(const Json& message,
                                     const std::function<void(const Json&)>& writer);

    [[nodiscard]] bool allow_flow() const { return options_.allow_flow; }
    [[nodiscard]] bool allow_write() const { return options_.allow_write; }

    // 结果包装（供单测直接验证截断/错误语义）
    // offset_hint：本次请求已跳过的条目数（由 arguments.offset 传入），
    // 用于让裁剪后的 nextOffset 是**绝对**偏移（P-B2）。
    [[nodiscard]] Json WrapToolResult(const Json& value, std::size_t offset_hint = 0) const;
    static Json WrapToolError(const std::string& message);

    // P-B2：分页式裁剪。把最大的数组裁到 limit 字节以内，并在对象上加
    // {truncated:true, total, nextOffset}；找不到可裁的数组时返回 null（走旧行为）。
    // 纯函数，单测直接覆盖。
    static Json TruncatePayload(const Json& value, std::size_t limit, std::size_t offset_hint);

    // 协商档是否开启（单测/诊断用）
    [[nodiscard]] bool structured_results() const { return options_.client_supports_structured; }

  private:
    std::optional<Json> HandleSingle(const Json& message);
    Json CallTool(const Json& params);

    // ---- P-B4：异步 tools/call + 取消 ----
    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    void CancelRequest(const Json& request_id);

    ToolDispatcher& dispatcher_;
    McpServerOptions options_;
    std::function<void(const Json&)> notification_writer_;

    std::thread worker_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;

    std::mutex inflight_mutex_;
    // requestId(dump 后的字符串) -> 取消令牌
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> inflight_;
};

}  // namespace fpgalsp::mcp