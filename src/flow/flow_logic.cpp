// =============================================================================
//  src/flow/flow_logic.cpp —— Flow 纯判定逻辑
// =============================================================================
//  逐条对照 %PYREF%\fpgalsp\flow.py 实现（行号见各函数注释）。
// =============================================================================

#include "flow/flow_logic.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "platform/encoding.h"
#include "platform/path.h"
#include "platform/write_service.h"
#include "td/td_output.h"

#include "platform/text.h"
namespace fpgalsp::flow {
namespace {

using platform::EndsWith;

using platform::StartsWith;

using platform::ToLowerAscii;

using platform::Trim;

// 对应 Python 的 out.replace("\r", "\n").split("\n")：
// 先把每个 \r 换成 \n，再按 \n 切（\r\n 会因此切出一个空行，与 Python 一致）
std::vector<std::string> SplitNormalisedLines(std::string_view text) {
    std::string normalised(text);
    for (char& c : normalised) {
        if (c == '\r') c = '\n';
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = normalised.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(normalised.substr(start));
            return lines;
        }
        lines.push_back(normalised.substr(start, pos - start));
        start = pos + 1;
    }
}

bool Contains(const std::vector<std::string>& items, std::string_view value) {
    return std::ranges::find(items, value) != items.end();
}

bool AnyEndsWith(const std::vector<std::string>& names, std::string_view suffix) {
    return std::ranges::any_of(names,
                               [suffix](const std::string& name) { return EndsWith(name, suffix); });
}

bool ContainsToken(std::string_view text, std::string_view token) {
    return text.contains(token);
}

}  // namespace

// -----------------------------------------------------------------------------
// 阶段
// -----------------------------------------------------------------------------

std::string StageLabel(std::string_view stage) {
    // Python STAGE_NAMES（注意 "all" 在 NormaliseStages 里就被展开成 syn+phy，
    // 这里保留映射只是为了让日志里仍能显示组合名）
    if (stage == "syn") return "Synthesis";
    if (stage == "phy") return "Place & Route";
    if (stage == "all") return "Synthesis + Place & Route";
    if (stage == "route") return "Incremental route";
    return std::string(stage);
}

std::vector<std::string> NormaliseStages(const std::vector<std::string>& stages) {
    std::vector<std::string> out;
    for (const std::string& raw : stages) {
        const std::string stage = ToLowerAscii(Trim(raw));
        // "all" 与 "route" 都会**立即终止**并覆盖整个结果（Python 的 return，不是 append）
        if (stage == "all") return {"syn", "phy"};
        if (stage == "route" || stage == "reroute" || stage == "route_only" ||
            stage == "incremental") {
            return {"route"};
        }
        if (stage == "syn" || stage == "synthesis" || stage == "synth") {
            out.emplace_back("syn");
        } else if (stage == "phy" || stage == "pnr" || stage == "place_route") {
            // ⚠ Python 的 elif 里还写了一个 "route"，但上面已经提前 return，
            //   那个分支是死代码。这里保留同样的语义（不额外接受 "route"）。
            out.emplace_back("phy");
        }
    }

    std::vector<std::string> ordered;
    for (const std::string& stage : out) {
        if (!Contains(ordered, stage)) ordered.push_back(stage);
    }
    return ordered;
}

// -----------------------------------------------------------------------------
// 进度识别
// -----------------------------------------------------------------------------

const std::vector<PhaseToken>& PhaseTokens() {
    // 顺序与 Python 完全一致：同一行命中多个 token 时取靠前的那个。
    // 实测 TD 2026.2 的两种写法（opt_rtl / optimize_rtl、opt_gate / optimize_gate）都在表里。
    static const std::vector<PhaseToken> kTokens = {
        {.token="read_design", .label="Read Design"},
        {.token="opt_rtl", .label="Optimize RTL"},
        {.token="optimize_rtl", .label="Optimize RTL"},
        {.token="opt_gate", .label="Optimize Gate"},
        {.token="optimize_gate", .label="Optimize Gate"},
        {.token="legalize_phy_inst", .label="Legalize"},
        {.token="place", .label="Place"},
        {.token="route", .label="Route"},
        {.token="fix_hold", .label="Fix Hold"},
        {.token="update_timing", .label="Update Timing"},
        {.token="bitgen", .label="Bitgen"},
    };
    return kTokens;
}

std::string MatchPhaseToken(std::string_view line) {
    const std::string lower = ToLowerAscii(line);
    for (const PhaseToken& entry : PhaseTokens()) {
        if (ContainsToken(lower, entry.token)) return entry.label;
    }
    return {};
}

