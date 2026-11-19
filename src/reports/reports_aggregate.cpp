// =============================================================================
//  src/reports/reports_aggregate.cpp —— fpgaLsp/reports 汇总
// =============================================================================
//  逐段对照 Y:\fpga-lsp\fpgalsp\reports.py::collect（469-592 行）实现。
// =============================================================================

#include "reports/reports_aggregate.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reports/report_text.h"

namespace fpgalsp::reports {

// -----------------------------------------------------------------------------
// section 掩码
// -----------------------------------------------------------------------------

namespace sections {

std::uint32_t Parse(std::string_view name, bool* ok) {
    if (ok != nullptr) *ok = true;
    if (name == "qor") return kQor;
    if (name == "timing") return kTiming;
    if (name == "area") return kArea;
    if (name == "flow") return kFlow;
    if (name == "files") return kFiles;
    if (name == "artifacts") return kArtifacts;
    // 插件首帧只要元数据；不解析 qor/timing/area/flow 的正文。
    if (name == "summary") return kFiles | kArtifacts;
    if (name == "all" || name.empty()) return kAll;
    if (ok != nullptr) *ok = false;
    return kNone;
}

std::string_view Name(std::uint32_t bit) {
    switch (bit) {
        case kQor: return "qor";
        case kTiming: return "timing";
        case kArea: return "area";
        case kFlow: return "flow";
        case kFiles: return "files";
        case kArtifacts: return "artifacts";
        default: return {};
    }
}

std::uint32_t MaskFromList(const std::vector<std::string>& names, bool* ok) {
    if (ok != nullptr) *ok = true;
    if (names.empty()) return kAll;
    std::uint32_t mask = kNone;
    for (const std::string& name : names) {
        bool known = true;
        mask |= Parse(name, &known);
        if (!known && ok != nullptr) *ok = false;
    }
    return mask;
}

}  // namespace sections

namespace {

std::vector<std::string> Concat(std::vector<std::string> head,
                                const std::vector<std::string>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

std::string DirName(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return std::string();
    return std::string(path.substr(0, pos));
}

std::string BaseName(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return std::string(path);
    return std::string(path.substr(pos + 1));
}

double NowSeconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

// ---- 解析入口：先问缓存，缓存未命中才读盘 + 解析（Phase 2 / A3）----
//  这些 Load* 是 BuildReports 里**唯一**读报告正文的地方；
//  cache 为空时与旧实现逐字节一致（ReadReportFile + ParseX）。
QorReport LoadQor(const std::string& path, std::string_view step, ReportParseCache* cache) {
    if (cache != nullptr) {
        if (std::shared_ptr<const QorReport> cached = cache->GetQor(path, step)) return *cached;
    }
    return ParseQor(ReadReportFile(path), step);
}

TimingReport LoadTiming(const std::string& path, std::string_view filename,
                        ReportParseCache* cache) {
    if (cache != nullptr) {
        if (std::shared_ptr<const TimingReport> cached = cache->GetTiming(path, filename)) {
            return *cached;
        }
    }
    return ParseTiming(ReadReportFile(path), filename);
}

AreaReport LoadArea(const std::string& path, ReportParseCache* cache) {
    if (cache != nullptr) {
        if (std::shared_ptr<const AreaReport> cached = cache->GetArea(path)) return *cached;
    }
    return ParseArea(ReadReportFile(path));
}

FlowStatusReport LoadFlowStatus(const std::string& path, ReportParseCache* cache) {
    if (cache != nullptr) {
        if (std::shared_ptr<const FlowStatusReport> cached = cache->GetFlowStatus(path)) {
            return *cached;
        }
    }
    return ParseFlowStatusText(ReadReportFile(path));
}

std::vector<QorSuggestion> LoadSuggestions(const std::string& path, ReportParseCache* cache) {
    if (cache != nullptr) {
        if (std::shared_ptr<const std::vector<QorSuggestion>> cached =
                cache->GetSuggestions(path)) {
            return *cached;
        }
    }
    return ParseSuggestionsTcl(path);
}

// 等价 Python 的 `_stage_section(...)`
StageSection BuildStageSection(const std::string& base_dir, std::string label,
                               const std::vector<std::string>& qor_names,
                               const std::vector<std::string>& timing_names,
                               const std::vector<std::string>& area_names,
                               const std::vector<std::string>& status_names,
                               std::string_view timing_glob, std::string_view area_glob,
                               std::uint32_t mask, ReportParseCache* cache,
                               std::vector<std::string>* warnings) {
    StageSection section;
    section.label = std::move(label);

    std::vector<std::string> qor_candidates;
    for (const std::string& name : qor_names) qor_candidates.push_back(JoinPath(base_dir, name));

    std::vector<std::string> timing_candidates;
    for (const std::string& name : timing_names) timing_candidates.push_back(JoinPath(base_dir, name));
    if (!timing_glob.empty()) {
        timing_candidates =
            Concat(std::move(timing_candidates), GlobReportFiles(base_dir, timing_glob));
    }

    std::vector<std::string> area_candidates;
    for (const std::string& name : area_names) area_candidates.push_back(JoinPath(base_dir, name));
    if (!area_glob.empty()) {
        area_candidates = Concat(std::move(area_candidates), GlobReportFiles(base_dir, area_glob));
    }

    std::vector<std::string> status_candidates;
    for (const std::string& name : status_names) status_candidates.push_back(JoinPath(base_dir, name));

    section.files.qor = FirstExisting(qor_candidates);
    section.files.timing = FirstExisting(timing_candidates);
    section.files.area = FirstExisting(area_candidates);
    section.files.flow_status = FirstExisting(status_candidates);

    // step 取 label 的第一个词的小写（Python: label.split()[0].lower()）
    const std::vector<std::string> label_words = SplitWhitespace(section.label);
    const std::string step = label_words.empty() ? std::string() : ToLowerAscii(label_words[0]);

    if ((mask & sections::kQor) != 0u && !section.files.qor.empty()) {
        QorReport qor = LoadQor(section.files.qor, step, cache);
        for (const std::string& warning : qor.warnings) warnings->push_back(warning);
        section.qor = std::move(qor);
        section.ok = true;
    }
    if ((mask & sections::kTiming) != 0u && !section.files.timing.empty()) {
        TimingReport timing = LoadTiming(section.files.timing, section.files.timing, cache);
        for (const std::string& warning : timing.warnings) warnings->push_back(warning);
        section.timing = std::move(timing);
        section.ok = true;
    }
    if ((mask & sections::kArea) != 0u && !section.files.area.empty()) {
        AreaReport area = LoadArea(section.files.area, cache);
        for (const std::string& warning : area.warnings) warnings->push_back(warning);
        section.area = std::move(area);
        section.ok = true;
    }
    if ((mask & sections::kFlow) != 0u && !section.files.flow_status.empty()) {
        FlowStatusReport status = LoadFlowStatus(section.files.flow_status, cache);
        for (const std::string& warning : status.warnings) warnings->push_back(warning);
        section.flow_status = std::move(status);
        section.ok = true;
    }
    return section;
}

}  // namespace

// -----------------------------------------------------------------------------
// JSON
// -----------------------------------------------------------------------------

jsonrpc::Json ReportsFiles::ToJson(bool include_bit) const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["qor"] = qor;
    out["timing"] = timing;
    out["area"] = area;
    out["flow_status"] = flow_status;
    if (include_bit) out["bit"] = bit;
    return out;
}

jsonrpc::Json StageSection::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = ok;
    out["label"] = label;
    out["files"] = files.ToJson(/*include_bit=*/false);
    if (qor.has_value()) out["qor"] = qor->ToJson();
    if (timing.has_value()) out["timing"] = timing->ToJson();
    if (area.has_value()) out["area"] = area->ToJson();
    if (flow_status.has_value()) out["flowStatus"] = flow_status->ToJson();
    return out;
}

