// =============================================================================
//  src/constraints/adc.cpp
// =============================================================================
//  逐条移植 Y:\fpga-lsp\fpgalsp\adc.py。
//
//  移植纪律（写给以后改这个文件的人）：
//    · 这里的每一段都对应 adc.py 的一个函数，函数名在注释里标了，
//      改行为前先去读那一段 Python，确认"是我写错了"还是"Python 就这样"。
//    · 发现 Python 行为可疑时，**先忠实复现 + 登记 tests/PYTHON-BUGS.md
//      + 用测试钉死**，不要顺手修。理由：C++ 与 Python 的逐字段对照是
//      阶段 5 的验收标准；顺手修会让差异无法归因。
//    · 正则一律 re2（std::regex 在本机 MSVC 上慢，且回溯递归容易爆栈）。
//    · 任何入口都不抛异常（`.adc` 是用户手写文件，畸形输入是常态）。
// =============================================================================

#include "constraints/adc.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "constraints/text_util.h"

#include <re2/re2.h>

namespace fpgalsp::constraints {
namespace adc {
namespace {

using lsp::Diagnostic;
using lsp::kError;
using lsp::kHint;
using lsp::kInfo;
using lsp::kWarning;
using text::IsSpace;
using text::LowerAscii;
using text::PyRepr;
using text::Strip;
using text::UpperAscii;

// -----------------------------------------------------------------------------
// 常量表
// -----------------------------------------------------------------------------

// Python: KNOWN_COMMANDS = frozenset({"set_pin_assignment"})
const std::set<std::string> kKnownCommands = {
    "set_pin_assignment",
};

// Python: KNOWN_IO_STANDARDS —— **71 项逐字复制**。
// 不要删减、不要"合并看起来重复的"：LVDS25E 与 LVDS25_E 是 TD 的两个不同取值。
// 白名单漏项会让用户的正确工程报 warning，是最伤信任的一类误报。
const std::set<std::string> kKnownIoStandards = {
    "BLVDS25", "DIFF_HSTL15", "DIFF_HSTL18_I", "DIFF_POD12", "DIFF_POD12_PZQ", "DIFF_SSTL12",
    "DIFF_SSTL12_PZQ", "DIFF_SSTL135", "DIFF_SSTL135_PZQ", "DIFF_SSTL15", "DIFF_SSTL15_PZQ",
    "DIFF_SSTL18_I", "HSTL12_I", "HSTL15", "HSTL18", "HSTL18_I", "HSTL18_II", "HSUL12",
    "LVCMOS10", "LVCMOS10V", "LVCMOS10V18", "LVCMOS10V25", "LVCMOS10V33", "LVCMOS12",
    "LVCMOS12L9", "LVCMOS12V18", "LVCMOS12V25", "LVCMOS12V33", "LVCMOS15", "LVCMOS15V18",
    "LVCMOS15V25", "LVCMOS15V33", "LVCMOS16", "LVCMOS18", "LVCMOS18L9", "LVCMOS18V",
    "LVCMOS18V25", "LVCMOS18V33", "LVCMOS25", "LVCMOS25V33", "LVCMOS33", "LVCMOS4", "LVCMOS8",
    "LVDS18", "LVDS18_E", "LVDS18_EH9", "LVDS25", "LVDS25E", "LVDS25_E", "LVDS33", "LVDS33_E",
    "LVDS71", "LVDS7_1", "LVPECL33", "LVTTL33", "MINI_LVDS25", "MINI_LVDS33", "PCI33", "POD12",
    "POD12_PZQ", "SSTL12", "SSTL12_PZQ", "SSTL135", "SSTL135_PZQ", "SSTL15", "SSTL15_PZQ",
    "SSTL15_R", "SSTL18", "SSTL18_I", "SSTL18_II", "SSTL25_I",
};

// Python: KNOWN_PULLTYPES（定义了，但 adc.py 从未用它校验 —— 见 PYTHON-BUGS.md）
const std::set<std::string> kKnownPulltypes = {
    "NONE", "PULLUP", "PULLDOWN", "KEEPER", "WEAKPULLUP", "WEAKPULLDOWN",
};

// Python: KNOWN_ATTR_KEYS（Python 里 PULLTYPE 写了两次，set 语义下等价）
const std::set<std::string> kKnownAttrKeys = {
    "LOCATION", "IOSTANDARD", "PULLTYPE", "DRIVESTRENGTH", "SLEWRATE",
    "VREF", "INTERNAL_VREF", "HYSTERESIS", "DIFFTERM", "IN_TERM",
    "OUT_TERM", "DCI_CASCADE", "CLOCK_DEDICATED_ROUTE",
};

// 诊断里的 source：Python `_diag` 写死 "fpga-lsp"，server 的 `_sanitize`
// 只在 falsy 时才替换，所以**线上最终也是 "fpga-lsp"**。保持一致，
// 不要改成 "fpga-lsp:adc"（那会让与 Python 的逐字段对照多一个差异）。
constexpr const char* kSource = "fpga-lsp";

// -----------------------------------------------------------------------------
// 正则（re2 静态编译一次；模式与 adc.py 逐字对应）
// -----------------------------------------------------------------------------

// Python: _PORT_RE = ^([A-Za-z_][A-Za-z0-9_$]*)(?:\[\s*(\d+)\s*\])?$
const re2::RE2& PortRe() {
    static const re2::RE2 re(R"(^([A-Za-z_][A-Za-z0-9_$]*)(?:\[\s*(\d+)\s*\])?$)");
    return re;
}

// Python 的 _ATTR_RE = ([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]*)(?:;|$)
// 由 ParseAttrs() 逐字符实现（见那里的说明），不再用 re2。

// Python: _WORD_RE / fallback re.match(r"[A-Za-z_][A-Za-z0-9_$]*", port)
// 两者都用 text::LeadingIdentifier() 手写实现，不再需要 re2 实例 —— 原因见
// text_util.h 的注释（RE2 的变参只对应捕获组，0 捕获组时传参数会返回 false）。

// -----------------------------------------------------------------------------
// span / range 小工具（对应 adc.py 的 _span / _range / _whole_line / _in_span）
// -----------------------------------------------------------------------------

lsp::Range RangeOf(int line, int start, int end) {
    lsp::Range range;
    range.start.line = line;
    range.start.character = start;
    range.end.line = line;
    range.end.character = end;
    return range;
}

lsp::Range RangeOf(const Span& span) { return RangeOf(span.line, span.start, span.end); }

// Python: _whole_line(entry) -> _range(line, 0, len(raw))
Span WholeLineSpan(const Entry& entry) {
    return Span{entry.line, 0, static_cast<int>(entry.raw.size())};
}

// Python: _in_span(span, character) -> start <= character <= end
// 注意 end 是**闭**的（不是 LSP 的半开区间），这是 Python 版既有语义，照抄。
bool InSpan(const Span& span, int character) {
    return span.start <= character && character <= span.end;
}

// Python: _diag(code, severity, rng, message, **data)
Diagnostic MakeDiag(std::string code, int severity, const lsp::Range& range, std::string message) {
    Diagnostic diagnostic =
        Diagnostic::AtLine(range.start.line, range.start.character, range.end.character, severity,
                           std::move(code), kSource, std::move(message));
    diagnostic.WithData("origin", Json("adc"));
    return diagnostic;
}

Diagnostic MakeDiag(std::string code, int severity, const Span& span, std::string message) {
    return MakeDiag(std::move(code), severity, RangeOf(span), std::move(message));
}

// Python: 诊断最终按 (line, character, code) 排序（稳定排序）
void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         if (a.range.start.line != b.range.start.line) {
                             return a.range.start.line < b.range.start.line;
                         }
                         if (a.range.start.character != b.range.start.character) {
                             return a.range.start.character < b.range.start.character;
                         }
                         return a.code < b.code;
                     });
}

