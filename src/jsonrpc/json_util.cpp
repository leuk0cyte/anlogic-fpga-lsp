// =============================================================================
//  src/jsonrpc/json_util.cpp
// =============================================================================

#include "jsonrpc/json_util.h"

#include <string>

namespace fpgalsp::jsonrpc {
namespace {

// nlohmann::json 的 operator[] 对非 object 会抛（const 版本在 key 不存在时抛
// out_of_range），所以统一先做类型判定再取。
const Json* FindMember(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(std::string(key));
    if (it == obj.end()) return nullptr;
    return &(*it);
}

// 把 compact JSON（dump(-1) 的产物）转成 Python json.dumps 的默认单行风格：
// 对象/数组元素之间是 ", "，键值之间是 ": "。
//
// 必须跳过字符串字面量——字符串内部的 "," 和 ":" 不能被改写，
// 否则会破坏数据（例如 {"s": "a,b"}）。
std::string AddPythonStyleSpaces(const std::string& compact) {
    std::string out;
    out.reserve(compact.size() + compact.size() / 8);

    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = 0; i < compact.size(); ++i) {
        const char c = compact[i];

        if (in_string) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }

        if (c == ',') {
            out.push_back(',');
            out.push_back(' ');
            continue;
        }
        if (c == ':') {
            out.push_back(':');
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

// ---- 取值 ----

int GetInt(const Json& obj, std::string_view key, int default_value) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_number()) return default_value;
    try {
        return v->get<int>();
    } catch (...) {
        return default_value;
    }
}

int64_t GetInt64(const Json& obj, std::string_view key, int64_t default_value) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_number()) return default_value;
    try {
        return v->get<int64_t>();
    } catch (...) {
        return default_value;
    }
}

double GetDouble(const Json& obj, std::string_view key, double default_value) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_number()) return default_value;
    try {
        return v->get<double>();
    } catch (...) {
        return default_value;
    }
}

bool GetBool(const Json& obj, std::string_view key, bool default_value) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_boolean()) return default_value;
    return v->get<bool>();
}

std::string GetString(const Json& obj, std::string_view key, std::string default_value) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_string()) return default_value;
    try {
        return v->get<std::string>();
    } catch (...) {
        return default_value;
    }
}

Json GetObject(const Json& obj, std::string_view key) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_object()) return Json::object();
    return *v;
}

Json GetArray(const Json& obj, std::string_view key) {
    const Json* v = FindMember(obj, key);
    if (v == nullptr || !v->is_array()) return Json::array();
    return *v;
}

std::string GetStringPath(const Json& root, std::initializer_list<std::string_view> path,
                          std::string default_value) {
    const Json* cur = &root;
    for (auto key : path) {
        cur = FindMember(*cur, key);
        if (cur == nullptr) return default_value;
    }
    if (!cur->is_string()) return default_value;
    try {
        return cur->get<std::string>();
    } catch (...) {
        return default_value;
    }
}

// ---- 判定 ----

bool HasKey(const Json& obj, std::string_view key) { return FindMember(obj, key) != nullptr; }
bool IsObject(const Json& value) { return value.is_object(); }
bool IsArray(const Json& value) { return value.is_array(); }
bool IsString(const Json& value) { return value.is_string(); }
bool IsNumber(const Json& value) { return value.is_number(); }

bool HasNonNull(const Json& obj, std::string_view key) {
    const Json* v = FindMember(obj, key);
    return v != nullptr && !v->is_null();
}

// ---- id ----

Json MessageId::ToJson() const {
    if (is_string) return Json(string_value);
    return Json(int_value);
}

std::optional<MessageId> GetMessageId(const Json& message) {
    const Json* v = FindMember(message, "id");
    if (v == nullptr || v->is_null()) return std::nullopt;

    MessageId id;
    if (v->is_string()) {
        id.is_string = true;
        id.string_value = v->get<std::string>();
        return id;
    }
    if (v->is_number_integer()) {
        id.int_value = v->get<int64_t>();
        return id;
    }
    if (v->is_number_unsigned()) {
        id.int_value = static_cast<int64_t>(v->get<uint64_t>());
        return id;
    }
    if (v->is_number_float()) {
        id.int_value = static_cast<int64_t>(v->get<double>());
        return id;
    }
    return std::nullopt;
}

// ---- 构造 ----

Json MakeResult(const MessageId& id, Json result) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id.ToJson();
    out["result"] = std::move(result);
    return out;
}

Json MakeError(std::string_view id_json, int code, std::string_view message, Json data) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    // id 为 null 时仍要显式出现（JSON-RPC 2.0 要求错误响应带 id:null）
    out["id"] = id_json.empty() ? Json(nullptr) : Json(id_json);
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    if (!data.is_null()) err["data"] = std::move(data);
    out["error"] = std::move(err);
    return out;
}

Json MakeError(const MessageId& id, int code, std::string_view message, Json data) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id.ToJson();
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    if (!data.is_null()) err["data"] = std::move(data);
    out["error"] = std::move(err);
    return out;
}

Json MakeNotification(std::string_view method, Json params) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["method"] = method;
    out["params"] = std::move(params);
    return out;
}

std::string Serialize(const Json& value, bool compact) {
    try {
        // nlohmann::json::dump(indent) 的行为：
        //   indent = -1 -> 完全紧凑：{"a":1,"b":2}       （无换行、无空格）
        //   indent =  0 -> **仍然换行**，只是不缩进：     "{\n\"a\":1,\n\"b\":2\n}"
        //   indent =  N -> 缩进 N 个空格的 pretty print
        //
        // 注意 indent=0 不是「紧凑」——它照样往 body 里塞换行。LSP 有
        // Content-Length 所以不怕换行，但 MCP 用 newline framing，body 里出现
        // 换行会把一条消息拆成两条。因此紧凑模式必须用 -1。
        //
        // 为了与 Python 参考实现对应：
        //   compact=true  <-> json.dumps(separators=(",", ":"))   -> dump(-1)
        //   compact=false <-> json.dumps() 默认（", " 与 ": "）    -> dump(-1) + 手工插入空格
        //
        // 非紧凑模式同样必须保证「无裸换行」，否则 LSP 客户端虽然能靠
        // Content-Length 解析，但多行 body 与 Python 版输出不一致。这里选择：
        // 两种模式都用 dump(-1)（绝无换行），非紧凑模式再补上 ", " 与 ": " 的空格，
        // 使其与 Python json.dumps 的默认单行输出逐字一致。
        std::string out = value.dump(-1, ' ', /*ensure_ascii=*/false,
                                     nlohmann::json::error_handler_t::replace);
        if (!compact) {
            out = AddPythonStyleSpaces(out);
        }
        return out;
    } catch (...) {
        return std::string("{}");
    }
}

}  // namespace fpgalsp::jsonrpc