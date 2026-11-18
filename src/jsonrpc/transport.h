// =============================================================================
//  src/jsonrpc/transport.h —— 双模式 JSON-RPC 帧传输
// =============================================================================
//  两种 framing：
//
//  1) LSP 模式（--stdio）
//       Content-Length: 42\r\n
//       \r\n
//       {"jsonrpc":"2.0",...}
//     header 读取要容错：header 名大小写不敏感、容忍多余 header（Content-Type 等）、
//     容忍 "\n" 单独作行尾（宽松客户端）、body 内允许换行。
//
//  2) MCP 模式（--mcp）
//       一行一条 JSON，以 '\n' 分隔；body 内**不能**有裸换行。
//     因此写出去必须用紧凑序列化（Serialize(value, compact=true)）。
//
//  两种模式的差异集中在「怎么读 header / 怎么切 body」，因此用策略类
//  （LspFramer / McpFramer）在**类型层面**区分，而不是在读循环里塞 if 分支。
//  TransportBase<Framer> 是模板；对外暴露两个具体别名：
//       LspTransport / McpTransport
//
//  线程安全：WriteMessage 内部持锁（对应 lsp.py 的 _wlock）；
//            ReadMessage 假定单线程调用（LSP 的 stdin 只由一个读线程消费）。
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "jsonrpc/json_util.h"

namespace fpgalsp::jsonrpc {

// JSON-RPC 2.0 标准错误码（对应 lsp.py 的常量）
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;

enum class TransportMode {
    Lsp,  // Content-Length framing
    Mcp,  // newline-delimited JSON
};

// 单条消息 body 的上限，防止恶意/损坏的 Content-Length 让进程 OOM。
inline constexpr std::size_t kMaxBodyBytes = 64u * 1024u * 1024u;

class TransportBaseCommon {
public:
    virtual ~TransportBaseCommon() = default;

    // 读一帧的结果。
    // 之所以不用 optional<Json>：必须能把「EOF」「读到一条坏帧」「读到一个
    // 合法消息」三者区分开——坏了要回 -32700 并继续读，EOF 要退出循环。
    enum class ReadStatus {
        Message,     // 成功解出一条消息，见 message()
        EndOfStream, // 对端关闭
        Malformed,   // 帧结构 / JSON 解析失败（上层应回 ParseError 并继续读）
        TooLarge,    // body 超过 kMaxBodyBytes（上层应回 InvalidRequest 并继续读）
    };

    struct ReadResult {
        ReadStatus status = ReadStatus::EndOfStream;
        Json message;  // 仅当 status == Message 时有效
        std::string raw_body;  // 失败时的原始 body（便于打日志）
    };

    // 读下一条消息。EOF 时返回 EndOfStream。
    virtual ReadResult ReadMessage() = 0;

    // 写一条消息。写失败（BrokenPipe 等）时标记关闭并返回 false，**不抛异常**。
    virtual bool WriteMessage(const Json& message) = 0;

    // 传输是否已关闭（写失败或显式关闭后为 true）。
    virtual bool closed() const = 0;
    virtual void Close() = 0;

    // 该模式的序列化是否使用紧凑形式（MCP 必须为 true）。
    virtual bool compact_json() const = 0;

    // 协议层诊断计数
    virtual int parse_errors() const = 0;
    virtual int write_errors() const = 0;
};

// -----------------------------------------------------------------------------
// 帧策略
// -----------------------------------------------------------------------------

// LSP: Content-Length: N\r\n(\r\n)(HEADER\r\n)* \r\n BODY(N)
struct LspFramer {
    static constexpr TransportMode kMode = TransportMode::Lsp;
    static constexpr bool kCompactJson = false;

    enum class Status { Body, EndOfStream, Malformed, TooLarge };

    // 读 header 块，并按声明的长度把 body 读进 out。
    static Status ReadFrame(std::istream& in, std::string& out);
    // 写一帧
    static bool WriteFrame(std::ostream& out, std::string_view body);
};

// MCP: 一行一个 JSON（以 '\n' 结尾）
struct McpFramer {
    static constexpr TransportMode kMode = TransportMode::Mcp;
    static constexpr bool kCompactJson = true;

    enum class Status { Body, EndOfStream, Malformed, TooLarge };

    static Status ReadFrame(std::istream& in, std::string& out);
    static bool WriteFrame(std::ostream& out, std::string_view body);
};

// -----------------------------------------------------------------------------
// 模板实现
// -----------------------------------------------------------------------------

template <typename Framer>
class TransportBase final : public TransportBaseCommon {
public:
    TransportBase(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

    ReadResult ReadMessage() override {
        ReadResult result;

        if (closed_) {
            result.status = ReadStatus::EndOfStream;
            return result;
        }

        std::string body;
        typename Framer::Status fr;
        try {
            fr = Framer::ReadFrame(in_, body);
        } catch (...) {
            closed_ = true;
            result.status = ReadStatus::EndOfStream;
            return result;
        }

        switch (fr) {
            case Framer::Status::EndOfStream:
                closed_ = true;
                result.status = ReadStatus::EndOfStream;
                return result;
            case Framer::Status::Malformed:
                ++parse_errors_;
                result.status = ReadStatus::Malformed;
                return result;
            case Framer::Status::TooLarge:
                ++parse_errors_;
                result.status = ReadStatus::TooLarge;
                return result;
            case Framer::Status::Body:
                break;
        }

        try {
            result.message = Json::parse(body);
            result.status = ReadStatus::Message;
            return result;
        } catch (...) {
            // 非法 JSON：回 ParseError 并继续读（不终止服务）
            ++parse_errors_;
            result.status = ReadStatus::Malformed;
            result.raw_body = std::move(body);
            return result;
        }
    }

    bool WriteMessage(const Json& message) override {
        std::string body = Serialize(message, Framer::kCompactJson);
        // MCP 的 body 里绝对不能有换行（json.dump 的紧凑形式已保证，
        // 但字符串值里如果含 U+000A 会以 \n 转义形式出现，是安全的）。
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (closed_) return false;
        try {
            if (!Framer::WriteFrame(out_, body)) {
                closed_ = true;
                ++write_errors_;
                return false;
            }
            out_.flush();  // 写完必须 flush（对应 lsp.py 的 self._out.flush()）
            if (!out_) {
                closed_ = true;
                ++write_errors_;
                return false;
            }
            return true;
        } catch (...) {
            // BrokenPipe / 任何 IO 异常：标记关闭而不抛（对应 lsp.py 的行为）
            closed_ = true;
            ++write_errors_;
            return false;
        }
    }

    bool closed() const override { return closed_.load(); }
    void Close() override { closed_ = true; }
    bool compact_json() const override { return Framer::kCompactJson; }
    int parse_errors() const override { return parse_errors_.load(); }
    int write_errors() const override { return write_errors_.load(); }

private:
    std::istream& in_;
    std::ostream& out_;
    std::mutex write_mutex_;
    std::atomic<bool> closed_{false};
    std::atomic<int> parse_errors_{0};
    std::atomic<int> write_errors_{0};
};

using LspTransport = TransportBase<LspFramer>;
using McpTransport = TransportBase<McpFramer>;

// 按运行期模式构造（app 里 --stdio / --mcp 选择）。返回的对象已绑定到具体类型。
std::unique_ptr<TransportBaseCommon> MakeTransport(TransportMode mode, std::istream& in,
                                                   std::ostream& out);

}  // namespace fpgalsp::jsonrpc