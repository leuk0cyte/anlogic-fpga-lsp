// =============================================================================
//  src/jsonrpc/dispatcher.h —— 方法名 -> handler 注册表 + 同步 dispatch
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\lsp.py :: Server._dispatch
//
//  关键契约：
//   * handler 抛异常 **不能** 杀死进程：捕获后转成 -32603 响应并记日志。
//   * notification（无 id）不产生任何响应，但 handler 照常执行。
//   * "$/" 前缀（LSP 自定义方法）静默忽略，不回错误。
//   * "exit" 由调用方（serve 循环）处理，Dispatcher 只做通用分派。
// =============================================================================

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"

namespace fpgalsp::jsonrpc {

// handler 的返回值：要么是 result，要么是显式的 JSON-RPC error。
struct HandlerResult {
    bool is_error = false;
    Json result;
    int error_code = 0;
    std::string error_message;
    Json error_data;

    static HandlerResult Ok(Json value = Json()) {
        HandlerResult r;
        r.result = std::move(value);
        return r;
    }
    static HandlerResult Error(int code, std::string message, Json data = Json()) {
        HandlerResult r;
        r.is_error = true;
        r.error_code = code;
        r.error_message = std::move(message);
        r.error_data = std::move(data);
        return r;
    }
};

using Handler = std::function<HandlerResult(const Json& params)>;

// 一条消息被处理后的产出
struct DispatchOutcome {
    // 是否有响应要写回（notification 不产生响应）
    // 其中可能的响应由 dispatcher 内部直接通过 transport 写出。
};

class Dispatcher {
public:
    explicit Dispatcher(TransportBaseCommon& transport);

    // 注册/覆盖一个方法。
    void Register(std::string method, Handler handler);

    // 是否已注册
    bool Has(std::string_view method) const;

    // 已注册方法名（测试与 tools/list 用）
    std::vector<std::string> Methods() const;

    // 设置一个「未知方法」的回退 handler（MCP 需要返回 tools/list 等固定集合时用）。
    void SetFallback(Handler handler);

    // 处理一条已解析的消息；把响应（如果有）写到 transport。
    // 返回值表示这条消息是否产生了响应。
    // never-raise：内部吞掉 handler 的所有异常。
    bool Dispatch(const Json& message);

    // 处理一条无法解析的帧：回 -32700 ParseError（id 为 null）。
    void SendParseError(std::string_view detail = {});

private:
    TransportBaseCommon& transport_;
    std::vector<std::pair<std::string, Handler>> handlers_;
    std::optional<Handler> fallback_;
};

}  // namespace fpgalsp::jsonrpc