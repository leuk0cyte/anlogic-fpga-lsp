// =============================================================================
//  src/reports/timing.cpp —— TD 时序报告解析
// =============================================================================
//  逐行对照 Y:\fpga-lsp\fpgalsp\reports.py（102-294 行）实现。
//  端点映射对照 Y:\fpga-lsp\fpgalsp\timing_map.py。
//
//  ⚠ 移植时刻意保留的 Python 行为（含看起来像 bug 的地方，见 tests/PYTHON-BUGS.md）
//    1. Statistics 行的正则要求 "Max|Min : SWNS"，而真实报告打的是
//       "Min  : HWNS ..."，所以 statistics["min"] **永远缺失**。
//       （P-8）
//    2. metadata 只在第一处 "Check Status" 之前采集；续行必须是行首冒号。
//    3. clock_detail 要求 >= 16 个空白分隔字段，列数少的变体会被整表跳过。
//    4. legacy 兜底只在 groups 里一条 path 都没有时才启用。
// =============================================================================

#include "reports/timing.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "reports/report_patterns_internal.h"
#include "reports/report_text.h"

namespace fpgalsp::reports {
namespace {

// ---- 与 Python 一字不差的模式表 ----
constexpr std::string_view kSummaryPattern =
    R"((SWNS|STNS|HWNS|HTNS|Period Check WNS)\s*:\s*(-?\d+(?:\.\d+)?)\s*ns)";
constexpr std::string_view kCoveragePattern = R"(STA coverage\s*:\s*([\d.]+)%)";
constexpr std::string_view kConfidencePattern = R"(Confidence\s*:\s*(\S.*))";
constexpr std::string_view kClockSectionPattern = R"(^\s+(clk\d+):\s+([\d.]+)\s+(\d+)\s+(\S.*)$)";
constexpr std::string_view kMetadataPattern = R"(^([A-Za-z][A-Za-z0-9 /_().-]{2,40}?)\s*:\s*(.+)$)";
constexpr std::string_view kCheckStatusPattern = R"(^(Slow|Fast)\s*\|\s*(\S+)\s+(\S+))";
constexpr std::string_view kPathGroupPattern = R"(^Path Group\s*:\s*(.+)$)";
constexpr std::string_view kGroupMetaPattern = R"(^(Type|From Clock|To Clock|Min Period|Fmax)\s*:\s*(.*)$)";
constexpr std::string_view kStatisticsPattern =
    R"(^(Max|Min)\s*:\s*SWNS\s+(-?[\d.]+)ns,\s*STNS\s+(-?[\d.]+)ns,\s*(\d+)\s+Viol Endpoints,\s*(\d+)\s+Total Endpoints)";
constexpr std::string_view kSlackPattern = R"(^Slack\s*:\s*(-?[\d.]+)ns)";
constexpr std::string_view kDetailPattern =
    R"(^(Begin Point|End Point|Check Type|Process|Budget|Data Path Delay|Clock Skew|Logic Level|Max Fanout|Clock Uncertainty)\s*:\s*(.*)$)";
// legacy 兜底用
constexpr std::string_view kLegacySlackPattern = R"(Slack\s*:\s*(-?\d+(?:\.\d+)?)\s*ns)";
// timing_map.py
constexpr std::string_view kTdSuffixPattern = R"(_(?:syn|phy|ret|map)_\d+$)";
constexpr std::string_view kPinSuffixPattern = R"(\.[A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?$)";
constexpr std::string_view kBitSuffixPattern = R"(\[\d+\]$)";

struct PatternEntry {
    std::string_view name;
    std::string_view pattern;
};

constexpr PatternEntry kPatterns[] = {
    {"summary", kSummaryPattern},
    {"coverage", kCoveragePattern},
    {"confidence", kConfidencePattern},
    {"clock_section", kClockSectionPattern},
    {"metadata", kMetadataPattern},
    {"check_status", kCheckStatusPattern},
    {"path_group", kPathGroupPattern},
    {"group_meta", kGroupMetaPattern},
    {"statistics", kStatisticsPattern},
    {"slack", kSlackPattern},
    {"detail", kDetailPattern},
    {"legacy_slack", kLegacySlackPattern},
    {"td_suffix", kTdSuffixPattern},
    {"pin_suffix", kPinSuffixPattern},
    {"bit_suffix", kBitSuffixPattern},
};

// clock_detail 的 17 个列名（顺序固定，与 Python 的 keys 列表一致）
constexpr std::string_view kClockDetailKeys[] = {
    "name",        "type",       "rise_ns",    "fall_ns",   "c_period_ns", "c_freq_mhz",
    "r_period_ns", "r_freq_mhz", "swns_ns",    "stns_ns",   "hwns_ns",     "htns_ns",
    "skew_ns",     "fanout",     "max_level",  "cc_from",   "cc_to",
};

// 等价 Python dict 的 `d[key] = value`（已存在则原位覆盖，保持插入顺序）。
// 返回该项在 vector 里的下标（metadata 续行需要原地追加，不能只看 back()）。
std::size_t SetField(std::vector<std::pair<std::string, std::string>>& fields,
                     const std::string& key, std::string value) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].first == key) {
            fields[i].second = std::move(value);
            return i;
        }
    }
    fields.emplace_back(key, std::move(value));
    return fields.size() - 1;
}