// -----------------------------------------------------------------------------
// JSON 取值（全部防御式：类型不符就当缺失，绝不抛）
// -----------------------------------------------------------------------------

const Json* JsonMember(const Json& json, const char* key) {
    if (!json.is_object()) return nullptr;
    const auto it = json.find(key);
    if (it == json.end()) return nullptr;
    return &(*it);
}

// Python 的 `str(x)`（只覆盖 JSON 里可能出现的类型）
std::string JsonToPyStr(const Json& json) {
    if (json.is_string()) return json.get<std::string>();
    if (json.is_boolean()) return json.get<bool>() ? "True" : "False";
    if (json.is_number_integer()) return std::to_string(json.get<long long>());
    if (json.is_number_unsigned()) return std::to_string(json.get<unsigned long long>());
    if (json.is_number_float()) {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%g", json.get<double>());
        return buffer;
    }
    if (json.is_null()) return "None";
    return {};
}

// Python 的真值判断（够用即可）
bool JsonTruthy(const Json& json) {
    if (json.is_null()) return false;
    if (json.is_boolean()) return json.get<bool>();
    if (json.is_string()) return !json.get<std::string>().empty();
    if (json.is_number_integer()) return json.get<long long>() != 0;
    if (json.is_number_unsigned()) return json.get<unsigned long long>() != 0;
    if (json.is_number_float()) return json.get<double>() != 0.0;
    if (json.is_array() || json.is_object()) return !json.empty();
    return true;
}

// -----------------------------------------------------------------------------
// 有序分组（Python dict 保插入顺序；诊断输出顺序依赖它，所以不能用 std::map）
// -----------------------------------------------------------------------------

class OrderedGroups {
  public:
    void Add(const std::string& key, const Entry* entry) {
        const auto it = index_.find(key);
        if (it == index_.end()) {
            index_.emplace(key, groups_.size());
            groups_.emplace_back();
            groups_.back().push_back(entry);
            return;
        }
        groups_[it->second].push_back(entry);
    }

    const std::vector<std::vector<const Entry*>>& groups() const { return groups_; }

  private:
    std::vector<std::vector<const Entry*>> groups_;
    std::unordered_map<std::string, size_t> index_;
};

std::string PortGroupKey(const Entry& entry) {
    std::string key = entry.base;
    key.push_back('\x1f');  // 标识符里不可能出现的分隔符
    if (entry.bit.has_value()) {
        key += std::to_string(*entry.bit);
    } else {
        key += "-";
    }
    return key;
}

// LOCATION 分组键：strip + lower（Python `location.strip().lower()`）
std::optional<std::string> PinGroupKey(const Entry& entry) {
    const auto it = entry.attrs.find("LOCATION");
    if (it == entry.attrs.end()) return std::nullopt;
    const std::string_view trimmed = Strip(it->second);
    if (trimmed.empty()) return std::nullopt;
    return LowerAscii(trimmed);
}

// -----------------------------------------------------------------------------
// {…} 分组（对应 adc.py 的 _brace_groups）
// -----------------------------------------------------------------------------

struct BraceGroup {
    int brace_start = 0;
    int content_start = 0;
    int content_end = 0;
    int brace_end = 0;
};

std::vector<BraceGroup> BraceGroups(std::string_view line) {
    std::vector<BraceGroup> groups;
    int depth = 0;
    int content_start = -1;
    for (int index = 0; index < static_cast<int>(line.size()); ++index) {
        const char ch = line[static_cast<size_t>(index)];
        if (ch == '{') {
            if (depth == 0) content_start = index + 1;
            ++depth;
        } else if (ch == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && content_start >= 0) {
                    groups.push_back(
                        BraceGroup{content_start - 1, content_start, index, index + 1});
                    content_start = -1;
                }
            }
        }
    }
    return groups;
}

// 对应 _group_token：返回组内去两端空白后的 token，并写回它在行内的 span
std::string GroupToken(std::string_view line, const BraceGroup& group, int lineno, Span* span) {
    const std::string_view segment =
        line.substr(static_cast<size_t>(group.content_start),
                    static_cast<size_t>(group.content_end - group.content_start));
    const std::string_view trimmed = Strip(segment);
    const size_t lead = segment.size() - text::LStrip(segment).size();
    const int token_start = group.content_start + static_cast<int>(lead);
    const int token_end = token_start + static_cast<int>(trimmed.size());
    *span = Span{lineno, token_start, token_end};
    return std::string(trimmed);
}

