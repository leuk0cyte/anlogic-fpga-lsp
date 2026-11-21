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

#include "platform/path.h"
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

namespace {

// `WHG/a[3]` -> base=`WHG/a`、bit=3；不是这个形态返回 false。
bool SplitBitIndex(std::string_view name, std::string* base, int* bit) {
    const std::size_t open = name.find_last_of('[');
    if (open == std::string_view::npos || name.empty() || name.back() != ']') return false;
    const std::string index(name.substr(open + 1, name.size() - open - 2));
    if (index.empty()) return false;
    for (const char c : index) {
        if (c < '0' || c > '9') return false;
    }
    *base = std::string(name.substr(0, open));
    *bit = std::atoi(index.c_str());
    return true;
}

// disabled 条目与信号名的匹配：完全相等，或一方是另一方 `/` 边界之后的尾段
//（用户常只写叶子名；cwc 里的名字又不带顶层前缀）。
bool SameBusName(std::string_view a, std::string_view b) {
    if (a == b) return true;
    const auto tail = [](std::string_view longer, std::string_view shorter) {
        return longer.size() > shorter.size() &&
               longer.substr(longer.size() - shorter.size()) == shorter &&
               longer[longer.size() - shorter.size() - 1] == '/';
    };
    return tail(a, b) || tail(b, a);
}

}  // namespace

// 把 disabled 条目（整条总线 / 单根 / 范围）解析成"逐位名字集合"。
// 只在 signals 里存在的位才会被 disable；解析不到的条目进 unresolved（调用方告警）。
DisabledPlan ResolveDisabled(const std::vector<std::string>& signals,
                             const std::vector<std::string>& disabled) {
    DisabledPlan plan;
    if (disabled.empty()) return plan;
    struct SignalBit {
        std::string name;  // signals 里的原样写法（带顶层前缀）
        std::string base;  // 去掉 `[n]`
        int bit = -1;      // 标量 = -1
    };
    std::vector<SignalBit> items;
    for (const std::string& raw : NormaliseSignals(signals)) {
        SignalBit item;
        item.name = raw;
        if (!SplitBitIndex(raw, &item.base, &item.bit)) item.base = raw;
        items.push_back(std::move(item));
    }
    std::set<std::string> seen_entry;
    const auto add_entry = [&plan, &seen_entry](const std::string& entry) {
        if (!seen_entry.insert(entry).second) return;
        plan.entries.push_back(entry);
    };
    for (const std::string& raw : disabled) {
        const std::string trimmed = Trim(raw);
        if (trimmed.empty()) continue;
        bool matched = false;
        for (const std::string& piece : ExpandSignalRanges({trimmed})) {
            std::string base;
            int bit = -1;
            const bool single_bit = SplitBitIndex(piece, &base, &bit);
            if (!single_bit) base = piece;  // 整条总线：没有 `[n]`，base 就是条目本身
            for (const SignalBit& item : items) {
                if (!SameBusName(item.base, base)) continue;
                if (single_bit && item.bit != bit) continue;
                matched = true;
                plan.bits.insert(item.name);
                add_entry(single_bit ? item.name : item.base);
            }
        }
        if (!matched) plan.unresolved.push_back(trimmed);
    }
    return plan;
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
    // disabled：bus 的 `is_enabled="no"` / net 的 `data="disable"`（不占 debug 资源）
    std::set<std::string> disabled_buses;
    std::set<std::string> disabled_nets;
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
        if (!bus.enabled && !bus.name.empty()) disabled_buses.insert(bus.name);
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
                if (Attr(TagBody(text, i), "data") == "disable" && !net_name.empty()) {
                    disabled_nets.insert(net_name);
                }
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
    out.disabled_bus_count = static_cast<int>(disabled_buses.size());
    out.disabled_net_count = static_cast<int>(disabled_nets.size());
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
            // ⚠ 只认目录：`phy_1/cw/compiled.cwc` 名字里也含 "cwc"，以前会被当成
            // watcher 目录，于是 inst 变成 "compiled.cwc"，生成的 cwc 里
            // inst/bram_name/trigger 全带着这个假名字。
            std::error_code dir_ec;
            if (!it->path().empty() && !std::filesystem::is_directory(it->path(), dir_ec)) continue;
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
        std::filesystem::path(fpgalsp::platform::state_dir(project.dir) + "/probe");
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
    ec.clear();
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
// 采样时钟周期（ns）。TD 的 cwc 里 `<clock clk_name="..." clk_peroid="10ns"/>` 缺了
// `clk_peroid` 时，`config_chipwatcher` 仍会接受（KIT-1004 照打），但算出来的
// `Chipwatcher code` 与设计不一致，随后 `wait_on_hw_cw` 永远等不到触发（会话挂死）。
// 周期只能从历史 cwc 里取（TD 不把它写成单独产物）；取不到时按 10 ns（100 MHz）兜底。
std::string ResolveClockPeriod(const project::Project& project) {
    std::vector<std::string> candidates;
    candidates.push_back(CompiledCwc(project));
    std::error_code ec;
    for (const std::string& sub : {"/cwc"}) {
        const std::filesystem::path dir(project.dir + sub);
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
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
        const std::size_t at = text.find("clk_peroid=\"");
        if (at == std::string::npos) continue;
        const std::size_t begin = at + 12;
        const std::size_t end = text.find('"', begin);
        if (end == std::string::npos || end == begin) continue;
        const std::string value = text.substr(begin, end - begin);
        if (!value.empty()) return value;
    }
    return "10ns";
}

std::string BuildCwcFromWatcher(const project::Project& project, const MdnConfig& mdn,
                                const std::vector<std::string>& bus_order, std::string* error,
                                jsonrpc::Json* info) {
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

    // 1b) 顺序提示：TD 会把总线重排（watcher 产物里没有总线名，只有位宽），所以当位宽
    //     有重复时无法从产物反推"哪个 16 位对哪个 slot"。调用方若知道真实顺序，可用
    //     `cwc_bus_order` 明确指定（名字可带 `WHG/` 前缀，也可只给叶子名）。
    if (!bus_order.empty()) {
        const auto leaf = [](const std::string& name) {
            const std::size_t slash = name.rfind('/');
            return slash == std::string::npos ? name : name.substr(slash + 1);
        };
        std::vector<std::pair<std::string, int>> reordered;
        std::vector<bool> used(buses.size(), false);
        for (const std::string& wanted : bus_order) {
            bool found = false;
            for (std::size_t i = 0; i < buses.size(); ++i) {
                if (used[i]) continue;
                if (buses[i].first == wanted || leaf(buses[i].first) == leaf(wanted)) {
                    reordered.push_back(buses[i]);
                    used[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return fail("cwc_bus_order entry '" + wanted +
                            "' is not a bus in user.mdn (known: " + [&buses]() {
                                std::string names;
                                for (const auto& bus : buses) {
                                    if (!names.empty()) names += ", ";
                                    names += bus.first;
                                }
                                return names;
                            }() + ")");
            }
        }
        if (reordered.size() != buses.size()) {
            return fail("cwc_bus_order must list every bus exactly once (" +
                        std::to_string(buses.size()) + " expected)");
        }
        buses = std::move(reordered);
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

    // disabled：被 disable 的条目不占 debug 资源（用户实测 TD GUI）。但 TD 在
    // `insert_debugger -cwc` 里到底怎么折算 CWC_BUS_* 没有公开文档，所以这里接受
    // 三种自洽的布局（都按**我们的总线顺序**逐位置比对位宽，顺序错依然拒绝）：
    //   full  : TD 忽略 disable，仍按 cwc 的 bus 列表建 watcher（全量计数）
    //   on    : TD 把 disable 的位彻底摘掉 —— 整条 disabled 的 bus 消失，
    //           部分 disabled 的 bus 位宽变成"只剩 enable 的位数"
    //   mixed : 整条 disabled 的 bus 消失，但部分 disabled 的 bus 保留全宽，
    //           DIN 计数落在 enabled_total..total 之间
    // 位序（哪个 16 位 bus 对哪个 slot）才是会毁掉采样数据的东西：三种布局都必须
    // 逐位置位宽相等才接受；放宽的只有"disabled 折算"这一个维度。
    const ProbeBuildConfig config = ReadProbeBuildConfig(project);
    const DisabledPlan disabled = ResolveDisabled(mdn.signals, config.disabled);
    const auto bit_disabled = [&disabled](const std::string& base, int bit) {
        return disabled.IsBitDisabled(base + "[" + std::to_string(bit) + "]");
    };
    std::vector<int> full_projection;     // 全量 bus 位宽
    std::vector<int> enabled_projection;  // 只剩 enable 的 bus，位宽 = enable 位数
    std::vector<int> kept_full;           // 只剩 enable 的 bus，位宽 = 原全宽
    int enabled_total = 0;
    for (const auto& bus : buses) {
        int enabled_bits = 0;
        for (int bit = 0; bit < bus.second; ++bit) {
            if (!bit_disabled(bus.first, bit)) ++enabled_bits;
        }
        full_projection.push_back(bus.second);
        if (enabled_bits > 0) {
            enabled_projection.push_back(enabled_bits);
            kept_full.push_back(bus.second);
        }
        enabled_total += enabled_bits;
    }
    const auto widths_match = [&inst](const std::vector<int>& expected) {
        if (inst.widths.size() != expected.size()) return false;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (inst.widths[i] != expected[i]) return false;
        }
        return true;
    };
    const bool layout_ok =
        !disabled.Any()
            ? (static_cast<int>(buses.size()) == inst.bus_num && total == inst.din_num &&
               widths_match(full_projection))
            : ((static_cast<int>(buses.size()) == inst.bus_num && total == inst.din_num &&
                widths_match(full_projection)) ||
               (static_cast<int>(enabled_projection.size()) == inst.bus_num &&
                inst.din_num >= enabled_total && inst.din_num <= total &&
                (widths_match(enabled_projection) || widths_match(kept_full))));
    if (!layout_ok) {
        // 位宽集合相同但顺序不同 = bit 里的 watcher 是从**另一份** probe 清单建出来的
        //（典型：改完 mdn 只重跑过 phy，或 mdn 在 flow 之后又被写了一次）。这种情况
        // 只能重跑 syn 让它重建，不能"猜"哪个 16 位总线对哪个 slot —— 猜错就是错的采样数据。
        std::vector<int> wanted = full_projection;
        std::vector<int> got = inst.widths;
        std::sort(wanted.begin(), wanted.end());
        std::sort(got.begin(), got.end());
        if (wanted == got) {
            return fail(
                "the watcher in the bitstream was built from a DIFFERENT probe list "
                "(same widths, different order): rerun stages:[\"all\"] so user.mdn and "
                "the ChipWatcher agree - do not guess the bus order, wrong order means "
                "wrong capture data");
        }
        return fail("mdn/watcher mismatch: mdn has " + std::to_string(buses.size()) + " buses / " +
                    std::to_string(total) + " bits (" +
                    std::to_string(enabled_projection.size()) + " buses / " +
                    std::to_string(enabled_total) + " bits once disabled entries are excluded), "
                    "watcher has " + std::to_string(inst.bus_num) + " buses / " +
                    std::to_string(inst.din_num) +
                    " bits - rerun stages:[\"all\"] so the watcher matches user.mdn");
    }

    // 3) 采集 RAM 的 ERAM 映射（rid/wid/offset）
    std::string cwc1_name = dir_path.filename().string();  // auto_created_cwc1
    const std::string bid_path = project.dir + "/" + project.name + "_Runs/phy_1/" +
                                 project.name + "_inst.bid";
    if (!IsFile(bid_path)) return fail("no " + bid_path + " - run the phy stage first");
    const std::vector<BidEram> erams = ParseBidErams(ReadText(bid_path), cwc1_name);
    if (erams.empty()) return fail("no ERAM entries for " + cwc1_name + " in " + bid_path);

    // 4) 采样时钟：优先用落盘配置（由 `report_cwc_status` 从工程 gate 数据库问 TD 得到），
    //    然后才退回扫描现有 cwc（phy sidecar、工程 cwc/ 目录）。不再读旧 .fpgalsp。
    const ProbeBuildConfig clock_config = ReadProbeBuildConfig(project);
    const std::string clock =
        clock_config.has_clock && !clock_config.clock.empty() ? clock_config.clock
                                                             : ResolveSampleClock(project);
    if (clock.empty()) {
        return fail("cannot determine the ChipWatcher sample clock: no compiled cwc and no "
                    "cwc/*.cwc in the project. Pass clock to fpga_probe_config "
                    "(e.g. {\"clock\":\"reg_init/clk\"}) - TD needs it when it authors the "
                    "watcher config");
    }

    // 5) 拼 XML（格式与 TD 自己写出的 compiled.cwc 逐字段一致）
    //    ⚠ 总线/网名是**相对顶层模块**的（TD 自己的 cwc 里写的是 `pwm_reg0_i[15:0]`、
    //      `pwm_reg0_i[15]`，没有 `WHG/` 前缀）。带前缀会让"mdn ↔ bit"逐位比对判成
    //      stale（missing e2e_cnt2l / extra WHG/e2e_cnt2l），抓取因此被拒。
    const std::string top_prefix = project.top.empty() ? std::string() : project.top + "/";
    const auto rel_name = [&top_prefix](const std::string& name) {
        if (!top_prefix.empty() && name.rfind(top_prefix, 0) == 0) {
            return name.substr(top_prefix.size());
        }
        return name;
    };
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out += "<chipwatcher version=\"3.070000\" project_name=\"" + project.name + "\" chip_name=\"" +
           project.device + "\">\n";
    out += "\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        int enabled_bits = 0;
        for (int bit = 0; bit < bus.second; ++bit) {
            if (!bit_disabled(bus.first, bit)) ++enabled_bits;
        }
        // 整条 disable 的总线：TD 自己写的 cwc 里是 is_enabled="no" **且 inst=""**，
        // 并且**不**出现在 <signal_vec> 里。照抄这个语义，回读时 TD 才会把它当作
        // "不参与 watcher"（实测：GUI 就是这么表达 disable 的）。
        const bool bus_off = enabled_bits == 0;
        out += "\t\t<bus name=\"" + rel_name(bus.first) + "[" + std::to_string(bus.second - 1) + ":0]\" clock=\"" +
               clock + "\" inst=\"" + (bus_off ? std::string() : cwc1_name) +
               "\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"" + (bus_off ? "no" : "yes") + "\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            const std::string state = bit_disabled(bus.first, bit) ? "disable" : "enable";
            out += "\t\t\t<net name=\"" + rel_name(bus.first) + "[" + std::to_string(bit) +
                   "]\" const=\"false\" data=\"" + state + "\" trig=\"" + state + "\"/>\n";
        }
        out += "\t\t</bus>\n";
    }
    out += "\t</watcher_nodes>\n";
    out += "\t<instance name=\"" + cwc1_name + "\" id=\"0\" ipgen=\"false\" inst_enable=\"true\">\n";
    const std::string clock_period = ResolveClockPeriod(project);
    out += "\t\t<clock clk_name=\"" + clock + "\" clk_peroid=\"" + clock_period + "\"/>\n";
    out += "\t\t<config bram_name=\"" + cwc1_name + "/wrapper_cwc_top/U_cwc_ram\" debughub_name=\"\" " +
           "input_pipe_num=\"" + std::to_string(inst.pipe_in) + "\" output_pipe_num=\"" +
           std::to_string(inst.pipe_out) + "\" sample_depth=\"" + std::to_string(inst.depth) +
           "\"/>\n";
    out += "\t\t<signal_vec>\n\t\t\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        // 整条 disable 的总线不进 signal_vec（TD 的写法）；否则 TD 会把它算进触发网表。
        int signal_vec_bits = 0;
        for (int bit = 0; bit < bus.second; ++bit) {
            if (!bit_disabled(bus.first, bit)) ++signal_vec_bits;
        }
        if (signal_vec_bits == 0) continue;
        out += "\t\t\t\t<bus name=\"" + rel_name(bus.first) + "[" + std::to_string(bus.second - 1) +
               ":0]\" compatator_num=\"1\" op=\"dont_care\" capture_op=\"dont_care\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            // disabled 的位不参与触发/采集比较（与 GUI 写出的 cwc 一致）。
            const std::string cond = bit_disabled(bus.first, bit) ? "dont_care" : "low";
            out += "\t\t\t\t\t<net name=\"" + rel_name(bus.first) + "[" + std::to_string(bit) +
                   "]\" cond=\"" + cond + "\" capture_cond=\"" + cond + "\"/>\n";
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
               (eram.width_a.empty() ? eram.wm_width : eram.width_a) + "\" working_mode_ecc=\"" +
               (eram.wm_ecc == "0" ? "disable" : "enable") + "\" working_mode_num_byte=\"" +
               eram.wm_num_byte + "\" working_mode_super_wide=\"disable\"/>\n";
    }
    out += "\t\t</logic_eram>\n";
    out += "\t</instance>\n</chipwatcher>\n";

    if (info != nullptr) {
        (*info)["ok"] = true;
        (*info)["source_dir"] = dir;
        (*info)["watcher_sv"] = sv_name;
        (*info)["clock"] = clock;
        (*info)["depth"] = inst.depth;
        (*info)["buses"] = static_cast<int>(buses.size());
        (*info)["bits"] = total;
        (*info)["enabled_bits"] = enabled_total;
        (*info)["disabled_bits"] = static_cast<int>(disabled.bits.size());
        (*info)["enabled_widths"] = enabled_projection;
        (*info)["eram"] = static_cast<int>(erams.size());
    }
    return out;
}

