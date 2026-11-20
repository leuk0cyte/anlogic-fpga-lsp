// =============================================================================
//  src/flow/probe.cpp —— ChipWatcher 探针后端实现（纯解析 + TD 会话编排）
// =============================================================================
//  约定与 flow 其它文件一致：**不抛异常**，失败都体现在返回的 JSON / error 字段里。
// =============================================================================

#include "flow/probe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "flow/hardware_lease.h"
#include "platform/encoding.h"
#include "td/tcl_quote.h"
#include "td/td_device.h"
#include "td/td_output.h"
#include "td/td_session.h"

namespace fpgalsp::probe {
namespace {

constexpr std::string_view kMarkDebug = "mark_debug";

// TD 从不通过退出码表达失败，只能扫文本。这里只挑出"有 severity 的错误行"，
// 不带 severity 的 INFO 行（如 HDL-1007）不要污染日志尾部。
std::vector<std::string> ErrorLines(std::string_view text, std::size_t limit) {
    static const RE2 kErrorRe(R"(\b[A-Z][A-Z0-9]{1,5}-\d{3,5}\s+(?:ERROR|CRITICAL-WARNING)\b)");
    std::vector<std::string> out;
    for (const std::string& line : fpgalsp::td::CleanLines(text)) {
        if (!RE2::PartialMatch(line, kErrorRe)) continue;
        out.push_back(line);
        if (out.size() >= limit) break;
    }
    return out;
}

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitWhitespace(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                   text[i] == '\n' || text[i] == '\v' || text[i] == '\f')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                    text[i] == '\n' || text[i] == '\v' || text[i] == '\f')) {
            ++i;
        }
        if (i > start) out.emplace_back(text.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> Lines(std::string_view text) {
    std::vector<std::string> out;
    std::string body(text);
    for (char& c : body) {
        if (c == '\r') c = '\n';
    }
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t pos = body.find('\n', start);
        if (pos == std::string::npos) {
            out.push_back(body.substr(start));
            break;
        }
        out.push_back(body.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool IsFile(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(std::string(path)), ec) && !ec;
}

std::string ReadText(const std::string& path) {
    const std::optional<std::string> text = fpgalsp::platform::try_read_file_utf8(path);
    return text.value_or(std::string());
}

// 从一段 XML 里取属性值：`name="..."` / `name='...'`
std::string Attr(std::string_view tag, std::string_view name) {
    const std::string pattern = "\\b" + std::string(name) + "\\s*=\\s*[\"']([^\"']*)[\"']";
    const RE2 re(pattern);
    std::string value;
    if (RE2::PartialMatch(tag, re, &value)) return value;
    return {};
}

int AttrInt(std::string_view tag, std::string_view name, int fallback = 0) {
    const std::string raw = Attr(tag, name);
    if (raw.empty()) return fallback;
    return std::atoi(raw.c_str());
}

// 取 `<tag ...>` 到下一个 '>' 之间的属性串（不含尖括号）
std::string_view TagBody(std::string_view text, std::size_t open) {
    const std::size_t close = text.find('>', open);
    if (close == std::string_view::npos) return text.substr(open);
    return text.substr(open, close - open);
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

// 二进制位串 -> 无符号整数（`0000000000000001` -> 1）。位串按「左高位」解释。
std::uint64_t BitsToValue(std::string_view bits) {
    std::uint64_t value = 0;
    for (const char c : bits) {
        if (c != '0' && c != '1') continue;
        if (value > (UINT64_MAX >> 1)) return value;  // 位宽不合法时不做回绕
        value = (value << 1) | static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

// 采样时钟/深度的"不可改 + 原因"说明。**英文原句**：这是给 agent 读的，
// 中文注释只解释为什么它必须这么说，字符串本身别翻译。
constexpr std::string_view kSamplingImmutableReason =
    "sample clock/depth are compile-time: TD writes them only from the GUI ChipWatcher "
    "instance before the Gate stage; the CLI has no setter and this backend never rewrites "
    "compiled.cwc (it only patches <trigger> into a temp copy for runtime trigger changes)";

jsonrpc::Json SamplingJson(const CwcSnapshot& cwc) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["clock"] = cwc.clock;
    out["depth"] = cwc.sample_depth;
    // editable 恒 false：不是"暂时没有实现"，而是 TD 的语义里根本没有这条运行期通道。
    out["editable"] = false;
    // ⚠ nlohmann::json 直接吃 std::string_view 在不同版本上行为不一致，这里显式转 string。
    out["reason"] = std::string(kSamplingImmutableReason);
    return out;
}

std::string JsonStringList(const std::vector<std::string>& items) {
    jsonrpc::Json array = jsonrpc::Json::array();
    for (const std::string& item : items) array.push_back(item);
    return array.dump();
}

}  // namespace

// -----------------------------------------------------------------------------
// user.mdn
// -----------------------------------------------------------------------------

std::vector<std::string> ParseMarkDebug(std::string_view text) {
    const std::size_t marker = text.find(kMarkDebug);
    if (marker == std::string_view::npos) return {};
    const std::size_t open = text.find('{', marker);
    if (open == std::string_view::npos) {
        // 允许 `mark_debug WHG/a[0]` 这种不带大括号的写法
        const std::size_t line_end = text.find('\n', marker);
        const std::string_view rest =
            text.substr(marker + kMarkDebug.size(),
                        (line_end == std::string_view::npos ? text.size() : line_end) -
                            (marker + kMarkDebug.size()));
        return SplitWhitespace(rest);
    }
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                return SplitWhitespace(text.substr(open + 1, i - open - 1));
            }
        }
    }
    // 大括号没闭合：把剩余部分当成清单，仍能给出可用结果
    return SplitWhitespace(text.substr(open + 1));
}

std::string NormaliseSignal(std::string_view signal) {
    const std::string trimmed = Trim(signal);
    // 把 `[n]` 拆出来重新拼，保证 `[10]` 与 `[2]` 的排序稳定
    const std::size_t open = trimmed.find_last_of('[');
    if (open == std::string::npos || trimmed.empty() || trimmed.back() != ']') return trimmed;
    const std::string base = trimmed.substr(0, open);
    const std::string index = trimmed.substr(open + 1, trimmed.size() - open - 2);
    if (index.empty()) return trimmed;
    for (const char c : index) {
        if (c < '0' || c > '9') return trimmed;
    }
    const int width = static_cast<int>(index.size());
    const int value = std::atoi(index.c_str());
    if (value < 0) return trimmed;
    // ⚠ 不要用固定大小的 char[]：`WHG/ANPC_init0/ANPC_SVPWM_init/cnt[0]` 这类
    //    层次名有 38 个字符，`char buf[32]` 会被 snprintf 截断，静默把探针名改坏
    //    （真机上踩过：16 个 counter 位被并成 1 个 `.../ANPC_SVPWM_init/`）。
    std::string padded = std::to_string(value);
    while (static_cast<int>(padded.size()) < width) padded.insert(padded.begin(), '0');
    return base + "[" + padded + "]";
}

std::vector<std::string> ExpandSignalRanges(const std::vector<std::string>& signals) {
    // `WHG/pwm_reg7_i[15:0]` / `WHG/bus[0:7]` 展开成逐位名字。
    // 用户从 RTL 里直接复制声明（`reg [15:0] pwm_reg7_i;`）时写的是范围，
    // 但 mark_debug 要的是逐位网名 —— 让工具替他们展开，别逼用户手写 16 条。
    static const RE2 kRange(R"(^(.+?)\[(\d+):(\d+)\])");
    std::vector<std::string> out;
    for (const std::string& raw : signals) {
        const std::string trimmed = Trim(raw);
        std::string base;
        std::string msb;
        std::string lsb;
        if (!RE2::FullMatch(trimmed, kRange, &base, &msb, &lsb)) {
            out.push_back(trimmed);
            continue;
        }
        const int a = std::atoi(msb.c_str());
        const int b = std::atoi(lsb.c_str());
        const int lo = std::min(a, b);
        const int hi = std::max(a, b);
        if (hi - lo > 1024) {  // 明显的笔误，别把内存撑爆
            out.push_back(trimmed);
            continue;
        }
        for (int bit = lo; bit <= hi; ++bit) {
            out.push_back(base + "[" + std::to_string(bit) + "]");
        }
    }
    return out;
}

std::optional<std::uint64_t> ParseBitString(std::string_view bits) {
    std::string trimmed = Trim(bits);
    if (trimmed.empty()) return std::nullopt;
    for (const char c : trimmed) {
        if (c != '0' && c != '1') return std::nullopt;
    }
    // 允许前导 0 超过 64 位（16 位总线一般是 16 个字符），超宽时直接判非法，
    // 而不是静默截断成另一个值 —— 触发值错一位的后果是"抓不到/抓错点"。
    std::size_t first_one = trimmed.find('1');
    if (first_one == std::string::npos) return 0ULL;
    if (trimmed.size() - first_one > 64) return std::nullopt;
    return BitsToValue(trimmed);
}

std::vector<std::string> NormaliseSignals(const std::vector<std::string>& signals) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& raw : ExpandSignalRanges(signals)) {
        const std::string signal = NormaliseSignal(raw);
        if (signal.empty()) continue;
        if (!seen.insert(signal).second) continue;
        out.push_back(signal);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::size_t open_a = a.find_last_of('[');
        const std::size_t open_b = b.find_last_of('[');
        const std::string base_a = open_a == std::string::npos ? a : a.substr(0, open_a);
        const std::string base_b = open_b == std::string::npos ? b : b.substr(0, open_b);
        if (base_a != base_b) return base_a < base_b;
        if (open_a == std::string::npos || open_b == std::string::npos) return a < b;
        return std::atoi(a.substr(open_a + 1).c_str()) < std::atoi(b.substr(open_b + 1).c_str());
    });
    return out;
}

MdnConfig ReadMdn(const std::string& path) {
    MdnConfig out;
    out.path = Slashes(path);
    if (!IsFile(path)) {
        out.error = "no chip_debugger/user.mdn at " + out.path;
        return out;
    }
    out.exists = true;
    const std::string text = ReadText(path);
    out.signals = NormaliseSignals(ParseMarkDebug(text));
    for (const std::string& line : Lines(text)) {
        if (line.find(kMarkDebug) != std::string::npos) continue;
        out.other_lines.push_back(line);
    }
    out.ok = true;
    return out;
}

