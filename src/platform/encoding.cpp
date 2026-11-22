// =============================================================================
//  src/platform/encoding.cpp
// =============================================================================

#include "platform/encoding.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "platform/utf16.h"

namespace fpgalsp::platform {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

}  // namespace

bool has_utf8_bom(std::string_view bytes) {
    return bytes.size() >= 3 && bytes.substr(0, 3) == kUtf8Bom;
}

std::string_view strip_utf8_bom(std::string_view bytes) {
    if (has_utf8_bom(bytes)) bytes.remove_prefix(3);
    return bytes;
}

std::string sanitize_utf8(std::string_view bytes) {
    if (is_valid_utf8(bytes)) {
        return std::string(bytes);
    }
    // 非法就整体走一次「解码 -> 编码」，非法位置自然落成 U+FFFD。
    // 用 utf16 往返，保证与位置换算使用完全相同的替换策略。
    return utf16_to_utf8(utf8_to_utf16(bytes));
}

ReadFileResult read_file_utf8(std::string_view path) {
    ReadFileResult result;

    if (path.empty()) return result;

    try {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) return result;

        std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (in.bad()) return result;

        result.ok = true;
        result.had_bom = has_utf8_bom(raw);

        const std::string_view body = strip_utf8_bom(raw);
        if (is_valid_utf8(body)) {
            result.content.assign(body);
        } else {
            result.had_invalid_bytes = true;
            result.content = sanitize_utf8(body);
        }
        return result;
    } catch (...) {
        return result;  // never-raise
    }
}

std::optional<std::string> try_read_file_utf8(std::string_view path) {
    ReadFileResult r = read_file_utf8(path);
    if (!r.ok) return std::nullopt;
    return std::move(r.content);
}

bool write_file_utf8(std::string_view path, std::string_view content) {
    if (path.empty()) return false;
    try {
        // 二进制模式：避免 Windows 下把 \n 自动改成 \r\n
        std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        // 不写 BOM
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}

bool ensure_directory(std::string_view path) {
    if (path.empty()) return false;
    try {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(std::string(path)), ec);
        // 已存在不算错误
        return !(ec && !std::filesystem::exists(std::filesystem::path(std::string(path)), ec));
    } catch (...) {
        return false;
    }
}

}  // namespace fpgalsp::platform