// 对应 _parse_attrs —— 属性组内的 `KEY = VALUE;` 对
//
// 实现方式：**逐字符扫描**，语义等价于 Python 的
//   for match in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]*)(?:;|$)", segment)
//
// 为什么不用 re2：
//   RE2 的变参（FullMatch/FindAndConsume 的 ...）**只对应捕获组 1..n**，没有
//   "整段匹配"的位置，所以拿不到 match.start(2) 这类偏移；而这里恰好需要精确
//   列号（hover 和诊断都指向属性值）。手写扫描把"不匹配就前进一个字符"的
//   finditer 语义写清楚，比反推 re2 的下标映射更可靠。
void ParseAttrs(std::string_view line, const BraceGroup& group, int lineno, Entry* entry) {
    const size_t content_len = static_cast<size_t>(group.content_end - group.content_start);
    const std::string_view segment =
        line.substr(static_cast<size_t>(group.content_start), content_len);
    const int base = group.content_start;
    const size_t length = segment.size();

    const auto ident_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const auto ident_char = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_';
    };

    size_t position = 0;
    while (position < length) {
        const size_t key_start = position;
        if (!ident_start(segment[key_start])) {
            ++position;  // finditer：这里不成 match，前进一格
            continue;
        }
        size_t key_end = key_start + 1;
        while (key_end < length && ident_char(segment[key_end])) ++key_end;

        // \s*=\s*
        size_t cursor = key_end;
        while (cursor < length && IsSpace(segment[cursor])) ++cursor;
        if (cursor >= length || segment[cursor] != '=') {
            position = key_start + 1;  // 键后面没有 '=' -> 本位置不成 match
            continue;
        }
        ++cursor;
        while (cursor < length && IsSpace(segment[cursor])) ++cursor;

        // ([^;]*)(?:;|$)
        const size_t value_begin = cursor;
        while (cursor < length && segment[cursor] != ';') ++cursor;
        const size_t value_raw_end = cursor;
        if (cursor < length) ++cursor;  // 吃掉 ';'
        position = (cursor > key_start) ? cursor : key_start + 1;

        const std::string key_raw(segment.substr(key_start, key_end - key_start));
        const std::string key = UpperAscii(key_raw);

        // Python：
        //   value_start = base + match.start(2) + lead
        //   value_end   = base + match.start(2) + tail
        //   value       = line[value_start:value_end]
        // match.start(2) == value_begin（'=' 后的空白已被正则吃掉）；
        // lead/tail 是 group(2) 两端空白修剪后的长度。
        const std::string_view value_raw =
            segment.substr(value_begin, value_raw_end - value_begin);
        const size_t lead = value_raw.size() - text::LStrip(value_raw).size();
        const size_t tail = text::RStrip(value_raw).size();
        const int value_start = base + static_cast<int>(value_begin) + static_cast<int>(lead);
        const int value_end = base + static_cast<int>(value_begin) + static_cast<int>(tail);

        // 值全为空白时 value_end == value_start，切片得到空串（Python 同）。
        std::string value;
        if (value_end > value_start) {
            value = std::string(line.substr(static_cast<size_t>(value_start),
                                            static_cast<size_t>(value_end - value_start)));
        }

        AttrItem item;
        item.key = key_raw;
        item.key_upper = key;
        item.value = value;
        item.key_span =
            Span{lineno, base + static_cast<int>(key_start), base + static_cast<int>(key_end)};
        item.value_span = Span{lineno, value_start, value_end};
        entry->attr_items.push_back(item);
        // Python: 后者覆盖前者（TD 的顺序解析语义）
        entry->attrs[key] = value;
        entry->attr_spans[key] = item.value_span;
    }
}

// -----------------------------------------------------------------------------
// 端口名解析（对应 _PORT_RE / _split_port）
// -----------------------------------------------------------------------------

int DigitsToInt(std::string_view digits) {
    // 不使用 std::stoi：超长数字会抛 out_of_range。这里饱和到 int 上限，
    // 方向是"把荒谬 bit 下标当成越界"，不会误报合法值。
    long long value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') continue;
        value = value * 10 + (c - '0');
        if (value > 2147483647LL) return 2147483647;
    }
    return static_cast<int>(value);
}

// -----------------------------------------------------------------------------
// 设备引脚表（对应 _pin_maps / _resolve_pin）
// -----------------------------------------------------------------------------

struct PinMaps {
    // 有序性无关，只做查找
    std::unordered_map<std::string, const Json*> pins;
    std::unordered_map<std::string, std::string> location_to_key;
    std::unordered_map<std::string, const Json*> dedicated;
    // 为 "dedicated" 数组里的裸字符串合成 {"pe": item} —— Python 也是这么做的。
    // 用 deque 保证元素地址稳定（vector 扩容会让已存指针失效）。
    std::deque<Json> synthesized;
};

PinMaps BuildPinMaps(const Json& device) {
    PinMaps maps;
    if (!device.is_object()) return maps;

    if (const Json* raw_pins = JsonMember(device, "pins"); raw_pins && raw_pins->is_object()) {
        for (auto it = raw_pins->begin(); it != raw_pins->end(); ++it) {
            const Json* record = &(*it);
            static const Json kEmptyRecord = Json::object();
            if (!record->is_object()) record = &kEmptyRecord;

            const std::string lower_key = LowerAscii(it.key());
            maps.pins[lower_key] = record;  // 后写覆盖（Python: pins[key] = record）

            if (const Json* pe = JsonMember(*record, "pe"); pe && JsonTruthy(*pe)) {
                maps.pins.emplace(LowerAscii(JsonToPyStr(*pe)), record);  // setdefault
            }
            if (const Json* location = JsonMember(*record, "location");
                location && JsonTruthy(*location)) {
                maps.location_to_key.emplace(LowerAscii(JsonToPyStr(*location)), lower_key);
            }
        }
    }

    if (const Json* raw_dedicated = JsonMember(device, "dedicated");
        raw_dedicated && raw_dedicated->is_array()) {
        for (const Json& item : *raw_dedicated) {
            if (item.is_object()) {
                const Json* pe = JsonMember(item, "pe");
                if (pe == nullptr || !JsonTruthy(*pe)) pe = JsonMember(item, "name");
                if (pe != nullptr && JsonTruthy(*pe)) {
                    maps.dedicated[LowerAscii(JsonToPyStr(*pe))] = &item;
                }
                if (const Json* location = JsonMember(item, "location");
                    location && JsonTruthy(*location)) {
                    maps.dedicated.emplace(LowerAscii(JsonToPyStr(*location)), &item);
                }
            } else if (item.is_string()) {
                const std::string text = item.get<std::string>();
                Json synthesized = Json::object();
                synthesized["pe"] = text;
                maps.synthesized.push_back(std::move(synthesized));
                maps.dedicated[LowerAscii(text)] = &maps.synthesized.back();
            }
        }
    }
    return maps;
}

