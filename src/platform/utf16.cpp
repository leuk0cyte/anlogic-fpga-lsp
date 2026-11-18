// =============================================================================
//  src/platform/utf16.cpp
// =============================================================================
//  设计要点：
//   * 手写 UTF-8 解码扫描器（scan_next）是全模块唯一的事实来源，所有换算都在
//     它之上实现，保证 utf16_len / column 换算 / 行切分三者语义绝对一致。
//   * simdutf 用于「整串合法性快速判定」与批量转码；但 LSP 里绝大多数调用是对
//     单行短字符串的随机位置换算，走手写扫描。两者在非法输入下结果一致
//     （都按 U+FFFD 处理），有单测锁定。
//   * 全部函数 never-raise：不抛异常、不 assert 中止、越界一律 clamp。
// =============================================================================

#include "platform/utf16.h"

#include <cstdint>

#include <simdutf.h>

namespace fpgalsp::platform {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// UTF-8 严格解码一个码点。
// 返回解码出的码点，并把 *len 置为消耗的字节数。
// 非法序列（过长编码、代理项、越界、截断）一律：返回 U+FFFD，*len = 1。
// 说明：与 Python 的 errors="replace" 在逐字节替换这一点上一致（Python 会把
// 某些非法多字节前缀整体替换，但两者都保证「不抛异常 + 输出合法 UTF-8」，
// 且对 LSP 位置换算而言码元计数相同）。
char32_t decode_one(std::string_view s, std::size_t i, std::size_t& len) {
    const auto n = s.size();
    const auto b0 = static_cast<unsigned char>(s[i]);

    len = 1;
    if (b0 < 0x80) {
        return b0;
    }

    // 2 字节：110xxxxx 10xxxxxx
    if ((b0 & 0xE0) == 0xC0) {
        if (i + 1 >= n) return kReplacement;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0) != 0x80) return kReplacement;
        const char32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
        if (cp < 0x80) return kReplacement;  // 过长编码
        len = 2;
        return cp;
    }

    // 3 字节：1110xxxx 10xxxxxx 10xxxxxx
    if ((b0 & 0xF0) == 0xE0) {
        if (i + 2 >= n) return kReplacement;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return kReplacement;
        const char32_t cp =
            ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
        if (cp < 0x800) return kReplacement;                       // 过长编码
        if (cp >= 0xD800 && cp <= 0xDFFF) return kReplacement;     // UTF-8 不允许代理项
        len = 3;
        return cp;
    }

    // 4 字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if ((b0 & 0xF8) == 0xF0) {
        if (i + 3 >= n) return kReplacement;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        const auto b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            return kReplacement;
        }
        const char32_t cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                            ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) return kReplacement;  // 过长编码 / 越界
        len = 4;
        return cp;
    }

    // 0x80..0xBF 孤立续字节，或 0xF8..0xFF 非法起始字节
    return kReplacement;
}