std::string ReplaceSpacesWithUnderscores(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == ' ') c = '_';
    }
    return out;
}

std::string Truncate(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);
    return std::string(text.substr(0, limit));
}

// 去掉位选后缀 [n]
std::string StripBitSuffix(std::string_view text) {
    static const RE2 kBitRe{std::string(kBitSuffixPattern)};
    std::string out(text);
    (void)RE2::Replace(&out, kBitRe, "");
    return out;
}

// -----------------------------------------------------------------------------
// 各分段解析
// -----------------------------------------------------------------------------

void ParseMetadata(const std::vector<std::string_view>& lines,
                   std::vector<std::pair<std::string, std::string>>& metadata) {
    static const RE2 kMetadataRe{std::string(kMetadataPattern)};

    std::size_t last_index = 0;
    bool has_last = false;
    for (const std::string_view raw_line : lines) {
        const std::string_view stripped = Strip(raw_line);
        if (stripped.empty()) continue;
        if (StartsWith(stripped, "Check Status")) break;

        if (StartsWith(stripped, ":") && has_last) {
            // 续行：`                    : Copyright (c) ...` 追加到上一条的值
            // Python: metadata[last] = (metadata.get(last) or "") + " " + stripped[1:].strip()
            std::string& value = metadata[last_index].second;
            value += " ";
            value += Strip(stripped.substr(1));
            continue;
        }

        std::string key;
        std::string value;
        if (RE2::FullMatch(stripped, kMetadataRe, &key, &value)) {
            last_index = SetField(metadata, key, value);
            has_last = true;
        }
    }
}

void ParseCheckStatus(const std::vector<std::string_view>& lines,
                      std::vector<TimingCheckStatusRow>& rows) {
    static const RE2 kCheckStatusRe{std::string(kCheckStatusPattern)};

    bool inside = false;
    for (const std::string_view raw_line : lines) {
        const std::string_view stripped = Strip(raw_line);
        if (StartsWith(stripped, "Check Status")) {
            inside = true;
            continue;
        }
        if (inside && StartsWith(stripped, "Timing Status")) break;
        if (!inside) continue;

        TimingCheckStatusRow row;
        if (RE2::FullMatch(stripped, kCheckStatusRe, &row.corner, &row.max, &row.min)) {
            rows.push_back(std::move(row));
        }
    }
}

void ParseClockDetail(const std::vector<std::string_view>& lines,
                      std::vector<TimingClockDetail>& detail) {
    bool have_header = false;
    for (const std::string_view raw_line : lines) {
        const std::string_view stripped = Strip(raw_line);
        if (!have_header) {
            if (stripped.find("Name") != std::string_view::npos &&
                stripped.find("Type") != std::string_view::npos &&
                stripped.find("Rise(ns)") != std::string_view::npos) {
                have_header = true;
            }
            continue;
        }
        if (stripped.empty() ||
            StartsWithAny(stripped, {"Notes", "Intra clock", "Inter clock", "*"})) {
            if (!detail.empty()) break;
            continue;
        }
        const std::vector<std::string> parts = SplitWhitespace(stripped);
        if (parts.size() < 16) continue;

        TimingClockDetail entry;
        const std::size_t count = std::min(parts.size(), std::size(kClockDetailKeys));
        entry.fields.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            entry.fields.emplace_back(std::string(kClockDetailKeys[i]), parts[i]);
        }
        detail.push_back(std::move(entry));
    }
}