jsonrpc::Json ReportsBundle::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = ok;
    if (!error.empty()) out["error"] = error;

    jsonrpc::Json artifacts_json = artifacts.ToJson();
    if (!artifacts.runs_found) {
        // 没有 runs 目录时 Python 只给 {"runs": []}，不给 bit/route_db
        artifacts_json = jsonrpc::Json::object();
        artifacts_json["runs"] = jsonrpc::Json::array();
    }
    out["artifacts"] = std::move(artifacts_json);

    if (!ok) return out;  // 早退形态：ok/error/artifacts 三个键

    out["files"] = files.ToJson(/*include_bit=*/true);
    if (qor.has_value()) out["qor"] = qor->ToJson();
    if (timing.has_value()) out["timing"] = timing->ToJson();
    if (area.has_value()) out["area"] = area->ToJson();
    if (flow_status.has_value()) {
        jsonrpc::Json status_json = flow_status->ToJson();
        if (!flow_stages.empty()) {
            jsonrpc::Json stages_json = jsonrpc::Json::object();
            for (const auto& [key, value] : flow_stages) stages_json[key] = value;
            status_json["stages"] = std::move(stages_json);
        }
        out["flowStatus"] = std::move(status_json);
    }
    if (syn.has_value()) out["syn"] = syn->ToJson();
    if (phy.has_value()) out["phy"] = phy->ToJson();
    out["generatedAt"] = generated_at;
    return out;
}

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------