enum class PinKind { Pin, Dedicated, Missing };

// 对应 _resolve_pin
std::pair<PinKind, const Json*> ResolvePin(const std::string& location, const PinMaps& maps) {
    const std::string lowered = LowerAscii(Strip(location));
    if (const auto it = maps.dedicated.find(lowered); it != maps.dedicated.end()) {
        return {PinKind::Dedicated, it->second};
    }
    const Json* record = nullptr;
    if (const auto it = maps.pins.find(lowered); it != maps.pins.end()) {
        record = it->second;
    } else if (const auto key_it = maps.location_to_key.find(lowered);
               key_it != maps.location_to_key.end()) {
        if (const auto pin_it = maps.pins.find(key_it->second); pin_it != maps.pins.end()) {
            record = pin_it->second;
        }
    }
    if (record != nullptr) {
        const std::string type =
            LowerAscii(JsonMember(*record, "type") != nullptr
                           ? JsonToPyStr(*JsonMember(*record, "type"))
                           : std::string());
        if (type == "dedicated" || type == "config") return {PinKind::Dedicated, record};
        return {PinKind::Pin, record};
    }
    return {PinKind::Missing, nullptr};
}

// 去重后的端口列表（按声明顺序）+ 名字索引（重名时第一个胜出）
struct OrderedPorts {
    std::vector<const Port*> ports;
    std::unordered_map<std::string, const Port*> by_name;
};

OrderedPorts BuildOrderedPorts(const std::vector<Port>& ports) {
    OrderedPorts result;
    result.ports.reserve(ports.size());
    result.by_name.reserve(ports.size());
    for (const Port& port : ports) {
        if (port.name.empty()) continue;
        const auto inserted = result.by_name.emplace(port.name, &port);
        if (inserted.second) result.ports.push_back(&port);
    }
    return result;
}

}  // namespace

// =============================================================================
//  常量表访问
// =============================================================================

const std::set<std::string>& KnownCommands() { return kKnownCommands; }
bool IsKnownCommand(std::string_view command) {
    return kKnownCommands.find(std::string(command)) != kKnownCommands.end();
}

const std::set<std::string>& KnownIoStandards() { return kKnownIoStandards; }

// Python: `standard.upper() not in KNOWN_IO_STANDARDS` —— 大小写不敏感
bool IsKnownIoStandard(std::string_view standard) {
    return kKnownIoStandards.find(UpperAscii(standard)) != kKnownIoStandards.end();
}

const std::set<std::string>& KnownPulltypes() { return kKnownPulltypes; }
const std::set<std::string>& KnownAttrKeys() { return kKnownAttrKeys; }

// =============================================================================
//  解析
// =============================================================================

PortName SplitPort(std::string_view port) {
    PortName result;
    if (port.empty()) return result;

    absl::string_view input(port.data(), port.size());
    re2::StringPiece name_piece;
    re2::StringPiece bit_piece;
    re2::RE2::Arg arg_name(&name_piece);
    re2::RE2::Arg arg_bit(&bit_piece);
    const re2::RE2::Arg* args[2] = {&arg_name, &arg_bit};
    if (re2::RE2::FullMatchN(input, PortRe(), args, 2)) {
        result.base.assign(name_piece.data(), name_piece.size());
        if (!bit_piece.empty()) result.bit = DigitsToInt({bit_piece.data(), bit_piece.size()});
        return result;
    }

    // 畸形端口文本：仍然尽量给出基名（Python: re.match 取一段标识符前缀）
    const std::string_view prefix = text::LeadingIdentifier(port);
    if (!prefix.empty()) {
        result.base.assign(prefix);
    } else {
        result.base = std::string(port);
    }
    return result;
}

std::optional<Entry> ParseLine(std::string_view raw, int line) {
    const std::string_view stripped = Strip(raw);
    if (stripped.empty()) return std::nullopt;
    if (stripped.front() == '#') return std::nullopt;
    if (stripped.size() >= 2 && stripped[0] == '/' && stripped[1] == '/') return std::nullopt;

    // Python: character = len(raw) - len(raw.lstrip())
    const int character = static_cast<int>(raw.size() - text::LStrip(raw).size());

    // Python: command = re.match(r"\S+", raw[character:])
    std::string command;
    int command_end = character;
    {
        size_t index = static_cast<size_t>(character);
        while (index < raw.size() && !IsSpace(raw[index])) ++index;
        command.assign(raw.substr(static_cast<size_t>(character),
                                  index - static_cast<size_t>(character)));
        command_end = static_cast<int>(index);
    }
    if (command.empty()) command = std::string(stripped);

    Entry entry;
    entry.line = line;
    entry.character = character;
    entry.raw = std::string(raw);
    entry.command = command;
    entry.command_span = Span{line, character, command_end};
    entry.port_span = Span{line, character, command_end};  // Python 的初值

    const std::vector<BraceGroup> groups = BraceGroups(raw);
    std::vector<BraceGroup> attr_groups;
    std::string port;

    if (groups.size() >= 2) {
        port = GroupToken(raw, groups[0], line, &entry.port_span);
        attr_groups.push_back(groups[1]);
    } else if (groups.size() == 1) {
        const BraceGroup& group = groups[0];
        const std::string_view content =
            raw.substr(static_cast<size_t>(group.content_start),
                       static_cast<size_t>(group.content_end - group.content_start));
        if (content.find('=') != std::string_view::npos) {
            attr_groups.push_back(group);
        } else {
            port = GroupToken(raw, group, line, &entry.port_span);
        }
    }

    if (!attr_groups.empty()) ParseAttrs(raw, attr_groups[0], line, &entry);

    const PortName split = SplitPort(port);
    entry.port = port;
    entry.base = split.base;
    entry.bit = split.bit;
    return entry;
}

