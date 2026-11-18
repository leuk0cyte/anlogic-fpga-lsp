// =============================================================================
//  tests/unit/transport_test.cpp
// =============================================================================
//  重点：LSP 与 MCP 的 framing 差异。尤其 MCP —— body 里绝不能有裸换行，
//  否则一条消息会被拆成两条。
// =============================================================================

#include <gtest/gtest.h>

#include <sstream>
#include <thread>
#include <vector>

#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"

namespace {

using namespace fpgalsp::jsonrpc;

// 把一条 JSON 打成 LSP 帧
std::string LspFrame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// -----------------------------------------------------------------------------
// LSP 分帧
// -----------------------------------------------------------------------------

TEST(LspTransport, ReadsSingleMessage) {
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"initialize"})";
    std::istringstream in(LspFrame(body));
    std::ostringstream out;

    LspTransport t(in, out);
    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 1);
    EXPECT_EQ(GetString(rr.message, "method"), "initialize");
}

TEST(LspTransport, ReadsMultipleMessagesInSequence) {
    std::string stream;
    stream += LspFrame(R"({"id":1})");
    stream += LspFrame(R"({"id":2})");
    stream += LspFrame(R"({"id":3})");

    std::istringstream in(stream);
    std::ostringstream out;
    LspTransport t(in, out);

    EXPECT_EQ(GetInt(t.ReadMessage().message, "id"), 1);
    EXPECT_EQ(GetInt(t.ReadMessage().message, "id"), 2);
    EXPECT_EQ(GetInt(t.ReadMessage().message, "id"), 3);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::EndOfStream);
}

TEST(LspTransport, HeaderCaseInsensitive) {
    const std::string body = R"({"id":7})";
    std::string frame = "content-length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::istringstream in(frame);
    std::ostringstream out;
    LspTransport t(in, out);

    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 7);
}

TEST(LspTransport, ExtraHeadersIgnored) {
    const std::string body = R"({"id":8})";
    std::string frame = "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body;
    std::istringstream in(frame);
    std::ostringstream out;
    LspTransport t(in, out);

    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 8);
}

TEST(LspTransport, BodyMayContainNewlines) {
    // LSP 用 Content-Length，body 里有换行是合法的
    const std::string body = "{\n  \"id\": 9,\n  \"method\": \"x\"\n}";
    std::istringstream in(LspFrame(body));
    std::ostringstream out;
    LspTransport t(in, out);

    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 9);
}

TEST(LspTransport, EofReturnsEndOfStream) {
    std::istringstream in("");
    std::ostringstream out;
    LspTransport t(in, out);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::EndOfStream);
}

TEST(LspTransport, MissingContentLengthIsMalformed) {
    std::istringstream in("X-Foo: bar\r\n\r\n");
    std::ostringstream out;
    LspTransport t(in, out);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::Malformed);
    EXPECT_EQ(t.parse_errors(), 1);
}

TEST(LspTransport, InvalidJsonIsMalformedButNotFatal) {
    // 帧完整但 JSON 坏 -> Malformed，且后续消息仍能读
    std::string stream = LspFrame("{not json");
    stream += LspFrame(R"({"id":42})");
    std::istringstream in(stream);
    std::ostringstream out;
    LspTransport t(in, out);

    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::Malformed);
    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 42);
}

TEST(LspTransport, HugeBodyRejectedWithoutAllocating) {
    // 声明一个超过上限的长度，不能真的去分配
    std::string frame = "Content-Length: " + std::to_string(kMaxBodyBytes + 1) + "\r\n\r\n";
    std::istringstream in(frame);
    std::ostringstream out;
    LspTransport t(in, out);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::TooLarge);
}

TEST(LspTransport, WritesContentLengthFrame) {
    std::istringstream in("");
    std::ostringstream out;
    LspTransport t(in, out);

    Json msg = Json::object();
    msg["jsonrpc"] = "2.0";
    msg["id"] = 1;
    ASSERT_TRUE(t.WriteMessage(msg));

    const std::string written = out.str();
    EXPECT_EQ(written.rfind("Content-Length: ", 0), 0u);
    EXPECT_NE(written.find("\r\n\r\n"), std::string::npos);

    // 校验声明的长度与实际 body 一致
    const std::size_t sep = written.find("\r\n\r\n");
    const std::string body = written.substr(sep + 4);
    const std::size_t declared =
        std::stoul(written.substr(std::string("Content-Length: ").size(),
                                  sep - std::string("Content-Length: ").size()));
    EXPECT_EQ(declared, body.size());
}

// -----------------------------------------------------------------------------
// MCP 分帧
// -----------------------------------------------------------------------------

TEST(McpTransport, ReadsNewlineDelimited) {
    std::istringstream in("{\"id\":1}\n{\"id\":2}\n");
    std::ostringstream out;
    McpTransport t(in, out);

    EXPECT_EQ(GetInt(t.ReadMessage().message, "id"), 1);
    EXPECT_EQ(GetInt(t.ReadMessage().message, "id"), 2);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::EndOfStream);
}

TEST(McpTransport, SkipsBlankLines) {
    std::istringstream in("\n\n{\"id\":5}\n\n");
    std::ostringstream out;
    McpTransport t(in, out);
    auto rr = t.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetInt(rr.message, "id"), 5);
}