std::string RenderMdn(std::string_view original, const std::vector<std::string>& signals) {
    const std::vector<std::string> normalised = NormaliseSignals(signals);
    std::string command;
    if (!normalised.empty()) {
        command = "mark_debug { ";
        for (const std::string& signal : normalised) {
            command += signal;
            command += ' ';
        }
        command += '}';
    }

    const std::size_t marker = original.find(kMarkDebug);
    if (marker == std::string_view::npos) {
        std::string out(original);
        if (!out.empty() && out.back() != '\n') out += '\n';
        if (!command.empty()) out += command + "\n";
        return out;
    }
    const std::size_t open = original.find('{', marker);
    if (open == std::string_view::npos) {
        // 没有大括号：只替换到行尾
        const std::size_t line_end = original.find('\n', marker);
        const std::size_t end = line_end == std::string_view::npos ? original.size() : line_end;
        std::string out(original.substr(0, marker));
        out += command;
        out += original.substr(end);
        return out;
    }
    int depth = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t i = open; i < original.size(); ++i) {
        if (original[i] == '{') {
            ++depth;
        } else if (original[i] == '}') {
            --depth;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }
    if (close == std::string_view::npos) close = original.size() - 1;
    // 连同前导空白一起吃掉，避免留下空行堆
    std::size_t begin = marker;
    while (begin > 0 && (original[begin - 1] == ' ' || original[begin - 1] == '\t')) --begin;
    std::string out(original.substr(0, begin));
    out += command;
    out += original.substr(close + 1);
    return out;
}