std::vector<TimingGroup> ParseGroups(const std::vector<std::string_view>& lines) {
    static const RE2 kPathGroupRe{std::string(kPathGroupPattern)};
    static const RE2 kGroupMetaRe{std::string(kGroupMetaPattern)};
    static const RE2 kStatisticsRe{std::string(kStatisticsPattern)};
    static const RE2 kSlackRe{std::string(kSlackPattern)};
    static const RE2 kDetailRe{std::string(kDetailPattern)};

    std::vector<TimingGroup> groups;
    std::optional<TimingGroup> current;
    std::optional<TimingPath> path;
    bool delay_header = false;

    const auto finish_path = [&]() {
        if (!path.has_value()) return;
        delay_header = false;
        if (current.has_value()) current->paths.push_back(std::move(*path));
        path.reset();
    };

    for (const std::string_view raw_line : lines) {
        const std::string_view stripped = Strip(raw_line);

        std::string group_name;
        if (RE2::FullMatch(stripped, kPathGroupRe, &group_name)) {
            finish_path();
            if (current.has_value()) groups.push_back(std::move(*current));
            current.emplace();
            current->name = std::move(group_name);
            continue;
        }
        if (!current.has_value()) continue;

        std::string label;
        std::string value;
        if (RE2::FullMatch(stripped, kGroupMetaRe, &label, &value)) {
            if (label == "Type") {
                current->type = value;
            } else if (label == "From Clock") {
                current->from_clock = value;
            } else if (label == "To Clock") {
                current->to_clock = value;
            } else if (label == "Min Period") {
                current->min_period = value;
            } else if (label == "Fmax") {
                current->fmax = value;
            }
        }

        std::string stat_kind;
        std::string stat_swns;
        std::string stat_stns;
        std::string stat_viol;
        std::string stat_total;
        if (RE2::FullMatch(stripped, kStatisticsRe, &stat_kind, &stat_swns, &stat_stns,
                           &stat_viol, &stat_total)) {
            TimingStatistics stats;
            bool ok = false;
            stats.swns_ns = ParseDouble(stat_swns, ok, 0.0);
            stats.stns_ns = ParseDouble(stat_stns, ok, 0.0);
            (void)ParseInt32(stat_viol, stats.viol_endpoints);
            (void)ParseInt32(stat_total, stats.total_endpoints);
            if (ToLowerAscii(stat_kind) == "max") {
                current->max_statistics = stats;
            } else {
                current->min_statistics = stats;
            }
        }

        std::string slack_text;
        if (RE2::FullMatch(stripped, kSlackRe, &slack_text)) {
            finish_path();
            bool ok = false;
            TimingPath entry;
            entry.slack = ParseDouble(slack_text, ok, 0.0);
            path.emplace(std::move(entry));
            delay_header = false;
            continue;
        }

        if (!path.has_value()) continue;

        if (StartsWith(stripped, "Delay Type")) {
            delay_header = true;
            continue;
        }
        if (delay_header) {
            if (stripped.empty()) {
                delay_header = false;
                continue;
            }
            if (StartsWithAny(stripped, {"Max Paths", "Min Paths", "Path Group", "Statistics"})) {
                delay_header = false;
                continue;
            }
            path->delay.emplace_back(RStrip(raw_line));
            continue;
        }

        std::string detail_label;
        std::string detail_value;
        if (RE2::FullMatch(stripped, kDetailRe, &detail_label, &detail_value)) {
            const std::string trimmed = std::string(Strip(detail_value));
            if (detail_label == "Begin Point") {
                path->begin = Truncate(trimmed, 240);
            } else if (detail_label == "End Point") {
                path->end = Truncate(trimmed, 240);
            } else if (detail_label == "Check Type") {
                path->check = trimmed;
            } else if (detail_label == "Process") {
                path->process = trimmed;
            }
            SetField(path->details, detail_label, trimmed);
        }
    }

    finish_path();
    if (current.has_value()) groups.push_back(std::move(*current));
    return groups;
}