std::vector<Entry> Parse(std::string_view text) {
    std::vector<Entry> entries;
    const std::vector<std::string_view> lines = text::SplitLines(text);
    entries.reserve(lines.size());
    for (int index = 0; index < static_cast<int>(lines.size()); ++index) {
        // Python 用 try/except 包住单行解析：任何异常都只丢这一行。
        // C++ 侧不会抛，但保留"坏行只丢自己"的语义。
        if (auto entry = ParseLine(lines[static_cast<size_t>(index)], index)) {
            entries.push_back(std::move(*entry));
        }
    }
    return entries;
}

// =============================================================================
//  覆盖率
// =============================================================================

CoverageReport Coverage(const std::vector<Entry>& entries, const std::vector<Port>& ports,
                     [[maybe_unused]] std::string_view top) {
    CoverageReport result;
    const OrderedPorts ordered = BuildOrderedPorts(ports);

    std::unordered_map<std::string, std::vector<const Entry*>> by_base;
    for (const Entry& entry : entries) {
        if (!entry.base.empty()) by_base[entry.base].push_back(&entry);
    }

    // ---- 重复端口 bit / 重复引脚 ----
    OrderedGroups port_groups;
    OrderedGroups pin_groups;
    for (const Entry& entry : entries) {
        if (!entry.port.empty()) port_groups.Add(PortGroupKey(entry), &entry);
        if (const auto key = PinGroupKey(entry)) pin_groups.Add(*key, &entry);
    }
    for (const auto& group : port_groups.groups()) {
        if (group.size() <= 1) continue;
        DuplicatePort duplicate;
        duplicate.port = group.front()->port;
        for (const Entry* entry : group) duplicate.lines.push_back(entry->line);
        std::sort(duplicate.lines.begin(), duplicate.lines.end());
        result.duplicate_ports.push_back(std::move(duplicate));
    }
    for (const auto& group : pin_groups.groups()) {
        if (group.size() <= 1) continue;
        DuplicatePin duplicate;
        const auto it = group.front()->attrs.find("LOCATION");
        duplicate.pin = (it != group.front()->attrs.end()) ? it->second : std::string();
        for (const Entry* entry : group) {
            duplicate.ports.push_back(entry->port);
            duplicate.lines.push_back(entry->line);
        }
        std::sort(duplicate.lines.begin(), duplicate.lines.end());
        result.duplicate_pins.push_back(std::move(duplicate));
    }

    // ---- 逐端口统计 ----
    for (const Port* port : ordered.ports) {
        const std::vector<int> bits = PortBits(*port);
        const bool is_bus = !bits.empty();

        std::vector<const Entry*> group;
        if (const auto it = by_base.find(port->name); it != by_base.end()) group = it->second;

        std::unordered_set<int> valid_bits;
        bool scalar_present = false;
        for (const Entry* entry : group) {
            if (entry->bit.has_value()) {
                valid_bits.insert(*entry->bit);
            } else {
                scalar_present = true;
            }
        }

        std::vector<int> present;
        if (is_bus) {
            present.reserve(valid_bits.size());
            for (const int bit : bits) {
                if (valid_bits.find(bit) != valid_bits.end()) present.push_back(bit);
            }
        }

        if (is_bus) {
            result.total_bits += static_cast<int>(bits.size());
            result.constrained_bits += static_cast<int>(present.size());
            if (!present.empty()) result.covered[port->name] = present;
            if (present.empty()) result.unconstrained_ports.push_back(port->name);

            std::vector<int> missing;
            for (const int bit : bits) {
                if (std::find(present.begin(), present.end(), bit) == present.end()) {
                    missing.push_back(bit);
                }
            }
            if (!missing.empty()) result.missing[port->name] = std::move(missing);
        } else {
            result.total_bits += 1;
            if (scalar_present) {
                result.covered[port->name] = {};
                result.constrained_bits += 1;
            } else {
                result.unconstrained_ports.push_back(port->name);
                result.missing[port->name] = {};
            }
        }
    }

    // ---- 顶层模块里不存在的端口（按条目顺序） ----
    for (const Entry& entry : entries) {
        if (entry.base.empty()) continue;
        if (ordered.by_name.find(entry.base) == ordered.by_name.end()) {
            result.extra_ports.push_back(entry);
        }
    }
    return result;
}

// =============================================================================
//  诊断
// =============================================================================

std::vector<Diagnostic> Diagnostics(const std::vector<Entry>& entries,
                                    const std::vector<Port>& ports, const Json* device,
                                    [[maybe_unused]] std::string_view text) {
    DiagnosticsOptions options;
    options.device = device;
    return Diagnostics(entries, ports, options);
}