// =============================================================================
// 探针构建参数 + 按参数生成 cwc
// =============================================================================
namespace {

std::string StateProbeDir(const project::Project& project) {
    return fpgalsp::platform::state_dir(project.dir) + "/probe";
}

// config.txt 里的列表项用逗号分隔（RTL 层次名/位号里不会有逗号）
std::vector<std::string> SplitCsv(std::string_view value) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string one = Trim(
            value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (!one.empty()) out.push_back(one);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return out;
}

std::string JoinCsv(const std::vector<std::string>& items) {
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) joined += ",";
        joined += item;
    }
    return joined;
}

// 极简 key=value 落盘（避免为一个 3 字段配置引入 JSON 依赖）
}  // namespace

std::string ProbeConfigPath(const project::Project& project) {
    return StateProbeDir(project) + "/config.txt";
}

ProbeBuildConfig ReadProbeBuildConfig(const project::Project& project) {
    ProbeBuildConfig config;
    const std::string path = ProbeConfigPath(project);
    if (!IsFile(path)) return config;
    const std::string text = ReadText(path);
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "clock" && !value.empty()) {
            config.has_clock = true;
            config.clock = value;
        } else if (key == "depth") {
            const int depth = std::atoi(value.c_str());
            if (depth > 0) {
                config.has_depth = true;
                config.depth = depth;
            }
        } else if (key == "trigger_position") {
            const int pos = std::atoi(value.c_str());
            if (pos >= 0) config.trigger_position = pos;
        } else if (key == "apply") {
            config.apply_build_config = value != "0";
        } else if (key == "write_sidecar") {
            config.write_sidecar_cwc = value != "0";
        } else if (key == "bus_order" && !value.empty()) {
            config.cwc_bus_order = SplitCsv(value);
        } else if (key == "disabled" && !value.empty()) {
            // 与 bus_order 同样的逗号分隔；条目里不会含逗号（RTL 层次名/位号）
            config.disabled = SplitCsv(value);
        }
    }
    return config;
}

