// =============================================================================
//  src/jsonrpc/transport.cpp
// =============================================================================

#include "jsonrpc/transport.h"

#include "platform/text.h"
#include <cctype>
#include <charconv>
#include <memory>
#include <string>

namespace fpgalsp::jsonrpc {
namespace {

char ToLower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

using platform::Trim;

std::string ToLowerString(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ToLower(c);
    return out;
}

void StripTrailingCR(std::string& s) {
    while (!s.empty() && s.back() == '\r') s.pop_back();
}

}  // namespace

// -----------------------------------------------------------------------------
// LSP: Content-Length framing
// -----------------------------------------------------------------------------

LspFramer::Status LspFramer::ReadFrame(std::istream& in, std::string& out) {
    std::size_t content_length = 0;
    bool have_length = false;
    bool any_header = false;

    // 逐个读 header 行直到空行为止
    while (true) {
        std::string line;
        if (!std::getline(in, line)) {
            // EOF：一个 header 都没读到 => 干净 EOF；否则是不完整帧
            return any_header ? Status::Malformed : Status::EndOfStream;
        }
        StripTrailingCR(line);

        if (line.empty()) break;  // header 块结束

        any_header = true;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;  // 无冒号的行：忽略，容错
        }

        const std::string name = ToLowerString(Trim(std::string_view(line).substr(0, colon)));
        const std::string value = Trim(std::string_view(line).substr(colon + 1));

        if (name == "content-length") {
            std::size_t parsed = 0;
            const char* begin = value.data();
            const char* end = value.data() + value.size();
            const auto res = std::from_chars(begin, end, parsed);
            if (res.ec != std::errc{} || res.ptr != end) {
                return Status::Malformed;  // Content-Length 非法
            }
            content_length = parsed;
            have_length = true;
        }
        // 其它 header（Content-Type 等）一律忽略
    }

    // 对应 lsp.py：length <= 0 -> 视为坏帧
    if (!have_length || content_length == 0) {
        return Status::Malformed;
    }
    // 上限保护：不真的去分配超大 buffer
    if (content_length > kMaxBodyBytes) {
        return Status::TooLarge;
    }

    out.assign(content_length, '\0');
    in.read(out.data(), static_cast<std::streamsize>(content_length));
    if (static_cast<std::size_t>(in.gcount()) != content_length) {
        return Status::EndOfStream;  // body 不完整
    }
    return Status::Body;
}

bool LspFramer::WriteFrame(std::ostream& out, std::string_view body) {
    out << "Content-Length: " << body.size() << "\r\n\r\n";
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
}

// -----------------------------------------------------------------------------
// MCP: newline-delimited JSON
// -----------------------------------------------------------------------------

McpFramer::Status McpFramer::ReadFrame(std::istream& in, std::string& out) {
    // ⚠ B7 / S-08：**有界**读行。
    //
    //   旧实现是 `std::getline(in, line)`，先把整行读进内存、**之后**才比较
    //   kMaxBodyBytes —— 一个不含 '\n' 的 4 GiB 流会在检查之前就把进程 OOM 掉
    //   （LSP 路径相反：先判 Content-Length 再分配，transport.cpp:86-90）。
    //
    //   现在逐字符取、边取边比上限：峰值内存被钉在 kMaxBodyBytes 以内。
    //   代价是每字节一次 streambuf 调用（缓冲区在 streambuf 里，正常小消息可忽略）。
    //   超限时立即返回 TooLarge，不把剩余内容读进来；残留部分会在后续 ReadFrame
    //   里继续被判超限，直到遇到 '\n' 自然重新同步（与 Python mcp.py 的行语义一致）。
    std::string line;
    line.reserve(4096);
    for (;;) {
        const int ch = in.get();
        if (ch == std::char_traits<char>::eof()) {
            StripTrailingCR(line);
            if (line.empty()) return Status::EndOfStream;
            out = std::move(line);
            return Status::Body;
        }
        if (ch == '\n') {
            StripTrailingCR(line);
            if (line.empty()) continue;  // 空行：跳过（对应 mcp.py 的 continue）
            out = std::move(line);
            return Status::Body;
        }
        if (line.size() >= kMaxBodyBytes) return Status::TooLarge;
        line.push_back(static_cast<char>(ch));
    }
}

bool McpFramer::WriteFrame(std::ostream& out, std::string_view body) {
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.put('\n');
    return static_cast<bool>(out);
}

// -----------------------------------------------------------------------------

std::unique_ptr<TransportBaseCommon> MakeTransport(TransportMode mode, std::istream& in,
                                                   std::ostream& out) {
    if (mode == TransportMode::Mcp) {
        return std::make_unique<McpTransport>(in, out);
    }
    return std::make_unique<LspTransport>(in, out);
}

}  // namespace fpgalsp::jsonrpc