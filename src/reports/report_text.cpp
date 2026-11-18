// =============================================================================
//  src/reports/report_text.cpp —— 报告解析的公共文本工具
// =============================================================================

#include "reports/report_text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "platform/encoding.h"
#include "reports/report_patterns_internal.h"

namespace fpgalsp::reports {
namespace {

constexpr bool IsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// -----------------------------------------------------------------------------
// 行 / 空白
// -----------------------------------------------------------------------------

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    // Python 的 "".split("\n") == [""]，上面的循环同样给出 {""}，一致。
    return lines;
}

std::vector<std::string> SplitWhitespace(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        while (i < n && IsAsciiSpace(text[i])) ++i;
        if (i >= n) break;
        const std::size_t begin = i;
        while (i < n && !IsAsciiSpace(text[i])) ++i;
        parts.emplace_back(text.substr(begin, i - begin));
    }
    return parts;
}

std::string_view LStrip(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && IsAsciiSpace(text[i])) ++i;
    return text.substr(i);
}

std::string_view RStrip(std::string_view text) {
    std::size_t n = text.size();
    while (n > 0 && IsAsciiSpace(text[n - 1])) --n;
    return text.substr(0, n);
}

std::string_view Strip(std::string_view text) { return RStrip(LStrip(text)); }

std::string_view LStripSpaces(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && text[i] == ' ') ++i;
    return text.substr(i);
}

std::string_view RStripSpaces(std::string_view text) {
    std::size_t n = text.size();
    while (n > 0 && text[n - 1] == ' ') --n;
    return text.substr(0, n);
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWithAny(std::string_view text, std::initializer_list<std::string_view> prefixes) {
    for (const std::string_view prefix : prefixes) {
        if (StartsWith(text, prefix)) return true;
    }
    return false;
}

std::string ToLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 数字
// -----------------------------------------------------------------------------

bool ParseInt(std::string_view text, int64_t& out) {
    const std::string_view body = Strip(text);
    if (body.empty()) return false;

    std::size_t i = 0;
    bool negative = false;
    if (body[i] == '+' || body[i] == '-') {
        negative = body[i] == '-';
        ++i;
    }
    if (i >= body.size()) return false;

    int64_t value = 0;
    for (; i < body.size(); ++i) {
        if (!IsDigit(body[i])) return false;
        // 溢出的输入按「不是合法整数」处理，不抛、不 UB
        if (value > (std::numeric_limits<int64_t>::max() - (body[i] - '0')) / 10) return false;
        value = value * 10 + (body[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool ParseInt32(std::string_view text, int& out) {
    int64_t wide = 0;
    if (!ParseInt(text, wide)) return false;
    if (wide < std::numeric_limits<int>::min() || wide > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(wide);
    return true;
}

double ParseDouble(std::string_view text, bool& ok, double fallback) {
    ok = false;
    const std::string_view body = Strip(text);
    if (body.empty()) return fallback;

    std::size_t i = 0;
    if (body[i] == '+' || body[i] == '-') ++i;

    std::size_t digits = 0;
    while (i < body.size() && IsDigit(body[i])) {
        ++i;
        ++digits;
    }
    if (i < body.size() && body[i] == '.') {
        ++i;
        while (i < body.size() && IsDigit(body[i])) {
            ++i;
            ++digits;
        }
    }
    if (digits == 0) return fallback;
    if (i != body.size()) return fallback;  // 存在尾随垃圾（"1.5ns" 之类）

    // 到这一步 body 已是 std::string_view，而 strtod 需要 NUL 结尾
    const std::string buffer(body);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str()) return fallback;
    ok = true;
    return value;
}

double Round2(double value) {
    if (!std::isfinite(value)) return value;
    // 就近取偶（默认舍入模式），与 CPython round(x, 2) 一致，见头文件说明
    return std::nearbyint(value * 100.0) / 100.0;
}

// -----------------------------------------------------------------------------
// 资源名归一化
// -----------------------------------------------------------------------------

std::string NormResource(std::string_view name) {
    const std::string_view body = Strip(name);
    std::string out;
    out.reserve(body.size());
    bool pending_separator = false;
    for (const char raw : body) {
        char c = raw;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_separator && !out.empty()) out.push_back('_');
            pending_separator = false;
            out.push_back(c);
        } else {
            pending_separator = true;
        }
    }
    return out;  // 尾部 pending_separator 自然被丢弃 == Python 的 .strip("_")
}

// -----------------------------------------------------------------------------
// 文件
// -----------------------------------------------------------------------------

std::string ReadReportFile(std::string_view path) {
    if (path.empty()) return {};
    const platform::ReadFileResult result = platform::read_file_utf8(path);
    if (!result.ok) return {};

    // ⚠ 必须做换行归一化，否则与 Python 分叉：
    //   Python 用 open(path, "r", encoding="utf-8", errors="replace")，**文本模式**
    //   会把 \r\n 和孤立的 \r 都翻译成 \n（universal newlines）。TD 在 Windows 上
    //   写的是 CRLF，所以 reports.py 拿到的 raw 里没有 \r。
    //   不归一化的话：`raw` 字段整段对不上（实测 1104 处差异里绝大多数来自这里），
    //   而且 delay 表里 RStrip 掉 \r 会掩盖问题，更难发现。
    std::string text;
    text.reserve(result.content.size());
    for (std::size_t i = 0; i < result.content.size(); ++i) {
        const char c = result.content[i];
        if (c == '\r') {
            text.push_back('\n');
            if (i + 1 < result.content.size() && result.content[i + 1] == '\n') ++i;
            continue;
        }
        text.push_back(c);
    }
    return text;
}

// -----------------------------------------------------------------------------
// 正则自检
// -----------------------------------------------------------------------------

bool AllReportPatternsValid(std::string* error) {
    std::string local;
    const bool ok = detail::ValidateQorPatterns(&local) && detail::ValidateTimingPatterns(&local) &&
                    detail::ValidateAreaPatterns(&local) &&
                    detail::ValidateFlowStatusPatterns(&local);
    if (!ok && error != nullptr) *error = local;
    if (ok && error != nullptr) error->clear();
    return ok;
}

}  // namespace fpgalsp::reports