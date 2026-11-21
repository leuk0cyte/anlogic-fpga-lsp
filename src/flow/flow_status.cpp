// =============================================================================
//  src/flow/flow_status.cpp —— Flow Statistics 表 + 步骤标记 -> GUI 的 6 个步骤
// =============================================================================
//  纯解析/聚合逻辑（不碰进程、不起线程），因此能在毫秒级单测里覆盖。
//  步骤标记的命名约定与 flow_logic.cpp 的 RunCompleted / ErrorMarkers 一致：
//      .<step>.begin.f / .<step>.end.f / .<step>.error.f
//  其中 <step> 就是 DefaultFlow.tcl 的 stepList（即参数命名空间）：
//      read_design / opt_rtl / opt_gate / opt_place / opt_route / bitgen
// =============================================================================

#include "flow/flow_status.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "platform/encoding.h"

namespace fpgalsp::flow {
namespace {

// -----------------------------------------------------------------------------
// 6 步模板
// -----------------------------------------------------------------------------
struct StepTemplate {
    std::string_view name;    // GUI 名称（TD GUI 的 Flow 视图逐字）
    std::string_view stage;   // 参数命名空间 / DefaultFlow 的 step
    std::string_view marker;  // .<marker>.begin.f 的前缀
};

// 顺序 = DefaultFlow.tcl 的 stepList（read_design opt_rtl opt_gate opt_place opt_route bitgen）
constexpr StepTemplate kTemplates[] = {
    {"Read Design", "design", "read_design"},
    {"Optimize RTL", "rtl", "opt_rtl"},
    {"Optimize Gate", "gate", "opt_gate"},
    {"Optimize Placement", "place", "opt_place"},
    {"Optimize Routing", "route", "opt_route"},
    {"Generate Bitstream", "bitgen", "bitgen"},
};
constexpr std::size_t kStepCount = sizeof(kTemplates) / sizeof(kTemplates[0]);

// -----------------------------------------------------------------------------
// 命令名 -> GUI 步骤
// -----------------------------------------------------------------------------
//  ⚠ 这张表是"必须精确"的映射（与 TD GUI 的 6 步一一对齐）：
//      analyze|elaborate|read_hdl|read_adc|read_sdc|import_db|import_device -> Read Design
//      optimize_rtl                                                         -> Optimize RTL
//      optimize_gate                                                        -> Optimize Gate
//      place|legalize_phy_inst                                              -> Optimize Placement
//      route                                                                -> Optimize Routing
//      bitgen|setup_debugger                                                -> Generate Bitstream
//   额外的别名（opt_rtl / opt_gate / opt_place / opt_route）只是同一个步骤的另一种
//   写法（标记文件名与 PHASE_TOKENS 里用的就是它们），不改变上面的语义。
//   **故意不映射** fix_hold / update_timing：它们同时出现在 opt_gate、opt_place、
//   opt_route 三个步骤里（见 DefaultFlow.tcl），记到任何一步都是错的，宁可不计。
struct CommandMapping {
    std::string_view command;
    std::string_view name;  // kTemplates 里的 name
};

constexpr CommandMapping kCommandMap[] = {
    {"analyze", "Read Design"},
    {"elaborate", "Read Design"},
    {"read_hdl", "Read Design"},
    {"read_adc", "Read Design"},
    {"read_sdc", "Read Design"},
    {"import_db", "Read Design"},
    {"import_device", "Read Design"},
    {"optimize_rtl", "Optimize RTL"},
    {"opt_rtl", "Optimize RTL"},
    {"optimize_gate", "Optimize Gate"},
    {"opt_gate", "Optimize Gate"},
    {"place", "Optimize Placement"},
    {"legalize_phy_inst", "Optimize Placement"},
    {"opt_place", "Optimize Placement"},
    {"route", "Optimize Routing"},
    {"opt_route", "Optimize Routing"},
    {"bitgen", "Generate Bitstream"},
    {"setup_debugger", "Generate Bitstream"},
};

// -----------------------------------------------------------------------------
// 字符串小工具
// -----------------------------------------------------------------------------

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string ToLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return ToLowerAscii(text.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    return ToLowerAscii(text.substr(text.size() - suffix.size())) == ToLowerAscii(suffix);
}

std::string Join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += name;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

// 目录里的文件名（不含路径），排序后返回。never-throw：目录不存在时返回空。
std::vector<std::string> ListDir(std::string_view dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(std::string(dir)), ec);
    if (ec) return names;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const std::u8string name = it->path().filename().u8string();
        names.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());
    return names;
}

