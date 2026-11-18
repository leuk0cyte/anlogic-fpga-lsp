// =============================================================================
//  src/reports/area.cpp —— TD Area 报告解析
// =============================================================================
//  逐行对照 Y:\fpga-lsp\fpgalsp\reports.py（298-437 行）实现。
// =============================================================================

#include "reports/area.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "reports/report_patterns_internal.h"
#include "reports/report_text.h"

namespace fpgalsp::reports {
namespace {

// 与 Python 完全相同的四个模式
constexpr std::string_view kUtilPattern =
    R"(^#([A-Za-z][A-Za-z0-9_ *]*?)\s+(\d+)\s+out of\s+(\d+)\s+([\d.]+)\s*%)";
constexpr std::string_view kIoCountPattern = R"(^#(input|output|inout)\s+(\d+)\s*$)";
constexpr std::string_view kLePattern = R"(^#le\s+(\d+)\s*$)";
constexpr std::string_view kGenericCountPattern = R"(^#([A-Za-z][A-Za-z0-9_]*)\s+(\d+)\s*$)";

struct PatternEntry {
    std::string_view pattern;
};

constexpr PatternEntry kPatterns[] = {
    {kUtilPattern},
    {kIoCountPattern},
    {kLePattern},
    {kGenericCountPattern},
};

// 等价 line.strip("|").split("|")
std::vector<std::string_view> SplitPipeCellsRaw(std::string_view line) {
    std::string_view body = line;
    while (!body.empty() && body.front() == '|') body.remove_prefix(1);
    while (!body.empty() && body.back() == '|') body.remove_suffix(1);

    std::vector<std::string_view> cells;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = body.find('|', start);
        if (pos == std::string_view::npos) {
            cells.push_back(body.substr(start));
            return cells;
        }
        cells.push_back(body.substr(start, pos - start));
        start = pos + 1;
    }
}

// 等价 [c.strip() for c in line.strip("|").split("|")]
std::vector<std::string_view> SplitPipeCells(std::string_view line) {
    std::vector<std::string_view> cells = SplitPipeCellsRaw(line);
    for (std::string_view& cell : cells) cell = Strip(cell);
    return cells;
}

// 等价 Python dict.setdefault(key, value)
void SetDefault(std::map<std::string, int>& map, const std::string& key, int value) {
    map.emplace(key, value);
}

}  // namespace

namespace detail {

bool ValidateAreaPatterns(std::string* error) {
    for (const PatternEntry& entry : kPatterns) {
        const RE2 probe{std::string(entry.pattern)};
        if (!probe.ok()) {
            if (error != nullptr) {
                *error = "reports/area: bad pattern '" + std::string(entry.pattern) +
                         "': " + probe.error();
            }
            return false;
        }
    }
    return true;
}

}  // namespace detail

std::string NormAreaResource(std::string_view name) { return NormResource(name); }

std::optional<int> AreaTotal(const std::map<std::string, int>& totals,
                             std::string_view resource) {
    // 与 Python _area_total 的别名表一致；顺序敏感（先试第一个别名）。
    static const std::map<std::string, std::vector<std::string>> kAliases = {
        {"lut", {"slice", "lut"}},
        {"logic_lut", {"slice", "lut"}},
        {"ripple", {"slice", "lut"}},
        {"seq", {"slice_reg", "reg"}},
        {"reg", {"slice_reg", "reg"}},
        {"bram", {"eram", "bram"}},
        {"eram", {"eram", "bram"}},
        {"dsp", {"dsp"}},
    };

    const std::string key(resource);
    std::vector<std::string> candidates;
    const auto alias = kAliases.find(key);
    if (alias != kAliases.end()) {
        candidates = alias->second;
    } else {
        candidates.push_back(key);
    }
    for (const std::string& candidate : candidates) {
        const auto found = totals.find(candidate);
        // Python 是 `if totals.get(key):` —— 0 也算"没有"
        if (found != totals.end() && found->second != 0) return found->second;
    }
    return std::nullopt;
}

