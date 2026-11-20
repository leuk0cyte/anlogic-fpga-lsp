// =============================================================================
//  src/td/td_output.cpp
// =============================================================================

#include "td/td_output.h"

#include <memory>
#include <string>
#include <vector>

#include <re2/re2.h>

namespace fpgalsp::td {
namespace {

// re2 对象构造有开销，做成进程级单例（re2 线程安全，可并发使用）
const RE2& MsgRe() {
    static const RE2 re(
        R"(^([A-Z][A-Z0-9]{1,5}-[0-9]{3,5})\s*(?:(ERROR|CRITICAL-WARNING|WARNING|INFO)\s*:?\s*)?(.*)$)");
    return re;
}

const RE2& InFileRe() {
    static const RE2 re(R"(([A-Za-z]:[/\\][^()]+?)\(([0-9]+)\)\s*$)");
    return re;
}

const RE2& RelInFileRe() {
    static const RE2 re(
        R"(([A-Za-z_][A-Za-z0-9_./\\-]*\.(?:v|sv|vh|svh|vhd|vhdl|adc|sdc))\(([0-9]+)\)\s*$)");
    return re;
}

const RE2& IgnoredFileRe() {
    // 对应 Python 的 re.I（忽略大小写）——用内联标志 (?i) 表达
    static const RE2 re(
        R"((?i)file\s+['"]([^'"]+\.(?:v|sv|vh|svh|vhd|vhdl))['"]\s+ignored due to errors)");
    return re;
}

const RE2& ErrorRe() {
    static const RE2 re(R"(\b[A-Z][A-Z0-9]{1,5}-[0-9]{3,5}\s+(?:ERROR|CRITICAL-WARNING)\b)");
    return re;
}

const RE2& FatalRe() {
    static const RE2 re(R"(\b[A-Z][A-Z0-9]{1,5}-[0-9]{3,5}\s+ERROR\b)");
    return re;
}

// 只有代码 + 冒号、没有 severity 词：`KIT-2501 : No mark_debug exists in the netlist.`
// ⚠ 不能退化成 `(?:ERROR|CRITICAL-WARNING)?` 那种可选写法 —— 那会把错误行也吃进来，
//   于是同一条消息在 flow 里既算 error 又算 notice。
const RE2& NoticeRe() {
    static const RE2 re(R"(\b[A-Z][A-Z0-9]{1,5}-[0-9]{3,5}\s*:)");
    return re;
}

// 一行是否算 notice：含 `<CODE> :`，且**不含** ERROR / CRITICAL-WARNING。
// 排除是必须的：`RUN-1002 : start command "read_hdl ..."` 这类行里可能嵌着别的
// 错误码（`... PRJ-8001 ERROR: ...`），不排除就会重复计数。
bool IsNoticeLine(const std::string& line) {
    if (!RE2::PartialMatch(line, NoticeRe())) return false;
    return !RE2::PartialMatch(line, ErrorRe());
}

int SeverityOf(std::string_view name) {
    if (name == "ERROR") return kSeverityError;
    if (name == "CRITICAL-WARNING") return kSeverityWarning;
    if (name == "WARNING") return kSeverityWarning;
    if (name == "INFO") return kSeverityInfo;
    return kSeverityInfo;  // Python: SEVERITY.get(sev_name, 3)
}

std::string RStrip(std::string_view s) {
    std::size_t end = s.size();
    while (end > 0) {
        const char c = s[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            --end;
        } else {
            break;
        }
    }
    return std::string(s.substr(0, end));
}

bool IsBlank(std::string_view s) {
    for (char c : s) {
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')) {
            return false;
        }
    }
    return true;
}

// Python str.strip() 的近似（ASCII 空白 + 常见 Unicode 空白按 ASCII 处理）
std::string Strip(std::string_view s) {
    std::size_t b = 0, e = s.size();
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

// 对应 Python: message = re.sub(r"\s+(in|at|from|of)$", "", message)
// 语义：若 message 以「若干空白 + in/at/from/of」结尾，则把这段（连同前面的空白）删掉。
// 注意是 \s+（一个或多个空白），且单词必须完整落在结尾。
std::string StripTrailingPreposition(std::string message) {
    static const std::vector<std::string> kWords = {"in", "at", "from", "of"};

    for (const auto& word : kWords) {
        if (message.size() < word.size()) continue;
        if (message.compare(message.size() - word.size(), word.size(), word) != 0) continue;

        // 往前跳过一个或多个空白
        std::size_t j = message.size() - word.size();
        std::size_t k = j;
        while (k > 0) {
            const char c = message[k - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                --k;
            } else {
                break;
            }
        }
        // 必须真的吃掉了至少一个空白（否则 "within" 之类的尾部不算）
        if (k < j) {
            return message.substr(0, k);
        }
    }
    return message;
}

// 对应 Python: message.lstrip(": ").strip()
std::string LStripColonSpace(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ':' || s[i] == ' ')) ++i;
    return Strip(s.substr(i));
}

}  // namespace