bool WriteMdn(const std::string& path, const std::vector<std::string>& signals,
              std::string* error) {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        (void)fpgalsp::platform::ensure_directory(path.substr(0, slash));
    }
    const std::string original = IsFile(path) ? ReadText(path) : std::string();
    if (!fpgalsp::platform::write_file_utf8(path, RenderMdn(original, signals))) {
        if (error) *error = "cannot write " + Slashes(path);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// compiled.cwc
// -----------------------------------------------------------------------------

CwcSnapshot ParseCwc(std::string_view text) {
    CwcSnapshot out;
    if (text.empty()) {
        out.error = "empty ChipWatcher configuration";
        return out;
    }
    const std::size_t root = text.find("<chipwatcher");
    if (root == std::string_view::npos) {
        out.error = "not a ChipWatcher file (no <chipwatcher> root)";
        return out;
    }
    const std::string_view root_body = TagBody(text, root);
    out.version = Attr(root_body, "version");
    out.project = Attr(root_body, "project_name");
    out.chip = Attr(root_body, "chip_name");

    // bus 列表（每个 bus 自带 clock / inst / type）
    // ⚠ compiled.cwc 里 `<bus>` 出现两次：顶层 `<watcher_nodes>`（探针清单）
    //   与 `<instance><signal_vec><watcher_nodes>`（触发/采样条件）。必须按名字去重，
    //   否则 4 条总线会被报成 8 条。
    std::map<std::string, std::size_t> bus_index;  // name -> out.buses 下标
    std::size_t pos = 0;
    while (true) {
        const std::size_t open = text.find("<bus ", pos);
        if (open == std::string_view::npos) break;
        const std::string_view body = TagBody(text, open);
        CwcBus bus;
        bus.name = Attr(body, "name");
        bus.clock = Attr(body, "clock");
        bus.instance = Attr(body, "inst");
        bus.type = Attr(body, "type");
        const std::string enabled = Attr(body, "is_enabled");
        bus.enabled = enabled.empty() || enabled == "yes" || enabled == "true";
        // 这条 bus 自己的 `<net>` 子元素（到下一个 <bus> / </bus> 为止）。
        // 有些 TD 版本的 <bus name> 不带 `[msb:lsb]`，位宽只能从这些 net 名还原。
        {
            const std::size_t next_bus = text.find("<bus ", open + 4);
            const std::size_t close = text.find("</bus>", open + 4);
            std::size_t end = text.size();
            if (next_bus != std::string_view::npos) end = std::min(end, next_bus);
            if (close != std::string_view::npos) end = std::min(end, close);
            for (std::size_t i = text.find("<net ", open + 4);
                 i != std::string_view::npos && i < end; i = text.find("<net ", i + 5)) {
                const std::string net_name = Attr(TagBody(text, i), "name");
                if (!net_name.empty()) bus.nets.push_back(net_name);
            }
        }
        if (!bus.name.empty()) {
            const auto found = bus_index.find(bus.name);
            if (found == bus_index.end()) {
                bus_index.emplace(bus.name, out.buses.size());
                out.buses.push_back(std::move(bus));
            } else {
                // 第二处定义（<instance> 里的 signal_vec）没有 inst 属性，
                // 但带触发条件；这里只补齐缺失的身份字段，不重复计数。
                CwcBus& existing = out.buses[found->second];
                if (existing.instance.empty() && !bus.instance.empty()) {
                    existing.instance = bus.instance;
                }
                if (existing.type.empty() && !bus.type.empty()) existing.type = bus.type;
                if (existing.nets.empty() && !bus.nets.empty()) {
                    existing.nets = std::move(bus.nets);
                }
            }
        }
        pos = open + 4;
    }

    pos = 0;
    if (const std::size_t clock = text.find("<clock ", pos); clock != std::string_view::npos) {
        out.clock = Attr(TagBody(text, clock), "clk_name");
    }
    if (const std::size_t config = text.find("<config ", pos); config != std::string_view::npos) {
        out.sample_depth = AttrInt(TagBody(text, config), "sample_depth");
    }
    if (const std::size_t trigger = text.find("<trigger ", pos);
        trigger != std::string_view::npos) {
        const std::string_view body = TagBody(text, trigger);
        out.trigger_position = AttrInt(body, "position");
        out.window_num = AttrInt(body, "window_num", 1);
        out.window_depth = AttrInt(body, "window_depth");
    }
    if (const std::size_t condition = text.find("<condition ", pos);
        condition != std::string_view::npos) {
        const std::string_view body = TagBody(text, condition);
        out.trigger_type = Attr(body, "type");
        const std::string enabled = Attr(body, "enabled");
        out.trigger_enabled = enabled == "yes" || enabled == "true";
    }
    if (const std::size_t instance = text.find("<instance ", pos);
        instance != std::string_view::npos) {
        out.instance = Attr(TagBody(text, instance), "name");
    }
    if (out.instance.empty() && !out.buses.empty()) out.instance = out.buses.front().instance;
    if (out.clock.empty() && !out.buses.empty()) out.clock = out.buses.front().clock;

    // net 数量：**去重后的 net 名**个数（同一份清单在文件里出现两次）
    std::set<std::string> nets;
    for (std::size_t i = text.find("<net "); i != std::string_view::npos;
         i = text.find("<net ", i + 5)) {
        const std::string name = Attr(TagBody(text, i), "name");
        if (!name.empty()) nets.insert(name);
    }
    out.net_count = static_cast<int>(nets.size());
    out.ok = !out.buses.empty();
    if (!out.ok) out.error = "no <bus> entries in ChipWatcher file";
    return out;
}

CwcSnapshot ReadCwc(const std::string& path) {
    CwcSnapshot out = ParseCwc(ReadText(path));
    out.path = Slashes(path);
    return out;
}

// -----------------------------------------------------------------------------
// 采集 CSV
// -----------------------------------------------------------------------------

CaptureData ParseCaptureCsv(std::string_view text) {
    CaptureData out;
    const std::vector<std::string> lines = Lines(text);
    std::size_t index = 0;
    while (index < lines.size() && Trim(lines[index]).empty()) ++index;
    if (index >= lines.size()) {
        out.error = "empty capture CSV";
        return out;
    }
    std::string header = lines[index];
    if (header.size() >= 3 && static_cast<unsigned char>(header[0]) == 0xEF) {
        header = header.substr(3);  // UTF-8 BOM
    }
    const std::vector<std::string> fields = [&header] {
        std::vector<std::string> parts;
        std::string current;
        for (const char c : header) {
            if (c == ',') {
                parts.push_back(Trim(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        parts.push_back(Trim(current));
        return parts;
    }();
    if (fields.size() < 2) {
        out.error = "capture CSV header has fewer than 2 columns";
        return out;
    }
    static const RE2 kRange(R"(^(.+?)\[(\d+):(\d+)\]\s*$)");
    for (std::size_t i = 2; i < fields.size(); ++i) {
        CaptureColumn column;
        column.name = fields[i];
        std::string msb;
        std::string lsb;
        std::string bus;
        if (RE2::FullMatch(column.name, kRange, &bus, &msb, &lsb)) {
            column.bus = bus;
            column.msb = std::atoi(msb.c_str());
            column.lsb = std::atoi(lsb.c_str());
            column.width = std::abs(column.msb - column.lsb) + 1;
        } else {
            column.bus = column.name;
            column.width = 1;
        }
        out.columns.push_back(std::move(column));
    }

    for (++index; index < lines.size(); ++index) {
        const std::string line = Trim(lines[index]);
        if (line.empty()) continue;
        std::vector<std::string> parts;
        std::string current;
        for (const char c : line) {
            if (c == ',') {
                parts.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        parts.push_back(current);
        if (parts.size() < 2) continue;
        CaptureRow row;
        row.index = std::atoi(Trim(parts[0]).c_str());
        row.trigger = Trim(parts[1]) == "1" || Trim(parts[1]) == "true";
        if (row.trigger && out.trigger_index < 0) out.trigger_index = row.index;
        for (std::size_t i = 2; i < parts.size() && (i - 2) < out.columns.size(); ++i) {
            row.values.push_back(BitsToValue(Trim(parts[i])));
        }
        while (row.values.size() < out.columns.size()) row.values.push_back(0);
        out.rows.push_back(std::move(row));
    }
    out.ok = !out.rows.empty();
    if (!out.ok) out.error = "capture CSV has no sample rows";
    return out;
}

// -----------------------------------------------------------------------------
// 路径
// -----------------------------------------------------------------------------

std::string RunsDir(const project::Project& project) {
    return Slashes(project.dir + "/" + project.name + "_Runs");
}

std::string ChipDebuggerDir(const project::Project& project) {
    return Slashes(project.dir + "/chip_debugger");
}

std::string MdnPath(const project::Project& project) {
    return ChipDebuggerDir(project) + "/user.mdn";
}

std::string CompiledBit(const project::Project& project) {
    return RunsDir(project) + "/phy_1/cw/compiled.bit";
}

std::string CompiledCwc(const project::Project& project) {
    return RunsDir(project) + "/phy_1/cw/compiled.cwc";
}

std::string CompiledData(const project::Project& project) {
    return RunsDir(project) + "/phy_1/cw/compiled.data";
}

// -----------------------------------------------------------------------------
// 状态 / 配置
// -----------------------------------------------------------------------------

namespace {

// 把"逐位清单"（`WHG/pwm_reg0_i[0]`）聚合成 base -> 位号集合，并去掉顶层前缀。
std::map<std::string, std::set<int>> GroupBitSignals(const std::vector<std::string>& signals,
                                                     std::string_view top) {
    std::map<std::string, std::set<int>> out;
    for (const std::string& raw : signals) {
        std::string name = raw;
        if (!top.empty()) {
            const std::string prefix = std::string(top) + "/";
            if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
        }
        const std::size_t open = name.find_last_of('[');
        if (open == std::string::npos || name.empty() || name.back() != ']') {
            out[name];  // 标量：空位集合
            continue;
        }
        const std::string base = name.substr(0, open);
        const std::string index = name.substr(open + 1, name.size() - open - 2);
        out[base].insert(std::atoi(index.c_str()));
    }
    return out;
}

// 把 ChipWatcher 的 bus（`pwm_reg0_i[15:0]`）展开成同样的 base -> 位号集合。
std::map<std::string, std::set<int>> GroupBusRanges(const std::vector<CwcBus>& buses) {
    std::map<std::string, std::set<int>> out;
    for (const CwcBus& bus : buses) {
        const std::size_t open = bus.name.find_last_of('[');
        const std::size_t colon =
            open == std::string::npos ? std::string::npos : bus.name.find(':', open);
        if (open == std::string::npos || colon == std::string::npos) {
            out[bus.name];
            continue;
        }
        const std::string base = bus.name.substr(0, open);
        const int msb = std::atoi(bus.name.substr(open + 1, colon - open - 1).c_str());
        const int lsb = std::atoi(bus.name.substr(colon + 1).c_str());
        const int lo = std::min(msb, lsb);
        const int hi = std::max(msb, lsb);
        for (int bit = lo; bit <= hi; ++bit) out[base].insert(bit);
    }
    return out;
}

}  // namespace

// =============================================================================
// 从 TD 自己的 watcher 产物反推 compiled.cwc
// =============================================================================
// 背景（2026-11-21 实测）：批量 `launch_runs` 跑完 phy 时，TD **不会**写
// `phy_1/cw/compiled.cwc`（只有 GUI 的 ChipWatcher 存盘会写），`setup_debugger`
// 在 CLI 下更是静默 no-op。但"探针确实进了位流"这件事是可证的：gate 之后 TD 会生成
//
//   syn_1/cw/auto_created_cwc1/auto_created_cwc1_watcherInst.sv  ← 总线数/位宽/DIN 位置/深度
//   phy_1/<prj>_inst.bid                                          ← 采集 RAM 的 ERAM 映射(rid/wid/offset)
//
// 这两样 + 当前 mdn 就足以重建 cwc（干净上下文 agent 手工做过一遍，实测抓取成功）。
// 这里把它固化成后端能力：cwc 缺失时自动生成，抓取不再依赖 GUI 存盘。
namespace {

struct WatcherInst {
    int bus_num = 0;
    int din_num = 0;
    int ram_len = 0;
    int depth = 0;
    int pipe_in = 0;
    int pipe_out = 0;
    std::vector<int> widths;
    std::vector<int> din_pos;
};

bool ParseIntAfter(const std::string& text, const char* key, int* out) {
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return false;
    const std::size_t eq = text.find('=', at);
    if (eq == std::string::npos) return false;
    std::size_t begin = eq + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    std::size_t end = begin;
    while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) != 0 ||
                                 text[end] == '-')) {
        ++end;
    }
    if (end == begin) return false;
    *out = std::atoi(text.substr(begin, end - begin).c_str());
    return true;
}

bool ParseIntListAfter(const std::string& text, const char* key, std::vector<int>* out) {
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return false;
    const std::size_t open = text.find('{', at);
    const std::size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return false;
    out->clear();
    std::string item;
    for (std::size_t i = open + 1; i < close; ++i) {
        const char c = text[i];
        if (c == ',' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!item.empty()) {
                out->push_back(std::atoi(item.c_str()));
                item.clear();
            }
            continue;
        }
        item.push_back(c);
    }
    if (!item.empty()) out->push_back(std::atoi(item.c_str()));
    return !out->empty();
}

std::string NewestWatcherDir(const project::Project& project) {
    const std::string runs = project.dir + "/" + project.name + "_Runs";
    std::string best;
    double best_time = 0.0;
    for (const char* run : {"syn_1", "phy_1"}) {
        const std::filesystem::path cw = std::filesystem::path(runs + "/" + run + "/cw");
        std::error_code ec;
        if (!std::filesystem::is_directory(cw, ec)) continue;
        for (std::filesystem::directory_iterator it(cw, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.find("cwc") == std::string::npos) continue;
            std::error_code tec;
            const auto stamp = std::filesystem::last_write_time(it->path(), tec);
            if (tec) continue;
            const double value = static_cast<double>(stamp.time_since_epoch().count());
            if (value > best_time) {
                best_time = value;
                best = it->path().string();
            }
        }
    }
    return best;
}

// 采样时钟：TD 不把它写成文本产物，只能从"历史 cwc"里取（当前 cwc / 抓取历史 / 工程 cwc 目录）。
std::string ResolveSampleClock(const project::Project& project) {
    std::vector<std::string> candidates;
    candidates.push_back(CompiledCwc(project));
    const std::filesystem::path history =
        std::filesystem::path(project.dir + "/.fpgalsp/probe");
    std::error_code ec;
    if (std::filesystem::is_directory(history, ec)) {
        for (std::filesystem::directory_iterator it(history, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > 4 && name.rfind(".cwc") == name.size() - 4) {
                candidates.push_back(it->path().string());
            }
        }
    }
    const std::filesystem::path project_cwc = std::filesystem::path(project.dir + "/cwc");
    if (std::filesystem::is_directory(project_cwc, ec)) {
        for (std::filesystem::directory_iterator it(project_cwc, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > 4 && name.rfind(".cwc") == name.size() - 4) {
                candidates.push_back(it->path().string());
            }
        }
    }
    for (const std::string& path : candidates) {
        if (!IsFile(path)) continue;
        const std::string text = ReadText(path);
        const std::size_t at = text.find("clock=");
        if (at == std::string::npos) continue;
        const std::size_t quote = text.find('"', at);
        if (quote == std::string::npos) continue;
        const std::size_t close = text.find('"', quote + 1);
        if (close == std::string::npos) continue;
        const std::string clock = text.substr(quote + 1, close - quote - 1);
        if (!clock.empty()) return clock;
    }
    return {};
}

struct BidEram {
    std::string name;
    std::string rid;
    std::string wid;
    std::string model_type;
    std::string width_a;
    std::string width_b;
    std::string debuggable;
    std::string initialized;
    std::string width;
    std::string depth;
    std::string address_offset;
    std::string data_offset;
    std::string address_step;
    std::string num_section;
    std::string section_size;
    std::string width_per_section;
    std::string bytes_in_per_section;
    std::string wm_depth;
    std::string wm_width;
    std::string wm_num_byte;
    std::string wm_ecc;
};

std::string TagText(const std::string& block, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const std::size_t at = block.find(open);
    if (at == std::string::npos) return {};
    const std::size_t begin = at + open.size();
    const std::size_t end = block.find(close, begin);
    if (end == std::string::npos) return {};
    return block.substr(begin, end - begin);
}

std::vector<BidEram> ParseBidErams(const std::string& text, const std::string& inst) {
    std::vector<BidEram> out;
    const std::string prefix = inst + "/";
    std::size_t at = 0;
    while ((at = text.find("<INST_", at)) != std::string::npos) {
        const std::size_t end = text.find("</INST_", at);
        if (end == std::string::npos) break;
        const std::string block = text.substr(at, end - at);
        at = end;
        const std::string name = TagText(block, "name");
        if (name.rfind(prefix, 0) != 0) continue;
        BidEram eram;
        eram.name = name;
        eram.rid = TagText(block, "rid");
        eram.wid = TagText(block, "wid");
        eram.model_type = TagText(block, "model_type");
        eram.width_a = TagText(block, "width_a");
        eram.width_b = TagText(block, "width_b");
        eram.debuggable = TagText(block, "is_debuggable");
        eram.initialized = TagText(block, "is_initialize");
        const std::size_t sub = block.find("<sub_bid_info>");
        const std::string sub_block =
            sub == std::string::npos ? block : block.substr(sub);
        eram.width = TagText(sub_block, "width");
        eram.depth = TagText(sub_block, "depth");
        eram.address_offset = TagText(sub_block, "address_offset");
        eram.data_offset = TagText(sub_block, "data_offset");
        eram.num_section = TagText(sub_block, "num_section");
        eram.section_size = TagText(sub_block, "section_size");
        eram.width_per_section = TagText(sub_block, "width_per_section");
        eram.bytes_in_per_section = TagText(sub_block, "bytes_in_per_section");
        eram.address_step = TagText(sub_block, "address_step");
        eram.wm_depth = TagText(sub_block, "depth");
        eram.wm_width = TagText(sub_block, "width");
        eram.wm_num_byte = TagText(sub_block, "num_byte");
        eram.wm_ecc = TagText(sub_block, "ecc");
        out.push_back(std::move(eram));
    }
    std::sort(out.begin(), out.end(), [](const BidEram& a, const BidEram& b) {
        return std::atoi(a.data_offset.c_str()) < std::atoi(b.data_offset.c_str());
    });
    return out;
}

bool EnableWord(const std::string& value) {
    return value == "y" || value == "Y" || value == "1" || value == "enable";
}

}  // namespace

// 生成 cwc 文本；失败时返回空串并把原因写进 error。
std::string BuildCwcFromWatcher(const project::Project& project, const MdnConfig& mdn,
                                std::string* error, jsonrpc::Json* info) {
    const auto fail = [error, info](const std::string& why) {
        if (error != nullptr) *error = why;
        if (info != nullptr) (*info)["ok"] = false;
        return std::string();
    };
    if (!mdn.exists || mdn.signals.empty()) return fail("no probes marked in chip_debugger/user.mdn");

    // 1) 按 mdn 顺序把信号分组回总线（ChipWatcher 的"总线"= 一个信号）
    std::vector<std::pair<std::string, int>> buses;
    for (const std::string& signal : mdn.signals) {
        std::string base = signal;
        const std::size_t open = signal.rfind('[');
        if (open != std::string::npos) base = signal.substr(0, open);
        if (buses.empty() || buses.back().first != base) {
            if (std::find_if(buses.begin(), buses.end(), [&base](const auto& item) {
                    return item.first == base;
                }) != buses.end()) {
            }
            buses.emplace_back(base, 0);
        }
        ++buses.back().second;
    }

    // 2) 读 TD 的 watcher 元数据（总线数/位宽/DIN 位置/深度）
    const std::string dir = NewestWatcherDir(project);
    if (dir.empty()) return fail("no watcher OOC artifacts (syn_1/cw/auto_created_cwc*) - the "
                                 "ChipWatcher was never built; run stages:[\"all\"]");
    const std::filesystem::path dir_path(dir);
    std::string sv_path;
    std::string sv_name;
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir_path, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > 3 && name.rfind(".sv") == name.size() - 3) {
                sv_path = it->path().string();
                sv_name = name;
                break;
            }
        }
    }
    if (sv_path.empty()) return fail("no *_watcherInst.sv under " + dir);
    const std::string sv = ReadText(sv_path);
    WatcherInst inst;
    ParseIntAfter(sv, "CWC_BUS_NUM", &inst.bus_num);
    ParseIntAfter(sv, "CWC_BUS_DIN_NUM", &inst.din_num);
    ParseIntAfter(sv, "RAM_LEN", &inst.ram_len);
    ParseIntAfter(sv, "RAM_DATA_DEPTH", &inst.depth);
    ParseIntAfter(sv, "INPUT_PIPE_NUM", &inst.pipe_in);
    ParseIntAfter(sv, "OUTPUT_PIPE_NUM", &inst.pipe_out);
    ParseIntListAfter(sv, "CWC_BUS_WIDTH", &inst.widths);
    ParseIntListAfter(sv, "CWC_BUS_DIN_POS", &inst.din_pos);

    int total = 0;
    for (const auto& bus : buses) total += bus.second;
    if (static_cast<int>(buses.size()) != inst.bus_num || total != inst.din_num ||
        inst.widths.size() != buses.size()) {
        return fail("mdn/watcher mismatch: mdn has " + std::to_string(buses.size()) + " buses / " +
                    std::to_string(total) + " bits, watcher has " +
                    std::to_string(inst.bus_num) + " buses / " + std::to_string(inst.din_num) +
                    " bits - rerun stages:[\"all\"] so the watcher matches user.mdn");
    }
    // 顺序不一致 = bit 里的 watcher 是从**另一份** probe 清单建出来的（典型：改完 mdn 只
    // 重跑过 phy，或 mdn 在 flow 之后又被写了一次）。这种情况只能重跑 syn 让它重建，
    // 不能"猜"哪个 16 位总线对哪个 slot —— 猜错就是错的采样数据。
    {
        std::vector<int> mdn_widths;
        for (const auto& bus : buses) mdn_widths.push_back(bus.second);
        std::vector<int> watcher_widths = inst.widths;
        std::sort(mdn_widths.begin(), mdn_widths.end());
        std::sort(watcher_widths.begin(), watcher_widths.end());
        const bool same_set = mdn_widths == watcher_widths;
        for (std::size_t i = 0; i < buses.size(); ++i) {
            if (buses[i].second != inst.widths[i]) {
                return fail(
                    same_set
                        ? "the watcher in the bitstream was built from a DIFFERENT probe list "
                          "(same widths, different order): rerun stages:[\"all\"] so user.mdn and "
                          "the ChipWatcher agree - do not guess the bus order, wrong order means "
                          "wrong capture data"
                        : "bus " + buses[i].first + " width " + std::to_string(buses[i].second) +
                              " != watcher width " + std::to_string(inst.widths[i]) +
                              " - rerun stages:[\"all\"]");
            }
        }
    }

    // 3) 采集 RAM 的 ERAM 映射（rid/wid/offset）
    std::string cwc1_name = dir_path.filename().string();  // auto_created_cwc1
    const std::string bid_path = project.dir + "/" + project.name + "_Runs/phy_1/" +
                                 project.name + "_inst.bid";
    if (!IsFile(bid_path)) return fail("no " + bid_path + " - run the phy stage first");
    const std::vector<BidEram> erams = ParseBidErams(ReadText(bid_path), cwc1_name);
    if (erams.empty()) return fail("no ERAM entries for " + cwc1_name + " in " + bid_path);

    // 4) 采样时钟（TD 不落文本，只能从历史 cwc 取）
    const std::string clock = ResolveSampleClock(project);
    if (clock.empty()) {
        return fail("cannot determine the ChipWatcher sample clock: no existing .cwc anywhere "
                    "under phy_1/cw, .fpgalsp/probe or cwc/. Save the ChipWatcher once from the "
                    "TD GUI (or capture once with a valid cwc) and retry");
    }

    // 5) 拼 XML（格式与 TD 自己写出的 compiled.cwc 逐字段一致）
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out += "<chipwatcher version=\"3.070000\" project_name=\"" + project.name + "\" chip_name=\"" +
           project.device + "\">\n";
    out += "\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        out += "\t\t<bus name=\"" + bus.first + "[" + std::to_string(bus.second - 1) + ":0]\" clock=\"" +
               clock + "\" inst=\"" + cwc1_name + "\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"yes\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            out += "\t\t\t<net name=\"" + bus.first + "[" + std::to_string(bit) +
                   "]\" const=\"false\" data=\"enable\" trig=\"enable\"/>\n";
        }
        out += "\t\t</bus>\n";
    }
    out += "\t</watcher_nodes>\n";
    out += "\t<instance name=\"" + cwc1_name + "\" id=\"0\" ipgen=\"false\" inst_enable=\"true\">\n";
    out += "\t\t<clock clk_name=\"" + clock + "\"/>\n";
    out += "\t\t<config bram_name=\"" + cwc1_name + "/wrapper_cwc_top/U_cwc_ram\" debughub_name=\"\" " +
           "input_pipe_num=\"" + std::to_string(inst.pipe_in) + "\" output_pipe_num=\"" +
           std::to_string(inst.pipe_out) + "\" sample_depth=\"" + std::to_string(inst.depth) +
           "\"/>\n";
    out += "\t\t<signal_vec>\n\t\t\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        out += "\t\t\t\t<bus name=\"" + bus.first + "[" + std::to_string(bus.second - 1) +
               ":0]\" compatator_num=\"1\" op=\"dont_care\" capture_op=\"dont_care\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            out += "\t\t\t\t\t<net name=\"" + bus.first + "[" + std::to_string(bit) +
                   "]\" cond=\"low\" capture_cond=\"low\"/>\n";
        }
        out += "\t\t\t\t</bus>\n";
    }
    out += "\t\t\t</watcher_nodes>\n\t\t</signal_vec>\n";
    out += "\t\t<trigger name=\"" + cwc1_name + "_trigger\" position=\"127\" window_num=\"1\" " +
           "window_depth=\"" + std::to_string(inst.depth) + "\">\n";
    out += std::string("\t\t\t<condition level=\"1\" enabled=\"yes\" type=\"basic_and\" ") +
           "capture_type=\"basic_and\"/>\n";
    out += "\t\t</trigger>\n";
    out += "\t\t<logic_eram name=\"" + cwc1_name + "/wrapper_cwc_top/U_cwc_ram\" width=\"" +
           std::to_string(inst.ram_len) + "\" depth=\"" + std::to_string(inst.depth) + "\">\n";
    for (const BidEram& eram : erams) {
        out += "\t\t\t<phy_eram name=\"" + eram.name + "\" wid=\"" + eram.wid + "\" rid=\"" + eram.rid +
               "\" model_type=\"" + eram.model_type + "\" width_a=\"" + eram.width_a +
               "\" width_b=\"" + eram.width_b + "\" debuggable=\"" +
               (EnableWord(eram.debuggable) ? "enable" : "disable") + "\" initialized=\"" +
               (EnableWord(eram.initialized) ? "enable" : "disable") + "\" width=\"" + eram.width +
               "\" depth=\"" + eram.depth + "\" address_offset=\"" + eram.address_offset +
               "\" data_offset=\"" + eram.data_offset + "\" address_step=\"" + eram.address_step +
               "\" num_section=\"" + eram.num_section + "\" section_size=\"" + eram.section_size +
               "\" width_per_section=\"" + eram.width_per_section +
               "\" bytes_in_per_section=\"" + eram.bytes_in_per_section +
               "\" working_mode_depth=\"" + eram.wm_depth + "\" working_mode_width=\"" +
               eram.wm_width + "\" working_mode_ecc=\"" +
               (eram.wm_ecc == "0" ? "disable" : "enable") + "\" working_mode_num_byte=\"" +
               eram.wm_num_byte + "\" working_mode_super_wide=\"disable\"/>\n";
    }
    out += "\t\t</logic_eram>\n\t</instance>\n</chipwatcher>\n";

    if (info != nullptr) {
        (*info)["ok"] = true;
        (*info)["source_dir"] = dir;
        (*info)["watcher_sv"] = sv_name;
        (*info)["clock"] = clock;
        (*info)["depth"] = inst.depth;
        (*info)["buses"] = static_cast<int>(buses.size());
        (*info)["bits"] = total;
        (*info)["eram"] = static_cast<int>(erams.size());
    }
    return out;
}