PhaseDetection DetectProgress(const std::vector<std::string>& tail_lines) {
    PhaseDetection out;
    for (const auto& tail_line : std::views::reverse(tail_lines)) {
        out.phase = MatchPhaseToken(tail_line);
        if (!out.phase.empty()) break;
    }
    for (const auto& tail_line : std::views::reverse(tail_lines)) {
        const std::string trimmed = Trim(tail_line);
        if (trimmed.empty() || StartsWith(trimmed, "===")) continue;
        out.message = trimmed.size() > 160 ? trimmed.substr(0, 160) : trimmed;
        break;
    }
    return out;
}

std::vector<std::string> CompletedStepsFromMarkers(const std::vector<std::string>& run_dir_names) {
    std::vector<std::string> done;
    for (const PhaseToken& entry : PhaseTokens()) {
        // ".%s.end.f" % step
        const std::string marker = "." + std::string(entry.token) + ".end.f";
        if (Contains(run_dir_names, marker)) done.emplace_back(entry.token);
    }
    return done;
}

// -----------------------------------------------------------------------------
// run 目录状态
// -----------------------------------------------------------------------------

bool RunStarted(const std::vector<std::string>& run_dir_names) {
    for (const std::string& name : run_dir_names) {
        if (EndsWith(name, ".begin.f") || EndsWith(name, ".end.f")) return true;
    }
    return Contains(run_dir_names, "run.log");
}

bool RunCompleted(std::string_view run_name, const std::vector<std::string>& run_dir_names,
                  bool project_bit_exists, std::string_view project_name) {
    // 条件 3（三种 PRJ 容错组合里都要成立）：run 目录里不能有任何步骤错误标记
    if (AnyEndsWith(run_dir_names, ".error.f")) return false;

    if (StartsWith(run_name, "syn")) {
        return Contains(run_dir_names, ".opt_gate.end.f") &&
               AnyEndsWith(run_dir_names, "_gate.db");
    }
    if (StartsWith(run_name, "phy")) {
        (void)project_name;  // Python 只在 isfile 判定里用到文件名，这里由调用方算好传入
        return Contains(run_dir_names, ".bitgen.end.f") &&
               (project_bit_exists || AnyEndsWith(run_dir_names, ".bit"));
    }
    return AnyEndsWith(run_dir_names, ".end.f");
}

std::vector<std::string> ErrorMarkers(const std::vector<std::string>& run_dir_names) {
    std::vector<std::string> markers;
    for (const std::string& name : run_dir_names) {
        if (EndsWith(name, ".error.f")) markers.push_back(name);
    }
    return markers;
}

std::vector<std::string> ErrorMarkerSteps(const std::vector<std::string>& run_dir_names) {
    // Python: name.strip(".").replace(".error.f", "")
    std::vector<std::string> steps;
    for (const std::string& name : ErrorMarkers(run_dir_names)) {
        std::string_view body(name);
        while (!body.empty() && body.front() == '.') body.remove_prefix(1);
        while (!body.empty() && body.back() == '.') body.remove_suffix(1);
        std::string step(body);
        if (const std::size_t pos = step.find(".error.f"); pos != std::string::npos) {
            step.erase(pos);
        }
        steps.push_back(std::move(step));
        if (steps.size() >= 5) break;  // Python 只列前 5 个
    }
    return steps;
}

// -----------------------------------------------------------------------------
// PRJ-8806 / PRJ-5303 容错
// -----------------------------------------------------------------------------

bool IsToleratedPrjCode(std::string_view text) {
    return ContainsToken(text, "PRJ-8806") || ContainsToken(text, "PRJ-5303");
}

bool OnlyToleratedPrjErrors(std::string_view text) {
    std::vector<std::string> errors;
    for (const std::string& line : SplitNormalisedLines(text)) {
        if (ContainsToken(line, "ERROR")) errors.push_back(Trim(line));
    }
    if (errors.empty()) return false;
    return std::ranges::all_of(errors, [](const std::string& error) {
        return IsToleratedPrjCode(error);
    });
}

std::string ErrorLines(std::string_view text, int limit) {
    std::string out;
    int used = 0;
    for (const std::string& line : SplitNormalisedLines(text)) {
        if (!ContainsToken(line, "ERROR") && !ContainsToken(line, "CRITICAL-WARNING")) continue;
        if (used >= limit) break;
        if (!out.empty()) out += " / ";
        out += Trim(line);
        ++used;
    }
    return out;
}

std::string FirstErrorLine(std::string_view text) {
    for (const std::string& line : SplitNormalisedLines(text)) {
        if (ContainsToken(line, "ERROR") || ContainsToken(line, "CRITICAL-WARNING")) {
            return Trim(line);
        }
    }
    return {};
}

