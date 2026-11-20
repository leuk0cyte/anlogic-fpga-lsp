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

std::vector<std::string> NormaliseSignals(const std::vector<std::string>& signals) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& raw : signals) {
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

jsonrpc::Json Status(const project::Project& project) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["project"] = project.name;
    out["target"] = project.device;

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

    bool ready = cwc.ok && IsFile(CompiledBit(project)) && !stale;
    out["ready"] = ready;
    out["matches_user_mdn"] = !stale;
    if (!mdn.exists) {
        out["reason"] =
            "no chip_debugger/user.mdn - no probe signal is marked (TD will not insert a watcher)";
    } else if (!cwc.ok) {
        out["reason"] = "no compiled ChipWatcher yet - run the phy flow to embed the probes";
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
        "user.mdn updated - run Place & Route (stages [\"phy\"]) so the probes get embedded; "
        "the incremental route does NOT re-insert the debugger";
    return out;
}

// -----------------------------------------------------------------------------
// 采集
// -----------------------------------------------------------------------------

jsonrpc::Json Capture(const td::TdChoice& td, const project::Project& project,
                      const CaptureOptions& options) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["stage"] = "precheck";

    const CwcSnapshot cwc = ReadCwc(CompiledCwc(project));
    if (!cwc.ok) {
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
    {
        out["stage"] = "config_chipwatcher";
        const std::string command =
            "config_chipwatcher " + td::TclQuoteGrouped(CompiledCwc(project));
        const std::string text = session->Run(command, 180.0);
        for (const std::string& line : ErrorLines(text, 4)) record(line);
        if (td::HasError(text) || text.find("KIT-1004") == std::string::npos) {
            return fail("config_chipwatcher",
                        "cannot import the compiled ChipWatcher configuration", text);
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

    // 6) 把带探针的 bit 灌进去（jtag_burst 是 GUI 的写法；失败回落 jtag）
    if (effective.download) {
        out["stage"] = "download";
        std::string text;
        bool ok = false;
        for (const char* mode : {"jtag_burst", "jtag"}) {
            const std::string command =
                std::string("download -bit ") + td::TclQuoteGrouped(CompiledBit(project)) +
                " -mode " + mode + " -spd 7 -cable " + effective.cable +
                (std::string(mode) == "jtag_burst" ? " -total_dev 1 -cur_dev 1 -bypass 11111111"
                                                   : "");
            text = session->Run(command, effective.session_timeout);
            for (const std::string& line : ErrorLines(text, 4)) record(line);
            if (!td::HasError(text) && text.find("Chip validation success") != std::string::npos) {
                ok = true;
                out["download_mode"] = mode;
                break;
            }
        }
        if (!ok) return fail("download", "cannot load the probe-enabled bitstream", text);
    }

    // 7) 触发（用第 4 步拿到的对象变量，不能用名字字面量）
    {
        out["stage"] = "run_hw_cw";
        std::string command = "run_hw_cw $fpgalsp_hw";
        if (effective.trigger_now) command += " -trigger_now";
        command += " -cable " + effective.cable;
        const std::string text = session->Run(command, 120.0);
        if (td::HasError(text)) return fail("run_hw_cw", "cannot start the capture", text);
    }

    // 8) 等触发完成
    {
        out["stage"] = "wait_on_hw_cw";
        std::string command = "wait_on_hw_cw $fpgalsp_hw -timeout ";
        command += std::to_string(static_cast<int>(effective.wait_timeout));
        command += " -cable " + effective.cable;
        const std::string text = session->Run(command, effective.wait_timeout + 60.0);
        if (td::HasError(text)) return fail("wait_on_hw_cw", "capture did not complete", text);
    }

    // 9) 落 CSV
    {
        out["stage"] = "write_hw_cw_data";
        const std::string command =
            "write_hw_cw_data $fpgalsp_hw -csv_file " + td::TclQuoteGrouped(csv);
        const std::string text = session->Run(command, 180.0);
        if (td::HasError(text) || !IsFile(csv)) {
            return fail("write_hw_cw_data", "capture data was not written",
                        text.empty() ? std::string("no output") : text);
        }
    }

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