// 报告里没有 Path Group 段时的兜底：直接扫 "Slack : x ns" 行 + 后面 6 行里的
// Begin/End/Check/Process。对应 Python parse_timing 末尾的 legacy 分支。
std::vector<TimingPath> ParseLegacyPaths(const std::vector<std::string_view>& lines) {
    static const RE2 kLegacySlackRe{std::string(kLegacySlackPattern)};

    std::vector<TimingPath> paths;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string_view line = lines[index];
        if (!StartsWith(line, "Slack")) continue;
        if (line.find(':') == std::string_view::npos) continue;

        std::string slack_text;
        if (!RE2::PartialMatch(line, kLegacySlackRe, &slack_text)) continue;

        TimingPath entry;
        entry.legacy = true;
        bool ok = false;
        entry.slack = ParseDouble(slack_text, ok, 0.0);

        for (std::size_t offset = 1; offset <= 6; ++offset) {
            if (index + offset >= lines.size()) break;
            const std::string_view row = lines[index + offset];
            const std::string_view stripped_row = Strip(row);
            const std::size_t colon = row.find(':');
            if (colon == std::string_view::npos) continue;
            const std::string_view tail = Strip(row.substr(colon + 1));
            if (StartsWith(stripped_row, "Begin Point")) {
                entry.begin = Truncate(tail, 180);
            } else if (StartsWith(stripped_row, "End Point")) {
                entry.end = Truncate(tail, 180);
            } else if (StartsWith(stripped_row, "Check Type")) {
                entry.check = Truncate(tail, 180);
            } else if (StartsWith(stripped_row, "Process")) {
                entry.process = Truncate(tail, 180);
            }
        }
        if (!entry.begin.empty() || !entry.end.empty()) paths.push_back(std::move(entry));
        if (paths.size() >= 50) break;
    }
    return paths;
}

}  // namespace

namespace detail {

bool ValidateTimingPatterns(std::string* error) {
    for (const PatternEntry& entry : kPatterns) {
        const RE2 probe{std::string(entry.pattern)};
        if (!probe.ok()) {
            if (error != nullptr) {
                *error = "reports/timing: bad pattern '" + std::string(entry.name) + "' (" +
                         std::string(entry.pattern) + "): " + probe.error();
            }
            return false;
        }
    }
    return true;
}

}  // namespace detail

// -----------------------------------------------------------------------------
// 端点映射
// -----------------------------------------------------------------------------

jsonrpc::Json TimingEndpoint::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["signal"] = signal;
    jsonrpc::Json hierarchy_json = jsonrpc::Json::array();
    for (const std::string& part : hierarchy) hierarchy_json.push_back(part);
    out["hierarchy"] = std::move(hierarchy_json);
    out["file"] = file.has_value() ? jsonrpc::Json(*file) : jsonrpc::Json(nullptr);
    out["line"] = line.has_value() ? jsonrpc::Json(*line) : jsonrpc::Json(nullptr);
    out["character"] = character.has_value() ? jsonrpc::Json(*character) : jsonrpc::Json(nullptr);
    out["resolved"] = resolved;
    out["module"] = module.has_value() ? jsonrpc::Json(*module) : jsonrpc::Json(nullptr);
    out["reason"] = reason.has_value() ? jsonrpc::Json(*reason) : jsonrpc::Json(nullptr);
    return out;
}

std::vector<std::string> CleanTimingPoint(std::string_view text) {
    static const RE2 kPinSuffixRe{std::string(kPinSuffixPattern)};
    static const RE2 kTdSuffixRe{std::string(kTdSuffixPattern)};

    // body = text.split("(")[0].strip()
    std::string_view body = text;
    if (const std::size_t paren = body.find('('); paren != std::string_view::npos) {
        body = body.substr(0, paren);
    }
    body = Strip(body);
    if (body.empty()) return {};

    std::string trimmed(body);
    (void)RE2::Replace(&trimmed, kPinSuffixRe, "");

    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t pos = trimmed.find('/', start);
        const std::string_view part =
            (pos == std::string::npos) ? std::string_view(trimmed).substr(start)
                                       : std::string_view(trimmed).substr(start, pos - start);
        if (!part.empty()) {
            std::string cleaned(part);
            (void)RE2::Replace(&cleaned, kTdSuffixRe, "");
            components.push_back(std::move(cleaned));
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return components;
}

void AttachEndpointLocations(TimingReport& report) {
    // TODO(阶段5 / HDL 索引)：接入索引后替换这里的降级分支。
    //   现在没有索引 -> 与 timing_map.resolve() 的 "no top module" 分支同形状，
    //   但 reason 明确写成 hdl-index-unavailable，方便前端区分"没索引"和"真找不到"。
    const auto map_point = [](const std::string& text) -> std::optional<TimingEndpoint> {
        if (text.empty()) return std::nullopt;  // Python: 空文本 -> None
        std::vector<std::string> components = CleanTimingPoint(text);
        if (components.empty()) return std::nullopt;

        TimingEndpoint point;
        point.signal = StripBitSuffix(components.back());
        point.hierarchy.assign(components.begin(), components.end() - 1);
        point.resolved = false;
        point.reason = "hdl-index-unavailable";
        return point;
    };

    int total = 0;
    int resolved = 0;
    for (TimingPath& path : report.paths) {
        path.begin_at = map_point(path.begin);
        path.end_at = map_point(path.end);
        total += 2;  // 每条路径两个端点（Python 的 summary 也是按 2*len(paths) 算）
        if (path.begin_at.has_value() && path.begin_at->resolved) ++resolved;
        if (path.end_at.has_value() && path.end_at->resolved) ++resolved;
    }
    // Python 的 summary() 只在 paths 非空时被调用（server.py 的 `if paths and ...`）
    if (!report.paths.empty()) report.begin_end_resolved = std::make_pair(total, resolved);
}

// -----------------------------------------------------------------------------
// JSON
// -----------------------------------------------------------------------------

jsonrpc::Json TimingStatistics::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["swns_ns"] = swns_ns;
    out["stns_ns"] = stns_ns;
    out["viol_endpoints"] = viol_endpoints;
    out["total_endpoints"] = total_endpoints;
    return out;
}