std::string BuildFailureDetail(std::string_view out, const std::vector<std::string>& error_markers,
                               std::string_view run_log_tail, std::string_view run_dir) {
    std::string detail = ErrorLines(out);
    if (!error_markers.empty()) {
        std::string steps;
        for (std::size_t i = 0; i < error_markers.size() && i < 5; ++i) {
            if (!steps.empty()) steps += ", ";
            // ".opt_gate.error.f" -> "opt_gate"
            std::string_view body(error_markers[i]);
            while (!body.empty() && body.front() == '.') body.remove_prefix(1);
            while (!body.empty() && body.back() == '.') body.remove_suffix(1);
            std::string step(body);
            if (const std::size_t pos = step.find(".error.f"); pos != std::string::npos) {
                step.erase(pos);
            }
            steps += step;
        }
        if (!detail.empty()) detail += " | ";
        detail += "failed run step: " + steps;
    }
    if (!run_log_tail.empty()) {
        if (!detail.empty()) detail += " | ";
        detail += "run.log: " + std::string(run_log_tail);
    }
    if (detail.empty()) detail = "run dir: " + std::string(run_dir);
    return detail.size() > 2000 ? detail.substr(0, 2000) : detail;
}

RunOutcome DecideRunOutcome(const RunSnapshot& snapshot, std::string_view out) {
    RunOutcome outcome;
    // 条件 0：没有错误 -> 成功（绝大多数正常情况走这里）
    if (!td::HasError(out)) {
        outcome.accepted = true;
        return outcome;
    }

    // ⚠ PRJ-8806 / PRJ-5303 容错：三条**全部**满足才判成功
    //   1) 所有 ERROR 行都是 PRJ-8806 / PRJ-5303
    //   2) run 目录里有完成标记（或 .bit）
    //   3) run 目录里没有 .error.f（由 RunCompleted 的前置判定负责）
    const bool only_tolerated = OnlyToleratedPrjErrors(out);
    const bool completed = RunCompleted(snapshot.run_name, snapshot.run_dir_names,
                                        snapshot.project_bit_exists, snapshot.project_name);
    if (only_tolerated && completed) {
        outcome.accepted = true;
        outcome.tolerated_prj_error = true;
        return outcome;
    }

    // ── 条件 1b：**只有** CRITICAL-WARNING、一个 ERROR 都没有，且产物齐全 -> 接受为成功 ──
    // 这是相对 Python 版的一处**有意偏离**（Python 用 "ERROR" 子串分类，警告一律判失败）。
    // 起因是 TD 2026.2_SP1 (6.2.3.218141) 的 release note：
    //   "修复 read_adc 命令报 CRITICAL-WARNING 但会导致 Flow 退出的问题"
    // —— 也就是说 SP1 **之前**这类警告会连带中断流程（判失败是对的），SP1 **之后**流程
    // 照常跑完、产物齐全，但警告文本仍然出现在 stdout 里。若继续"有警告即失败"，
    // 就会出现「TD 已经产出 bit、后端却报失败」。
    // 仍然要求 completed：不把「只有警告、但根本没跑完」当成成功。
    if (!td::HasFatal(out) && completed) {
        outcome.accepted = true;
        outcome.warnings_only = ErrorLines(out, 3);
        return outcome;
    }

    outcome.accepted = false;
    outcome.failure_detail = BuildFailureDetail(out, ErrorMarkers(snapshot.run_dir_names),
                                                snapshot.run_log_tail, snapshot.run_dir);
    if (!snapshot.missing_run_log.empty()) {
        if (!outcome.failure_detail.empty()) outcome.failure_detail += " | ";
        outcome.failure_detail += "run.log missing: " + snapshot.missing_run_log;
    }
    if (outcome.failure_detail.size() > 2000) outcome.failure_detail.resize(2000);
    return outcome;
}

// -----------------------------------------------------------------------------
// stall 看门狗
// -----------------------------------------------------------------------------

std::string StallReason(double activity_mtime, double now, double stage_started,
                        double stall_seconds) {
    if (activity_mtime <= 0.0 && now - stage_started > 120.0) {
        return "produced no run trace (the child process may have failed, e.g. PRJ-8001)";
    }
    if (activity_mtime > 0.0 && now - activity_mtime > stall_seconds) {
        // 与 snprintf("%.0f") 逐字节一致
        return std::format(
            "produced no new artifacts for {:.0f} s (stalled or child process died)",
            now - activity_mtime);
    }
    return {};
}

// -----------------------------------------------------------------------------
// 日志与摘要
// -----------------------------------------------------------------------------

std::vector<std::string> TrimLog(std::vector<std::string> log, std::size_t limit) {
    if (log.size() > limit) log.erase(log.begin(), log.begin() + (log.size() - limit));
    return log;
}