TEST(McpTransport, WriteIsCompactSingleLine) {
    // ⚠ MCP 核心契约：写出的 body 必须是单行（紧凑），否则分帧会断
    std::istringstream in("");
    std::ostringstream out;
    McpTransport t(in, out);

    Json msg = Json::object();
    msg["jsonrpc"] = "2.0";
    msg["id"] = 1;
    Json result = Json::object();
    result["a"] = 1;
    result["b"] = 2;
    msg["result"] = result;

    ASSERT_TRUE(t.WriteMessage(msg));

    const std::string written = out.str();
    ASSERT_FALSE(written.empty());
    EXPECT_EQ(written.back(), '\n');
    // 去掉结尾换行后，body 内部不能再有换行
    const std::string body = written.substr(0, written.size() - 1);
    EXPECT_EQ(body.find('\n'), std::string::npos);
    // 紧凑形式：不含 ", " 分隔（对应 separators=(",", ":")）
    EXPECT_EQ(body.find(", "), std::string::npos);
}

TEST(McpTransport, BodyWithEmbeddedNewlineInStringIsEscaped) {
    // 字符串值里含换行，序列化时会变成 \n 转义，因此不会破坏分帧
    std::istringstream in("");
    std::ostringstream out;
    McpTransport t(in, out);

    Json msg = Json::object();
    msg["text"] = "line1\nline2";
    ASSERT_TRUE(t.WriteMessage(msg));

    const std::string written = out.str();
    const std::string body = written.substr(0, written.size() - 1);
    EXPECT_EQ(body.find('\n'), std::string::npos);  // 没有裸换行

    // 能再读回来
    std::istringstream in2(written);
    std::ostringstream out2;
    McpTransport t2(in2, out2);
    auto rr = t2.ReadMessage();
    ASSERT_EQ(rr.status, TransportBaseCommon::ReadStatus::Message);
    EXPECT_EQ(GetString(rr.message, "text"), "line1\nline2");
}

TEST(McpTransport, LspFrameIsNotValidMcp) {
    // 反向验证：LSP 的 \r\n\r\n 帧喂给 MCP 不应该被当成一条好消息
    std::istringstream in(LspFrame(R"({"id":1})"));
    std::ostringstream out;
    McpTransport t(in, out);
    auto rr = t.ReadMessage();
    // 第一行 "Content-Length: 9" 不是 JSON -> Malformed
    EXPECT_EQ(rr.status, TransportBaseCommon::ReadStatus::Malformed);
}

TEST(McpTransport, EofReturnsEndOfStream) {
    std::istringstream in("");
    std::ostringstream out;
    McpTransport t(in, out);
    EXPECT_EQ(t.ReadMessage().status, TransportBaseCommon::ReadStatus::EndOfStream);
}

// -----------------------------------------------------------------------------
// 写失败 / 关闭语义
// -----------------------------------------------------------------------------

class FailingStream : public std::ostream {
public:
    FailingStream() : std::ostream(nullptr) { setstate(std::ios::badbit); }
};

TEST(TransportWrite, FailureMarksClosedWithoutThrowing) {
    std::istringstream in("");
    FailingStream out;
    LspTransport t(in, out);

    Json msg = Json::object();
    msg["id"] = 1;

    EXPECT_NO_THROW({
        const bool ok = t.WriteMessage(msg);
        EXPECT_FALSE(ok);
        EXPECT_TRUE(t.closed());
    });
}

TEST(TransportWrite, ClosedTransportDropsWrites) {
    std::istringstream in("");
    std::ostringstream out;
    LspTransport t(in, out);
    t.Close();
    Json msg = Json::object();
    msg["id"] = 1;
    EXPECT_FALSE(t.WriteMessage(msg));
    EXPECT_TRUE(out.str().empty());
}

// -----------------------------------------------------------------------------
// 并发写
// -----------------------------------------------------------------------------

TEST(TransportWrite, ConcurrentWritesDoNotInterleave) {
    std::istringstream in("");
    std::ostringstream out;
    LspTransport t(in, out);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int ti = 0; ti < kThreads; ++ti) {
        threads.emplace_back([&t, ti]() {
            for (int i = 0; i < kPerThread; ++i) {
                Json msg = Json::object();
                msg["t"] = ti;
                msg["i"] = i;
                t.WriteMessage(msg);
            }
        });
    }
    for (auto& th : threads) th.join();

    // 逐帧解回来，确认没有交错损坏
    std::istringstream in2(out.str());
    std::ostringstream out2;
    LspTransport reader(in2, out2);
    int count = 0;
    while (true) {
        auto rr = reader.ReadMessage();
        if (rr.status != TransportBaseCommon::ReadStatus::Message) break;
        EXPECT_TRUE(rr.message.contains("t"));
        EXPECT_TRUE(rr.message.contains("i"));
        ++count;
    }
    EXPECT_EQ(count, kThreads * kPerThread);
}

// -----------------------------------------------------------------------------
// MakeTransport 工厂
// -----------------------------------------------------------------------------

TEST(TransportFactory, CreatesCorrectMode) {
    std::istringstream in("");
    std::ostringstream out;

    auto lsp = MakeTransport(TransportMode::Lsp, in, out);
    EXPECT_FALSE(lsp->compact_json());

    auto mcp = MakeTransport(TransportMode::Mcp, in, out);
    EXPECT_TRUE(mcp->compact_json());
}

}  // namespace