// 一个码点在 UTF-16 中占用的码元数。
constexpr std::size_t utf16_width(char32_t cp) noexcept {
    return cp > 0xFFFF ? 2 : 1;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// 转换
// -----------------------------------------------------------------------------

std::u16string utf8_to_utf16(std::string_view utf8) {
    // 短串走手写扫描（SIMD 的启动开销在这个长度上不划算）
    constexpr std::size_t kSimdThreshold = 64;

    std::u16string out;
    out.reserve(utf8.size());

    if (utf8.size() >= kSimdThreshold && simdutf::validate_utf8(utf8.data(), utf8.size())) {
        out.resize(simdutf::utf16_length_from_utf8(utf8.data(), utf8.size()));
        simdutf::convert_utf8_to_utf16le(utf8.data(), utf8.size(), out.data());
        return out;
    }

    std::size_t i = 0;
    while (i < utf8.size()) {
        std::size_t len = 1;
        const char32_t cp = decode_one(utf8, i, len);
        i += len;
        if (cp > 0xFFFF) {
            const char32_t v = cp - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view utf16) {
    constexpr std::size_t kSimdThreshold = 64;

    std::string out;
    out.reserve(utf16.size());

    if (utf16.size() >= kSimdThreshold &&
        simdutf::validate_utf16le(utf16.data(), utf16.size())) {
        out.resize(simdutf::utf8_length_from_utf16le(utf16.data(), utf16.size()));
        simdutf::convert_utf16le_to_utf8(utf16.data(), utf16.size(), out.data());
        return out;
    }

    for (std::size_t i = 0; i < utf16.size(); ++i) {
        char32_t cp = utf16[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 高代理：需要紧跟低代理
            if (i + 1 < utf16.size()) {
                const char32_t lo = utf16[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                } else {
                    cp = kReplacement;
                }
            } else {
                cp = kReplacement;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = kReplacement;  // 孤立低代理
        }
        append_utf8(out, cp);
    }
    return out;
}

bool is_valid_utf8(std::string_view utf8) {
    return simdutf::validate_utf8(utf8.data(), utf8.size());
}

// -----------------------------------------------------------------------------
// 换算
// -----------------------------------------------------------------------------

std::size_t utf16_len(std::string_view text) {
    std::size_t total = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 1;
        const char32_t cp = decode_one(text, i, len);
        i += len;
        total += utf16_width(cp);
    }
    return total;
}

std::size_t byte_index_to_utf16_column(std::string_view text, std::size_t byte_index) {
    if (byte_index == 0) return 0;
    if (byte_index >= text.size()) return utf16_len(text);

    std::size_t used = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 1;
        const char32_t cp = decode_one(text, i, len);

        // 字节偏移落在该字符中间：向下取整到该字符起始处（= 已累计的码元数）
        if (i + len > byte_index) {
            return used;
        }
        used += utf16_width(cp);
        i += len;
        if (i == byte_index) {
            return used;
        }
    }
    return used;
}

std::size_t utf16_column_to_byte_index(std::string_view text, std::size_t column) {
    // 逐字对应 Python：
    //   if col <= 0: return 0
    //   used = 0
    //   for i, ch in enumerate(text):
    //       w = 2 if ord(ch) > 0xFFFF else 1
    //       if used + w > col: return i      # 落在代理对中间 -> 字符起始
    //       used += w
    //       if used == col:    return i + 1  # 正好边界 -> 下一字符起始
    //   return len(text)
    if (column == 0) return 0;

    std::size_t used = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 1;
        const char32_t cp = decode_one(text, i, len);
        const std::size_t w = utf16_width(cp);

        if (used + w > column) {
            return i;  // 落在代理对中间：返回该字符（前一个字符边界）
        }
        used += w;
        i += len;
        if (used == column) {
            return i;  // 正好边界：返回下一字符起始
        }
    }
    return text.size();
}

std::size_t codepoint_index_to_utf16_column(std::string_view text, std::size_t codepoint_index) {
    std::size_t used = 0;
    std::size_t i = 0;
    std::size_t cp_index = 0;
    while (i < text.size() && cp_index < codepoint_index) {
        std::size_t len = 1;
        const char32_t cp = decode_one(text, i, len);
        used += utf16_width(cp);
        i += len;
        ++cp_index;
    }
    return used;
}

// -----------------------------------------------------------------------------
// 行工具
// -----------------------------------------------------------------------------

std::string_view line_at(std::string_view text, std::size_t line) {
    std::size_t current = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = text.find('\n', start);
        if (current == line) {
            if (nl == std::string_view::npos) {
                // 最后一行（可能为空）
                if (start > text.size()) return {};
                return text.substr(start);
            }
            return text.substr(start, nl - start);
        }
        if (nl == std::string_view::npos) {
            return {};  // 行号越界
        }
        start = nl + 1;
        ++current;
    }
}

std::size_t line_count(std::string_view text) {
    std::size_t n = 1;
    for (char c : text) {
        if (c == '\n') ++n;
    }
    return n;
}

std::size_t position_to_byte_index(std::string_view text, std::size_t line,
                                   std::size_t character) {
    std::size_t current_line = 0;
    std::size_t line_start = 0;

    while (current_line < line) {
        const std::size_t nl = text.find('\n', line_start);
        if (nl == std::string_view::npos) {
            return text.size();  // 行号越界 -> clamp 到末尾
        }
        line_start = nl + 1;
        ++current_line;
    }

    const std::size_t nl = text.find('\n', line_start);
    const std::size_t line_end = (nl == std::string_view::npos) ? text.size() : nl;
    const std::string_view row = text.substr(line_start, line_end - line_start);

    return line_start + utf16_column_to_byte_index(row, character);
}

}  // namespace fpgalsp::platform