jsonrpc::Json AreaReport::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();

    jsonrpc::Json io_json = jsonrpc::Json::object();
    for (const auto& [key, value] : io) io_json[key] = value;
    out["io"] = std::move(io_json);

    jsonrpc::Json rows_json = jsonrpc::Json::array();
    for (const AreaRow& row : rows) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["resource"] = row.resource;
        item["used"] = row.used;
        item["total"] = row.total;
        item["utilization"] = row.utilization;
        rows_json.push_back(std::move(item));
    }
    out["rows"] = std::move(rows_json);

    jsonrpc::Json counts_json = jsonrpc::Json::object();
    for (const auto& [key, value] : counts) counts_json[key] = value;
    out["counts"] = std::move(counts_json);

    jsonrpc::Json totals_json = jsonrpc::Json::object();
    for (const auto& [key, value] : totals) totals_json[key] = value;
    out["totals"] = std::move(totals_json);

    jsonrpc::Json modules_json = jsonrpc::Json::array();
    for (const AreaModule& module : modules) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = module.name;
        item["module"] = module.module;
        item["level"] = module.level;

        jsonrpc::Json resources_json = jsonrpc::Json::object();
        for (const auto& [key, value] : module.resources) resources_json[key] = value;
        item["resources"] = std::move(resources_json);

        jsonrpc::Json utilization_json = jsonrpc::Json::object();
        for (const auto& [key, value] : module.utilization) {
            if (value.has_value()) {
                utilization_json[key] = *value;
            } else {
                utilization_json[key] = nullptr;  // Python 的 None
            }
        }
        item["utilization"] = std::move(utilization_json);
        item["path"] = module.path;
        modules_json.push_back(std::move(item));
    }
    out["modules"] = std::move(modules_json);
    out["raw"] = raw;
    return out;
}

std::vector<AreaModule> ParseAreaHierarchy(std::string_view text,
                                           const std::map<std::string, int>& totals) {
    const std::vector<std::string_view> lines = SplitLines(text);

    std::size_t start = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("Report Hierarchy Area") != std::string_view::npos) {
            start = i;
            break;
        }
    }
    if (start >= lines.size()) return {};

    std::vector<std::string_view> header;
    std::vector<AreaModule> rows;

    for (std::size_t i = start; i < lines.size(); ++i) {
        const std::string_view stripped = Strip(lines[i]);
        if (!StartsWith(stripped, "|")) {
            if (!rows.empty()) break;
            continue;
        }
        const std::vector<std::string_view> cells = SplitPipeCells(stripped);
        if (cells.size() < 7) continue;
        if (EqualsIgnoreCase(cells[0], "instance")) {
            header.assign(cells.begin(), cells.end());
            continue;
        }
        if (header.empty()) continue;

        // 缩进只看 cells[0] 前面那一段原始空白（等价 Python 的 raw_instance）
        const std::vector<std::string_view> raw_cells = SplitPipeCellsRaw(stripped);
        const std::string_view raw_instance = raw_cells.empty() ? std::string_view{} : raw_cells[0];
        std::size_t indent = 0;
        while (indent < raw_instance.size() && raw_instance[indent] == ' ') ++indent;

        AreaModule row;
        row.name = std::string(cells[0]);
        row.module = cells.size() > 1 ? std::string(cells[1]) : std::string{};
        row.level = static_cast<int>(indent / 2);

        // 保留表头给出的每一列，不假设列名集合（PH1 有 le/logic lut/eram，EF2 只有 lut/seq）
        for (std::size_t c = 2; c < header.size() && c < cells.size(); ++c) {
            const std::string key = NormResource(header[c]);
            int value = 0;
            (void)ParseInt32(cells[c], value);
            row.resources[key] = value;
        }
        // 把 PH1 的名字别名到 webview 已经认识的 lut / bram
        if (const auto it = row.resources.find("logic_lut"); it != row.resources.end()) {
            SetDefault(row.resources, "lut", it->second);
        }
        if (const auto it = row.resources.find("eram"); it != row.resources.end()) {
            SetDefault(row.resources, "bram", it->second);
        }
        for (const auto& [key, used] : row.resources) {
            const std::optional<int> total = AreaTotal(totals, key);
            if (total.has_value() && *total != 0) {
                row.utilization[key] = Round2(static_cast<double>(used) * 100.0 /
                                              static_cast<double>(*total));
            } else {
                row.utilization[key] = std::nullopt;
            }
        }
        rows.push_back(std::move(row));
    }

    // 由 level 栈拼完整层次路径（等价 Python 的 stack = stack[:level] + [name]）
    std::vector<std::string> stack;
    for (AreaModule& row : rows) {
        const std::size_t level = static_cast<std::size_t>(row.level < 0 ? 0 : row.level);
        if (stack.size() > level) stack.resize(level);
        stack.push_back(row.name);
        std::string path;
        for (const std::string& part : stack) {
            if (!path.empty()) path.push_back('/');
            path += part;
        }
        row.path = std::move(path);
    }
    return rows;
}