jsonrpc::Json EnsureCompiledCwc(const project::Project& project) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["path"] = CompiledCwc(project);
    if (IsFile(CompiledCwc(project))) {
        const CwcSnapshot existing = ReadCwc(CompiledCwc(project));
        if (existing.ok && existing.net_count > 0) {
            out["ok"] = true;
            out["existed"] = true;
            out["written"] = false;
            out["net_count"] = existing.net_count;
            return out;
        }
    }
    const MdnConfig mdn = ReadMdn(MdnPath(project));
    std::string error;
    jsonrpc::Json info = jsonrpc::Json::object();
    const std::string text = BuildCwcFromWatcher(project, mdn, &error, &info);
    out["existed"] = false;
    out["generated"] = info;
    if (text.empty()) {
        out["reason"] = error.empty() ? "cannot rebuild compiled.cwc" : error;
        return out;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(CompiledCwc(project)).parent_path(), ec);
    if (!fpgalsp::platform::write_file_utf8(CompiledCwc(project), text)) {
        out["reason"] = "cannot write " + CompiledCwc(project);
        return out;
    }
    const CwcSnapshot written = ReadCwc(CompiledCwc(project));
    out["ok"] = written.ok && written.net_count > 0;
    out["written"] = true;
    out["net_count"] = written.net_count;
    if (!out["ok"].get<bool>()) out["reason"] = "generated cwc does not parse back";
    return out;
}