std::vector<Diagnostic> Diagnostics(const std::vector<Entry>& entries,
                                    const std::vector<Port>& ports,
                                    const DiagnosticsOptions& options) {
    std::vector<Diagnostic> result;
    const OrderedPorts ordered = BuildOrderedPorts(ports);
    // Python: check_ports = bool(port_by_name)
    // 没有顶层端口表（单独打开一个 .adc 预览）时无法判断端口是否存在，跳过该检查。
    const bool check_ports = !ordered.by_name.empty();
    const PinMaps maps = (options.device != nullptr) ? BuildPinMaps(*options.device) : PinMaps{};

    for (const Entry& entry : entries) {
        // ---- 未知命令（Python: ADC-CMD，severity 3）----
        if (kKnownCommands.find(entry.command) == kKnownCommands.end()) {
            result.push_back(MakeDiag(
                "ADC-CMD", kInfo, entry.command_span,
                "Unknown ADC command " + PyRepr(entry.command) + " (expected set_pin_assignment)"));
            continue;  // Python 对未知命令直接 continue，不再做后续检查
        }

        const Span port_span = entry.port_span;
        const lsp::Range port_range = RangeOf(port_span);

        if (entry.port.empty()) {
            result.push_back(MakeDiag("ADC-ATTR", kWarning, WholeLineSpan(entry),
                                      "set_pin_assignment is missing the { port } argument"));
        }

        // ---- 属性：重复键 warning / 未知键 hint ----
        std::unordered_set<std::string> seen;
        for (const AttrItem& item : entry.attr_items) {
            const std::string key = item.key_upper.empty() ? item.key : item.key_upper;
            if (seen.find(key) != seen.end()) {
                result.push_back(MakeDiag("ADC-ATTR", kWarning, item.value_span,
                                          "Duplicate attribute " + key +
                                              "; later values override earlier ones"));
            } else {
                seen.insert(key);
                if (kKnownAttrKeys.find(key) == kKnownAttrKeys.end()) {
                    result.push_back(MakeDiag("ADC-ATTR", kInfo, item.key_span,
                                              "Unknown ADC attribute " + key));
                }
            }
        }

        // ---- 端口存在性 / bit 合法性 ----
        const Port* port_object = nullptr;
        if (!entry.base.empty()) {
            if (const auto it = ordered.by_name.find(entry.base); it != ordered.by_name.end()) {
                port_object = it->second;
            }
        }
        if (!entry.port.empty() && check_ports) {
            if (port_object == nullptr) {
                result.push_back(MakeDiag("USR-8052", kError, port_range,
                                          "Port " + entry.port +
                                              " does not exist in the top module"));
            } else {
                const std::vector<int> bits = PortBits(*port_object);
                if (!entry.bit.has_value()) {
                    if (!bits.empty()) {
                        result.push_back(MakeDiag(
                            "USR-8052", kError, port_range,
                            "Bus port " + entry.base + " is missing a bit index (write " +
                                entry.base + "[n])"));
                    }
                } else if (bits.empty()) {
                    result.push_back(MakeDiag("USR-8052", kError, port_range,
                                              "Scalar port " + entry.base +
                                                  " must not have a bit index"));
                } else if (std::find(bits.begin(), bits.end(), *entry.bit) == bits.end()) {
                    result.push_back(MakeDiag("USR-8052", kError, port_range,
                                              "Port bit " + entry.port +
                                                  " is outside the range of " + entry.base));
                }
            }
        }

        // ---- LOCATION ----
        const auto location_it = entry.attrs.find("LOCATION");
        const std::string location =
            (location_it != entry.attrs.end()) ? std::string(Strip(location_it->second)) : "";
        const auto location_span_it = entry.attr_spans.find("LOCATION");
        const Span location_span = (location_span_it != entry.attr_spans.end())
                                       ? location_span_it->second
                                       : port_span;
        if (location.empty()) {
            result.push_back(MakeDiag("USR-8056", kError, port_range, "Missing LOCATION attribute"));
        } else if (options.device != nullptr) {
            const auto [kind, record] = ResolvePin(location, maps);
            (void)record;
            if (kind == PinKind::Dedicated) {
                result.push_back(MakeDiag("USR-8027", kError, location_span,
                                          location +
                                              " is a dedicated/configuration pin and cannot be "
                                              "used as a regular I/O"));
            } else if (kind == PinKind::Missing) {
                result.push_back(MakeDiag("USR-8026", kError, location_span,
                                          "Pin " + location + " does not exist on this device"));
            }
        }

        // ---- IOSTANDARD ----
        const auto standard_it = entry.attrs.find("IOSTANDARD");
        const std::string standard =
            (standard_it != entry.attrs.end()) ? std::string(Strip(standard_it->second)) : "";
        const auto standard_span_it = entry.attr_spans.find("IOSTANDARD");
        const Span standard_span = (standard_span_it != entry.attr_spans.end())
                                       ? standard_span_it->second
                                       : port_span;
        if (standard.empty()) {
            result.push_back(MakeDiag("ADC-NO-IOSTD", kWarning, port_range,
                                      "Missing IOSTANDARD attribute"));
        } else if (!IsKnownIoStandard(standard)) {
            result.push_back(MakeDiag("ADC-IOSTD", kWarning, standard_span,
                                      "Unknown I/O standard " + standard));
        }
    }

    // ---- 重复端口 bit ----
    {
        OrderedGroups groups;
        for (const Entry& entry : entries) {
            if (!entry.port.empty()) groups.Add(PortGroupKey(entry), &entry);
        }
        for (const auto& group : groups.groups()) {
            if (group.size() < 2) continue;
            const Entry* first = group.front();
            for (size_t index = 1; index < group.size(); ++index) {
                const Entry* entry = group[index];
                result.push_back(MakeDiag(
                    "ADC-DUP-PORT", kError, entry->port_span,
                    "Port bit " + entry->port + " is assigned more than once (first at line " +
                        std::to_string(first->line + 1) + ")"));
            }
        }
    }

    // ---- 重复引脚 ----
    {
        OrderedGroups groups;
        for (const Entry& entry : entries) {
            if (const auto key = PinGroupKey(entry)) groups.Add(*key, &entry);
        }
        for (const auto& group : groups.groups()) {
            if (group.size() < 2) continue;
            const Entry* first = group.front();
            for (size_t index = 1; index < group.size(); ++index) {
                const Entry* entry = group[index];
                const auto location_it = entry->attrs.find("LOCATION");
                const auto span_it = entry->attr_spans.find("LOCATION");
                const std::string pin_name =
                    (location_it != entry->attrs.end()) ? location_it->second : std::string();
                const Span span = (span_it != entry->attr_spans.end()) ? span_it->second
                                                                      : entry->port_span;
                result.push_back(MakeDiag(
                    "ADC-DUP-PIN", kError, span,
                    "Pin " + pin_name + " is used more than once (already assigned to " +
                        first->port + " at line " + std::to_string(first->line + 1) + ")"));
            }
        }
    }

    // ---- 缺失的总线 bit（BIT-8123）----
    if (check_ports) {
        const CoverageReport coverage = Coverage(entries, ports);
        for (const Port* port : ordered.ports) {
            const auto missing_it = coverage.missing.find(port->name);
            if (missing_it == coverage.missing.end()) continue;
            const std::vector<int> bits = PortBits(*port);
            if (bits.empty()) continue;  // 只有总线 bit 才触发 BIT-8123
            for (const int bit : missing_it->second) {
                const int decl_line = port->line;
                const int decl_char = port->character;
                Diagnostic diagnostic = MakeDiag(
                    "BIT-8123", kError,
                    RangeOf(decl_line, decl_char, decl_char + static_cast<int>(port->name.size())),
                    "Bus bit " + port->name + "[" + std::to_string(bit) +
                        "] has no pin assignment; bitgen will reject it (BIT-8123)");
                diagnostic.WithData("port", Json(port->name));
                diagnostic.WithData("bit", Json(bit));
                result.push_back(std::move(diagnostic));
            }
        }
    }

    // ---- 覆盖率摘要（Python 版没有；本工程新增，见头注释第 3 条）----
    if (options.include_coverage) {
        const CoverageReport coverage = Coverage(entries, ports);
        if (auto summary = CoverageDiagnostic(coverage, options.top)) {
            result.push_back(std::move(*summary));
        }
    }

    SortDiagnostics(result);
    return result;
}