std::string FormatLogLine(double elapsed, std::string_view stage, std::string_view message) {
    // 对应 Python 的 "%6.1fs [%s] %s"；`{:6.1f}` 与 printf 的宽度/精度语义一致
    std::string out = std::format("{:6.1f}s [", elapsed);
    out += stage.empty() ? "-" : std::string(stage);
    out += "] ";
    out += message;
    return out;
}

std::string SummaryFromReports(const jsonrpc::Json& reports) {
    std::vector<std::string> parts;

    // qor.metrics 是 [{group,key,value}]，取最后出现的同名 key（与 Python dict 覆盖一致）
    const auto qor = reports.find("qor");
    if (qor != reports.end() && qor->is_object()) {
        const auto metrics = qor->find("metrics");
        if (metrics != qor->end() && metrics->is_array()) {
            std::string setup_wns;
            std::string hold_wns;
            for (const auto& item : *metrics) {
                if (!item.is_object()) continue;
                const auto key = item.find("key");
                const auto value = item.find("value");
                if (key == item.end() || value == item.end()) continue;
                if (!key->is_string() || !value->is_string()) continue;
                if (key->get<std::string>() == "Setup WNS") setup_wns = value->get<std::string>();
                if (key->get<std::string>() == "Hold WNS") hold_wns = value->get<std::string>();
            }
            if (!setup_wns.empty()) parts.push_back("Setup WNS " + setup_wns + "ps");
            if (!hold_wns.empty()) parts.push_back("Hold WNS " + hold_wns + "ps");
        }
    }

    const auto timing = reports.find("timing");
    if (timing != reports.end() && timing->is_object()) {
        const auto summary = timing->find("summary");
        if (summary != timing->end() && summary->is_object()) {
            const auto swns = summary->find("SWNS");
            if (swns != summary->end() && swns->is_number()) {
                parts.emplace_back(std::format("SWNS {:g}ns", swns->get<double>()));
            }
        }
    }

    const auto area = reports.find("area");
    if (area != reports.end() && area->is_object()) {
        const auto rows = area->find("rows");
        if (rows != area->end() && rows->is_array()) {
            for (const auto& row : *rows) {
                if (!row.is_object()) continue;
                const auto resource = row.find("resource");
                if (resource == row.end() || !resource->is_string()) continue;
                if (resource->get<std::string>() != "#lut") continue;
                const auto used = row.find("used");
                const auto total = row.find("total");
                if (used != row.end() && total != row.end() && used->is_number() &&
                    total->is_number()) {
                    parts.push_back("LUT " + std::to_string(used->get<long long>()) + "/" +
                                    std::to_string(total->get<long long>()));
                }
                break;
            }
        }
    }

    const auto artifacts = reports.find("artifacts");
    if (artifacts != reports.end() && artifacts->is_object()) {
        const auto bit = artifacts->find("bit");
        if (bit != artifacts->end() && bit->is_object()) {
            const auto exists = bit->find("exists");
            if (exists != bit->end() && exists->is_boolean() && exists->get<bool>()) {
                long long size = 0;
                const auto size_it = bit->find("size");
                if (size_it != bit->end() && size_it->is_number()) {
                    size = size_it->get<long long>();
                }
                parts.push_back("bit " + std::to_string(size / 1024) + " KB");
            }
        }
    }

    if (parts.empty()) return "flow complete";
    std::string out = "flow complete: ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += ", ";
        out += parts[i];
    }
    return out;
}

std::string ReportedRunLogPath(std::string_view text) {
    static const RE2 kRunLogRe(R"((?i)Run log file is\s+(.+?)\s*$)");
    for (const std::string& line : SplitNormalisedLines(text)) {
        std::string path;
        if (!RE2::PartialMatch(line, kRunLogRe, &path)) continue;
        path = Trim(path);
        while (!path.empty() && path.back() == '.') path.pop_back();
        return path;
    }
    return {};
}

bool RewriteProjectPath(const std::string& al_path, const std::string& new_root) {
    std::string text = platform::try_read_file_utf8(al_path).value_or(std::string());
    if (text.empty()) return false;
    const std::size_t marker = text.find("Path=\"");
    if (marker == std::string::npos) return false;
    const std::size_t begin = marker + 6;
    const std::size_t end = text.find('"', begin);
    if (end == std::string::npos) return false;

    std::string root(new_root);
    for (char& c : root) {
        if (c == '\\') c = '/';
    }
    text.replace(begin, end - begin, root);
    // 统一写服务：工程文件（.al）由 service 做策略/备份/原子写；失败时原文件保持不变。
    return platform::writes::Write(platform::path_dirname(al_path), al_path, text,
                                   platform::writes::Options{.caller = "flow.project_path"})
        .ok;
}


}  // namespace fpgalsp::flow