std::vector<std::string> CleanLines(std::string_view text) {
    // 1) "\r\n" -> "\n"
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            normalized.push_back('\n');
            ++i;
        } else if (text[i] == '\r') {
            normalized.push_back('\n');  // 残余 \r 也当换行
        } else {
            normalized.push_back(text[i]);
        }
    }

    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = normalized.find('\n', start);
        const std::string_view line =
            (nl == std::string::npos)
                ? std::string_view(normalized).substr(start)
                : std::string_view(normalized).substr(start, nl - start);

        const std::string stripped = RStrip(line);
        if (!IsBlank(stripped)) {
            out.push_back(stripped);
        }

        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::vector<TdMessage> ParseMessageLines(std::string_view text) {
    std::vector<TdMessage> out;

    for (const auto& raw : CleanLines(text)) {
        const std::string line = Strip(raw);

        std::string code, sev_name, body;
        if (!RE2::FullMatch(line, MsgRe(), &code, &sev_name, &body)) {
            continue;
        }
        if (code.empty()) continue;

        // 大写化 severity（对应 Python 的 (m.group("sev") or "").upper()）
        for (char& c : sev_name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        std::optional<std::string> path;
        std::optional<int> line_no;
        std::string message = body;

        // 先试绝对路径，再试相对路径
        std::string matched_path;
        int matched_line = 0;
        bool found = RE2::PartialMatch(body, InFileRe(), &matched_path, &matched_line);
        if (!found) {
            found = RE2::PartialMatch(body, RelInFileRe(), &matched_path, &matched_line);
        }
        if (found) {
            path = matched_path;
            line_no = matched_line;
            // 找到 path(line) 的起始位置，截取其前的内容作为消息
            const std::string needle = "(" + std::to_string(matched_line) + ")";
            const std::size_t p = body.rfind(needle);
            std::string head = (p == std::string::npos) ? body : body.substr(0, p);
            // 去掉被匹配到的 path 部分（head 的尾部就是 path）
            if (!matched_path.empty() && head.size() >= matched_path.size() &&
                head.compare(head.size() - matched_path.size(), matched_path.size(),
                             matched_path) == 0) {
                head.resize(head.size() - matched_path.size());
            }
            message = LStripColonSpace(head);
            message = StripTrailingPreposition(message);
        }

        if (sev_name.empty()) {
            // TD 只有在明确写 WARNING / CRITICAL-WARNING 时才把消息升级为警告。
            // 其余（HDL-1007、elaborate module 等）一律 INFO。
            if (!path.has_value() || !line_no.has_value()) {
                // 无位置的闲聊：除非是 "file ... ignored due to errors"，否则丢弃
                std::string ignored_path;
                if (!RE2::PartialMatch(body, IgnoredFileRe(), &ignored_path)) {
                    continue;
                }
                path = ignored_path;
                line_no = 1;
                message = "TD ignored this file due to errors; the module is treated as a "
                          "black box, so TD lint is unavailable until the errors are fixed";
                sev_name = "WARNING";
            } else {
                sev_name = "INFO";
            }
        }

        TdMessage item;
        item.code = code;
        item.severity = SeverityOf(sev_name);
        item.message = Strip(message);
        if (path.has_value()) {
            std::string p = *path;
            for (char& c : p) {
                if (c == '\\') c = '/';
            }
            item.path = p;
        }
        item.line = line_no;
        out.push_back(std::move(item));
    }

    return out;
}

bool HasError(std::string_view text) {
    return RE2::PartialMatch(std::string(text), ErrorRe());
}

bool HasFatal(std::string_view text) {
    return RE2::PartialMatch(std::string(text), FatalRe());
}

bool HasNotice(std::string_view text) {
    for (const std::string& line : CleanLines(text)) {
        if (IsNoticeLine(line)) return true;
    }
    return false;
}

std::string NoticeLines(std::string_view text, int limit) {
    std::string out;
    if (limit <= 0) return out;
    int used = 0;
    for (const std::string& line : CleanLines(text)) {
        if (used >= limit) break;
        if (!IsNoticeLine(line)) continue;
        if (!out.empty()) out += " / ";
        out += Strip(line);  // CleanLines 只 rstrip，行首缩进在这里去掉
        ++used;
    }
    return out;
}

std::string StripSentinelsAndPrompt(std::string_view raw, std::string_view tag) {
    std::string text(raw);

    const std::string begin = "__B" + std::string(tag) + "__";
    const std::string end = "__E" + std::string(tag) + "__";

    const std::size_t b = text.find(begin);
    if (b != std::string::npos) {
        text = text.substr(b + begin.size());
    }
    const std::size_t e = text.rfind(end);
    if (e != std::string::npos) {
        text = text.substr(0, e);
    }

    // ⚠ 全局替换（与 Python 一致）：会吃掉输出里真实的 "td > " 字面
    constexpr std::string_view kPrompt = "td > ";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t hit = text.find(kPrompt, pos);
        if (hit == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, hit - pos);
        pos = hit + kPrompt.size();
    }
    return out;
}

}  // namespace fpgalsp::td