jsonrpc::Json Status(const project::Project& project) {
    // 批量 phy 不写 sidecar：能重建就先重建，避免 Status 把"位流里有 watcher"误报成没探针。
    const jsonrpc::Json ensured = EnsureCompiledCwc(project);
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["project"] = project.name;
    out["target"] = project.device;
    out["cwc"] = ensured;

    const MdnConfig mdn = ReadMdn(MdnPath(project));
    {
        jsonrpc::Json mdn_json = jsonrpc::Json::object();
        mdn_json["path"] = mdn.path;
        mdn_json["exists"] = mdn.exists;
        mdn_json["count"] = static_cast<int>(mdn.signals.size());
        jsonrpc::Json signals = jsonrpc::Json::array();
        for (const std::string& signal : mdn.signals) signals.push_back(signal);
        mdn_json["signals"] = std::move(signals);
        out["chip_debugger"] = std::move(mdn_json);
    }

    const CwcSnapshot cwc = ReadCwc(CompiledCwc(project));
    jsonrpc::Json watcher = jsonrpc::Json::object();
    watcher["present"] = cwc.ok;
    watcher["compiled_cwc"] = CompiledCwc(project);
    watcher["compiled_bit"] = CompiledBit(project);
    watcher["bit_exists"] = IsFile(CompiledBit(project));
    watcher["instance"] = cwc.instance;
    watcher["clock"] = cwc.clock;
    watcher["chip"] = cwc.chip;
    watcher["sample_depth"] = cwc.sample_depth;
    watcher["trigger_position"] = cwc.trigger_position;
    watcher["trigger_type"] = cwc.trigger_type;
    watcher["net_count"] = cwc.net_count;
    jsonrpc::Json buses = jsonrpc::Json::array();
    for (const CwcBus& bus : cwc.buses) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = bus.name;
        item["clock"] = bus.clock;
        item["type"] = bus.type;
        item["enabled"] = bus.enabled;
        buses.push_back(std::move(item));
    }
    watcher["buses"] = std::move(buses);
    if (!cwc.ok) watcher["error"] = cwc.error;

    // 探针清单 vs 已经嵌进 bit 的 watcher。
    // ⚠ 这是整个功能最容易踩的坑：改了 user.mdn 之后跑「增量布线」或只跑 route，
    //   insert_debugger（在 opt_place 阶段）不会重新执行，compiled.cwc 时间戳不变，
    //   探针根本没进 bit。这里把差异算出来，避免"以为改了其实没生效"。
    {
        const auto wanted = GroupBitSignals(mdn.signals, project.top);
        const auto embedded = GroupBusRanges(cwc.buses);
        jsonrpc::Json missing = jsonrpc::Json::array();
        jsonrpc::Json extra = jsonrpc::Json::array();
        for (const auto& [base, bits] : wanted) {
            const auto found = embedded.find(base);
            if (found == embedded.end() || found->second != bits) missing.push_back(base);
        }
        for (const auto& [base, bits] : embedded) {
            const auto found = wanted.find(base);
            if (found == wanted.end() || found->second != bits) extra.push_back(base);
        }
        const bool stale = cwc.ok && mdn.exists && (!missing.empty() || !extra.empty());
        watcher["stale"] = stale;
        watcher["missing_in_bit"] = std::move(missing);
        watcher["extra_in_bit"] = std::move(extra);
    }
    const bool stale = watcher.value("stale", false);
    out["watcher"] = std::move(watcher);

    // 平铺的 clock/sample_depth 保留给现有测试与插件；sampling 是"能不能改"的显式答案。
    out["sampling"] = SamplingJson(cwc);

    bool ready = cwc.ok && IsFile(CompiledBit(project)) && !stale;
    out["ready"] = ready;
    out["matches_user_mdn"] = !stale;
    if (!mdn.exists) {
        out["reason"] =
            "no chip_debugger/user.mdn - no probe signal is marked (TD will not insert a watcher)";
    } else if (!cwc.ok) {
        // 区分两种完全不同的状态：
        //   ① 探针压根没建 watcher  → 要重跑 syn（gate 之后才建实例）；
        //   ② watcher 已经建好并进了位流，只是 TD 在批量模式下**没有写** compiled.cwc
        //      （只有 GUI 的 ChipWatcher 存盘会写）→ 位流可用，缺的只是抓取用的 sidecar。
        // 实测（2026-11-21）：后者非常常见，之前一律回报 "run the phy flow"，会让
        // agent 反复重跑 5–10 分钟的流程却永远拿不到 cwc。
        bool watcher_built = false;
        const std::string runs = project.dir + "/" + project.name + "_Runs";
        for (const char* run : {"syn_1", "phy_1"}) {
            const std::filesystem::path cw = std::filesystem::path(runs + "/" + run + "/cw");
            std::error_code ec;
            if (!std::filesystem::is_directory(cw, ec)) continue;
            for (std::filesystem::directory_iterator it(cw, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->path().filename().string().find("cwc") != std::string::npos) {
                    watcher_built = true;
                    break;
                }
            }
            if (watcher_built) break;
        }
        out["watcher_built"] = watcher_built;
        out["reason"] =
            watcher_built
                ? "the ChipWatcher was built and placed (syn_1/cw/auto_created_cwc* exists) but "
                  "TD did not write phy_1/cw/compiled.cwc - batch runs never write that sidecar. "
                  "Copy any cwc that matches the current user.mdn there (e.g. an earlier "
                  ".fpgalsp/probe/trigger_*.cwc), or save once from the TD GUI; capture needs it "
                  "for the instance name and trigger setup."
                : "no ChipWatcher yet - run stages [\"all\"] (NOT [\"phy\"]): TD creates the "
                  "watcher right after Optimize Gate, which belongs to the syn run";
    } else if (!IsFile(CompiledBit(project))) {
        out["reason"] = "compiled.bit is missing - run the phy flow again";
    } else if (stale) {
        out["reason"] =
            "chip_debugger/user.mdn changed but the probes in the bitstream were not rebuilt - "
            "run Place & Route (stages [\"phy\"]); the incremental route does NOT re-insert the "
            "debugger";
    } else {
        out["reason"] = "ready to capture on " + cwc.clock;
        out["ok"] = true;
    }
    return out;
}

jsonrpc::Json Configure(const project::Project& project, const std::vector<std::string>& signals,
                        bool write) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["path"] = MdnPath(project);

    const MdnConfig before = ReadMdn(MdnPath(project));
    const std::vector<std::string> after =
        signals.empty() ? before.signals : NormaliseSignals(signals);
    const std::set<std::string> old_set(before.signals.begin(), before.signals.end());
    const std::set<std::string> new_set(after.begin(), after.end());

    jsonrpc::Json added = jsonrpc::Json::array();
    jsonrpc::Json removed = jsonrpc::Json::array();
    for (const std::string& item : after) {
        if (old_set.find(item) == old_set.end()) added.push_back(item);
    }
    for (const std::string& item : before.signals) {
        if (new_set.find(item) == new_set.end()) removed.push_back(item);
    }

    jsonrpc::Json before_json = jsonrpc::Json::array();
    for (const std::string& item : before.signals) before_json.push_back(item);
    jsonrpc::Json after_json = jsonrpc::Json::array();
    for (const std::string& item : after) after_json.push_back(item);

    out["before"] = std::move(before_json);
    out["after"] = std::move(after_json);
    out["added"] = std::move(added);
    out["removed"] = std::move(removed);
    out["count"] = static_cast<int>(after.size());

    // ---- 预算校验：ChipWatcher 的硬上限 ----
    // 实测（2026-11-21，干净上下文 agent + TD 2026.2_SP1）：
    //   * 4 条总线 / 64 bit 以内 → gate 之后会建出 `auto_created_cwcN`，探针进 bit；
    //   * 5 条总线 → 第 5 条**静默丢弃**（前 4 条照常），没有任何报错；
    //   * 6 条总线 / 70 bit → `probe_status` 只剩 `empty ChipWatcher configuration`，
    //     位流照常产出、探针全丢。
    // TD 在 insert_debugger 那一步不告警，所以必须在写 mdn 之前就挡住，
    // 否则一次白跑就是 5–10 分钟的 syn+phy。
    std::map<std::string, int> bus_widths;
    for (const std::string& signal : after) {
        const std::size_t open = signal.rfind('[');
        const std::string base = open == std::string::npos ? signal : signal.substr(0, open);
        ++bus_widths[base];
    }
    constexpr int kMaxBuses = 4;
    constexpr int kMaxBits = 64;
    jsonrpc::Json buses_json = jsonrpc::Json::array();
    for (const auto& [name, width] : bus_widths) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = name;
        item["width"] = width;
        buses_json.push_back(std::move(item));
    }
    out["buses"] = std::move(buses_json);
    out["bus_count"] = static_cast<int>(bus_widths.size());
    out["bit_count"] = static_cast<int>(after.size());
    jsonrpc::Json limits = jsonrpc::Json::object();
    limits["buses"] = kMaxBuses;
    limits["bits"] = kMaxBits;
    out["limits"] = std::move(limits);

    const bool over_budget =
        static_cast<int>(bus_widths.size()) > kMaxBuses || static_cast<int>(after.size()) > kMaxBits;
    if (over_budget) {
        out["reason"] =
            "probe budget exceeded: TD's ChipWatcher accepts at most 4 buses / 64 bits - "
            "extra buses are dropped SILENTLY and an over-budget list yields "
            "'empty ChipWatcher configuration' after a full syn+phy run. "
            "Keep <=4 signals (a bus counts once, e.g. WHG/x[15:0]) and <=64 bits total.";
        return out;  // ok 保持 false：连 dry run 也不放行
    }

    if (!write) {
        out["ok"] = true;
        out["written"] = false;
        out["reason"] = "dry run (pass write:true to update chip_debugger/user.mdn)";
        return out;
    }
    if (after.empty()) {
        out["reason"] = "refusing to write an empty probe list (that would delete the watcher)";
        return out;
    }
    std::string error;
    if (!WriteMdn(MdnPath(project), after, &error)) {
        out["reason"] = error;
        return out;
    }
    out["ok"] = true;
    out["written"] = true;
    out["reason"] =
        "user.mdn updated - now run stages [\"all\"] (NOT [\"phy\"]): TD creates the "
        "ChipWatcher instance right after Optimize Gate, which belongs to the syn run. "
        "The backend adds syn automatically when user.mdn is newer than the gate database.";
    return out;
}

