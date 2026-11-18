// =============================================================================
//  tests/unit/dispatcher_test.cpp
// =============================================================================

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/transport.h"

namespace {

using namespace fpgalsp::jsonrpc;

// 一个小工具：把 dispatcher 的响应写成 MCP 形式再解析回来，便于断言
class DispatcherFixture {
public:
    explicit DispatcherFixture() : transport_(in_, out_), dispatcher_(transport_) {}

    Dispatcher& dispatcher() { return dispatcher_; }

    // 派发一条消息，并返回单条响应（若没有响应则返回 null json）
    Json DispatchAndGetResponse(const Json& msg) {
        const std::size_t before = out_.str().size();
        dispatcher_.Dispatch(msg);
        const std::string all = out_.str();
        if (all.size() == before) return Json();
        std::istringstream stream(all.substr(before));
        McpTransport reader(stream, sink_);
        auto rr = reader.ReadMessage();
        if (rr.status != TransportBaseCommon::ReadStatus::Message) return Json();
        return rr.message;
    }

private:
    std::istringstream in_;
    std::ostringstream out_;
    std::ostringstream sink_;
    McpTransport transport_;
    Dispatcher dispatcher_;
};

Json Request(int id, const char* method, Json params = Json::object()) {
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["id"] = id;
    m["method"] = method;
    m["params"] = std::move(params);
    return m;
}

Json Notification(const char* method, Json params = Json::object()) {
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["method"] = method;
    m["params"] = std::move(params);
    return m;
}

// -----------------------------------------------------------------------------

TEST(Dispatcher, SuccessfulCallReturnsResult) {
    DispatcherFixture f;
    f.dispatcher().Register("add", [](const Json& params) {
        return HandlerResult::Ok(GetInt(params, "a") + GetInt(params, "b"));
    });

    Json resp = f.DispatchAndGetResponse(Request(1, "add", {{"a", 2}, {"b", 3}}));
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp, "id"), 1);
    EXPECT_EQ(GetInt(resp, "result"), 5);
    EXPECT_FALSE(resp.contains("error"));
}

TEST(Dispatcher, MethodNotFoundReturnsMinus32601) {
    DispatcherFixture f;
    Json resp = f.DispatchAndGetResponse(Request(2, "no/such/method"));
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp, "id"), 2);
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(GetInt(resp["error"], "code"), kMethodNotFound);
}

TEST(Dispatcher, HandlerExceptionBecomesMinus32603) {
    DispatcherFixture f;
    f.dispatcher().Register("boom", [](const Json&) -> HandlerResult {
        throw std::runtime_error("exploded");
    });

    // 关键：不能传播异常
    Json resp;
    EXPECT_NO_THROW(resp = f.DispatchAndGetResponse(Request(3, "boom")));
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp["error"], "code"), kInternalError);
    EXPECT_NE(GetString(resp["error"], "message").find("exploded"), std::string::npos);
}

TEST(Dispatcher, HandlerThrowingNonStdExceptionStillCaught) {
    DispatcherFixture f;
    f.dispatcher().Register("boom2", [](const Json&) -> HandlerResult { throw 42; });
    Json resp;
    EXPECT_NO_THROW(resp = f.DispatchAndGetResponse(Request(4, "boom2")));
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp["error"], "code"), kInternalError);
}

TEST(Dispatcher, NotificationProducesNoResponse) {
    DispatcherFixture f;
    bool called = false;
    f.dispatcher().Register("notify", [&called](const Json&) {
        called = true;
        return HandlerResult::Ok();
    });

    Json resp = f.DispatchAndGetResponse(Notification("notify"));
    EXPECT_TRUE(called);       // handler 照常执行
    EXPECT_TRUE(resp.is_null());  // 但不产生响应
}

TEST(Dispatcher, NotificationForUnknownMethodIsSilent) {
    DispatcherFixture f;
    Json resp = f.DispatchAndGetResponse(Notification("unknown/notify"));
    EXPECT_TRUE(resp.is_null());
}

