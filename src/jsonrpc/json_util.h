// =============================================================================
//  src/jsonrpc/json_util.h —— 从 nlohmann::json 安全取值（绝不抛异常）
// =============================================================================
//  后续 project/td/reports/constraints 的解析全部建立在这里：
//  外部输入（TD 输出、模型返回、LSP 客户端消息）都不可信，取值必须是
//  「缺失 / 类型不符 -> 返回默认值」，而不是抛 out_of_range / type_error。
// =============================================================================

#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace fpgalsp::jsonrpc {

using Json = nlohmann::json;

// ---- 取值：缺失或类型不符返回默认值 ----

int GetInt(const Json& obj, std::string_view key, int default_value = 0);
int64_t GetInt64(const Json& obj, std::string_view key, int64_t default_value = 0);
double GetDouble(const Json& obj, std::string_view key, double default_value = 0.0);
bool GetBool(const Json& obj, std::string_view key, bool default_value = false);
std::string GetString(const Json& obj, std::string_view key, std::string default_value = {});

// 取子对象；不是 object 时返回空的 object（而不是抛异常）
Json GetObject(const Json& obj, std::string_view key);
// 取数组；不是 array 时返回空的 array
Json GetArray(const Json& obj, std::string_view key);

// 取嵌套路径，例如 GetPath(root, {"params", "textDocument", "uri"})
std::string GetStringPath(const Json& root, std::initializer_list<std::string_view> path,
                          std::string default_value = {});

// ---- 判定 ----

bool HasKey(const Json& obj, std::string_view key);
bool IsObject(const Json& value);
bool IsArray(const Json& value);
bool IsString(const Json& value);
bool IsNumber(const Json& value);

// key 存在且不是 null
bool HasNonNull(const Json& obj, std::string_view key);

// 取 id 字段：JSON-RPC 的 id 可以是 number / string / null。
// 返回 nullopt 表示「没有 id 或 id 为 null」—— 即这是一条 notification。
struct MessageId {
    bool is_string = false;
    std::string string_value;
    int64_t int_value = 0;

    Json ToJson() const;
};

std::optional<MessageId> GetMessageId(const Json& message);

// ---- 构造 ----

// 成功响应
Json MakeResult(const MessageId& id, Json result);
// 错误响应
Json MakeError(std::string_view id_json, int code, std::string_view message, Json data = Json());
// 用 MessageId 构造错误响应
Json MakeError(const MessageId& id, int code, std::string_view message, Json data = Json());
// notification（无 id）
Json MakeNotification(std::string_view method, Json params);

// 序列化：MCP 模式必须用紧凑形式（body 内不能有裸换行）。
// compact=true  -> separators (",", ":") 且不缩进（对应 mcp.py 的 _write）
// compact=false -> 等价的 LSP 风格（默认 json.dumps 语义）
std::string Serialize(const Json& value, bool compact);

}  // namespace fpgalsp::jsonrpc