// -----------------------------------------------------------------------------
// 探针验收（只读）
// -----------------------------------------------------------------------------

namespace {

// 一根总线的请求位集合 + 每个 bit 对应的**原始 mdn 名字**（报 missing_nets 时要逐字还回去）。
struct RequestedBus {
    std::set<int> bits;
    std::map<int, std::string> names;
    std::vector<std::string> scalars;  // 不带 `[n]` 的标量名
};

// 去掉工程顶层前缀（`WHG/pwm_reg0_i[15:0]` -> `pwm_reg0_i[15:0]`）。
// mdn 里写的是层次名，compiled.cwc 里的 bus 名可能不带顶层 —— 两边都要归一化到同一侧，
// 否则每条探针都会被判成 missing（Status 只剥 mdn 一侧，那是历史行为，这里不动它）。
std::string StripTopPrefix(std::string name, std::string_view top) {
    if (!top.empty()) {
        const std::string prefix = std::string(top) + "/";
        if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    }
    return Trim(name);
}

// `pwm_reg0_i[3]` / `pwm_reg0_i[15:0]` / `pwm_reg0_i` -> base
std::string BusBase(std::string_view name) {
    const std::size_t open = name.find_last_of('[');
    return Trim(open == std::string_view::npos ? name : name.substr(0, open));
}

// 解析 `base[bit]`；不是这个形态就返回 false。
bool ParseBitName(std::string_view name, std::string* base, int* bit) {
    const std::size_t open = name.find_last_of('[');
    if (open == std::string_view::npos || name.empty() || name.back() != ']') return false;
    const std::string_view index = name.substr(open + 1, name.size() - open - 2);
    if (index.empty()) return false;
    for (const char c : index) {
        if (c < '0' || c > '9') return false;
    }
    *base = Trim(name.substr(0, open));
    *bit = std::atoi(std::string(index).c_str());
    return true;
}

// 解析 `base[msb:lsb]`；不是这个形态就返回 false。
bool ParseBusRange(std::string_view name, std::string* base, int* msb, int* lsb) {
    const std::size_t open = name.find_last_of('[');
    if (open == std::string_view::npos || name.empty() || name.back() != ']') return false;
    const std::string inner(name.substr(open + 1, name.size() - open - 2));
    const std::size_t colon = inner.find(':');
    if (colon == std::string::npos) return false;
    const std::string a = Trim(inner.substr(0, colon));
    const std::string b = Trim(inner.substr(colon + 1));
    if (a.empty() || b.empty()) return false;
    for (const char c : a) {
        if (c < '0' || c > '9') return false;
    }
    for (const char c : b) {
        if (c < '0' || c > '9') return false;
    }
    *base = Trim(name.substr(0, open));
    *msb = std::atoi(a.c_str());
    *lsb = std::atoi(b.c_str());
    return true;
}

// mdn 清单 -> base -> 请求位（按位号对齐，这就是"逐位比对"的一半）。
std::map<std::string, RequestedBus> CollectRequested(const MdnConfig& mdn,
                                                     std::string_view top) {
    std::map<std::string, RequestedBus> out;
    for (const std::string& raw : mdn.signals) {
        const std::string name = StripTopPrefix(raw, top);
        if (name.empty()) continue;
        std::string base;
        int bit = 0;
        if (ParseBitName(name, &base, &bit)) {
            out[base].bits.insert(bit);
            // 同一个 bit 写两次时保留第一次的写法（mdn 已去重，这里只是兜底）
            out[base].names.emplace(bit, raw);
        } else {
            out[BusBase(name)].scalars.push_back(raw);
        }
    }
    return out;
}

// compiled.cwc 的 bus/net 清单 -> base -> 真正嵌进 bit 的位（逐位比对的另一半）。
// ⚠ bus 名带 `[msb:lsb]` 时直接用范围；不带时用它的 `<net name="base[i]">` 还原
//   （两种写法都在真实 compiled.cwc 里出现过）。
std::map<std::string, std::set<int>> CollectEmbedded(const CwcSnapshot& cwc,
                                                    std::string_view top) {
    std::map<std::string, std::set<int>> out;
    for (const CwcBus& bus : cwc.buses) {
        const std::string name = StripTopPrefix(bus.name, top);
        std::string base;
        int msb = 0;
        int lsb = 0;
        if (ParseBusRange(name, &base, &msb, &lsb)) {
            std::set<int>& bits = out[base];
            const int lo = std::min(msb, lsb);
            const int hi = std::max(msb, lsb);
            for (int bit = lo; bit <= hi && bit - lo <= 4096; ++bit) bits.insert(bit);
            continue;
        }
        // 不带范围的写法：先按名字建条目（可能确实是标量），再从 net 名补位号
        std::set<int>& bits = out[BusBase(name)];
        for (const std::string& net_raw : bus.nets) {
            const std::string net = StripTopPrefix(net_raw, top);
            std::string net_base;
            int bit = 0;
            if (!ParseBitName(net, &net_base, &bit)) continue;
            if (net_base == BusBase(name)) bits.insert(bit);
        }
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

}  // namespace

jsonrpc::Json Verify(const project::Project& project, bool include_bits,
                     const std::function<bool(std::string_view)>& known_name) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = true;

    const MdnConfig mdn = ReadMdn(MdnPath(project));
    const std::string cwc_path = CompiledCwc(project);
    const bool has_cwc_file = IsFile(cwc_path);
    CwcSnapshot cwc;
    if (has_cwc_file) cwc = ReadCwc(cwc_path);
    // cwc 不存在**或**解析不出 bus：bit 里就是没有探针，所有请求都算 missing。
    const bool no_bitstream = !has_cwc_file || !cwc.ok;

    const auto requested = CollectRequested(mdn, project.top);
    const auto embedded = no_bitstream ? std::map<std::string, std::set<int>>{}
                                       : CollectEmbedded(cwc, project.top);

    int requested_bits = 0;
    int embedded_bits = 0;
    std::vector<std::string> missing_nets;
    std::vector<std::string> not_found_names;  // not-in-build 的那些 base（hints 用）

    jsonrpc::Json buses = jsonrpc::Json::array();
    for (const auto& [base, want] : requested) {
        const auto found = embedded.find(base);
        const std::set<int>* have = found == embedded.end() ? nullptr : &found->second;

        int width = static_cast<int>(want.bits.size());
        int hit = 0;
        for (const int bit : want.bits) {
            if (have != nullptr && have->count(bit) != 0) {
                ++hit;
            } else {
                const auto name = want.names.find(bit);
                if (name != want.names.end()) missing_nets.push_back(name->second);
            }
        }
        // 标量（mdn 里没带 `[n]`）：只要 cwc 里有同名 bus 就算嵌进去了
        for (const std::string& scalar : want.scalars) {
            ++width;
            if (have != nullptr) {
                ++hit;
            } else {
                missing_nets.push_back(scalar);
            }
        }
        requested_bits += width;
        embedded_bits += hit;
        const int missing = width - hit;

        std::string status;
        std::string reason;
        if (missing == 0) {
            status = "embedded";
        } else if (hit > 0) {
            status = "partial";
            reason = "partial";
        } else {
            status = "dropped";
            if (no_bitstream) {
                reason = "no-bitstream";
            } else if (known_name && !known_name(base)) {
                // 名字在设计里找不到 —— 要么拼错，要么这个网根本没进构建
                reason = "not-in-build";
                not_found_names.push_back(base);
            } else {
                reason = "dropped-by-td";
            }
        }

        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = base;
        item["width"] = width;
        item["embedded_bits"] = hit;
        item["missing_bits"] = missing;
        item["status"] = status;
        item["reason"] = reason;
        buses.push_back(std::move(item));
    }

    // 多出来的（bit 里有、mdn 里没写）= stale 的另一半
    int extra_bits = 0;
    for (const auto& [base, bits] : embedded) {
        const auto want = requested.find(base);
        if (want == requested.end()) {
            extra_bits += static_cast<int>(bits.size()) + 1;  // +1：这条 bus 本身也算多
            continue;
        }
        for (const int bit : bits) {
            if (want->second.bits.count(bit) == 0) ++extra_bits;
        }
    }

    const int missing_total = requested_bits - embedded_bits;
    jsonrpc::Json signals = jsonrpc::Json::object();
    signals["requested"] = requested_bits;
    signals["embedded"] = embedded_bits;
    signals["missing"] = missing_total;
    out["signals"] = std::move(signals);
    out["buses"] = std::move(buses);

    // 逐位全量（include_bits:true）或前 32 条 + truncated（默认）—— 64+ 位的清单
    // 整段塞进 MCP 响应没有意义，agent 需要的是"哪几根"而不是"全部 1024 根"。
    constexpr std::size_t kMissingNetLimit = 32;
    jsonrpc::Json missing = jsonrpc::Json::array();
    bool truncated = false;
    for (const std::string& name : missing_nets) {
        if (!include_bits && missing.size() >= kMissingNetLimit) {
            truncated = true;
            break;
        }
        missing.push_back(name);
    }
    out["missing_nets"] = std::move(missing);
    out["truncated"] = truncated;
    out["sampling"] = SamplingJson(cwc);

    const bool stale =
        cwc.ok && mdn.exists && (missing_total > 0 || extra_bits > 0);
    out["stale"] = stale;

    // hints：一句话、可照做。顺序按"最该先做什么"排：先补配置，再重跑 phy，最后才是排错。
    jsonrpc::Json hints = jsonrpc::Json::array();
    if (!mdn.exists) {
        hints.push_back(
            "chip_debugger/user.mdn does not exist: mark the probe signals first "
            "(fpga_probe_config) and rerun syn+phy.");
    }
    if (no_bitstream) {
        hints.push_back(
            "No ChipWatcher configuration yet: run phy (after changing the mdn you must rerun "
            "syn+phy, not just route) so the probes get embedded into phy_1/cw/compiled.cwc.");
    }
    if (missing_total > 0 && !no_bitstream) {
        hints.push_back(
            "TD dropped these nets: cross-clock-domain (no derivable trigger clock) or "
            "optimized away; add (* synthesis keep *) or use a same-domain mirror register.");
    }
    if (stale) {
        hints.push_back(
            "The probes in the bitstream do not match the current mdn; rerun phy so the bit "
            "matches the mdn.");
    }
    if (!not_found_names.empty()) {
        hints.push_back(
            "Some requested names are not found in the project HDL index (" +
            JoinList(not_found_names) +
            "): confirm the spelling with fpga_symbol_path before rerunning phy.");
    }
    if (missing_total == 0 && !no_bitstream && mdn.exists) {
        hints.push_back("All requested probes are embedded in the bitstream; capture is safe.");
    }
    out["hints"] = std::move(hints);
    return out;
}

// -----------------------------------------------------------------------------
// 触发配置（运行期可改）
// -----------------------------------------------------------------------------

std::string PatchTrigger(std::string_view cwc_text, const CaptureOptions::Trigger& trigger) {
    std::string text(cwc_text);
    if (!trigger.Any()) return text;

    const auto set_attr = [&text](const std::string& pattern, const std::string& value) {
        const RE2 re(pattern);
        if (!re.ok()) return;
        (void)RE2::GlobalReplace(&text, re, value);
    };

    if (trigger.position >= 0) {
        set_attr(R"((<trigger\b[^>]*?\bposition=")\d+("))",
                 "\\1" + std::to_string(trigger.position) + "\\2");
    }
    if (trigger.window_num > 0) {
        set_attr(R"((<trigger\b[^>]*?\bwindow_num=")\d+("))",
                 "\\1" + std::to_string(trigger.window_num) + "\\2");
    }
    if (trigger.window_depth > 0) {
        set_attr(R"((<trigger\b[^>]*?\bwindow_depth=")\d+("))",
                 "\\1" + std::to_string(trigger.window_depth) + "\\2");
    }
    if (!trigger.type.empty()) {
        set_attr(R"((<condition\b[^>]*?\btype=")[^"]*("))", "\\1" + trigger.type + "\\2");
        set_attr(R"((<condition\b[^>]*?\bcapture_type=")[^"]*("))", "\\1" + trigger.type + "\\2");
    }
    if (trigger.enabled.has_value()) {
        const std::string value = *trigger.enabled ? "yes" : "no";
        set_attr(R"((<condition\b[^>]*?\benabled=")[^"]*("))", "\\1" + value + "\\2");
    }
    if (!trigger.bus_op.empty()) {
        // 实例里的 bus 带 `op=...`（顶层 watcher_nodes 那组没有，所以只会命中实例那组）
        set_attr(R"((<bus\b[^>]*?\bop=")[^"]*("))", "\\1" + trigger.bus_op + "\\2");
        set_attr(R"((<bus\b[^>]*?\bcapture_op=")[^"]*("))", "\\1" + trigger.bus_op + "\\2");
    }
    for (const auto& [net, cond] : trigger.net_conds) {
        if (net.empty() || cond.empty()) continue;
        // 只改**有 cond 属性**的那条 <net>（顶层那条是 const/data/trig，不该动）
        const std::string pattern =
            R"((<net\b[^>]*?\bname=")" + RE2::QuoteMeta(net) + R"("[^>]*?\bcond=")[^"]*(")";
        set_attr(pattern, "\\1" + net + "\\2" + cond + "\\3");
    }
    for (const auto& bus : trigger.buses) {
        if (bus.name.empty()) continue;
        // `pwm_reg0_i` / `pwm_reg0_i[15:0]` -> base + [msb:lsb]
        static const RE2 kBusRange(R"(^(.+?)\[(\d+):(\d+)\])");
        std::string base = bus.name;
        std::string msb_s;
        std::string lsb_s;
        int msb = 0;
        int lsb = 0;
        if (RE2::FullMatch(bus.name, kBusRange, &base, &msb_s, &lsb_s)) {
            msb = std::atoi(msb_s.c_str());
            lsb = std::atoi(lsb_s.c_str());
        } else {
            // `pwm_reg0_i` 这种没带宽度的写法：从 .cwc 的 bus 名里补
            const RE2 find_bus("(<bus\\b[^>]*?\\bname=\")" + RE2::QuoteMeta(bus.name) +
                               "(\\[(\\d+):(\\d+)\\])");
            std::string full;
            std::string a;
            std::string b;
            if (RE2::PartialMatch(text, find_bus, &full, &a, &b)) {
                msb = std::atoi(a.c_str());
                lsb = std::atoi(b.c_str());
            } else {
                continue;  // 找不到这条总线，跳过而不是猜
            }
        }
        const int lo = std::min(msb, lsb);
        const int hi = std::max(msb, lsb);
        if (hi - lo > 64) continue;
        const std::string op = bus.op.empty() ? trigger.bus_op : bus.op;
        if (!op.empty()) {
            const std::string pattern = R"((<bus\b[^>]*?\bname=")" + RE2::QuoteMeta(base) +
                                        R"((\[[^\]]*\]")[^>]*?\bop=")[^"]*(")";
            set_attr(pattern, "\\1\\2" + base + "\\3\\4" + op + "\\5");
        }
        if (!bus.has_value) continue;
        for (int bit = lo; bit <= hi; ++bit) {
            const bool high = ((bus.value >> (bit - lo)) & 1ULL) != 0;
            const std::string net = base + "[" + std::to_string(bit) + "]";
            const std::string pattern = R"((<net\b[^>]*?\bname=")" + RE2::QuoteMeta(net) +
                                        R"("[^>]*?\bcond=")[^"]*(")";
            set_attr(pattern, "\\1" + net + "\\2" + (high ? "high" : "low") + "\\3");
        }
    }
    return text;
}

