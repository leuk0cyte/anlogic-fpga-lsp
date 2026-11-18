// =============================================================================
//  tests/unit/json_util_test.cpp
// =============================================================================

#include <gtest/gtest.h>

#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"  // 错误码常量

namespace {

using namespace fpgalsp::jsonrpc;

// -----------------------------------------------------------------------------
// 正常取值
// -----------------------------------------------------------------------------

TEST(JsonUtil, GetsTypedValues) {
    Json obj = Json::object();
    obj["i"] = 42;
    obj["s"] = "hello";
    obj["b"] = true;
    obj["d"] = 1.5;

    EXPECT_EQ(GetInt(obj, "i"), 42);
    EXPECT_EQ(GetString(obj, "s"), "hello");
    EXPECT_TRUE(GetBool(obj, "b"));
    EXPECT_DOUBLE_EQ(GetDouble(obj, "d"), 1.5);
}

// -----------------------------------------------------------------------------
// 缺失
// -----------------------------------------------------------------------------

TEST(JsonUtil, MissingKeyReturnsDefault) {
    Json obj = Json::object();
    EXPECT_EQ(GetInt(obj, "nope", -7), -7);
    EXPECT_EQ(GetString(obj, "nope", "dflt"), "dflt");
    EXPECT_TRUE(GetBool(obj, "nope", true));
    EXPECT_FALSE(HasKey(obj, "nope"));
}

TEST(JsonUtil, MissingKeyDoesNotThrow) {
    Json obj = Json::object();
    EXPECT_NO_THROW({
        (void)GetInt(obj, "x");
        (void)GetObject(obj, "x");
        (void)GetArray(obj, "x");
    });
}

// -----------------------------------------------------------------------------
// 类型错误
// -----------------------------------------------------------------------------

TEST(JsonUtil, TypeMismatchReturnsDefault) {
    Json obj = Json::object();
    obj["s"] = "not-a-number";
    obj["i"] = 5;

    // 类型不符：返回默认值而不是抛 type_error
    EXPECT_EQ(GetInt(obj, "s", 99), 99);
    EXPECT_EQ(GetString(obj, "i", "dflt"), "dflt");
    EXPECT_FALSE(GetBool(obj, "s", false));
}

TEST(JsonUtil, TypeMismatchDoesNotThrow) {
    Json obj = Json::object();
    obj["arr"] = Json::array({1, 2, 3});
    obj["n"] = nullptr;

    EXPECT_NO_THROW({
        (void)GetInt(obj, "arr");
        (void)GetString(obj, "n");
        (void)GetObject(obj, "arr");  // 请求 object 但给的是 array
        (void)GetArray(obj, "n");     // 请求 array 但给的是 null
    });
}

// -----------------------------------------------------------------------------
// null
// -----------------------------------------------------------------------------

TEST(JsonUtil, NullValueReturnsDefault) {
    Json obj = Json::object();
    obj["v"] = nullptr;

    EXPECT_EQ(GetInt(obj, "v", 3), 3);
    EXPECT_EQ(GetString(obj, "v", "x"), "x");
    EXPECT_FALSE(HasNonNull(obj, "v"));
    EXPECT_TRUE(HasKey(obj, "v"));  // key 存在，只是值为 null
}

TEST(JsonUtil, NonObjectRootReturnsDefault) {
    Json arr = Json::array({1, 2});
    EXPECT_EQ(GetInt(arr, "k", 5), 5);
    EXPECT_TRUE(GetObject(arr, "k").is_object());
    EXPECT_TRUE(GetArray(arr, "k").is_array());
}

// -----------------------------------------------------------------------------
// 嵌套
// -----------------------------------------------------------------------------

TEST(JsonUtil, NestedObject) {
    Json root = Json::parse(R"({"a":{"b":{"c":7}}})");
    EXPECT_EQ(GetInt(GetObject(GetObject(root, "a"), "b"), "c"), 7);
}

TEST(JsonUtil, GetStringPath) {
    Json root = Json::parse(
        R"({"params":{"textDocument":{"uri":"file:///Y:/a.sv"}}})");
    EXPECT_EQ(GetStringPath(root, {"params", "textDocument", "uri"}),
              "file:///Y:/a.sv");
}

TEST(JsonUtil, GetStringPathMissingIntermediateReturnsDefault) {
    Json root = Json::parse(R"({"params":{}})");
    EXPECT_EQ(GetStringPath(root, {"params", "textDocument", "uri"}, "dflt"), "dflt");
}

// -----------------------------------------------------------------------------
// 数组
// -----------------------------------------------------------------------------

TEST(JsonUtil, ArrayAccess) {
    Json obj = Json::object();
    obj["items"] = Json::array({1, 2, 3});

    Json arr = GetArray(obj, "items");
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].get<int>(), 1);
}

TEST(JsonUtil, GetArrayOnNonArrayReturnsEmptyArray) {
    Json obj = Json::object();
    obj["x"] = "string";
    Json arr = GetArray(obj, "x");
    EXPECT_TRUE(arr.is_array());
    EXPECT_TRUE(arr.empty());
}