// 子目录名（不含路径），排序后返回。never-throw。
std::vector<std::string> ListSubdirs(std::string_view dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(std::string(dir)), ec);
    if (ec) return names;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        if (std::filesystem::is_directory(it->path(), status_ec) && !status_ec) {
            const std::u8string name = it->path().filename().u8string();
            names.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string ReadTextFile(std::string_view path) {
    const auto result = platform::try_read_file_utf8(path);
    return result.has_value() ? *result : std::string();
}

bool Contains(const std::vector<std::string>& items, std::string_view value) {
    return std::find(items.begin(), items.end(), value) != items.end();
}

// -----------------------------------------------------------------------------
// 表行解析
// -----------------------------------------------------------------------------
// 与 reports/flow_status.cpp 的同名解析保持同一套列语义：
//   <command> | <calls> | <wall time> | <cpu time(s)> | <used memory(mb)> | <peak>
// 用 PartialMatch（不是 Consume）：PartialMatch 只要求行首按此形状开始，
// 表行尾部的空格/多余列不会让整行失配。
constexpr std::string_view kRowPattern =
    R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\|\s*(\d+)\s*\|\s*([^|]*)\|\s*([^|]*)\|\s*(\d+)\s*\|\s*(\d+))";

// 表里的一行（**命令级**，还没聚合到 GUI 步骤）
struct StatusRow {
    std::string command;  // 已转小写
    double wall = 0.0;
    double cpu = 0.0;
    long long mem_mb = 0;
};

