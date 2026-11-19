// =============================================================================
//  src/constraints/text_util.cpp
// =============================================================================
//  每一条都对应 Python 内置语义；改动前先看 text_util.h 的注释，
//  尤其是「刻意不做」的两处（多字节行分隔符、Unicode 数字）。
// =============================================================================

#include "constraints/text_util.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace fpgalsp::constraints::text {
namespace {

bool IsLineBreakByte(char c) {
    switch (c) {
        case '\n':
        case '\v':
        case '\f':
        case '\x1c':
        case '\x1d':
        case '\x1e':
            return true;
        default:
            return false;
    }
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

bool IsSpace(char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\v':
        case '\f':
            return true;
        default:
            return false;
    }
}

std::string_view LStrip(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    return s.substr(i);
}

std::string_view RStrip(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && IsSpace(s[n - 1])) --n;
    return s.substr(0, n);
}

std::string_view Strip(std::string_view s) { return LStrip(RStrip(s)); }

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> out;
    size_t start = 0;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back(text.substr(start, i - start));
            i += (i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
            start = i;
            continue;
        }
        if (IsLineBreakByte(c)) {
            out.push_back(text.substr(start, i - start));
            ++i;
            start = i;
            continue;
        }
        ++i;
    }
    // Python splitlines：末尾的分隔符不产生额外的空行
    if (start < text.size()) out.push_back(text.substr(start));
    return out;
}

std::string_view LineAt(std::string_view text, int line) {
    if (line < 0) return {};
    int current = 0;
    size_t start = 0;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        size_t next = i + 1;
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') next = i + 2;
        } else if (!IsLineBreakByte(c)) {
            ++i;
            continue;
        }
        if (current == line) return text.substr(start, i - start);
        ++current;
        start = next;
        i = next;
    }
    if (current == line) return text.substr(start);
    return {};
}

std::string UpperAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string LowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

int FindOffset(std::string_view haystack, std::string_view needle) {
    const size_t position = haystack.find(needle);
    if (position == std::string_view::npos) return -1;
    return static_cast<int>(position);
}

std::string_view LeadingIdentifier(std::string_view text, bool allow_dollar) {
    const auto ident_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const auto ident_char = [allow_dollar](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || (allow_dollar && c == '$');
    };
    if (text.empty() || !ident_start(text.front())) return {};
    size_t end = 1;
    while (end < text.size() && ident_char(text[end])) ++end;
    return text.substr(0, end);
}

std::string PyRepr(std::string_view s) {
    const bool has_single = s.find('\'') != std::string_view::npos;
    const bool has_double = s.find('"') != std::string_view::npos;
    // Python 的规则：能不用转义就不用；只有含单引号且不含双引号时才改用双引号
    const char quote = (has_single && !has_double) ? '"' : '\'';
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back(quote);
    char buffer[8];
    for (const char ch : s) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default: {
                const unsigned char byte = static_cast<unsigned char>(ch);
                if (ch == quote) {
                    out.push_back('\\');
                    out.push_back(ch);
                } else if (byte < 0x20 || byte == 0x7f) {
                    std::snprintf(buffer, sizeof(buffer), "\\x%02x", byte);
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
            }
        }
    }
    out.push_back(quote);
    return out;
}

std::optional<double> PyFloat(std::string_view s) {
    const std::string_view trimmed = Strip(s);
    if (trimmed.empty()) return std::nullopt;

    // 数字之间的下划线（Python 3.6+ 的 float() 接受 float("1_0") == 10.0）
    std::string cleaned;
    cleaned.reserve(trimmed.size());
    for (size_t i = 0; i < trimmed.size(); ++i) {
        const char c = trimmed[i];
        if (c == '_') {
            const bool left_ok = i > 0 && IsDigit(trimmed[i - 1]);
            const bool right_ok = i + 1 < trimmed.size() && IsDigit(trimmed[i + 1]);
            if (!left_ok || !right_ok) return std::nullopt;
            continue;
        }
        cleaned.push_back(c);
    }
    if (cleaned.empty()) return std::nullopt;

    // Python 只认十进制：strtod 会额外接受 "0x10"、"nan(...)"，这里提前挡掉
    const std::string lower = LowerAscii(cleaned);
    if (lower.find('x') != std::string::npos) return std::nullopt;
    if (lower.find('(') != std::string::npos) return std::nullopt;

    const char* const begin = cleaned.c_str();
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(begin, &end);
    if (end == begin) return std::nullopt;
    if (static_cast<size_t>(end - begin) != cleaned.size()) return std::nullopt;
    // 溢出按 Python 语义变成 inf（Python 不报错），所以不检查 errno
    return value;
}

std::string_view StripQuotes(std::string_view s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && s[start] == '"') ++start;
    while (end > start && s[end - 1] == '"') --end;
    return s.substr(start, end - start);
}

}  // namespace fpgalsp::constraints::text