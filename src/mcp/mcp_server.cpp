// =============================================================================
//  src/mcp/mcp_server.cpp
// =============================================================================
//  逐条移植 fpgalsp/mcp.py 的 McpServer。对照关系：
//    Handle()          <- handle()
//    WrapToolResult()  <- _tool_result()
//    WrapToolError()   <- _error_result()
//    CallTool()        <- _call()（含 required 校验与 ToolError）
// =============================================================================

#include "mcp/mcp_server.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "mcp/tools.h"

namespace fpgalsp::mcp {
namespace {

// Python mcp.py 的 instructions 原文（客户端会展示给用户，逐字复现）
constexpr const char* kInstructions =
    "Tools for Anlogic TangDynasty FPGA projects. Line/column arguments are 1-based. "
    "Pass a project (.al path, directory or name) when the workspace has several projects. "
    "Only read-only tools are enabled unless the server was started with --allow-flow/"
    "--allow-write.";

Json Response(const Json& id, Json result) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

Json ErrorResponse(const Json& id, int code, std::string message) {
    Json error = Json::object();
    error["code"] = code;
    error["message"] = std::move(message);
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = std::move(error);
    return out;
}

// Python: `message.get("id")` 为 None 时视为通知
bool HasId(const Json& message, Json* id) {
    if (!message.is_object()) return false;
    const auto it = message.find("id");
    if (it == message.end() || it->is_null()) return false;
    *id = *it;
    return true;
}

const Json* GetMember(const Json& json, const char* key) {
    if (!json.is_object()) return nullptr;
    const auto it = json.find(key);
    if (it == json.end()) return nullptr;
    return &(*it);
}

bool VersionSupported(const std::string& version) {
    for (const char* supported : McpServer::kSupportedVersions) {
        if (version == supported) return true;
    }
    return false;
}

// P-B1：协商档下 text 只留一行摘要，数据在 structuredContent 里。
// 摘要必须能区分"成功"与"工具自己报告的失败"（后者 isError 仍为 false）。
std::string SummaryOf(const Json& value) {
    if (value.is_object()) {
        const auto ok = value.find("ok");
        if (ok != value.end() && ok->is_boolean() && !ok->get<bool>()) {
            const auto error = value.find("error");
            if (error != value.end() && error->is_string()) {
                return "failed: " + error->get<std::string>();
            }
            return "failed";
        }
        return "ok";
    }
    if (value.is_array()) {
        return "ok: " + std::to_string(value.size()) + " items";
    }
    return "ok";
}

// Python: `arguments[key] in (None, "")`
bool MissingRequired(const Json* value) {
    if (value == nullptr) return true;
    if (value->is_null()) return true;
    if (value->is_string() && value->get<std::string>().empty()) return true;
    return false;
}

}  // namespace

McpServer::McpServer(ToolDispatcher& dispatcher, McpServerOptions options)
    : dispatcher_(dispatcher), options_(std::move(options)) {}

void McpServer::SetNotificationWriter(std::function<void(const Json&)> writer) {
    notification_writer_ = std::move(writer);
}

Json McpServer::WrapToolError(const std::string& message) {
    Json structured = Json::object();
    structured["ok"] = false;
    structured["error"] = message;

    Json content = Json::array();
    Json text_item = Json::object();
    text_item["type"] = "text";
    text_item["text"] = "Error: " + message;
    content.push_back(std::move(text_item));

    Json out = Json::object();
    out["content"] = std::move(content);
    out["structuredContent"] = std::move(structured);
    out["isError"] = true;
    return out;
}

Json McpServer::WrapToolResult(const Json& value_in) const {
    Json value = value_in;
    // Python: 非 dict/list 的结果包成 {"ok": true, "result": value}
    if (!value.is_object() && !value.is_array()) {
        Json wrapped = Json::object();
        wrapped["ok"] = true;
        wrapped["result"] = std::move(value);
        value = std::move(wrapped);
    }

    // P-B1：只发一份数据。
    //   默认档：紧凑 JSON 放进 text（旧客户端照旧读 text；JSON 空白无语义）。
    //   协商档：**对象**结果进 structuredContent，text 只留一行摘要。
    //   数组结果在任何档位都只能走 text（structuredContent 我们的约束是对象），
    //   否则它会变成一句 "ok: N items" 而数据丢失。
    // 以前的实现两档都发全量 → 实测占响应 58–64%。
    const std::string compact = value.dump(-1, ' ', false, Json::error_handler_t::replace);
    const bool structured_payload = options_.client_supports_structured && value.is_object();
    std::string text = structured_payload ? SummaryOf(value) : compact;
    // 上限按**载荷**算，不能按 text 算：协商档的 text 是一行摘要，
    // 按它算会让超大 structuredContent 直接漏过去（客户端解析器会崩）。
    if (compact.size() > kMaxToolResultBytes) {
        Json too_large = Json::object();
        too_large["ok"] = false;
        too_large["error"] = "Tool result too large; pass a smaller limit";
        too_large["truncated"] = true;
        value = std::move(too_large);
        text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    Json content = Json::array();
    Json text_item = Json::object();
    text_item["type"] = "text";
    text_item["text"] = std::move(text);
    content.push_back(std::move(text_item));

    Json out = Json::object();
    out["content"] = std::move(content);
    out["isError"] = false;
    if (structured_payload && value.is_object()) {
        out["structuredContent"] = std::move(value);
    }
    return out;
}

Json McpServer::CallTool(const Json& params) {
    const Json* name_member = GetMember(params, "name");
    const std::string name = (name_member != nullptr && name_member->is_string())
                                 ? name_member->get<std::string>()
                                 : std::string();

    const Tool* tool = FindTool(name);
    if (tool == nullptr) throw ToolError("Unknown tool: " + name);

    if (tool->requires_flag == "flow" && !options_.allow_flow) {
        throw ToolError("Tool " + name + " is disabled; start the MCP server with --allow-flow");
    }
    if (tool->requires_flag == "write" && !options_.allow_write) {
        throw ToolError("Tool " + name + " is disabled; start the MCP server with --allow-write");
    }

    Json arguments = Json::object();
    if (const Json* args = GetMember(params, "arguments"); args != nullptr && args->is_object()) {
        arguments = *args;
    }

    // required 校验（Python 在调用 handler 之前做，这里保持一致）
    if (const Json* required = GetMember(tool->input_schema, "required");
        required != nullptr && required->is_array()) {
        for (const Json& key : *required) {
            if (!key.is_string()) continue;
            const std::string key_name = key.get<std::string>();
            if (MissingRequired(GetMember(arguments, key_name.c_str()))) {
                throw ToolError("Missing required argument: " + key_name);
            }
        }
    }

    // 进度回调：只有带 progressToken 的调用才接（对应 Python _progress_callback）
    bool has_progress_token = false;
    if (const Json* meta = GetMember(params, "_meta"); meta != nullptr && meta->is_object()) {
        const Json* token = GetMember(*meta, "progressToken");
        has_progress_token = token != nullptr && !token->is_null();
        if (has_progress_token && notification_writer_ && token->is_string()) {
            const std::string token_text = token->get<std::string>();
            dispatcher_.SetProgressEmitter([this, token_text](double progress,
                                                              const std::string& message) {
                Json params_json = Json::object();
                params_json["progressToken"] = token_text;
                params_json["progress"] = progress;
                if (!message.empty()) params_json["message"] = message;
                Json note = Json::object();
                note["jsonrpc"] = "2.0";
                note["method"] = "notifications/progress";
                note["params"] = std::move(params_json);
                notification_writer_(note);
            });
        } else if (has_progress_token && notification_writer_ && token->is_number()) {
            const double token_number = token->get<double>();
            dispatcher_.SetProgressEmitter([this, token_number](double progress,
                                                                const std::string& message) {
                Json params_json = Json::object();
                params_json["progressToken"] = token_number;
                params_json["progress"] = progress;
                if (!message.empty()) params_json["message"] = message;
                Json note = Json::object();
                note["jsonrpc"] = "2.0";
                note["method"] = "notifications/progress";
                note["params"] = std::move(params_json);
                notification_writer_(note);
            });
        } else {
            dispatcher_.SetProgressEmitter({});
        }
    } else {
        dispatcher_.SetProgressEmitter({});
    }

    return dispatcher_.Call(tool->handler, arguments);
}

std::optional<Json> McpServer::Handle(const Json& message) {
    // MCP 允许批量数组
    if (message.is_array()) {
        Json responses = Json::array();
        for (const Json& item : message) {
            if (auto response = HandleSingle(item)) responses.push_back(std::move(*response));
        }
        if (responses.empty()) return std::nullopt;
        return responses;
    }
    return HandleSingle(message);
}

std::optional<Json> McpServer::HandleSingle(const Json& message) {
    if (!message.is_object()) {
        return ErrorResponse(Json(nullptr), -32600, "Invalid Request");
    }

    const Json* method_member = GetMember(message, "method");
    if (method_member == nullptr || !method_member->is_string()) {
        return std::nullopt;  // 不是请求（Python: method is None -> None）
    }
    const std::string method = method_member->get<std::string>();

    Json id = Json(nullptr);
    if (!HasId(message, &id)) {
        // 通知（initialized / cancelled / progress / exit 等）一律不应答
        return std::nullopt;
    }

    Json params = Json::object();
    if (const Json* params_member = GetMember(message, "params");
        params_member != nullptr && params_member->is_object()) {
        params = *params_member;
    }

    if (method == "initialize") {
        std::string requested = "2024-11-05";
        if (const Json* version = GetMember(params, "protocolVersion");
            version != nullptr && version->is_string() && !version->get<std::string>().empty()) {
            requested = version->get<std::string>();
        }
        if (!VersionSupported(requested)) requested = kSupportedVersions[0];

        Json tools_cap = Json::object();
        tools_cap["listChanged"] = false;
        Json capabilities = Json::object();
        capabilities["tools"] = std::move(tools_cap);

        Json server_info = Json::object();
        server_info["name"] = "fpga-lsp-mcp";
        server_info["version"] = options_.server_version;

        Json result = Json::object();
        result["protocolVersion"] = requested;
        result["capabilities"] = std::move(capabilities);
        result["serverInfo"] = std::move(server_info);
        result["instructions"] = kInstructions;

        // P-B1：只有客户端显式声明才走 structuredContent 档。
        // 不按协议版本自动开启 —— 说 2025-06-18 但不声明能力的客户端仍然只读 text。
        const Json* client_caps = GetMember(params, "capabilities");
        const Json* experimental =
            client_caps != nullptr ? GetMember(*client_caps, "experimental") : nullptr;
        const Json* structured =
            experimental != nullptr ? GetMember(*experimental, "structuredContent") : nullptr;
        options_.client_supports_structured =
            structured != nullptr && structured->is_boolean() && structured->get<bool>();
        return Response(id, std::move(result));
    }

    if (method == "ping") return Response(id, Json::object());

    if (method == "tools/list") {
        Json result = Json::object();
        result["tools"] = VisibleTools(options_.allow_flow, options_.allow_write);
        return Response(id, std::move(result));
    }

    if (method == "resources/list") {
        Json result = Json::object();
        result["resources"] = Json::array();
        return Response(id, std::move(result));
    }
    if (method == "resources/templates/list") {
        Json result = Json::object();
        result["resourceTemplates"] = Json::array();
        return Response(id, std::move(result));
    }
    if (method == "prompts/list") {
        Json result = Json::object();
        result["prompts"] = Json::array();
        return Response(id, std::move(result));
    }
    if (method == "logging/setLevel") return Response(id, Json::object());

    if (method == "tools/call") {
        try {
            return Response(id, WrapToolResult(CallTool(params)));
        } catch (const ToolError& error) {
            return Response(id, WrapToolError(error.what()));
        } catch (const std::exception& error) {
            // Python: `"%s: %s" % (type(exc).__name__, exc)` —— C++ 没有稳定的类型名，
            // 用 InternalError 前缀，保证客户端看到的是"工具失败"而不是协议错误。
            return Response(id, WrapToolError("InternalError: " + std::string(error.what())));
        } catch (...) {
            return Response(id, WrapToolError("InternalError: unknown exception"));
        }
    }

    if (method == "shutdown") return Response(id, Json(nullptr));

    return ErrorResponse(id, -32601, "Method not found: " + method);
}

}  // namespace fpgalsp::mcp