std::optional<Diagnostic> CoverageDiagnostic(const CoverageReport& coverage,
                                             std::string_view top) {
    // 没有顶层端口表，或全部约束完毕 -> 不产生任何噪音
    if (coverage.total_bits <= 0) return std::nullopt;
    if (coverage.unconstrained_ports.empty() && coverage.missing.empty() &&
        coverage.constrained_bits >= coverage.total_bits) {
        return std::nullopt;
    }

    const double percent = 100.0 * coverage.constrained_bits / coverage.total_bits;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", percent);

    std::string message = "ADC coverage: " + std::to_string(coverage.constrained_bits) + "/" +
                          std::to_string(coverage.total_bits) + " bits constrained (" + buffer +
                          "%)";

    // 只列前 8 项：消息要能一眼看完，这是"不许刷屏"这条原则的延伸。
    constexpr size_t kMaxListed = 8;
    std::vector<std::string> items;
    for (const std::string& name : coverage.unconstrained_ports) {
        items.push_back(name);
    }
    for (const auto& [name, bits] : coverage.missing) {
        if (std::find(coverage.unconstrained_ports.begin(), coverage.unconstrained_ports.end(),
                      name) != coverage.unconstrained_ports.end()) {
            continue;
        }
        for (const int bit : bits) {
            items.push_back(name + "[" + std::to_string(bit) + "]");
        }
    }
    if (!items.empty()) {
        message += "; unconstrained: ";
        for (size_t i = 0; i < items.size() && i < kMaxListed; ++i) {
            if (i != 0) message += ", ";
            message += items[i];
        }
        if (items.size() > kMaxListed) {
            message += " and " + std::to_string(items.size() - kMaxListed) + " more";
        }
    }
    if (!top.empty()) {
        message += " (top module " + std::string(top) + ")";
    }

    // severity 恒为 Information：绝不能是 warning，否则未约束完的工程会刷屏。
    Diagnostic diagnostic = MakeDiag("ADC-COVERAGE", kInfo, RangeOf(0, 0, 1), std::move(message));
    diagnostic.WithData("total_bits", Json(coverage.total_bits));
    diagnostic.WithData("constrained_bits", Json(coverage.constrained_bits));
    Json unconstrained = Json::array();
    for (const std::string& name : coverage.unconstrained_ports) unconstrained.push_back(name);
    diagnostic.WithData("unconstrained_ports", std::move(unconstrained));
    return diagnostic;
}

// =============================================================================
//  光标辅助
// =============================================================================

std::optional<CursorContext> Context(std::string_view text, int line, int character) {
    if (line < 0 || character < 0) return std::nullopt;
    const std::vector<std::string_view> lines = text::SplitLines(text);
    if (static_cast<size_t>(line) >= lines.size()) return std::nullopt;
    const std::string_view raw = lines[static_cast<size_t>(line)];
    const std::optional<Entry> entry = ParseLine(raw, line);
    if (!entry.has_value()) return std::nullopt;

    // Python: if _in_span(command_span, character) or character <= command_span["end"]
    if (InSpan(entry->command_span, character) || character <= entry->command_span.end) {
        return CursorContext{"command", entry->command_span};
    }

    const std::vector<BraceGroup> groups = BraceGroups(raw);
    const BraceGroup* port_group = nullptr;
    const BraceGroup* attr_group = nullptr;
    if (groups.size() >= 2) {
        port_group = &groups[0];
        attr_group = &groups[1];
    } else if (groups.size() == 1) {
        const std::string_view content =
            raw.substr(static_cast<size_t>(groups[0].content_start),
                       static_cast<size_t>(groups[0].content_end - groups[0].content_start));
        if (content.find('=') != std::string_view::npos) {
            attr_group = &groups[0];
        } else {
            port_group = &groups[0];
        }
    }

    if (port_group != nullptr && port_group->brace_start <= character &&
        character <= port_group->brace_end) {
        Span span = entry->port_span;
        // 语义上的端口是**基名**：`w[0]` 让 hover/definition 去解析 `w`
        if (!entry->base.empty() &&
            span.start >= 0 &&
            static_cast<size_t>(span.start) + entry->base.size() <= raw.size() &&
            raw.substr(static_cast<size_t>(span.start), entry->base.size()) == entry->base) {
            span.end = span.start + static_cast<int>(entry->base.size());
        }
        return CursorContext{"port", span};
    }

    const auto value_kind = [](const std::string& key) -> const char* {
        if (key == "LOCATION") return "pin";
        if (key == "IOSTANDARD") return "iostandard";
        if (key == "PULLTYPE") return "pulltype";
        return nullptr;
    };

    for (const AttrItem& item : entry->attr_items) {
        const std::string key = item.key_upper.empty() ? UpperAscii(item.key) : item.key_upper;
        if (InSpan(item.value_span, character)) {
            if (const char* kind = value_kind(key)) return CursorContext{kind, item.value_span};
            return std::nullopt;
        }
        if (InSpan(item.key_span, character)) return CursorContext{"attr_key", item.key_span};
    }

    (void)attr_group;
    // 属性组内部但不在键/值上 -> 无意义
    return std::nullopt;
}