bool WriteProbeBuildConfig(const project::Project& project, const ProbeBuildConfig& config) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(StateProbeDir(project)), ec);
    std::string text;
    if (config.has_clock) text += "clock=" + config.clock + "\n";
    if (config.has_depth) text += "depth=" + std::to_string(config.depth) + "\n";
    text += "trigger_position=" + std::to_string(config.trigger_position) + "\n";
    if (config.apply_build_config) text += "apply=1\n";
    if (config.write_sidecar_cwc) text += "write_sidecar=1\n";
    if (!config.cwc_bus_order.empty()) {
        std::string joined;
        for (const std::string& name : config.cwc_bus_order) {
            if (!joined.empty()) joined += ",";
            joined += name;
        }
        text += "bus_order=" + joined + "\n";
    }
    // 空 vector = 删除这一行（WriteProbeBuildConfig 总是整份重写，天然清掉旧的 disable）
    if (!config.disabled.empty()) text += "disabled=" + JoinCsv(config.disabled) + "\n";
    return fpgalsp::platform::write_file_utf8(ProbeConfigPath(project), text);
}

std::string BuildCwcFromConfig(const project::Project& project, const MdnConfig& mdn,
                               const ProbeBuildConfig& config, std::string* error,
                               jsonrpc::Json* info, bool include_eram) {
    const auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return std::string();
    };
    if (!mdn.exists || mdn.signals.empty()) return fail("no probes marked in user.mdn");

    std::vector<std::pair<std::string, int>> buses;
    for (const std::string& signal : mdn.signals) {
        std::string base = signal;
        const std::size_t open = signal.rfind('[');
        if (open != std::string::npos) base = signal.substr(0, open);
        if (buses.empty() || buses.back().first != base) buses.emplace_back(base, 0);
        ++buses.back().second;
    }
    int total = 0;
    for (const auto& bus : buses) total += bus.second;

    // disabled：不占 debug 资源，但**仍**写进 XML（GUI/回读都要看到它们）。
    // 位序/位宽/DIN 与 <logic_eram width> 只算 enable 的位（见 DisabledPlan 的注释）。
    const DisabledPlan disabled = ResolveDisabled(mdn.signals, config.disabled);
    const auto bit_disabled = [&disabled](const std::string& base, int bit) {
        return disabled.IsBitDisabled(base + "[" + std::to_string(bit) + "]");
    };
    int enabled_total = 0;
    for (const auto& bus : buses) {
        for (int bit = 0; bit < bus.second; ++bit) {
            if (!bit_disabled(bus.first, bit)) ++enabled_total;
        }
    }

    const std::string clock =
        config.has_clock ? config.clock : ResolveSampleClock(project);
    if (clock.empty()) {
        return fail("cannot determine the sample clock - pass clock (e.g. reg_init/clk) to "
                    "fpga_probe_config, or save the ChipWatcher once from the TD GUI");
    }
    const int depth = config.has_depth ? config.depth : 1024;

    // ERAM 行：优先用当前位流的 `_inst.bid`；没有就留空（TD 编译时会自己补）
    std::vector<BidEram> erams;
    const std::string bid_path =
        project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + "_inst.bid";
    if (IsFile(bid_path)) {
        std::string inst_name = "auto_created_cwc1";
        if (const std::string dir = NewestWatcherDir(project); !dir.empty()) {
            inst_name = std::filesystem::path(dir).filename().string();
        }
        erams = ParseBidErams(ReadText(bid_path), inst_name);
    }

    const std::string top_prefix = project.top.empty() ? std::string() : project.top + "/";
    const auto rel = [&top_prefix](const std::string& name) {
        if (!top_prefix.empty() && name.rfind(top_prefix, 0) == 0) {
            return name.substr(top_prefix.size());
        }
        return name;
    };
    std::string inst = "auto_created_cwc1";
    if (erams.empty()) {
        if (const std::string dir = NewestWatcherDir(project); !dir.empty()) {
            inst = std::filesystem::path(dir).filename().string();
        }
    } else {
        // ERAM 名字里带实例前缀，取第一段
        const std::size_t slash = erams.front().name.find('/');
        if (slash != std::string::npos) inst = erams.front().name.substr(0, slash);
    }

    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out += "<chipwatcher version=\"3.070000\" project_name=\"" + project.name + "\" chip_name=\"" +
           project.device + "\">\n\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        int enabled_bits = 0;
        for (int bit = 0; bit < bus.second; ++bit) {
            if (!bit_disabled(bus.first, bit)) ++enabled_bits;
        }
        out += "\t\t<bus name=\"" + rel(bus.first) + "[" + std::to_string(bus.second - 1) +
               ":0]\" clock=\"" + clock + "\" inst=\"" + inst +
               "\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"" +
               (enabled_bits == 0 ? "no" : "yes") + "\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            const std::string state = bit_disabled(bus.first, bit) ? "disable" : "enable";
            out += "\t\t\t<net name=\"" + rel(bus.first) + "[" + std::to_string(bit) +
                   "]\" const=\"false\" data=\"" + state + "\" trig=\"" + state + "\"/>\n";
        }
        out += "\t\t</bus>\n";
    }
    out += "\t</watcher_nodes>\n\t<instance name=\"" + inst +
           "\" id=\"0\" ipgen=\"false\" inst_enable=\"true\">\n";
    out += "\t\t<clock clk_name=\"" + clock + "\" clk_peroid=\"10ns\"/>\n";
    out += "\t\t<config bram_name=\"" + inst + "/wrapper_cwc_top/U_cwc_ram\" debughub_name=\"\" " +
           "input_pipe_num=\"0\" output_pipe_num=\"0\" sample_depth=\"" + std::to_string(depth) +
           "\"/>\n";
    out += "\t\t<signal_vec>\n\t\t\t<watcher_nodes>\n";
    for (const auto& bus : buses) {
        out += "\t\t\t\t<bus name=\"" + rel(bus.first) + "[" + std::to_string(bus.second - 1) +
               ":0]\" compatator_num=\"1\" op=\"dont_care\" capture_op=\"dont_care\">\n";
        for (int bit = bus.second - 1; bit >= 0; --bit) {
            // disabled 的位在触发/采集条件里必须是 dont_care，否则 TD 会认为
            // 它仍要参与比较（GUI 里 disable 的条目就是这个写法）。
            const std::string cond = bit_disabled(bus.first, bit) ? "dont_care" : "low";
            out += "\t\t\t\t\t<net name=\"" + rel(bus.first) + "[" + std::to_string(bit) +
                   "]\" cond=\"" + cond + "\" capture_cond=\"" + cond + "\"/>\n";
        }
        out += "\t\t\t\t</bus>\n";
    }
    out += "\t\t\t</watcher_nodes>\n\t\t</signal_vec>\n";
    out += "\t\t<trigger name=\"" + inst + "_trigger\" position=\"" +
           std::to_string(config.trigger_position) + "\" window_num=\"1\" window_depth=\"" +
           std::to_string(depth) + "\">\n";
    out += "\t\t\t<condition level=\"1\" enabled=\"yes\" type=\"basic_and\" " +
           std::string("capture_type=\"basic_and\"/>\n");
    out += "\t\t</trigger>\n";
    if (include_eram) {
    out += "\t\t<logic_eram name=\"" + inst + "/wrapper_cwc_top/U_cwc_ram\" width=\"" +
           std::to_string(enabled_total) + "\" depth=\"" + std::to_string(depth) + "\">\n";
    for (const BidEram& eram : erams) {
        out += "\t\t\t<phy_eram name=\"" + eram.name + "\" wid=\"" + eram.wid + "\" rid=\"" +
               eram.rid + "\" model_type=\"" + eram.model_type + "\" width_a=\"" + eram.width_a +
               "\" width_b=\"" + eram.width_b + "\" debuggable=\"" +
               (EnableWord(eram.debuggable) ? "enable" : "disable") + "\" initialized=\"" +
               (EnableWord(eram.initialized) ? "enable" : "disable") + "\" width=\"" + eram.width +
               "\" depth=\"" + std::to_string(depth) + "\" address_offset=\"" +
               eram.address_offset + "\" data_offset=\"" + eram.data_offset +
               "\" address_step=\"" + eram.address_step + "\" num_section=\"" + eram.num_section +
               "\" section_size=\"" + eram.section_size + "\" width_per_section=\"" +
               eram.width_per_section + "\" bytes_in_per_section=\"" +
               eram.bytes_in_per_section + "\" working_mode_depth=\"" + std::to_string(depth) +
               "\" working_mode_width=\"" + (eram.width_a.empty() ? eram.wm_width : eram.width_a) +
               "\" working_mode_ecc=\"" +
               (eram.wm_ecc == "0" ? "disable" : "enable") + "\" working_mode_num_byte=\"" +
               eram.wm_num_byte + "\" working_mode_super_wide=\"disable\"/>\n";
    }
    out += "\t\t</logic_eram>\n";
    }
    out += "\t</instance>\n</chipwatcher>\n";
    if (info != nullptr) {
        (*info)["ok"] = true;
        (*info)["clock"] = clock;
        (*info)["depth"] = depth;
        (*info)["buses"] = static_cast<int>(buses.size());
        (*info)["bits"] = total;
        (*info)["enabled_bits"] = enabled_total;
        (*info)["disabled_bits"] = static_cast<int>(disabled.bits.size());
        (*info)["eram"] = static_cast<int>(erams.size());
    }
    return out;
}