AreaReport ParseArea(std::string_view text) {
    AreaReport report;
    report.raw = std::string(text);
    if (text.empty()) {
        report.warnings.emplace_back("empty-report");
        return report;
    }

    static const RE2 kUtilRe{std::string(kUtilPattern)};
    static const RE2 kIoCountRe{std::string(kIoCountPattern)};
    static const RE2 kLeRe{std::string(kLePattern)};
    static const RE2 kGenericCountRe{std::string(kGenericCountPattern)};

    std::string section;
    bool saw_utilization = false;

    for (const std::string_view raw_line : SplitLines(text)) {
        const std::string_view stripped = Strip(raw_line);
        if (EndsWith(stripped, "Statistics") &&
            StartsWithAny(stripped, {"IO", "LUT", "Utilization"})) {
            section = std::string(stripped);
            continue;
        }

        std::string name;
        std::string used;
        std::string total;
        std::string percent;
        re2::StringPiece input(stripped);
        if (RE2::Consume(&input, kUtilRe, &name, &used, &total, &percent)) {
            AreaRow row;
            row.resource = "#" + name;
            (void)ParseInt32(used, row.used);
            (void)ParseInt32(total, row.total);
            row.utilization = percent + "%";
            // 同一个资源名出现多次时后者覆盖前者（Python dict 语义）
            report.totals[NormResource(name)] = row.total;
            report.rows.push_back(std::move(row));
            saw_utilization = true;
            continue;
        }

        std::string io_name;
        std::string io_count;
        if (RE2::FullMatch(stripped, kIoCountRe, &io_name, &io_count)) {
            int value = 0;
            (void)ParseInt32(io_count, value);
            report.io[io_name] = value;
            continue;
        }

        std::string le_count;
        if (RE2::FullMatch(stripped, kLeRe, &le_count)) {
            int value = 0;
            (void)ParseInt32(le_count, value);
            report.io["le"] = value;
            continue;
        }

        std::string count_name;
        std::string count_value;
        if (StartsWith(section, "LUT") &&
            RE2::FullMatch(stripped, kGenericCountRe, &count_name, &count_value)) {
            int value = 0;
            (void)ParseInt32(count_value, value);
            report.counts[count_name] = value;
        }
    }

    report.modules = ParseAreaHierarchy(text, report.totals);

    if (!saw_utilization) report.warnings.emplace_back("no-utilization-rows");
    if (text.find("Report Hierarchy Area") == std::string_view::npos) {
        report.warnings.emplace_back("no-hierarchy-table");
    }
    return report;
}

}  // namespace fpgalsp::reports