jsonrpc::Json TimingPath::ToJson(bool with_endpoints) const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["slack"] = slack;
    out["begin"] = begin;
    out["end"] = end;
    out["check"] = check;
    out["process"] = process;
    if (!legacy) {
        jsonrpc::Json details_json = jsonrpc::Json::object();
        for (const auto& [key, value] : details) details_json[key] = value;
        out["details"] = std::move(details_json);

        jsonrpc::Json delay_json = jsonrpc::Json::array();
        for (const std::string& entry : delay) delay_json.push_back(entry);
        out["delay"] = std::move(delay_json);

        if (group.has_value()) out["group"] = *group;
    }
    if (with_endpoints) {
        out["begin_at"] = begin_at.has_value() ? begin_at->ToJson() : jsonrpc::Json(nullptr);
        out["end_at"] = end_at.has_value() ? end_at->ToJson() : jsonrpc::Json(nullptr);
    }
    return out;
}

jsonrpc::Json TimingGroup::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["name"] = name;
    out["type"] = type;
    out["from_clock"] = from_clock;
    out["to_clock"] = to_clock;
    out["min_period"] = min_period;
    out["fmax"] = fmax;

    jsonrpc::Json statistics = jsonrpc::Json::object();
    if (max_statistics.has_value()) statistics["max"] = max_statistics->ToJson();
    if (min_statistics.has_value()) statistics["min"] = min_statistics->ToJson();
    out["statistics"] = std::move(statistics);

    jsonrpc::Json paths_json = jsonrpc::Json::array();
    for (const TimingPath& path : paths) paths_json.push_back(path.ToJson(false));
    out["paths"] = std::move(paths_json);
    return out;
}

jsonrpc::Json TimingClock::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["name"] = name;
    out["freq_mhz"] = freq_mhz;
    out["fanout"] = fanout;
    out["source"] = source;
    return out;
}

jsonrpc::Json TimingClockDetail::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    for (const auto& [key, value] : fields) out[key] = value;
    return out;
}

jsonrpc::Json TimingCheckStatusRow::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["corner"] = corner;
    out["max"] = max;
    out["min"] = min;
    return out;
}