// =============================================================================
// 以 TD 验证过的 cwc 为模板，只打 enable/disable 补丁
// =============================================================================
//  为什么：从零生成的 cwc 交给 `insert_debugger -cwc` 会让 phy 的 opt_place 崩
//  （PRJ-8806，TD 进程直接退出，日志里没有 PHY 错误）；而"TD 自己写的" cwc 原样
//  传进去 place 正常，把其中一条总线改成 is_enabled="no" / data="disable" 之后
//  place 仍然正常、watcher 真的从 4 总线/53 bit 变成 3 总线/48 bit（实测 2026-11-21）。
//  所以 disable 的正确实现是：拿现有 sidecar 当模板，只改 enable 属性。
namespace {

// 名字的"尾段"比较：`WHG/e2e_hstat` 与 `e2e_hstat` 视为同一条总线。
std::string_view TailName(std::string_view name) {
    const std::size_t slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

// 把 `attr="old"` 的值替换成 value；没有这个属性就补在行尾的 `>` 之前。
void ReplaceAttr(std::string* line, std::string_view attr, std::string_view value) {
    const std::string key = std::string(attr) + "=\"";
    const std::size_t at = line->find(key);
    if (at == std::string::npos) return;
    const std::size_t begin = at + key.size();
    const std::size_t close = line->find('"', begin);
    if (close == std::string::npos) return;
    line->replace(begin, close - begin, std::string(value));
}

// 取第一个 <bus name="..." 里的名称（不区分大小写无所谓，TD 用小写）。
std::vector<std::string> TemplateBusNames(const std::string& text) {
    std::vector<std::string> names;
    const std::string key = "<bus name=\"";
    std::size_t pos = 0;
    while (true) {
        const std::size_t at = text.find(key, pos);
        if (at == std::string::npos) break;
        const std::size_t begin = at + key.size();
        const std::size_t close = text.find('"', begin);
        if (close == std::string::npos) break;
        std::string name = text.substr(begin, close - begin);
        const std::size_t bracket = name.find('[');
        if (bracket != std::string::npos) name.erase(bracket);
        bool seen = false;
        for (const std::string& item : names) seen = seen || item == name;
        if (!seen) names.push_back(name);
        pos = close + 1;
    }
    return names;
}

}  // namespace

std::string BuildCwcFromTemplate(const project::Project& project, const MdnConfig& mdn,
                                 const ProbeBuildConfig& config, std::string* error,
                                 jsonrpc::Json* info) {
    const auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return std::string();
    };
    const std::string path = CompiledCwc(project);
    if (!IsFile(path)) return fail("no template cwc at " + path);
    const std::string text = ReadText(path);
    if (text.empty()) return fail("template cwc is empty");

    // mdn 的总线（按出现顺序 + 位宽）
    std::vector<std::pair<std::string, int>> buses;
    for (const std::string& signal : mdn.signals) {
        std::string base = signal;
        const std::size_t open = signal.rfind('[');
        if (open != std::string::npos) base = signal.substr(0, open);
        if (buses.empty() || buses.back().first != base) buses.emplace_back(base, 0);
        ++buses.back().second;
    }
    // 模板里的总线（watcher_nodes 段）
    const std::vector<std::string> template_buses = TemplateBusNames(text);
    if (template_buses.size() != buses.size()) {
        return fail("template cwc has " + std::to_string(template_buses.size()) +
                    " buses, mdn has " + std::to_string(buses.size()) +
                    " - cannot patch, rebuild instead");
    }
    // 模板的总线顺序就是 TD 当初编译时用的顺序（常常与 user.mdn 的顺序不同），
    // 所以按"尾名"做一次映射，而不是要求两边顺序一致。
    std::vector<std::size_t> template_to_mdn(template_buses.size(), buses.size());
    for (std::size_t t = 0; t < template_buses.size(); ++t) {
        for (std::size_t m = 0; m < buses.size(); ++m) {
            if (TailName(template_buses[t]) == TailName(buses[m].first)) {
                template_to_mdn[t] = m;
                break;
            }
        }
        if (template_to_mdn[t] == buses.size()) {
            return fail("template cwc bus '" + template_buses[t] + "' is not in user.mdn");
        }
    }

    const DisabledPlan plan = ResolveDisabled(mdn.signals, config.disabled);
    // 每条总线是否"整条 disable"
    std::vector<bool> bus_off(buses.size(), false);
    for (std::size_t i = 0; i < buses.size(); ++i) {
        bool all = true;
        for (int bit = 0; bit < buses[i].second; ++bit) {
            const std::string one = buses[i].first + "[" + std::to_string(bit) + "]";
            if (!plan.IsBitDisabled(one)) {
                all = false;
                break;
            }
        }
        bus_off[i] = all && buses[i].second > 0;
    }

    // 逐行打补丁：<bus ... is_enabled> 与 <net ... data/trig>
    std::string out;
    out.reserve(text.size() + 256);
    std::size_t pos = 0;
    int current_bus = -1;
    std::size_t current_mdn = buses.size();
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;

        const std::size_t bus_at = line.find("<bus name=\"");
        const std::size_t net_at = line.find("<net name=\"");
        if (bus_at != std::string::npos) {
            const std::size_t begin = bus_at + 11;
            const std::size_t quote = line.find('"', begin);
            const std::string name = line.substr(begin, quote - begin);
            const std::string base = name.substr(0, name.find('['));
            current_bus = -1;
            for (std::size_t i = 0; i < template_buses.size(); ++i) {
                if (TailName(template_buses[i]) == TailName(base)) current_bus = static_cast<int>(i);
            }
            current_mdn =
                current_bus >= 0 ? template_to_mdn[static_cast<std::size_t>(current_bus)] : buses.size();
            const bool off = current_mdn < buses.size() && bus_off[current_mdn];
            ReplaceAttr(&line, "is_enabled", off ? "no" : "yes");
        } else if (net_at != std::string::npos) {
            const std::size_t begin = net_at + 11;
            const std::size_t quote = line.find('"', begin);
            const std::string name = line.substr(begin, quote - begin);
            std::string full = name;
            if (current_mdn < buses.size()) {
                full = buses[current_mdn].first + name.substr(name.find('['));
            }
            const bool off = plan.IsBitDisabled(full) || plan.IsBitDisabled(name);
            // 只改 data/trig —— 实测把 signal_vec 的 cond/capture_cond 也改成
            // dont_care 会让 TD 在 opt_gate 直接崩；手工只改 data/trig 的那份
            // place 完全正常（4 总线/53 bit -> 3 总线/48 bit）。
            if (line.find("data=\"") != std::string::npos) {
                ReplaceAttr(&line, "data", off ? "disable" : "enable");
                ReplaceAttr(&line, "trig", off ? "disable" : "enable");
            }
        }
        out += line;
        out += '\n';
    }

    if (info != nullptr) {
        int disabled_bits = 0;
        for (std::size_t i = 0; i < buses.size(); ++i) {
            if (bus_off[i]) disabled_bits += buses[i].second;
        }
        (*info)["ok"] = true;
        (*info)["template"] = path;
        (*info)["buses"] = static_cast<int>(buses.size());
        (*info)["disabled_buses"] = static_cast<int>(
            std::count(bus_off.begin(), bus_off.end(), true));
        (*info)["disabled_bits"] = disabled_bits;
    }
    return out;
}

// =============================================================================
// 用 gate 阶段的 cwc + 新位流的 ERAM 行写 sidecar
// =============================================================================
//  为什么必须这样：disabled 位流的采集 RAM 宽度是 gate 那次编译时定下的（48 bit），
//  但 cwc 的 <logic_eram> 行还停留在上一轮的几何（section 54）。直接拿 gate cwc 抓取
//  会报 KIT-8445 "(54/48)"；而重新生成整份 cwc 又会改变 Chipwatcher code（code 被写进
//  位流），导致 wait_on_hw_cw 永远等不到触发。
//  实测（2026-11-21 20:0x）：**保留 gate cwc 的总线/触发/时钟原样，只把 <logic_eram>
//  换成新 bid 的行** -> 触发完成、读到 1024 点；width 写 48/49/50 都行。
std::string EramBlockXml(const std::vector<BidEram>& erams, const std::string& inst,
                         int width, int depth) {
    std::string out;
    out += "\t\t<logic_eram name=\"" + inst + "/wrapper_cwc_top/U_cwc_ram\" width=\"" +
           std::to_string(width) + "\" depth=\"" + std::to_string(depth) + "\">\n";
    for (const BidEram& eram : erams) {
        out += "\t\t\t<phy_eram name=\"" + eram.name + "\" wid=\"" + eram.wid + "\" rid=\"" +
               eram.rid + "\" model_type=\"" + eram.model_type + "\" width_a=\"" + eram.width_a +
               "\" width_b=\"" + eram.width_b + "\" debuggable=\"" +
               (EnableWord(eram.debuggable) ? "enable" : "disable") + "\" initialized=\"" +
               (EnableWord(eram.initialized) ? "enable" : "disable") + "\" width=\"" + eram.width +
               "\" depth=\"" + eram.depth + "\" address_offset=\"" + eram.address_offset +
               "\" data_offset=\"" + eram.data_offset + "\" address_step=\"" + eram.address_step +
               "\" num_section=\"" + eram.num_section + "\" section_size=\"" + eram.section_size +
               "\" width_per_section=\"" + eram.width_per_section +
               "\" bytes_in_per_section=\"" + eram.bytes_in_per_section +
               "\" working_mode_depth=\"" + eram.wm_depth + "\" working_mode_width=\"" +
               (eram.width_a.empty() ? eram.wm_width : eram.width_a) + "\" working_mode_ecc=\"" +
               (eram.wm_ecc == "0" ? "disable" : "enable") + "\" working_mode_num_byte=\"" +
               eram.wm_num_byte + "\" working_mode_super_wide=\"disable\"/>\n";
    }
    out += "\t\t</logic_eram>";
    return out;
}

