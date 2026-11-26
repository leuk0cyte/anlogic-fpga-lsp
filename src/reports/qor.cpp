// =============================================================================
//  src/reports/qor.cpp —— TD QoR 报告解析
// =============================================================================
//  逐行对照 %PYREF%\fpgalsp\reports.py::parse_qor（44-98 行）实现。
//  ⚠ 两个循环刻意保持与 Python 完全相同的顺序与判定，包括：
//     * metrics 循环里 `key.lower() in ("qor metrics","value")` 的跳过规则
//     * suggestions 循环里"QOR 号起一条、后续同表行追加正文"的状态机
// =============================================================================

#include "reports/qor.h"

#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "reports/report_patterns_internal.h"
#include "reports/report_text.h"

#include <array>
namespace fpgalsp::reports {
namespace {

// 与 Python 一致保留第 1 个捕获组：RE2::PartialMatch(..., &id) 取的是组 1，
// 模式里没有捕获组时 re2 会报 "invalid argument" 而不是抛异常。
constexpr std::string_view kSuggestionPattern = R"((QOR-\d{3,5}))";
constexpr std::string_view kPriorityPattern = R"(\b(CRITICAL|HIGH|MEDIUM|LOW)\b)";

// 对应 line.strip("|").split("|")：先去首尾所有 '|'，再按 '|' 切分（不丢空段）
std::vector<std::string_view> SplitPipeCells(std::string_view line) {
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

std::string JoinBySpace(const std::vector<std::string>& parts) {
    std::string out;
    bool first = true;
    for (const std::string& part : parts) {
        if (!first) out.push_back(' ');
        first = false;
        out += part;
    }
    return out;
}

struct PatternEntry {
    std::string_view pattern;
};

constexpr auto kPatterns = std::to_array<PatternEntry>({
    {kSuggestionPattern},
    {kPriorityPattern},
});

}  // namespace

namespace detail {

bool ValidateQorPatterns(std::string* error) {
    return std::ranges::all_of(kPatterns, [error](const PatternEntry& entry) {
        const RE2 probe{std::string(entry.pattern)};
        if (probe.ok()) return true;
        if (error != nullptr) {
            *error = "reports/qor: bad pattern '" + std::string(entry.pattern) +
                     "': " + probe.error();
        }
        return false;
    });
}

}  // namespace detail

jsonrpc::Json QorReport::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["step"] = step;

    jsonrpc::Json metrics_json = jsonrpc::Json::array();
    for (const QorMetric& metric : metrics) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["group"] = metric.group;
        item["key"] = metric.key;
        item["value"] = metric.value;
        metrics_json.push_back(std::move(item));
    }
    out["metrics"] = std::move(metrics_json);

    jsonrpc::Json suggestions_json = jsonrpc::Json::array();
    for (const QorSuggestion& suggestion : suggestions) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["id"] = suggestion.id;
        item["priority"] = suggestion.priority;
        item["text"] = suggestion.text;
        // Python 的 parse_qor 条目没有 file 键，parse_suggestions_tcl 的条目才有。
        // 用非空判断复现这个差别，保证 JSON 逐字段一致。
        if (!suggestion.file.empty()) item["file"] = suggestion.file;
        suggestions_json.push_back(std::move(item));
    }
    out["suggestions"] = std::move(suggestions_json);
    return out;
}

QorReport ParseQor(std::string_view text, std::string_view step) {
    QorReport report;
    report.step = std::string(step);
    if (text.empty()) {
        report.warnings.emplace_back("empty-report");
        return report;
    }

    static const RE2 kSuggestionRe{std::string(kSuggestionPattern)};
    static const RE2 kPriorityRe{std::string(kPriorityPattern)};

    const std::vector<std::string_view> lines = SplitLines(text);

    // ---- 指标表 ----
    std::string group;
    bool saw_pipe_table = false;
    for (const std::string_view raw : lines) {
        const std::string_view line = Strip(raw);
        if (!StartsWith(line, "|")) continue;
        saw_pipe_table = true;

        const std::vector<std::string_view> cells = SplitPipeCells(line);
        if (cells.size() < 2) continue;

        const std::string_view key = Strip(cells[0]);
        const std::string_view value = Strip(cells[1]);
        if (key.empty() || EqualsIgnoreCase(key, "qor metrics") || EqualsIgnoreCase(key, "value")) {
            continue;
        }
        if (value.empty()) {
            group = std::string(key);  // 分组标题，后续行归入该组
            continue;
        }
        report.metrics.push_back(QorMetric{.group=group, .key=std::string(key), .value=std::string(value)});
    }

    // ---- 建议表 ----
    bool has_current = false;
    QorSuggestion current;
    for (const std::string_view raw : lines) {
        const std::string_view line = Strip(raw);
        if (!StartsWith(line, "|")) continue;

        const std::vector<std::string_view> cells = SplitPipeCells(line);
        if (cells.size() < 3) continue;

        std::string id;
        if (RE2::PartialMatch(line, kSuggestionRe, &id)) {
            if (has_current) report.suggestions.push_back(std::move(current));
            has_current = false;

            std::vector<std::string> stripped_cells;
            stripped_cells.reserve(cells.size());
            for (const std::string_view cell : cells) stripped_cells.emplace_back(Strip(cell));
            const std::string joined = JoinBySpace(stripped_cells);

            std::string priority;
            (void)RE2::PartialMatch(joined, kPriorityRe, &priority);

            std::vector<std::string> text_parts;
            for (const std::string& cell : stripped_cells) {
                if (cell.empty()) continue;
                if (RE2::PartialMatch(cell, kSuggestionRe)) continue;
                if (RE2::FullMatch(cell, kPriorityRe)) continue;
                text_parts.push_back(cell);
            }

            current = QorSuggestion{};
            current.id = id;
            current.priority = priority;
            current.text = JoinBySpace(text_parts);
            has_current = true;
            continue;
        }

        if (!has_current) continue;
        // 后续行：只把最后一个单元格当作正文续行（复现 Python 的 cells[-1]）
        const std::string_view body = Strip(cells.back());
        if (body.empty()) continue;
        current.text += '\n';
        current.text += body;
    }
    if (has_current) report.suggestions.push_back(std::move(current));

    if (!saw_pipe_table) report.warnings.emplace_back("no-pipe-table");
    if (report.metrics.empty()) report.warnings.emplace_back("no-metrics");
    return report;
}

std::vector<QorSuggestion> ParseSuggestionsTcl(std::string_view path) {
    std::vector<QorSuggestion> out;
    const std::string text = ReadReportFile(path);
    if (text.empty()) return out;

    const std::string path_string(path);
    for (const std::string_view raw : SplitLines(text)) {
        const std::string_view line = Strip(raw);
        if (line.empty() || StartsWith(line, "#")) continue;
        QorSuggestion suggestion;
        suggestion.id = "TD-SUGGESTION";
        suggestion.priority.clear();
        suggestion.text = std::string(line);
        suggestion.file = path_string;
        out.push_back(std::move(suggestion));
    }
    return out;
}

}  // namespace fpgalsp::reports