std::vector<StatusRow> ParseRows(std::string_view text) {
    static const RE2 kRowRe{std::string(kRowPattern)};
    std::vector<StatusRow> rows;
    if (text.empty()) return rows;

    std::string normalised(text);
    for (char& c : normalised) {
        if (c == '\r') c = '\n';
    }
    std::size_t start = 0;
    while (start <= normalised.size()) {
        const std::size_t pos = normalised.find('\n', start);
        const std::string_view raw = pos == std::string::npos
                                         ? std::string_view(normalised).substr(start)
                                         : std::string_view(normalised).substr(start, pos - start);
        const std::string line = Trim(raw);
        if (!line.empty()) {
            std::string command;
            std::string calls;
            std::string wall;
            std::string cpu;
            std::string mem;
            std::string peak;
            const re2::StringPiece input(line.data(), line.size());
            if (RE2::PartialMatch(input, kRowRe, &command, &calls, &wall, &cpu, &mem, &peak)) {
                StatusRow row;
                row.command = ToLowerAscii(command);
                row.wall = ParseFlowStatusSeconds(wall);
                row.cpu = ParseFlowStatusSeconds(cpu);
                const std::string mem_text = Trim(mem);
                if (!mem_text.empty()) row.mem_mb = std::strtoll(mem_text.c_str(), nullptr, 10);
                rows.push_back(std::move(row));
            }
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return rows;
}

// 命令名 -> kTemplates 下标；不认识（total / fix_hold / update_timing / 报告行）返回 -1
int GuiIndexForCommand(std::string_view command) {
    const std::string lower = ToLowerAscii(command);
    for (const CommandMapping& entry : kCommandMap) {
        if (lower != entry.command) continue;
        for (std::size_t i = 0; i < kStepCount; ++i) {
            if (kTemplates[i].name == entry.name) return static_cast<int>(i);
        }
    }
    return -1;
}

// 把命令级行聚合到给定步骤列表里（同一 GUI 步骤的 wall/cpu 相加，mem 取最大）
void AggregateInto(std::vector<FlowStep>& steps, const std::vector<StatusRow>& rows) {
    for (const StatusRow& row : rows) {
        const int index = GuiIndexForCommand(row.command);
        if (index < 0) continue;
        const std::string_view name = kTemplates[static_cast<std::size_t>(index)].name;
        for (FlowStep& step : steps) {
            if (std::string_view(step.name) != name) continue;
            step.wall += row.wall;
            step.cpu += row.cpu;
            step.mem_mb = std::max(step.mem_mb, row.mem_mb);
            break;
        }
    }
}

// 合并多份快照：同一命令取较大值（这些文件是同一 run 的累计快照，相加会重复计数）
void MergeRows(std::vector<StatusRow>& into, const std::vector<StatusRow>& extra) {
    for (const StatusRow& row : extra) {
        auto it = std::find_if(into.begin(), into.end(), [&row](const StatusRow& existing) {
            return existing.command == row.command;
        });
        if (it == into.end()) {
            into.push_back(row);
            continue;
        }
        it->wall = std::max(it->wall, row.wall);
        it->cpu = std::max(it->cpu, row.cpu);
        it->mem_mb = std::max(it->mem_mb, row.mem_mb);
    }
}

// 收集一个 run 目录里的全部表行：flow.status，再合并 *_flow.status
// （place_flow.status / route_flow.status —— DefaultFlow 每个步骤结束时写的累积快照）
std::vector<StatusRow> CollectTableRows(const std::string& run_dir,
                                        const std::vector<std::string>& dir_names) {
    std::vector<StatusRow> rows;
    if (Contains(dir_names, "flow.status")) {
        rows = ParseRows(ReadTextFile(Join(run_dir, "flow.status")));
    }
    for (const std::string& name : dir_names) {
        // "flow.status" 本身不以 "_flow.status" 结尾，因此不会被重复解析
        if (!EndsWithNoCase(name, "_flow.status")) continue;
        MergeRows(rows, ParseRows(ReadTextFile(Join(run_dir, name))));
    }
    return rows;
}

// 这个 run 负责 6 步里的哪几步
bool RunKeepsStage(std::string_view run_name, std::string_view stage) {
    if (StartsWithNoCase(run_name, "syn")) {
        return stage == "design" || stage == "rtl" || stage == "gate";
    }
    if (StartsWithNoCase(run_name, "phy")) {
        return stage == "place" || stage == "route" || stage == "bitgen";
    }
    if (StartsWithNoCase(run_name, "route")) {
        // 增量布线：复用 place.db，因此只有 route + bitgen 两步
        return stage == "route" || stage == "bitgen";
    }
    return true;  // 认不出的 run 名：不过滤（自定义 run 目录/单测用得上）
}

// 目录里以 prefix 开头的 run：优先 <prefix>_1（FlowRunner 的真实命名）
std::string PickRun(const std::vector<std::string>& names, std::string_view prefix) {
    const std::string first_choice = std::string(prefix) + "_1";
    for (const std::string& name : names) {
        if (StartsWithNoCase(name, first_choice)) return name;
    }
    for (const std::string& name : names) {
        if (StartsWithNoCase(name, prefix)) return name;
    }
    return {};
}

}  // namespace

// -----------------------------------------------------------------------------
// 公开 API
// -----------------------------------------------------------------------------

const std::vector<FlowStep>& GuiFlowSteps() {
    static const std::vector<FlowStep> kSteps = [] {
        std::vector<FlowStep> steps;
        steps.reserve(kStepCount);
        for (const StepTemplate& tpl : kTemplates) {
            FlowStep step;
            step.name = std::string(tpl.name);
            step.stage = std::string(tpl.stage);
            step.status = "pending";
            steps.push_back(std::move(step));
        }
        return steps;
    }();
    return kSteps;
}

double ParseFlowStatusSeconds(std::string_view text) {
    // "1m11s(62%)" -> 71.0 ；"238(83%)" -> 238 ；"1h2m3s" -> 3723 ；"12.5s" -> 12.5
    // 步骤：先砍掉括号里的百分比，再按 <数字><单位> 累加；没单位的纯数字按秒。
    std::string_view body = text;
    if (const std::size_t cut = body.find('('); cut != std::string_view::npos) {
        body = body.substr(0, cut);
    }
    double total = 0.0;
    double number = 0.0;
    bool has_number = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        const bool is_digit = c >= '0' && c <= '9';
        if (is_digit || c == '.') {
            std::size_t j = i;
            while (j < body.size() && ((body[j] >= '0' && body[j] <= '9') || body[j] == '.')) ++j;
            number = std::strtod(std::string(body.substr(i, j - i)).c_str(), nullptr);
            has_number = true;
            i = j - 1;
            continue;
        }
        if (!has_number) continue;  // 空白等：跳过
        const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (unit == 'h') {
            total += number * 3600.0;
        } else if (unit == 'm') {
            total += number * 60.0;  // "1m11s" 里的 m
        } else if (unit == 's') {
            total += number;
        }
        number = 0.0;
        has_number = false;
    }
    if (has_number) total += number;  // 没有单位的尾数 -> 秒
    return total;
}