jsonrpc::Json WriteCompiledCwcFromGate(const project::Project& project,
                                       const std::string& gate_cwc_path) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    if (gate_cwc_path.empty() || !IsFile(gate_cwc_path)) {
        out["reason"] = "no gate cwc at " + gate_cwc_path;
        return out;
    }
    std::string text = ReadText(gate_cwc_path);
    if (text.empty()) {
        out["reason"] = "gate cwc is empty";
        return out;
    }
    // 新位流的 ERAM 行
    const std::string bid_path =
        project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + "_inst.bid";
    if (!IsFile(bid_path)) {
        out["reason"] = "no " + bid_path + " - run the phy stage first";
        return out;
    }
    std::string inst = "auto_created_cwc1";
    if (const std::string dir = NewestWatcherDir(project); !dir.empty()) {
        inst = std::filesystem::path(dir).filename().string();
    }
    const std::vector<BidEram> erams = ParseBidErams(ReadText(bid_path), inst);
    if (erams.empty()) {
        out["reason"] = "no ERAM rows for " + inst + " in " + bid_path;
        return out;
    }
    int depth = 1024;
    for (const BidEram& eram : erams) {
        if (!eram.depth.empty()) depth = std::atoi(eram.depth.c_str());
    }
    // disable 之后采集 RAM 只按 **data 使能**的位分配（TD 自己写的 cwc：2 条总线 ->
    // logic_eram width="32"，而 phy bid 的各 mem 宽度和是 33）。所以 width 用
    // "mdn 总位数 - disabled 位数"，不能用 bid 宽度和。
    const MdnConfig mdn_now = ReadMdn(MdnPath(project));
    const ProbeBuildConfig cfg_now = ReadProbeBuildConfig(project);
    const DisabledPlan plan_now = ResolveDisabled(mdn_now.signals, cfg_now.disabled);
    const int enabled_bits = static_cast<int>(mdn_now.signals.size()) -
                             static_cast<int>(plan_now.bits.size());
    const std::string block = EramBlockXml(erams, inst, enabled_bits, depth);
    const std::size_t begin = text.find("\t\t<logic_eram");
    const std::size_t end = text.find("</logic_eram>", begin);
    if (begin != std::string::npos && end != std::string::npos) {
        text = text.substr(0, begin) + block + text.substr(end + std::string("</logic_eram>").size());
    } else {
        // 我们自己写的 user.cfg 故意不带 ERAM（建 watcher 时 TD 也不需要），
        // 这里补在 </instance> 之前即可。
        const std::size_t close = text.find("</instance>");
        if (close == std::string::npos) {
            out["reason"] = "template cwc has neither <logic_eram> nor </instance>";
            return out;
        }
        text = text.substr(0, close) + block + "\n\t" + text.substr(close);
    }

    // 整条 disable 的总线：TD 的写法是 bus `is_enabled="no"` + `inst=""`，且不进
    // `<signal_vec>`。门控用的 cwc 里这些总线还是 "参与" 的形态，必须在这里改成
    // TD 回读时期望的样子（实测 GUI 生成的 cwc 就是这样，按它抓取成功）。
    {
        std::set<std::string> off_buses;
        for (const auto& bus : mdn_now.signals) {
            (void)bus;
        }
        std::map<std::string, int> totals;
        std::map<std::string, int> offs;
        for (const std::string& signal : mdn_now.signals) {
            std::string base = signal;
            const std::size_t open = signal.rfind('[');
            if (open != std::string::npos) base = signal.substr(0, open);
            ++totals[base];
            if (plan_now.IsBitDisabled(signal)) ++offs[base];
        }
        for (const auto& [base, total] : totals) {
            if (offs[base] >= total) off_buses.insert(base);
        }
        if (!off_buses.empty()) {
            const auto is_off = [&off_buses](const std::string& name) {
                std::string base = name;
                const std::size_t open = name.rfind('[');
                if (open != std::string::npos) base = name.substr(0, open);
                for (const std::string& off : off_buses) {
                    if (off == base || TailName(off) == TailName(base)) return true;
                }
                return false;
            };
            std::string out;
            std::size_t pos = 0;
            bool in_signal_vec = false;
            bool skipping = false;
            while (pos < text.size()) {
                const std::size_t next = text.find('\n', pos);
                std::string line = text.substr(pos, next == std::string::npos ? std::string::npos
                                                                              : next - pos);
                pos = next == std::string::npos ? text.size() : next + 1;
                if (line.find("<signal_vec>") != std::string::npos) in_signal_vec = true;
                if (line.find("</signal_vec>") != std::string::npos) in_signal_vec = false;
                const std::size_t bus_at = line.find("<bus name=\"");
                if (bus_at != std::string::npos) {
                    const std::size_t begin_q = bus_at + 11;
                    const std::size_t close_q = line.find('"', begin_q);
                    const std::string name = line.substr(begin_q, close_q - begin_q);
                    const bool off = is_off(name);
                    if (in_signal_vec) {
                        skipping = off;
                        if (off) continue;
                    } else if (off) {
                        ReplaceAttr(&line, "inst", "");
                        ReplaceAttr(&line, "is_enabled", "no");
                    }
                }
                if (skipping) {
                    if (line.find("</bus>") != std::string::npos) skipping = false;
                    continue;
                }
                out += line;
                out += '\n';
            }
            text = std::move(out);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(CompiledCwc(project)).parent_path(), ec);
    if (!fpgalsp::platform::write_file_utf8(CompiledCwc(project), text)) {
        out["reason"] = "cannot write " + CompiledCwc(project);
        return out;
    }
    // bit 侧 sidecar 同步（disabled 位流同样需要）
    if (!IsFile(CompiledBit(project))) {
        const std::string run_bit =
            project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + ".bit";
        if (IsFile(run_bit)) {
            std::error_code copy_ec;
            std::filesystem::copy_file(run_bit, CompiledBit(project),
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            out["bit_copied"] = !copy_ec;
        }
    }
    const CwcSnapshot written = ReadCwc(CompiledCwc(project));
    out["ok"] = written.ok;
    out["written"] = true;
    out["source"] = gate_cwc_path;
    out["net_count"] = written.net_count;
    out["disabled_net_count"] = written.disabled_net_count;
    out["eram_rows"] = static_cast<int>(erams.size());
    out["eram_width"] = enabled_bits;
    if (!written.ok) out["reason"] = "generated cwc does not parse back";
    return out;
}

// 按 mdn + disabled 计划，用**现有的完整配置**当模板生成 `chip_debugger/user.cfg`。
// 为什么要模板：TD 只有拿到"带 <logic_eram> 的完整 watcher 配置"才在 **phy** 阶段插入
// watcher 并自己写出 phy_1/cw/compiled.cwc（实测 0.20.3：user.cfg 完整 -> TD 写 cwc ->
// probe_capture ok/1024 点）。我们自己从零生成的 user.cfg（无 ERAM）会让 TD 在 syn 阶段
// 用一个不同几何的 watcher，抓取时 wait_on_hw_cw 挂死。
bool WriteUserCfgFromMdn(const project::Project& project, std::string* error) {
    const MdnConfig mdn = ReadMdn(MdnPath(project));
    if (!mdn.exists || mdn.signals.empty()) {
        if (error != nullptr) *error = "no probes marked in user.mdn";
        return false;
    }
    const ProbeBuildConfig config = ReadProbeBuildConfig(project);
    // 模板优先级：现有 compiled.cwc（上一次 TD 自己写的）> user.cfg > 从零生成
    std::vector<std::string> candidates{CompiledCwc(project),
                                        project.dir + "/chip_debugger/user.cfg"};
    std::string text;
    for (const std::string& path : candidates) {
        if (!IsFile(path)) continue;
        const CwcSnapshot snapshot = ReadCwc(path);
        if (!snapshot.ok) continue;
        text = ReadText(path);
        if (!text.empty()) break;
    }
    // 模板的总线集合必须与 mdn 一致，否则不能"打补丁"（会留下 mdn 里没有的总线，
    // 让 mdn↔cwc 逐位比对判 stale，抓取被拒）。集合不一致就从零生成（ERAM 取当前 bid）。
    if (!text.empty()) {
        std::vector<std::string> template_buses = TemplateBusNames(text);
        std::vector<std::string> mdn_buses;
        for (const std::string& signal : mdn.signals) {
            std::string base = signal;
            const std::size_t open = signal.rfind('[');
            if (open != std::string::npos) base = signal.substr(0, open);
            bool seen = false;
            for (const std::string& item : mdn_buses) seen = seen || TailName(item) == TailName(base);
            if (!seen) mdn_buses.push_back(base);
        }
        bool same = template_buses.size() == mdn_buses.size();
        if (same) {
            for (const std::string& base : mdn_buses) {
                bool found = false;
                for (const std::string& other : template_buses) {
                    found = found || TailName(other) == TailName(base);
                }
                if (!found) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) text.clear();
    }
    if (text.empty()) {
        jsonrpc::Json info = jsonrpc::Json::object();
        text = BuildCwcFromConfig(project, mdn, config, error, &info, /*include_eram=*/true);
        if (text.empty()) return false;
    } else {
        // 模板打补丁：按 disabled 计划改 is_enabled/inst/data/trig
        const DisabledPlan plan = ResolveDisabled(mdn.signals, config.disabled);
        std::map<std::string, int> totals;
        std::map<std::string, int> offs;
        for (const std::string& signal : mdn.signals) {
            std::string base = signal;
            const std::size_t open = signal.rfind('[');
            if (open != std::string::npos) base = signal.substr(0, open);
            ++totals[base];
            if (plan.IsBitDisabled(signal)) ++offs[base];
        }
        std::string out;
        std::size_t pos = 0;
        bool in_signal_vec = false;
        bool skipping = false;
        std::string current_bus;
        while (pos < text.size()) {
            const std::size_t next = text.find('\n', pos);
            std::string line =
                text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            pos = next == std::string::npos ? text.size() : next + 1;
            if (line.find("<signal_vec>") != std::string::npos) in_signal_vec = true;
            if (line.find("</signal_vec>") != std::string::npos) in_signal_vec = false;
            const std::size_t bus_at = line.find("<bus name=\"");
            if (bus_at != std::string::npos) {
                const std::size_t begin_q = bus_at + 11;
                const std::size_t close_q = line.find('"', begin_q);
                std::string name = line.substr(begin_q, close_q - begin_q);
                const std::size_t open = name.rfind('[');
                if (open != std::string::npos) name.erase(open);
                current_bus = name;
                bool off = false;
                for (const auto& [base, total] : totals) {
                    if (TailName(base) == TailName(name)) off = offs[base] >= total;
                }
                if (in_signal_vec && off) {
                    skipping = true;
                    continue;
                }
                if (!in_signal_vec) {
                    ReplaceAttr(&line, "is_enabled", off ? "no" : "yes");
                    if (off) ReplaceAttr(&line, "inst", "");
                }
            }
            if (skipping) {
                if (line.find("</bus>") != std::string::npos) skipping = false;
                continue;
            }
            const std::size_t net_at = line.find("<net name=\"");
            if (net_at != std::string::npos && !in_signal_vec) {
                const std::size_t begin_q = net_at + 11;
                const std::size_t close_q = line.find('"', begin_q);
                const std::string bit_name = line.substr(begin_q, close_q - begin_q);
                std::string full = bit_name;
                for (const auto& [base, total] : totals) {
                    (void)total;
                    if (TailName(base) == TailName(current_bus)) {
                        full = base + bit_name.substr(bit_name.find('['));
                    }
                }
                const bool off = plan.IsBitDisabled(full) || plan.IsBitDisabled(bit_name);
                ReplaceAttr(&line, "data", off ? "disable" : "enable");
                ReplaceAttr(&line, "trig", off ? "disable" : "enable");
            }
            out += line;
            out += '\n';
        }
        text = std::move(out);
    }
    const std::string user_cfg = project.dir + "/chip_debugger/user.cfg";
    if (IsFile(user_cfg)) {
        std::error_code ec;
        const std::string backup =
            fpgalsp::platform::state_dir(project.dir) + "/probe/user.cfg.bak";
        std::filesystem::create_directories(std::filesystem::path(backup).parent_path(), ec);
        std::filesystem::copy_file(user_cfg, backup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!fpgalsp::platform::write_file_utf8(user_cfg, text)) {
        if (error != nullptr) *error = "cannot write " + user_cfg;
        return false;
    }
    return true;
}

jsonrpc::Json WriteCompiledCwcAfterPhy(const project::Project& project) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    const MdnConfig mdn = ReadMdn(MdnPath(project));
    if (!mdn.exists || mdn.signals.empty()) {
        out["reason"] = "no probes marked - nothing to write";
        return out;
    }
    // ⚠ 只补缺，不覆盖：TD/GUI 或上一次成功抓取留下的 cwc 是**经过验证**的，
    //   我们生成的这份还不一定与位流完全对齐（实测拿它抓取会让 TD 会话在
    //   wait_on_hw_cw/write_hw_cw_data 阶段挂掉）。有就不要动。
    if (IsFile(CompiledCwc(project))) {
        const CwcSnapshot existing = ReadCwc(CompiledCwc(project));
        // 与当前 mdn/disabled 计划一致才复用；否则必须重建 —— disable 生效后
        // 位流里的 watcher 位宽会变小（实测 53 -> 48），留着旧 cwc 会让
        // `config_chipwatcher` 报出不同的 Chipwatcher code，抓取随即挂死。
        const ProbeBuildConfig existing_config = ReadProbeBuildConfig(project);
        const DisabledPlan existing_plan =
            ResolveDisabled(mdn.signals, existing_config.disabled);
        const int existing_disabled = static_cast<int>(existing_plan.bits.size());
        const int expected_enabled =
            static_cast<int>(mdn.signals.size()) - existing_disabled;
        const bool same_shape = existing.ok && existing.net_count == expected_enabled &&
                                existing.disabled_net_count == existing_disabled;
        if (same_shape) {
            out["ok"] = true;
            out["existed"] = true;
            out["written"] = false;
            out["net_count"] = existing.net_count;
            return out;
        }
    }
    const ProbeBuildConfig config = ReadProbeBuildConfig(project);
    const auto copy_bit_sidecar = [&project, &out]() {
        if (IsFile(CompiledBit(project))) return;
        const std::string run_bit =
            project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + ".bit";
        if (!IsFile(run_bit)) return;
        std::error_code copy_ec;
        std::filesystem::create_directories(
            std::filesystem::path(CompiledBit(project)).parent_path(), copy_ec);
        std::filesystem::copy_file(run_bit, CompiledBit(project),
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        out["bit_copied"] = !copy_ec;
    };
    // 优先用 TD 自己的 `chip_debugger/user.cfg` 当模板：它是 TD 格式的 watcher 配置
    //（总线/时钟/触发都正确），只要把 <logic_eram> 换成当前位流的 ERAM 行就能抓。
    // 实测（2026-11-21 23:4x）：user.cfg + 新 bid -> ok / 1024 点 / 触发完成。
    {
        const std::string user_cfg = project.dir + "/chip_debugger/user.cfg";
        if (IsFile(user_cfg)) {
            const jsonrpc::Json from_user_cfg = WriteCompiledCwcFromGate(project, user_cfg);
            if (from_user_cfg.value("ok", false)) {
                out = from_user_cfg;
                out["source"] = "chip_debugger/user.cfg";
                return out;
            }
        }
    }
    if (config.write_sidecar_cwc == false) {
        // 默认只同步位流：我们生成的 cwc 与 TD 自己那份不等价（Chipwatcher code 不同 ->
        // 抓取挂死），写出去反而会被后续 capture 误用。
        copy_bit_sidecar();
        out["ok"] = IsFile(CompiledBit(project));
        out["written"] = false;
        out["reason"] = "cwc generation is opt-in (probe_config{write_sidecar_cwc:true}); "
                        "bit sidecar was synced instead";
        return out;
    }
    std::string error;
    jsonrpc::Json info = jsonrpc::Json::object();
    // ⚠ 必须走 `BuildCwcFromWatcher`：它会拿 watcher 的 CWC_BUS_WIDTH 逐位校验总线顺序。
    //   用 mdn 顺序直接写会生成一份"位序与位流不一致"的 cwc —— TD 会接受
    //   config_chipwatcher（KIT-1004 照打），但 `wait_on_hw_cw` 永远等不到触发，
    //   实测表现为 TD 会话挂死。宁可拒绝。
    const std::string text = BuildCwcFromWatcher(project, mdn, config.cwc_bus_order,
                                                &error, &info);
    if (text.empty()) {
        out["reason"] = error;
        return out;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(CompiledCwc(project)).parent_path(), ec);
    if (!fpgalsp::platform::write_file_utf8(CompiledCwc(project), text)) {
        out["reason"] = "cannot write " + CompiledCwc(project);
        return out;
    }
    // bit 侧 sidecar 也补上
    if (!IsFile(CompiledBit(project))) {
        const std::string run_bit =
            project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + ".bit";
        if (IsFile(run_bit)) {
            std::error_code copy_ec;
            std::filesystem::copy_file(run_bit, CompiledBit(project),
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            out["bit_copied"] = !copy_ec;
        }
    }
    const CwcSnapshot written = ReadCwc(CompiledCwc(project));
    out["ok"] = written.ok;
    out["net_count"] = written.net_count;
    out["depth"] = config.has_depth ? config.depth : 1024;
    out["generated"] = info;
    if (!written.ok) out["reason"] = "generated cwc does not parse back";
    return out;
}

// =============================================================================
// 采样时钟：直接从**工程的 gate 数据库**问 TD
// =============================================================================
//  TD 只把采样时钟写进它自己的产物（watcher RTL 里没有、bid 里也没有），但
//  `report_cwc_status` 在 gate 数据库上会打印一行 `Wathcer | depth | Clock | ...`。
//  这就是"从用户工程读取"的正路：只要 syn 跑过（有 `<prj>_gate.db`），就能拿到 TD
//  为这些信号推导出的时钟，不依赖任何历史 cwc。
//  结果按 gate.db 的 (mtime,size) 缓存在 state dir，避免每次抓取都起一次 TD。
std::string QuerySampleClockFromGateDb(const td::TdChoice& td, const project::Project& project,
                                       std::string* error) {
    const auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return std::string();
    };
    const std::string gate_db =
        RunsDir(project) + "/syn_1/" + project.name + "_gate.db";
    std::error_code ec;
    if (!IsFile(gate_db)) {
        return fail("no " + gate_db + " - run stages:[\"all\"] first");
    }
    const auto stamp = std::filesystem::last_write_time(std::filesystem::path(gate_db), ec);
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(gate_db), ec);
    const std::string key = std::to_string(static_cast<long long>(stamp.time_since_epoch().count())) +
                            ":" + std::to_string(size);
    const std::string cache_path = StateProbeDir(project) + "/clock-cache.txt";
    if (IsFile(cache_path)) {
        const std::string cached = ReadText(cache_path);
        const std::size_t newline = cached.find('\n');
        if (newline != std::string::npos && cached.substr(0, newline) == key) {
            std::string value = cached.substr(newline + 1);
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
            if (!value.empty()) return value;
        }
    }

    td::TdSessionOptions options;
    options.work_dir = StateProbeDir(project);
    (void)fpgalsp::platform::ensure_directory(options.work_dir);
    std::unique_ptr<td::TdSession> session;
    try {
        session = std::make_unique<td::TdSession>(td, options);
    } catch (const std::exception& e) {
        return fail(std::string("cannot start TD: ") + e.what());
    }
    const std::string al = Slashes(project.al_path);
    const std::string commands =
        td::MakeImportDeviceCommand("ph1_100.db", project.device, project.speed) + "\n" +
        "cd {" + Slashes(project.dir) + "}\n" +
        "open_project {" + al + "} -single_run\n" +
        "import_db {" + Slashes(gate_db) + "}\n" +
        "report_cwc_status\n";
    const std::string out = session->Run(commands, 240.0);
    session->Kill();

    // 解析：`auto_created_cwc1 | 1024 | reg_init/clk | 0 | ...`
    std::string clock;
    std::size_t pos = 0;
    while (pos < out.size() && clock.empty()) {
        const std::size_t end = out.find('\n', pos);
        const std::string line = out.substr(pos, end == std::string::npos ? std::string::npos
                                                                        : end - pos);
        pos = end == std::string::npos ? out.size() : end + 1;
        if (line.find('|') == std::string::npos) continue;
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (begin <= line.size()) {
            const std::size_t bar = line.find('|', begin);
            fields.push_back(Trim(line.substr(begin, bar == std::string::npos ? std::string::npos
                                                                             : bar - begin)));
            if (bar == std::string::npos) break;
            begin = bar + 1;
        }
        if (fields.size() < 3) continue;
        if (fields[0].find("auto_created_cwc") == std::string::npos &&
            fields[0].find("chipwatcher") == std::string::npos) {
            continue;
        }
        const std::string candidate = fields[2];
        if (candidate.empty() || candidate == "Clock") continue;
        clock = candidate;
    }
    if (clock.empty()) {
        return fail("report_cwc_status did not report a watcher clock (is the watcher built? run "
                    "stages:[\"all\"] first)");
    }
    (void)fpgalsp::platform::write_file_utf8(cache_path, key + "\n" + clock + "\n");
    return clock;
}

jsonrpc::Json EnsureCompiledCwc(const project::Project& project,
                                   const std::vector<std::string>& bus_order) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["path"] = CompiledCwc(project);
    const MdnConfig shape_mdn = ReadMdn(MdnPath(project));

    // sidecar 的另一半：批量 phy 也不写 `cw/compiled.bit`（只有 GUI 会拷）。
    // 抓到数据只需要 bit + cwc 都在，所以这里顺手把 `<run>/<prj>.bit` 同步过去。
    if (!IsFile(CompiledBit(project))) {
        const std::string run_bit =
            project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + ".bit";
        if (IsFile(run_bit)) {
            std::error_code copy_ec;
            std::filesystem::create_directories(
                std::filesystem::path(CompiledBit(project)).parent_path(), copy_ec);
            std::filesystem::copy_file(run_bit, CompiledBit(project),
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            out["bit_copied"] = !copy_ec;
        }
    }
    if (IsFile(CompiledCwc(project))) {
        const CwcSnapshot existing = ReadCwc(CompiledCwc(project));
        // 与当前 mdn/disabled 计划一致才复用；否则必须重建 —— disable 生效后
        // 位流里的 watcher 位宽会变小（实测 53 -> 48），留着旧 cwc 会让
        // `config_chipwatcher` 报出不同的 Chipwatcher code，抓取随即挂死。
        const ProbeBuildConfig existing_config = ReadProbeBuildConfig(project);
        const DisabledPlan existing_plan =
            ResolveDisabled(shape_mdn.signals, existing_config.disabled);
        const int existing_disabled = static_cast<int>(existing_plan.bits.size());
        const int expected_enabled =
            static_cast<int>(shape_mdn.signals.size()) - existing_disabled;
        const bool same_shape = existing.ok && existing.net_count == expected_enabled &&
                                existing.disabled_net_count == existing_disabled;
        if (same_shape) {
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
    const std::string text = BuildCwcFromWatcher(project, mdn, bus_order, &error, &info);
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
    // 探针 sidecar 的自动重建默认**不**做（见 CaptureOptions::generate_cwc 的注释：
    // 生成的 ERAM 段没有经过 bit 侧校验，贸然拿去抓取可能让 TD 会话崩）。
    // Status 只报告"能不能重建"，把决定权留给调用方。
    const jsonrpc::Json ensured = std::filesystem::exists(CompiledCwc(project))
                                      ? jsonrpc::Json::object()
                                      : jsonrpc::Json::object();
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

    // disabled（不占 debug 资源）：请求侧来自 .fpgalsp/probe/config.txt 的 `disabled=`，
    // 实际侧看 compiled.cwc 里的 `is_enabled="no"` / `data="disable"`。
    const ProbeBuildConfig build = ReadProbeBuildConfig(project);
    const DisabledPlan disabled = ResolveDisabled(mdn.signals, build.disabled);
    {
        jsonrpc::Json list = jsonrpc::Json::array();
        for (const std::string& entry : disabled.entries) list.push_back(entry);
        out["disabled"] = std::move(list);
        out["disabled_count"] = static_cast<int>(disabled.bits.size());
        if (!disabled.unresolved.empty()) {
            jsonrpc::Json unresolved = jsonrpc::Json::array();
            for (const std::string& entry : disabled.unresolved) unresolved.push_back(entry);
            out["disabled_unresolved"] = std::move(unresolved);
        }
    }

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
    // disabled 数量：disabled_count = 配置里要求的位数（不占 debug 资源）；
    // disabled_bus_count / disabled_net_count = compiled.cwc 里实际标成 disable 的条目。
    watcher["disabled_count"] = static_cast<int>(disabled.bits.size());
    watcher["disabled_bus_count"] = cwc.disabled_bus_count;
    watcher["disabled_net_count"] = cwc.disabled_net_count;
    watcher["enabled_net_count"] = cwc.net_count - cwc.disabled_net_count;
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
                  "a trigger_*.cwc in the state dir's probe/), or save once from the TD GUI; capture needs it "
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
                        bool write, bool has_clock, const std::string& clock, int depth,
                        const std::vector<std::string>& disabled, bool has_disabled) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["path"] = MdnPath(project);

    const MdnConfig before = ReadMdn(MdnPath(project));
    const std::vector<std::string> after =
        signals.empty() ? before.signals : NormaliseSignals(signals);
    const std::set<std::string> old_set(before.signals.begin(), before.signals.end());
    const std::set<std::string> new_set(after.begin(), after.end());

    // disabled：显式给了就整体替换，没给就沿用落盘的那份（按新清单重新解析）。
    // 解析不到的条目只告警、不判失败 —— 清单可能刚被替换过。
    ProbeBuildConfig build = ReadProbeBuildConfig(project);
    const std::vector<std::string> disabled_input = has_disabled ? disabled : build.disabled;
    const DisabledPlan disabled_plan = ResolveDisabled(after, disabled_input);

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
    // ---- disabled / enabled 计数 ----
    // disabled 的条目不占 debug 资源，所以 enabled_* 才是 TD 真正要分配资源的量；
    // bus_count/bit_count 保持"清单全量"的旧语义，别让现有调用方跟着变。
    const int disabled_bit_count = static_cast<int>(disabled_plan.bits.size());
    const int enabled_bit_count = static_cast<int>(after.size()) - disabled_bit_count;
    int enabled_bus_count = 0;
    for (const auto& [name, width] : bus_widths) {
        int off = 0;
        for (int bit = 0; bit < width; ++bit) {
            if (disabled_plan.IsBitDisabled(name + "[" + std::to_string(bit) + "]")) ++off;
        }
        if (off < width) ++enabled_bus_count;
    }
    {
        jsonrpc::Json disabled_json = jsonrpc::Json::array();
        for (const std::string& entry : disabled_plan.entries) disabled_json.push_back(entry);
        out["disabled"] = std::move(disabled_json);
        out["disabled_count"] = disabled_bit_count;
        out["enabled_bus_count"] = enabled_bus_count;
        out["enabled_bit_count"] = enabled_bit_count;
        // disabled 只能通过我们自写的 cwc（insert_debugger -cwc）表达，而那条路
        // 默认是关的 —— 明确告诉调用方要开 apply_build_config，别让它以为写进
        // mdn 就生效了（mdn 没有 disable 语法）。
        out["requires_apply_build_config"] = disabled_plan.Any();
    }
    if (!disabled_plan.unresolved.empty()) {
        jsonrpc::Json unresolved = jsonrpc::Json::array();
        for (const std::string& entry : disabled_plan.unresolved) unresolved.push_back(entry);
        out["unresolved_disabled"] = std::move(unresolved);
        out["warning"] =
            "some disabled entries are not part of the probe list (they will be ignored): " +
            [&disabled_plan]() {
                std::string names;
                for (const std::string& entry : disabled_plan.unresolved) {
                    if (!names.empty()) names += ", ";
                    names += entry;
                }
                return names;
            }() +
            ". Disabled entries are matched against the current signal list; a bus base "
            "(WHG/x) disables all of its bits, WHG/x[2] disables a single bit.";
    }
    // ⚠ 故意**不**设上限：每颗芯片的 ChipWatcher 预算不同（TD 在 phy 阶段自己会报错），
    //   而且 GUI 里可以对任意信号 mark_debug、也可以 disable 掉列表里的条目
    //   （disable 的条目不占 debug 资源）。后端只报数量，超了让 TD 自己报错。
    jsonrpc::Json budget = jsonrpc::Json::object();
    budget["buses"] = static_cast<int>(bus_widths.size());
    budget["bits"] = static_cast<int>(after.size());
    budget["enabled_bus_count"] = enabled_bus_count;
    budget["enabled_bit_count"] = enabled_bit_count;
    budget["capped"] = false;
    budget["note"] =
        "no cap is applied here: every device has its own ChipWatcher budget and TD reports "
        "it during the phy stage. Disabled entries do not consume debug resources - remove or "
        "disable signals there if TD complains.";
    out["budget"] = std::move(budget);
    if (static_cast<int>(bus_widths.size()) > 4 || static_cast<int>(after.size()) > 64) {
        const std::string large =
            "this is a large probe list (4 buses / 64 bits is what worked on PH1A100SFG676); "
            "TD enforces the real per-device limit at the phy stage - if it errors, mark fewer "
            "signals (a bus counts once, e.g. WHG/x[15:0]) and rerun stages:[\"all\"].";
        out["warning"] = out.contains("warning") && out["warning"].is_string()
                             ? out["warning"].get<std::string>() + " " + large
                             : large;
    }

    if (!write) {
        out["ok"] = true;
        out["written"] = false;
        out["reason"] = "dry run (pass write:true to update chip_debugger/user.mdn" +
                        std::string(disabled_plan.Any()
                                        ? " and the state dir's probe/config.txt disabled= line)"
                                        : ")");
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
    // 采样时钟/深度 + disabled：TD 只在 "Optimize Gate 之后" 那次 insert_debugger 里采纳，
    // 而且必须通过 `-cwc <配置>` 传进去才会被采用（实测 depth 2048 生效）。
    // 这里只落盘参数；真正生成配置并挂钩子是 flow_runner 的事。
    const bool disabled_changed = disabled_plan.entries != build.disabled;
    if (has_clock || depth > 0 || has_disabled || disabled_changed) {
        if (has_clock) {
            build.has_clock = true;
            build.clock = clock;
        }
        if (depth > 0) {
            build.has_depth = true;
            build.depth = depth;
        }
        // 只落能落到清单上的条目：清单一换，旧的 disabled 条目就自动被剪掉
        //（未解析的已经在 unresolved_disabled 里告警过）。
        build.disabled = disabled_plan.entries;
        out["build_config_saved"] = WriteProbeBuildConfig(project, build);
        jsonrpc::Json saved = jsonrpc::Json::object();
        saved["clock"] = build.has_clock ? build.clock : std::string();
        saved["depth"] = build.has_depth ? build.depth : 0;
        jsonrpc::Json disabled_json = jsonrpc::Json::array();
        for (const std::string& entry : build.disabled) disabled_json.push_back(entry);
        saved["disabled"] = std::move(disabled_json);
        saved["path"] = ProbeConfigPath(project);
        out["build_config"] = std::move(saved);
    }
    out["reason"] =
        "user.mdn updated - now run stages [\"all\"] (NOT [\"phy\"]): TD creates the "
        "ChipWatcher instance right after Optimize Gate, which belongs to the syn run. "
        "The backend adds syn automatically when user.mdn is newer than the gate database.";
    if (disabled_plan.Any()) {
        // ⚠ user.mdn 没有 disable 语法：disable 只能靠我们自写的 cwc（insert_debugger -cwc），
        //   而 flow_runner 只在 apply_build_config:true 时才把那份 cwc 交给 TD。
        out["reason"] = out["reason"].get<std::string>() +
                        " Disabled entries only reach TD through the authored cwc: set "
                        "apply_build_config:true (otherwise they stay enabled in the bitstream).";
    }
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

    // 采样时钟：没有 sidecar（也就没有 TD 写的 clock）时，**直接从工程的 gate 数据库
    // 问 TD**（report_cwc_status），并落盘到 state dir 的 probe/config.txt，
    // 后续 flow 写 user.cfg 时直接复用 —— 不再依赖任何历史 cwc。
    if (!IsFile(CompiledCwc(project))) {
        const ProbeBuildConfig existing = ReadProbeBuildConfig(project);
        if (!existing.has_clock) {
            std::string clock_error;
            const std::string clock = QuerySampleClockFromGateDb(td, project, &clock_error);
            if (!clock.empty()) {
                ProbeBuildConfig updated = existing;
                updated.has_clock = true;
                updated.clock = clock;
                (void)WriteProbeBuildConfig(project, updated);
                out["clock_from_td"] = clock;
            } else if (!clock_error.empty()) {
                out["clock_query"] = clock_error;
            }
        }
    }

    // 抓取前按需补齐 sidecar：默认不生成（generate_cwc=false），因为生成的 ERAM 段
    // 没有经过 bit 侧校验；只有调用方明确要求时才生成并承担风险。
    jsonrpc::Json ensured = jsonrpc::Json::object();
    if (options.generate_cwc) {
        ensured = EnsureCompiledCwc(project, options.cwc_bus_order);
        if (ensured.contains("written") && ensured["written"].get<bool>()) {
            out["cwc_generated"] = true;
        }
    }
    const CwcSnapshot cwc = ReadCwc(CompiledCwc(project));
    if (!cwc.ok) {
        out["cwc"] = ensured;
        out["refused"] = true;
        if (out.contains("clock_query")) {
            // 时钟都拿不到时，"empty ChipWatcher configuration" 会误导：真正缺的是
            // syn 产物（gate 数据库）或显式 clock。
            out["reason"] = std::string("cannot determine the ChipWatcher sample clock: ") +
                            out["clock_query"].get<std::string>() +
                            " - run stages:[\"all\"] first, or pass clock to "
                            "fpga_probe_config (e.g. {\"clock\":\"reg_init/clk\"})";
        } else {
            out["reason"] = cwc.error.empty()
                                ? "no compiled ChipWatcher (phy_1/cw/compiled.cwc) - run the phy flow"
                                : cwc.error;
        }
        return out;
    }
    if (!IsFile(CompiledBit(project))) {
        out["refused"] = true;
        out["reason"] = "phy_1/cw/compiled.bit is missing - run the phy flow again";
        return out;
    }

    // disabled 探针的位流抓不了（TD 2026.2 批量流程的硬限制）：
    // 实测 8 种 cwc 变体（模板 / 新 ERAM / width 48/49/50/53/54/55 / 删掉 disabled 总线）
    // 全部失败，且只有两种结局：
    //   a) 触发成功但回读报 KIT-8445 "sample depth is shorter than the set value (54/48)"
    //      —— 位流里的采集 RAM 仍按 disable 前的宽度分配（54），而 cwc 只有 48 个 enabled 位；
    //   b) 换一份"宽度对齐"的 cwc 就变成 Chipwatcher code 与位流不一致，
    //      `wait_on_hw_cw` 永远等不到触发（会话挂 30 s）。
    // 与其让 agent 白等并杀掉会话，不如立刻拒绝并给出可执行的两条路。
    // disabled 位流现在可以抓了：sidecar 由 `WriteCompiledCwcFromGate` 写成
    // "gate cwc（总线/触发/时钟原样）+ 新 bid 的 <logic_eram> 行"，实测触发完成、1024 点。
    // 但如果 cwc 里 disabled 计数与位流不符（比如还留着上一轮的 sidecar），先给出提示。
    // disabled 位流的抓取：实测 2026-11-21 有 1 次成功（gate cwc + 新 bid ERAM -> 1024 点），
    // 但随后 2 次重建 × 3 份 cwc（各自的 sidecar 互相交换）= 6 次尝试全部失败：
    // 触发过程 `wait_on_hw_cw` 不返回，TD 会话退出。属于 TD 侧的系统性问题，
    // 不是我们 cwc 字段写错。默认立刻拒绝（0.3 s），别让 agent 白等 30 s 并杀会话。
    // disabled 位流现在可以正常抓（0.20.3 实测）：只要 chip_debugger/user.cfg 是完整配置，
    // TD 会在 phy 阶段自己写 phy_1/cw/compiled.cwc，config_chatcher/回读都正常。
    // 保留 allow_disabled_capture 参数只为兼容旧调用；默认不再拦截。
    if (false && cwc.disabled_net_count > 0 && !options.allow_disabled_capture) {
        out["refused"] = true;
        out["stage"] = "precheck";
        out["disabled_net_count"] = cwc.disabled_net_count;
        out["reason"] =
            "this bitstream was built with disabled probes and TD 2026.2 cannot read it back "
            "reliably in batch mode: the trigger never completes (wait_on_hw_cw hangs, session "
            "exits). One attempt succeeded with a sidecar made of the gate cwc plus fresh bid "
            "ERAM rows, but it is not reproducible (2 rebuilds x 3 cwc variants all failed). "
            "Clear the disabled list (fpga_probe_config{\"disabled\":[]}) and rerun "
            "stages:[\"all\"], or capture this bitstream from the TD GUI. Pass "
            "allow_disabled_capture:true to try anyway.";
        return out;
    }
    if (cwc.disabled_net_count > 0) {
        out["disabled_net_count"] = cwc.disabled_net_count;
        out["warning"] = "capturing a bitstream with disabled probes - TD may hang at the trigger "
                         "stage; this path is experimental.";
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
        effective.cwd = Slashes(fpgalsp::platform::state_dir(project.dir) + "/probe");
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
        // TD 解析 .al 里的相对路径以它自己的 cwd 为基准；会话 cwd 现在在系统 temp
        // （不污染工程），所以这里显式 cd 到工程目录，否则会报
        // `HDL-8007 cannot open Verilog file '/al_ip/...'`。cd 不会往工程里写日志。
        (void)session->Run("cd {" + Slashes(project.dir) + "}", 60.0);
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