std::optional<Word> WordAt(std::string_view text, int line, int character) {
    if (line < 0 || character < 0) return std::nullopt;
    const std::vector<std::string_view> lines = text::SplitLines(text);
    if (static_cast<size_t>(line) >= lines.size()) return std::nullopt;
    const std::string_view raw = lines[static_cast<size_t>(line)];
    const std::string_view stripped = Strip(raw);
    if (stripped.empty()) return std::nullopt;
    if (stripped.front() == '#') return std::nullopt;
    if (stripped.size() >= 2 && stripped[0] == '/' && stripped[1] == '/') return std::nullopt;

    // 与 Python 的 re.finditer 逐 token 对应：
    //   交替式  [A-Za-z_][A-Za-z0-9_$]*(?:\[[^\]\s]*\])?  |  \d+(?:\.\d+)?
    // 手写扫描的理由同 ParseAttrs（re2 变参拿不到 0 捕获组的匹配文本，
    // 而这里每个候选词都要落到列号上）。
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t position = 0;
    while (position < raw.size()) {
        const char current = raw[position];
        size_t match_start = position;
        size_t match_end = position;

        const std::string_view identifier = text::LeadingIdentifier(raw.substr(position));
        if (!identifier.empty()) {
            match_end = position + identifier.size();
            // 可选位选择 `[ ... ]`：内部不能出现 ']' 或空白
            if (match_end < raw.size() && raw[match_end] == '[') {
                size_t cursor = match_end + 1;
                bool closed = false;
                while (cursor < raw.size()) {
                    if (raw[cursor] == ']') {
                        closed = true;
                        break;
                    }
                    if (IsSpace(raw[cursor])) break;
                    ++cursor;
                }
                if (closed) match_end = cursor + 1;
            }
        } else if (is_digit(current)) {
            match_end = position + 1;
            while (match_end < raw.size() && is_digit(raw[match_end])) ++match_end;
            if (match_end + 1 < raw.size() && raw[match_end] == '.' &&
                is_digit(raw[match_end + 1])) {
                match_end += 2;
                while (match_end < raw.size() && is_digit(raw[match_end])) ++match_end;
            }
        } else {
            ++position;
            continue;
        }

        // Python: start <= character <= end（end 是半开右端，但比较用闭区间）
        if (match_start <= static_cast<size_t>(character) &&
            static_cast<size_t>(character) <= match_end) {
            return Word{std::string(raw.substr(match_start, match_end - match_start)), line,
                        static_cast<int>(match_start), static_cast<int>(match_end)};
        }
        position = match_end;
    }
    return std::nullopt;
}

// =============================================================================
//  JSON 序列化
// =============================================================================

namespace {

Json SpanToJson(const Span& span) {
    Json json = Json::object();
    json["line"] = span.line;
    json["start"] = span.start;
    json["end"] = span.end;
    return json;
}

}  // namespace

Json ToJson(const Entry& entry) {
    Json json = Json::object();
    json["line"] = entry.line;
    json["character"] = entry.character;
    json["raw"] = entry.raw;
    json["command"] = entry.command;
    json["port"] = entry.port;
    json["base"] = entry.base;
    json["bit"] = entry.bit.has_value() ? Json(*entry.bit) : Json(nullptr);

    Json attrs = Json::object();
    for (const auto& [key, value] : entry.attrs) attrs[key] = value;
    json["attrs"] = std::move(attrs);

    Json attr_spans = Json::object();
    for (const auto& [key, span] : entry.attr_spans) attr_spans[key] = SpanToJson(span);
    json["attr_spans"] = std::move(attr_spans);

    json["port_span"] = SpanToJson(entry.port_span);
    json["command_span"] = SpanToJson(entry.command_span);

    Json items = Json::array();
    for (const AttrItem& item : entry.attr_items) {
        Json item_json = Json::object();
        item_json["key"] = item.key;
        item_json["key_upper"] = item.key_upper;
        item_json["value"] = item.value;
        item_json["key_span"] = SpanToJson(item.key_span);
        item_json["value_span"] = SpanToJson(item.value_span);
        items.push_back(std::move(item_json));
    }
    json["attr_items"] = std::move(items);
    return json;
}

Json ToJson(const std::vector<Entry>& entries) {
    Json json = Json::array();
    for (const Entry& entry : entries) json.push_back(ToJson(entry));
    return json;
}

Json ToJson(const CoverageReport& coverage) {
    Json json = Json::object();

    Json covered = Json::object();
    for (const auto& [name, bits] : coverage.covered) covered[name] = bits;
    json["covered"] = std::move(covered);

    Json missing = Json::object();
    for (const auto& [name, bits] : coverage.missing) missing[name] = bits;
    json["missing"] = std::move(missing);

    Json unconstrained = Json::array();
    for (const std::string& name : coverage.unconstrained_ports) unconstrained.push_back(name);
    json["unconstrained_ports"] = std::move(unconstrained);

    Json extra = Json::array();
    for (const Entry& entry : coverage.extra_ports) extra.push_back(ToJson(entry));
    json["extra_ports"] = std::move(extra);

    Json duplicate_ports = Json::array();
    for (const DuplicatePort& duplicate : coverage.duplicate_ports) {
        Json item = Json::object();
        item["port"] = duplicate.port;
        item["lines"] = duplicate.lines;
        duplicate_ports.push_back(std::move(item));
    }
    json["duplicate_ports"] = std::move(duplicate_ports);

    Json duplicate_pins = Json::array();
    for (const DuplicatePin& duplicate : coverage.duplicate_pins) {
        Json item = Json::object();
        item["pin"] = duplicate.pin;
        item["ports"] = duplicate.ports;
        item["lines"] = duplicate.lines;
        duplicate_pins.push_back(std::move(item));
    }
    json["duplicate_pins"] = std::move(duplicate_pins);

    json["total_bits"] = coverage.total_bits;
    json["constrained_bits"] = coverage.constrained_bits;
    return json;
}

}  // namespace adc
}  // namespace fpgalsp::constraints