jsonrpc::Json TimingReport::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();

    jsonrpc::Json summary_json = jsonrpc::Json::object();
    for (const auto& [key, value] : summary) summary_json[key] = value;
    out["summary"] = std::move(summary_json);

    out["coverage"] = coverage.has_value() ? jsonrpc::Json(*coverage) : jsonrpc::Json(nullptr);
    out["confidence"] =
        confidence.has_value() ? jsonrpc::Json(*confidence) : jsonrpc::Json(nullptr);

    jsonrpc::Json clocks_json = jsonrpc::Json::array();
    for (const TimingClock& clock : clocks) clocks_json.push_back(clock.ToJson());
    out["clocks"] = std::move(clocks_json);

    jsonrpc::Json detail_json = jsonrpc::Json::array();
    for (const TimingClockDetail& entry : clock_detail) detail_json.push_back(entry.ToJson());
    out["clock_detail"] = std::move(detail_json);

    jsonrpc::Json paths_json = jsonrpc::Json::array();
    for (const TimingPath& path : paths) paths_json.push_back(path.ToJson(true));
    out["paths"] = std::move(paths_json);

    jsonrpc::Json metadata_json = jsonrpc::Json::object();
    for (const auto& [key, value] : metadata) metadata_json[key] = value;
    out["metadata"] = std::move(metadata_json);

    jsonrpc::Json check_json = jsonrpc::Json::array();
    for (const TimingCheckStatusRow& row : check_status) check_json.push_back(row.ToJson());
    out["check_status"] = std::move(check_json);

    jsonrpc::Json groups_json = jsonrpc::Json::array();
    for (const TimingGroup& group : groups) groups_json.push_back(group.ToJson());
    out["groups"] = std::move(groups_json);

    out["raw"] = raw;
    out["file"] = file;

    if (begin_end_resolved.has_value()) {
        jsonrpc::Json resolved_json = jsonrpc::Json::object();
        resolved_json["total"] = begin_end_resolved->first;
        resolved_json["resolved"] = begin_end_resolved->second;
        out["begin_end_resolved"] = std::move(resolved_json);
    }
    return out;
}

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------

TimingReport ParseTiming(std::string_view text, std::string_view file) {
    static const RE2 kSummaryRe{std::string(kSummaryPattern)};
    static const RE2 kCoverageRe{std::string(kCoveragePattern)};
    static const RE2 kConfidenceRe{std::string(kConfidencePattern)};
    static const RE2 kClockSectionRe{std::string(kClockSectionPattern)};

    TimingReport report;
    report.file = std::string(file);
    report.raw = std::string(text);
    if (text.empty()) {
        report.warnings.emplace_back("empty-report");
        return report;
    }

    const re2::StringPiece whole(text.data(), text.size());

    // ---- summary：同样的 key 出现多次时后者覆盖前者（Python dict 语义）----
    {
        re2::StringPiece input = whole;
        std::string key;
        std::string value;
        while (RE2::FindAndConsume(&input, kSummaryRe, &key, &value)) {
            bool ok = false;
            const double parsed = ParseDouble(value, ok, 0.0);
            if (!ok) continue;
            report.summary[ReplaceSpacesWithUnderscores(key)] = parsed;
        }
    }

    {
        std::string value;
        if (RE2::PartialMatch(whole, kCoverageRe, &value)) {
            bool ok = false;
            const double parsed = ParseDouble(value, ok, 0.0);
            if (ok) report.coverage = parsed;
        }
    }
    {
        std::string value;
        if (RE2::PartialMatch(whole, kConfidenceRe, &value)) {
            const std::string trimmed(Strip(value));
            if (!trimmed.empty()) report.confidence = trimmed;
        }
    }

    const std::vector<std::string_view> lines = SplitLines(text);

    // ---- clocks（注意需要行首空白，表头行不会被误匹配）----
    for (const std::string_view raw_line : lines) {
        std::string name;
        std::string freq;
        std::string fanout;
        std::string source;
        if (!RE2::FullMatch(raw_line, kClockSectionRe, &name, &freq, &fanout, &source)) continue;
        TimingClock clock;
        clock.name = std::move(name);
        bool ok = false;
        clock.freq_mhz = ParseDouble(freq, ok, 0.0);
        (void)ParseInt32(fanout, clock.fanout);
        clock.source = std::string(Strip(source));
        report.clocks.push_back(std::move(clock));
    }

    ParseMetadata(lines, report.metadata);
    ParseCheckStatus(lines, report.check_status);
    ParseClockDetail(lines, report.clock_detail);
    report.groups = ParseGroups(lines);

    // ---- 展平 groups 里的 paths ----
    for (const TimingGroup& group : report.groups) {
        for (const TimingPath& entry : group.paths) {
            TimingPath copy = entry;  // Python 的 dict(entry) 浅拷贝；内容等价
            copy.group = group.name;
            report.paths.push_back(std::move(copy));
        }
    }

    // ---- legacy 兜底：报告里没有任何 Path Group ----
    if (report.paths.empty()) report.paths = ParseLegacyPaths(lines);

    AttachEndpointLocations(report);

    if (report.summary.empty()) report.warnings.emplace_back("no-timing-status");
    if (report.clocks.empty()) report.warnings.emplace_back("no-clock-summary");
    if (report.paths.empty()) report.warnings.emplace_back("no-paths");
    return report;
}

}  // namespace fpgalsp::reports