std::vector<FlowStep> ParseFlowStatus(std::string_view text) {
    const std::vector<StatusRow> rows = ParseRows(text);
    std::vector<FlowStep> steps;
    for (std::size_t i = 0; i < kStepCount; ++i) {
        const bool present =
            std::any_of(rows.begin(), rows.end(), [i](const StatusRow& row) {
                return GuiIndexForCommand(row.command) == static_cast<int>(i);
            });
        if (!present) continue;
        FlowStep step;
        step.name = std::string(kTemplates[i].name);
        step.stage = std::string(kTemplates[i].stage);
        step.status.clear();  // 表只能给耗时；完成/失败看标记文件
        steps.push_back(std::move(step));
    }
    AggregateInto(steps, rows);
    return steps;
}

std::vector<FlowStep> CollectFlowSteps(const std::string& run_dir, const std::string& run_name) {
    const std::vector<std::string> dir_names = ListDir(run_dir);

    std::vector<FlowStep> steps;
    for (const StepTemplate& tpl : kTemplates) {
        if (!RunKeepsStage(run_name, tpl.stage)) continue;
        FlowStep step;
        step.name = std::string(tpl.name);
        step.stage = std::string(tpl.stage);
        step.status = "pending";
        steps.push_back(std::move(step));
    }

    // 1) 标记文件 -> status（顺序来自模板，不由标记文件排序）
    for (FlowStep& step : steps) {
        std::string_view marker;
        for (const StepTemplate& tpl : kTemplates) {
            if (std::string_view(step.stage) == tpl.stage) marker = tpl.marker;
        }
        if (marker.empty()) continue;
        const std::string base = "." + std::string(marker);
        if (Contains(dir_names, base + ".error.f")) {
            step.status = "failed";
        } else if (Contains(dir_names, base + ".end.f")) {
            step.status = "done";
        } else if (Contains(dir_names, base + ".begin.f")) {
            step.status = "running";
        }
    }

    // 2) flow.status / *_flow.status -> 每步耗时
    AggregateInto(steps, CollectTableRows(run_dir, dir_names));

    // 3) 表里有耗时 = 这条命令跑完过；标记文件被清理掉的老 run 也要显示成 done
    for (FlowStep& step : steps) {
        if (step.status == "pending" && step.wall > 0.0) step.status = "done";
    }
    return steps;
}

std::vector<RunFlowSteps> CollectRunsFlowSteps(const std::string& runs_root) {
    const std::vector<std::string> dirs = ListSubdirs(runs_root);

    std::string syn_run = PickRun(dirs, "syn");
    std::string phy_run = PickRun(dirs, "phy");
    if (syn_run.empty() && phy_run.empty()) {
        // 只有增量布线（route_*）时，把它当成 phy 那一段
        phy_run = PickRun(dirs, "route");
    }

    std::vector<RunFlowSteps> runs;
    runs.push_back(RunFlowSteps{
        syn_run,
        syn_run.empty() ? std::vector<FlowStep>{}
                        : CollectFlowSteps(Join(runs_root, syn_run), syn_run)});
    runs.push_back(RunFlowSteps{
        phy_run,
        phy_run.empty() ? std::vector<FlowStep>{}
                        : CollectFlowSteps(Join(runs_root, phy_run), phy_run)});
    return runs;
}

std::vector<FlowStep> CollectAllFlowSteps(const std::string& runs_root) {
    const std::vector<RunFlowSteps> runs = CollectRunsFlowSteps(runs_root);

    // 固定 6 条：模板顺序，缺的步骤补 pending（没跑过 / 没有 flow.status 都走这里）
    std::vector<FlowStep> steps;
    steps.reserve(kStepCount);
    for (const StepTemplate& tpl : kTemplates) {
        const FlowStep* found = nullptr;
        for (const RunFlowSteps& run : runs) {
            for (const FlowStep& step : run.steps) {
                if (std::string_view(step.name) == tpl.name) {
                    found = &step;
                    break;
                }
            }
            if (found != nullptr) break;
        }
        if (found != nullptr) {
            steps.push_back(*found);
            continue;
        }
        FlowStep step;
        step.name = std::string(tpl.name);
        step.stage = std::string(tpl.stage);
        step.status = "pending";
        steps.push_back(std::move(step));
    }
    return steps;
}

jsonrpc::Json StepsToJson(const std::vector<FlowStep>& steps) {
    jsonrpc::Json out = jsonrpc::Json::array();
    for (const FlowStep& step : steps) {
        jsonrpc::Json item = jsonrpc::Json::object();
        item["name"] = step.name;
        item["stage"] = step.stage;
        item["status"] = step.status;
        item["wall"] = step.wall;
        item["cpu"] = step.cpu;
        item["mem_mb"] = step.mem_mb;
        // run 名（syn_1 / phy_1）由调用方在合并时补，这里不写。
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace fpgalsp::flow