ReportsBundle BuildReports(const project::Project& project,
                          const ReportsBuildOptions& options) {
    const std::uint32_t mask = options.sections;
    ReportParseCache* const cache = options.cache;

    ReportsBundle bundle;
    bundle.generated_at = NowSeconds();

    bundle.artifacts = CollectArtifacts(project);
    if (!bundle.artifacts.runs_found) {
        bundle.ok = false;
        bundle.error = "No flow has run yet (cannot find " +
                       BaseName(bundle.artifacts.runs_dir) + ")";
        bundle.warnings.emplace_back("no-runs-directory");
        return bundle;
    }
    bundle.ok = true;

    const std::string& runs = bundle.artifacts.runs_dir;
    const std::string syn = JoinPath(runs, "syn_1");
    const std::string phy = JoinPath(runs, "phy_1");

    // ---- 顶层：挑"最新/最完整"的那一份 ----
    bundle.files.qor = FirstExisting({JoinPath(phy, "route.qor"), JoinPath(phy, "place.qor"),
                                      JoinPath(syn, "gate.qor")});

    std::vector<std::string> timing_candidates{JoinPath(phy, "final_timing.rpt")};
    timing_candidates = Concat(std::move(timing_candidates), GlobReportFiles(phy, R"(.*_pr\.timing)"));
    timing_candidates = Concat(std::move(timing_candidates), GlobReportFiles(syn, R"(.*_gate\.timing)"));
    bundle.files.timing = FirstExisting(timing_candidates);

    std::vector<std::string> area_candidates = GlobReportFiles(phy, R"(.*_phy\.area)");
    area_candidates = Concat(std::move(area_candidates), GlobReportFiles(syn, R"(.*_gate\.area)"));
    bundle.files.area = FirstExisting(area_candidates);

    bundle.files.flow_status = FirstExisting({JoinPath(phy, "route_flow.status"),
                                              JoinPath(phy, "place_flow.status"),
                                              JoinPath(syn, "flow.status")});
    bundle.files.bit = bundle.artifacts.bit.path;

    if ((mask & sections::kQor) != 0u && !bundle.files.qor.empty()) {
        std::string step = "gate";
        if (EndsWith(bundle.files.qor, "route.qor")) {
            step = "route";
        } else if (EndsWith(bundle.files.qor, "place.qor")) {
            step = "place";
        }
        QorReport qor = LoadQor(bundle.files.qor, step, cache);
        for (const std::string& warning : qor.warnings) bundle.warnings.push_back(warning);

        // TD 另外写出的 *_qor_suggestion.tcl（纯 set_param 处方）也并进建议列表
        for (const char* step_name : {"gate", "place", "route"}) {
            const std::string dir = std::string(step_name) == "gate" ? syn : phy;
            const std::string tcl = JoinPath(dir, std::string(step_name) + "_qor_suggestion.tcl");
            if (!IsFile(tcl)) continue;
            for (const QorSuggestion& suggestion : LoadSuggestions(tcl, cache)) {
                qor.suggestions.push_back(suggestion);
            }
        }
        bundle.qor = std::move(qor);
    }
    if ((mask & sections::kTiming) != 0u && !bundle.files.timing.empty()) {
        TimingReport timing = LoadTiming(bundle.files.timing, bundle.files.timing, cache);
        for (const std::string& warning : timing.warnings) bundle.warnings.push_back(warning);
        bundle.timing = std::move(timing);
    }
    if ((mask & sections::kArea) != 0u && !bundle.files.area.empty()) {
        AreaReport area = LoadArea(bundle.files.area, cache);
        for (const std::string& warning : area.warnings) bundle.warnings.push_back(warning);
        bundle.area = std::move(area);
    }
    if ((mask & sections::kFlow) != 0u && !bundle.files.flow_status.empty()) {
        const std::string status_text = ReadReportFile(bundle.files.flow_status);
        // 文件名对得上但内容不是 flow.status（TD 被打断时会留下半截文件）
        if (!LooksLikeFlowStatus(status_text)) bundle.warnings.emplace_back("flow-status-unrecognised");
        FlowStatusReport status = LoadFlowStatus(bundle.files.flow_status, cache);
        for (const std::string& warning : status.warnings) bundle.warnings.push_back(warning);
        // 该 status 属于哪个阶段：取所在目录名 "_" 前那一截（route_flow.status -> phy）
        const std::string stage = BaseName(DirName(bundle.files.flow_status));
        const std::size_t underscore = stage.find('_');
        const std::string stage_tag =
            underscore == std::string::npos ? stage : stage.substr(0, underscore);
        for (FlowStep& step : status.steps) step.stage = stage_tag;
        bundle.flow_status = std::move(status);
    }

    // ---- 一个报告文件都没找到 ----
    if (bundle.files.qor.empty() && bundle.files.timing.empty() && bundle.files.area.empty() &&
        bundle.files.flow_status.empty()) {
        bundle.warnings.emplace_back("no-report-files");
    }

    // ---- 分阶段（v13）----
    if (IsDirectory(syn)) {
        bundle.syn = BuildStageSection(syn, "Synthesis", {"gate.qor"}, {"gate_timing.rpt"},
                                       {"gate.area"}, {"flow.status"}, R"(.*_gate\.timing)",
                                       R"(.*_gate\.area)", mask, cache, &bundle.warnings);
    }
    if (IsDirectory(phy)) {
        bundle.phy = BuildStageSection(phy, "Place & Route", {"route.qor", "place.qor"},
                                       {"final_timing.rpt"}, {}, {"route_flow.status",
                                                                  "place_flow.status"},
                                       R"(.*_pr\.timing)", R"(.*_phy\.area)", mask, cache,
                                       &bundle.warnings);
    }

    // ---- 合并两阶段的步骤到顶层 flowStatus ----
    std::vector<FlowStep> merged_steps;
    const auto collect_steps = [&merged_steps](const std::optional<StageSection>& section,
                                               const char* stage_key) {
        if (!section.has_value() || !section->flow_status.has_value()) return;
        for (const FlowStep& step : section->flow_status->steps) {
            FlowStep copy = step;
            copy.stage = stage_key;
            merged_steps.push_back(std::move(copy));
        }
    };
    collect_steps(bundle.syn, "syn");
    collect_steps(bundle.phy, "phy");

    if (!merged_steps.empty()) {
        FlowStatusReport merged =
            bundle.flow_status.has_value() ? *bundle.flow_status : FlowStatusReport{};
        merged.steps = std::move(merged_steps);
        bundle.flow_status = std::move(merged);

        // stages 里放各阶段的原始 flowStatus（steps 不带 stage 标签），与 Python 一致
        for (const auto& [key, section] :
             {std::pair<std::string, const std::optional<StageSection>*>{"syn", &bundle.syn},
              std::pair<std::string, const std::optional<StageSection>*>{"phy", &bundle.phy}}) {
            if (!section->has_value()) continue;
            jsonrpc::Json status_json = jsonrpc::Json::object();
            if ((*section)->flow_status.has_value()) {
                status_json = (*section)->flow_status->ToJson();
            }
            bundle.flow_stages.emplace_back(key, std::move(status_json));
        }
    }

    return bundle;
}

}  // namespace fpgalsp::reports