bool IsTriggerBusOp(std::string_view op) {
    // 与 tools.cpp 里 fpga_probe_capture 的 buses[].op 枚举逐字一致；
    // 这里做校验是为了让非法值在 MCP 层就变成 ToolError，而不是让 TD 报一句
    // 读不懂的话（TD 对未知 op 的表现是"静默按默认处理"）。
    static const std::set<std::string> kOps = {
        "equal",   "not_equal",     "less", "less_equal",
        "greater", "greater_equal", "bit_trigger",
    };
    return kOps.find(std::string(op)) != kOps.end();
}

// -----------------------------------------------------------------------------
// 采集
// -----------------------------------------------------------------------------

jsonrpc::Json Capture(const td::TdChoice& td, const project::Project& project,
                      const CaptureOptions& options) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["stage"] = "precheck";

    // 抓取前同样补齐 sidecar（批量 phy 不写它）。
    const jsonrpc::Json ensured = EnsureCompiledCwc(project);
    if (ensured.contains("written") && ensured["written"].get<bool>()) {
        out["cwc_generated"] = true;
    }
    const CwcSnapshot cwc = ReadCwc(CompiledCwc(project));
    if (!cwc.ok) {
        out["cwc"] = ensured;
        out["refused"] = true;
        out["reason"] = cwc.error.empty()
                            ? "no compiled ChipWatcher (phy_1/cw/compiled.cwc) - run the phy flow"
                            : cwc.error;
        return out;
    }
    if (!IsFile(CompiledBit(project))) {
        out["refused"] = true;
        out["reason"] = "phy_1/cw/compiled.bit is missing - run the phy flow again";
        return out;
    }

    // 清单与 bit 不一致 -> 默认拒绝（采回来的不是清单里写的信号）
    if (!options.allow_stale) {
        const MdnConfig mdn = ReadMdn(MdnPath(project));
        const auto wanted = GroupBitSignals(mdn.signals, project.top);
        const auto embedded = GroupBusRanges(cwc.buses);
        if (mdn.exists && wanted != embedded) {
            out["refused"] = true;
            out["stage"] = "stale";
            out["reason"] =
                "chip_debugger/user.mdn does not match the probes embedded in the bitstream - "
                "run Place & Route (stages [\"phy\"], the incremental route does not re-insert "
                "the debugger), or pass allow_stale:true to read the current bit anyway";
            return out;
        }
    }

    flow::HardwareLeaseGuard lease("probe");
    if (!lease.held()) {
        out["refused"] = true;
        out["stage"] = "busy";
        out["reason"] = "another hardware operation is running (" + flow::HardwareLease::Owner() +
                        "); wait for it to finish";
        return out;
    }

    CaptureOptions effective = options;
    if (effective.cwd.empty()) {
        effective.cwd = Slashes(project.dir + "/.fpgalsp/probe");
    }
    (void)fpgalsp::platform::ensure_directory(effective.cwd);
    const std::string instance =
        effective.instance.empty() ? (cwc.instance.empty() ? "auto_created_cwc1" : cwc.instance)
                                   : effective.instance;
    std::string csv = effective.csv_path;
    if (csv.empty()) {
        csv = RunsDir(project) + "/phy_1/cw/capture_" + Timestamp() + ".csv";
    }
    csv = Slashes(csv);
    {
        const std::size_t slash = csv.find_last_of('/');
        if (slash != std::string::npos) {
            (void)fpgalsp::platform::ensure_directory(csv.substr(0, slash));
        }
    }

    out["stage"] = "session";
    out["instance"] = instance;
    out["clock"] = cwc.clock;
    out["sample_depth"] = cwc.sample_depth;

    jsonrpc::Json log = jsonrpc::Json::array();
    const auto record = [&log](const std::string& line) {
        if (!line.empty()) log.push_back(line);
    };

    td::TdSessionOptions session_options;
    session_options.work_dir = effective.cwd;
    std::unique_ptr<td::TdSession> session;
    try {
        session = std::make_unique<td::TdSession>(td, session_options);
    } catch (const std::exception& e) {
        out["reason"] = std::string("cannot start TD: ") + e.what();
        return out;
    }

    const auto fail = [&](const std::string& stage, const std::string& reason,
                          std::string_view text) {
        out["stage"] = stage;
        out["reason"] = reason;
        out["log"] = log;
        for (const std::string& line : ErrorLines(std::string(text), 6)) record(line);
        out["log"] = log;
        session->Kill();
        return out;
    };

    // 1) 器件库（db 名按 family 猜，失败的候选逐个试，与 flow_runner 同法）
    {
        out["stage"] = "import_device";
        const std::vector<std::string> candidates = td::DbCandidates(td.root, project.family);
        bool imported = false;
        std::string last;
        for (const std::string& db : candidates) {
            const std::string command =
                td::MakeImportDeviceCommand(db, project.device, project.speed);
            last = session->Run(command, 180.0);
            if (!td::HasError(last)) {
                imported = true;
                break;
            }
        }
        if (!imported) {
            return fail("import_device", "cannot import the device database",
                        last.empty() ? std::string("no candidate in <td>/arch") : last);
        }
    }

    // 2) 打开工程（单 run 模式，只为了拿到 ChipWatcher 上下文）
    {
        out["stage"] = "open_project";
        const std::string command =
            "open_project " + td::TclQuoteGrouped(Slashes(project.al_path)) + " -single_run";
        const std::string text = session->Run(command, 300.0);
        // ⚠ 这里只能用 HasFatal（只看 ERROR），不能用 HasError：
        //    真实工程 `%WORKSPACE%\CorpusDelta` 的 RTL 自带
        //    `HDL-7225 CRITICAL-WARNING: 'busclk' is already implicitly declared`
        //    —— 它跟 open_project 成不成功毫无关系，用 HasError 会把正常的工程判成失败。
        if (td::HasFatal(text)) return fail("open_project", "open_project failed", text);
    }

    // 3) 导入 ChipWatcher 编译结果（**必须**带文件；不带参数是静默空操作）
    //    触发位置/窗口是运行期可改的：把 compiled.cwc 复制一份改掉 <trigger>，用它来
    //    config_chipwatcher（实测 position 127->512 后 CSV 的 Trigger 行就落在 512）。
    {
        out["stage"] = "config_chipwatcher";
        std::string cwc_path = CompiledCwc(project);
        if (effective.trigger.Any()) {
            const std::string patched = PatchTrigger(ReadText(CompiledCwc(project)),
                                                     effective.trigger);
            const std::string target = effective.cwd + "/trigger_" + Timestamp() + ".cwc";
            if (fpgalsp::platform::write_file_utf8(target, patched)) {
                cwc_path = target;
                out["trigger_cwc"] = target;
            } else {
                record("cannot write the patched .cwc, falling back to compiled.cwc");
            }
        }
        const std::string command = "config_chipwatcher " + td::TclQuoteGrouped(cwc_path);
        const std::string text = session->Run(command, 180.0);
        for (const std::string& line : ErrorLines(text, 4)) record(line);
        // 成功判据用**文本锚**为主、消息号为辅：
        //   文本锚 "chipwatcher: sanity check successfully" 在 2026.2(210850) 与
        //   2026.2_SP1(218141) 上都稳定出现；消息号是运行时"前缀+数字"拼出来的，
        //   在 exe 里扫不到，TD 升级后是否改号无法静态确认（SP1 实测仍是 KIT-1004）。
        //   两者取或，任一命中即认为配置已导入。
        const bool imported =
            text.find("sanity check successfully") != std::string::npos ||
            text.find("KIT-1004") != std::string::npos;
        // 判致命只看 ERROR：与其它 TD 步骤一致（CRITICAL-WARNING 只记录，不判死），
        // 否则 SP1 起"警告不中断 Flow"的新行为会在这里把成功的导入判成失败。
        if (td::HasFatal(text) || !imported) {
            return fail("config_chipwatcher",
                        "cannot import the ChipWatcher configuration", text);
        }
    }

    // 4) 拿实例句柄（必须带名字，否则 KIT-8414）
    //    ⚠ 拿到的是 TD 的 **对象**，不是字符串：后续命令必须用 `$fpgalsp_hw` 引用，
    //      直接把名字当参数传会报 `KIT-8000 ERROR: No watcher instance matched.`
    //      （本机实测：`run_hw_cw auto_created_cwc1` 失败，`set h [get_hw_cws ...]` +
    //        `run_hw_cw $h` 成功）。
    {
        out["stage"] = "get_hw_cws";
        const std::string command = "set fpgalsp_hw [get_hw_cws " + td::TclQuote(instance) + "]";
        const std::string text = session->Run(command, 60.0);
        if (td::HasError(text)) return fail("get_hw_cws", "watcher instance not found", text);
    }

    // 5) 连接下载线
    {
        out["stage"] = "connect_cable";
        const std::string text = session->Run("connect_cable -allink", 60.0);
        if (td::HasError(text)) return fail("connect_cable", "no cable/board detected", text);
    }

    // 6-9) 下载（按需）-> 触发 -> 等待 -> 落 CSV
    //   实测：GUI 的 ChipWatcher **不是每次抓取都先下载**，只要器件里跑的就是那份
    //   带探针的 bit 就能直接抓。所以默认 "auto"：先直接抓，抓不动再下载重抓一次
    //   （同一个会话内重试，省掉 open_project 的 40 s）。
    std::string download_mode = effective.download;
    for (char& c : download_mode) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const bool never_download = download_mode == "false" || download_mode == "never" ||
                                download_mode == "no" || download_mode == "0";
    const bool always_download = download_mode == "true" || download_mode == "always" ||
                                 download_mode == "yes" || download_mode == "1";

    std::string failure_text;
    const auto do_download = [&]() -> bool {
        out["stage"] = "download";
        const std::string spd = std::to_string(effective.speed);
        for (const char* mode : {"jtag_burst", "jtag"}) {
            std::string command = "download -bit " + td::TclQuoteGrouped(CompiledBit(project)) +
                                  " -mode " + mode + " -spd " + spd + " -cable " + effective.cable;
            if (std::string(mode) == "jtag_burst") {
                command += " -total_dev 1 -cur_dev 1 -bypass 11111111";
            }
            failure_text = session->Run(command, effective.session_timeout);
            for (const std::string& line : ErrorLines(failure_text, 4)) record(line);
            if (!td::HasError(failure_text) &&
                failure_text.find("Chip validation success") != std::string::npos) {
                out["download_mode"] = mode;
                out["download_speed"] = effective.speed;
                return true;
            }
        }
        return false;
    };

    // 返回空串 = 成功；否则是失败的阶段名
    const auto attempt = [&]() -> std::string {
        failure_text.clear();
        {
            out["stage"] = "run_hw_cw";
            std::string command = "run_hw_cw $fpgalsp_hw";
            if (effective.trigger_now) command += " -trigger_now";
            command += " -cable " + effective.cable;
            failure_text = session->Run(command, 120.0);
            if (td::HasError(failure_text)) return "run_hw_cw";
        }
        {
            out["stage"] = "wait_on_hw_cw";
            std::string command = "wait_on_hw_cw $fpgalsp_hw -timeout ";
            command += std::to_string(static_cast<int>(effective.wait_timeout));
            command += " -cable " + effective.cable;
            failure_text = session->Run(command, effective.wait_timeout + 60.0);
            if (td::HasError(failure_text)) return "wait_on_hw_cw";
        }
        {
            out["stage"] = "write_hw_cw_data";
            const std::string command =
                "write_hw_cw_data $fpgalsp_hw -csv_file " + td::TclQuoteGrouped(csv);
            failure_text = session->Run(command, 180.0);
            if (td::HasError(failure_text) || !IsFile(csv)) return "write_hw_cw_data";
        }
        return {};
    };

    bool downloaded = false;
    if (always_download) {
        if (!do_download()) {
            return fail("download", "cannot load the probe-enabled bitstream", failure_text);
        }
        downloaded = true;
    }

    std::string failed = attempt();
    if (!failed.empty() && !downloaded && !never_download) {
        // auto：器件里那份 bit 抓不动（多半不是带探针的），下载后重抓一次
        record("loaded bitstream did not answer the capture - downloading the probe-enabled bit");
        if (!do_download()) {
            return fail("download", "cannot load the probe-enabled bitstream", failure_text);
        }
        downloaded = true;
        failed = attempt();
    }
    if (!failed.empty()) {
        const std::string reason =
            failed == "wait_on_hw_cw"
                ? "capture did not complete - the bitstream in the device may not contain these "
                  "probes (retry with download:true)"
            : failed == "write_hw_cw_data" ? "capture data was not written"
                                           : "cannot start the capture";
        return fail(failed, reason, failure_text);
    }
    out["downloaded"] = downloaded;

    session->Kill();

    const CaptureData data = ParseCaptureCsv(ReadText(csv));
    out["csv"] = csv;
    out["samples"] = static_cast<int>(data.rows.size());
    out["stage"] = "done";
    if (!data.ok) {
        out["reason"] = data.error;
        out["log"] = log;
        return out;
    }

    jsonrpc::Json columns = jsonrpc::Json::array();
    for (const CaptureColumn& column : data.columns) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = column.bus;
        item["raw"] = column.name;
        item["width"] = column.width;
        columns.push_back(std::move(item));
    }
    out["columns"] = std::move(columns);
    out["trigger_index"] = data.trigger_index;
    out["trigger_mode"] = effective.trigger_now ? "manual" : "condition";

    // 每条总线给完整采样序列（深度 1024 时 4 条总线约 4k 个数字，MCP 端可接受）
    jsonrpc::Json signals = jsonrpc::Json::object();
    for (std::size_t c = 0; c < data.columns.size(); ++c) {
        jsonrpc::Json values = jsonrpc::Json::array();
        for (const CaptureRow& row : data.rows) {
            values.push_back(row.values.size() > c ? row.values[c] : 0);
        }
        signals[data.columns[c].bus] = std::move(values);
    }
    out["signals"] = std::move(signals);

    // 首/末采样与去重计数，方便一眼判断"是不是常量"
    jsonrpc::Json summary = jsonrpc::Json::object();
    for (std::size_t c = 0; c < data.columns.size(); ++c) {
        std::set<std::uint64_t> distinct;
        for (const CaptureRow& row : data.rows) {
            if (row.values.size() > c) distinct.insert(row.values[c]);
        }
        jsonrpc::Json item = jsonrpc::Json::object();
        item["distinct"] = static_cast<int>(distinct.size());
        item["first"] = data.rows.front().values.size() > c ? data.rows.front().values[c] : 0;
        item["last"] = data.rows.back().values.size() > c ? data.rows.back().values[c] : 0;
        summary[data.columns[c].bus] = std::move(item);
    }
    out["summary"] = std::move(summary);
    out["log"] = log;
    out["ok"] = true;
    out["reason"] = "captured " + std::to_string(data.rows.size()) + " sample(s) from " +
                    instance + " on " + cwc.clock;
    return out;
}

}  // namespace fpgalsp::probe