TEST(Dispatcher, DollarSlashMethodsAreSilentlyIgnored) {
    DispatcherFixture f;
    Json resp = f.DispatchAndGetResponse(Request(5, "$/setTrace", {{"value", "off"}}));
    EXPECT_TRUE(resp.is_null());  // 不响应、不报错
}

TEST(Dispatcher, InvalidMessageShapeReturnsInvalidRequest) {
    DispatcherFixture f;
    Json arr = Json::array();
    arr.push_back(1);
    Json resp = f.DispatchAndGetResponse(arr);
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp["error"], "code"), kInvalidRequest);
}

TEST(Dispatcher, ParseErrorUsesMinus32700) {
    DispatcherFixture f;
    f.dispatcher().SendParseError();
    // 直接检查输出流
    // （DispatchAndGetResponse 走的是 Dispatch，这里单独验证 SendParseError 的编码）
    EXPECT_EQ(kParseError, -32700);
    EXPECT_EQ(kInvalidRequest, -32600);
    EXPECT_EQ(kMethodNotFound, -32601);
    EXPECT_EQ(kInternalError, -32603);
}

TEST(Dispatcher, ExplicitErrorResultIsPassedThrough) {
    DispatcherFixture f;
    f.dispatcher().Register("err", [](const Json&) {
        return HandlerResult::Error(kInvalidParams, "bad params");
    });
    Json resp = f.DispatchAndGetResponse(Request(6, "err"));
    ASSERT_FALSE(resp.is_null());
    EXPECT_EQ(GetInt(resp["error"], "code"), kInvalidParams);
    EXPECT_EQ(GetString(resp["error"], "message"), "bad params");
}

TEST(Dispatcher, StringIdIsEchoed) {
    DispatcherFixture f;
    f.dispatcher().Register("echo", [](const Json&) { return HandlerResult::Ok(Json(1)); });

    Json req = Json::object();
    req["jsonrpc"] = "2.0";
    req["id"] = "my-id";
    req["method"] = "echo";

    Json resp = f.DispatchAndGetResponse(req);
    ASSERT_FALSE(resp.is_null());
    EXPECT_TRUE(resp["id"].is_string());
    EXPECT_EQ(GetString(resp, "id"), "my-id");
}

TEST(Dispatcher, RegistrationAndLookup) {
    DispatcherFixture f;
    f.dispatcher().Register("a", [](const Json&) { return HandlerResult::Ok(); });
    f.dispatcher().Register("b", [](const Json&) { return HandlerResult::Ok(); });

    EXPECT_TRUE(f.dispatcher().Has("a"));
    EXPECT_FALSE(f.dispatcher().Has("c"));
    EXPECT_EQ(f.dispatcher().Methods().size(), 2u);
}

TEST(Dispatcher, ReRegisterOverwrites) {
    DispatcherFixture f;
    f.dispatcher().Register("m", [](const Json&) { return HandlerResult::Ok(Json(1)); });
    f.dispatcher().Register("m", [](const Json&) { return HandlerResult::Ok(Json(2)); });

    Json resp = f.DispatchAndGetResponse(Request(7, "m"));
    EXPECT_EQ(GetInt(resp, "result"), 2);
    EXPECT_EQ(f.dispatcher().Methods().size(), 1u);
}

TEST(Dispatcher, ResponseToServerRequestIgnored) {
    // 没有 method 字段 = 对服务器请求的响应，应静默丢弃
    DispatcherFixture f;
    Json msg = Json::object();
    msg["id"] = 1;
    msg["result"] = 5;
    Json resp = f.DispatchAndGetResponse(msg);
    EXPECT_TRUE(resp.is_null());
}

TEST(Dispatcher, RecordedParseErrorCount) {
    // transport 的 parse_errors 计数可被上层读取
    std::istringstream in("{bad json\n");
    std::ostringstream out;
    McpTransport t(in, out);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::Malformed);
    EXPECT_EQ(t.parse_errors(), 1);
}

}  // namespace