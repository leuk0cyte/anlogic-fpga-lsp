// =============================================================================
//  src/jsonrpc/transport.cpp
// =============================================================================

#include "jsonrpc/transport.h"

#include <cctype>
#include <charconv>
#include <memory>
#include <string>

namespace fpgalsp::jsonrpc {
namespace {

char ToLower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string Trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

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
    while (true) {
        std::string line;
        if (!std::getline(in, line)) {
            return Status::EndOfStream;
        }
        StripTrailingCR(line);
        if (line.empty()) continue;  // 空行：跳过（对应 mcp.py 的 continue）

        if (line.size() > kMaxBodyBytes) {
            return Status::TooLarge;
        }
        out = std::move(line);
        return Status::Body;
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