// -----------------------------------------------------------------------------
// MessageId
// -----------------------------------------------------------------------------

TEST(JsonUtil, IntegerId) {
    Json msg = Json::parse(R"({"id":42})");
    auto id = GetMessageId(msg);
    ASSERT_TRUE(id.has_value());
    EXPECT_FALSE(id->is_string);
    EXPECT_EQ(id->int_value, 42);
}

TEST(JsonUtil, StringId) {
    Json msg = Json::parse(R"({"id":"abc"})");
    auto id = GetMessageId(msg);
    ASSERT_TRUE(id.has_value());
    EXPECT_TRUE(id->is_string);
    EXPECT_EQ(id->string_value, "abc");
}

TEST(JsonUtil, MissingIdIsNotification) {
    Json msg = Json::parse(R"({"method":"x"})");
    EXPECT_FALSE(GetMessageId(msg).has_value());
}

TEST(JsonUtil, NullIdIsNotification) {
    Json msg = Json::parse(R"({"id":null,"method":"x"})");
    EXPECT_FALSE(GetMessageId(msg).has_value());
}

// -----------------------------------------------------------------------------
// 序列化
// -----------------------------------------------------------------------------

TEST(JsonUtil, CompactSerializationHasNoWhitespace) {
    Json obj = Json::object();
    obj["a"] = 1;
    obj["b"] = Json::array({1, 2});

    const std::string compact = Serialize(obj, true);
    EXPECT_EQ(compact.find(' '), std::string::npos);
    EXPECT_EQ(compact.find('\n'), std::string::npos);
    EXPECT_EQ(compact, R"({"a":1,"b":[1,2]})");
}

TEST(JsonUtil, NonCompactHasSpaces) {
    Json obj = Json::object();
    obj["a"] = 1;
    obj["b"] = 2;
    const std::string pretty = Serialize(obj, false);
    EXPECT_NE(pretty.find(' '), std::string::npos);
}

TEST(JsonUtil, NonCompactMatchesPythonDefaultSingleLine) {
    // Python: json.dumps({"a":1,"b":[1,2]}) == '{"a": 1, "b": [1, 2]}'
    Json obj = Json::object();
    obj["a"] = 1;
    obj["b"] = Json::array({1, 2});
    EXPECT_EQ(Serialize(obj, false), R"({"a": 1, "b": [1, 2]})");
}

TEST(JsonUtil, NeitherModeEmitsNewline) {
    // ⚠ 回归测试：曾用 dump(0) 实现非紧凑模式，结果 body 里出现换行。
    // LSP 有 Content-Length 不会解析错，但会与 Python 版输出不一致，
    // 且 MCP 模式下会直接破坏分帧。
    Json obj = Json::object();
    obj["a"] = Json::array({1, 2, 3});
    obj["b"] = Json::object({{"x", 1}});
    for (bool compact : {true, false}) {
        const std::string s = Serialize(obj, compact);
        EXPECT_EQ(s.find('\n'), std::string::npos) << "compact=" << compact << " s=" << s;
        EXPECT_EQ(s.find('\r'), std::string::npos) << "compact=" << compact << " s=" << s;
    }
}

TEST(JsonUtil, SpacesAreNotInsertedInsideStrings) {
    // 字符串内容里的 "," 与 ":" 不能被改写
    Json obj = Json::object();
    obj["s"] = "a,b:c";
    EXPECT_EQ(Serialize(obj, true), R"({"s":"a,b:c"})");
    EXPECT_EQ(Serialize(obj, false), R"({"s": "a,b:c"})");
}

TEST(JsonUtil, EscapedQuoteInStringHandled) {
    Json obj = Json::object();
    obj["s"] = "he said \"a,b\"";
    const std::string pretty = Serialize(obj, false);
    // 必须能重新解析回同一个值
    EXPECT_EQ(Json::parse(pretty)["s"].get<std::string>(), "he said \"a,b\"");
}

TEST(JsonUtil, MakeResultAndError) {
    MessageId id;
    id.int_value = 1;
    Json ok = MakeResult(id, Json(nullptr));
    EXPECT_EQ(ok["jsonrpc"], "2.0");
    ASSERT_TRUE(ok.contains("result"));
    EXPECT_TRUE(ok["result"].is_null());

    Json err = MakeError(id, -32601, "nope");
    EXPECT_EQ(err["error"]["code"], -32601);
    EXPECT_EQ(err["id"], 1);
}

TEST(JsonUtil, ParseErrorResponseHasNullId) {
    // JSON-RPC 2.0 要求解析错误响应带 id: null
    Json err = MakeError(std::string(), kParseError, "Parse error");
    ASSERT_TRUE(err.contains("id"));
    EXPECT_TRUE(err["id"].is_null());
    EXPECT_EQ(err["error"]["code"], -32700);
}

}  // namespace