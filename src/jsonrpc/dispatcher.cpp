// =============================================================================
//  src/jsonrpc/dispatcher.cpp
// =============================================================================

#include "jsonrpc/dispatcher.h"

#include <string>
#include <utility>

#include "platform/log.h"

namespace fpgalsp::jsonrpc {

Dispatcher::Dispatcher(TransportBaseCommon& transport) : transport_(transport) {}

void Dispatcher::Register(std::string method, Handler handler) {
    for (auto& [name, existing] : handlers_) {
        if (name == method) {
            existing = std::move(handler);
            return;
        }
    }
    handlers_.emplace_back(std::move(method), std::move(handler));
}

bool Dispatcher::Has(std::string_view method) const {
    for (const auto& [name, _] : handlers_) {
        if (name == method) return true;
    }
    return false;
}

std::vector<std::string> Dispatcher::Methods() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& [name, _] : handlers_) out.push_back(name);
    return out;
}

void Dispatcher::SetFallback(Handler handler) { fallback_ = std::move(handler); }

bool Dispatcher::Dispatch(const Json& message) {
    // 对应 lsp.py：msg 必须是个 object；否则 InvalidRequest
    if (!message.is_object()) {
        transport_.WriteMessage(MakeError(std::string(), kInvalidRequest, "Invalid Request"));
        return true;
    }

    const std::string method = GetString(message, "method");
    const auto id = GetMessageId(message);

    // 没有 method：这是对「服务器 -> 客户端」请求的响应。当前服务器不发这类
    // 请求，直接忽略（对应 lsp.py 的 `if method is None: return`）。
    if (!HasKey(message, "method")) {
        return false;
    }

    // "$/" 前缀（LSP 自定义方法）静默忽略
    if (method.rfind("$/", 0) == 0) {
        return false;
    }

    const Json params = GetObject(message, "params");

    // 找 handler
    const Handler* handler = nullptr;
    for (const auto& [name, fn] : handlers_) {
        if (name == method) {
            handler = &fn;
            break;
        }
    }

    if (handler == nullptr) {
        if (fallback_.has_value()) {
            handler = &fallback_.value();
        } else {
            // 方法不存在：只有带 id 的请求才回 -32601，notification 静默丢弃
            if (id.has_value()) {
                transport_.WriteMessage(MakeError(
                    *id, kMethodNotFound, "unsupported method: " + method));
                return true;
            }
            return false;
        }
    }

    platform::default_logger().debug("recv " + method);

    HandlerResult result;
    try {
        result = (*handler)(params);
    } catch (const std::exception& exc) {
        // 关键：一个坏消息绝不能杀死服务（对应 lsp.py 的 except Exception）
        platform::default_logger().error("handler " + method + " crashed: " + exc.what());
        if (id.has_value()) {
            transport_.WriteMessage(MakeError(*id, kInternalError,
                                              std::string("exception: ") + exc.what()));
            return true;
        }
        return false;
    } catch (...) {
        platform::default_logger().error("handler " + method + " crashed: unknown exception");
        if (id.has_value()) {
            transport_.WriteMessage(MakeError(*id, kInternalError, "exception: unknown"));
            return true;
        }
        return false;
    }

    // notification（无 id）：执行完不响应
    if (!id.has_value()) {
        return false;
    }

    if (result.is_error) {
        transport_.WriteMessage(
            MakeError(*id, result.error_code, result.error_message, result.error_data));
    } else {
        transport_.WriteMessage(MakeResult(*id, std::move(result.result)));
    }
    return true;
}

std::optional<HandlerResult> Dispatcher::Invoke(std::string_view method,
                                                const Json& params) const {
    const Handler* handler = nullptr;
    for (const auto& [name, fn] : handlers_) {
        if (name == method) {
            handler = &fn;
            break;
        }
    }
    if (handler == nullptr) {
        if (!fallback_.has_value()) return std::nullopt;
        handler = &fallback_.value();
    }
    return (*handler)(params);
}

void Dispatcher::SendParseError(std::string_view detail) {
    transport_.WriteMessage(MakeError(std::string(), kParseError,
                                      detail.empty() ? "Parse error" : std::string(detail)));
}

}  // namespace fpgalsp::jsonrpc